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
/*
 * Gadget tools: see gadget_tools.h. Everything here runs beside the voice task
 * without touching the codec directly: sounds, clips and recordings go through
 * muse_voice_request_*, text goes through muse_state's caption, and the
 * screen's "reply page" is borrowed by putting Muse in MUSE_MODE_SPEAKING while
 * a message is up. A 1 s tick (esp_timer) drives countdowns, message expiry and
 * timer alarms; a 40 ms one animates the light. Long jobs (downloads, the mic,
 * Wi-Fi scans) run on their own short-lived tasks and answer later through
 * noise_ctrl_send_command_result().
 */
#include "gadget_tools.h"
#include "gadget_more.h"
#if CONFIG_MUSE_PET
#include "gadget_pet.h"
#endif

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "qrcode.h"

#include "muse_audio.h"
#include "muse_chat.h"
#include "muse_prompt.h"
#include "muse_settings.h"
#include "muse_state.h"
#include "muse_ui.h"
#include "muse_voice.h"
#include "muse_wifi.h"

#if CONFIG_HOMEHUB_DISPLAY_COMMANDS
#include "led_status.h"
#endif
#if CONFIG_MUSE_TOOLS_RGB_LED_GPIO >= 0
#include "led_strip.h"
#endif
#if SOC_TEMP_SENSOR_SUPPORTED
#include "driver/temperature_sensor.h"
#endif

static const char *TAG = "gadget_tools";

#define MAX_TIMERS 8
#define LABEL_MAX 32
#define TEXT_DEFAULT_S 20
#define TIMER_DONE_SHOW_S 60
#define TIMER_ALARM_TIMES 3
#define LIGHT_ALERT_S 30
#define LIGHT_DEFAULT_PCT 40
#define MIC_MIN_S 1
#define MIC_MAX_S 10
#define MIC_DEFAULT_S 3
#define LISTEN_MAX_S 600
#define RECORD_MAX_S 20
#define FETCH_MAX_BYTES 16384
#define FETCH_DEFAULT_BYTES 4096
#define AUDIO_DL_MAX (1200 * 1024)         /* an MP3 or WAV clip over the network */
#define AUDIO_MAX_FRAMES (MUSE_AUDIO_RATE * 40)   /* 40 s of 16 kHz mono, 1.28 MB of PSRAM */
#define SD_BASE "/sd"
#define SD_SAVE_MAX (8 * 1024 * 1024)
#define SD_READ_MAX 16384
#define TIME_IS_SET 1600000000LL   /* seconds since 1970 that only a synced clock reaches */
#define REQUEST_ID_MAX 64

/* ---- results ------------------------------------------------------------ */

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

/* The request a background task answers later. */
typedef struct {
    noise_ctrl_session_generation_t gen;
    char request_id[REQUEST_ID_MAX];
} pending_t;

static void pending_init(pending_t *p, const char *request_id, noise_ctrl_session_generation_t gen)
{
    p->gen = gen;
    strlcpy(p->request_id, request_id, sizeof(p->request_id));
}

static void pending_send(const pending_t *p, cJSON *result)
{
    noise_ctrl_send_command_result(p->gen, p->request_id, result);
}

/* ---- params ------------------------------------------------------------- */

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
    if (cJSON_IsString(item) && item->valuestring) {   /* "90" from a loose agent */
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

static void *big_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
}

/* Mostly printable UTF-8-ish bytes: shown as text; anything else goes out as base64. */
static bool looks_like_text(const uint8_t *b, size_t n)
{
    size_t bad = 0;
    for (size_t i = 0; i < n; i++) {
        if (b[i] == 0) {
            return false;
        }
        if (b[i] < 0x20 && b[i] != '\n' && b[i] != '\r' && b[i] != '\t') {
            bad++;
        }
    }
    return bad * 50 < n + 1;
}

static void add_bytes(cJSON *payload, const char *key, const uint8_t *b, size_t n)
{
    if (looks_like_text(b, n)) {
        char *s = malloc(n + 1);
        if (s) {
            memcpy(s, b, n);
            s[n] = '\0';
            cJSON_AddStringToObject(payload, key, s);
            cJSON_AddStringToObject(payload, "encoding", "text");
            free(s);
            return;
        }
    }
    size_t cap = (n + 2) / 3 * 4 + 1, out = 0;
    unsigned char *enc = malloc(cap);
    if (enc && mbedtls_base64_encode(enc, cap, &out, b, n) == 0) {
        cJSON_AddStringToObject(payload, key, (char *)enc);
        cJSON_AddStringToObject(payload, "encoding", "base64");
    }
    free(enc);
}

/* ---- clock -------------------------------------------------------------- */

static bool clock_set(void)
{
    return time(NULL) > TIME_IS_SET;
}

static void local_time_str(time_t t, char *out, size_t cap)
{
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(out, cap, "%Y-%m-%d %H:%M:%S", &tm);
}

/* ---- the message on the screen ----------------------------------------- */

static int64_t s_text_shown_us;        /* when the current message went up (0: none) */
static int64_t s_text_until_us;        /* 0: until cleared */
static char s_text_pending[MUSE_CAPTION_MAX];   /* shown at the next idle tick */
static int s_text_pending_s;
static bool s_caption_mine;            /* the idle caption is our countdown */

static void text_clear(void)
{
    if (!s_text_shown_us) {
        return;
    }
    float secs = 0;
    muse_mode_t mode = muse_state_mode(&secs);
    /* Only if the voice task hasn't been through here since: it would have
     * changed the mode more recently than we did. */
    int64_t ours = (esp_timer_get_time() - s_text_shown_us) / 1000;
    if (mode == MUSE_MODE_SPEAKING && (int64_t)(secs * 1000) >= ours - 1500) {
        muse_state_set_mode(MUSE_MODE_IDLE);
        muse_state_set_caption("%s", "");
    }
    s_text_shown_us = 0;
    s_text_until_us = 0;
}

/* Puts text on the reply page. False when Muse is busy with a voice turn. */
static bool text_show(const char *text, int seconds)
{
    muse_state_set_asleep(false);
    muse_mode_t mode = muse_state_mode(NULL);
    if (mode != MUSE_MODE_IDLE && !(mode == MUSE_MODE_SPEAKING && s_text_shown_us)) {
        return false;
    }
#if CONFIG_HOMEHUB_DISPLAY_COMMANDS
    if (muse_ui_image_visible()) {   /* a message wins over a picture or clock */
        gadget_more_screen_clear();
        led_status_show_animation();
    }
#endif
    s_caption_mine = false;
    muse_state_set_mode(MUSE_MODE_SPEAKING);
    muse_state_set_caption("%s", text);
    s_text_shown_us = esp_timer_get_time();
    s_text_until_us = seconds > 0 ? s_text_shown_us + (int64_t)seconds * 1000000 : 0;
    return true;
}

/* ---- timers ------------------------------------------------------------- */

typedef struct {
    bool used;
    int id;
    int64_t start_us, end_us;
    time_t end_at;             /* 0 when the clock wasn't set */
    char label[LABEL_MAX];
    int sound;
} timer_t_;

static timer_t_ s_timers[MAX_TIMERS];
static int s_next_timer_id = 1;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static int timers_active(void)
{
    int n = 0;
    for (int i = 0; i < MAX_TIMERS; i++) {
        n += s_timers[i].used;
    }
    return n;
}

static timer_t_ *timer_soonest(void)
{
    timer_t_ *best = NULL;
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (s_timers[i].used && (!best || s_timers[i].end_us < best->end_us)) {
            best = &s_timers[i];
        }
    }
    return best;
}

/* The soonest timer's progress, 0..1, or -1 without one. */
static float timer_progress(void)
{
    portENTER_CRITICAL(&s_lock);
    timer_t_ *t = timer_soonest();
    float p = -1;
    if (t) {
        int64_t now = esp_timer_get_time();
        int64_t total = t->end_us - t->start_us;
        p = total > 0 ? (float)(now - t->start_us) / total : 1.0f;
    }
    portEXIT_CRITICAL(&s_lock);
    return p < 0 ? p : p > 1 ? 1 : p;
}

static cJSON *timer_json(const timer_t_ *t, int64_t now)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "id", t->id);
    cJSON_AddStringToObject(j, "label", t->label);
    int64_t left = (t->end_us - now + 500000) / 1000000;
    cJSON_AddNumberToObject(j, "remaining_s", left < 0 ? 0 : (double)left);
    if (t->end_at) {
        char when[32];
        local_time_str(t->end_at, when, sizeof(when));
        cJSON_AddStringToObject(j, "ends_at", when);
    }
    cJSON_AddStringToObject(j, "sound", muse_sound_name((muse_sound_t)t->sound));
    return j;
}

static void light_alert(void);

static void timer_fire(timer_t_ *t)
{
    ESP_LOGI(TAG, "timer %d '%s' done", t->id, t->label);
    char msg[LABEL_MAX + 24];
    snprintf(msg, sizeof(msg), "TIMER DONE\n\n%s", t->label);
    t->used = false;
    muse_voice_request_sound(t->sound, TIMER_ALARM_TIMES);
    light_alert();
    if (!text_show(msg, TIMER_DONE_SHOW_S)) {
        strlcpy(s_text_pending, msg, sizeof(s_text_pending));
        s_text_pending_s = TIMER_DONE_SHOW_S;
    }
}

/* Once a second: countdown caption, message expiry, timers going off. */
static void tick_cb(void *arg)
{
    int64_t now = esp_timer_get_time();
    gadget_more_tick(now);
    portENTER_CRITICAL(&s_lock);
    timer_t_ due[MAX_TIMERS];
    int ndue = 0;
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (s_timers[i].used && s_timers[i].end_us <= now) {
            due[ndue++] = s_timers[i];
            s_timers[i].used = false;
        }
    }
    portEXIT_CRITICAL(&s_lock);
    for (int i = 0; i < ndue; i++) {
        due[i].used = true;
        timer_fire(&due[i]);
    }

    if (s_text_until_us && now >= s_text_until_us) {
        text_clear();
    }
    muse_mode_t mode = muse_state_mode(NULL);
    if (s_text_pending[0] && mode == MUSE_MODE_IDLE) {
        if (text_show(s_text_pending, s_text_pending_s)) {
            s_text_pending[0] = '\0';
        }
    }
    if (mode != MUSE_MODE_IDLE || s_text_shown_us) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    timer_t_ *soon = timer_soonest();
    timer_t_ copy;
    if (soon) {
        copy = *soon;
    }
    portEXIT_CRITICAL(&s_lock);
    if (soon) {
        int64_t left = (copy.end_us - now + 500000) / 1000000;
        if (left < 0) {
            left = 0;
        }
        int n = timers_active();
        if (left >= 3600) {
            muse_state_set_caption("%s %lld:%02lld:%02lld%s", copy.label, left / 3600, (left / 60) % 60, left % 60,
                                   n > 1 ? " +" : "");
        } else {
            muse_state_set_caption("%s %lld:%02lld%s", copy.label, left / 60, left % 60, n > 1 ? " +" : "");
        }
        s_caption_mine = true;
    } else {
        char more[48];
        if (gadget_more_caption(more, sizeof(more))) {
            muse_state_set_caption("%s", more);
            s_caption_mine = true;
        } else if (s_caption_mine) {
            muse_state_set_caption("%s", "");
            s_caption_mine = false;
        }
    }
}

static cJSON *timer_start(int seconds, const char *label, const char *sound_name, time_t end_at)
{
    if (seconds < 1 || seconds > 24 * 3600) {
        return fail("invalid_params", "seconds must be 1 to 86400");
    }
    int sound = MUSE_SOUND_ALARM;
    if (sound_name) {
        sound = muse_sound_by_name(sound_name);
        if (sound < 0) {
            return fail("invalid_params", "unknown sound; see sound.play");
        }
    }
    timer_t_ *t = NULL;
    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < MAX_TIMERS && !t; i++) {
        if (!s_timers[i].used) {
            t = &s_timers[i];
        }
    }
    if (t) {
        memset(t, 0, sizeof(*t));
        t->used = true;
        t->id = s_next_timer_id++;
        t->start_us = esp_timer_get_time();
        t->end_us = t->start_us + (int64_t)seconds * 1000000;
        t->end_at = end_at ? end_at : clock_set() ? time(NULL) + seconds : 0;
        strlcpy(t->label, label && label[0] ? label : "TIMER", sizeof(t->label));
        for (char *p = t->label; *p; p++) {
            if (*p == '\n' || *p == '\r') {
                *p = ' ';
            }
        }
        t->sound = sound;
    }
    timer_t_ copy = t ? *t : (timer_t_){ 0 };
    portEXIT_CRITICAL(&s_lock);
    if (!t) {
        return fail("busy", "all 8 timers are in use; cancel one first");
    }
    muse_state_set_asleep(false);
    muse_voice_request_sound(MUSE_SOUND_TICK, 1);
    ESP_LOGI(TAG, "timer %d '%s' for %d s", copy.id, copy.label, seconds);
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddItemToObject(payload, "timer", timer_json(&copy, esp_timer_get_time()));
    cJSON_AddNumberToObject(payload, "active", timers_active());
    return ok_with(payload);
}

