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
#include "gadget_more.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include <math.h>
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "ping/ping_sock.h"

#include "gadget_canvas.h"
#include "gadget_tools.h"
#include "muse_audio.h"
#include "muse_chat.h"
#include "muse_state.h"
#include "muse_ui.h"
#include "muse_voice.h"

#if CONFIG_HOMEHUB_DISPLAY_COMMANDS
#include "led_status.h"
#endif

static const char *TAG = "gadget_more";

static void marquee_stop(void);

#define SD_BASE "/sd"
#define WATCH_MAX_S 3600
#define DRAW_MAX_ITEMS 80

/* ---- results and params (same shapes as gadget_tools.c) ---------------- */

static cJSON *ok_with(cJSON *payload)
{
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddItemToObject(result, "payload", payload ? payload : cJSON_CreateObject());
    return result;
}

static cJSON *fail(const char *code, const char *message)
{
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", false);
    cJSON *error = cJSON_CreateObject();
    cJSON_AddStringToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message);
    cJSON_AddItemToObject(result, "error", error);
    return result;
}

static cJSON *async_result(void)
{
    cJSON *async = cJSON_CreateObject();
    cJSON_AddBoolToObject(async, "_async", true);
    return async;
}

static const char *str_param(cJSON *params, const char *key)
{
    cJSON *item = params ? cJSON_GetObjectItem(params, key) : NULL;
    return cJSON_IsString(item) && item->valuestring && item->valuestring[0] ? item->valuestring : NULL;
}

static bool int_param(cJSON *params, const char *key, int *out)
{
    cJSON *item = params ? cJSON_GetObjectItem(params, key) : NULL;
    if (cJSON_IsNumber(item)) {
        *out = (int)item->valuedouble;
        return true;
    }
    if (cJSON_IsString(item) && item->valuestring) {
        char *end;
        long v = strtol(item->valuestring, &end, 10);
        if (end != item->valuestring && !*end) {
            *out = (int)v;
            return true;
        }
    }
    return false;
}

static bool float_param(cJSON *params, const char *key, float *out)
{
    cJSON *item = params ? cJSON_GetObjectItem(params, key) : NULL;
    if (cJSON_IsNumber(item)) {
        *out = (float)item->valuedouble;
        return true;
    }
    return false;
}

static bool bool_param(cJSON *params, const char *key, bool *out)
{
    cJSON *item = params ? cJSON_GetObjectItem(params, key) : NULL;
    if (cJSON_IsBool(item)) {
        *out = cJSON_IsTrue(item);
        return true;
    }
    if (cJSON_IsString(item) && item->valuestring) {
        if (!strcasecmp(item->valuestring, "true") || !strcasecmp(item->valuestring, "yes")) {
            *out = true;
            return true;
        }
        if (!strcasecmp(item->valuestring, "false") || !strcasecmp(item->valuestring, "no")) {
            *out = false;
            return true;
        }
    }
    return false;
}

static uint32_t color_param(cJSON *params, const char *key, uint32_t fallback)
{
    const char *s = str_param(params, key);
    uint32_t rgb;
    return s && canvas_parse_color(s, &rgb) ? rgb : fallback;
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static cJSON *param_spec(const char *type, const char *description)
{
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "type", type);
    cJSON_AddStringToObject(p, "description", description);
    return p;
}

static void add_command(cJSON *commands, const char *name, const char *description,
                        cJSON *required, cJSON *optional, int timeout_ms)
{
    cJSON *command = cJSON_CreateObject();
    cJSON_AddStringToObject(command, "description", description);
    cJSON_AddItemToObject(command, "required", required ? required : cJSON_CreateObject());
    cJSON_AddItemToObject(command, "optional", optional ? optional : cJSON_CreateObject());
    if (timeout_ms > 0) {
        cJSON_AddNumberToObject(command, "timeout_ms", timeout_ms);
    }
    cJSON_AddItemToObject(commands, name, command);
}

typedef struct {
    noise_ctrl_session_generation_t gen;
    char request_id[64];
} pending_t;

static void pending_init(pending_t *p, const char *request_id, noise_ctrl_session_generation_t gen)
{
    p->gen = gen;
    strlcpy(p->request_id, request_id, sizeof(p->request_id));
}

/* ---- pictures on the canvas --------------------------------------------- */

static int64_t s_pic_until_us;      /* a picture that comes down by itself; 0: stays */
static bool s_pic_up;
static bool s_clock_on;
static int64_t s_clock_until_us;
static int s_clock_minute = -1;
static uint32_t s_clock_color = 0x00fc00;
static bool s_clock_date = true;

static bool screen_size(int *w, int *h)
{
#if CONFIG_HOMEHUB_DISPLAY_COMMANDS
    return led_status_display_info(w, h);
#else
    return false;
#endif
}

static void picture_down(void)
{
    marquee_stop();
#if CONFIG_HOMEHUB_DISPLAY_COMMANDS
    if (muse_ui_image_visible()) {
        led_status_show_animation();
    }
#endif
    s_pic_up = false;
    s_pic_until_us = 0;
    s_clock_on = false;
}

void gadget_more_screen_clear(void)
{
    marquee_stop();
    s_pic_up = false;
    s_pic_until_us = 0;
    s_clock_on = false;
}

/* Word-wraps text into lines that fit maxw at `scale`; -1 when a word is too wide or there are too many lines. */
#define WRAP_LINE_MAX 64
#define WRAP_LINES_MAX 12

static int wrap_lines(const char *text, int scale, int maxw, char lines[WRAP_LINES_MAX][WRAP_LINE_MAX])
{
    int n = 0;
    lines[0][0] = '\0';
    const char *p = text;
    while (*p) {
        if (*p == '\n') {
            if (++n >= WRAP_LINES_MAX) {
                return -1;
            }
            lines[n][0] = '\0';
            p++;
            continue;
        }
        if (*p == ' ') {
            p++;
            continue;
        }
        const char *e = p;
        while (*e && *e != ' ' && *e != '\n') {
            e++;
        }
        char word[WRAP_LINE_MAX];
        size_t wl = (size_t)(e - p) < sizeof(word) - 1 ? (size_t)(e - p) : sizeof(word) - 1;
        memcpy(word, p, wl);
        word[wl] = '\0';
        if (canvas_text_width(word, scale) > maxw) {
            return -1;
        }
        char trial[2 * WRAP_LINE_MAX + 2];
        if (lines[n][0]) {
            snprintf(trial, sizeof(trial), "%s %s", lines[n], word);
        } else {
            strlcpy(trial, word, sizeof(trial));
        }
        if (canvas_text_width(trial, scale) <= maxw && strlen(trial) < WRAP_LINE_MAX) {
            strlcpy(lines[n], trial, WRAP_LINE_MAX);
        } else {
            if (++n >= WRAP_LINES_MAX) {
                return -1;
            }
            strlcpy(lines[n], word, WRAP_LINE_MAX);
        }
        p = e;
    }
    return n + 1;
}

