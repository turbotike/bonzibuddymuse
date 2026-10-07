/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "muse_ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lvgl.h"
#include "mbedtls/base64.h"
#include "src/draw/lv_image_decoder_private.h"   /* custom decoder */
#include "src/misc/lv_area_private.h"            /* lv_area_intersect, for the ring */

#include "muse_ble.h"
#include "muse_board.h"
#include "muse_chat.h"
#include "muse_console.h"
#include "muse_link.h"
#include "muse_mem.h"
#include "muse_menu.h"
#include "muse_pixel.h"
#include "muse_settings.h"
#include "muse_settings_ui.h"
#include "muse_state.h"
#include "muse_voice.h"
#if CONFIG_MUSE_PET
#include "muse_audio.h"
#include "pet.h"
#endif
#include "muse_wifi.h"
#if CONFIG_MUSE_WATCHER_CAMERA
#include "boards/watcher_camera.h"
#endif

static const char *TAG = "muse_ui";

#define METER_SEGS 14
#define METER_SEG_PX 9
#define METER_GAP_PX 4
#define RING_RANGE 1000
#define CAPTION_W 256           /* 16 columns of unscii_16, the width reply captions wrap to */
#define CAPTION_LINE_SPACE 2
#define ART_BLANK_ROWS 3        /* Muse's art never reaches the grid's bottom rows */
#define MINI_CELL_PX 2          /* Muse's grid cells over a reply that's read */
#define ANSWER_MS 300           /* Muse making room for a reply, and back */
#define SPEAKER_PX 64
#define SPEAKER_GROW_PX 8       /* how much the speaker button swells while held */
#define SPEAKER_HOLD_MS 400     /* LVGL's long press */

#define COLOR_DIM 0x8b84a8
#define COLOR_CAPTION 0xd8d2ff
#define COLOR_RING_BG 0x140f22
#define COLOR_METER_OFF 0x1d1733
#define COLOR_ACCENT 0xa77dff
#define COLOR_DOT_OFF 0x3a3358
#define COLOR_LIT 0xf2efff
#define SETTINGS_TICK_S 0.25f

/* Text on 128 px screens, as in the button menu (muse_menu.c). */
#if LV_FONT_MONTSERRAT_12
#define FONT_COMPACT (&lv_font_montserrat_12)
#else
#define FONT_COMPACT (&lv_font_montserrat_14)
#endif

/*
 * Two layouts: the full one (round 466 px or similar) with a progress ring,
 * level meter and settings tile, and a compact one for tiny screens (128 px)
 * where Muse fills the screen and text overlays it.
 */
static int s_w, s_h;
static bool s_small;
static bool s_tall;         /* compact, with room above and below Muse (StickS3) */
static int s_canvas_px;     /* Muse's size on screen */
static int s_dy;            /* full layout: offset from a 466 px tall screen */
static lv_indev_t *s_indev;
static lv_obj_t *s_tv;
static lv_obj_t *s_face;
static lv_obj_t *s_settings;
static lv_obj_t *s_dots[2];
static lv_obj_t *s_wifi_icon;
static lv_obj_t *s_ble_icon;
static lv_obj_t *s_cover;
static lv_obj_t *s_pair;
static lv_obj_t *s_pair_code;
static lv_obj_t *s_pair_title;
static lv_obj_t *s_pair_hint;
static lv_obj_t *s_canvas;
static lv_obj_t *s_mic_icon;
static lv_obj_t *s_ring;
static lv_obj_t *s_bar;     /* compact layout's stand-in for the ring */
static lv_obj_t *s_state_lbl;
static lv_obj_t *s_name_lbl;    /* this gadget's own name, to tell it from the next one */
static lv_obj_t *s_power_lbl;
static lv_obj_t *s_caption_lbl;
static lv_obj_t *s_reply_lbl;   /* full layout: the reply's page while answering */
/* The tall layout's transcript: a box under Muse's feet with what it heard, dim, and its whole
 * reply so far, scrolled by finger. It follows the newest text until the reader scrolls back,
 * and stays up after the reply, longer with each scroll. */
#define TRANSCRIPT_PAD_R 8
#define TRANSCRIPT_HOLD_S 45.0f
#define TRANSCRIPT_PAUSE_S 8.0f   /* a finger scroll pauses the following this long */
static lv_obj_t *s_reply_box;
static lv_obj_t *s_heard_lbl;
static float s_user_scroll_until;   /* the reader scrolled: no following until then */
static int s_follow_from;           /* where the line being said was last found */
static float s_hold_until;
static char *s_transcript;      /* MUSE_TRANSCRIPT_MAX */
static lv_obj_t *s_meter[METER_SEGS];
static lv_obj_t *s_speaker;
static lv_obj_t *s_speaker_icon;
static lv_obj_t *s_aux_icon;
static lv_obj_t *s_image;   /* display.draw_url, over the face */
#if CONFIG_MUSE_WATCHER_CAMERA
static lv_obj_t *s_camera_hint;
#endif
static lv_image_dsc_t s_image_dsc;  /* its data is set once the image is shown */
/* The download writes the pixels without the display lock, so a big JPEG isn't
 * held up by each frame; this guards the buffer and what changed in it. */
static SemaphoreHandle_t s_image_mutex;
static uint16_t *s_image_buf;
static lv_area_t s_image_area;
static bool s_image_dirty;
static bool s_ready;

static float s_level;
static int s_shown_state = -1;
static const char *s_shown_name;
static const char *s_idle_name = "READY";   /* idle's label: set by the Wi-Fi state */
static int s_shown_lit = -1;
static uint32_t s_shown_accent;
static bool s_meter_visible = true;
static int s_ring_value = -1;
static uint32_t s_caption_version;
static float s_next_power_update;
static float s_next_settings_tick;
static bool s_dark;
static int s_brightness = -1;       /* last applied */
static int s_preview_brightness = -1;
static int s_shown_page = -1;
static int s_shown_speaker = -1;
static muse_mode_t s_last_mode = MUSE_MODE_COUNT;

/*
 * While Muse is thinking or speaking it shrinks to make room for the reply:
 * a little when the reply is heard, over a few lines to follow along, and
 * to the top when it's only read, over a page.
 */
typedef struct {
    int px, y;                /* Muse's size and centre */
    int cols, lines;          /* the reply's page */
    int w, h, top;            /* and where it goes */
    lv_text_align_t align;
    lv_obj_t *hides[4];       /* what it covers */
} answer_layout_t;

enum { ANSWER_HEARD, ANSWER_READ };
static answer_layout_t s_answers[2];
static int s_answer = -1;       /* the layout showing, or -1 */
static int s_page_for = -1;     /* the layout the reply's page is sized for */
static int s_big_y;             /* Muse's centre at full size */
static int s_muse_y;            /* and now */
static int s_from_px, s_from_y, s_to_px, s_to_y;

static const char *const MODE_NAMES[MUSE_MODE_COUNT] = {
    [MUSE_MODE_BOOT] = "WAKING UP",
    [MUSE_MODE_IDLE] = "READY",
    [MUSE_MODE_LISTENING] = "LISTENING",
    [MUSE_MODE_THINKING] = "THINKING",
    [MUSE_MODE_SPEAKING] = "SPEAKING",
    [MUSE_MODE_ERROR] = "ERROR",
    [MUSE_MODE_OFF] = "GOODBYE",
};

/*
 * Muse reaches LVGL as an image whose pixels are made on demand: a decoder
 * scales the 64 px grid up one strip of rows at a time, so the full-size frame
 * never sits in RAM and LVGL never runs its (slow) image transform.
 *
 * LVGL splits each refresh into a tile per draw unit and draws them at once,
 * each with its own open decoder, so each one takes its own strip: with one
 * shared strip, a tile blended rows the other had just scaled into it.
 */
#define STRIP_ROWS 16
#define STRIPS LV_DRAW_SW_DRAW_UNIT_CNT

#define MUSE_PIXEL_MAX_PX 512   /* muse_pixel_set_size's cap */
#define DIRTY_RECTS_MAX 8       /* with the meter and labels, well inside LVGL's 32 areas */
#define DIRTY_MERGE_PX 2048     /* pixels that cost about one walk of the widget tree */

static lv_image_dsc_t s_muse_src;
static lv_draw_buf_t *s_strips[STRIPS];
static bool s_strip_busy[STRIPS];
static uint16_t *s_cells;       /* each cell's colour as last invalidated */
static uint16_t *s_cell_row;    /* one screen row of Muse */
static bool s_cells_valid;

static lv_result_t muse_dec_info(lv_image_decoder_t *dec, lv_image_decoder_dsc_t *dsc, lv_image_header_t *header)
{
    (void)dec;
    if (dsc->src != &s_muse_src) {
        return LV_RESULT_INVALID;
    }
    *header = s_muse_src.header;
    return LV_RESULT_OK;
}

/* LVGL holds its decoder lock around open and close. */
static lv_result_t muse_dec_open(lv_image_decoder_t *dec, lv_image_decoder_dsc_t *dsc)
{
    (void)dec;
    for (int i = 0; i < STRIPS; i++) {
        if (!s_strip_busy[i]) {
            s_strip_busy[i] = true;
            dsc->user_data = s_strips[i];
            return LV_RESULT_OK;   /* the rest of the work is in get_area */
        }
    }
    return LV_RESULT_INVALID;
}

static lv_result_t muse_dec_get_area(lv_image_decoder_t *dec, lv_image_decoder_dsc_t *dsc,
                                     const lv_area_t *full, lv_area_t *area)
{
    (void)dec;
    int32_t y1 = area->y1 == LV_COORD_MIN ? full->y1 : area->y2 + 1;
    if (y1 > full->y2) {
        return LV_RESULT_INVALID;
    }
    int32_t y2 = LV_MIN(y1 + STRIP_ROWS - 1, full->y2);
    int32_t w = lv_area_get_width(full);
    lv_draw_buf_t *buf = lv_draw_buf_reshape(dsc->user_data, LV_COLOR_FORMAT_RGB565, w, y2 - y1 + 1, LV_STRIDE_AUTO);
    if (!buf) {
        return LV_RESULT_INVALID;
    }
    muse_pixel_scale((uint16_t *)buf->data, buf->header.stride / sizeof(uint16_t), full->x1, full->x2, y1, y2);
    area->x1 = full->x1;
    area->x2 = full->x2;
    area->y1 = y1;
    area->y2 = y2;
    dsc->decoded = buf;
    return LV_RESULT_OK;
}

static void muse_dec_close(lv_image_decoder_t *dec, lv_image_decoder_dsc_t *dsc)
{
    (void)dec;
    for (int i = 0; i < STRIPS; i++) {
        if (s_strips[i] == dsc->user_data) {
            s_strip_busy[i] = false;   /* kept for the next open */
        }
    }
}

static void muse_image_init(void)
{
    s_muse_src.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_muse_src.header.cf = LV_COLOR_FORMAT_RGB565;
    s_muse_src.header.w = s_canvas_px;
    s_muse_src.header.h = s_canvas_px;
    s_muse_src.header.stride = s_canvas_px * sizeof(uint16_t);
    s_muse_src.data = (const uint8_t *)&s_muse_src;   /* LVGL skips sources without data */
    muse_pixel_set_size(s_canvas_px);

    for (int i = 0; i < STRIPS; i++) {
        s_strips[i] = lv_draw_buf_create(s_canvas_px, STRIP_ROWS, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);
        assert(s_strips[i]);
    }
    lv_image_decoder_t *dec = lv_image_decoder_create();
    assert(dec);
    lv_image_decoder_set_info_cb(dec, muse_dec_info);
    lv_image_decoder_set_open_cb(dec, muse_dec_open);
    lv_image_decoder_set_get_area_cb(dec, muse_dec_get_area);
    lv_image_decoder_set_close_cb(dec, muse_dec_close);
    dec->name = "muse";

#if CONFIG_MUSE_AVATAR_FULL_REDRAW
    /* A sprite avatar is not drawn in 64x64 cells: the per-cell change check would miss its
     * one-pixel moves and leave stale slivers, so the whole canvas is redrawn every frame. */
    s_cells = NULL;
    s_cell_row = NULL;
#else
    s_cells = heap_caps_malloc(MUSE_PX_W * MUSE_PX_H * sizeof(uint16_t), MUSE_BIG_CAPS);
    s_cell_row = heap_caps_malloc(MUSE_PIXEL_MAX_PX * sizeof(uint16_t), MUSE_BIG_CAPS);
#endif
    if (!s_cells || !s_cell_row) {
        free(s_cells);
        free(s_cell_row);
        s_cells = s_cell_row = NULL;   /* redraw all of Muse every frame */
    }
}

/*
 * Most frames change only a few of Muse's cells, so only those are redrawn.
 * Each frame reads back one screen pixel of every cell (its first, which the
 * faint grid never dims) and compares it with the last frame's. A row's
 * changed cells make one span, and neighbouring spans merge while the pixels
 * drawn for nothing cost less than LVGL's walk of the widget tree for one more
 * area.
 */
static int32_t cell_px(int cell, int size)
{
    return (cell * size + MUSE_PX_W - 1) / MUSE_PX_W;   /* a cell's first screen pixel */
}

static int32_t rect_cells(const lv_area_t *r)
{
    return lv_area_get_width(r) * lv_area_get_height(r);
}

static void invalidate_muse(void)
{
    if (!s_cells) {
        lv_obj_invalidate(s_canvas);
        return;
    }
    int size = (int)s_muse_src.header.w;
    lv_area_t r[MUSE_PX_H];   /* in cells */
    int n = 0;
    for (int cy = 0; cy < MUSE_PX_H; cy++) {
        int32_t y = cell_px(cy, size);
        muse_pixel_scale(s_cell_row, size, 0, size - 1, y, y);
        uint16_t *last = &s_cells[cy * MUSE_PX_W];
        int x1 = -1, x2 = -1;
        for (int cx = 0; cx < MUSE_PX_W; cx++) {
            uint16_t c = s_cell_row[cell_px(cx, size)];
            if (c != last[cx] || !s_cells_valid) {
                last[cx] = c;
                x1 = x1 < 0 ? cx : x1;
                x2 = cx;
            }
        }
        if (x2 >= 0) {
            r[n++] = (lv_area_t){x1, cy, x2, cy};
        }
    }
    s_cells_valid = true;

    int64_t merge_cells = (int64_t)DIRTY_MERGE_PX * MUSE_PX_W * MUSE_PX_H / (size * size);
    while (n > 1) {
        int best = 0;
        int32_t best_cost = INT32_MAX;
        for (int i = 0; i + 1 < n; i++) {
            lv_area_t j = {LV_MIN(r[i].x1, r[i + 1].x1), r[i].y1, LV_MAX(r[i].x2, r[i + 1].x2), r[i + 1].y2};
            int32_t cost = rect_cells(&j) - rect_cells(&r[i]) - rect_cells(&r[i + 1]);
            if (cost < best_cost) {
                best = i;
                best_cost = cost;
            }
        }
        if (n <= DIRTY_RECTS_MAX && best_cost > merge_cells) {
            break;
        }
        r[best].x1 = LV_MIN(r[best].x1, r[best + 1].x1);
        r[best].x2 = LV_MAX(r[best].x2, r[best + 1].x2);
        r[best].y2 = r[best + 1].y2;
        memmove(&r[best + 1], &r[best + 2], (n - best - 2) * sizeof(r[0]));
        n--;
    }

    lv_area_t at;
    lv_obj_get_coords(s_canvas, &at);
    for (int i = 0; i < n; i++) {
        lv_area_t a = {
            at.x1 + cell_px(r[i].x1, size),
            at.y1 + cell_px(r[i].y1, size),
            at.x1 + cell_px(r[i].x2 + 1, size) - 1,
            at.y1 + cell_px(r[i].y2 + 1, size) - 1,
        };
        lv_obj_invalidate_area(s_canvas, &a);
    }
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color);