static cJSON *cmd_timer_set(cJSON *params)
{
    int seconds = 0, minutes = 0;
    bool have_s = int_param(params, "seconds", &seconds);
    bool have_m = int_param(params, "minutes", &minutes);
    if (!have_s && !have_m) {
        return fail("invalid_params", "seconds or minutes is required");
    }
    int total = (have_s ? seconds : 0) + (have_m ? minutes * 60 : 0);
    return timer_start(total, str_param(params, "label"), str_param(params, "sound"), 0);
}

static cJSON *cmd_timer_cancel(cJSON *params)
{
    int id = 0;
    bool one = int_param(params, "id", &id);
    int cancelled = 0;
    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (s_timers[i].used && (!one || s_timers[i].id == id)) {
            s_timers[i].used = false;
            cancelled++;
        }
    }
    portEXIT_CRITICAL(&s_lock);
    if (one && !cancelled) {
        return fail("not_found", "no timer with that id");
    }
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddNumberToObject(payload, "cancelled", cancelled);
    cJSON_AddNumberToObject(payload, "active", timers_active());
    return ok_with(payload);
}

static cJSON *timers_list_json(void)
{
    cJSON *list = cJSON_CreateArray();
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    timer_t_ copy[MAX_TIMERS];
    memcpy(copy, s_timers, sizeof(copy));
    portEXIT_CRITICAL(&s_lock);
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (copy[i].used) {
            cJSON_AddItemToArray(list, timer_json(&copy[i], now));
        }
    }
    return list;
}

static cJSON *cmd_timer_list(void)
{
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddItemToObject(payload, "timers", timers_list_json());
    cJSON_AddBoolToObject(payload, "clock_set", clock_set());
    return ok_with(payload);
}

static cJSON *cmd_alarm_set(cJSON *params)
{
    const char *at = str_param(params, "time");
    if (!at) {
        return fail("invalid_params", "time is required, as HH:MM (24 h)");
    }
    if (!clock_set()) {
        return fail("no_clock", "the clock isn't set yet (no time sync); use timer.set with seconds instead");
    }
    int hh, mm;
    if (sscanf(at, "%d:%d", &hh, &mm) != 2 || hh < 0 || hh > 23 || mm < 0 || mm > 59) {
        return fail("invalid_params", "time must be HH:MM, 24 hour");
    }
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    tm.tm_hour = hh;
    tm.tm_min = mm;
    tm.tm_sec = 0;
    time_t when = mktime(&tm);
    if (when <= now) {
        when += 24 * 3600;   /* that time has passed today: tomorrow */
    }
    const char *label = str_param(params, "label");
    return timer_start((int)(when - now), label ? label : "ALARM", str_param(params, "sound"), when);
}

static cJSON *cmd_clock_now(void)
{
    cJSON *payload = cJSON_CreateObject();
    bool set = clock_set();
    cJSON_AddBoolToObject(payload, "clock_set", set);
    if (set) {
        time_t now = time(NULL);
        char buf[48];
        local_time_str(now, buf, sizeof(buf));
        cJSON_AddStringToObject(payload, "local_time", buf);
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(buf, sizeof(buf), "%A", &tm);
        cJSON_AddStringToObject(payload, "weekday", buf);
        cJSON_AddNumberToObject(payload, "unix_time", (double)now);
    }
    cJSON_AddStringToObject(payload, "tz", CONFIG_MUSE_TOOLS_TZ);
    cJSON_AddNumberToObject(payload, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    return ok_with(payload);
}

/* ---- the light ---------------------------------------------------------- */

#if CONFIG_MUSE_TOOLS_RGB_LED_GPIO >= 0
#define HAVE_LIGHT 1
typedef enum { FX_OFF, FX_SOLID, FX_BLINK, FX_BREATHE, FX_RAINBOW, FX_PULSE, FX_CANDLE, FX_POLICE, FX_PROGRESS, FX_SPARKLE, FX_COUNT } light_fx_t;
static const char *const FX_NAMES[FX_COUNT] = { "off", "solid", "blink", "breathe", "rainbow", "pulse", "candle", "police", "progress", "sparkle" };
typedef struct {
    uint8_t r, g, b;
    uint8_t r2, g2, b2;    /* the second colour of police / sparkle */
    int pct;
    int speed;             /* 1..10 */
    light_fx_t fx;
} light_t;

static led_strip_handle_t s_strip;
static light_t s_light, s_light_prev;
static int64_t s_light_until_us;
static int s_light_phase;
static float s_candle;
static esp_timer_handle_t s_light_timer;

static const struct { const char *name; uint32_t rgb; } COLORS[] = {
    { "red", 0xff0000 }, { "green", 0x00ff00 }, { "blue", 0x0000ff }, { "white", 0xffffff },
    { "warm", 0xffb060 }, { "orange", 0xff6a00 }, { "yellow", 0xffd000 }, { "purple", 0x8000ff },
    { "pink", 0xff2080 }, { "cyan", 0x00ffff }, { "teal", 0x00c0a0 }, { "lime", 0x80ff00 },
    { "off", 0x000000 },
};

static bool parse_color(const char *s, uint32_t *rgb)
{
    for (size_t i = 0; i < sizeof(COLORS) / sizeof(COLORS[0]); i++) {
        if (!strcasecmp(s, COLORS[i].name)) {
            *rgb = COLORS[i].rgb;
            return true;
        }
    }
    if (*s == '#') {
        s++;
    }
    if (strlen(s) != 6) {
        return false;
    }
    char *end;
    unsigned long v = strtoul(s, &end, 16);
    if (*end) {
        return false;
    }
    *rgb = (uint32_t)v;
    return true;
}

static void hsv_to_rgb(float h, float s, float v, uint8_t *r, uint8_t *g, uint8_t *b)
{
    float c = v * s, x = c * (1 - fabsf(fmodf(h / 60.0f, 2) - 1)), m = v - c;
    float rr, gg, bb;
    if (h < 60) { rr = c; gg = x; bb = 0; }
    else if (h < 120) { rr = x; gg = c; bb = 0; }
    else if (h < 180) { rr = 0; gg = c; bb = x; }
    else if (h < 240) { rr = 0; gg = x; bb = c; }
    else if (h < 300) { rr = x; gg = 0; bb = c; }
    else { rr = c; gg = 0; bb = x; }
    *r = (uint8_t)((rr + m) * 255);
    *g = (uint8_t)((gg + m) * 255);
    *b = (uint8_t)((bb + m) * 255);
}

static void light_out(uint8_t r, uint8_t g, uint8_t b, float level)
{
    if (!s_strip) {
        return;
    }
    float k = level * s_light.pct / 100.0f;
    led_strip_set_pixel(s_strip, 0, (uint32_t)(r * k), (uint32_t)(g * k), (uint32_t)(b * k));
    led_strip_refresh(s_strip);
}

static void light_write(float level)
{
    light_out(s_light.r, s_light.g, s_light.b, level);
}

static void light_apply(void);

static void light_tick_cb(void *arg)
{
    int64_t now = esp_timer_get_time();
    if (s_light_until_us && now >= s_light_until_us) {
        s_light_until_us = 0;
        s_light = s_light_prev;
        light_apply();
        return;
    }
    s_light_phase++;
    float sp = s_light.speed / 5.0f;   /* 1.0 at speed 5 */
    switch (s_light.fx) {
    case FX_BLINK: {
        int period = (int)(20 / sp);   /* 800 ms at speed 5 */
        light_write((s_light_phase / (period > 1 ? period : 1)) % 2 ? 0.0f : 1.0f);
        break;
    }
    case FX_BREATHE: {
        int period = (int)(75 / sp);   /* a 3 s breath */
        float x = (s_light_phase % (period > 1 ? period : 1)) / (float)period;
        float level = x < 0.5f ? x * 2 : (1.0f - x) * 2;
        light_write(0.05f + 0.95f * level * level);
        break;
    }
    case FX_RAINBOW: {
        uint8_t r, g, b;
        hsv_to_rgb(fmodf(s_light_phase * 1.2f * sp, 360.0f), 1.0f, 1.0f, &r, &g, &b);
        light_out(r, g, b, 1.0f);
        break;
    }
    case FX_PULSE: {   /* a heartbeat: two beats, then rest */
        int period = (int)(30 / sp);
        int t = s_light_phase % (period > 1 ? period : 1);
        float level = t < 3 ? 1.0f : t < 6 ? 0.2f : t < 9 ? 0.9f : t < 14 ? 0.3f - (t - 9) * 0.05f : 0.05f;
        light_write(level);
        break;
    }
    case FX_CANDLE: {
        float target = 0.45f + (esp_random() % 1000) / 1000.0f * 0.55f;
        s_candle += (target - s_candle) * 0.35f * sp;
        light_write(s_candle);
        break;
    }
    case FX_POLICE: {
        int step = (int)(4 / sp);
        int t = (s_light_phase / (step > 0 ? step : 1)) % 8;
        bool first = t < 4;
        bool on = (t % 2) == 0;
        if (on) {
            light_out(first ? s_light.r : s_light.r2, first ? s_light.g : s_light.g2, first ? s_light.b : s_light.b2, 1.0f);
        } else {
            light_out(0, 0, 0, 0);
        }
        break;
    }
    case FX_PROGRESS: {   /* the soonest timer: green at the start, red at the end, breathing */
        float p = timer_progress();
        if (p < 0) {
            light_write(1.0f);
            break;
        }
        uint8_t r, g, b;
        hsv_to_rgb(120.0f * (1.0f - p), 1.0f, 1.0f, &r, &g, &b);
        float x = (s_light_phase % 50) / 50.0f;
        float breath = x < 0.5f ? x * 2 : (1.0f - x) * 2;
        light_out(r, g, b, 0.3f + 0.7f * breath);
        break;
    }
    case FX_SPARKLE: {
        if (esp_random() % (int)(12 / sp + 1) == 0) {
            bool alt = (esp_random() & 1) && (s_light.r2 | s_light.g2 | s_light.b2);
            light_out(alt ? s_light.r2 : s_light.r, alt ? s_light.g2 : s_light.g, alt ? s_light.b2 : s_light.b, 1.0f);
        } else {
            light_write(0.04f);
        }
        break;
    }
    default:
        break;
    }
}

static void light_apply(void)
{
    if (!s_strip) {
        return;
    }
    esp_timer_stop(s_light_timer);
    s_light_phase = 0;
    s_candle = 0.6f;
    if (s_light.fx == FX_OFF) {
        led_strip_clear(s_strip);
    } else {
        light_write(1.0f);
    }
    if (s_light.fx >= FX_BLINK || s_light_until_us) {
        esp_timer_start_periodic(s_light_timer, 40000);
    }
}

static void light_set(uint32_t rgb, uint32_t rgb2, int pct, light_fx_t fx, int speed, int seconds)
{
    if (seconds > 0 && !s_light_until_us) {
        s_light_prev = s_light;   /* back to this afterwards */
    }
    s_light.r = (rgb >> 16) & 0xff;
    s_light.g = (rgb >> 8) & 0xff;
    s_light.b = rgb & 0xff;
    s_light.r2 = (rgb2 >> 16) & 0xff;
    s_light.g2 = (rgb2 >> 8) & 0xff;
    s_light.b2 = rgb2 & 0xff;
    s_light.pct = pct;
    s_light.speed = clampi(speed, 1, 10);
    s_light.fx = rgb == 0 && fx != FX_RAINBOW ? FX_OFF : fx;
    s_light_until_us = seconds > 0 ? esp_timer_get_time() + (int64_t)seconds * 1000000 : 0;
    light_apply();
}

static void light_alert(void)
{
    light_set(0xff0000, 0, 100, FX_BLINK, 5, LIGHT_ALERT_S);
}

static void light_init(void)
{
    led_strip_config_t strip = {
        .strip_gpio_num = CONFIG_MUSE_TOOLS_RGB_LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rmt = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
    };
    if (led_strip_new_rmt_device(&strip, &rmt, &s_strip) != ESP_OK) {
        ESP_LOGW(TAG, "no RGB LED on GPIO %d", CONFIG_MUSE_TOOLS_RGB_LED_GPIO);
        s_strip = NULL;
        return;
    }
    const esp_timer_create_args_t args = { .callback = light_tick_cb, .name = "light" };
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_light_timer));
    s_light.pct = LIGHT_DEFAULT_PCT;
    s_light.speed = 5;
    led_strip_clear(s_strip);
}

static cJSON *light_json(void)
{
    cJSON *j = cJSON_CreateObject();
    char hex[8];
    snprintf(hex, sizeof(hex), "#%02x%02x%02x", s_light.r, s_light.g, s_light.b);
    cJSON_AddStringToObject(j, "color", hex);
    if (s_light.r2 | s_light.g2 | s_light.b2) {
        snprintf(hex, sizeof(hex), "#%02x%02x%02x", s_light.r2, s_light.g2, s_light.b2);
        cJSON_AddStringToObject(j, "color2", hex);
    }
    cJSON_AddNumberToObject(j, "brightness", s_light.pct);
    cJSON_AddStringToObject(j, "effect", FX_NAMES[s_light.fx]);
    cJSON_AddNumberToObject(j, "speed", s_light.speed);
    if (s_light_until_us) {
        cJSON_AddNumberToObject(j, "seconds_left", (double)((s_light_until_us - esp_timer_get_time()) / 1000000));
    }
    return j;
}