/* The biggest text that fits the box, centred. Returns the scale used. */
static int draw_big_text(canvas_t *c, const char *text, uint32_t rgb, int x, int y, int w, int h, int max_scale)
{
    static char lines[WRAP_LINES_MAX][WRAP_LINE_MAX];
    for (int scale = max_scale; scale >= 1; scale--) {
        int n = wrap_lines(text, scale, w, lines);
        if (n < 0) {
            continue;
        }
        int lh = CANVAS_CELL_H * scale;
        int total = n * lh - scale;
        if (total > h) {
            continue;
        }
        int ty = y + (h - total) / 2;
        for (int i = 0; i < n; i++) {
            int tw = canvas_text_width(lines[i], scale);
            canvas_text(c, x + (w - tw) / 2, ty + i * lh, lines[i], scale, rgb);
        }
        return scale;
    }
    canvas_text(c, x, y, text, 1, rgb);
    return 1;
}

static cJSON *cmd_big_text(cJSON *params)
{
    const char *text = str_param(params, "text");
    if (!text) {
        return fail("invalid_params", "text is required");
    }
    int w, h;
    if (!screen_size(&w, &h)) {
        return fail("unavailable", "no screen for pictures");
    }
    uint32_t fg = color_param(params, "color", 0xffffff);
    uint32_t bg = color_param(params, "background", 0x000000);
    int seconds = 20;
    int_param(params, "seconds", &seconds);
    seconds = clampi(seconds, 0, 24 * 3600);
    canvas_t *c = canvas_create(w, h, bg);
    if (!c) {
        return fail("out_of_memory", "no room for the picture");
    }
    int scale = draw_big_text(c, text, fg, 8, 8, w - 16, h - 16, 12);
    muse_state_set_asleep(false);
    bool ok = canvas_show(c);
    canvas_free(c);
    if (!ok) {
        return fail("draw_failed", "the screen refused the picture");
    }
    s_clock_on = false;
    s_pic_up = true;
    s_pic_until_us = seconds ? esp_timer_get_time() + (int64_t)seconds * 1000000 : 0;
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddNumberToObject(payload, "scale", scale);
    cJSON_AddNumberToObject(payload, "seconds", seconds);
    return ok_with(payload);
}

static void render_clock(void)
{
    int w, h;
    if (!screen_size(&w, &h)) {
        return;
    }
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    canvas_t *c = canvas_create(w, h, 0x000000);
    if (!c) {
        return;
    }
    char hm[8], date[32], wd[16];
    strftime(hm, sizeof(hm), "%H:%M", &tm);
    strftime(date, sizeof(date), "%b %d", &tm);
    strftime(wd, sizeof(wd), "%A", &tm);
    int scale = (w - 16) / (5 * CANVAS_CELL_W - 1);
    scale = clampi(scale, 1, 12);
    int th = CANVAS_GLYPH_H * scale;
    int ty = h / 2 - th / 2 - (s_clock_date ? 24 : 0);
    canvas_text(c, (w - canvas_text_width(hm, scale)) / 2, ty, hm, scale, s_clock_color);
    if (s_clock_date) {
        int ds = clampi((w - 32) / (canvas_text_width(wd, 1) > canvas_text_width(date, 1) ? canvas_text_width(wd, 1) : canvas_text_width(date, 1)), 1, 4);
        canvas_fill_rect(c, 24, ty + th + 14, w - 48, 2, 0x404040);
        canvas_text(c, (w - canvas_text_width(wd, ds)) / 2, ty + th + 26, wd, ds, 0xa8a8a8);
        canvas_text(c, (w - canvas_text_width(date, ds)) / 2, ty + th + 26 + CANVAS_CELL_H * ds + 4, date, ds, 0xa8a8a8);
    }
    canvas_show(c);
    canvas_free(c);
    s_clock_minute = tm.tm_min;
}

static cJSON *cmd_clock(cJSON *params)
{
    bool on = true;
    bool_param(params, "on", &on);
    if (!on) {
        picture_down();
        return ok_with(NULL);
    }
    if (time(NULL) < 1600000000LL) {
        return fail("no_clock", "the clock isn't synced yet");
    }
    int w, h;
    if (!screen_size(&w, &h)) {
        return fail("unavailable", "no screen for pictures");
    }
    int seconds = 0;
    int_param(params, "seconds", &seconds);
    s_clock_color = color_param(params, "color", 0x00fc00);
    s_clock_date = true;
    bool_param(params, "date", &s_clock_date);
    muse_state_set_asleep(false);
    render_clock();
    s_clock_on = true;
    s_pic_up = true;
    s_pic_until_us = 0;
    s_clock_until_us = seconds > 0 ? esp_timer_get_time() + (int64_t)seconds * 1000000 : 0;
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddBoolToObject(payload, "clock", true);
    cJSON_AddNumberToObject(payload, "seconds", seconds);
    cJSON_AddStringToObject(payload, "note", "a tap on the screen or screen.clear brings the avatar back");
    return ok_with(payload);
}

