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
#include "muse_prompt.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "lvgl.h"
#include "sdkconfig.h"

#include "muse_board.h"
#include "muse_state.h"

static const char *TAG = "muse_prompt";

#define TITLE_MAX 48
#define ITEM_MAX 40

#if CONFIG_MUSE_SKIN_WINAMP
#define BG 0x000000
#define FRAME 0x686c68
#define TITLE_COLOR 0xa8a8a8
#define BTN 0x383c38
#define BTN_PRESSED 0x0000c6
#define BTN_TEXT 0x00fc00
#define TITLE_FONT (&lv_font_unscii_8)
#define ITEM_FONT (&lv_font_unscii_16)
#define RADIUS 0
#else
#define BG 0x1a1530
#define FRAME 0xa77dff
#define TITLE_COLOR 0x8b84a8
#define BTN 0x2e2552
#define BTN_PRESSED 0xa77dff
#define BTN_TEXT 0xf2efff
#define TITLE_FONT (&lv_font_montserrat_14)
#define ITEM_FONT (&lv_font_montserrat_20)
#define RADIUS 10
#endif

static struct {
    bool active;
    lv_obj_t *box;
    muse_prompt_cb_t cb;
    void *user;
    esp_timer_handle_t timer;
    char title[TITLE_MAX];
    char items[MUSE_PROMPT_MAX_ITEMS][ITEM_MAX];
    int n;
} s;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

/* Ends the prompt once; true for the caller that got there first. */
static bool take(void)
{
    portENTER_CRITICAL(&s_lock);
    bool was = s.active;
    s.active = false;
    portEXIT_CRITICAL(&s_lock);
    return was;
}

/* From the UI task (a tap): LVGL is ours already. */
static void on_item(lv_event_t *e)
{
    int choice = (int)(intptr_t)lv_event_get_user_data(e);
    if (!take()) {
        return;
    }
    esp_timer_stop(s.timer);
    lv_obj_delete_async(s.box);
    s.box = NULL;
    ESP_LOGI(TAG, "chose %d: %s", choice, s.items[choice]);
    muse_state_poke();
    if (s.cb) {
        s.cb(choice, s.user);
    }
}

/* From another task (timeout, dismiss): take the display lock. */
static void end_from_outside(int choice)
{
    if (!take()) {
        return;
    }
    esp_timer_stop(s.timer);
    muse_board->display_lock(-1);
    if (s.box) {
        lv_obj_delete(s.box);
        s.box = NULL;
    }
    muse_board->display_unlock();
    ESP_LOGI(TAG, "ended: %d", choice);
    if (s.cb) {
        s.cb(choice, s.user);
    }
}

static void on_timeout(void *arg)
{
    end_from_outside(-1);
}

bool muse_prompt_show(const char *title, const char *const *items, int n, int timeout_ms,
                      muse_prompt_cb_t cb, void *user)
{
    if (!muse_board || !muse_board->display_lock || n < 1 || n > MUSE_PROMPT_MAX_ITEMS) {
        return false;
    }
    portENTER_CRITICAL(&s_lock);
    bool busy = s.active;
    if (!busy) {
        s.active = true;
    }
    portEXIT_CRITICAL(&s_lock);
    if (busy) {
        return false;
    }
    if (!s.timer) {
        const esp_timer_create_args_t args = { .callback = on_timeout, .name = "prompt" };
        esp_timer_create(&args, &s.timer);
    }
    s.cb = cb;
    s.user = user;
    s.n = n;
    strlcpy(s.title, title && title[0] ? title : "CHOOSE", sizeof(s.title));
    for (int i = 0; i < n; i++) {
        strlcpy(s.items[i], items[i] ? items[i] : "", sizeof(s.items[i]));
    }

    muse_state_set_asleep(false);
    const int w = muse_board->width, h = muse_board->height;
    muse_board->display_lock(-1);
    lv_obj_t *box = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, w - 8, h - 8);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, lv_color_hex(BG), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(FRAME), 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_radius(box, RADIUS, 0);
    lv_obj_set_style_pad_all(box, 6, 0);
    lv_obj_set_style_pad_row(box, 6, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);   /* taps outside a button stay here */

    lv_obj_t *title_lbl = lv_label_create(box);
    lv_obj_set_style_text_font(title_lbl, TITLE_FONT, 0);
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(TITLE_COLOR), 0);
    lv_obj_set_width(title_lbl, lv_pct(100));
    lv_obj_set_style_text_align(title_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(title_lbl, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(title_lbl, s.title);

    lv_obj_t *list = lv_obj_create(box);
    lv_obj_remove_style_all(list);
    lv_obj_set_width(list, lv_pct(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 6, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    /* Buttons tall enough for a thumb; they share the height when few. */
    int avail = h - 8 - 12 - 24 - 6;
    int bh = avail / n - 6;
    bh = bh > 56 ? 56 : bh < 40 ? 40 : bh;
    for (int i = 0; i < n; i++) {
        lv_obj_t *btn = lv_button_create(list);
        lv_obj_remove_style_all(btn);
        lv_obj_set_size(btn, lv_pct(100), bh);
        lv_obj_set_style_bg_color(btn, lv_color_hex(BTN), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(BTN_PRESSED), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(btn, RADIUS, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(FRAME), 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(btn, on_item, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_obj_set_style_text_font(lbl, ITEM_FONT, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(BTN_TEXT), 0);
        lv_obj_set_width(lbl, lv_pct(96));
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_MODE_DOTS);
        lv_label_set_text(lbl, s.items[i]);
        lv_obj_center(lbl);
    }
    s.box = box;
    muse_board->display_unlock();

    if (timeout_ms > 0) {
        esp_timer_start_once(s.timer, (uint64_t)timeout_ms * 1000);
    }
    ESP_LOGI(TAG, "menu '%s', %d items, %d ms", s.title, n, timeout_ms);
    return true;
}

bool muse_prompt_active(void)
{
    return s.active;
}

void muse_prompt_dismiss(void)
{
    end_from_outside(-1);
}