static cJSON *cmd_light_set(cJSON *params)
{
    if (!s_strip) {
        return fail("unavailable", "the RGB LED didn't initialise");
    }
    const char *color = str_param(params, "color");
    uint32_t rgb = ((uint32_t)s_light.r << 16) | ((uint32_t)s_light.g << 8) | s_light.b;
    uint32_t rgb2 = ((uint32_t)s_light.r2 << 16) | ((uint32_t)s_light.g2 << 8) | s_light.b2;
    if (color && !parse_color(color, &rgb)) {
        return fail("invalid_params", "color: a name (red, green, blue, white, warm, orange, yellow, purple, pink, cyan, teal, lime, off) or #rrggbb");
    }
    const char *color2 = str_param(params, "color2");
    if (color2 && !parse_color(color2, &rgb2)) {
        return fail("invalid_params", "color2: a name or #rrggbb");
    }
    if (!color && s_light.fx == FX_OFF) {
        rgb = 0xffb060;   /* "turn the light on": warm white */
    }
    int pct = s_light.pct;
    if (int_param(params, "brightness", &pct)) {
        pct = clampi(pct, 1, 100);
    }
    int speed = s_light.speed;
    int_param(params, "speed", &speed);
    light_fx_t fx = s_light.fx == FX_OFF ? FX_SOLID : s_light.fx;
    const char *effect = str_param(params, "effect");
    if (effect) {
        int k;
        for (k = 0; k < FX_COUNT && strcasecmp(effect, FX_NAMES[k]); k++) {
        }
        if (k == FX_COUNT) {
            return fail("invalid_params", "effect: solid, blink, breathe, rainbow, pulse, candle, police, progress, sparkle or off");
        }
        fx = (light_fx_t)k;
        if (fx == FX_OFF) {
            rgb = 0;
        }
        if (fx == FX_POLICE && !color2) {
            rgb2 = 0x0000ff;
            if (!color) {
                rgb = 0xff0000;
            }
        }
        if (fx == FX_CANDLE && !color) {
            rgb = 0xff9030;
        }
    }
    int seconds = 0;
    int_param(params, "seconds", &seconds);
    light_set(rgb, rgb2, pct, fx, speed, clampi(seconds, 0, 24 * 3600));
    return ok_with(light_json());
}
#else
#define HAVE_LIGHT 0
static void light_alert(void) {}
#endif

/* ---- GPIO --------------------------------------------------------------- */

static int s_gpio_allow[16];
static int s_gpio_allow_n;
static bool s_gpio_is_output[49];

static void gpio_allow_init(void)
{
    const char *p = CONFIG_MUSE_TOOLS_GPIO_ALLOW;
    while (*p && s_gpio_allow_n < 16) {
        char *end;
        long v = strtol(p, &end, 10);
        if (end == p) {
            p++;
            continue;
        }
        if (v >= 0 && v <= 48) {
            s_gpio_allow[s_gpio_allow_n++] = (int)v;
        }
        p = end;
    }
}

static bool gpio_allowed(int pin)
{
    for (int i = 0; i < s_gpio_allow_n; i++) {
        if (s_gpio_allow[i] == pin) {
            return true;
        }
    }
    return false;
}

bool gadget_tools_gpio_allowed(int pin)
{
    return gpio_allowed(pin);
}

static void gpio_allow_list(char *out, size_t cap)
{
    out[0] = '\0';
    for (int i = 0; i < s_gpio_allow_n; i++) {
        char num[8];
        snprintf(num, sizeof(num), "%s%d", i ? "," : "", s_gpio_allow[i]);
        strlcat(out, num, cap);
    }
}

static cJSON *gpio_check(cJSON *params, int *pin)
{
    if (!int_param(params, "pin", pin)) {
        return fail("invalid_params", "pin is required");
    }
    if (!gpio_allowed(*pin)) {
        char list[80], msg[128];
        gpio_allow_list(list, sizeof(list));
        snprintf(msg, sizeof(msg), "pin %d is not on the allowed list (%s)", *pin, list);
        return fail("not_allowed", msg);
    }
    return NULL;
}

static void gpio_output(int pin, int level)
{
    if (!s_gpio_is_output[pin]) {
        gpio_reset_pin(pin);
        gpio_set_direction(pin, GPIO_MODE_INPUT_OUTPUT);
        s_gpio_is_output[pin] = true;
    }
    gpio_set_level(pin, level ? 1 : 0);
}

static cJSON *cmd_gpio_set(cJSON *params)
{
    int pin, level;
    cJSON *err = gpio_check(params, &pin);
    if (err) {
        return err;
    }
    if (!int_param(params, "level", &level)) {
        bool b;
        if (bool_param(params, "level", &b)) {
            level = b;
        } else {
            return fail("invalid_params", "level is required (0 or 1)");
        }
    }
    gpio_output(pin, level);
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddNumberToObject(payload, "pin", pin);
    cJSON_AddNumberToObject(payload, "level", gpio_get_level(pin));
    return ok_with(payload);
}

static void gpio_pulse_end(void *arg)
{
    int pin = (int)(intptr_t)arg;
    gpio_set_level(pin, 0);
}

static cJSON *cmd_gpio_pulse(cJSON *params)
{
    int pin, ms = 200;
    cJSON *err = gpio_check(params, &pin);
    if (err) {
        return err;
    }
    int_param(params, "ms", &ms);
    ms = clampi(ms, 10, 10000);
    gpio_output(pin, 1);
    esp_timer_handle_t t;
    const esp_timer_create_args_t args = { .callback = gpio_pulse_end, .arg = (void *)(intptr_t)pin, .name = "pulse" };
    if (esp_timer_create(&args, &t) == ESP_OK) {
        esp_timer_start_once(t, (uint64_t)ms * 1000);   /* one-shot timers free nothing; a few bytes a pulse */
    }
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddNumberToObject(payload, "pin", pin);
    cJSON_AddNumberToObject(payload, "ms", ms);
    return ok_with(payload);
}

static cJSON *cmd_gpio_read(cJSON *params)
{
    int pin;
    cJSON *err = gpio_check(params, &pin);
    if (err) {
        return err;
    }
    if (!s_gpio_is_output[pin]) {
        gpio_reset_pin(pin);
        gpio_set_direction(pin, GPIO_MODE_INPUT);
        gpio_set_pull_mode(pin, GPIO_PULLUP_ONLY);
    }
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddNumberToObject(payload, "pin", pin);
    cJSON_AddNumberToObject(payload, "level", gpio_get_level(pin));
    cJSON_AddBoolToObject(payload, "output", s_gpio_is_output[pin]);
    return ok_with(payload);
}

/* ---- the mic ------------------------------------------------------------ */

static volatile bool s_mic_busy;

typedef struct {
    pending_t req;
    int seconds;
    float threshold;
} mic_args_t;

static void mic_task(void *arg)
{
    mic_args_t *a = arg;
    muse_voice_set_monitor(true);
    vTaskDelay(pdMS_TO_TICKS(300));   /* the meter settles */
    float peak = -100.0f, sum = 0;
    int n = 0;
    for (int i = 0; i < a->seconds * 10; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
        float db = muse_voice_monitor_db();
        if (db > -99.0f) {
            peak = db > peak ? db : peak;
            sum += db;
            n++;
        }
    }
    muse_voice_set_monitor(false);
    cJSON *result;
    if (!n) {
        result = fail("unavailable", "the mic isn't running (asleep on battery, or in a voice turn)");
    } else {
        cJSON *payload = cJSON_CreateObject();
        float avg = sum / n;
        cJSON_AddNumberToObject(payload, "peak_dbfs", (double)((int)(peak * 10) / 10.0));
        cJSON_AddNumberToObject(payload, "average_dbfs", (double)((int)(avg * 10) / 10.0));
        cJSON_AddNumberToObject(payload, "seconds", a->seconds);
        cJSON_AddStringToObject(payload, "room", peak > -18 ? "loud" : peak > -38 ? "normal" : peak > -55 ? "quiet" : "silent");
        result = ok_with(payload);
    }
    pending_send(&a->req, result);
    free(a);
    s_mic_busy = false;
    vTaskDelete(NULL);
}

/* Waits for the room to get loud. */
static void listen_task(void *arg)
{
    mic_args_t *a = arg;
    muse_voice_set_monitor(true);
    vTaskDelay(pdMS_TO_TICKS(500));
    muse_voice_set_monitor(true);   /* resets the peak after the settle */
    int64_t t0 = esp_timer_get_time();
    bool hit = false;
    float peak = -100.0f;
    while ((esp_timer_get_time() - t0) < (int64_t)a->seconds * 1000000) {
        vTaskDelay(pdMS_TO_TICKS(100));
        float db = muse_voice_monitor_db();
        peak = db > peak ? db : peak;
        if (db >= a->threshold) {
            hit = true;
            break;
        }
    }
    muse_voice_set_monitor(false);
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddBoolToObject(payload, "triggered", hit);
    cJSON_AddNumberToObject(payload, "peak_dbfs", (double)((int)(peak * 10) / 10.0));
    cJSON_AddNumberToObject(payload, "threshold_dbfs", (double)a->threshold);
    cJSON_AddNumberToObject(payload, "after_s", (double)((esp_timer_get_time() - t0) / 1000000));
    pending_send(&a->req, ok_with(payload));
    free(a);
    s_mic_busy = false;
    vTaskDelete(NULL);
}

static cJSON *mic_start(cJSON *params, const char *request_id, noise_ctrl_session_generation_t gen, bool listen)
{
    if (muse_voice_resting()) {
        return fail("unavailable", "the mic is resting (asleep on battery)");
    }
    if (s_mic_busy) {
        return fail("busy", "the mic is already measuring or recording");
    }
    mic_args_t *a = calloc(1, sizeof(*a));
    if (!a) {
        return fail("out_of_memory", "failed to allocate");
    }
    pending_init(&a->req, request_id, gen);
    if (listen) {
        a->seconds = 60;
        int_param(params, "seconds", &a->seconds);
        a->seconds = clampi(a->seconds, 1, LISTEN_MAX_S);
        a->threshold = -25.0f;
        float_param(params, "threshold_dbfs", &a->threshold);
        if (a->threshold > -3) {
            a->threshold = -3;
        }
        if (a->threshold < -70) {
            a->threshold = -70;
        }
    } else {
        a->seconds = MIC_DEFAULT_S;
        int_param(params, "seconds", &a->seconds);
        a->seconds = clampi(a->seconds, MIC_MIN_S, MIC_MAX_S);
    }
    s_mic_busy = true;
    if (xTaskCreate(listen ? listen_task : mic_task, listen ? "mic_listen" : "mic_level", 4096, a, 4, NULL) != pdPASS) {
        s_mic_busy = false;
        free(a);
        return fail("out_of_memory", "failed to start");
    }
    return async_result();
}

/* ---- WAV ---------------------------------------------------------------- */