static cJSON *cmd_draw(cJSON *params)
{
    cJSON *items = params ? cJSON_GetObjectItem(params, "items") : NULL;
    if (!cJSON_IsArray(items)) {
        return fail("invalid_params", "items: an array of shapes");
    }
    int w, h;
    if (!screen_size(&w, &h)) {
        return fail("unavailable", "no screen for pictures");
    }
    canvas_t *c = canvas_create(w, h, color_param(params, "background", 0x000000));
    if (!c) {
        return fail("out_of_memory", "no room for the picture");
    }
    int n = 0, skipped = 0;
    cJSON *it;
    cJSON_ArrayForEach(it, items) {
        if (++n > DRAW_MAX_ITEMS) {
            break;
        }
        const char *type = str_param(it, "type");
        int x = 0, y = 0, ww = 0, hh = 0, x2 = 0, y2 = 0, r = 0, t = 2, scale = 2, value = 0;
        int_param(it, "x", &x);
        int_param(it, "y", &y);
        int_param(it, "w", &ww);
        int_param(it, "h", &hh);
        int_param(it, "x2", &x2);
        int_param(it, "y2", &y2);
        int_param(it, "r", &r);
        int_param(it, "thickness", &t);
        int_param(it, "scale", &scale);
        int_param(it, "value", &value);
        uint32_t rgb = color_param(it, "color", 0xffffff);
        bool fill = true;
        bool_param(it, "fill", &fill);
        if (!type) {
            skipped++;
        } else if (!strcasecmp(type, "rect")) {
            if (fill) canvas_fill_rect(c, x, y, ww, hh, rgb);
            else canvas_rect(c, x, y, ww, hh, rgb, t);
        } else if (!strcasecmp(type, "circle")) {
            canvas_circle(c, x, y, r, rgb, fill, t);
        } else if (!strcasecmp(type, "line")) {
            canvas_line(c, x, y, x2, y2, rgb, t);
        } else if (!strcasecmp(type, "text")) {
            const char *text = str_param(it, "text");
            if (text) {
                scale = clampi(scale, 1, 12);
                const char *align = str_param(it, "align");
                int tw = canvas_text_width(text, scale);
                int tx = x;
                if (align && !strcasecmp(align, "center")) tx = x - tw / 2;
                else if (align && !strcasecmp(align, "right")) tx = x - tw;
                canvas_text(c, tx, y, text, scale, rgb);
            } else {
                skipped++;
            }
        } else if (!strcasecmp(type, "bar")) {
            uint32_t bg = color_param(it, "background", 0x303030);
            canvas_fill_rect(c, x, y, ww, hh, bg);
            canvas_fill_rect(c, x, y, ww * clampi(value, 0, 100) / 100, hh, rgb);
            canvas_rect(c, x, y, ww, hh, 0x808080, 1);
        } else {
            skipped++;
        }
    }
    int seconds = 0;
    int_param(params, "seconds", &seconds);
    seconds = clampi(seconds, 0, 24 * 3600);
    muse_state_set_asleep(false);
    bool ok = canvas_show(c);
    canvas_free(c);
    if (!ok) {
        return fail("draw_failed", "the screen refused the picture");
    }
    s_clock_on = false;
    s_pic_up = true;
    s_pic_until_us = seconds ? esp_timer_get_time() + (int64_t)seconds * 1000000 : 0;
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddNumberToObject(payload, "items", n > DRAW_MAX_ITEMS ? DRAW_MAX_ITEMS : n);
    cJSON_AddNumberToObject(payload, "skipped", skipped);
    cJSON_AddNumberToObject(payload, "width", w);
    cJSON_AddNumberToObject(payload, "height", h);
    return ok_with(payload);
}

/* ---- watching for something to happen ---------------------------------- */

typedef struct {
    bool noise;
    float threshold;
    int pin, level;
    char message[200];
    int seconds, cooldown_s;
    bool once;
    volatile bool cancel;
    int fired;
} watch_t;

static watch_t *volatile s_watch;

static void watch_say(const watch_t *w)
{
    if (!muse_hatch_ready()) {
        ESP_LOGW(TAG, "watch fired but Muse isn't reachable");
        return;
    }
    char *msg = strdup(w->message);
    if (msg) {
        muse_hatch_text_turn(msg);   /* frees it; the reply goes to the console */
    }
}

static void watch_task(void *arg)
{
    watch_t *w = arg;
    int64_t t0 = esp_timer_get_time();
    int last = -1;
    if (w->noise) {
        muse_voice_set_monitor(true);
        vTaskDelay(pdMS_TO_TICKS(500));
        muse_voice_set_monitor(true);   /* a fresh peak after the settle */
    } else {
        gpio_reset_pin(w->pin);
        gpio_set_direction(w->pin, GPIO_MODE_INPUT);
        gpio_set_pull_mode(w->pin, GPIO_PULLUP_ONLY);
        last = gpio_get_level(w->pin);
    }
    while (!w->cancel && esp_timer_get_time() - t0 < (int64_t)w->seconds * 1000000) {
        vTaskDelay(pdMS_TO_TICKS(100));
        bool hit = false;
        if (w->noise) {
            hit = muse_voice_monitor_db() >= w->threshold;
        } else {
            int lvl = gpio_get_level(w->pin);
            hit = lvl == w->level && last != lvl;
            last = lvl;
        }
        if (!hit) {
            continue;
        }
        w->fired++;
        ESP_LOGI(TAG, "watch fired (%d): %s", w->fired, w->message);
        muse_voice_request_sound(MUSE_SOUND_TICK, 1);
        watch_say(w);
        if (w->once) {
            break;
        }
        for (int i = 0; i < w->cooldown_s * 10 && !w->cancel; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (w->noise) {
            muse_voice_set_monitor(true);
        }
    }
    if (w->noise) {
        muse_voice_set_monitor(false);
    }
    ESP_LOGI(TAG, "watch over: fired %d, %s", w->fired, w->cancel ? "cancelled" : "done");
    s_watch = NULL;
    free(w);
    vTaskDelete(NULL);
}

static cJSON *cmd_watch(cJSON *params)
{
    if (s_watch) {
        return fail("busy", "already watching; event.cancel first");
    }
    const char *kind = str_param(params, "kind");
    const char *message = str_param(params, "message");
    if (!kind || !message) {
        return fail("invalid_params", "kind (noise or gpio) and message are required");
    }
    watch_t *w = calloc(1, sizeof(*w));
    if (!w) {
        return fail("out_of_memory", "failed to allocate");
    }
    strlcpy(w->message, message, sizeof(w->message));
    w->seconds = 900;
    int_param(params, "seconds", &w->seconds);
    w->seconds = clampi(w->seconds, 5, WATCH_MAX_S);
    w->cooldown_s = 30;
    int_param(params, "cooldown_s", &w->cooldown_s);
    w->cooldown_s = clampi(w->cooldown_s, 1, 3600);
    w->once = true;
    bool_param(params, "once", &w->once);
    if (!strcasecmp(kind, "noise")) {
        if (muse_voice_resting()) {
            free(w);
            return fail("unavailable", "the mic is resting (asleep on battery)");
        }
        w->noise = true;
        w->threshold = -25.0f;
        float_param(params, "threshold_dbfs", &w->threshold);
        if (w->threshold > -3) w->threshold = -3;
        if (w->threshold < -70) w->threshold = -70;
    } else if (!strcasecmp(kind, "gpio")) {
        if (!int_param(params, "pin", &w->pin) || !gadget_tools_gpio_allowed(w->pin)) {
            free(w);
            return fail("not_allowed", "pin is required and must be on the allowed list");
        }
        w->level = 0;
        int_param(params, "level", &w->level);
        w->level = w->level ? 1 : 0;
    } else {
        free(w);
        return fail("invalid_params", "kind: noise or gpio");
    }
    s_watch = w;
    if (xTaskCreate(watch_task, "event_watch", 4096, w, 3, NULL) != pdPASS) {
        s_watch = NULL;
        free(w);
        return fail("out_of_memory", "failed to start");
    }
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddBoolToObject(payload, "watching", true);
    cJSON_AddStringToObject(payload, "kind", kind);
    cJSON_AddNumberToObject(payload, "seconds", w->seconds);
    cJSON_AddBoolToObject(payload, "once", w->once);
    return ok_with(payload);
}

static cJSON *cmd_watch_cancel(void)
{
    watch_t *w = s_watch;
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddBoolToObject(payload, "cancelled", w != NULL);
    if (w) {
        cJSON_AddNumberToObject(payload, "fired", w->fired);
        w->cancel = true;
    }
    return ok_with(payload);
}

/* ---- ping --------------------------------------------------------------- */

typedef struct {
    pending_t req;
    char host[96];
    int count;
    volatile int done;
    int replies;
    uint32_t sum_ms, min_ms, max_ms;
} ping_ctx_t;

static void on_ping_ok(esp_ping_handle_t hdl, void *arg)
{
    ping_ctx_t *p = arg;
    uint32_t ms = 0;
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &ms, sizeof(ms));
    p->replies++;
    p->sum_ms += ms;
    p->min_ms = p->replies == 1 || ms < p->min_ms ? ms : p->min_ms;
    p->max_ms = ms > p->max_ms ? ms : p->max_ms;
}