/* The procedural art fills its grid but for the bottom rows; a sprite avatar overrides this. */
__attribute__((weak)) void muse_pixel_blank_rows(int px, int *top, int *bottom)
{
    *top = 0;
    *bottom = ART_BLANK_ROWS * (px / MUSE_PX_W);
}

/* The reply's area while answering: the transcript box where there is one, else the page. */
static lv_obj_t *reply_area(void)
{
    return s_reply_box ? s_reply_box : s_reply_lbl;
}

/* A microphone from primitives: LVGL's symbol font has none. */
static lv_obj_t *make_mic(lv_obj_t *parent, int size)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, size, size);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
    int stroke = size / 10 > 1 ? size / 10 : 1;

    lv_obj_t *head = lv_obj_create(box);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, size * 3 / 8, size * 9 / 16);
    lv_obj_set_style_radius(head, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(head, LV_OPA_COVER, 0);
    lv_obj_align(head, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *cradle = lv_arc_create(box);
    lv_obj_set_size(cradle, size * 5 / 8 + stroke, size * 5 / 8 + stroke);
    lv_arc_set_bg_angles(cradle, 0, 180);
    lv_obj_remove_style(cradle, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(cradle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_all(cradle, 0, 0);
    lv_obj_set_style_arc_opa(cradle, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(cradle, stroke, LV_PART_MAIN);
    lv_obj_align(cradle, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *stem = lv_obj_create(box);
    lv_obj_remove_style_all(stem);
    lv_obj_set_size(stem, stroke, size * 7 / 8 - size * 5 / 8);
    lv_obj_set_style_bg_opa(stem, LV_OPA_COVER, 0);
    lv_obj_align(stem, LV_ALIGN_TOP_MID, 0, size * 5 / 8);

    lv_obj_t *base = lv_obj_create(box);
    lv_obj_remove_style_all(base);
    lv_obj_set_size(base, size * 3 / 8, stroke);
    lv_obj_set_style_bg_opa(base, LV_OPA_COVER, 0);
    lv_obj_align(base, LV_ALIGN_TOP_MID, 0, size * 7 / 8);
    return box;
}

static void set_mic_color(uint32_t color)
{
    for (uint32_t i = 0; i < lv_obj_get_child_count(s_mic_icon); i++) {
        lv_obj_t *part = lv_obj_get_child(s_mic_icon, i);
        lv_obj_set_style_bg_color(part, lv_color_hex(color), 0);
        lv_obj_set_style_arc_color(part, lv_color_hex(color), LV_PART_MAIN);
    }
}

/* A finger on the box (our own scrolls come from a timer, with no input device active):
 * the following pauses and the transcript stays up longer. */
static void on_transcript_event(lv_event_t *e)
{
    (void)e;
    if (!lv_indev_active()) {
        return;
    }
    float now = (float)esp_timer_get_time() / 1e6f;
    s_user_scroll_until = now + TRANSCRIPT_PAUSE_S;
    s_hold_until = now + TRANSCRIPT_HOLD_S;
}

static lv_obj_t *make_transcript_label(uint32_t color)
{
    lv_obj_t *l = make_label(s_reply_box, &lv_font_unscii_16, color);
    lv_obj_set_width(l, lv_pct(100));
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_line_space(l, CAPTION_LINE_SPACE, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    return l;
}

/* The transcript box: what Muse heard, dim, then its reply, in a column that scrolls. */
static void build_transcript(lv_obj_t *face)
{
    s_transcript = heap_caps_malloc(MUSE_TRANSCRIPT_MAX, MUSE_BIG_CAPS);
    if (s_transcript) {
        s_transcript[0] = '\0';
    }
    s_reply_box = lv_obj_create(face);
    lv_obj_remove_style_all(s_reply_box);
    lv_obj_set_scroll_dir(s_reply_box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_reply_box, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(s_reply_box, lv_color_hex(COLOR_ACCENT), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(s_reply_box, LV_OPA_70, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(s_reply_box, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(s_reply_box, 2, LV_PART_SCROLLBAR);
    /* A vertical drag stays in the box; a sideways one still swipes to the settings tile. */
    lv_obj_remove_flag(s_reply_box, LV_OBJ_FLAG_SCROLL_CHAIN_VER | LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_flex_flow(s_reply_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_reply_box, 8, 0);
    lv_obj_set_style_pad_right(s_reply_box, TRANSCRIPT_PAD_R, 0);
    lv_obj_add_event_cb(s_reply_box, on_transcript_event, LV_EVENT_SCROLL_BEGIN, NULL);
    lv_obj_add_event_cb(s_reply_box, on_transcript_event, LV_EVENT_PRESSED, NULL);
    lv_obj_add_flag(s_reply_box, LV_OBJ_FLAG_HIDDEN);

    s_heard_lbl = make_transcript_label(COLOR_DIM);
    lv_obj_add_flag(s_heard_lbl, LV_OBJ_FLAG_HIDDEN);
    s_reply_lbl = make_transcript_label(COLOR_CAPTION);
}

/* Where in the transcript the caption begins: the page being said starts with a whole wrapped
 * line cut from the reply, and the Pi's "now saying" line is a sentence of it. -1: not there. */
static int transcript_find(const char *caption)
{
    while (*caption == ' ') {
        caption++;
    }
    size_t n = strcspn(caption, "\n");
    while (n && caption[n - 1] == ' ') {
        n--;
    }
    char key[80];
    if (n >= sizeof(key)) {
        n = sizeof(key) - 1;
    }
    if (n < 4) {
        return -1;
    }
    memcpy(key, caption, n);
    key[n] = '\0';
    const char *hit = strstr(s_transcript + s_follow_from, key);   /* speech moves forward */
    if (!hit) {
        hit = strstr(s_transcript, key);
    }
    if (!hit && n > 32) {
        key[32] = '\0';   /* the Pi may have trimmed the sentence's end */
        hit = strstr(s_transcript, key);
    }
    return hit ? (int)(hit - s_transcript) : -1;
}

/* Scrolls the box so the line holding byte `at` of the reply sits a quarter of the way down. */
static void transcript_show(int at)
{
    lv_obj_update_layout(s_reply_box);
    lv_point_t p;
    lv_label_get_letter_pos(s_reply_lbl, (uint32_t)at, &p);
    int32_t y = lv_obj_get_y(s_reply_lbl) + p.y;   /* in the box's content, from its top */
    int32_t target = y - lv_obj_get_content_height(s_reply_box) / 4;
    int32_t now_y = lv_obj_get_scroll_y(s_reply_box);
    int32_t max = now_y + lv_obj_get_scroll_bottom(s_reply_box);
    target = LV_CLAMP(0, target, max < 0 ? 0 : max);
    if (LV_ABS(target - now_y) >= 8) {
        lv_obj_scroll_to_y(s_reply_box, target, LV_ANIM_ON);
    }
}

/* Each frame while the transcript shows: new text goes in (the view stays put, so a long
 * reply reads from its start), a new caption is found in it and, unless the reader just
 * scrolled, the box follows the line being said. Before there's a reply, the caption says
 * what's happening. */
static void update_transcript(const char *caption, bool fresh, bool opened, float now)
{
    static char heard[MUSE_HEARD_MAX];
    static uint32_t version;
    bool changed = s_transcript && muse_state_transcript(heard, sizeof(heard), s_transcript, MUSE_TRANSCRIPT_MAX, &version);
    if (opened) {
        s_user_scroll_until = 0;
        s_follow_from = 0;
        lv_obj_scroll_to_y(s_reply_box, 0, LV_ANIM_OFF);
        lv_obj_remove_flag(s_reply_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_caption_lbl, LV_OBJ_FLAG_HIDDEN);
        changed = fresh = true;
    }
    if (!changed && !fresh) {
        return;
    }
    if (changed) {
        if (heard[0]) {
            lv_label_set_text_fmt(s_heard_lbl, "\"%s\"", heard);
        }
        lv_obj_set_flag(s_heard_lbl, LV_OBJ_FLAG_HIDDEN, !heard[0]);
    }
    bool have = s_transcript && s_transcript[0];
    if (changed || !have) {
        lv_label_set_text(s_reply_lbl, have ? s_transcript : caption);
        lv_obj_set_style_text_color(s_reply_lbl, lv_color_hex(have ? COLOR_CAPTION : COLOR_DIM), 0);
        lv_obj_set_flag(s_reply_lbl, LV_OBJ_FLAG_HIDDEN, !have && !caption[0]);
    }
    if (have && fresh && caption[0]) {
        int at = transcript_find(caption);
        if (at >= 0) {
            s_follow_from = at;
            if (now >= s_user_scroll_until) {
                transcript_show(at);
            }
        }
    }
}


#if CONFIG_MUSE_PET
/* ---- the toy: a 90s virtual pet round the LCD ---------------------------------------- */

typedef struct {
    uint32_t frame, frame_dark, lcd, lcd_line, neon, neon2, text, dim;
    const char *name;
} theme_t;

static const theme_t THEMES[] = {
    { 0x3d2a73, 0x1c1238, 0x07061a, 0x2a1f52, 0xff2bd6, 0x2bf5ff, 0xf4ecff, 0x6b5c93, "NEON" },
    { 0x1f5a42, 0x0b2a1e, 0x03100c, 0x114d3a, 0x39ff14, 0xffb000, 0xeafff0, 0x4d8a68, "TOXIC" },
    { 0x6a3a1a, 0x2a160a, 0x120803, 0x5a2d10, 0xff7a00, 0x00c8ff, 0xfff1e6, 0x9a6a4a, "LAVA" },
    { 0x4a1f58, 0x1e0c26, 0x0c0410, 0x45124d, 0xffe600, 0xff3d7f, 0xfff8e0, 0x8a5d93, "ARCADE" },
};
#define THEME_COUNT 4

enum { ICON_FOOD, ICON_TRAIN, ICON_CLEAN, ICON_MEDS, ICON_LIGHT, ICON_STATS, ICON_THEME, ICON_COUNT, ICON_HEART = ICON_COUNT, ICON_FUN, ICON_ALL };

/* 8x8 pixel icons, as alpha masks the theme colours. */
static const char *const ICON_ART[ICON_ALL][8] = {
    { "...X....", "..XXXX..", ".XXXXXX.", "XXXXXXXX", "XXXXXXXX", "XXXXXXXX", ".XXXXXX.", "..X..X.." },   /* food */
    { "........", "........", "X......X", "XX.XX.XX", "XXXXXXXX", "XX.XX.XX", "X......X", "........" },   /* train */
    { "...X....", "...X....", "..XXX...", ".XXXXX..", "XXXXXXX.", "XXXXXXX.", ".XXXXX..", "..XXX..." },   /* clean */
    { "..XXX...", "..XXX...", "..XXX...", "XXXXXXXX", "XXXXXXXX", "XXXXXXXX", "..XXX...", "..XXX..." },   /* meds */
    { "..XXXX..", ".X....X.", "X......X", "X......X", ".X....X.", "..XXXX..", "..XXXX..", "...XX..." },   /* light */
    { "......XX", "......XX", "...XX.XX", "...XX.XX", "XX.XX.XX", "XX.XX.XX", "XX.XX.XX", "XXXXXXXX" },   /* stats */
    { ".XXXXXX.", "X.X.X..X", "XX.X.X.X", "X.X.X..X", "XX.X.XXX", "X......X", "X......X", ".XXXXXX." },   /* theme */
    { "........", ".XX..XX.", "XXXXXXXX", "XXXXXXXX", ".XXXXXX.", "..XXXX..", "...XX...", "........" },   /* heart */
    { "..XXXX..", ".X....X.", "X.X..X.X", "X......X", "X.X..X.X", "X..XX..X", ".X....X.", "..XXXX.." },   /* fun */
};
static uint8_t s_icon_px[ICON_ALL][64];
static lv_image_dsc_t s_icon_dsc[ICON_ALL];

#define LCD_X 10
#define LCD_Y 12
#define LCD_W 300
#define LCD_H 376
#define TOY_BUBBLE_S 12
#define TRAIN_ROUNDS 5

static lv_obj_t *s_toy_frame, *s_toy_body, *s_toy_lcd;
static lv_obj_t *s_toy_icons[ICON_COUNT], *s_toy_icon_imgs[ICON_COUNT];
static lv_obj_t *s_toy_btn[3], *s_toy_btn_lbl[3];
static lv_obj_t *s_toy_strip, *s_toy_mood, *s_toy_caption;
static lv_obj_t *s_toy_alert[4];      /* hungry, dirty, sick, lonely/bored */
static lv_obj_t *s_bubble, *s_bubble_lbl, *s_bubble_tail;
static lv_obj_t *s_stats, *s_stats_bars[6], *s_stats_lbls[6], *s_stats_text;
static lv_obj_t *s_train, *s_train_meter, *s_train_zone, *s_train_marker, *s_train_rock, *s_train_crack[4], *s_train_msg, *s_train_fire;
static int s_toy_sel = -1;            /* the highlighted icon */
static int s_theme = -1;
static float s_bubble_until;
static float s_toy_next_update, s_stats_until;
static uint32_t s_toy_transcript_version;
static char *s_toy_text;              /* a transcript copy for the bubble */
/* Training. */
static bool s_train_on;
static int s_train_round, s_train_hits;
static float s_train_t0, s_train_msg_until, s_train_next_round;
static bool s_train_firing, s_train_round_done;
static volatile int s_toy_debug_req;   /* from the console: 1 stats, 2 training, 3 a bubble */

void muse_ui_pet_debug(const char *what)
{
    s_toy_debug_req = !strcmp(what, "stats") ? 1 : !strcmp(what, "train") ? 2 : !strcmp(what, "bubble") ? 3 : 0;
}

static void upper(char *s)
{
    for (; *s; s++) {
        if (*s >= 'a' && *s <= 'z') {
            *s = (char)(*s - 'a' + 'A');
        }
    }
}

static const theme_t *theme(void)
{
    return &THEMES[(s_theme < 0 ? 0 : s_theme) % THEME_COUNT];
}

static void icon_init(void)
{
    for (int i = 0; i < ICON_ALL; i++) {
        for (int r = 0; r < 8; r++) {
            for (int c = 0; c < 8; c++) {
                s_icon_px[i][r * 8 + c] = ICON_ART[i][r][c] == 'X' ? 255 : 0;
            }
        }
        lv_image_dsc_t *d = &s_icon_dsc[i];
        memset(d, 0, sizeof(*d));
        d->header.magic = LV_IMAGE_HEADER_MAGIC;
        d->header.cf = LV_COLOR_FORMAT_A8;
        d->header.w = 8;
        d->header.h = 8;
        d->header.stride = 8;
        d->data_size = 64;
        d->data = s_icon_px[i];
    }
}

static lv_obj_t *make_icon(lv_obj_t *parent, int which, int px, uint32_t color)
{
    lv_obj_t *img = lv_image_create(parent);
    lv_image_set_src(img, &s_icon_dsc[which]);
    lv_obj_set_size(img, px, px);
    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_STRETCH);
    lv_image_set_antialias(img, false);
    lv_obj_set_style_image_recolor(img, lv_color_hex(color), 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    return img;
}

static void bubble_hide(void)
{
    s_bubble_until = 0;
    lv_obj_add_flag(s_bubble, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_bubble_tail, LV_OBJ_FLAG_HIDDEN);
}

static void bubble_show(const char *text, float secs)
{
    if (!text || !text[0]) {
        return;
    }
    lv_label_set_text(s_bubble_lbl, text);
    lv_obj_remove_flag(s_bubble, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_bubble_tail, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(s_bubble);
    s_bubble_until = (float)esp_timer_get_time() / 1e6f + secs;
}

/* From the pet task or Home Link: the words go through the transcript store, the frame shows them. */
void pet_ui_bubble(const char *text, int secs)
{
    muse_state_set_heard("");
    muse_state_set_transcript(text);
    s_bubble_until = (float)esp_timer_get_time() / 1e6f + (float)secs;
}

static void apply_theme(void)
{
    const theme_t *t = theme();
    lv_obj_set_style_bg_color(s_toy_frame, lv_color_hex(t->frame), 0);
    lv_obj_set_style_bg_color(s_toy_body, lv_color_hex(t->frame_dark), 0);
    lv_obj_set_style_border_color(s_toy_body, lv_color_hex(t->lcd_line), 0);
    lv_obj_set_style_bg_color(s_toy_lcd, lv_color_hex(t->lcd), 0);
    lv_obj_set_style_border_color(s_toy_lcd, lv_color_hex(t->neon), 0);
    for (int i = 0; i < ICON_COUNT; i++) {
        bool sel = i == s_toy_sel;
        lv_obj_set_style_bg_color(s_toy_icons[i], lv_color_hex(t->neon), 0);
        lv_obj_set_style_bg_opa(s_toy_icons[i], sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_image_recolor(s_toy_icon_imgs[i], lv_color_hex(sel ? t->lcd : t->dim), 0);
    }
    for (int b = 0; b < 3; b++) {
        lv_obj_set_style_bg_color(s_toy_btn[b], lv_color_hex(t->frame_dark), 0);
        lv_obj_set_style_bg_color(s_toy_btn[b], lv_color_hex(t->neon), LV_STATE_PRESSED);
        lv_obj_set_style_border_color(s_toy_btn[b], lv_color_hex(t->neon2), 0);
        lv_obj_set_style_text_color(s_toy_btn_lbl[b], lv_color_hex(t->text), 0);
    }
    lv_obj_set_style_text_color(s_toy_strip, lv_color_hex(t->neon2), 0);
    lv_obj_set_style_text_color(s_toy_mood, lv_color_hex(t->text), 0);
    lv_obj_set_style_text_color(s_toy_caption, lv_color_hex(t->dim), 0);
    for (int i = 0; i < 4; i++) {
        lv_obj_set_style_image_recolor(s_toy_alert[i], lv_color_hex(t->neon), 0);
    }
    lv_obj_set_style_bg_color(s_bubble, lv_color_hex(t->text), 0);
    lv_obj_set_style_border_color(s_bubble, lv_color_hex(t->neon), 0);
    lv_obj_set_style_text_color(s_bubble_lbl, lv_color_hex(t->lcd), 0);
    lv_obj_set_style_bg_color(s_bubble_tail, lv_color_hex(t->text), 0);
    lv_obj_set_style_border_color(s_bubble_tail, lv_color_hex(t->neon), 0);
    lv_obj_set_style_bg_color(s_stats, lv_color_hex(t->lcd), 0);
    lv_obj_set_style_border_color(s_stats, lv_color_hex(t->neon2), 0);
    lv_obj_set_style_text_color(s_stats_text, lv_color_hex(t->text), 0);
    static const uint32_t BAR_HUES[6] = { 0xffa64d, 0x6ea8ff, 0xffe066, 0x5ad1ff, 0xff7ad9, 0xff5c5c };
    for (int i = 0; i < 6; i++) {
        lv_obj_set_style_bg_color(s_stats_bars[i], lv_color_hex(t->lcd_line), LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_stats_bars[i], lv_color_hex(BAR_HUES[i]), LV_PART_INDICATOR);
        lv_obj_set_style_text_color(s_stats_lbls[i], lv_color_hex(t->dim), 0);
    }
    lv_obj_set_style_bg_color(s_train, lv_color_hex(t->lcd), 0);
    lv_obj_set_style_border_color(s_train, lv_color_hex(t->neon2), 0);
    lv_obj_set_style_bg_color(s_train_meter, lv_color_hex(t->lcd_line), 0);
    lv_obj_set_style_bg_color(s_train_zone, lv_color_hex(t->neon2), 0);
    lv_obj_set_style_bg_color(s_train_marker, lv_color_hex(t->neon), 0);
    lv_obj_set_style_bg_color(s_train_rock, lv_color_hex(t->dim), 0);
    lv_obj_set_style_border_color(s_train_rock, lv_color_hex(t->lcd_line), 0);
    for (int i = 0; i < 4; i++) {
        lv_obj_set_style_line_color(s_train_crack[i], lv_color_hex(t->lcd), 0);
    }
    lv_obj_set_style_text_color(s_train_msg, lv_color_hex(t->neon), 0);
    lv_obj_set_style_bg_color(s_train_fire, lv_color_hex(t->neon), 0);
    lv_obj_set_style_border_color(s_train_fire, lv_color_hex(t->text), 0);
    uint32_t c = t->lcd;
    muse_pixel_set_background((uint16_t)((((c >> 16) & 0xff) >> 3) << 11 | (((c >> 8) & 0xff) >> 2) << 5 | ((c & 0xff) >> 3)));
    invalidate_muse();
}

static void set_theme(int th)
{
    s_theme = ((th % THEME_COUNT) + THEME_COUNT) % THEME_COUNT;
    apply_theme();
    if (pet_theme() != s_theme) {
        pet_set_theme(s_theme);
    }
}

static void select_icon(int i)
{
    s_toy_sel = i;
    apply_theme();
}

/* ---- the stats page ---- */

static void stats_show(float now)
{
    pet_view_t v;
    pet_view(&v);
    static const int ORDER[6] = { PET_NEED_FOOD, PET_NEED_ENERGY, PET_NEED_FUN, PET_NEED_CLEAN, PET_NEED_BOND, -1 };
    for (int i = 0; i < 6; i++) {
        lv_bar_set_value(s_stats_bars[i], ORDER[i] < 0 ? v.power : v.needs[ORDER[i]], LV_ANIM_OFF);
    }
    char stage[12], mood[16], age[16];
    strlcpy(stage, pet_stage_name(v.stage), sizeof(stage));
    strlcpy(mood, pet_mood_name(v.mood), sizeof(mood));
    upper(stage);
    upper(mood);
    if (v.age_min < 60) {
        snprintf(age, sizeof(age), "%uM", (unsigned)v.age_min);
    } else if (v.age_min < 24 * 60) {
        snprintf(age, sizeof(age), "%uH", (unsigned)(v.age_min / 60));
    } else {
        snprintf(age, sizeof(age), "%uD %uH", (unsigned)(v.age_min / 1440), (unsigned)(v.age_min / 60 % 24));
    }
    lv_label_set_text_fmt(s_stats_text, "%s\nGEN %u  %s  %s\n%s%s  HP %d  CARE %d", v.name[0] ? v.name : "(NO NAME)",
                          (unsigned)v.generation, stage, age, v.sick ? "SICK " : "", mood, v.health, v.care);
    lv_obj_remove_flag(s_stats, LV_OBJ_FLAG_HIDDEN);
    s_stats_until = now + 10.0f;
    bubble_hide();
}

static void stats_hide(void)
{
    lv_obj_add_flag(s_stats, LV_OBJ_FLAG_HIDDEN);
    s_stats_until = 0;
}

/* ---- training: time the marker into the zone, fire at the boulder ---- */

static float train_marker_pos(float now)
{
    float period = 1.5f - s_train_round * 0.14f;   /* faster each round */
    float ph = fmodf((now - s_train_t0) / period, 1.0f);
    return ph < 0.5f ? ph * 2.0f : 2.0f - ph * 2.0f;   /* 0..1..0 */
}

static float train_zone_half(void)
{
    return 0.13f - s_train_round * 0.012f;
}

static void train_layout_marker(float p)
{
    int mh = lv_obj_get_height(s_train_meter);
    int y = (int)((1.0f - p) * (mh - 8));
    lv_obj_set_pos(s_train_marker, -3, y);
}

static void train_msg(const char *text, float now, float secs)
{
    lv_label_set_text(s_train_msg, text);
    lv_obj_remove_flag(s_train_msg, LV_OBJ_FLAG_HIDDEN);
    s_train_msg_until = now + secs;
}

static void train_start(float now)
{
    s_train_on = true;
    s_train_round = 0;
    s_train_hits = 0;
    s_train_t0 = now;
    s_train_firing = false;
    s_train_round_done = false;
    for (int i = 0; i < 4; i++) {
        lv_obj_add_flag(s_train_crack[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_add_flag(s_train_fire, LV_OBJ_FLAG_HIDDEN);
    float hw = train_zone_half();
    int mh = lv_obj_get_height(s_train_meter);
    lv_obj_set_size(s_train_zone, lv_obj_get_width(s_train_meter), (int)(mh * hw * 2));
    lv_obj_set_pos(s_train_zone, 0, (int)(mh * (0.5f - hw)));
    lv_obj_remove_flag(s_train, LV_OBJ_FLAG_HIDDEN);
    train_msg("TRAINING! TAP OR B\nWHEN THE MARK\nIS IN THE ZONE", now, 2.5f);
    stats_hide();
    bubble_hide();
}

static void train_end(float now)
{
    s_train_on = false;
    lv_obj_add_flag(s_train, LV_OBJ_FLAG_HIDDEN);
    pet_train(s_train_hits, TRAIN_ROUNDS);
    char line[48];
    snprintf(line, sizeof(line), "%d of %d! %s", s_train_hits, TRAIN_ROUNDS,
             s_train_hits >= 5 ? "PERFECT!" : s_train_hits >= 3 ? "Getting stronger." : "More practice...");
    bubble_show(line, 5.0f);
    (void)now;
}

static void train_fire_done(lv_anim_t *a)
{
    (void)a;
    lv_obj_add_flag(s_train_fire, LV_OBJ_FLAG_HIDDEN);
    s_train_firing = false;
    if (s_train_hits > 0 && s_train_hits <= 4) {
        lv_obj_remove_flag(s_train_crack[s_train_hits - 1], LV_OBJ_FLAG_HIDDEN);
    }
}

static void train_fire_x(void *obj, int32_t x)
{
    lv_obj_set_x(obj, x);
}

static void train_fire(float now)
{
    if (!s_train_on || s_train_firing || s_train_round_done) {
        return;
    }
    float p = train_marker_pos(now);
    bool hit = fabsf(p - 0.5f) <= train_zone_half();
    s_train_round_done = true;
    s_train_next_round = now + 1.3f;
    if (hit) {
        s_train_hits++;
        s_train_firing = true;
        lv_obj_remove_flag(s_train_fire, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_train_fire, 150, 120);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_train_fire);
        lv_anim_set_exec_cb(&a, train_fire_x);
        lv_anim_set_values(&a, 150, 225);
        lv_anim_set_duration(&a, 320);
        lv_anim_set_completed_cb(&a, train_fire_done);
        lv_anim_start(&a);
        muse_voice_request_sound(MUSE_SOUND_TICK, 1);
        train_msg("HIT!", now, 1.2f);
    } else {
        train_msg("MISS", now, 1.2f);
    }
}

static void train_tick(float now)
{
    if (!s_train_on) {
        return;
    }
    if (s_train_round_done) {
        if (now >= s_train_next_round) {
            s_train_round++;
            s_train_round_done = false;
            s_train_t0 = now;
            if (s_train_round >= TRAIN_ROUNDS) {
                train_end(now);
                return;
            }
            float hw = train_zone_half();
            int mh = lv_obj_get_height(s_train_meter);
            lv_obj_set_size(s_train_zone, lv_obj_get_width(s_train_meter), (int)(mh * hw * 2));
            lv_obj_set_pos(s_train_zone, 0, (int)(mh * (0.5f - hw)));
            char line[32];
            snprintf(line, sizeof(line), "ROUND %d / %d", s_train_round + 1, TRAIN_ROUNDS);
            train_msg(line, now, 1.0f);
        }
    } else {
        train_layout_marker(train_marker_pos(now));
    }
    if (s_train_msg_until && now > s_train_msg_until) {
        lv_obj_add_flag(s_train_msg, LV_OBJ_FLAG_HIDDEN);
        s_train_msg_until = 0;
    }
}

/* ---- the controls ---- */

static void toy_activate(int which, float now)
{
    pet_view_t v;
    pet_view(&v);
    muse_state_poke();
    stats_hide();
    switch (which) {
    case ICON_FOOD:
        if (!pet_feed(false)) {
            bubble_show(v.stage == PET_EGG ? "..." : v.asleep ? "zzz..." : "*munch* ...later.", 3);
        }
        break;
    case ICON_TRAIN:
        if (v.stage == PET_EGG || v.asleep) {
            bubble_show(v.stage == PET_EGG ? "..." : "zzz...", 3);
        } else {
            train_start(now);
        }
        break;
    case ICON_CLEAN:
        pet_clean();
        break;
    case ICON_MEDS:
        if (!pet_medicine()) {
            bubble_show("Not sick!", 3);
        }
        break;
    case ICON_LIGHT:
        pet_lights(!v.lights_off);
        break;
    case ICON_STATS:
        stats_show(now);
        break;
    case ICON_THEME:
        set_theme(s_theme + 1);
        bubble_show(theme()->name, 2);
        break;
    default:
        break;
    }
    s_toy_next_update = 0;
}

static void on_toy_icon(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    float now = (float)esp_timer_get_time() / 1e6f;
    if (s_train_on) {
        return;
    }
    select_icon(i);
    toy_activate(i, now);
}

static void on_toy_button(lv_event_t *e)
{
    int b = (int)(intptr_t)lv_event_get_user_data(e);
    float now = (float)esp_timer_get_time() / 1e6f;
    muse_state_poke();
    if (s_train_on) {
        if (b == 1) {
            train_fire(now);
        } else if (b == 2) {
            s_train_on = false;
            lv_obj_add_flag(s_train, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    switch (b) {
    case 0:   /* A: next icon */
        select_icon((s_toy_sel + 1) % ICON_COUNT);
        break;
    case 1:   /* B: go */
        if (s_toy_sel >= 0) {
            toy_activate(s_toy_sel, now);
        } else {
            stats_show(now);
        }
        break;
    default:   /* C: back */
        stats_hide();
        if (!lv_obj_has_flag(s_bubble, LV_OBJ_FLAG_HIDDEN)) {
            s_bubble_until = 0;
        } else {
            select_icon(-1);
        }
        break;
    }
}

static void on_train_tap(lv_event_t *e)
{
    (void)e;
    train_fire((float)esp_timer_get_time() / 1e6f);
}

static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static void build_toy(lv_obj_t *face)
{
    icon_init();
    s_toy_text = heap_caps_malloc(MUSE_TRANSCRIPT_MAX, MUSE_BIG_CAPS);
    if (s_toy_text) {
        s_toy_text[0] = '\0';
    }
    /* The shell, behind everything. */
    s_toy_frame = plain(face);
    lv_obj_set_size(s_toy_frame, s_w, s_h);
    lv_obj_set_pos(s_toy_frame, 0, 0);
    lv_obj_set_style_bg_opa(s_toy_frame, LV_OPA_COVER, 0);
    lv_obj_move_to_index(s_toy_frame, 0);
    s_toy_body = plain(face);
    lv_obj_set_size(s_toy_body, s_w - 8, s_h - 8);
    lv_obj_set_pos(s_toy_body, 4, 4);
    lv_obj_set_style_bg_opa(s_toy_body, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_toy_body, 26, 0);
    lv_obj_set_style_border_width(s_toy_body, 2, 0);
    lv_obj_move_to_index(s_toy_body, 1);
    s_toy_lcd = plain(face);
    lv_obj_set_size(s_toy_lcd, LCD_W, LCD_H);
    lv_obj_set_pos(s_toy_lcd, LCD_X, LCD_Y);
    lv_obj_set_style_bg_opa(s_toy_lcd, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_toy_lcd, 14, 0);
    lv_obj_set_style_border_width(s_toy_lcd, 3, 0);
    lv_obj_move_to_index(s_toy_lcd, 2);

    /* The icon bar along the LCD's top. */
    for (int i = 0; i < ICON_COUNT; i++) {
        lv_obj_t *cell = plain(face);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(cell, 36, 34);
        lv_obj_set_pos(cell, LCD_X + 12 + i * 40, LCD_Y + 8);
        lv_obj_set_style_radius(cell, 6, 0);
        lv_obj_add_event_cb(cell, on_toy_icon, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        s_toy_icons[i] = cell;
        s_toy_icon_imgs[i] = make_icon(cell, i, 24, 0xffffff);
        lv_obj_center(s_toy_icon_imgs[i]);
    }

    /* The strip under the creature: name, stage and age; mood; alerts; the system's caption. */
    s_toy_strip = make_label(face, &lv_font_unscii_16, 0xffffff);
    lv_obj_set_style_text_align(s_toy_strip, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_pos(s_toy_strip, LCD_X + 14, LCD_Y + LCD_H - 96);
    s_toy_mood = make_label(face, &lv_font_unscii_8, 0xffffff);
    lv_obj_set_style_text_align(s_toy_mood, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_pos(s_toy_mood, LCD_X + 14, LCD_Y + LCD_H - 74);
    for (int i = 0; i < 4; i++) {
        static const int ART[4] = { ICON_FOOD, ICON_CLEAN, ICON_MEDS, ICON_HEART };
        s_toy_alert[i] = make_icon(face, ART[i], 16, 0xffffff);
        lv_obj_set_pos(s_toy_alert[i], LCD_X + LCD_W - 14 - 20 * (4 - i), LCD_Y + LCD_H - 76);
        lv_obj_add_flag(s_toy_alert[i], LV_OBJ_FLAG_HIDDEN);
    }
    s_toy_caption = make_label(face, &lv_font_unscii_8, 0xffffff);
    lv_obj_set_width(s_toy_caption, LCD_W - 28);
    lv_label_set_long_mode(s_toy_caption, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(s_toy_caption, LCD_X + 14, LCD_Y + LCD_H - 46);
    lv_obj_add_flag(s_toy_caption, LV_OBJ_FLAG_HIDDEN);

    /* The buttons under the LCD. */
    static const char *const BTN[3] = { "A", "B", "C" };
    for (int b = 0; b < 3; b++) {
        lv_obj_t *btn = lv_button_create(face);
        lv_obj_remove_style_all(btn);
        lv_obj_set_size(btn, 58, 58);
        lv_obj_align(btn, LV_ALIGN_TOP_MID, (b - 1) * 96, LCD_Y + LCD_H + 14);
        lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(btn, 3, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_add_event_cb(btn, on_toy_button, LV_EVENT_CLICKED, (void *)(intptr_t)b);
        s_toy_btn_lbl[b] = make_label(btn, &lv_font_unscii_16, 0xffffff);
        lv_label_set_text(s_toy_btn_lbl[b], BTN[b]);
        lv_obj_center(s_toy_btn_lbl[b]);
        s_toy_btn[b] = btn;
    }

    /* The speech bubble, above the creature's head. */
    s_bubble_tail = plain(face);
    lv_obj_set_size(s_bubble_tail, 16, 16);
    lv_obj_set_style_bg_opa(s_bubble_tail, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_bubble_tail, 2, 0);
    lv_obj_set_style_transform_rotation(s_bubble_tail, 450, 0);
    lv_obj_set_style_transform_pivot_x(s_bubble_tail, 8, 0);
    lv_obj_set_style_transform_pivot_y(s_bubble_tail, 8, 0);
    lv_obj_add_flag(s_bubble_tail, LV_OBJ_FLAG_HIDDEN);
    s_bubble = plain(face);
    lv_obj_set_width(s_bubble, LCD_W - 28);
    lv_obj_set_height(s_bubble, LV_SIZE_CONTENT);
    lv_obj_set_pos(s_bubble, LCD_X + 14, LCD_Y + 8);
    lv_obj_set_style_bg_opa(s_bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_bubble, 12, 0);
    lv_obj_set_style_border_width(s_bubble, 2, 0);
    lv_obj_set_style_pad_all(s_bubble, 8, 0);
    lv_obj_add_flag(s_bubble, LV_OBJ_FLAG_HIDDEN);
    s_bubble_lbl = make_label(s_bubble, &lv_font_unscii_16, 0x000000);
    lv_obj_set_width(s_bubble_lbl, lv_pct(100));
    lv_obj_set_style_text_align(s_bubble_lbl, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_line_space(s_bubble_lbl, 2, 0);
    lv_label_set_long_mode(s_bubble_lbl, LV_LABEL_LONG_MODE_WRAP);

    /* The stats page, over the LCD. */
    s_stats = plain(face);
    lv_obj_set_size(s_stats, LCD_W - 24, 190);
    lv_obj_set_pos(s_stats, LCD_X + 12, LCD_Y + 56);
    lv_obj_set_style_bg_opa(s_stats, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_stats, 10, 0);
    lv_obj_set_style_border_width(s_stats, 2, 0);
    lv_obj_set_style_pad_all(s_stats, 10, 0);
    static const char *const NAMES[6] = { "FOOD", "REST", "FUN", "CLEAN", "LOVE", "POWER" };
    for (int i = 0; i < 6; i++) {
        s_stats_lbls[i] = make_label(s_stats, &lv_font_unscii_8, 0xffffff);
        lv_label_set_text(s_stats_lbls[i], NAMES[i]);
        lv_obj_set_style_text_align(s_stats_lbls[i], LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_pos(s_stats_lbls[i], 0, i * 18 + 1);
        lv_obj_t *bar = lv_bar_create(s_stats);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, LCD_W - 24 - 20 - 56, 9);
        lv_obj_set_pos(bar, 56, i * 18 + 1);
        lv_bar_set_range(bar, 0, 100);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(bar, 3, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_radius(bar, 3, LV_PART_INDICATOR);
        s_stats_bars[i] = bar;
    }
    s_stats_text = make_label(s_stats, &lv_font_unscii_8, 0xffffff);
    lv_obj_set_style_text_align(s_stats_text, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_line_space(s_stats_text, 4, 0);
    lv_obj_set_pos(s_stats_text, 0, 6 * 18 + 8);
    lv_obj_add_flag(s_stats, LV_OBJ_FLAG_HIDDEN);

    /* Training, over the LCD: a meter on the left, the boulder on the right. */
    s_train = plain(face);
    lv_obj_add_flag(s_train, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_train, LCD_W - 24, 232);
    lv_obj_set_pos(s_train, LCD_X + 12, LCD_Y + 46);
    lv_obj_set_style_bg_opa(s_train, LV_OPA_40, 0);
    lv_obj_set_style_radius(s_train, 10, 0);
    lv_obj_set_style_border_width(s_train, 2, 0);
    lv_obj_add_event_cb(s_train, on_train_tap, LV_EVENT_PRESSED, NULL);
    s_train_meter = plain(s_train);
    lv_obj_set_size(s_train_meter, 14, 170);
    lv_obj_set_pos(s_train_meter, 10, 40);
    lv_obj_set_style_bg_opa(s_train_meter, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_train_meter, 4, 0);
    s_train_zone = plain(s_train_meter);
    lv_obj_set_style_bg_opa(s_train_zone, LV_OPA_COVER, 0);
    s_train_marker = plain(s_train_meter);
    lv_obj_set_size(s_train_marker, 20, 8);
    lv_obj_set_style_bg_opa(s_train_marker, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_train_marker, 2, 0);
    s_train_rock = plain(s_train);
    lv_obj_set_size(s_train_rock, 64, 56);
    lv_obj_set_pos(s_train_rock, LCD_W - 24 - 76, 150);
    lv_obj_set_style_bg_opa(s_train_rock, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_train_rock, 22, 0);
    lv_obj_set_style_border_width(s_train_rock, 3, 0);
    static lv_point_precise_t CRACKS[4][3] = {
        { { 30, 2 }, { 24, 22 }, { 34, 40 } }, { { 8, 20 }, { 26, 26 }, { 44, 14 } }, { { 50, 8 }, { 40, 30 }, { 56, 48 } }, { { 12, 44 }, { 30, 36 }, { 20, 54 } },
    };
    for (int i = 0; i < 4; i++) {
        s_train_crack[i] = lv_line_create(s_train_rock);
        lv_line_set_points(s_train_crack[i], CRACKS[i], 3);
        lv_obj_set_style_line_width(s_train_crack[i], 3, 0);
        lv_obj_set_style_line_rounded(s_train_crack[i], true, 0);
        lv_obj_add_flag(s_train_crack[i], LV_OBJ_FLAG_HIDDEN);
    }
    s_train_fire = plain(s_train);
    lv_obj_set_size(s_train_fire, 16, 16);
    lv_obj_set_style_radius(s_train_fire, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_train_fire, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_train_fire, 2, 0);
    lv_obj_add_flag(s_train_fire, LV_OBJ_FLAG_HIDDEN);
    s_train_msg = make_label(s_train, &lv_font_unscii_16, 0xffffff);
    lv_obj_set_style_text_line_space(s_train_msg, 2, 0);
    lv_obj_align(s_train_msg, LV_ALIGN_TOP_MID, 0, 6);
    lv_obj_add_flag(s_train, LV_OBJ_FLAG_HIDDEN);

    /* The theme: the pet remembers it. */
    s_theme = pet_theme() % THEME_COUNT;
    apply_theme();

    /* The old chrome has no place on a toy. */
    lv_obj_add_flag(lv_obj_get_parent(s_wifi_icon), LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_state_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_name_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_caption_lbl, LV_OBJ_FLAG_HIDDEN);
    if (s_bar) {
        lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Every frame: the game and the bubble; four times a second the strip, alerts and bubble text. */
static void update_toy(float now, const char *caption, bool fresh)
{
    /* Chrome other code keeps showing. */
    if (s_mic_icon) {
        lv_obj_add_flag(s_mic_icon, LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < 2; i++) {
        if (s_dots[i]) {
            lv_obj_add_flag(s_dots[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    lv_obj_add_flag(s_state_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_caption_lbl, LV_OBJ_FLAG_HIDDEN);
    if (s_reply_box) {
        lv_obj_add_flag(s_reply_box, LV_OBJ_FLAG_HIDDEN);
    }
    train_tick(now);
    if (s_stats_until && now > s_stats_until) {
        stats_hide();
    }
    /* Words: the pet's (pet_ui_bubble) and Muse's replies both arrive through the transcript store. */
    static char heard[MUSE_HEARD_MAX];
    if (s_toy_text && muse_state_transcript(heard, sizeof(heard), s_toy_text, MUSE_TRANSCRIPT_MAX, &s_toy_transcript_version) &&
        s_toy_text[0]) {
        if (strlen(s_toy_text) > 110) {
            strcpy(s_toy_text + 107, "...");
        }
        float secs = s_bubble_until > now ? s_bubble_until - now : TOY_BUBBLE_S;
        bubble_show(s_toy_text, secs);
    }
    if (!lv_obj_has_flag(s_bubble, LV_OBJ_FLAG_HIDDEN) && now > s_bubble_until) {
        lv_obj_add_flag(s_bubble, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_bubble_tail, LV_OBJ_FLAG_HIDDEN);
    }
    if (!lv_obj_has_flag(s_bubble, LV_OBJ_FLAG_HIDDEN)) {
        /* The tail hangs under the bubble, pointing at the head. */
        int bh = lv_obj_get_height(s_bubble);
        lv_obj_set_pos(s_bubble_tail, LCD_X + 14 + (LCD_W - 28) * 58 / 100, LCD_Y + 8 + bh - 10);
        lv_obj_move_to_index(s_bubble_tail, lv_obj_get_index(s_bubble) - 1);
    }
    /* The icon bar makes way for the bubble. */
    bool bubbling = !lv_obj_has_flag(s_bubble, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < ICON_COUNT; i++) {
        lv_obj_set_flag(s_toy_icons[i], LV_OBJ_FLAG_HIDDEN, bubbling || s_train_on);
    }
    if (s_toy_debug_req) {
        int req = s_toy_debug_req;
        s_toy_debug_req = 0;
        if (req == 1) {
            stats_show(now);
        } else if (req == 2 && !s_train_on) {
            train_start(now);
        } else if (req == 3) {
            bubble_show("Rawr! Testing the bubble, one two three. Rawr rawr.", 20);
        }
    }
    if (fresh) {
        lv_label_set_text(s_toy_caption, caption);
        lv_obj_set_flag(s_toy_caption, LV_OBJ_FLAG_HIDDEN, !caption[0]);
    }
    if (now < s_toy_next_update) {
        return;
    }
    s_toy_next_update = now + 0.25f;
    pet_view_t v;
    pet_view(&v);
    if (v.theme % THEME_COUNT != s_theme) {
        set_theme(v.theme);   /* Muse switched it */
    }
    char stage[12], mood[16], age[16];
    strlcpy(stage, pet_stage_name(v.stage), sizeof(stage));
    strlcpy(mood, pet_mood_name(v.mood), sizeof(mood));
    upper(stage);
    upper(mood);
    if (v.age_min < 60) {
        snprintf(age, sizeof(age), "%uM", (unsigned)v.age_min);
    } else if (v.age_min < 24 * 60) {
        snprintf(age, sizeof(age), "%uH", (unsigned)(v.age_min / 60));
    } else {
        snprintf(age, sizeof(age), "%uD", (unsigned)(v.age_min / 1440));
    }
    if (v.stage == PET_EGG) {
        lv_label_set_text_fmt(s_toy_strip, "EGG  GEN %u", (unsigned)v.generation);
        lv_label_set_text_fmt(s_toy_mood, "%s  WARMTH %d%%", v.egg_warmth < 0.3f ? "TAP IT TO WARM IT" : v.egg_warmth < 0.7f ? "IT'S MOVING..." : "ALMOST!",
                              (int)(v.egg_warmth * 100));
    } else {
        lv_label_set_text_fmt(s_toy_strip, "%s", v.name[0] ? v.name : "(NO NAME)");
        lv_label_set_text_fmt(s_toy_mood, "%s %s  %s%s  HP %d  PWR %d", stage, age, v.sick ? "SICK " : "", mood, v.health, v.power);
    }
    bool awake = !v.asleep && v.stage != PET_EGG;
    bool blink_on = fmodf(now, 1.0f) < 0.6f;
    lv_obj_set_flag(s_toy_alert[0], LV_OBJ_FLAG_HIDDEN, !(awake && v.needs[PET_NEED_FOOD] < 30 && blink_on));
    lv_obj_set_flag(s_toy_alert[1], LV_OBJ_FLAG_HIDDEN, !(awake && (v.needs[PET_NEED_CLEAN] < 30 || v.poops >= 2) && blink_on));
    lv_obj_set_flag(s_toy_alert[2], LV_OBJ_FLAG_HIDDEN, !(v.sick && blink_on));
    lv_obj_set_flag(s_toy_alert[3], LV_OBJ_FLAG_HIDDEN, !(awake && (v.needs[PET_NEED_BOND] < 30 || v.needs[PET_NEED_FUN] < 30) && blink_on));
    if (s_stats_until) {
        stats_show(now);
        s_stats_until = s_stats_until;   /* refreshed, same deadline */
    }
}
#endif

/* Icons beside the physical buttons, in place of an instruction caption. */
static void build_button_icons(lv_obj_t *face)
{
    const muse_button_hint_t *t = &muse_board->talk_hint, *a = &muse_board->aux_hint;
    s_mic_icon = make_mic(face, s_tall ? 24 : s_small ? 12 : 26);
    lv_obj_align(s_mic_icon, t->align, t->x, t->y);
    set_mic_color(COLOR_DIM);

    /* Without touch the aux button opens the menu rather than sleeping. A board
     * that leaves aux_hint out has no button to put an icon beside. */
    if (a->align == LV_ALIGN_DEFAULT) {
        return;
    }
    s_aux_icon = make_label(face, s_small ? &lv_font_montserrat_14 : &lv_font_montserrat_28, COLOR_DIM);
    lv_label_set_text(s_aux_icon, muse_board->touch ? LV_SYMBOL_POWER : LV_SYMBOL_LIST);
    lv_obj_align(s_aux_icon, a->align, a->x, a->y);
}

static void on_canvas_clicked(lv_event_t *e)
{
    (void)e;
    muse_state_make_happy();
#if CONFIG_MUSE_PET
    pet_tap();
#endif
}

static const lv_font_t *font_pick(const lv_font_t *full, const lv_font_t *compact)
{
    return s_small ? compact : full;
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(l, "");
    return l;
}

static void show_speaker(bool on)
{
    /* Off is the one that stands out, like a lit flashlight button. */
    lv_obj_set_style_bg_color(s_speaker, lv_color_hex(on ? COLOR_METER_OFF : COLOR_LIT), 0);
    lv_obj_set_style_text_color(s_speaker_icon, lv_color_hex(on ? COLOR_DIM : 0x000000), 0);
    lv_label_set_text(s_speaker_icon, on ? LV_SYMBOL_VOLUME_MAX : LV_SYMBOL_MUTE);
    s_shown_speaker = on;
}

static void set_speaker_size(void *obj, int32_t px)
{
    lv_obj_set_size(obj, px, px);
}

/* Swells over the long press, so the toggle lands as it reaches full size. */
static void speaker_grow(bool grow)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_speaker);
    lv_anim_set_exec_cb(&a, set_speaker_size);
    lv_anim_set_values(&a, lv_obj_get_width(s_speaker), SPEAKER_PX + (grow ? SPEAKER_GROW_PX : 0));
    lv_anim_set_duration(&a, grow ? SPEAKER_HOLD_MS : 150);
    lv_anim_start(&a);
}

/* Touch and hold to toggle, like the lock screen's flashlight; a tap only says so. */
static void on_speaker_event(lv_event_t *e)
{
    bool on = muse_settings_speaker_on();
    bool idle = muse_state_mode(NULL) == MUSE_MODE_IDLE;   /* don't cover a reply's captions */
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
        speaker_grow(true);
        break;
    case LV_EVENT_LONG_PRESSED:
        muse_settings_set_speaker_on(!on);
        show_speaker(!on);
        if (idle) {
            muse_state_set_caption(on ? "SPEAKER OFF" : "SPEAKER ON");
        }
        break;
    case LV_EVENT_SHORT_CLICKED:
        if (idle) {
            muse_state_set_caption(on ? "HOLD TO MUTE" : "HOLD TO UNMUTE");
        }
        break;
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:
        speaker_grow(false);
        break;
    default:
        break;
    }
}

/* Upper left by the status icons, where a thumb finds it without covering the reply. */
static void build_speaker(lv_obj_t *face, int x, int y)
{
    s_speaker = lv_obj_create(face);
    lv_obj_remove_style_all(s_speaker);
    lv_obj_set_size(s_speaker, SPEAKER_PX, SPEAKER_PX);
    lv_obj_set_style_radius(s_speaker, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_speaker, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_speaker, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_ext_click_area(s_speaker, 12);   /* a fingertip is bigger than the circle */
    static const lv_event_code_t EVENTS[] = { LV_EVENT_PRESSED, LV_EVENT_LONG_PRESSED, LV_EVENT_SHORT_CLICKED,
                                              LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST };
    for (size_t i = 0; i < sizeof(EVENTS) / sizeof(EVENTS[0]); i++) {
        lv_obj_add_event_cb(s_speaker, on_speaker_event, EVENTS[i], NULL);
    }
    s_speaker_icon = make_label(s_speaker, &lv_font_montserrat_28, COLOR_DIM);
    lv_obj_center(s_speaker_icon);
    lv_obj_align(s_speaker, LV_ALIGN_CENTER, x, y);
    show_speaker(muse_settings_speaker_on());
}

static void set_canvas_px(int px)
{
    if ((int)s_muse_src.header.w == px) {
        return;
    }
    s_muse_src.header.w = px;
    s_muse_src.header.h = px;
    s_muse_src.header.stride = px * sizeof(uint16_t);
    muse_pixel_set_size(px);
    lv_image_set_src(s_canvas, &s_muse_src);   /* picks up the new size */
}

static void move_muse_t(void *obj, int32_t t)
{
    (void)obj;
    set_canvas_px(s_from_px + (s_to_px - s_from_px) * t / 256);
    s_muse_y = s_from_y + (s_to_y - s_from_y) * t / 256;
    lv_obj_align(s_canvas, LV_ALIGN_CENTER, 0, s_muse_y);
}

/* Eases Muse to `px` centred at `y`, from wherever it is now. */
static void move_muse(int px, int y)
{
    s_from_px = s_muse_src.header.w;
    s_from_y = s_muse_y;
    s_to_px = px;
    s_to_y = y;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_canvas);
    lv_anim_set_exec_cb(&a, move_muse_t);
    lv_anim_set_values(&a, 0, 256);
    lv_anim_set_duration(&a, ANSWER_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

/* Switches to answer layout `which`, or back to the usual one for -1. */
static void set_answer(int which)
{
    const answer_layout_t *was = s_answer >= 0 ? &s_answers[s_answer] : NULL;
    const answer_layout_t *l = which >= 0 ? &s_answers[which] : NULL;
    s_answer = which;
    const size_t n = sizeof(s_answers[0].hides) / sizeof(s_answers[0].hides[0]);
    for (size_t i = 0; was && i < n; i++) {
        if (was->hides[i]) {
            lv_obj_remove_flag(was->hides[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    for (size_t i = 0; l && i < n; i++) {
        if (l->hides[i]) {
            lv_obj_add_flag(l->hides[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (!muse_board->round) {
        if (s_ring) {
            lv_obj_set_flag(s_ring, LV_OBJ_FLAG_HIDDEN, l != NULL);   /* the reply runs past a rectangle's ring */
        }
    }
    if (l) {
        lv_obj_set_size(reply_area(), l->w, l->h);
        lv_obj_align(reply_area(), LV_ALIGN_CENTER, 0, l->top + l->h / 2);
        lv_obj_set_style_text_align(s_reply_lbl, l->align, 0);
    }
    move_muse(l ? l->px : s_canvas_px, l ? l->y : s_big_y);
}

/* Whether a reply `w` px wide fits across the screen `y` px from the centre. */
static bool fits_across(int w, int y, int ring_in)
{
    if (!muse_board->round) {
        return w <= s_w - 16;
    }
    int r = ring_in - 6;
    return w * w / 4 + y * y <= r * r;
}

/* How far down a reply `w` px wide can reach: inside the ring, above the page dots. */
static int reply_bottom(int w, int ring_in)
{
    int bottom = s_h / 2 - 28;
    if (muse_board->round) {
        int r = ring_in - 6;
        int chord = w / 2 < r ? (int)sqrtf((float)(r * r - w * w / 4)) : 0;
        bottom = chord < bottom ? chord : bottom;
    }
    return bottom;
}

static void set_reply_box(answer_layout_t *l, int cols, int lines, int top, int cw, int pitch)
{
    l->cols = cols;
    l->lines = lines;
    l->w = cols * cw;
    l->h = lines * pitch - CAPTION_LINE_SPACE;
    l->top = top;
}

/* The hint icons a layout's reply would cover go while it's up. */
static void add_hides(answer_layout_t *l, int n)
{
    lv_area_t box = {
        .x1 = s_w / 2 - l->w / 2, .y1 = s_h / 2 + l->top,
        .x2 = s_w / 2 + l->w / 2 - 1, .y2 = s_h / 2 + l->top + l->h - 1,
    };
    lv_obj_t *const hints[] = { s_mic_icon, s_aux_icon };
    for (size_t i = 0; i < 2; i++) {
        if (!hints[i]) {
            continue;   /* no aux icon on this board */
        }
        lv_area_t a;
        lv_obj_get_coords(hints[i], &a);
        if (a.x1 <= box.x2 && a.x2 >= box.x1 && a.y1 <= box.y2 && a.y2 >= box.y1) {
            l->hides[n++] = hints[i];
        }
    }
}

/*
 * The answer layouts, both with Muse centred. Heard: Muse a size smaller,
 * where it was if there's room, over three lines at the bottom. Read: Muse
 * small under the status line, and under it and the speaker button the
 * biggest page of reply text that fits inside the ring.
 */
static void build_answer(lv_obj_t *face, int ring_in)
{
    int spk_r = (SPEAKER_PX + SPEAKER_GROW_PX) / 2;
    int spk_x = -s_w / 2 + 8 + spk_r, spk_y = -s_h / 2 + 8 + spk_r;
    if (muse_board->round) {
        spk_y = -ring_in * 5 / 8;
        int d = ring_in - spk_r - 4;   /* just inside the ring, even when swollen */
        spk_x = -(int)sqrtf((float)(d * d - spk_y * spk_y));
    }
    const lv_font_t *font = &lv_font_unscii_16;
    int cw = lv_font_get_glyph_width(font, 'M', ' ');
    int pitch = lv_font_get_line_height(font) + CAPTION_LINE_SPACE;

    answer_layout_t *l = &s_answers[ANSWER_HEARD];
    int cell = s_canvas_px / MUSE_PX_W - 1;
    cell = cell > MINI_CELL_PX ? cell : MINI_CELL_PX;
    l->px = MUSE_PX_W * cell;
    l->y = s_big_y;
    l->align = LV_TEXT_ALIGN_CENTER;
    int h = 3 * pitch - CAPTION_LINE_SPACE;
    int art_bottom = l->y + l->px / 2 - ART_BLANK_ROWS * cell;
    set_reply_box(l, CAPTION_W / cw, 3, reply_bottom(CAPTION_W, ring_in) - h, cw, pitch);
    for (int c = 24; c > l->cols; c--) {   /* wider if it still clears Muse */
        int top = reply_bottom(c * cw, ring_in) - h;
        if (top >= art_bottom + 6 && fits_across(c * cw, top, ring_in)) {
            set_reply_box(l, c, 3, top, cw, pitch);
            break;
        }
    }
    if (l->top < art_bottom + 6) {
        l->y -= art_bottom + 6 - l->top;   /* no room under Muse: it moves up */
    }

    l = &s_answers[ANSWER_READ];
    int status_bottom = 20 + s_dy + 16 - s_h / 2;
    l->px = MUSE_PX_W * MINI_CELL_PX;
    l->y = status_bottom + 2 + l->px / 2;   /* its sparkles clear of the status line */
    l->align = LV_TEXT_ALIGN_LEFT;
    art_bottom = l->y + l->px / 2 - ART_BLANK_ROWS * MINI_CELL_PX;
    int top = (art_bottom > spk_y + spk_r ? art_bottom : spk_y + spk_r) + 8;
    set_reply_box(l, 16, 2, top, cw, pitch);
    /* The widest page isn't the biggest: a round screen narrows towards the bottom. */
    for (int c = 12; c <= 24 && fits_across(c * cw, top, ring_in); c++) {
        int n = (reply_bottom(c * cw, ring_in) - top + CAPTION_LINE_SPACE) / pitch;
        /* A third of the caption spare for characters wider than a byte. */
        while ((c + 1) * n > MUSE_CAPTION_MAX * 2 / 3) {
            n--;
        }
        if (c * n > l->cols * l->lines) {
            set_reply_box(l, c, n, top, cw, pitch);
        }
    }
    ESP_LOGI(TAG, "reply pages: %d x %d heard, %d x %d read", s_answers[ANSWER_HEARD].cols,
             s_answers[ANSWER_HEARD].lines, l->cols, l->lines);

    s_reply_lbl = make_label(face, font, COLOR_CAPTION);
    lv_obj_set_style_text_line_space(s_reply_lbl, CAPTION_LINE_SPACE, 0);
    /* Pages come wrapped to fit; the transcript while thinking doesn't. */
    lv_label_set_long_mode(s_reply_lbl, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_remove_flag(s_reply_lbl, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_reply_lbl, LV_OBJ_FLAG_HIDDEN);

    if (muse_board->touch) {
        build_speaker(face, spk_x, spk_y);
    }

    /* Out of the way while a page is up: the mode name, where Muse was. */
    lv_obj_update_layout(face);
    s_answers[ANSWER_READ].hides[0] = s_state_lbl;
    s_answers[ANSWER_READ].hides[1] = s_name_lbl;   /* the reply takes the top too */
    add_hides(&s_answers[ANSWER_READ], 2);
    add_hides(&s_answers[ANSWER_HEARD], 0);
}

/*
 * A full circle's arc masks every pixel of every row it's asked to draw, hole
 * and corners included, which costs a full-screen redraw (a swipe) several ms.
 * So the ring is drawn a slab of rows at a time, each only as wide as the ring
 * is there: where a slab crosses the hole, a left piece and a right one.
 */
#define RING_SLAB_ROWS 52

static void on_ring_draw(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    const lv_area_t clip = layer->_clip_area;
    lv_area_t c;
    lv_obj_get_coords(s_ring, &c);
    int32_t cx = (c.x1 + c.x2) / 2;
    int32_t cy = (c.y1 + c.y2) / 2;
    int32_t out = lv_area_get_width(&c) / 2 + 1;                                     /* with antialiasing */
    int32_t hole = out - 1 - lv_obj_get_style_arc_width(s_ring, LV_PART_MAIN) - 2;   /* without */
    lv_event_stop_processing(e);
    for (int32_t y = LV_MAX(clip.y1, cy - out); y <= LV_MIN(clip.y2, cy + out); y += RING_SLAB_ROWS) {
        int32_t y2 = LV_MIN(y + RING_SLAB_ROWS - 1, clip.y2);
        int32_t near = y <= cy && cy <= y2 ? 0 : LV_MIN(LV_ABS(y - cy), LV_ABS(y2 - cy));
        int32_t far = LV_MAX(LV_ABS(y - cy), LV_ABS(y2 - cy));
        int32_t ow = near < out ? (int32_t)ceilf(sqrtf((float)(out * out - near * near))) : 0;
        int32_t iw = far < hole ? (int32_t)sqrtf((float)(hole * hole - far * far)) : 0;
        lv_area_t pieces[2] = {
            { cx - ow, y, iw ? cx - iw : cx + ow, y2 },
            { cx + iw, y, cx + ow, y2 },
        };
        for (int i = 0; i < (iw ? 2 : 1); i++) {
            if (lv_area_intersect(&layer->_clip_area, &clip, &pieces[i])) {
                lv_obj_event_base(NULL, e);   /* the arc's own drawing */
            }
        }
    }
    layer->_clip_area = clip;
}

static void build_screen(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *face = scr;
    if (muse_board->touch) {
        /* Swipe left from Muse for settings. */
        s_tv = lv_tileview_create(scr);
        lv_obj_set_style_bg_color(s_tv, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_tv, LV_OPA_COVER, 0);
        lv_obj_set_scrollbar_mode(s_tv, LV_SCROLLBAR_MODE_OFF);
        s_face = lv_tileview_add_tile(s_tv, 0, 0, LV_DIR_RIGHT);
        /* It never scrolls, but LVGL would size its scrollbars from all its
         * children every time it draws any part of it. */
        lv_obj_set_scrollbar_mode(s_face, LV_SCROLLBAR_MODE_OFF);
        s_settings = lv_tileview_add_tile(s_tv, 1, 0, LV_DIR_LEFT);
        face = s_face;
    }

    if (!s_small) {
        /* Progress ring around the bezel. */
        int d = (s_w < s_h ? s_w : s_h) - 8;
        if (muse_board->avatar_px > 0 && s_canvas_px + 56 < d) {
            d = s_canvas_px + 56;        /* the ring follows a Muse that was made smaller */
        }
        s_ring = lv_arc_create(face);
        lv_obj_set_size(s_ring, d, d);
        lv_obj_center(s_ring);
        lv_arc_set_bg_angles(s_ring, 0, 360);
        lv_arc_set_rotation(s_ring, 270);
        lv_arc_set_range(s_ring, 0, RING_RANGE);
        lv_arc_set_value(s_ring, 0);
        lv_obj_remove_style(s_ring, NULL, LV_PART_KNOB);
        lv_obj_remove_flag(s_ring, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_arc_width(s_ring, 6, LV_PART_MAIN);
        lv_obj_set_style_arc_color(s_ring, lv_color_hex(COLOR_RING_BG), LV_PART_MAIN);
        lv_obj_set_style_arc_width(s_ring, 6, LV_PART_INDICATOR);
        lv_obj_set_style_arc_rounded(s_ring, false, LV_PART_INDICATOR);
        lv_obj_add_event_cb(s_ring, on_ring_draw, LV_EVENT_DRAW_MAIN | LV_EVENT_PREPROCESS, NULL);
    }

    /*
     * Full layout, from the bottom up (offsets from the centre): two caption
     * lines as low as the ring leaves them room, the level meter, then Muse,
     * whose blank bottom rows can tuck in behind the meter.
     */
    int ring_in = (s_w < s_h ? s_w : s_h) / 2 - 10;   /* the ring's inner edge */
    int cap_h = 2 * lv_font_get_line_height(&lv_font_unscii_16) + CAPTION_LINE_SPACE;
    int cap_bottom = 179;                              /* a 466 px circle's; fine for rectangles */
    if (muse_board->round) {
        cap_bottom = (int)sqrtf((float)(ring_in * ring_in - CAPTION_W * CAPTION_W / 4)) - 3;
    }
    int cap_top = cap_bottom - cap_h;
    int meter_y = cap_top - 6 - METER_SEG_PX / 2;
    int art_bottom = meter_y - METER_SEG_PX / 2 - 4;
    s_big_y = s_small ? 0 : art_bottom - (s_canvas_px / 2 - ART_BLANK_ROWS * (s_canvas_px / MUSE_PX_W));

    /* The character. */
    muse_image_init();
    s_canvas = lv_image_create(face);
    lv_image_set_src(s_canvas, &s_muse_src);
    lv_obj_align(s_canvas, LV_ALIGN_CENTER, 0, s_big_y);
    s_muse_y = s_big_y;
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_canvas, on_canvas_clicked, LV_EVENT_CLICKED, NULL);
    if (s_ring) {
        /* The canvas's black corners reach the bezel; keep the ring on top. */
        lv_obj_move_foreground(s_ring);
    }
    build_button_icons(face);

    /* Status line: connectivity icons + power. */
    lv_obj_t *status = lv_obj_create(face);
    lv_obj_remove_style_all(status);
    lv_obj_remove_flag(status, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(status, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(status, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(status, s_small ? 4 : 8, 0);
    lv_obj_align(status, LV_ALIGN_TOP_MID, 0, s_small ? 1 : 20 + s_dy);
    s_wifi_icon = make_label(status, &lv_font_montserrat_14, COLOR_DIM);
    s_ble_icon = make_label(status, &lv_font_montserrat_14, COLOR_DIM);
    s_power_lbl = make_label(status, &lv_font_unscii_8, COLOR_DIM);

    /* The compact layout leaves the state to the avatar and the caption,
     * unless the screen is tall enough to fit it in small type above Muse. */
    s_state_lbl = make_label(face, s_small ? &lv_font_unscii_8 : &lv_font_unscii_16, 0xffffff);
    lv_obj_set_style_text_letter_space(s_state_lbl, s_small ? 1 : 2, 0);
    lv_obj_align(s_state_lbl, LV_ALIGN_TOP_MID, 0, s_small ? 22 : 40 + s_dy);
    lv_obj_set_flag(s_state_lbl, LV_OBJ_FLAG_HIDDEN, s_small && !s_tall && s_h < 200);

    /* This gadget's own name, dim under the state while it's unpaired: with
     * more than one on the bench, the screen says which one to pick in the
     * Muse app. update_chrome() fills it in, shortens it to the hex tail on a
     * screen too narrow for the whole thing, and empties it once paired. */
    s_name_lbl = make_label(face, s_small ? &lv_font_unscii_8 : &lv_font_unscii_16, COLOR_DIM);
    lv_obj_align(s_name_lbl, LV_ALIGN_TOP_MID, 0, s_small ? 32 : 60 + s_dy);
    /* Same rule as the state label: a square 128 px screen centres Muse over
     * these rows, so there's nowhere to put this without covering the face. */
    lv_obj_set_flag(s_name_lbl, LV_OBJ_FLAG_HIDDEN, s_small && !s_tall && s_h < 200);

    s_caption_lbl = make_label(face, font_pick(&lv_font_unscii_16, &lv_font_unscii_8), COLOR_CAPTION);
    if (s_small) {
        /* Two lines over the bottom of the face, on a dark band so they stay
         * legible. A tall screen has room to keep them above the mic icon. */
        lv_obj_set_size(s_caption_lbl, s_w, 2 * 8 + 2 + 4);
        lv_obj_set_style_pad_ver(s_caption_lbl, 2, 0);
        lv_obj_set_style_text_line_space(s_caption_lbl, 2, 0);
        lv_obj_set_style_bg_color(s_caption_lbl, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_caption_lbl, LV_OPA_70, 0);
        lv_label_set_long_mode(s_caption_lbl, LV_LABEL_LONG_MODE_DOTS);
        /* Touch screens need the caption above the navigation dots too. */
        lv_obj_align(s_caption_lbl, LV_ALIGN_BOTTOM_MID, 0, (s_tall || s_tv) ? -30 : -3);

        s_bar = lv_obj_create(face);
        lv_obj_remove_style_all(s_bar);
        lv_obj_set_size(s_bar, 0, 3);
        lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, 0);
        lv_obj_align(s_bar, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        if (s_tall && s_w >= 200) {
            const int pitch = 16 + CAPTION_LINE_SPACE;
            /* Room under the full-size figure (its canvas's blank rows don't count) for some
             * lines of the 16 px pixel font (320x480): Muse stays big, moved up under the
             * status row, and the whole reply scrolls under its feet, above the mic icon
             * and page dots. */
            int blank_top, blank_bottom;
            muse_pixel_blank_rows(s_canvas_px, &blank_top, &blank_bottom);
            const int canvas_top = 20 - blank_top;
            const int box_top = canvas_top + s_canvas_px - blank_bottom + 6, box_bottom = s_h - 34;
            if (box_bottom - box_top >= 6 * pitch) {
                for (int k = 0; k < 2; k++) {
                    answer_layout_t *l = &s_answers[k];
                    l->px = s_canvas_px;
                    l->y = canvas_top + s_canvas_px / 2 - s_h / 2;
                    l->w = s_w - 12;
                    l->h = box_bottom - box_top;
                    l->top = box_top - s_h / 2;
                    l->cols = (l->w - TRANSCRIPT_PAD_R) / 8;
                    /* The box shows the whole reply; the voice task's pages, which the caption
                     * follows, are three lines (two new ones each turn) so the following keeps
                     * up with the speech. */
                    l->lines = 3;
                    l->align = LV_TEXT_ALIGN_LEFT;
                    l->hides[0] = s_state_lbl;
                    l->hides[1] = s_name_lbl;
                    l->hides[2] = s_caption_lbl;
                }
                build_transcript(face);
#if CONFIG_MUSE_PET
                /* The creature lives at the top; its panel takes the room under its feet, above
                 * the caption line and the mic icon. */
                build_toy(face);
                s_big_y = s_answers[0].y;
                lv_obj_align(s_canvas, LV_ALIGN_CENTER, 0, s_big_y);
                s_muse_y = s_big_y;
#endif
                ESP_LOGI(TAG, "transcript: %d x %d px under a %d px Muse", s_answers[0].w, s_answers[0].h, s_canvas_px);
                return;
            }
            /* Tall enough (240x320) for a real reply page: Muse shrinks to one art pixel per
             * pixel at the top and the reply, in the 16 px pixel font, takes the rest. */
            const int px = MUSE_PX_W;
            const int muse_y = 8 + px / 2, top = 8 + px + 10, bottom = s_h - 44;
            for (int k = 0; k < 2; k++) {
                answer_layout_t *l = &s_answers[k];
                l->px = px;
                l->y = muse_y - s_h / 2;
                l->w = s_w - 12;
                l->h = (bottom - top) / pitch * pitch;
                l->top = top - s_h / 2;
                l->cols = l->w / 8;
                l->lines = l->h / pitch;
                while ((l->cols + 1) * l->lines > MUSE_CAPTION_MAX * 2 / 3) {
                    l->lines--;
                }
                l->align = LV_TEXT_ALIGN_LEFT;
                l->hides[0] = s_state_lbl;
                l->hides[1] = s_name_lbl;
                l->hides[2] = s_caption_lbl;
            }
            s_reply_lbl = make_label(face, &lv_font_unscii_16, COLOR_CAPTION);
            lv_obj_set_style_text_line_space(s_reply_lbl, CAPTION_LINE_SPACE, 0);
            lv_label_set_long_mode(s_reply_lbl, LV_LABEL_LONG_MODE_WRAP);
            lv_obj_remove_flag(s_reply_lbl, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(s_reply_lbl, LV_OBJ_FLAG_HIDDEN);
            ESP_LOGI(TAG, "reply page: %d x %d", s_answers[0].cols, s_answers[0].lines);
        }
        return;
    }
    /* Fixed height: a longer caption ends in dots rather than growing into the ring. */
    lv_obj_set_size(s_caption_lbl, CAPTION_W, cap_h);
    lv_obj_set_style_text_line_space(s_caption_lbl, CAPTION_LINE_SPACE, 0);
    lv_label_set_long_mode(s_caption_lbl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(s_caption_lbl, LV_ALIGN_CENTER, 0, cap_top + cap_h / 2);

    /* Chunky level meter. */
    int span = METER_SEGS * (METER_SEG_PX + METER_GAP_PX) - METER_GAP_PX;
    for (int i = 0; i < METER_SEGS; i++) {
        lv_obj_t *seg = lv_obj_create(face);
        lv_obj_remove_style_all(seg);
        lv_obj_remove_flag(seg, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(seg, METER_SEG_PX, METER_SEG_PX);
        lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(seg, lv_color_hex(COLOR_METER_OFF), 0);
        int x = -span / 2 + i * (METER_SEG_PX + METER_GAP_PX) + METER_SEG_PX / 2;
        lv_obj_align(seg, LV_ALIGN_CENTER, x, meter_y);
        s_meter[i] = seg;
    }

    build_answer(face, ring_in);
}


static void on_cover_event(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
        /* Wake on touch; keep the (now clear) cover until release so the tap
         * doesn't land on whatever is underneath. */
        muse_state_set_asleep(false);
        lv_obj_set_style_bg_opa(s_cover, LV_OPA_TRANSP, 0);
    } else if (!muse_state_asleep()) {
        lv_obj_add_flag(s_cover, LV_OBJ_FLAG_HIDDEN);
    }
}

/* With the display lock held. */
static void image_hide_locked(void)
{
    xSemaphoreTake(s_image_mutex, portMAX_DELAY);
    if (s_image_buf) {
        lv_obj_add_flag(s_image, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(s_image, NULL);
        heap_caps_free(s_image_buf);
        s_image_buf = NULL;
        s_image_dsc.data = NULL;
        s_image_dirty = false;
    }
    xSemaphoreGive(s_image_mutex);
}

/* Each frame: shows a new image and redraws what the download changed. */
static void image_sync(void)
{
    xSemaphoreTake(s_image_mutex, portMAX_DELAY);
    if (s_image_buf && !s_image_dsc.data) {
        s_image_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        s_image_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        s_image_dsc.header.w = s_w;
        s_image_dsc.header.h = s_h;
        s_image_dsc.header.stride = s_w * sizeof(uint16_t);
        s_image_dsc.data_size = (size_t)s_w * s_h * sizeof(uint16_t);
        s_image_dsc.data = (const uint8_t *)s_image_buf;
        muse_menu_close();
        lv_image_set_src(s_image, &s_image_dsc);
        lv_obj_remove_flag(s_image, LV_OBJ_FLAG_HIDDEN);
        muse_state_set_asleep(false);
    }
    if (s_image_dirty) {
        lv_obj_invalidate_area(s_image, &s_image_area);
        s_image_dirty = false;
    }
    xSemaphoreGive(s_image_mutex);
}

static void on_image_clicked(lv_event_t *e)
{
    (void)e;
#if CONFIG_MUSE_WATCHER_CAMERA
    if (watcher_camera_preview_active()) {
        watcher_camera_preview_toggle();
        return;
    }
#endif
    image_hide_locked();
}

#if CONFIG_MUSE_WATCHER_CAMERA
static void on_camera_hint_clicked(lv_event_t *e)
{
    (void)e;
    if (watcher_camera_preview_active()) watcher_camera_preview_toggle();
}
#endif

static void on_any_press(lv_event_t *e)
{
    (void)e;
    muse_state_poke();
}

static void build_overlays(void)
{
    lv_obj_t *scr = lv_screen_active();

    /* Page dots. */
    for (int i = 0; i < 2 && s_tv; i++) {
        lv_obj_t *d = lv_obj_create(scr);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, 8, 8);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(d, lv_color_hex(COLOR_DOT_OFF), 0);
        lv_obj_remove_flag(d, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(d, LV_ALIGN_BOTTOM_MID, i ? 8 : -8, -14);
        s_dots[i] = d;
    }

    /* A downloaded image: over everything on the screen (and in snapshots),
     * under the pairing code and sleep cover on the top layer. */
    s_image = lv_image_create(scr);
    lv_obj_set_pos(s_image, 0, 0);
    lv_obj_add_flag(s_image, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_image, on_image_clicked, LV_EVENT_CLICKED, NULL);
#if CONFIG_MUSE_WATCHER_CAMERA
    s_camera_hint = lv_btn_create(scr);
    lv_obj_set_size(s_camera_hint, 244, 46);
    lv_obj_align(s_camera_hint, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_bg_color(s_camera_hint, lv_color_hex(0x201a35), 0);
    lv_obj_set_style_bg_opa(s_camera_hint, LV_OPA_90, 0);
    lv_obj_set_style_border_color(s_camera_hint, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_border_width(s_camera_hint, 2, 0);
    lv_obj_set_style_radius(s_camera_hint, 18, 0);
    lv_obj_add_flag(s_camera_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *hint_text = lv_label_create(s_camera_hint);
    lv_label_set_text(hint_text, "TAP TO TAKE PHOTO");
    lv_obj_center(hint_text);
    lv_obj_add_event_cb(s_camera_hint, on_camera_hint_clicked, LV_EVENT_CLICKED, NULL);
#endif

    /* BLE pairing code, or the Muse app's ask for the talk button: a centred
     * column in one typeface, the code large. The small card grows with the hint. */
    s_pair = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_pair);
    lv_obj_set_size(s_pair, s_small ? s_w - 8 : 300, s_small ? LV_SIZE_CONTENT : 150);
    lv_obj_center(s_pair);
    lv_obj_set_flex_flow(s_pair, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_pair, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(s_pair, s_small ? 10 : 18, 0);
    lv_obj_set_style_pad_hor(s_pair, s_small ? 6 : 16, 0);
    lv_obj_set_style_pad_row(s_pair, s_small ? 4 : 10, 0);
    lv_obj_set_style_radius(s_pair, s_small ? 10 : 24, 0);
    lv_obj_set_style_bg_opa(s_pair, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_pair, lv_color_hex(0x1a1530), 0);
    lv_obj_set_style_border_color(s_pair, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_border_width(s_pair, 2, 0);
    lv_obj_remove_flag(s_pair, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_pair, LV_OBJ_FLAG_HIDDEN);
    s_pair_title = make_label(s_pair, font_pick(&lv_font_montserrat_20, FONT_COMPACT), COLOR_LIT);
    lv_label_set_text(s_pair_title, "Pairing code");
    s_pair_code = make_label(s_pair, font_pick(&lv_font_montserrat_28, &lv_font_montserrat_20), COLOR_ACCENT);
    lv_obj_set_style_text_letter_space(s_pair_code, s_small ? 2 : 6, 0);
    s_pair_hint = make_label(s_pair, font_pick(&lv_font_montserrat_14, FONT_COMPACT), COLOR_DIM);
    lv_label_set_text(s_pair_hint, s_small ? "Enter on phone" : "Enter it on your phone");
    /* Wraps: "bottom right button" is wider than the AIPI's card. */
    lv_obj_set_width(s_pair_hint, lv_pct(100));
    lv_label_set_long_mode(s_pair_hint, LV_LABEL_LONG_MODE_WRAP);

    /* Sleep cover: swallows the waking touch. */
    s_cover = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_cover);
    lv_obj_set_size(s_cover, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_cover, lv_color_black(), 0);
    lv_obj_add_flag(s_cover, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_cover, on_cover_event, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_cover, on_cover_event, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_cover, on_cover_event, LV_EVENT_PRESS_LOST, NULL);

    if (s_indev) {
        lv_indev_add_event_cb(s_indev, on_any_press, LV_EVENT_PRESSED, NULL);
    }
}

static void apply_brightness(int pct)
{
    if (pct != s_brightness) {
        muse_board->set_brightness(pct);
        s_brightness = pct;
    }
}

bool muse_ui_dark(void)
{
    return s_dark;
}

/* Returns true while asleep (skip rendering). */
static bool update_sleep(void)
{
    bool asleep = muse_state_asleep();
    if (asleep && !s_dark) {
        muse_menu_close();
        apply_brightness(0);
        if (muse_board->panel_sleep) {
            muse_board->panel_sleep(true);
        }
        lv_obj_set_style_bg_opa(s_cover, LV_OPA_COVER, 0);
        lv_obj_remove_flag(s_cover, LV_OBJ_FLAG_HIDDEN);
        s_dark = true;
    } else if (!asleep && s_dark) {
        s_dark = false;
        if (muse_board->panel_sleep) {
            muse_board->panel_sleep(false);
        }
        /* Woken by a button: drop the cover now. Woken by touch: on release. */
        if (lv_obj_get_style_bg_opa(s_cover, 0) == LV_OPA_COVER) {
            lv_obj_add_flag(s_cover, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (!s_dark) {
        int target = muse_settings_brightness();
        if (s_preview_brightness >= 0) {
            if (s_preview_brightness == target) {
                s_preview_brightness = -1;
            } else {
                target = s_preview_brightness;
            }
        }
        apply_brightness(target);
    }
    return s_dark;
}

/* Idle's label: whether a press reaches Hatch now or waits for Wi-Fi. */
static const char *idle_name(muse_wifi_state_t wifi)
{
    static bool joined;   /* since boot: from then on, a drop is reconnecting */
    switch (wifi) {
    case MUSE_WIFI_CONNECTED:
        joined = true;
        return MODE_NAMES[MUSE_MODE_IDLE];
    case MUSE_WIFI_OFF:
        return "WI-FI OFF";
    case MUSE_WIFI_NO_NETWORK:
        return "SET UP WI-FI";
    case MUSE_WIFI_NOT_NEARBY:
        return "NO WI-FI";   /* none of the saved networks is in range */
    default:
        return joined ? "RECONNECTING" : "CONNECTING";
    }
}

static void update_chrome(float now)
{
    if (now < s_next_settings_tick) {
        return;
    }
    s_next_settings_tick = now + SETTINGS_TICK_S;

    if (s_tv) {
        int page = lv_tileview_get_tile_active(s_tv) == s_settings;
        bool subpage = muse_settings_ui_in_subpage();
        bool swipe = !page || !subpage;
        if (swipe != lv_obj_has_flag(s_tv, LV_OBJ_FLAG_SCROLLABLE)) {
            lv_obj_set_flag(s_tv, LV_OBJ_FLAG_SCROLLABLE, swipe);
        }
        int shown = page * 2 + subpage;
        if (shown != s_shown_page) {
            for (int i = 0; i < 2; i++) {
                lv_obj_set_style_bg_color(s_dots[i], lv_color_hex(i == page ? COLOR_ACCENT : COLOR_DOT_OFF), 0);
                lv_obj_set_flag(s_dots[i], LV_OBJ_FLAG_HIDDEN, page && subpage);
            }
            s_shown_page = shown;
        }
        muse_settings_ui_tick(lv_obj_get_scroll_x(s_tv) > 0);
    }

    /* Joining, the icon blinks: the compact layout has no state label. */
    muse_wifi_status_t w;
    muse_wifi_status(&w);
    bool joining = w.state == MUSE_WIFI_CONNECTING || w.state == MUSE_WIFI_FAILED;
    const char *wifi = w.state == MUSE_WIFI_CONNECTED || (joining && (int)(now * 2) % 2 == 0) ? LV_SYMBOL_WIFI : "";
    if (strcmp(wifi, lv_label_get_text(s_wifi_icon)) != 0) {
        lv_label_set_text(s_wifi_icon, wifi);
    }
    s_idle_name = idle_name(w.state);
    muse_ble_status_t b;
    muse_ble_status(&b);
    const char *ble = b.state != MUSE_BLE_OFF ? LV_SYMBOL_BLUETOOTH : "";
    if (strcmp(ble, lv_label_get_text(s_ble_icon)) != 0) {
        lv_label_set_text(s_ble_icon, ble);
        lv_obj_set_style_text_color(s_ble_icon, lv_color_hex(b.state == MUSE_BLE_CONNECTED ? COLOR_ACCENT : COLOR_DIM), 0);
    }

    /* Paired, the name has done its job (picking this one out in the Muse
     * app) and the speaker button has replies to mute. */
    muse_hatch_status_t h;
    muse_hatch_status(&h);
    bool paired = h.state != MUSE_HATCH_NOT_SET;

    /* The gadget's name, until it's paired. Emptied rather than hidden: the
     * read layout unhides it on the way out. A narrow screen gets the hex tail
     * on its own, which is the part that differs between two of them, rather
     * than a head that ends in dots before it gets there. */
    const lv_font_t *name_font = s_small ? &lv_font_unscii_8 : &lv_font_unscii_16;
    int name_cw = lv_font_get_glyph_width(name_font, 'M', ' ');
    const char *shown = paired ? "" : b.name;
    if (name_cw > 0 && (int)strlen(shown) * name_cw > s_w) {
        const char *tail = strrchr(shown, '-');
        if (tail && tail[1]) {
            shown = tail + 1;
        }
    }
    if (strcmp(shown, lv_label_get_text(s_name_lbl)) != 0) {
        lv_label_set_text(s_name_lbl, shown);
    }

    /* The same card asks for the talk button when the Muse app pairs. */
    bool confirm = !b.passkey && muse_link_state() == MUSE_LINK_CONFIRM;
    if (b.passkey || confirm) {
        char code[24], hint[40];
        if (confirm) {
            strlcpy(code, s_small ? "Press" : "Press button", sizeof(code));
            snprintf(hint, sizeof(hint), s_small ? "%s button" : "Press the %s button", muse_board->talk_button);
        } else {
            snprintf(code, sizeof(code), "%06lu", (unsigned long)b.passkey);
            strlcpy(hint, s_small ? "Enter on phone" : "Enter it on your phone", sizeof(hint));
        }
        const char *title = confirm ? (s_small ? "Muse app" : "Pair with Muse app") : "Pairing code";
        if (strcmp(code, lv_label_get_text(s_pair_code)) != 0) {
            lv_label_set_text(s_pair_code, code);
            lv_label_set_text(s_pair_title, title);
            lv_label_set_text(s_pair_hint, hint);
        }
    }
    lv_obj_set_flag(s_pair, LV_OBJ_FLAG_HIDDEN, !b.passkey && !confirm);

    bool speaker = muse_settings_speaker_on();   /* also set from settings, the phone and serial */
    if (s_speaker && (int)speaker != s_shown_speaker) {
        show_speaker(speaker);
    }
    if (s_speaker && paired == lv_obj_has_flag(s_speaker, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_set_flag(s_speaker, LV_OBJ_FLAG_HIDDEN, !paired);
    }
    /* Unpaired, a press only says "SET UP MUSE FIRST", so the mic goes too.
     * While a reply's layout is up it decides; that's only ever paired. */
    if (s_answer < 0 && paired == lv_obj_has_flag(s_mic_icon, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_set_flag(s_mic_icon, LV_OBJ_FLAG_HIDDEN, !paired);
    }
}

/* One area for the whole meter before its segments change: LVGL won't join
 * squares with gaps between them, and each area it keeps costs a walk of the
 * widget tree. The segments' own areas then fall inside this one. */
static void invalidate_meter(void)
{
    lv_area_t a, last;
    lv_obj_get_coords(s_meter[0], &a);
    lv_obj_get_coords(s_meter[METER_SEGS - 1], &last);
    a.x2 = last.x2;
    lv_obj_invalidate_area(lv_obj_get_parent(s_meter[0]), &a);
}

static void set_meter_visible(bool visible)
{
    if (s_small || visible == s_meter_visible) {
        return;
    }
    invalidate_meter();
    for (int i = 0; i < METER_SEGS; i++) {
        if (visible) {
            lv_obj_remove_flag(s_meter[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_meter[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    s_meter_visible = visible;
    s_shown_lit = -1;
}

static void update_power(float now)
{
    if (now < s_next_power_update) {
        return;
    }
    s_next_power_update = now + 1.0f;

    muse_power_t p = muse_state_power();
    char buf[32];
    if (p.battery_pct < 0) {
        strlcpy(buf, p.usb ? (s_small ? "USB" : "USB POWER") : "", sizeof(buf));
    } else if (s_small) {
        snprintf(buf, sizeof(buf), "%s%d%%", p.charging ? "+" : "", p.battery_pct);
    } else if (p.charging) {
        snprintf(buf, sizeof(buf), "CHARGING %d%%", p.battery_pct);
    } else {
        snprintf(buf, sizeof(buf), "BATTERY %d%%", p.battery_pct);
    }
    if (strcmp(buf, lv_label_get_text(s_power_lbl)) != 0) {
        lv_label_set_text(s_power_lbl, buf);
    }
}

static void update_status(muse_mode_t mode, float now)
{
    uint32_t accent = muse_pixel_accent(mode);
    const char *name = mode == MUSE_MODE_IDLE ? s_idle_name : MODE_NAMES[mode];

    if (name != s_shown_name) {
        lv_label_set_text(s_state_lbl, name);
        s_shown_name = name;
    }
    if ((int)mode != s_shown_state) {
        lv_obj_set_style_text_color(s_state_lbl, lv_color_hex(accent), 0);
        if (s_ring) {
            lv_obj_set_style_arc_color(s_ring, lv_color_hex(accent), LV_PART_INDICATOR);
        } else {
            lv_obj_set_style_bg_color(s_bar, lv_color_hex(accent), 0);
        }
        set_mic_color(mode == MUSE_MODE_LISTENING ? accent : COLOR_DIM);   /* lights up while recording */
        s_shown_state = (int)mode;
        s_shown_lit = -1;
    }

    /* Ring: phase progress while listening/speaking, a spinner while thinking. */
    int ring = 0;
    if (mode == MUSE_MODE_LISTENING || mode == MUSE_MODE_SPEAKING) {
        ring = (int)(muse_state_progress() * RING_RANGE);
    } else if (mode == MUSE_MODE_THINKING) {
        ring = RING_RANGE / 6;
    }
    if (s_ring && mode == MUSE_MODE_THINKING) {
        /* Move the indicator rather than rotate the arc: a rotation redraws
         * the whole ring, and with it the whole screen. */
        int start = (int)(now * 300.0f) % 360;
        lv_arc_set_angles(s_ring, start, start + ring * 360 / RING_RANGE);
        s_ring_value = -1;
    } else if (s_ring) {
        if (s_ring_value < 0) {
            lv_arc_set_bg_start_angle(s_ring, 0);   /* unchanged, but puts the value's angles back */
        }
        if (ring != s_ring_value) {
            lv_arc_set_value(s_ring, ring);
            s_ring_value = ring;
        }
    } else {
        /* Bar along the bottom edge; a sliding segment while thinking. */
        int x = mode == MUSE_MODE_THINKING ? (int)(now * s_w) % s_w : 0;
        int w = ring * s_w / RING_RANGE;
        if (ring != s_ring_value || x) {
            lv_obj_set_width(s_bar, w);
            lv_obj_align(s_bar, LV_ALIGN_BOTTOM_LEFT, x, 0);
            s_ring_value = ring;
        }
    }

    /* The mic's meter; a reply's page has the room while answering. */
    bool meter = mode == MUSE_MODE_LISTENING;
    set_meter_visible(meter);
    if (meter && !s_small) {
        int lit = (int)lroundf(s_level * METER_SEGS);
        if (lit != s_shown_lit || accent != s_shown_accent) {
            invalidate_meter();
            for (int i = 0; i < METER_SEGS; i++) {
                /* Outer segments light last, like a centred VU. */
                int rank = abs(2 * i - (METER_SEGS - 1)) / 2;
                bool on = rank < (lit + 1) / 2;
                lv_obj_set_style_bg_color(s_meter[i], lv_color_hex(on ? accent : COLOR_METER_OFF), 0);
            }
            s_shown_lit = lit;
            s_shown_accent = accent;
        }
    }

    static char caption[MUSE_CAPTION_MAX];
    bool fresh = muse_state_caption(caption, sizeof(caption), &s_caption_version);
    int answer = -1;
    if (s_reply_lbl) {
        /* The speaker picks the layout, even mid-reply: the voice task pages to fit. */
        int layout = muse_settings_speaker_on() ? ANSWER_HEARD : ANSWER_READ;
        if (layout != s_page_for) {
            muse_state_set_page(s_answers[layout].cols, s_answers[layout].lines);
            s_page_for = layout;
        }
        bool answering = mode == MUSE_MODE_THINKING || mode == MUSE_MODE_SPEAKING;
        bool have_text = s_transcript && s_transcript[0];
#if CONFIG_MUSE_PET
        if (s_toy_frame) {
            answering = false;   /* the toy: words go to the bubble, never the text box */
        }
#endif
        if (answering) {
            answer = layout;
            s_hold_until = now + TRANSCRIPT_HOLD_S;
        } else if (s_reply_box && s_answer >= 0 && mode == MUSE_MODE_IDLE && now < s_hold_until && have_text &&
                   !(fresh && caption[0] && !muse_voice_stream_active())) {
            /* The transcript stays to be read, until a press, or a caption with news (an
             * error; not the Pi's "now saying" line, which comes with its speech). */
            answer = s_answer;
        }
    }
    bool opened = answer >= 0 && s_answer < 0;
    if (answer != s_answer) {
        if (answer < 0 && s_reply_box) {
            /* Read and done: the next clip or bubble starts from nothing. */
            muse_state_set_heard("");
            muse_state_set_transcript("");
        }
        set_answer(answer);
        fresh = true;   /* the caption moves between labels */
    }
    if (s_reply_box && answer >= 0) {
        update_transcript(caption, fresh, opened, now);
    } else if (fresh) {
        lv_obj_t *lbl = answer >= 0 ? s_reply_lbl : s_caption_lbl;
        lv_label_set_text(lbl, caption);
        lv_obj_set_flag(lbl, LV_OBJ_FLAG_HIDDEN, !caption[0]);
        if (s_reply_lbl) {
            lv_obj_add_flag(answer >= 0 ? s_caption_lbl : reply_area(), LV_OBJ_FLAG_HIDDEN);
        }
    }
#if CONFIG_MUSE_PET
    if (s_toy_frame) {
        update_toy(now, caption, fresh);
    }
#endif
    update_power(now);
}

static volatile bool s_snapshot;

/* Streams the screen over the USB cable as base64 RGB565 (bench testing; needs
 * LV_USE_SNAPSHOT, which devices/sdkconfig.muse-bench turns on). */
static void send_snapshot(void)
{
#if LV_USE_SNAPSHOT
    lv_draw_buf_t *buf = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
    if (!buf) {
        return;
    }
    enum { RAW = 144 };   /* lines must fit the driver's 256-byte TX ring */
    char hdr[48];
    int n = snprintf(hdr, sizeof(hdr), "\nSNAP BEGIN %d %d %d\n", (int)buf->header.w, (int)buf->header.h, RAW);
    muse_console_write(hdr, n);
    static unsigned char b64[4 * RAW / 3 + 4];
    for (uint32_t y = 0; y < buf->header.h; y++) {
        const unsigned char *p = buf->data + y * buf->header.stride;
        size_t left = buf->header.w * 2;
        while (left) {
            size_t chunk = left > RAW ? RAW : left, olen;
            mbedtls_base64_encode(b64, sizeof(b64), &olen, p, chunk);
            b64[olen++] = '\n';
            muse_console_write(b64, olen);
            p += chunk;
            left -= chunk;
        }
    }
    muse_console_write("SNAP END\n", 9);
    lv_draw_buf_destroy(buf);
#else
    muse_console_write("\nSNAP OFF\n", 10);   /* so snap.py can say why */
#endif
}

void muse_ui_request_snapshot(void)
{
    s_snapshot = true;
}

static void frame_tick(lv_timer_t *timer)
{
    if (s_snapshot) {
        s_snapshot = false;
        send_snapshot();
    }
    (void)timer;
    image_sync();
    float mode_t;
    muse_mode_t mode = muse_state_mode(&mode_t);
    float now = (float)esp_timer_get_time() / 1e6f;

    if (mode != s_last_mode) {
        if (mode == MUSE_MODE_LISTENING) {
            image_hide_locked();
            muse_ui_show_face();
        }
        s_last_mode = mode;
    }
    if (update_sleep()) {
        return;
    }
    update_chrome(now);
    if (muse_menu_tick(now)) {
        image_hide_locked();
        return;   /* the menu covers the face */
    }
    if (s_image_dsc.data) {
        return;   /* the image covers the face */
    }
    if (s_tv && lv_obj_get_scroll_x(s_tv) != 0) {
        /* Off screen, or sliding to or from settings: hold still so the
         * slide gets the whole frame time. */
        return;
    }

    /* Fast attack, slow release keeps the mouth and meter lively but readable. */
    float level = muse_state_level();
    s_level += (level - s_level) * (level > s_level ? 0.6f : 0.2f);

    muse_pose_t pose = {
        .mode = mode,
        .t = now,
        .mode_t = mode_t,
        .level = s_level,
        .happy = muse_state_happiness(),
    };
    muse_pixel_render(&pose);
    invalidate_muse();

    update_status(mode, now);
}

esp_err_t muse_ui_start(void)
{
    s_w = muse_board->width;
    s_h = muse_board->height;
    /* The full layout assumes room for the 466 px board's header and bottom
     * captions. Short landscape panels (BOX-3) need the compact layout too. */
    bool short_landscape = s_w > s_h && s_h < 320;
    /* A narrow portrait panel (240x320) is too short for the full layout as well: its status
     * rows would sit at 20 + s_dy, above the top edge. */
    bool narrow_portrait = s_w < s_h && s_w < 340;   /* 240x320 and 320x480 alike */
    s_small = s_h < 200 || s_w < 200 || short_landscape || narrow_portrait;
    s_tall = s_small && s_h >= s_w + 64;
    /* Small screens keep room for the status line and button icons. A narrow
     * one is as wide as Muse gets, in whole pixels. */
    s_canvas_px = s_small ? s_h * 3 / 4 : MUSE_PX_W * 5;
    if (short_landscape) {
        /* Leave the header's first 40 rows and bottom captions clear. */
        s_canvas_px = s_h * 2 / 3;
    }
    if (s_canvas_px > s_w) {
        s_canvas_px = s_w / MUSE_PX_W * MUSE_PX_W;
    }
    if (muse_board->avatar_px > 0 && muse_board->avatar_px < s_canvas_px) {
        /* The board asked for a smaller Muse than the screen allows. */
        s_canvas_px = LV_MAX(MUSE_PX_W, muse_board->avatar_px / MUSE_PX_W * MUSE_PX_W);
    }
    s_dy = (s_h - 466) / 2;
    if (!s_small && muse_board->round && s_dy < 0) {
        /* A shorter screen moves the labels out towards its edges, but a
         * smaller circle's edges curve away from them. Keep them where they
         * are and shrink Muse to whole pixels that fit between them. */
        s_canvas_px = (MUSE_PX_W * 5 + 2 * s_dy) / MUSE_PX_W * MUSE_PX_W;
        s_dy = 0;
    }

    lv_display_t *disp = muse_board->display_start(&s_indev);
    if (!disp) {
        ESP_LOGE(TAG, "display init failed");
        return ESP_FAIL;
    }

    s_image_mutex = xSemaphoreCreateMutex();
    muse_board->display_lock(-1);
    build_screen();
    if (s_settings) {
        muse_settings_ui_build(s_settings);
    } else {
        muse_menu_build(lv_screen_active(), s_w, s_h);
    }
    build_overlays();
    lv_timer_create(frame_tick, muse_board->frame_ms, NULL);
    s_ready = true;
    muse_board->display_unlock();

    ESP_LOGI(TAG, "UI up: %dx%d, %d px Muse, %d ms frames", s_w, s_h, s_canvas_px, muse_board->frame_ms);
    return ESP_OK;
}

void muse_ui_show_face(void)
{
    if (!s_tv) {
        return;
    }
    lv_obj_add_flag(s_tv, LV_OBJ_FLAG_SCROLLABLE);
    lv_tileview_set_tile(s_tv, s_face, LV_ANIM_ON);
}

void muse_ui_set_swipe_enabled(bool enabled)
{
    if (!s_tv) {
        return;
    }
    lv_obj_set_flag(s_tv, LV_OBJ_FLAG_SCROLLABLE, enabled);
    s_shown_page = -1;
    s_next_settings_tick = 0;
}

void muse_ui_preview_brightness(int pct)
{
    s_preview_brightness = pct;
    apply_brightness(pct);
}

bool muse_ui_image_size(int *w, int *h)
{
    if (!s_ready || !heap_caps_get_total_size(MALLOC_CAP_SPIRAM)) {
        return false;
    }
    *w = s_w;
    *h = s_h;
    return true;
}

bool muse_ui_image_draw(int x, int y, int w, int h, const uint16_t *pixels)
{
    if (!s_ready || x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > s_w || y + h > s_h) {
        return false;
    }
    xSemaphoreTake(s_image_mutex, portMAX_DELAY);
    if (!s_image_buf) {
        /* The first rectangle of an image: a black screen to draw onto. */
        s_image_buf = heap_caps_calloc((size_t)s_w * s_h, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        if (!s_image_buf) {
            xSemaphoreGive(s_image_mutex);
            return false;
        }
        /* Now, not when it's shown: asleep, the display may be paused (muse_input.c). */
        muse_state_set_asleep(false);
    }
    const uint8_t *src = (const uint8_t *)pixels;
    for (int row = 0; row < h; row++) {
        uint16_t *dst = s_image_buf + (size_t)(y + row) * s_w + x;
        for (int i = 0; i < w; i++, src += 2) {
            dst[i] = (uint16_t)(src[0] << 8 | src[1]);
        }
    }
    lv_area_t *a = &s_image_area;
    if (!s_image_dirty) {
        *a = (lv_area_t){ x, y, x + w - 1, y + h - 1 };
        s_image_dirty = true;
    } else {
        a->x1 = LV_MIN(a->x1, x);
        a->y1 = LV_MIN(a->y1, y);
        a->x2 = LV_MAX(a->x2, x + w - 1);
        a->y2 = LV_MAX(a->y2, y + h - 1);
    }
    xSemaphoreGive(s_image_mutex);
    return true;
}

bool muse_ui_image_visible(void)
{
    return s_ready && s_image_buf != NULL;
}

void muse_ui_image_hide(void)
{
    if (!s_ready) {
        return;
    }
    muse_board->display_lock(-1);
    image_hide_locked();
#if CONFIG_MUSE_WATCHER_CAMERA
    if (s_camera_hint) lv_obj_add_flag(s_camera_hint, LV_OBJ_FLAG_HIDDEN);
#endif
    muse_board->display_unlock();
}

#if CONFIG_MUSE_WATCHER_CAMERA
void muse_ui_camera_hint(bool visible)
{
    if (!s_ready || !s_camera_hint) return;
    muse_board->display_lock(-1);
    if (visible) lv_obj_remove_flag(s_camera_hint, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_camera_hint, LV_OBJ_FLAG_HIDDEN);
    muse_board->display_unlock();
}
#endif