static void wav_header(uint8_t *h, uint32_t frames, int rate)
{
    uint32_t data = frames * 2, riff = 36 + data;
    memcpy(h, "RIFF", 4);
    memcpy(h + 4, &riff, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    uint32_t fmt_len = 16;
    memcpy(h + 16, &fmt_len, 4);
    uint16_t pcm = 1, ch = 1, bits = 16, align = 2;
    uint32_t r = rate, bps = rate * 2;
    memcpy(h + 20, &pcm, 2);
    memcpy(h + 22, &ch, 2);
    memcpy(h + 24, &r, 4);
    memcpy(h + 28, &bps, 4);
    memcpy(h + 32, &align, 2);
    memcpy(h + 34, &bits, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &data, 4);
}

/* Linear resampling of 16-bit PCM (any channel count, mixed to mono) to 16 kHz. Returns frames out. */
static size_t pcm_to_16k_mono(const int16_t *in, size_t frames, int channels, int rate, int16_t *out, size_t max_out)
{
    if (channels < 1 || rate < 4000) {
        return 0;
    }
    uint64_t step = ((uint64_t)rate << 16) / MUSE_AUDIO_RATE;
    uint64_t pos = 0;
    size_t o = 0;
    while ((pos >> 16) + 1 < frames && o < max_out) {
        size_t i = pos >> 16;
        int32_t a = 0, b = 0;
        for (int c = 0; c < channels; c++) {
            a += in[i * channels + c];
            b += in[(i + 1) * channels + c];
        }
        a /= channels;
        b /= channels;
        out[o++] = (int16_t)(a + (((b - a) * (int32_t)(pos & 0xffff)) >> 16));
        pos += step;
    }
    return o;
}

/* A WAV file to 16 kHz mono PCM in a new PSRAM buffer. */
static size_t wav_decode(const uint8_t *d, size_t len, int16_t **out, int *rate_out, int *ch_out)
{
    if (len < 44 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) {
        return 0;
    }
    size_t off = 12;
    int channels = 0, rate = 0, bits = 0, fmt = 0;
    const uint8_t *data = NULL;
    size_t data_len = 0;
    while (off + 8 <= len) {
        uint32_t clen;
        memcpy(&clen, d + off + 4, 4);
        if (!memcmp(d + off, "fmt ", 4) && clen >= 16) {
            uint16_t f, c, b;
            uint32_t r;
            memcpy(&f, d + off + 8, 2);
            memcpy(&c, d + off + 10, 2);
            memcpy(&r, d + off + 12, 4);
            memcpy(&b, d + off + 22, 2);
            fmt = f;
            channels = c;
            rate = r;
            bits = b;
        } else if (!memcmp(d + off, "data", 4)) {
            data = d + off + 8;
            data_len = clen > len - off - 8 ? len - off - 8 : clen;
            break;
        }
        off += 8 + clen + (clen & 1);
    }
    if (!data || (fmt != 1 && fmt != 0xfffe) || bits != 16 || channels < 1 || channels > 2) {
        return 0;
    }
    size_t frames = data_len / (2 * channels);
    size_t want = (size_t)((uint64_t)frames * MUSE_AUDIO_RATE / rate) + 2;
    if (want > AUDIO_MAX_FRAMES) {
        want = AUDIO_MAX_FRAMES;
    }
    int16_t *pcm = big_alloc(want * sizeof(int16_t));
    if (!pcm) {
        return 0;
    }
    size_t n = pcm_to_16k_mono((const int16_t *)data, frames, channels, rate, pcm, want);
    *out = pcm;
    *rate_out = rate;
    *ch_out = channels;
    return n;
}

/* ---- HTTP --------------------------------------------------------------- */

typedef struct {
    uint8_t *buf;
    size_t len, cap;
    bool truncated;
    char content_type[64];
    FILE *file;            /* streaming to the SD card instead */
    size_t file_bytes;
} http_body_t;

static esp_err_t http_event(esp_http_client_event_t *evt)
{
    http_body_t *b = evt->user_data;
    switch (evt->event_id) {
    case HTTP_EVENT_ON_HEADER:
        if (b && evt->header_key && evt->header_value && !strcasecmp(evt->header_key, "Content-Type")) {
            strlcpy(b->content_type, evt->header_value, sizeof(b->content_type));
        }
        break;
    case HTTP_EVENT_ON_DATA: {
        if (!b) {
            break;
        }
        int status = esp_http_client_get_status_code(evt->client);
        if (status >= 300 && status < 400) {
            break;   /* a redirect's own little page, not the file */
        }
        if (b->file) {
            if (b->file_bytes + evt->data_len <= SD_SAVE_MAX) {
                fwrite(evt->data, 1, evt->data_len, b->file);
                b->file_bytes += evt->data_len;
            } else {
                b->truncated = true;
            }
        } else if (b->buf) {
            size_t room = b->cap - b->len;
            size_t n = (size_t)evt->data_len < room ? (size_t)evt->data_len : room;
            memcpy(b->buf + b->len, evt->data, n);
            b->len += n;
            if (n < (size_t)evt->data_len) {
                b->truncated = true;
            }
        }
        break;
    }
    default:
        break;
    }
    return ESP_OK;
}

/* GETs (or sends `body` with `method`) and collects the response. Returns the HTTP status, or <0 on a transport error. */
static int http_do(const char *url, const char *method, cJSON *headers, const char *body, const char *content_type,
                   int timeout_s, http_body_t *out, char *err, size_t errcap)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = timeout_s * 1000,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .event_handler = http_event,
        .user_data = out,
        .user_agent = "MuseGadget/1.0 (esp32)",
        .disable_auto_redirect = false,
        .max_redirection_count = 3,
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) {
        strlcpy(err, "could not create the HTTP client", errcap);
        return -1;
    }
    esp_http_client_method_t m = HTTP_METHOD_GET;
    if (method) {
        if (!strcasecmp(method, "POST")) m = HTTP_METHOD_POST;
        else if (!strcasecmp(method, "PUT")) m = HTTP_METHOD_PUT;
        else if (!strcasecmp(method, "DELETE")) m = HTTP_METHOD_DELETE;
        else if (!strcasecmp(method, "PATCH")) m = HTTP_METHOD_PATCH;
        else if (!strcasecmp(method, "HEAD")) m = HTTP_METHOD_HEAD;
    }
    esp_http_client_set_method(h, m);
    if (headers && cJSON_IsObject(headers)) {
        cJSON *it;
        cJSON_ArrayForEach(it, headers) {
            if (cJSON_IsString(it) && it->string && it->valuestring) {
                esp_http_client_set_header(h, it->string, it->valuestring);
            }
        }
    }
    if (body) {
        esp_http_client_set_header(h, "Content-Type", content_type ? content_type : "application/json");
        esp_http_client_set_post_field(h, body, (int)strlen(body));
    }
    esp_err_t e = esp_http_client_perform(h);
    int status = e == ESP_OK ? esp_http_client_get_status_code(h) : -1;
    if (e != ESP_OK) {
        strlcpy(err, esp_err_to_name(e), errcap);
    }
    esp_http_client_cleanup(h);
    return status;
}

/* Uploads raw bytes (a WAV) with one POST. */
static int http_post_bytes(const char *url, const uint8_t *data, size_t len, const char *content_type, char *err, size_t errcap)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .user_agent = "MuseGadget/1.0 (esp32)",
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) {
        strlcpy(err, "could not create the HTTP client", errcap);
        return -1;
    }
    esp_http_client_set_method(h, HTTP_METHOD_POST);
    esp_http_client_set_header(h, "Content-Type", content_type);
    esp_err_t e = esp_http_client_open(h, (int)len);
    int status = -1;
    if (e == ESP_OK) {
        size_t sent = 0;
        while (sent < len) {
            int n = esp_http_client_write(h, (const char *)data + sent, (int)(len - sent > 4096 ? 4096 : len - sent));
            if (n <= 0) {
                break;
            }
            sent += n;
        }
        if (sent == len) {
            esp_http_client_fetch_headers(h);
            status = esp_http_client_get_status_code(h);
        } else {
            strlcpy(err, "upload stopped early", errcap);
        }
    } else {
        strlcpy(err, esp_err_to_name(e), errcap);
    }
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    return status;
}

typedef struct {
    pending_t req;
    char *url, *method, *body, *content_type;
    cJSON *headers;
    int timeout_s, max_bytes;
} fetch_args_t;

static void fetch_task(void *arg)
{
    fetch_args_t *a = arg;
    http_body_t out = { .cap = a->max_bytes };
    out.buf = big_alloc(out.cap);
    cJSON *result;
    if (!out.buf) {
        result = fail("out_of_memory", "no room for the response");
    } else {
        char err[48] = "";
        int status = http_do(a->url, a->method, a->headers, a->body, a->content_type, a->timeout_s, &out, err, sizeof(err));
        if (status < 0) {
            result = fail("request_failed", err);
        } else {
            cJSON *payload = cJSON_CreateObject();
            cJSON_AddNumberToObject(payload, "status", status);
            cJSON_AddStringToObject(payload, "content_type", out.content_type);
            cJSON_AddNumberToObject(payload, "bytes", (double)out.len);
            cJSON_AddBoolToObject(payload, "truncated", out.truncated);
            if (out.len) {
                add_bytes(payload, "body", out.buf, out.len);
            }
            result = ok_with(payload);
        }
    }
    free(out.buf);
    pending_send(&a->req, result);
    free(a->url);
    free(a->method);
    free(a->body);
    free(a->content_type);
    cJSON_Delete(a->headers);
    free(a);
    vTaskDelete(NULL);
}

static cJSON *cmd_net_fetch(cJSON *params, const char *request_id, noise_ctrl_session_generation_t gen)
{
    const char *url = str_param(params, "url");
    if (!url || (strncmp(url, "http://", 7) && strncmp(url, "https://", 8))) {
        return fail("invalid_params", "url must start with http:// or https://");
    }
    fetch_args_t *a = calloc(1, sizeof(*a));
    if (!a) {
        return fail("out_of_memory", "failed to allocate");
    }
    pending_init(&a->req, request_id, gen);
    a->url = strdup(url);
    const char *s;
    if ((s = str_param(params, "method"))) a->method = strdup(s);
    if ((s = str_param(params, "body"))) a->body = strdup(s);
    if ((s = str_param(params, "content_type"))) a->content_type = strdup(s);
    cJSON *hdr = params ? cJSON_GetObjectItem(params, "headers") : NULL;
    if (cJSON_IsObject(hdr)) {
        a->headers = cJSON_Duplicate(hdr, 1);
    }
    a->timeout_s = 15;
    int_param(params, "timeout_s", &a->timeout_s);
    a->timeout_s = clampi(a->timeout_s, 2, 30);
    a->max_bytes = FETCH_DEFAULT_BYTES;
    int_param(params, "max_bytes", &a->max_bytes);
    a->max_bytes = clampi(a->max_bytes, 256, FETCH_MAX_BYTES);
    if (xTaskCreateWithCaps(fetch_task, "net_fetch", 16384, a, 4, NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        free(a->url);
        free(a->method);
        free(a->body);
        free(a->content_type);
        cJSON_Delete(a->headers);
        free(a);
        return fail("out_of_memory", "failed to start");
    }
    return async_result();
}

/* ---- the SD card -------------------------------------------------------- */

#if CONFIG_MUSE_TOOLS_SD_CLK >= 0
#define HAVE_SD 1
static sdmmc_card_t *s_card;
static bool s_sd_mounted;

static bool sd_mount(void)
{
    if (s_sd_mounted) {
        return true;
    }
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = CONFIG_MUSE_TOOLS_SD_CLK;
    slot.cmd = CONFIG_MUSE_TOOLS_SD_CMD;
    slot.d0 = CONFIG_MUSE_TOOLS_SD_D0;
    slot.d1 = CONFIG_MUSE_TOOLS_SD_D1;
    slot.d2 = CONFIG_MUSE_TOOLS_SD_D2;
    slot.d3 = CONFIG_MUSE_TOOLS_SD_D3;
    slot.width = CONFIG_MUSE_TOOLS_SD_D3 >= 0 ? 4 : 1;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false,
        .max_files = 6,
        .allocation_unit_size = 16 * 1024,
    };
    esp_err_t e = esp_vfs_fat_sdmmc_mount(SD_BASE, &host, &slot, &mount, &s_card);
    if (e != ESP_OK && slot.width == 4) {
        slot.width = 1;
        e = esp_vfs_fat_sdmmc_mount(SD_BASE, &host, &slot, &mount, &s_card);
    }
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "SD mount: %s", esp_err_to_name(e));
        return false;
    }
    s_sd_mounted = true;
    ESP_LOGI(TAG, "SD mounted: %s, %llu MB, %d-bit", s_card->cid.name,
             ((uint64_t)s_card->csd.capacity * s_card->csd.sector_size) >> 20, slot.width);
    return true;
}

static void sd_unmount(void)
{
    if (s_sd_mounted) {
        esp_vfs_fat_sdcard_unmount(SD_BASE, s_card);
        s_sd_mounted = false;
        s_card = NULL;
    }
}

/* "/notes/a.txt" -> "/sd/notes/a.txt"; rejects anything that climbs out. */
static bool sd_path(const char *in, char *out, size_t cap)
{
    if (!in || strstr(in, "..") || strchr(in, '\\')) {
        return false;
    }
    if (!strncmp(in, SD_BASE "/", sizeof(SD_BASE)) || !strcmp(in, SD_BASE)) {
        in += sizeof(SD_BASE) - 1;
    }
    while (*in == '/') {
        in++;
    }
    snprintf(out, cap, SD_BASE "/%s", in);
    size_t n = strlen(out);
    while (n > sizeof(SD_BASE) && out[n - 1] == '/') {
        out[--n] = '\0';
    }
    return true;
}

static cJSON *sd_need(char *path, size_t cap, cJSON *params, const char *key)
{
    if (!sd_mount()) {
        return fail("no_sd", "no SD card is mounted; is one in the slot?");
    }
    const char *p = str_param(params, key);
    if (!sd_path(p ? p : "/", path, cap)) {
        return fail("invalid_params", "path must be inside the card, like /notes/today.txt");
    }
    return NULL;
}

static cJSON *cmd_sd_info(void)
{
    cJSON *payload = cJSON_CreateObject();
    bool ok = sd_mount();
    cJSON_AddBoolToObject(payload, "mounted", ok);
    if (ok) {
        cJSON_AddStringToObject(payload, "card", s_card->cid.name);
        cJSON_AddNumberToObject(payload, "capacity_mb", (double)(((uint64_t)s_card->csd.capacity * s_card->csd.sector_size) >> 20));
        uint64_t total = 0, free_b = 0;
        if (esp_vfs_fat_info(SD_BASE, &total, &free_b) == ESP_OK) {
            cJSON_AddNumberToObject(payload, "total_mb", (double)(total >> 20));
            cJSON_AddNumberToObject(payload, "free_mb", (double)(free_b >> 20));
        }
    }
    return ok_with(payload);
}