static void on_ping_end(esp_ping_handle_t hdl, void *arg)
{
    ((ping_ctx_t *)arg)->done = 1;
}

static void ping_task(void *arg)
{
    ping_ctx_t *p = arg;
    cJSON *result;
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    if (getaddrinfo(p->host, NULL, &hints, &res) != 0 || !res) {
        result = fail("not_found", "could not resolve that host");
    } else {
        ip_addr_t target;
        memset(&target, 0, sizeof(target));
        struct sockaddr_in *sin = (struct sockaddr_in *)res->ai_addr;
        inet_addr_to_ip4addr(ip_2_ip4(&target), &sin->sin_addr);
        target.type = IPADDR_TYPE_V4;
        char ip[16];
        strlcpy(ip, inet_ntoa(sin->sin_addr), sizeof(ip));
        freeaddrinfo(res);

        esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
        cfg.target_addr = target;
        cfg.count = p->count;
        cfg.interval_ms = 400;
        cfg.timeout_ms = 1000;
        esp_ping_callbacks_t cbs = { .on_ping_success = on_ping_ok, .on_ping_end = on_ping_end, .cb_args = p };
        esp_ping_handle_t hdl;
        if (esp_ping_new_session(&cfg, &cbs, &hdl) != ESP_OK) {
            result = fail("ping_failed", "could not start a ping session");
        } else {
            esp_ping_start(hdl);
            for (int i = 0; i < p->count * 15 + 30 && !p->done; i++) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            esp_ping_stop(hdl);
            esp_ping_delete_session(hdl);
            cJSON *payload = cJSON_CreateObject();
            cJSON_AddStringToObject(payload, "host", p->host);
            cJSON_AddStringToObject(payload, "ip", ip);
            cJSON_AddNumberToObject(payload, "sent", p->count);
            cJSON_AddNumberToObject(payload, "received", p->replies);
            cJSON_AddBoolToObject(payload, "up", p->replies > 0);
            if (p->replies) {
                cJSON_AddNumberToObject(payload, "avg_ms", (double)(p->sum_ms / p->replies));
                cJSON_AddNumberToObject(payload, "min_ms", p->min_ms);
                cJSON_AddNumberToObject(payload, "max_ms", p->max_ms);
            }
            result = ok_with(payload);
        }
    }
    noise_ctrl_send_command_result(p->req.gen, p->req.request_id, result);
    free(p);
    vTaskDelete(NULL);
}

static cJSON *cmd_ping(cJSON *params, const char *request_id, noise_ctrl_session_generation_t gen)
{
    const char *host = str_param(params, "host");
    if (!host) {
        return fail("invalid_params", "host is required");
    }
    ping_ctx_t *p = calloc(1, sizeof(*p));
    if (!p) {
        return fail("out_of_memory", "failed to allocate");
    }
    pending_init(&p->req, request_id, gen);
    strlcpy(p->host, host, sizeof(p->host));
    p->count = 3;
    int_param(params, "count", &p->count);
    p->count = clampi(p->count, 1, 5);
    if (xTaskCreate(ping_task, "net_ping", 4096, p, 4, NULL) != pdPASS) {
        free(p);
        return fail("out_of_memory", "failed to start");
    }
    return async_result();
}

/* ---- a log on the card --------------------------------------------------- */

static cJSON *cmd_log_append(cJSON *params)
{
    const char *text = str_param(params, "text");
    if (!text) {
        return fail("invalid_params", "text is required");
    }
    struct stat st;
    if (stat(SD_BASE, &st) != 0) {
        return fail("no_sd", "no SD card is mounted");
    }
    const char *name = str_param(params, "name");
    char clean[32] = "log";
    if (name) {
        size_t n = 0;
        for (const char *p = name; *p && n < sizeof(clean) - 1; p++) {
            if (isalnum((unsigned char)*p) || *p == '-' || *p == '_') {
                clean[n++] = (char)tolower((unsigned char)*p);
            }
        }
        clean[n] = '\0';
        if (!n) {
            strlcpy(clean, "log", sizeof(clean));
        }
    }
    mkdir(SD_BASE "/logs", 0775);
    char path[64];
    snprintf(path, sizeof(path), SD_BASE "/logs/%s.txt", clean);
    FILE *f = fopen(path, "a");
    if (!f) {
        return fail("write_failed", "could not open the log");
    }
    char stamp[32];
    time_t now = time(NULL);
    if (now > 1600000000LL) {
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm);
    } else {
        snprintf(stamp, sizeof(stamp), "+%llus", (unsigned long long)(esp_timer_get_time() / 1000000));
    }
    int n = fprintf(f, "%s  %s\n", stamp, text);
    fclose(f);
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "path", path + sizeof(SD_BASE) - 1);
    cJSON_AddStringToObject(payload, "stamp", stamp);
    cJSON_AddNumberToObject(payload, "bytes", n);
    return ok_with(payload);
}

/* ---- the stopwatch ------------------------------------------------------- */

static bool s_sw_running;
static int64_t s_sw_start_us, s_sw_acc_us;

static int64_t sw_elapsed_us(void)
{
    return s_sw_acc_us + (s_sw_running ? esp_timer_get_time() - s_sw_start_us : 0);
}

static cJSON *sw_json(void)
{
    cJSON *j = cJSON_CreateObject();
    int64_t us = sw_elapsed_us();
    cJSON_AddBoolToObject(j, "running", s_sw_running);
    cJSON_AddNumberToObject(j, "elapsed_s", (double)((us / 100000) / 10.0));
    char buf[24];
    int64_t s = us / 1000000;
    snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld.%lld", s / 3600, (s / 60) % 60, s % 60, (us / 100000) % 10);
    cJSON_AddStringToObject(j, "display", buf);
    return j;
}