static cJSON *cmd_sd_list(cJSON *params)
{
    char path[160];
    cJSON *err = sd_need(path, sizeof(path), params, "path");
    if (err) {
        return err;
    }
    DIR *d = opendir(path);
    if (!d) {
        return fail("not_found", "no such folder");
    }
    cJSON *list = cJSON_CreateArray();
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d)) && n < 100) {
        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "name", e->d_name);
        cJSON_AddBoolToObject(j, "dir", e->d_type == DT_DIR);
        if (e->d_type != DT_DIR) {
            char full[160 + 256 + 2];
            struct stat st;
            snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
            if (stat(full, &st) == 0) {
                cJSON_AddNumberToObject(j, "size", (double)st.st_size);
            }
        }
        cJSON_AddItemToArray(list, j);
        n++;
    }
    closedir(d);
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "path", path + sizeof(SD_BASE) - 1);
    cJSON_AddItemToObject(payload, "entries", list);
    return ok_with(payload);
}

static cJSON *cmd_sd_read(cJSON *params)
{
    char path[160];
    cJSON *err = sd_need(path, sizeof(path), params, "path");
    if (err) {
        return err;
    }
    int offset = 0, max = SD_READ_MAX;
    int_param(params, "offset", &offset);
    int_param(params, "max_bytes", &max);
    max = clampi(max, 1, SD_READ_MAX);
    FILE *f = fopen(path, "rb");
    if (!f) {
        return fail("not_found", "no such file");
    }
    struct stat st;
    stat(path, &st);
    fseek(f, offset, SEEK_SET);
    uint8_t *buf = big_alloc(max);
    size_t n = buf ? fread(buf, 1, max, f) : 0;
    fclose(f);
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddNumberToObject(payload, "size", (double)st.st_size);
    cJSON_AddNumberToObject(payload, "offset", offset);
    cJSON_AddNumberToObject(payload, "bytes", (double)n);
    cJSON_AddBoolToObject(payload, "more", offset + (int)n < st.st_size);
    if (n) {
        add_bytes(payload, "data", buf, n);
    }
    free(buf);
    return ok_with(payload);
}

static void mkdirs_for(const char *path)
{
    char tmp[160];
    strlcpy(tmp, path, sizeof(tmp));
    for (char *p = tmp + sizeof(SD_BASE); *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0775);
            *p = '/';
        }
    }
}

static cJSON *cmd_sd_write(cJSON *params)
{
    char path[160];
    cJSON *err = sd_need(path, sizeof(path), params, "path");
    if (err) {
        return err;
    }
    const char *text = str_param(params, "text");
    const char *b64 = str_param(params, "base64");
    if (!text && !b64) {
        return fail("invalid_params", "text or base64 is required");
    }
    bool append = false;
    bool_param(params, "append", &append);
    mkdirs_for(path);
    FILE *f = fopen(path, append ? "ab" : "wb");
    if (!f) {
        return fail("write_failed", "could not open the file for writing");
    }
    size_t n = 0;
    if (text) {
        n = fwrite(text, 1, strlen(text), f);
    } else {
        size_t cap = strlen(b64) * 3 / 4 + 4, out = 0;
        uint8_t *buf = big_alloc(cap);
        if (buf && mbedtls_base64_decode(buf, cap, &out, (const uint8_t *)b64, strlen(b64)) == 0) {
            n = fwrite(buf, 1, out, f);
        }
        free(buf);
    }
    fclose(f);
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "path", path + sizeof(SD_BASE) - 1);
    cJSON_AddNumberToObject(payload, "bytes", (double)n);
    cJSON_AddBoolToObject(payload, "appended", append);
    return ok_with(payload);
}

static cJSON *cmd_sd_delete(cJSON *params)
{
    char path[160];
    cJSON *err = sd_need(path, sizeof(path), params, "path");
    if (err) {
        return err;
    }
    if (!strcmp(path, SD_BASE)) {
        return fail("invalid_params", "not the whole card");
    }
    struct stat st;
    if (stat(path, &st) != 0) {
        return fail("not_found", "no such file or folder");
    }
    int r = S_ISDIR(st.st_mode) ? rmdir(path) : unlink(path);
    if (r != 0) {
        return fail("delete_failed", S_ISDIR(st.st_mode) ? "folder not empty?" : "could not delete");
    }
    return ok_with(NULL);
}

static cJSON *cmd_sd_mkdir(cJSON *params)
{
    char path[160];
    cJSON *err = sd_need(path, sizeof(path), params, "path");
    if (err) {
        return err;
    }
    mkdirs_for(path);
    if (mkdir(path, 0775) != 0 && errno != EEXIST) {
        return fail("mkdir_failed", "could not create the folder");
    }
    return ok_with(NULL);
}

typedef struct {
    pending_t req;
    char *url;
    char path[160];
} save_args_t;

static void save_task(void *arg)
{
    save_args_t *a = arg;
    cJSON *result;
    mkdirs_for(a->path);
    FILE *f = fopen(a->path, "wb");
    if (!f) {
        result = fail("write_failed", "could not create the file");
    } else {
        http_body_t out = { .file = f };
        char err[48] = "";
        int status = http_do(a->url, "GET", NULL, NULL, NULL, 60, &out, err, sizeof(err));
        fclose(f);
        if (status < 0 || status >= 300) {
            unlink(a->path);
            char msg[96];
            if (status < 0) {
                snprintf(msg, sizeof(msg), "download failed: %s", err);
            } else {
                snprintf(msg, sizeof(msg), "HTTP %d", status);
            }
            result = fail("download_failed", msg);
        } else {
            cJSON *payload = cJSON_CreateObject();
            cJSON_AddStringToObject(payload, "path", a->path + sizeof(SD_BASE) - 1);
            cJSON_AddNumberToObject(payload, "bytes", (double)out.file_bytes);
            cJSON_AddStringToObject(payload, "content_type", out.content_type);
            cJSON_AddBoolToObject(payload, "truncated", out.truncated);
            result = ok_with(payload);
        }
    }
    pending_send(&a->req, result);
    free(a->url);
    free(a);
    vTaskDelete(NULL);
}

static cJSON *cmd_sd_save_url(cJSON *params, const char *request_id, noise_ctrl_session_generation_t gen)
{
    const char *url = str_param(params, "url");
    if (!url || (strncmp(url, "http://", 7) && strncmp(url, "https://", 8))) {
        return fail("invalid_params", "url must start with http:// or https://");
    }
    save_args_t *a = calloc(1, sizeof(*a));
    if (!a) {
        return fail("out_of_memory", "failed to allocate");
    }
    cJSON *err = sd_need(a->path, sizeof(a->path), params, "path");
    if (err || !strcmp(a->path, SD_BASE)) {
        free(a);
        return err ? err : fail("invalid_params", "path must name a file");
    }
    pending_init(&a->req, request_id, gen);
    a->url = strdup(url);
    if (xTaskCreateWithCaps(save_task, "sd_save", 16384, a, 4, NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        free(a->url);
        free(a);
        return fail("out_of_memory", "failed to start");
    }
    return async_result();
}
#else
#define HAVE_SD 0
#endif

/* ---- clips: play a file or URL, record the mic --------------------------- */

typedef struct {
    pending_t req;
    char *url;
    char path[160];
    int times;
} clip_args_t;

/* Reads a whole file from the SD card into PSRAM. */
static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0 || st.st_size > AUDIO_DL_MAX) {
        fclose(f);
        return NULL;
    }
    uint8_t *buf = big_alloc(st.st_size);
    size_t n = buf ? fread(buf, 1, st.st_size, f) : 0;
    fclose(f);
    *len = n;
    return buf;
}

static void clip_task(void *arg)
{
    clip_args_t *a = arg;
    cJSON *result = NULL;
    uint8_t *data = NULL;
    size_t len = 0;
    char err[48] = "";
    if (a->url) {
        http_body_t out = { .cap = AUDIO_DL_MAX };
        out.buf = big_alloc(out.cap);
        if (!out.buf) {
            result = fail("out_of_memory", "no room for the clip");
        } else {
            int status = http_do(a->url, "GET", NULL, NULL, NULL, 60, &out, err, sizeof(err));
            if (status < 0 || status >= 300) {
                char msg[96];
                if (status < 0) {
                    snprintf(msg, sizeof(msg), "download failed: %s", err);
                } else {
                    snprintf(msg, sizeof(msg), "HTTP %d", status);
                }
                result = fail("download_failed", msg);
                free(out.buf);
            } else {
                data = out.buf;
                len = out.len;
                if (out.truncated) {
                    ESP_LOGW(TAG, "clip cut at %u bytes", (unsigned)len);
                }
            }
        }
    } else {
#if HAVE_SD
        if (!sd_mount()) {
            result = fail("no_sd", "no SD card is mounted");
        } else {
            data = read_file(a->path, &len);
            if (!data) {
                result = fail("not_found", "no such file, or it is over 1.2 MB");
            }
        }
#else
        result = fail("unavailable", "no SD card on this board");
#endif
    }
    if (!result) {
        int16_t *pcm = NULL;
        size_t frames = 0;
        const char *format = "";
        int rate = 0, channels = 0;
        if (len > 12 && !memcmp(data, "RIFF", 4)) {
            frames = wav_decode(data, len, &pcm, &rate, &channels);
            format = "wav";
        } else {
            frames = muse_hatch_mp3_decode(data, len, AUDIO_MAX_FRAMES, &pcm, &rate, &channels);
            format = "mp3";
        }
        free(data);
        if (!frames || !pcm) {
            free(pcm);
            result = fail("bad_audio", "not a 16-bit WAV or an MP3 I can decode");
        } else if (!muse_voice_request_pcm(pcm, frames, a->times)) {
            free(pcm);
            result = fail("busy", "a clip is already playing");
        } else {
            cJSON *payload = cJSON_CreateObject();
            cJSON_AddStringToObject(payload, "format", format);
            cJSON_AddNumberToObject(payload, "source_rate_hz", rate);
            cJSON_AddNumberToObject(payload, "source_channels", channels);
            cJSON_AddNumberToObject(payload, "seconds", (double)((int)(frames * 10 / MUSE_AUDIO_RATE) / 10.0));
            cJSON_AddNumberToObject(payload, "times", a->times);
            cJSON_AddBoolToObject(payload, "speaker_on", muse_settings_speaker_on());
            result = ok_with(payload);
        }
    }
    pending_send(&a->req, result);
    free(a->url);
    free(a);
    vTaskDelete(NULL);
}