static cJSON *cmd_stopwatch(const char *what)
{
    if (!strcmp(what, "start")) {
        if (!s_sw_running) {
            s_sw_start_us = esp_timer_get_time();
            s_sw_running = true;
            muse_voice_request_sound(MUSE_SOUND_TICK, 1);
        }
    } else if (!strcmp(what, "stop")) {
        if (s_sw_running) {
            s_sw_acc_us += esp_timer_get_time() - s_sw_start_us;
            s_sw_running = false;
            muse_voice_request_sound(MUSE_SOUND_BEEP, 1);
        }
    } else if (!strcmp(what, "reset")) {
        s_sw_running = false;
        s_sw_acc_us = 0;
    }
    return ok_with(sw_json());
}

bool gadget_more_caption(char *out, size_t cap)
{
    if (!s_sw_running) {
        return false;
    }
    int64_t s = sw_elapsed_us() / 1000000;
    if (s >= 3600) {
        snprintf(out, cap, "STOPWATCH %lld:%02lld:%02lld", s / 3600, (s / 60) % 60, s % 60);
    } else {
        snprintf(out, cap, "STOPWATCH %lld:%02lld", s / 60, s % 60);
    }
    return true;
}

/* ---- identify, reboot --------------------------------------------------- */

static void call_tool(const char *command, cJSON *params)
{
    cJSON *r = gadget_tools_command(command, params, "self", 0);
    cJSON_Delete(r);
    cJSON_Delete(params);
}

static cJSON *cmd_identify(void)
{
    cJSON *light = cJSON_CreateObject();
    cJSON_AddStringToObject(light, "color", "white");
    cJSON_AddStringToObject(light, "effect", "blink");
    cJSON_AddNumberToObject(light, "brightness", 100);
    cJSON_AddNumberToObject(light, "seconds", 10);
    call_tool("light.set", light);
    cJSON *sound = cJSON_CreateObject();
    cJSON_AddStringToObject(sound, "name", "success");
    cJSON_AddNumberToObject(sound, "times", 2);
    call_tool("sound.play", sound);
    cJSON *text = cJSON_CreateObject();
    cJSON_AddStringToObject(text, "text", "HERE I AM!");
    cJSON_AddNumberToObject(text, "seconds", 10);
    cJSON_AddBoolToObject(text, "chime", false);
    call_tool("screen.show_text", text);
    return ok_with(NULL);
}

static void reboot_cb(void *arg)
{
    esp_restart();
}

static cJSON *cmd_reboot(void)
{
    esp_timer_handle_t t;
    const esp_timer_create_args_t args = { .callback = reboot_cb, .name = "reboot" };
    if (esp_timer_create(&args, &t) == ESP_OK) {
        esp_timer_start_once(t, 1500000);
    }
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddBoolToObject(payload, "rebooting", true);
    cJSON_AddStringToObject(payload, "note", "back in about 20 seconds");
    return ok_with(payload);
}


/* ---- a scrolling marquee ------------------------------------------------- */

static canvas_t *s_mq;
static esp_timer_handle_t s_mq_timer;
static char s_mq_text[200];
static int s_mq_x, s_mq_scale, s_mq_step, s_mq_w, s_mq_h, s_mq_frames;
static uint32_t s_mq_fg, s_mq_bg;
static int64_t s_mq_until_us;

static void marquee_stop(void)
{
    if (s_mq_timer) {
        esp_timer_stop(s_mq_timer);
    }
    if (s_mq) {
        canvas_free(s_mq);
        s_mq = NULL;
    }
}

static void marquee_cb(void *arg)
{
    if (!s_mq) {
        return;
    }
    int64_t now = esp_timer_get_time();
#if CONFIG_HOMEHUB_DISPLAY_COMMANDS
    bool hidden = !muse_ui_image_visible();
#else
    bool hidden = true;
#endif
    if ((s_mq_until_us && now >= s_mq_until_us) || (hidden && s_mq_frames > 0)) {
        /* time's up, or a tap took the picture down */
        marquee_stop();
        if (!hidden) {
            picture_down();
        }
        s_pic_up = false;
        return;
    }
    s_mq_frames++;
    int tw = canvas_text_width(s_mq_text, s_mq_scale);
    canvas_clear(s_mq, s_mq_bg);
    canvas_text(s_mq, s_mq_x, (s_mq_h - CANVAS_GLYPH_H * s_mq_scale) / 2, s_mq_text, s_mq_scale, s_mq_fg);
    canvas_show(s_mq);
    s_mq_x -= s_mq_step;
    if (s_mq_x + tw < 0) {
        s_mq_x = s_mq_w;
    }
}

static cJSON *cmd_marquee(cJSON *params)
{
    const char *text = str_param(params, "text");
    if (!text) {
        return fail("invalid_params", "text is required");
    }
    int w, h;
    if (!screen_size(&w, &h)) {
        return fail("unavailable", "no screen for pictures");
    }
    int scale = 6, speed = 5, seconds = 20;
    int_param(params, "scale", &scale);
    int_param(params, "speed", &speed);
    int_param(params, "seconds", &seconds);
    scale = clampi(scale, 2, 10);
    speed = clampi(speed, 1, 10);
    seconds = clampi(seconds, 0, 3600);
    marquee_stop();
    s_mq = canvas_create(w, h, color_param(params, "background", 0x000000));
    if (!s_mq) {
        return fail("out_of_memory", "no room for the picture");
    }
    strlcpy(s_mq_text, text, sizeof(s_mq_text));
    for (char *p = s_mq_text; *p; p++) {
        if (*p == '\n') {
            *p = ' ';
        }
    }
    s_mq_fg = color_param(params, "color", 0xffffff);
    s_mq_bg = color_param(params, "background", 0x000000);
    s_mq_w = w;
    s_mq_h = h;
    s_mq_scale = scale;
    s_mq_step = speed * 2;
    s_mq_x = w;
    s_mq_frames = 0;
    s_mq_until_us = seconds ? esp_timer_get_time() + (int64_t)seconds * 1000000 : 0;
    if (!s_mq_timer) {
        const esp_timer_create_args_t args = { .callback = marquee_cb, .name = "marquee" };
        esp_timer_create(&args, &s_mq_timer);
    }
    muse_state_set_asleep(false);
    s_clock_on = false;
    s_pic_up = true;
    s_pic_until_us = 0;
    esp_timer_start_periodic(s_mq_timer, 80000);
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddNumberToObject(payload, "scale", scale);
    cJSON_AddNumberToObject(payload, "px_per_s", s_mq_step * 1000 / 80);
    cJSON_AddNumberToObject(payload, "seconds", seconds);
    return ok_with(payload);
}