static cJSON *clip_start(cJSON *params, const char *request_id, noise_ctrl_session_generation_t gen, bool from_url)
{
    clip_args_t *a = calloc(1, sizeof(*a));
    if (!a) {
        return fail("out_of_memory", "failed to allocate");
    }
    if (from_url) {
        const char *url = str_param(params, "url");
        if (!url || (strncmp(url, "http://", 7) && strncmp(url, "https://", 8))) {
            free(a);
            return fail("invalid_params", "url must start with http:// or https://");
        }
        a->url = strdup(url);
    } else {
#if HAVE_SD
        cJSON *err = sd_need(a->path, sizeof(a->path), params, "path");
        if (err) {
            free(a);
            return err;
        }
#else
        free(a);
        return fail("unavailable", "no SD card on this board");
#endif
    }
    pending_init(&a->req, request_id, gen);
    a->times = 1;
    int_param(params, "times", &a->times);
    a->times = clampi(a->times, 1, 5);
    muse_state_set_asleep(false);
    /* TLS handshake plus minimp3's frame scratch (about 12 KB) live on this stack. */
    if (xTaskCreateWithCaps(clip_task, "clip", 32768, a, 4, NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        free(a->url);
        free(a);
        return fail("out_of_memory", "failed to start");
    }
    return async_result();
}

typedef struct {
    pending_t req;
    int seconds;
    char *post_url;
    char path[160];
} record_args_t;

static void record_task(void *arg)
{
    record_args_t *a = arg;
    size_t frames = (size_t)a->seconds * MUSE_AUDIO_RATE;
    frames = (frames + MUSE_AUDIO_CHUNK - 1) / MUSE_AUDIO_CHUNK * MUSE_AUDIO_CHUNK;
    uint8_t *wav = big_alloc(44 + frames * sizeof(int16_t));
    cJSON *result = NULL;
    if (!wav) {
        result = fail("out_of_memory", "no room for the recording");
    } else if (!muse_voice_request_capture((int16_t *)(wav + 44), frames)) {
        result = fail("busy", "the voice task is busy");
    } else {
        size_t got = 0;
        int waited = 0;
        while (!muse_voice_capture_done(&got) && waited < (a->seconds + 15) * 10) {
            vTaskDelay(pdMS_TO_TICKS(100));
            waited++;
        }
        if (!got) {
            result = fail("timeout", "the recording didn't happen (asleep, or a voice turn in progress)");
        } else {
            wav_header(wav, got, MUSE_AUDIO_RATE);
            size_t total = 44 + got * 2;
            cJSON *payload = cJSON_CreateObject();
            cJSON_AddNumberToObject(payload, "seconds", (double)((int)(got * 10 / MUSE_AUDIO_RATE) / 10.0));
            cJSON_AddNumberToObject(payload, "bytes", (double)total);
            cJSON_AddStringToObject(payload, "format", "wav 16 kHz mono 16-bit");
            bool any = false;
#if HAVE_SD
            if (a->path[0]) {
                any = true;
                if (sd_mount()) {
                    mkdirs_for(a->path);
                    FILE *f = fopen(a->path, "wb");
                    if (f) {
                        fwrite(wav, 1, total, f);
                        fclose(f);
                        cJSON_AddStringToObject(payload, "saved", a->path + sizeof(SD_BASE) - 1);
                    } else {
                        cJSON_AddStringToObject(payload, "save_error", "could not write the file");
                    }
                } else {
                    cJSON_AddStringToObject(payload, "save_error", "no SD card");
                }
            }
#endif
            if (a->post_url) {
                any = true;
                char err[48] = "";
                int status = http_post_bytes(a->post_url, wav, total, "audio/wav", err, sizeof(err));
                cJSON_AddNumberToObject(payload, "post_status", status);
                if (status < 0) {
                    cJSON_AddStringToObject(payload, "post_error", err);
                }
            }
            if (!any) {
                cJSON_AddStringToObject(payload, "note", "recorded and discarded: give post_url or path to keep it");
            }
            result = ok_with(payload);
        }
    }
    free(wav);
    pending_send(&a->req, result);
    free(a->post_url);
    free(a);
    s_mic_busy = false;
    vTaskDelete(NULL);
}

static cJSON *cmd_mic_record(cJSON *params, const char *request_id, noise_ctrl_session_generation_t gen)
{
    if (muse_voice_resting()) {
        return fail("unavailable", "the mic is resting (asleep on battery)");
    }
    if (s_mic_busy) {
        return fail("busy", "the mic is already in use");
    }
    record_args_t *a = calloc(1, sizeof(*a));
    if (!a) {
        return fail("out_of_memory", "failed to allocate");
    }
    pending_init(&a->req, request_id, gen);
    a->seconds = 5;
    int_param(params, "seconds", &a->seconds);
    a->seconds = clampi(a->seconds, 1, RECORD_MAX_S);
    const char *url = str_param(params, "post_url");
    if (url) {
        if (strncmp(url, "http://", 7) && strncmp(url, "https://", 8)) {
            free(a);
            return fail("invalid_params", "post_url must start with http:// or https://");
        }
        a->post_url = strdup(url);
    }
#if HAVE_SD
    const char *p = str_param(params, "path");
    if (p && !sd_path(p, a->path, sizeof(a->path))) {
        free(a->post_url);
        free(a);
        return fail("invalid_params", "path must be inside the card");
    }
#endif
    s_mic_busy = true;
    muse_state_set_asleep(false);
    if (xTaskCreateWithCaps(record_task, "mic_record", 16384, a, 4, NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_mic_busy = false;
        free(a->post_url);
        free(a);
        return fail("out_of_memory", "failed to start");
    }
    return async_result();
}

/* ---- QR codes ----------------------------------------------------------- */

#if CONFIG_HOMEHUB_DISPLAY_COMMANDS
typedef struct {
    int w, h;
    uint16_t *pixels;
    int size, scale;
} qr_ctx_t;

static void qr_paint(esp_qrcode_handle_t qr, void *user)
{
    qr_ctx_t *c = user;
    int size = esp_qrcode_get_size(qr);
    int margin = 2;   /* modules of quiet zone */
    int scale = (c->w < c->h ? c->w : c->h) / (size + 2 * margin);
    if (scale < 1) {
        return;
    }
    int px = (size + 2 * margin) * scale;
    int x0 = (c->w - px) / 2, y0 = (c->h - px) / 2;
    for (int i = 0; i < c->w * c->h; i++) {
        c->pixels[i] = 0xffff;   /* white page, swapped or not it's white */
    }
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            if (!esp_qrcode_get_module(qr, x, y)) {
                continue;
            }
            int sx = x0 + (x + margin) * scale, sy = y0 + (y + margin) * scale;
            for (int dy = 0; dy < scale; dy++) {
                uint16_t *row = c->pixels + (sy + dy) * c->w + sx;
                for (int dx = 0; dx < scale; dx++) {
                    row[dx] = 0x0000;
                }
            }
        }
    }
    c->size = size;
    c->scale = scale;
}

static cJSON *cmd_draw_qr(cJSON *params)
{
    const char *text = str_param(params, "text");
    if (!text) {
        return fail("invalid_params", "text is required");
    }
    if (strlen(text) > 300) {
        return fail("invalid_params", "text is too long for a QR code the screen can show (300 characters)");
    }
    int w, h;
    if (!led_status_display_info(&w, &h)) {
        return fail("unavailable", "no screen for pictures");
    }
    qr_ctx_t c = { .w = w, .h = h };
    c.pixels = heap_caps_malloc((size_t)w * h * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!c.pixels) {
        return fail("out_of_memory", "no room for the picture");
    }
    esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
    cfg.display_func_with_cb = qr_paint;
    cfg.user_data = &c;
    cfg.qrcode_ecc_level = ESP_QRCODE_ECC_MED;
    cfg.max_qrcode_version = 10;
    esp_err_t e = esp_qrcode_generate(&cfg, text);
    if (e != ESP_OK || !c.size) {
        free(c.pixels);
        return fail("qr_failed", "could not encode that text (too long for version 10?)");
    }
    /* The image buffer is RGB565 with the high byte first on the wire; black
     * and white are the same either way. */
    muse_state_set_asleep(false);
    bool drawn = true;
    for (int y = 0; y < h && drawn; y += 40) {
        int rows = h - y < 40 ? h - y : 40;
        drawn = led_status_draw_rect(0, y, w, rows, c.pixels + (size_t)y * w);
    }
    led_status_draw_done();
    free(c.pixels);
    if (!drawn) {
        return fail("draw_failed", "the screen refused the picture");
    }
    const char *caption = str_param(params, "caption");
    if (caption) {
        muse_state_set_caption("%s", caption);
    }
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddNumberToObject(payload, "modules", c.size);
    cJSON_AddNumberToObject(payload, "pixels_per_module", c.scale);
    cJSON_AddStringToObject(payload, "note", "stays until screen.clear, display.show_animation, a tap or the talk button");
    return ok_with(payload);
}
#endif

/* ---- a menu on the screen ---------------------------------------------- */

typedef struct {
    pending_t req;
    int n;
    char items[MUSE_PROMPT_MAX_ITEMS][40];
    int64_t shown_us;
} menu_ctx_t;

static menu_ctx_t *s_menu;

static void menu_done(int choice, void *user)
{
    menu_ctx_t *m = user;
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddNumberToObject(payload, "choice", choice);
    cJSON_AddBoolToObject(payload, "timed_out", choice < 0);
    if (choice >= 0 && choice < m->n) {
        cJSON_AddStringToObject(payload, "label", m->items[choice]);
    }
    cJSON_AddNumberToObject(payload, "after_s", (double)((esp_timer_get_time() - m->shown_us) / 1000000));
    pending_send(&m->req, ok_with(payload));
    if (choice >= 0) {
        muse_voice_request_sound(MUSE_SOUND_TICK, 1);
    }
    s_menu = NULL;
    free(m);
}

static cJSON *cmd_menu(cJSON *params, const char *request_id, noise_ctrl_session_generation_t gen)
{
    cJSON *items = params ? cJSON_GetObjectItem(params, "items") : NULL;
    if (!cJSON_IsArray(items) || cJSON_GetArraySize(items) < 1 || cJSON_GetArraySize(items) > MUSE_PROMPT_MAX_ITEMS) {
        return fail("invalid_params", "items: an array of 1 to 8 short strings");
    }
    if (muse_prompt_active()) {
        return fail("busy", "a menu is already on the screen");
    }
    menu_ctx_t *m = calloc(1, sizeof(*m));
    if (!m) {
        return fail("out_of_memory", "failed to allocate");
    }
    pending_init(&m->req, request_id, gen);
    const char *ptrs[MUSE_PROMPT_MAX_ITEMS];
    cJSON *it;
    cJSON_ArrayForEach(it, items) {
        const char *s = cJSON_IsString(it) && it->valuestring ? it->valuestring : "?";
        strlcpy(m->items[m->n], s, sizeof(m->items[0]));
        ptrs[m->n] = m->items[m->n];
        m->n++;
    }
    int timeout_s = 60;
    int_param(params, "timeout_s", &timeout_s);
    timeout_s = clampi(timeout_s, 5, 600);
    m->shown_us = esp_timer_get_time();
    s_menu = m;
    if (!muse_prompt_show(str_param(params, "title"), ptrs, m->n, timeout_s * 1000, menu_done, m)) {
        s_menu = NULL;
        free(m);
        return fail("unavailable", "the screen isn't ready for a menu");
    }
    bool chime = true;
    bool_param(params, "chime", &chime);
    if (chime) {
        muse_voice_request_sound(MUSE_SOUND_CHIME, 1);
    }
    return async_result();
}

/* ---- Wi-Fi -------------------------------------------------------------- */

static void wifi_scan_task(void *arg)
{
    pending_t *req = arg;
    cJSON *result;
    if (muse_wifi_scan() != ESP_OK) {
        result = fail("scan_failed", "could not start a scan (Wi-Fi off?)");
    } else {
        for (int i = 0; i < 120 && muse_wifi_scanning(); i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        muse_wifi_ap_t aps[20];
        uint32_t gen = 0;
        int n = muse_wifi_scan_results(aps, 20, &gen);
        cJSON *list = cJSON_CreateArray();
        for (int i = 0; i < n; i++) {
            cJSON *j = cJSON_CreateObject();
            cJSON_AddStringToObject(j, "ssid", aps[i].ssid);
            cJSON_AddNumberToObject(j, "rssi_dbm", aps[i].rssi);
            cJSON_AddBoolToObject(j, "secure", aps[i].secure);
            cJSON_AddItemToArray(list, j);
        }
        cJSON *payload = cJSON_CreateObject();
        cJSON_AddItemToObject(payload, "networks", list);
        cJSON_AddNumberToObject(payload, "count", n);
        result = ok_with(payload);
    }
    pending_send(req, result);
    free(req);
    vTaskDelete(NULL);
}

static cJSON *cmd_wifi_scan(const char *request_id, noise_ctrl_session_generation_t gen)
{
    pending_t *req = calloc(1, sizeof(*req));
    if (!req) {
        return fail("out_of_memory", "failed to allocate");
    }
    pending_init(req, request_id, gen);
    if (xTaskCreate(wifi_scan_task, "wifi_scan", 4096, req, 4, NULL) != pdPASS) {
        free(req);
        return fail("out_of_memory", "failed to start");
    }
    return async_result();
}

/* ---- settings and status ----------------------------------------------- */

static cJSON *settings_json(void)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "volume", muse_settings_volume());
    cJSON_AddBoolToObject(j, "speaker_on", muse_settings_speaker_on());
    cJSON_AddNumberToObject(j, "brightness", muse_settings_brightness());
    cJSON_AddNumberToObject(j, "sleep_s", muse_settings_sleep_s());
    return j;
}

static cJSON *cmd_configure(cJSON *params)
{
    int v;
    bool b;
    int changed = 0;
    if (int_param(params, "volume", &v)) {
        muse_settings_set_volume(clampi(v, 0, 100));
        changed++;
    }
    if (bool_param(params, "speaker_on", &b)) {
        muse_settings_set_speaker_on(b);
        changed++;
    }
    if (int_param(params, "brightness", &v)) {
        muse_settings_set_brightness(clampi(v, 10, 100));
        changed++;
    }
    if (int_param(params, "sleep_s", &v)) {
        muse_settings_set_sleep_s(clampi(v, 0, 24 * 3600));
        changed++;
    }
    cJSON *payload = settings_json();
    cJSON_AddNumberToObject(payload, "changed", changed);
    return ok_with(payload);
}

static cJSON *cmd_sleep(cJSON *params)
{
    bool on = true;
    bool_param(params, "on", &on);
    if (on) {
        text_clear();
        muse_prompt_dismiss();
    }
    muse_state_set_asleep(on);
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddBoolToObject(payload, "asleep", muse_state_asleep());
    return ok_with(payload);
}

#if SOC_TEMP_SENSOR_SUPPORTED
static temperature_sensor_handle_t s_tsens;
#endif