/* ---- PWM on a pin -------------------------------------------------------- */

#define PWM_CHANNELS 4
static int s_pwm_pin[PWM_CHANNELS] = { -1, -1, -1, -1 };

static cJSON *cmd_gpio_pwm(cJSON *params)
{
    int pin, duty = 50, freq = 1000;
    if (!int_param(params, "pin", &pin) || !gadget_tools_gpio_allowed(pin)) {
        return fail("not_allowed", "pin is required and must be on the allowed list");
    }
    int_param(params, "duty_pct", &duty);
    int_param(params, "freq_hz", &freq);
    duty = clampi(duty, 0, 100);
    freq = clampi(freq, 1, 40000);
    int ch = -1;
    for (int i = 0; i < PWM_CHANNELS; i++) {
        if (s_pwm_pin[i] == pin) {
            ch = i;
        }
    }
    for (int i = 0; i < PWM_CHANNELS && ch < 0; i++) {
        if (s_pwm_pin[i] < 0) {
            ch = i;
            s_pwm_pin[i] = pin;
        }
    }
    if (ch < 0) {
        return fail("busy", "all 4 PWM channels are in use");
    }
    if (duty == 0) {   /* off: stop the channel and give the pin back, no reconfigure */
        ledc_stop(LEDC_LOW_SPEED_MODE, (ledc_channel_t)(LEDC_CHANNEL_1 + ch), 0);
        gpio_reset_pin(pin);
        s_pwm_pin[ch] = -1;
        cJSON *off = cJSON_CreateObject();
        cJSON_AddNumberToObject(off, "pin", pin);
        cJSON_AddNumberToObject(off, "duty_pct", 0);
        return ok_with(off);
    }
    /* Timer 0 and channel 0 belong to the backlight; every PWM pin shares timer 1, so one frequency. */
    ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_1,
        .freq_hz = freq,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    if (ledc_timer_config(&t) != ESP_OK) {
        return fail("pwm_failed", "that frequency isn't possible");
    }
    ledc_channel_config_t c = {
        .gpio_num = pin,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = (ledc_channel_t)(LEDC_CHANNEL_1 + ch),
        .timer_sel = LEDC_TIMER_1,
        .duty = (uint32_t)duty * 1023 / 100,
        .hpoint = 0,
    };
    if (ledc_channel_config(&c) != ESP_OK) {
        return fail("pwm_failed", "could not set up the channel");
    }
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddNumberToObject(payload, "pin", pin);
    cJSON_AddNumberToObject(payload, "freq_hz", freq);
    cJSON_AddNumberToObject(payload, "duty_pct", duty);
    cJSON_AddStringToObject(payload, "note", "all PWM pins share one frequency");
    return ok_with(payload);
}

/* ---- a spectrum of the room ---------------------------------------------- */

#define FFT_N 512

typedef struct {
    pending_t req;
    int frames, bands;
} spectrum_args_t;

/* In place, radix-2, n a power of two. */
static void fft(float *re, float *im, int n)
{
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            float t = re[i];
            re[i] = re[j];
            re[j] = t;
            t = im[i];
            im[i] = im[j];
            im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        float ang = -2.0f * (float)M_PI / len;
        float wr = cosf(ang), wi = sinf(ang);
        for (int i = 0; i < n; i += len) {
            float cr = 1, ci = 0;
            for (int k = 0; k < len / 2; k++) {
                int a = i + k, b = i + k + len / 2;
                float tr = re[b] * cr - im[b] * ci, ti = re[b] * ci + im[b] * cr;
                re[b] = re[a] - tr;
                im[b] = im[a] - ti;
                re[a] += tr;
                im[a] += ti;
                float ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = ncr;
            }
        }
    }
}

static void spectrum_task(void *arg)
{
    spectrum_args_t *a = arg;
    int16_t *pcm = heap_caps_malloc((size_t)a->frames * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    float *re = malloc(FFT_N * sizeof(float)), *im = malloc(FFT_N * sizeof(float));
    float *mag = calloc(FFT_N / 2, sizeof(float));
    cJSON *result = NULL;
    if (!pcm || !re || !im || !mag) {
        result = fail("out_of_memory", "no room for the analysis");
    } else if (!muse_voice_request_capture(pcm, a->frames)) {
        result = fail("busy", "the mic is in use");
    } else {
        size_t got = 0;
        for (int waited = 0; !muse_voice_capture_done(&got) && waited < 100; waited++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (got < FFT_N) {
            result = fail("unavailable", "no audio came in (asleep, or a voice turn in progress)");
        } else {
            int windows = 0;
            for (size_t off = 0; off + FFT_N <= got; off += FFT_N / 2) {
                for (int i = 0; i < FFT_N; i++) {
                    float win = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * i / (FFT_N - 1));
                    re[i] = pcm[off + i] / 32768.0f * win;
                    im[i] = 0;
                }
                fft(re, im, FFT_N);
                for (int k = 0; k < FFT_N / 2; k++) {
                    mag[k] += re[k] * re[k] + im[k] * im[k];
                }
                windows++;
            }
            const float ref = (FFT_N / 4.0f) * (FFT_N / 4.0f);   /* a full-scale sine's bin, Hann windowed */
            int peak_k = 2;
            float total = 0;
            for (int k = 2; k < FFT_N / 2; k++) {
                mag[k] /= windows;
                total += mag[k];
                if (mag[k] > mag[peak_k]) {
                    peak_k = k;
                }
            }
            cJSON *bands = cJSON_CreateArray();
            float lo = 60.0f, ratio = powf(8000.0f / 60.0f, 1.0f / a->bands);
            int loudest = 0;
            float loudest_db = -200;
            for (int b = 0; b < a->bands; b++) {
                float hi = lo * ratio;
                int k0 = (int)(lo * FFT_N / MUSE_AUDIO_RATE), k1 = (int)(hi * FFT_N / MUSE_AUDIO_RATE);
                if (k0 < 1) k0 = 1;
                if (k1 <= k0) k1 = k0 + 1;
                if (k1 > FFT_N / 2) k1 = FFT_N / 2;
                float e = 0;
                for (int k = k0; k < k1; k++) {
                    e += mag[k];
                }
                float db = 10.0f * log10f(e / ref + 1e-12f);
                cJSON *j = cJSON_CreateObject();
                cJSON_AddNumberToObject(j, "from_hz", (int)lo);
                cJSON_AddNumberToObject(j, "to_hz", (int)hi);
                cJSON_AddNumberToObject(j, "dbfs", (double)((int)(db * 10) / 10.0));
                cJSON_AddItemToArray(bands, j);
                if (db > loudest_db) {
                    loudest_db = db;
                    loudest = b;
                }
                lo = hi;
            }
            float peak_hz = (float)peak_k * MUSE_AUDIO_RATE / FFT_N;
            float overall = 10.0f * log10f(total / ref + 1e-12f);
            cJSON *payload = cJSON_CreateObject();
            cJSON_AddItemToObject(payload, "bands", bands);
            cJSON_AddNumberToObject(payload, "loudest_band", loudest);
            cJSON_AddNumberToObject(payload, "peak_hz", (int)peak_hz);
            cJSON_AddNumberToObject(payload, "peak_dbfs", (double)((int)(10.0f * log10f(mag[peak_k] / ref + 1e-12f) * 10) / 10.0));
            cJSON_AddNumberToObject(payload, "overall_dbfs", (double)((int)(overall * 10) / 10.0));
            cJSON_AddStringToObject(payload, "character",
                                    overall < -60 ? "silence" : peak_hz < 250 ? "bass / hum" : peak_hz < 2500 ? "voice / mid" : "treble / hiss");
            cJSON_AddNumberToObject(payload, "seconds", (double)((int)(got * 10 / MUSE_AUDIO_RATE) / 10.0));
            result = ok_with(payload);
        }
    }
    free(pcm);
    free(re);
    free(im);
    free(mag);
    noise_ctrl_send_command_result(a->req.gen, a->req.request_id, result);
    free(a);
    vTaskDelete(NULL);
}

static cJSON *cmd_mic_spectrum(cJSON *params, const char *request_id, noise_ctrl_session_generation_t gen)
{
    if (muse_voice_resting()) {
        return fail("unavailable", "the mic is resting (asleep on battery)");
    }
    spectrum_args_t *a = calloc(1, sizeof(*a));
    if (!a) {
        return fail("out_of_memory", "failed to allocate");
    }
    pending_init(&a->req, request_id, gen);
    float seconds = 1.0f;
    float_param(params, "seconds", &seconds);
    if (seconds < 0.5f) seconds = 0.5f;
    if (seconds > 3.0f) seconds = 3.0f;
    a->frames = (int)(seconds * MUSE_AUDIO_RATE);
    a->frames = (a->frames + MUSE_AUDIO_CHUNK - 1) / MUSE_AUDIO_CHUNK * MUSE_AUDIO_CHUNK;
    a->bands = 8;
    int_param(params, "bands", &a->bands);
    a->bands = clampi(a->bands, 4, 16);
    if (xTaskCreateWithCaps(spectrum_task, "mic_spectrum", 8192, a, 4, NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        free(a);
        return fail("out_of_memory", "failed to start");
    }
    return async_result();
}

/* ---- ticks ---------------------------------------------------------------- */

void gadget_more_tick(int64_t now)
{
    if (s_pic_until_us && now >= s_pic_until_us) {
        picture_down();
    }
    if (s_clock_on) {
        if (s_clock_until_us && now >= s_clock_until_us) {
            picture_down();
            return;
        }
#if CONFIG_HOMEHUB_DISPLAY_COMMANDS
        if (!muse_ui_image_visible()) {   /* a tap took it down */
            s_clock_on = false;
            s_pic_up = false;
            return;
        }
#endif
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        if (tm.tm_min != s_clock_minute) {
            render_clock();
        }
    }
}

/* ---- the command table ---------------------------------------------------- */

void gadget_more_add_commands(cJSON *commands)
{
    cJSON *req, *opt;

#if CONFIG_HOMEHUB_DISPLAY_COMMANDS
    req = cJSON_CreateObject();
    cJSON_AddItemToObject(req, "text", param_spec("string", "A few words or a number; it is sized to fill the screen (newlines allowed)."));
    opt = cJSON_CreateObject();
    cJSON_AddItemToObject(opt, "color", param_spec("string", "Text colour name or #rrggbb; default white."));
    cJSON_AddItemToObject(opt, "background", param_spec("string", "Background colour; default black."));
    cJSON_AddItemToObject(opt, "seconds", param_spec("integer", "How long it stays; default 20, 0 until cleared or tapped."));
    add_command(commands, "screen.big_text",
                "Fill the gadget's screen with huge pixel-font text readable from across the room: a number, a word, a short "
                "status like DOOR OPEN or 21^C (^ draws a degree sign).", req, opt, 0);

    opt = cJSON_CreateObject();
    cJSON_AddItemToObject(opt, "on", param_spec("boolean", "true shows the clock (default), false takes it down."));
    cJSON_AddItemToObject(opt, "seconds", param_spec("integer", "How long to show it; default 0 = until cleared or tapped."));
    cJSON_AddItemToObject(opt, "color", param_spec("string", "Digit colour; default lime green."));
    cJSON_AddItemToObject(opt, "date", param_spec("boolean", "Show the weekday and date too; default true."));
    add_command(commands, "screen.clock", "Turn the gadget into a big clock (local time, updates every minute).", NULL, opt, 0);

    req = cJSON_CreateObject();
    cJSON_AddItemToObject(req, "items", param_spec("array",
        "Shapes drawn in order on a 240x320 screen, each {type, ...}: rect {x,y,w,h,color,fill,thickness}; "
        "circle {x,y,r,color,fill,thickness}; line {x,y,x2,y2,color,thickness}; "
        "text {x,y,text,color,scale 1-12 (5x7 px font times scale),align left|center|right}; "
        "bar {x,y,w,h,value 0-100,color,background}. Colours are names or #rrggbb. Up to 80 items."));
    opt = cJSON_CreateObject();
    cJSON_AddItemToObject(opt, "background", param_spec("string", "Background colour; default black."));
    cJSON_AddItemToObject(opt, "seconds", param_spec("integer", "How long it stays; default 0 = until cleared or tapped."));
    add_command(commands, "screen.draw",
                "Draw your own picture on the gadget's screen: charts, gauges, icons, diagrams, a dashboard of numbers. "
                "Portrait 240 wide by 320 tall.", req, opt, 0);
#endif

    req = cJSON_CreateObject();
    cJSON_AddItemToObject(req, "kind", param_spec("string", "noise (the room gets loud) or gpio (an allowed pin changes)."));
    cJSON_AddItemToObject(req, "message", param_spec("string", "What to say in the gadget's chat when it happens, as if the user typed it, so you can react (e.g. 'The dog barked, tell Mat' or 'Door opened, turn the light red')."));
    opt = cJSON_CreateObject();
    cJSON_AddItemToObject(opt, "threshold_dbfs", param_spec("number", "noise: how loud counts, -70 to -3; default -25."));
    cJSON_AddItemToObject(opt, "pin", param_spec("integer", "gpio: the pin to watch (allowed list only)."));
    cJSON_AddItemToObject(opt, "level", param_spec("integer", "gpio: the level that triggers, 0 (default, e.g. a switch to ground) or 1."));
    cJSON_AddItemToObject(opt, "seconds", param_spec("integer", "How long to watch, 5 to 3600; default 900."));
    cJSON_AddItemToObject(opt, "once", param_spec("boolean", "Stop after the first trigger (default true) or keep watching with a cooldown."));
    cJSON_AddItemToObject(opt, "cooldown_s", param_spec("integer", "Quiet time after a trigger when once is false; default 30."));
    add_command(commands, "event.watch",
                "Leave the gadget watching for something and have it speak up in this chat when it happens. Replies at once "
                "with watching:true; the message arrives later as a new user turn. One watch at a time.", req, opt, 0);
    add_command(commands, "event.cancel", "Stop the current event.watch.", NULL, NULL, 0);

    req = cJSON_CreateObject();
    cJSON_AddItemToObject(req, "host", param_spec("string", "Hostname or IP on the home network or the internet."));
    opt = cJSON_CreateObject();
    cJSON_AddItemToObject(opt, "count", param_spec("integer", "Pings to send, 1 to 5; default 3."));
    add_command(commands, "net.ping", "Ping a host from the gadget: is it up, and how fast does it answer?", req, opt, 20000);

    req = cJSON_CreateObject();
    cJSON_AddItemToObject(req, "text", param_spec("string", "The line to log."));
    opt = cJSON_CreateObject();
    cJSON_AddItemToObject(opt, "name", param_spec("string", "Log name, letters and digits; default log. Kept at /logs/<name>.txt on the SD card."));
    add_command(commands, "log.append", "Append a timestamped line to a log file on the SD card (a diary, a tally, measurements).", req, opt, 0);

    add_command(commands, "stopwatch.start", "Start (or resume) the gadget's stopwatch; it shows under the avatar while it runs.", NULL, NULL, 0);
    add_command(commands, "stopwatch.stop", "Pause the stopwatch and report the time.", NULL, NULL, 0);
    add_command(commands, "stopwatch.reset", "Reset the stopwatch to zero.", NULL, NULL, 0);
    add_command(commands, "stopwatch.read", "The stopwatch's time so far.", NULL, NULL, 0);

#if CONFIG_HOMEHUB_DISPLAY_COMMANDS
    req = cJSON_CreateObject();
    cJSON_AddItemToObject(req, "text", param_spec("string", "The line to scroll, up to 200 characters."));
    opt = cJSON_CreateObject();
    cJSON_AddItemToObject(opt, "scale", param_spec("integer", "Letter size 2 to 10 (5x7 px times scale); default 6."));
    cJSON_AddItemToObject(opt, "speed", param_spec("integer", "1 slow to 10 fast; default 5."));
    cJSON_AddItemToObject(opt, "color", param_spec("string", "Text colour; default white."));
    cJSON_AddItemToObject(opt, "background", param_spec("string", "Background colour; default black."));
    cJSON_AddItemToObject(opt, "seconds", param_spec("integer", "How long it scrolls; default 20, 0 until cleared or tapped."));
    add_command(commands, "screen.marquee", "Scroll a long message across the gadget's screen in big letters, like a ticker.", req, opt, 0);
#endif

    req = cJSON_CreateObject();
    cJSON_AddItemToObject(req, "pin", param_spec("integer", "An allowed GPIO."));
    opt = cJSON_CreateObject();
    cJSON_AddItemToObject(opt, "duty_pct", param_spec("integer", "0 to 100; default 50. 0 stops the PWM and frees the pin."));
    cJSON_AddItemToObject(opt, "freq_hz", param_spec("integer", "1 to 40000; default 1000. All PWM pins share one frequency."));
    add_command(commands, "gpio.pwm", "PWM on an allowed pin: dim an LED, drive a buzzer at a pitch, set a motor's speed (through a proper driver board).", req, opt, 0);

    opt = cJSON_CreateObject();
    cJSON_AddItemToObject(opt, "seconds", param_spec("number", "How long to listen, 0.5 to 3; default 1."));
    cJSON_AddItemToObject(opt, "bands", param_spec("integer", "Frequency bands between 60 Hz and 8 kHz, 4 to 16; default 8."));
    add_command(commands, "mic.spectrum",
                "A frequency breakdown of the room's sound: level per band in dBFS, the peak frequency, and a word for its character "
                "(silence, bass/hum, voice/mid, treble/hiss). Music, a running fan, a hum, a voice. Levels only, nothing kept.", NULL, opt, 15000);

    add_command(commands, "gadget.identify", "Make the gadget show itself: light blinks white, a jingle, HERE I AM on the screen.", NULL, NULL, 0);
    add_command(commands, "gadget.reboot", "Restart the gadget (it reconnects in about 20 seconds).", NULL, NULL, 0);
}

cJSON *gadget_more_command(const char *command, cJSON *params, const char *request_id,
                           noise_ctrl_session_generation_t gen)
{
#if CONFIG_HOMEHUB_DISPLAY_COMMANDS
    if (!strcmp(command, "screen.big_text")) return cmd_big_text(params);
    if (!strcmp(command, "screen.clock")) return cmd_clock(params);
    if (!strcmp(command, "screen.draw")) return cmd_draw(params);
#endif
    if (!strcmp(command, "event.watch")) return cmd_watch(params);
    if (!strcmp(command, "event.cancel")) return cmd_watch_cancel();
    if (!strcmp(command, "net.ping")) return cmd_ping(params, request_id, gen);
    if (!strcmp(command, "log.append")) return cmd_log_append(params);
    if (!strcmp(command, "stopwatch.start")) return cmd_stopwatch("start");
    if (!strcmp(command, "stopwatch.stop")) return cmd_stopwatch("stop");
    if (!strcmp(command, "stopwatch.reset")) return cmd_stopwatch("reset");
    if (!strcmp(command, "stopwatch.read")) return cmd_stopwatch("read");
#if CONFIG_HOMEHUB_DISPLAY_COMMANDS
    if (!strcmp(command, "screen.marquee")) return cmd_marquee(params);
#endif
    if (!strcmp(command, "gpio.pwm")) return cmd_gpio_pwm(params);
    if (!strcmp(command, "mic.spectrum")) return cmd_mic_spectrum(params, request_id, gen);
    if (!strcmp(command, "gadget.identify")) return cmd_identify();
    if (!strcmp(command, "gadget.reboot")) return cmd_reboot();
    return NULL;
}

void gadget_more_init(void)
{
    ESP_LOGI(TAG, "ready");
}