static cJSON *cmd_status(void)
{
    cJSON *p = cJSON_CreateObject();
    cJSON_AddNumberToObject(p, "uptime_s", (double)(esp_timer_get_time() / 1000000));

    muse_power_t power = muse_state_power();
    cJSON *pw = cJSON_CreateObject();
    if (power.battery_pct >= 0) {
        cJSON_AddNumberToObject(pw, "battery_pct", power.battery_pct);
    } else {
        cJSON_AddNullToObject(pw, "battery_pct");
    }
    if (power.battery_mv > 0) {
        cJSON_AddNumberToObject(pw, "battery_v", power.battery_mv / 1000.0);
    }
    cJSON_AddBoolToObject(pw, "usb", power.usb);
    cJSON_AddBoolToObject(pw, "charging", power.charging);
    cJSON_AddItemToObject(p, "power", pw);

    muse_wifi_status_t wifi;
    muse_wifi_status(&wifi);
    cJSON *w = cJSON_CreateObject();
    cJSON_AddBoolToObject(w, "connected", wifi.state == MUSE_WIFI_CONNECTED);
    cJSON_AddStringToObject(w, "ssid", wifi.ssid);
    cJSON_AddStringToObject(w, "ip", wifi.ip);
    cJSON_AddNumberToObject(w, "rssi_dbm", wifi.rssi);
    cJSON_AddItemToObject(p, "wifi", w);

    cJSON *mem = cJSON_CreateObject();
    cJSON_AddNumberToObject(mem, "free_internal_kb", (double)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    cJSON_AddNumberToObject(mem, "free_psram_kb", (double)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    cJSON_AddItemToObject(p, "memory", mem);

#if SOC_TEMP_SENSOR_SUPPORTED
    float celsius;
    if (s_tsens && temperature_sensor_get_celsius(s_tsens, &celsius) == ESP_OK) {
        cJSON_AddNumberToObject(p, "chip_temp_c", (double)((int)(celsius * 10) / 10.0));
    }
#endif

    static const char *const MODES[MUSE_MODE_COUNT] = { "booting", "idle", "listening", "thinking", "speaking", "error", "off" };
    float secs = 0;
    muse_mode_t mode = muse_state_mode(&secs);
    cJSON *face = cJSON_CreateObject();
    cJSON_AddStringToObject(face, "mode", (int)mode >= 0 && mode < MUSE_MODE_COUNT ? MODES[mode] : "?");
    cJSON_AddBoolToObject(face, "asleep", muse_state_asleep());
    cJSON_AddNumberToObject(face, "idle_s", (double)(int)muse_state_idle_secs());
    cJSON_AddBoolToObject(face, "message_showing", s_text_shown_us != 0);
    cJSON_AddBoolToObject(face, "menu_showing", muse_prompt_active());
    cJSON_AddItemToObject(p, "screen", face);

    cJSON_AddItemToObject(p, "settings", settings_json());
    cJSON_AddItemToObject(p, "timers", timers_list_json());
#if HAVE_LIGHT
    if (s_strip) {
        cJSON_AddItemToObject(p, "light", light_json());
    }
#endif
#if HAVE_SD
    cJSON_AddBoolToObject(p, "sd_mounted", s_sd_mounted);
#endif
    cJSON_AddBoolToObject(p, "mic_busy", s_mic_busy);
    char allow[80];
    gpio_allow_list(allow, sizeof(allow));
    cJSON_AddStringToObject(p, "gpio_allowed", allow);
    bool set = clock_set();
    cJSON_AddBoolToObject(p, "clock_set", set);
    if (set) {
        char buf[32];
        local_time_str(time(NULL), buf, sizeof(buf));
        cJSON_AddStringToObject(p, "local_time", buf);
    }
    return ok_with(p);
}

/* ---- screen, sound, face ----------------------------------------------- */

static cJSON *cmd_show_text(cJSON *params)
{
    const char *text = str_param(params, "text");
    if (!text) {
        return fail("invalid_params", "text is required");
    }
    int seconds = TEXT_DEFAULT_S;
    int_param(params, "seconds", &seconds);
    seconds = clampi(seconds, 0, 24 * 3600);
    bool chime = true;
    bool_param(params, "chime", &chime);
    char buf[MUSE_CAPTION_MAX];
    strlcpy(buf, text, sizeof(buf));
    if (!text_show(buf, seconds)) {
        return fail("busy", "Muse is in a voice turn; try again in a few seconds");
    }
    if (chime) {
        muse_voice_request_sound(MUSE_SOUND_CHIME, 1);
    }
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddNumberToObject(payload, "seconds", seconds);
    cJSON_AddNumberToObject(payload, "chars", (double)strlen(buf));
    if (strlen(text) >= sizeof(buf)) {
        cJSON_AddStringToObject(payload, "note", "text was cut to fit the screen");
    }
    return ok_with(payload);
}

static cJSON *cmd_clear(void)
{
    s_text_pending[0] = '\0';
    text_clear();
    muse_prompt_dismiss();
    gadget_more_screen_clear();
#if CONFIG_HOMEHUB_DISPLAY_COMMANDS
    led_status_show_animation();   /* a picture from display.draw_url or a QR code goes too */
#endif
    return ok_with(NULL);
}

static cJSON *cmd_sound(cJSON *params)
{
    const char *name = str_param(params, "name");
    if (!name) {
        return fail("invalid_params", "name is required");
    }
    int id = muse_sound_by_name(name);
    if (id < 0) {
        return fail("invalid_params", "unknown sound");
    }
    int times = 1;
    int_param(params, "times", &times);
    times = clampi(times, 1, 10);
    if (muse_voice_resting()) {
        muse_state_set_asleep(false);
    }
    if (!muse_voice_request_sound(id, times)) {
        return fail("busy", "a sound is already queued");
    }
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "sound", muse_sound_name((muse_sound_t)id));
    cJSON_AddNumberToObject(payload, "times", times);
    cJSON_AddBoolToObject(payload, "speaker_on", muse_settings_speaker_on());
    return ok_with(payload);
}

static cJSON *cmd_happy(void)
{
    muse_state_set_asleep(false);
    muse_state_make_happy();
    muse_state_nudge();
    return ok_with(NULL);
}

/* ---- the command table -------------------------------------------------- */

void gadget_tools_add_commands(cJSON *commands)
{
    cJSON *req, *opt;

    req = cJSON_CreateObject();
    cJSON_AddItemToObject(req, "text", param_spec("string", "What to show; about 250 characters fit. Plain ASCII, newlines allowed."));
    opt = cJSON_CreateObject();
    cJSON_AddItemToObject(opt, "seconds", param_spec("integer", "How long it stays, default 20; 0 keeps it until screen.clear or the talk button."));
    cJSON_AddItemToObject(opt, "chime", param_spec("boolean", "Play a chime with it; default true."));
    add_command(commands, "screen.show_text",
                "Show a message on the gadget's screen, in place of the avatar, and wake the screen. "
                "Use it for reminders, notifications, answers to look at, or anything the user asked to put on the gadget.",
                req, opt, 0);

    add_command(commands, "screen.clear", "Take a message, menu, QR code or picture off the screen and bring the avatar back.", NULL, NULL, 0);

    req = cJSON_CreateObject();
    cJSON_AddItemToObject(req, "items", param_spec("array", "1 to 8 short button labels (strings), under 30 characters each."));
    opt = cJSON_CreateObject();
    cJSON_AddItemToObject(opt, "title", param_spec("string", "The question above the buttons, short."));
    cJSON_AddItemToObject(opt, "timeout_s", param_spec("integer", "How long to wait for a tap, 5 to 600; default 60."));
    cJSON_AddItemToObject(opt, "chime", param_spec("boolean", "Chime when it appears; default true."));
    add_command(commands, "screen.menu",
                "Ask the user something on the gadget's touch screen: a title and tappable buttons. Replies with the tapped "
                "choice (index and label) or timed_out. Use it for yes/no, pick-one, snooze/dismiss, confirmations.", req, opt, 610000);

#if CONFIG_HOMEHUB_DISPLAY_COMMANDS
    req = cJSON_CreateObject();
    cJSON_AddItemToObject(req, "text", param_spec("string", "What to encode, up to about 300 characters: a URL, WIFI:T:WPA;S:ssid;P:pass;; , an otpauth:// URI, plain text."));
    opt = cJSON_CreateObject();
    cJSON_AddItemToObject(opt, "caption", param_spec("string", "A short line shown under it."));
    add_command(commands, "display.draw_qr",
                "Show a QR code on the gadget's screen, as large as fits, for the user to scan with a phone. "
                "Stays until screen.clear or a tap.", req, opt, 0);
#endif

    req = cJSON_CreateObject();
    cJSON_AddItemToObject(req, "name", param_spec("string", "chime, beep, success, error, alarm, doorbell, tick or siren."));
    opt = cJSON_CreateObject();
    cJSON_AddItemToObject(opt, "times", param_spec("integer", "Repeats, 1 to 10; default 1."));
    add_command(commands, "sound.play",
                "Play a short synthesised sound on the gadget's speaker (it has no speech of its own). "
                "Honours the user's speaker switch and volume.", req, opt, 0);

    req = cJSON_CreateObject();
    cJSON_AddItemToObject(req, "url", param_spec("string", "http(s) URL of an MP3 or a 16-bit WAV, up to 1.2 MB / 40 s."));
    opt = cJSON_CreateObject();
    cJSON_AddItemToObject(opt, "times", param_spec("integer", "Repeats, 1 to 5; default 1."));
    add_command(commands, "sound.play_url",
                "Download a sound clip and play it on the gadget's speaker: a custom doorbell, a voice line, a jingle, a short song. "
                "The avatar's mouth moves with it. Replies when the clip is queued, with its length.", req, opt, 90000);

#if HAVE_SD
    req = cJSON_CreateObject();
    cJSON_AddItemToObject(req, "path", param_spec("string", "File on the SD card, e.g. /sounds/hello.mp3."));
    opt = cJSON_CreateObject();
    cJSON_AddItemToObject(opt, "times", param_spec("integer", "Repeats, 1 to 5; default 1."));
    add_command(commands, "sound.play_file", "Play an MP3 or WAV from the gadget's SD card (saved there earlier with sd.save_url).", req, opt, 30000);
#endif

    opt = cJSON_CreateObject();
    cJSON_AddItemToObject(opt, "seconds", param_spec("integer", "Length in seconds (1 to 86400). Give seconds or minutes, or both."));
    cJSON_AddItemToObject(opt, "minutes", param_spec("integer", "Length in minutes."));
    cJSON_AddItemToObject(opt, "label", param_spec("string", "Short name shown while it counts down, up to 31 characters, e.g. EPOXY or PIZZA."));
    cJSON_AddItemToObject(opt, "sound", param_spec("string", "Sound when it ends; default alarm. See sound.play."));
    add_command(commands, "timer.set",
                "Start a countdown on the gadget. The screen shows the label and time left under the avatar; when it ends "
                "the gadget sounds the alarm, blinks its light red and shows TIMER DONE for a minute. Up to 8 at once. "
                "Returns the timer id and when it ends. light.set effect progress colours the light by how far along the soonest timer is.", NULL, opt, 0);

    opt = cJSON_CreateObject();
    cJSON_AddItemToObject(opt, "id", param_spec("integer", "The timer to cancel; without it, all timers are cancelled."));
    add_command(commands, "timer.cancel", "Cancel a countdown or alarm (or all of them).", NULL, opt, 0);
    add_command(commands, "timer.list", "The running countdowns and alarms with their time left.", NULL, NULL, 0);

    req = cJSON_CreateObject();
    cJSON_AddItemToObject(req, "time", param_spec("string", "Local clock time as HH:MM, 24 hour. If already past today, it is set for tomorrow."));
    opt = cJSON_CreateObject();
    cJSON_AddItemToObject(opt, "label", param_spec("string", "Short name, default ALARM."));
    cJSON_AddItemToObject(opt, "sound", param_spec("string", "Sound when it goes off; default alarm."));
    add_command(commands, "alarm.set",
                "Set an alarm for a clock time on the gadget (it keeps local time over the network). "
                "Works like timer.set: shown while waiting, alarm sound, red blink and TIMER DONE when it goes off.", req, opt, 0);

    add_command(commands, "clock.now", "The gadget's local date and time, time zone, whether its clock is synced, and uptime.", NULL, NULL, 0);

#if HAVE_LIGHT
    opt = cJSON_CreateObject();
    cJSON_AddItemToObject(opt, "color", param_spec("string", "red, green, blue, white, warm, orange, yellow, purple, pink, cyan, teal, lime, off, or #rrggbb."));
    cJSON_AddItemToObject(opt, "color2", param_spec("string", "Second colour for police and sparkle."));
    cJSON_AddItemToObject(opt, "brightness", param_spec("integer", "1 to 100; default stays as it is (40 at start)."));
    cJSON_AddItemToObject(opt, "effect", param_spec("string", "solid (default), blink, breathe, rainbow, pulse (heartbeat), candle, police, progress (tracks the soonest timer green to red), sparkle, off."));
    cJSON_AddItemToObject(opt, "speed", param_spec("integer", "1 slow to 10 fast; default 5."));
    cJSON_AddItemToObject(opt, "seconds", param_spec("integer", "Go back to the previous light after this long; default 0 = stay."));
    add_command(commands, "light.set",
                "Set the gadget's small RGB light: a colour, brightness, effect and speed, optionally for a while. "
                "Use it as a status or mood light, a timer progress bar, or to catch the user's eye.", NULL, opt, 0);
#endif

    opt = cJSON_CreateObject();
    cJSON_AddItemToObject(opt, "seconds", param_spec("integer", "How long to listen, 1 to 10; default 3."));
    add_command(commands, "mic.level",
                "Measure how loud the room is at the gadget's microphone: peak and average in dBFS and a word "
                "(silent, quiet, normal, loud). Levels only; nothing is recorded or transcribed.", NULL, opt, 15000);

    opt = cJSON_CreateObject();
    cJSON_AddItemToObject(opt, "seconds", param_spec("integer", "How long to wait, 1 to 600; default 60."));
    cJSON_AddItemToObject(opt, "threshold_dbfs", param_spec("number", "Loudness that counts, -70 quiet to -3 very loud; default -25 (a door slam, a shout, a bark)."));
    add_command(commands, "mic.listen",
                "Wait for a loud sound in the room, then reply at once (or when the time runs out): triggered true/false, the peak "
                "level and how long it took. Levels only, no recording. For 'tell me if the dog barks' or 'when the kettle clicks'.",
                NULL, opt, 620000);

    opt = cJSON_CreateObject();
    cJSON_AddItemToObject(opt, "seconds", param_spec("integer", "Length, 1 to 20; default 5."));
    cJSON_AddItemToObject(opt, "post_url", param_spec("string", "http(s) URL to POST the WAV to (Content-Type audio/wav)."));
#if HAVE_SD
    cJSON_AddItemToObject(opt, "path", param_spec("string", "Save it on the SD card at this path, e.g. /memos/1.wav."));
#endif
    add_command(commands, "mic.record",
                "Record a voice memo from the gadget's microphone (16 kHz mono WAV) and send it to a URL and/or keep it on the card. "
                "Only when the user asked for it. The screen shows RECORDING while it runs.", NULL, opt, 60000);

    add_command(commands, "face.happy", "Make the avatar on the gadget's screen smile for a moment.", NULL, NULL, 0);

    req = cJSON_CreateObject();
    cJSON_AddItemToObject(req, "url", param_spec("string", "http:// or https:// URL. Local addresses on the home network work too."));
    opt = cJSON_CreateObject();
    cJSON_AddItemToObject(opt, "method", param_spec("string", "GET (default), POST, PUT, PATCH, DELETE, HEAD."));
    cJSON_AddItemToObject(opt, "body", param_spec("string", "Request body for POST/PUT/PATCH."));
    cJSON_AddItemToObject(opt, "content_type", param_spec("string", "Body type; default application/json."));
    cJSON_AddItemToObject(opt, "headers", param_spec("object", "Extra request headers as name: value."));
    cJSON_AddItemToObject(opt, "timeout_s", param_spec("integer", "2 to 30; default 15."));
    cJSON_AddItemToObject(opt, "max_bytes", param_spec("integer", "Most of the response to return, 256 to 16384; default 4096."));
    add_command(commands, "net.fetch",
                "The gadget makes an HTTP request itself from inside the home network and returns status, content type and body "
                "(text, or base64 for binary). Webhooks, Home Assistant, local devices, public APIs.", req, opt, 40000);

    add_command(commands, "wifi.scan", "Wi-Fi networks the gadget can see right now, with signal strength.", NULL, NULL, 20000);

    if (s_gpio_allow_n) {
        char allow[80], desc[200];
        gpio_allow_list(allow, sizeof(allow));
        req = cJSON_CreateObject();
        cJSON_AddItemToObject(req, "pin", param_spec("integer", "GPIO number."));
        cJSON_AddItemToObject(req, "level", param_spec("integer", "1 high, 0 low."));
        snprintf(desc, sizeof(desc), "Drive a GPIO pin high or low (a relay, a buzzer, an LED on a proper carrier board). Allowed pins: %s.", allow);
        add_command(commands, "gpio.set", desc, req, NULL, 0);
        req = cJSON_CreateObject();
        cJSON_AddItemToObject(req, "pin", param_spec("integer", "GPIO number."));
        opt = cJSON_CreateObject();
        cJSON_AddItemToObject(opt, "ms", param_spec("integer", "Pulse length, 10 to 10000; default 200."));
        snprintf(desc, sizeof(desc), "Pulse a GPIO pin high for a moment, then low: a doorbell, a garage button. Allowed pins: %s.", allow);
        add_command(commands, "gpio.pulse", desc, req, opt, 0);
        req = cJSON_CreateObject();
        cJSON_AddItemToObject(req, "pin", param_spec("integer", "GPIO number."));
        snprintf(desc, sizeof(desc), "Read a GPIO pin (pulled up when it isn't an output): a switch, a door sensor. Allowed pins: %s.", allow);
        add_command(commands, "gpio.read", desc, req, NULL, 0);
    }

#if HAVE_SD
    add_command(commands, "sd.info", "Whether an SD card is in, its size and free space.", NULL, NULL, 0);
    opt = cJSON_CreateObject();
    cJSON_AddItemToObject(opt, "path", param_spec("string", "Folder, default /."));
    add_command(commands, "sd.list", "List a folder on the gadget's SD card.", NULL, opt, 0);
    req = cJSON_CreateObject();
    cJSON_AddItemToObject(req, "path", param_spec("string", "File to read."));
    opt = cJSON_CreateObject();
    cJSON_AddItemToObject(opt, "offset", param_spec("integer", "Start byte; default 0."));
    cJSON_AddItemToObject(opt, "max_bytes", param_spec("integer", "Up to 16384; default 16384. 'more' says if there is more."));
    add_command(commands, "sd.read", "Read a file from the SD card (text, or base64 for binary).", req, opt, 0);
    req = cJSON_CreateObject();
    cJSON_AddItemToObject(req, "path", param_spec("string", "File to write; folders are created."));
    opt = cJSON_CreateObject();
    cJSON_AddItemToObject(opt, "text", param_spec("string", "Text to write."));
    cJSON_AddItemToObject(opt, "base64", param_spec("string", "Binary to write, base64."));
    cJSON_AddItemToObject(opt, "append", param_spec("boolean", "Add to the end instead of replacing; default false."));
    add_command(commands, "sd.write", "Write a file on the SD card: notes, logs, lists, settings, anything to keep.", req, opt, 0);
    req = cJSON_CreateObject();
    cJSON_AddItemToObject(req, "path", param_spec("string", "File or empty folder."));
    add_command(commands, "sd.delete", "Delete a file or an empty folder on the SD card.", req, NULL, 0);
    req = cJSON_CreateObject();
    cJSON_AddItemToObject(req, "path", param_spec("string", "Folder to create."));
    add_command(commands, "sd.mkdir", "Create a folder on the SD card.", req, NULL, 0);
    req = cJSON_CreateObject();
    cJSON_AddItemToObject(req, "url", param_spec("string", "http(s) URL to download."));
    cJSON_AddItemToObject(req, "path", param_spec("string", "Where to save it on the card, e.g. /sounds/bonzi-hello.mp3."));
    add_command(commands, "sd.save_url", "Download a file (up to 8 MB) onto the SD card: sounds for sound.play_file, pictures, documents.", req, NULL, 120000);
#endif

    opt = cJSON_CreateObject();
    cJSON_AddItemToObject(opt, "on", param_spec("boolean", "true sleeps (screen dark), false wakes; default true."));
    add_command(commands, "gadget.sleep", "Put the gadget's screen to sleep or wake it.", NULL, opt, 0);

    opt = cJSON_CreateObject();
    cJSON_AddItemToObject(opt, "volume", param_spec("integer", "Speaker volume 0 to 100."));
    cJSON_AddItemToObject(opt, "speaker_on", param_spec("boolean", "false mutes sounds (messages still show)."));
    cJSON_AddItemToObject(opt, "brightness", param_spec("integer", "Screen brightness 10 to 100."));
    cJSON_AddItemToObject(opt, "sleep_s", param_spec("integer", "Screen sleeps after this many idle seconds; 0 never."));
    add_command(commands, "gadget.configure",
                "Change the gadget's settings (kept across restarts). Without parameters, reports them.", NULL, opt, 0);

    add_command(commands, "gadget.status",
                "Everything about the gadget right now: uptime, power and battery, Wi-Fi, free memory, chip temperature, "
                "what the screen is doing, settings, running timers, the light, the SD card, the clock.", NULL, NULL, 0);
    gadget_more_add_commands(commands);
#if CONFIG_MUSE_PET
    gadget_pet_add_commands(commands);
#endif
}

cJSON *gadget_tools_command(const char *command, cJSON *params, const char *request_id,
                            noise_ctrl_session_generation_t gen)
{
    if (!strcmp(command, "screen.show_text")) return cmd_show_text(params);
    if (!strcmp(command, "screen.clear")) return cmd_clear();
    if (!strcmp(command, "screen.menu")) return cmd_menu(params, request_id, gen);
#if CONFIG_HOMEHUB_DISPLAY_COMMANDS
    if (!strcmp(command, "display.draw_qr")) return cmd_draw_qr(params);
#endif
    if (!strcmp(command, "sound.play")) return cmd_sound(params);
    if (!strcmp(command, "sound.play_url")) return clip_start(params, request_id, gen, true);
#if HAVE_SD
    if (!strcmp(command, "sound.play_file")) return clip_start(params, request_id, gen, false);
#endif
    if (!strcmp(command, "timer.set")) return cmd_timer_set(params);
    if (!strcmp(command, "timer.cancel")) return cmd_timer_cancel(params);
    if (!strcmp(command, "timer.list")) return cmd_timer_list();
    if (!strcmp(command, "alarm.set")) return cmd_alarm_set(params);
    if (!strcmp(command, "clock.now")) return cmd_clock_now();
#if HAVE_LIGHT
    if (!strcmp(command, "light.set")) return cmd_light_set(params);
#endif
    if (!strcmp(command, "mic.level")) return mic_start(params, request_id, gen, false);
    if (!strcmp(command, "mic.listen")) return mic_start(params, request_id, gen, true);
    if (!strcmp(command, "mic.record")) return cmd_mic_record(params, request_id, gen);
    if (!strcmp(command, "face.happy")) return cmd_happy();
    if (!strcmp(command, "net.fetch")) return cmd_net_fetch(params, request_id, gen);
    if (!strcmp(command, "wifi.scan")) return cmd_wifi_scan(request_id, gen);
    if (!strcmp(command, "gpio.set")) return cmd_gpio_set(params);
    if (!strcmp(command, "gpio.pulse")) return cmd_gpio_pulse(params);
    if (!strcmp(command, "gpio.read")) return cmd_gpio_read(params);
#if HAVE_SD
    if (!strcmp(command, "sd.info")) return cmd_sd_info();
    if (!strcmp(command, "sd.list")) return cmd_sd_list(params);
    if (!strcmp(command, "sd.read")) return cmd_sd_read(params);
    if (!strcmp(command, "sd.write")) return cmd_sd_write(params);
    if (!strcmp(command, "sd.delete")) return cmd_sd_delete(params);
    if (!strcmp(command, "sd.mkdir")) return cmd_sd_mkdir(params);
    if (!strcmp(command, "sd.save_url")) return cmd_sd_save_url(params, request_id, gen);
#endif
    if (!strcmp(command, "gadget.sleep")) return cmd_sleep(params);
    if (!strcmp(command, "gadget.configure")) return cmd_configure(params);
    if (!strcmp(command, "gadget.status")) return cmd_status();
#if CONFIG_MUSE_PET
    cJSON *pet = gadget_pet_command(command, params, request_id, gen);
    if (pet) {
        return pet;
    }
#endif
    return gadget_more_command(command, params, request_id, gen);
}

void gadget_tools_init(void)
{
    setenv("TZ", CONFIG_MUSE_TOOLS_TZ, 1);
    tzset();
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_err_t err = esp_netif_sntp_init(&sntp);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sntp init: %s", esp_err_to_name(err));
    }

#if SOC_TEMP_SENSOR_SUPPORTED
    temperature_sensor_config_t tcfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 80);
    if (temperature_sensor_install(&tcfg, &s_tsens) == ESP_OK && temperature_sensor_enable(s_tsens) != ESP_OK) {
        temperature_sensor_uninstall(s_tsens);
        s_tsens = NULL;
    }
#endif

    gpio_allow_init();
#if HAVE_LIGHT
    light_init();
#endif
#if HAVE_SD
    sd_mount();   /* a card in the slot at boot; later calls retry */
#endif

    esp_timer_handle_t tick;
    const esp_timer_create_args_t args = { .callback = tick_cb, .name = "gadget_tick" };
    ESP_ERROR_CHECK(esp_timer_create(&args, &tick));
    gadget_more_init();
#if CONFIG_MUSE_PET
    gadget_pet_init();
#endif
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick, 1000000));
    ESP_LOGI(TAG, "ready: tz %s, light %s, sd %s, gpio pins %d", CONFIG_MUSE_TOOLS_TZ, HAVE_LIGHT ? "yes" : "no",
             HAVE_SD ? "configured" : "no", s_gpio_allow_n);
}
