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
 * The pet's life (see pet.h). Everything here is deterministic clockwork on
 * the board: needs that drain by the minute at rates set by its stage and
 * traits, sleep when it's tired or it's night, poop after meals, sickness
 * when it's neglected, growth through five stages with an evolution at each
 * step shaped by how it was cared for, and, after a day at zero health, it
 * leaves and a new egg takes its place. Muse gets reports and answers in the
 * pet's voice; it never has to be online for the pet to live.
 */
#include "pet.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "muse_audio.h"
#include "muse_chat.h"
#include "muse_state.h"
#include "muse_voice.h"

#define TAG "pet"

#ifndef CONFIG_MUSE_PET_REPORT_MIN
#define CONFIG_MUSE_PET_REPORT_MIN 90
#endif

#define NS "pet"
#define SAVE_MAGIC 0x50455431u   /* "PET1" */
#define SAVE_VERSION 3   /* 3: power and theme */
#define SAVE_EVERY_S 300
#define OFFLINE_MAX_MIN (7 * 24 * 60)
#define EGG_AUTO_HATCH_MIN (6 * 60)
#define EGG_TAP_WARMTH 0.05f
#define EGG_TAP_GAP_US 400000
#define DIGEST_MIN 40.0f
#define MAX_POOPS 4
#define RUN_AWAY_MIN 1440.0f
#define EVENT_REPORT_GAP_S 90
#define NEEDY_REPORT_MIN 30
#define REPORT_REPLY_WINDOW_S 90

/* Stage boundaries in minutes since hatching. */
static const uint32_t STAGE_END_MIN[PET_STAGE_COUNT] = {
    [PET_EGG] = 0,
    [PET_BABY] = 24 * 60,
    [PET_KID] = 72 * 60,
    [PET_TEEN] = 168 * 60,
    [PET_ADULT] = 504 * 60,
    [PET_ELDER] = 0xffffffffu,
};

/* How fast the needs drain per stage (the baby is the hungriest). */
static const float STAGE_RATE[PET_STAGE_COUNT] = { 0, 1.4f, 1.2f, 1.0f, 0.8f, 0.7f };

/* Base drain per hour of each need, awake, as a teen. */
static const float NEED_PER_H[PET_NEED_COUNT] = {
    [PET_NEED_FOOD] = 6.0f,
    [PET_NEED_ENERGY] = 4.0f,
    [PET_NEED_FUN] = 5.0f,
    [PET_NEED_CLEAN] = 3.0f,
    [PET_NEED_BOND] = 2.5f,
};

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t generation;
    pet_genome_t g;
    uint8_t stage;
    uint8_t sick;
    uint8_t lights_off;
    uint8_t poops;
    uint8_t medicine;        /* doses taken this illness */
    uint8_t next_variant;    /* Muse's pick for the next evolution, 0 = by care */
    uint8_t theme;
    uint8_t pad;
    float needs[PET_NEED_COUNT];
    float health;
    float care_acc, care_n;  /* average of the needs over this stage */
    float egg_warmth;
    float age_min;
    float zero_health_min;
    float digest_min;        /* minutes until the next poop, 0 = none coming */
    float power;             /* 0..100, from training; fades slowly */
    int64_t last_ts;         /* unix time at the last tick, 0 = unknown */
    uint32_t feeds, plays, cleans;
    char name[16];
} pet_save_t;

static pet_save_t s;
static SemaphoreHandle_t s_lock;
static bool s_dirty;
static int64_t s_last_save_us;
static int s_time_scale = 1;

static pet_anim_t s_anim = PET_ANIM_IDLE;
static int64_t s_anim_start_us, s_anim_end_us;
static bool s_asleep;                 /* this minute: lights, tiredness or the night */
static int64_t s_awake_until_us;      /* a tap at night keeps it up a while */
static pet_mood_t s_mood = PET_MOOD_CONTENT;
static pet_mood_t s_mood_override = PET_MOOD_COUNT;
static int64_t s_mood_override_until_us;
static int64_t s_last_tap_us;
static int s_taps_recent;

static int64_t s_last_report_us = -(int64_t)3600 * 1000000;
static int64_t s_last_routine_us;
static bool s_report_pending;
static int64_t s_report_pending_us;
static char s_last_said[128];

/* ---- helpers ------------------------------------------------------------- */

static uint32_t xorshift(uint32_t *x)
{
    uint32_t v = *x ? *x : 0x9e3779b9u;
    v ^= v << 13;
    v ^= v >> 17;
    v ^= v << 5;
    *x = v;
    return v;
}

static int rnd(uint32_t *x, int lo, int hi)
{
    return lo + (int)(xorshift(x) % (uint32_t)(hi - lo + 1));
}

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static bool clock_valid(time_t t)
{
    return t > 1700000000;   /* past 2023: SNTP has set it */
}

static int local_hour(void)
{
    time_t t = time(NULL);
    if (!clock_valid(t)) {
        return -1;
    }
    struct tm tm;
    localtime_r(&t, &tm);
    return tm.tm_hour;
}

static void lock(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_lock);
}

/* ---- the genome ------------------------------------------------------------ */

static void make_genome(pet_genome_t *g, uint32_t seed, const pet_genome_t *parent)
{
    uint32_t x = seed;
    memset(g, 0, sizeof(*g));
    g->seed = seed;
    g->species = (uint8_t)rnd(&x, 0, PET_SP_COUNT - 1);
    if (parent && rnd(&x, 0, 2)) {
        g->species = parent->species;   /* mostly its parent's kind */
    }
    g->size = (uint8_t)rnd(&x, 96, 170);
    g->head_size = (uint8_t)rnd(&x, 100, 170);
    g->tail_len = (uint8_t)rnd(&x, 90, 180);
    int r = rnd(&x, 0, 99);
    g->eye_n = r < 70 ? 2 : r < 90 ? 1 : 3;
    g->eye_size = (uint8_t)rnd(&x, 2, 5);
    g->pattern = (uint8_t)rnd(&x, 0, 2);
    switch (g->species) {
    case PET_SP_REX:
        g->neck = (uint8_t)rnd(&x, 40, 90);
        g->jaw = (uint8_t)rnd(&x, 0, 1);
        g->teeth = (uint8_t)rnd(&x, 1, 2);
        g->crest = rnd(&x, 0, 3) == 0 ? 1 : 0;
        g->back = rnd(&x, 0, 2) == 0 ? 1 : 0;
        break;
    case PET_SP_RAPTOR:
        g->neck = (uint8_t)rnd(&x, 60, 110);
        g->jaw = 1;
        g->teeth = (uint8_t)rnd(&x, 1, 2);
        g->crest = 3;
        g->tail_tip = rnd(&x, 0, 1) ? 3 : 0;
        break;
    case PET_SP_SAUROPOD:
        g->neck = (uint8_t)rnd(&x, 170, 255);
        g->back = rnd(&x, 0, 2) == 0 ? 4 : 0;
        g->tail_len = (uint8_t)rnd(&x, 160, 230);
        break;
    case PET_SP_STEGO:
        g->neck = (uint8_t)rnd(&x, 40, 80);
        g->jaw = rnd(&x, 0, 2) == 0 ? 2 : 0;
        g->back = 2;
        g->tail_tip = 2;
        break;
    case PET_SP_CERATOPS:
        g->neck = (uint8_t)rnd(&x, 30, 60);
        g->jaw = 2;
        g->crest = 4;
        g->tail_len = (uint8_t)rnd(&x, 60, 110);
        break;
    case PET_SP_ANKYLO:
        g->neck = (uint8_t)rnd(&x, 30, 60);
        g->crest = rnd(&x, 0, 1) ? 2 : 0;
        g->back = 4;
        g->tail_tip = 1;
        break;
    default:   /* ptero */
        g->neck = (uint8_t)rnd(&x, 70, 120);
        g->jaw = rnd(&x, 0, 1) ? 1 : 2;
        g->teeth = (uint8_t)rnd(&x, 0, 1);
        g->crest = 5;
        g->tail_tip = rnd(&x, 0, 1) ? 3 : 0;
        g->tail_len = (uint8_t)rnd(&x, 50, 100);
        break;
    }
    g->hue = (uint8_t)rnd(&x, 0, 255);
    if (parent) {
        g->hue = (uint8_t)(parent->hue + rnd(&x, -24, 24));   /* the family colour */
    }
    g->hue2 = (uint8_t)(g->hue + rnd(&x, 70, 190));
    g->eye_hue = (uint8_t)rnd(&x, 0, 255);
    g->sat = (uint8_t)rnd(&x, 160, 255);
    g->voice = (uint8_t)rnd(&x, 0, 255);
    g->traits = (uint8_t)(1u << rnd(&x, 0, 3));
    if (rnd(&x, 0, 1)) {
        g->traits |= (uint8_t)(1u << rnd(&x, 0, 3));
    }
}

static const char *hue_word(uint8_t hue)
{
    static const char *const NAMES[] = { "red", "orange", "yellow", "lime", "green", "teal",
                                         "cyan", "sky blue", "blue", "indigo", "purple", "pink" };
    return NAMES[(hue + 10) * 12 / 256 % 12];
}

const char *pet_species_name(pet_species_t sp)
{
    static const char *const NAMES[PET_SP_COUNT] = { "rex", "raptor", "long-neck", "stego", "tri-horn", "ankylo", "ptero" };
    return (int)sp >= 0 && sp < PET_SP_COUNT ? NAMES[sp] : "?";
}

static bool two_legged(uint8_t species)
{
    return species == PET_SP_REX || species == PET_SP_RAPTOR || species == PET_SP_PTERO;
}

static const char *jaw_word(uint8_t jaw)
{
    static const char *const NAMES[] = { "a round snout", "a long snout", "a beak" };
    return NAMES[jaw % 3];
}

static const char *teeth_word(uint8_t teeth)
{
    static const char *const NAMES[] = { "", "teeth", "big fangs" };
    return NAMES[teeth % 3];
}

static const char *crest_word(uint8_t crest)
{
    static const char *const NAMES[] = { "", "a nose horn", "brow horns", "a feather crest", "a frill and horns", "a long head crest" };
    return NAMES[crest % 6];
}

static const char *back_word(uint8_t back)
{
    static const char *const NAMES[] = { "", "spikes down its back", "plates down its back", "a sail on its back", "armour bumps" };
    return NAMES[back % 5];
}

static const char *tail_word(uint8_t tip)
{
    static const char *const NAMES[] = { "a plain tail", "a club tail", "a spiked tail", "a tufted tail" };
    return NAMES[tip % 4];
}

static const char *pattern_word(uint8_t pattern)
{
    static const char *const NAMES[] = { "", "spots", "stripes" };
    return NAMES[pattern % 3];
}

static const char *trait_words(uint8_t t, char *buf, size_t cap)
{
    buf[0] = '\0';
    const char *names[] = { "bold", "lazy", "sociable", "greedy" };
    for (int i = 0; i < 4; i++) {
        if (t & (1u << i)) {
            strlcat(buf, buf[0] ? " and " : "", cap);
            strlcat(buf, names[i], cap);
        }
    }
    return buf[0] ? buf : "easy-going";
}

/* "a small two-legged teal rex dino with a cream belly, a long snout, big fangs, a feather crest..." */
static void describe(char *out, size_t cap)
{
    const pet_genome_t *g = &s.g;
    char traits[48];
    if (s.stage == PET_EGG) {
        snprintf(out, cap, "a %s speckled egg (a %s dino is inside)", hue_word(g->hue), pet_species_name(g->species));
        return;
    }
    if (s.stage == PET_BABY) {
        snprintf(out, cap, "a tiny round %s in-training blob with %s eyes, a baby %s dino that hasn't grown its body yet; %s by nature",
                 hue_word(g->hue), g->eye_n == 1 ? "one" : g->eye_n == 2 ? "two" : "three", pet_species_name(g->species),
                 trait_words(g->traits, traits, sizeof(traits)));
        return;
    }
    static const char *const SIZE[PET_STAGE_COUNT] = { "", "", "small chibi", "half-grown", "full-grown", "old" };
    int n = snprintf(out, cap, "a %s %s %s %s dino with a cream belly, %s eyes and %s", SIZE[s.stage],
                     two_legged(g->species) ? (g->species == PET_SP_PTERO ? "winged" : "two-legged") : "four-legged",
                     hue_word(g->hue), pet_species_name(g->species), g->eye_n == 1 ? "one" : g->eye_n == 2 ? "two" : "three",
                     jaw_word(g->jaw));
#define ADD(fmt, ...) n += snprintf(out + n, n < (int)cap ? cap - n : 0, fmt, __VA_ARGS__)
    if (g->teeth && s.stage >= PET_TEEN) {
        ADD(", %s", teeth_word(g->teeth));
    }
    if (s.stage >= PET_TEEN && g->crest) {
        ADD(", %s", crest_word(g->crest));
    }
    if (s.stage >= PET_TEEN && g->back) {
        ADD(", %s", back_word(g->back));
    }
    if (s.stage >= PET_ADULT) {
        ADD(", %s", tail_word(g->tail_tip));
        if (g->pattern) {
            ADD(" and %s %s", hue_word(g->hue2), pattern_word(g->pattern));
        }
    }
    ADD("; %s by nature", trait_words(g->traits, traits, sizeof(traits)));
#undef ADD
}

/* ---- sounds: little chirps in the creature's own pitch --------------------- */

#define CHIRP_MAX_FRAMES (MUSE_AUDIO_RATE * 3 / 5)

typedef struct {
    float f;        /* Hz, 0 = noise burst, -1 = rest */
    int ms;
} note_t;

static void chirp(const note_t *notes, int n, float gain)
{
    /* A fresh buffer each time: the voice task frees a clip once it has played it. */
    int16_t *s_chirp = heap_caps_malloc(CHIRP_MAX_FRAMES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_chirp) {
        s_chirp = malloc(CHIRP_MAX_FRAMES * sizeof(int16_t));
    }
    if (!s_chirp) {
        return;
    }
    float f0 = 1.0f + s.g.voice / 255.0f;   /* 1..2: the voice's pitch */
    size_t at = 0;
    uint32_t noise = 0x1234567u ^ s.g.seed;
    for (int i = 0; i < n && at < CHIRP_MAX_FRAMES; i++) {
        size_t len = (size_t)MUSE_AUDIO_RATE * notes[i].ms / 1000;
        if (at + len > CHIRP_MAX_FRAMES) {
            len = CHIRP_MAX_FRAMES - at;
        }
        float phase = 0, step = notes[i].f * f0 / MUSE_AUDIO_RATE;
        for (size_t k = 0; k < len; k++) {
            float env = 1.0f - (float)k / (float)len;
            env *= k < 40 ? k / 40.0f : 1.0f;
            float v = 0;
            if (notes[i].f > 0) {
                phase += step;
                phase -= (int)phase;
                v = (phase < 0.5f ? 1.0f : -1.0f) * 0.55f + (phase * 2 - 1) * 0.45f;   /* square + saw */
            } else if (notes[i].f == 0) {
                v = ((xorshift(&noise) >> 8) & 0xffff) / 32768.0f - 1.0f;
            }
            s_chirp[at + k] = (int16_t)(v * env * gain * 9000.0f);
        }
        at += len;
    }
    if (!at || !muse_voice_request_pcm(s_chirp, at, 1)) {
        free(s_chirp);   /* nothing to play, or a clip is already waiting */
    }
}

typedef enum { SND_HAPPY, SND_SAD, SND_EAT, SND_CALL, SND_SLEEP, SND_HATCH, SND_EVOLVE, SND_PLAY, SND_SAY, SND_LEAVE } sound_t;

static void play(sound_t which)
{
    switch (which) {
    case SND_HAPPY: {
        const note_t n[] = { { 520, 70 }, { -1, 30 }, { 660, 90 } };
        chirp(n, 3, 1.0f);
        break;
    }
    case SND_SAD: {
        const note_t n[] = { { 480, 120 }, { 400, 120 }, { 330, 200 } };
        chirp(n, 3, 0.8f);
        break;
    }
    case SND_EAT: {
        const note_t n[] = { { 0, 45 }, { -1, 90 }, { 0, 45 }, { -1, 90 }, { 0, 45 } };
        chirp(n, 5, 0.6f);
        break;
    }
    case SND_CALL: {
        const note_t n[] = { { 600, 80 }, { -1, 60 }, { 600, 80 }, { -1, 60 }, { 760, 140 } };
        chirp(n, 5, 1.0f);
        break;
    }
    case SND_SLEEP: {
        const note_t n[] = { { 300, 180 }, { 250, 260 } };
        chirp(n, 2, 0.4f);
        break;
    }
    case SND_HATCH: {
        const note_t n[] = { { 400, 90 }, { 500, 90 }, { 600, 90 }, { 800, 220 } };
        chirp(n, 4, 1.0f);
        break;
    }
    case SND_EVOLVE: {
        const note_t n[] = { { 400, 110 }, { 500, 110 }, { 600, 110 }, { 500, 90 }, { 800, 300 } };
        chirp(n, 5, 1.0f);
        break;
    }
    case SND_PLAY: {
        const note_t n[] = { { 500, 60 }, { 700, 60 }, { 500, 60 }, { 900, 120 } };
        chirp(n, 4, 0.9f);
        break;
    }
    case SND_SAY: {
        const note_t n[] = { { 560, 50 }, { -1, 30 }, { 620, 50 }, { -1, 30 }, { 540, 70 } };
        chirp(n, 5, 0.7f);
        break;
    }
    case SND_LEAVE: {
        const note_t n[] = { { 500, 200 }, { 400, 200 }, { 300, 200 }, { 200, 400 } };
        chirp(n, 4, 0.8f);
        break;
    }
    }
}

/* ---- animation -------------------------------------------------------------- */

static void set_anim(pet_anim_t a, float secs)
{
    s_anim = a;
    s_anim_start_us = esp_timer_get_time();
    s_anim_end_us = secs > 0 ? s_anim_start_us + (int64_t)(secs * 1e6f) : 0;
}

static pet_anim_t current_anim(int64_t now)
{
    if (s_anim_end_us && now >= s_anim_end_us) {
        s_anim = PET_ANIM_IDLE;
        s_anim_end_us = 0;
    }
    if (s_anim != PET_ANIM_IDLE) {
        return s_anim;
    }
    if (s.stage == PET_EGG) {
        return PET_ANIM_IDLE;
    }
    if (s_asleep) {
        return PET_ANIM_SLEEP;
    }
    if (s.sick) {
        return PET_ANIM_SICK;
    }
    if (s_mood == PET_MOOD_HUNGRY || s_mood == PET_MOOD_LONELY || s_mood == PET_MOOD_BORED) {
        return PET_ANIM_SAD;
    }
    return PET_ANIM_IDLE;
}

/* ---- the mood ---------------------------------------------------------------- */

static pet_mood_t compute_mood(int64_t now)
{
    if (s_mood_override != PET_MOOD_COUNT && now < s_mood_override_until_us) {
        return s_mood_override;
    }
    if (s.stage == PET_EGG) {
        return PET_MOOD_CONTENT;
    }
    if (s_asleep) {
        return PET_MOOD_ASLEEP;
    }
    if (s.sick) {
        return PET_MOOD_SICK;
    }
    if (s.needs[PET_NEED_FOOD] < 30) {
        return PET_MOOD_HUNGRY;
    }
    if (s.needs[PET_NEED_ENERGY] < 25) {
        return PET_MOOD_TIRED;
    }
    if (s.needs[PET_NEED_CLEAN] < 30 || s.poops >= 2) {
        return PET_MOOD_DIRTY;
    }
    if (s.needs[PET_NEED_FUN] < 30) {
        return PET_MOOD_BORED;
    }
    if (s.needs[PET_NEED_BOND] < 30) {
        return PET_MOOD_LONELY;
    }
    float avg = 0;
    for (int i = 0; i < PET_NEED_COUNT; i++) {
        avg += s.needs[i];
    }
    return avg / PET_NEED_COUNT >= 75 ? PET_MOOD_HAPPY : PET_MOOD_CONTENT;
}

/* ---- persistence --------------------------------------------------------------- */

static void save_now(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    time_t t = time(NULL);
    s.last_ts = clock_valid(t) ? (int64_t)t : 0;
    nvs_set_blob(h, "state", &s, sizeof(s));
    nvs_commit(h);
    nvs_close(h);
    s_dirty = false;
    s_last_save_us = esp_timer_get_time();
}

static bool load(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    pet_save_t tmp;
    size_t n = sizeof(tmp);
    esp_err_t err = nvs_get_blob(h, "state", &tmp, &n);
    nvs_close(h);
    if (err != ESP_OK || n != sizeof(tmp) || tmp.magic != SAVE_MAGIC || tmp.version != SAVE_VERSION) {
        return false;
    }
    s = tmp;
    return true;
}

/* ---- life events ---------------------------------------------------------------- */

static void lay_egg(const pet_genome_t *parent, uint16_t generation)
{
    pet_genome_t g;
    make_genome(&g, esp_random(), parent);
    memset(&s, 0, sizeof(s));
    s.magic = SAVE_MAGIC;
    s.version = SAVE_VERSION;
    s.generation = generation;
    s.g = g;
    s.stage = PET_EGG;
    s.health = 100;
    for (int i = 0; i < PET_NEED_COUNT; i++) {
        s.needs[i] = 70;
    }
    s_mood_override = PET_MOOD_COUNT;
    s_dirty = true;
    ESP_LOGI(TAG, "a new egg: generation %u, seed %08x", (unsigned)generation, (unsigned)g.seed);
}

static void hatch(void)
{
    s.stage = PET_BABY;
    s.age_min = 0;
    s.care_acc = s.care_n = 0;
    s.egg_warmth = 1;
    s.health = 100;
    for (int i = 0; i < PET_NEED_COUNT; i++) {
        s.needs[i] = 70;
    }
    s.needs[PET_NEED_FOOD] = 45;   /* hungry from the start */
    s_dirty = true;
    set_anim(PET_ANIM_HATCH, 4.0f);
    ESP_LOGI(TAG, "hatched");
}

/* The evolution at a stage's end: its body changes with how it was looked after
 * (or as Muse asked): noble, cute or feral. */
static void evolve(int variant)
{
    pet_genome_t *g = &s.g;
    uint32_t x = g->seed ^ (0x51edu * (s.stage + 1));
    int care = s.care_n > 0 ? (int)(s.care_acc / s.care_n) : 60;
    if (variant <= 0) {
        variant = care >= 65 && s.power >= 45 ? 1 : care >= 45 ? 2 : 3;
    }
    switch (variant) {
    case 1:   /* noble: a grander crest and back, brighter, bigger */
        if (g->crest == 0 || g->crest == 1) {
            g->crest = rnd(&x, 0, 1) ? 2 : 3;
        }
        if (g->species == PET_SP_CERATOPS) {
            g->crest = 4;
        }
        if (g->back == 0 || g->back == 4) {
            g->back = rnd(&x, 0, 1) ? 2 : 3;
        }
        g->sat = (uint8_t)(g->sat > 215 ? 255 : g->sat + 40);
        g->tail_tip = g->tail_tip == 0 ? 3 : g->tail_tip;
        g->teeth = g->teeth == 2 ? 1 : g->teeth;
        g->size = (uint8_t)(g->size > 235 ? 255 : g->size + 20);
        break;
    case 2:   /* cute: bigger eyes and head, a round snout, no teeth, spots */
        g->eye_size = (uint8_t)(g->eye_size < 5 ? g->eye_size + 1 : 5);
        g->head_size = (uint8_t)(g->head_size > 225 ? 255 : g->head_size + 30);
        g->jaw = g->jaw == 1 ? 0 : g->jaw;
        g->teeth = 0;
        g->pattern = 1;
        g->crest = g->crest == 0 ? 3 : g->crest;
        g->back = g->back == 1 ? 4 : g->back;
        break;
    default:   /* feral: fangs, spikes, a spiked tail, stripes, darker */
        g->teeth = 2;
        g->jaw = g->jaw == 2 ? 2 : 1;
        g->back = (g->back == 0 || g->back == 4) ? 1 : g->back;
        g->tail_tip = 2;
        g->pattern = 2;
        g->sat = (uint8_t)(g->sat < 190 ? 150 : g->sat - 40);
        g->crest = g->crest == 0 ? 1 : g->crest == 3 ? 2 : g->crest;
        g->size = (uint8_t)(g->size > 245 ? 255 : g->size + 10);
        break;
    }
    s.stage++;
    s.care_acc = s.care_n = 0;
    s.next_variant = 0;
    s_dirty = true;
    set_anim(PET_ANIM_EVOLVE, 3.5f);
    ESP_LOGI(TAG, "evolved to %s (variant %d, care %d)", pet_stage_name(s.stage), variant, care);
}

static void run_away(void)
{
    ESP_LOGW(TAG, "the pet left (generation %u)", (unsigned)s.generation);
    pet_genome_t parent = s.g;
    uint16_t gen = s.generation + 1;
    lay_egg(&parent, gen);
    set_anim(PET_ANIM_LEAVE, 5.0f);
}

/* ---- the clock: dt minutes of life -------------------------------------------- */

typedef enum { EV_NONE, EV_HATCHED, EV_EVOLVED, EV_SICK, EV_LEFT, EV_HUNGRY, EV_WOKE, EV_POOPED } event_t;

static event_t advance(float dt_min, bool offline)
{
    event_t ev = EV_NONE;
    if (s.stage == PET_EGG) {
        s.age_min += dt_min;
        if (s.egg_warmth >= 1.0f) {   /* only warmed by hand: it waits for Mat */
            hatch();
            return EV_HATCHED;
        }
        return EV_NONE;
    }
    const pet_genome_t *g = &s.g;
    float stage_k = STAGE_RATE[s.stage] * (offline ? 0.5f : 1.0f);
    float k[PET_NEED_COUNT] = { 1, 1, 1, 1, 1 };
    if (g->traits & PET_TRAIT_GREEDY) {
        k[PET_NEED_FOOD] *= 1.3f;
    }
    if (g->traits & PET_TRAIT_LAZY) {
        k[PET_NEED_ENERGY] *= 1.3f;
        k[PET_NEED_FUN] *= 0.8f;
    }
    if (g->traits & PET_TRAIT_SOCIAL) {
        k[PET_NEED_BOND] *= 1.5f;
    }
    if (g->traits & PET_TRAIT_BOLD) {
        k[PET_NEED_FUN] *= 1.2f;
    }
    if (s_asleep) {
        k[PET_NEED_FOOD] *= 0.5f;
        k[PET_NEED_FUN] *= 0.3f;
        k[PET_NEED_CLEAN] *= 0.5f;
        k[PET_NEED_BOND] *= 0.5f;
    }
    bool was_hungry = s.needs[PET_NEED_FOOD] < 30;
    for (int i = 0; i < PET_NEED_COUNT; i++) {
        if (i == PET_NEED_ENERGY) {
            float d = s_asleep ? 15.0f : -NEED_PER_H[i] * k[i] * stage_k;
            s.needs[i] = clampf(s.needs[i] + d / 60.0f * dt_min, 0, 100);
        } else {
            s.needs[i] = clampf(s.needs[i] - NEED_PER_H[i] * k[i] * stage_k / 60.0f * dt_min, 0, 100);
        }
    }
    if (!was_hungry && s.needs[PET_NEED_FOOD] < 30) {
        ev = EV_HUNGRY;
    }
    s.power = clampf(s.power - 0.6f / 60.0f * dt_min, 0, 100);   /* training wears off over days */
    /* Digestion. */
    if (s.digest_min > 0) {
        s.digest_min -= dt_min;
        if (s.digest_min <= 0) {
            s.digest_min = 0;
            if (s.poops < MAX_POOPS) {
                s.poops++;
                s.needs[PET_NEED_CLEAN] = clampf(s.needs[PET_NEED_CLEAN] - 15, 0, 100);
                ev = ev == EV_NONE ? EV_POOPED : ev;
            }
        }
    }
    /* Health. */
    int critical = 0;
    bool all_fine = true;
    for (int i = 0; i < PET_NEED_COUNT; i++) {
        critical += s.needs[i] < 15;
        all_fine = all_fine && s.needs[i] >= 40;
    }
    float dh = 0;
    if (s.sick) {
        dh -= 6.0f;
    }
    dh -= 4.0f * critical;
    if (s.poops >= 3) {
        dh -= 2.0f;
    }
    if (!s.sick && all_fine && s.poops < 3) {
        dh += 2.0f;
    }
    s.health = clampf(s.health + dh / 60.0f * dt_min, 0, 100);
    if (!s.sick && s.health < 40) {
        s.sick = true;
        s.medicine = 0;
        ev = EV_SICK;
    }
    if (s.health <= 0) {
        s.zero_health_min += dt_min;
        if (s.zero_health_min >= RUN_AWAY_MIN) {
            run_away();
            return EV_LEFT;
        }
    } else {
        s.zero_health_min = 0;
    }
    /* Care, for the next evolution. */
    float avg = 0;
    for (int i = 0; i < PET_NEED_COUNT; i++) {
        avg += s.needs[i];
    }
    s.care_acc += avg / PET_NEED_COUNT * dt_min;
    s.care_n += dt_min;
    /* Age and stages. */
    s.age_min += dt_min;
    if (s.stage < PET_ELDER && s.age_min >= STAGE_END_MIN[s.stage]) {
        evolve(s.next_variant);
        return EV_EVOLVED;
    }
    return ev;
}

/* Asleep this minute? Lights off, worn out, or the night, unless a tap keeps it up. */
static void update_sleep(int64_t now)
{
    if (s.stage == PET_EGG) {
        s_asleep = false;
        return;
    }
    bool was = s_asleep;
    if (s.lights_off) {
        s_asleep = true;
    } else if (now < s_awake_until_us) {
        s_asleep = false;
    } else if (s_asleep) {
        s_asleep = s.needs[PET_NEED_ENERGY] < 92;   /* sleeps until rested */
        int h = local_hour();
        if (h >= 0 && (h >= 23 || h < 7)) {
            s_asleep = true;   /* the night, through */
        }
    } else {
        int h = local_hour();
        s_asleep = s.needs[PET_NEED_ENERGY] < 10 || (h >= 0 && (h >= 23 || h < 7));
    }
    if (was != s_asleep) {
        ESP_LOGI(TAG, "%s", s_asleep ? "fell asleep" : "woke up");
    }
}

/* ---- Muse: reports and replies ------------------------------------------------- */

static const char *need_state(float v)
{
    return v < 15 ? "critical" : v < 30 ? "low" : v < 60 ? "okay" : "good";
}

bool pet_report(const char *reason)
{
    if (CONFIG_MUSE_PET_REPORT_MIN <= 0 || !muse_hatch_ready() || muse_state_mode(NULL) != MUSE_MODE_IDLE) {
        return false;
    }
    int64_t now = esp_timer_get_time();
    if (now - s_last_report_us < (int64_t)EVENT_REPORT_GAP_S * 1000000) {
        return false;
    }
    char desc[200], status[300];
    lock();
    describe(desc, sizeof(desc));
    snprintf(status, sizeof(status),
             "food %d (%s), energy %d (%s), fun %d (%s), clean %d (%s), bond %d (%s); health %d%s; mood %s; "
             "%s; age %uh%02um; %u poop%s on the floor; power %d (from training)",
             (int)s.needs[0], need_state(s.needs[0]), (int)s.needs[1], need_state(s.needs[1]), (int)s.needs[2],
             need_state(s.needs[2]), (int)s.needs[3], need_state(s.needs[3]), (int)s.needs[4], need_state(s.needs[4]),
             (int)s.health, s.sick ? " (SICK, needs medicine)" : "", pet_mood_name(s_mood),
             s_asleep ? "asleep" : "awake", (unsigned)(s.age_min / 60), (unsigned)s.age_min % 60, (unsigned)s.poops,
             s.poops == 1 ? "" : "s", (int)s.power);
    char name[16];
    strlcpy(name, s.name[0] ? s.name : "the unnamed pet", sizeof(name));
    pet_stage_t stage = s.stage;
    unlock();
    size_t cap = 1024;
    char *msg = malloc(cap);
    if (!msg) {
        return false;
    }
    snprintf(msg, cap,
             "[pet report] %s. You are the spirit of %s, %s, %s, living on Mat's desk gadget. Status: %s. "
             "Answer with ONE short line (under 110 characters) that the creature says out loud right now, in its own "
             "small voice and personality (it is a pet, not an assistant; no stage directions). You may also call pet.* "
             "commands: pet.say, pet.set_mood, pet.name (if it has no name, name it), pet.feed/play/clean/medicine/lights, "
             "pet.evolve. Don't mention this message.",
             reason, name, pet_stage_name(stage), desc, status);
    s_last_report_us = now;
    s_report_pending = true;
    s_report_pending_us = now;
    muse_hatch_text_turn(msg);   /* frees it */
    ESP_LOGI(TAG, "report to Muse: %s", reason);
    return true;
}

/* The chat session: a typed turn's reply. Ours if a report just went. */
void muse_hatch_typed_reply(const char *text)
{
    if (!s_report_pending || !text || !text[0]) {
        return;
    }
    if (esp_timer_get_time() - s_report_pending_us > (int64_t)REPORT_REPLY_WINDOW_S * 1000000) {
        s_report_pending = false;
        return;
    }
    s_report_pending = false;
    char line[160];
    strlcpy(line, text, sizeof(line));
    char *nl = strchr(line, '\n');
    if (nl) {
        *nl = '\0';
    }
    if (line[0] == '"') {
        memmove(line, line + 1, strlen(line));
        size_t n = strlen(line);
        if (n && line[n - 1] == '"') {
            line[n - 1] = '\0';
        }
    }
    pet_say(line, 14);
}

/* ---- the tick task ------------------------------------------------------------------ */

static void tick_task(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    int64_t last_us = esp_timer_get_time();
    int second = 0;
    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(1000));
        int64_t now = esp_timer_get_time();
        float dt_min = (float)(now - last_us) / 60e6f * (float)s_time_scale;
        last_us = now;
        event_t ev;
        pet_mood_t mood_was;
        bool asleep_was;
        char said[96] = "";
        sound_t snd = SND_HAPPY;
        bool sound = false;
        lock();
        mood_was = s_mood;
        asleep_was = s_asleep;
        update_sleep(now);
        ev = advance(dt_min, false);
        if (ev != EV_LEFT) {
            update_sleep(now);
        }
        s_mood = compute_mood(now);
        if (s_taps_recent && now - s_last_tap_us > 3000000) {
            s_taps_recent = 0;
        }
        s_dirty = s_dirty || ev != EV_NONE || (second % 60 == 0);
        bool save = s_dirty && (ev != EV_NONE || now - s_last_save_us > (int64_t)SAVE_EVERY_S * 1000000);
        if (asleep_was && !s_asleep && ev == EV_NONE) {
            ev = EV_WOKE;
        }
        unlock();
        second++;

        const char *reason = NULL;
        switch (ev) {
        case EV_HATCHED:
            reason = "It just hatched out of its egg";
            snprintf(said, sizeof(said), "...!");
            snd = SND_HATCH;
            sound = true;
            break;
        case EV_EVOLVED:
            reason = "It just evolved into its next stage";
            snd = SND_EVOLVE;
            sound = true;
            break;
        case EV_SICK:
            reason = "It just got sick";
            snprintf(said, sizeof(said), "...urgh...");
            snd = SND_SAD;
            sound = true;
            break;
        case EV_LEFT:
            reason = "The previous pet ran away after a day at zero health and left this new egg behind";
            snprintf(said, sizeof(said), "...it left. A new egg is here.");
            snd = SND_LEAVE;
            sound = true;
            break;
        case EV_HUNGRY:
            reason = "It just got hungry";
            snd = SND_CALL;
            sound = true;
            break;
        case EV_WOKE:
            reason = "It just woke up";
            snd = SND_HAPPY;
            sound = true;
            break;
        case EV_POOPED:
            snd = SND_SAD;
            sound = true;
            break;
        default:
            break;
        }
        if (sound && !s_asleep) {
            play(snd);
        }
        if (said[0]) {
            pet_ui_bubble(said, 6);
        }
        if (reason) {
            pet_report(reason);
        } else if (s_mood != mood_was && !s_asleep && s.stage != PET_EGG &&
                   (s_mood == PET_MOOD_BORED || s_mood == PET_MOOD_LONELY || s_mood == PET_MOOD_DIRTY || s_mood == PET_MOOD_TIRED)) {
            if (now - s_last_report_us > (int64_t)NEEDY_REPORT_MIN * 60 * 1000000) {
                char why[64];
                snprintf(why, sizeof(why), "It is feeling %s now", pet_mood_name(s_mood));
                pet_report(why);
            }
        } else if (!s_asleep && s.stage != PET_EGG && CONFIG_MUSE_PET_REPORT_MIN > 0 &&
                   now - s_last_routine_us > (int64_t)CONFIG_MUSE_PET_REPORT_MIN * 60 * 1000000 &&
                   now - s_last_report_us > (int64_t)NEEDY_REPORT_MIN * 60 * 1000000) {
            s_last_routine_us = now;
            pet_report("Just checking in");
        }
        if (s_report_pending && now - s_report_pending_us > (int64_t)REPORT_REPLY_WINDOW_S * 1000000) {
            s_report_pending = false;
        }
        if (save) {
            lock();
            save_now();
            unlock();
        }
    }
}

/* ---- public ------------------------------------------------------------------------- */

void pet_init(void)
{
    static StaticSemaphore_t lock_buf;
    s_lock = xSemaphoreCreateMutexStatic(&lock_buf);
    if (!load()) {
        lay_egg(NULL, 1);
        save_now();
    } else {
        ESP_LOGI(TAG, "loaded: generation %u, %s, age %.0f min, health %.0f", (unsigned)s.generation,
                 pet_stage_name(s.stage), (double)s.age_min, (double)s.health);
        /* Life went on while the power was off, at half pace. */
        time_t t = time(NULL);
        if (s.last_ts && clock_valid(t) && (int64_t)t > s.last_ts) {
            float away = (float)((int64_t)t - s.last_ts) / 60.0f;
            if (away > OFFLINE_MAX_MIN) {
                away = OFFLINE_MAX_MIN;
            }
            if (away > 1) {
                ESP_LOGI(TAG, "catching up %.0f minutes away", (double)away);
                for (float left = away; left > 0; left -= 30) {
                    event_t ev = advance(left > 30 ? 30 : left, true);
                    if (ev == EV_LEFT) {
                        break;
                    }
                }
            }
        }
    }
    s_mood = compute_mood(esp_timer_get_time());
    s_last_routine_us = esp_timer_get_time();
    xTaskCreate(tick_task, "pet", 6144, NULL, 3, NULL);
}

void pet_view(pet_view_t *out)
{
    int64_t now = esp_timer_get_time();
    lock();
    out->g = s.g;
    out->stage = (pet_stage_t)s.stage;
    out->anim = current_anim(now);
    out->anim_t = (float)(now - s_anim_start_us) / 1e6f;
    out->mood = s_mood;
    for (int i = 0; i < PET_NEED_COUNT; i++) {
        out->needs[i] = (uint8_t)(s.needs[i] + 0.5f);
    }
    out->health = (uint8_t)(s.health + 0.5f);
    out->asleep = s_asleep;
    out->sick = s.sick;
    out->lights_off = s.lights_off;
    out->poops = s.poops;
    out->egg_warmth = s.egg_warmth;
    out->age_min = (uint32_t)s.age_min;
    out->generation = s.generation;
    strlcpy(out->name, s.name, sizeof(out->name));
    out->care = (uint8_t)(s.care_n > 0 ? s.care_acc / s.care_n : 60);
    out->power = (uint8_t)(s.power + 0.5f);
    out->theme = s.theme;
    unlock();
}

bool pet_feed(bool snack)
{
    lock();
    if (s.stage == PET_EGG || s_asleep || s_anim == PET_ANIM_EAT) {
        unlock();
        return false;
    }
    if (snack) {
        s.needs[PET_NEED_FOOD] = clampf(s.needs[PET_NEED_FOOD] + 15, 0, 100);
        s.needs[PET_NEED_FUN] = clampf(s.needs[PET_NEED_FUN] + 10, 0, 100);
        if (s.needs[PET_NEED_FOOD] > 90) {
            s.health = clampf(s.health - 3, 0, 100);   /* too many snacks */
        }
    } else {
        s.needs[PET_NEED_FOOD] = clampf(s.needs[PET_NEED_FOOD] + 40, 0, 100);
    }
    s.digest_min = s.digest_min > 0 ? s.digest_min : DIGEST_MIN;
    s.feeds++;
    s_dirty = true;
    set_anim(PET_ANIM_EAT, 3.0f);
    unlock();
    play(SND_EAT);
    return true;
}

bool pet_play(int score)
{
    lock();
    if (s.stage == PET_EGG || s_asleep) {
        unlock();
        return false;
    }
    score = score < 0 ? 0 : score > 10 ? 10 : score;
    s.needs[PET_NEED_FUN] = clampf(s.needs[PET_NEED_FUN] + 15 + 2 * score, 0, 100);
    s.needs[PET_NEED_BOND] = clampf(s.needs[PET_NEED_BOND] + 5 + score / 2, 0, 100);
    s.needs[PET_NEED_ENERGY] = clampf(s.needs[PET_NEED_ENERGY] - 8, 0, 100);
    s.plays++;
    s_dirty = true;
    set_anim(score >= 5 ? PET_ANIM_HAPPY : PET_ANIM_PLAY, 3.0f);
    unlock();
    play(SND_PLAY);
    return true;
}

bool pet_train(int hits, int rounds)
{
    lock();
    if (s.stage == PET_EGG || s_asleep) {
        unlock();
        return false;
    }
    hits = hits < 0 ? 0 : hits > rounds ? rounds : hits;
    s.needs[PET_NEED_FUN] = clampf(s.needs[PET_NEED_FUN] + 10 + 4 * hits, 0, 100);
    s.needs[PET_NEED_BOND] = clampf(s.needs[PET_NEED_BOND] + 3, 0, 100);
    s.needs[PET_NEED_ENERGY] = clampf(s.needs[PET_NEED_ENERGY] - 10, 0, 100);
    s.power = clampf(s.power + 4 * hits + (rounds - hits), 0, 100);
    s.plays++;
    s_dirty = true;
    bool good = hits * 2 >= rounds;
    set_anim(good ? PET_ANIM_HAPPY : PET_ANIM_SAD, 3.0f);
    unlock();
    play(good ? SND_PLAY : SND_SAD);
    return true;
}

void pet_set_theme(int theme)
{
    lock();
    s.theme = (uint8_t)(theme < 0 ? 0 : theme);
    save_now();
    unlock();
}

int pet_theme(void)
{
    return s.theme;
}

bool pet_clean(void)
{
    lock();
    if (s.stage == PET_EGG) {
        unlock();
        return false;
    }
    s.poops = 0;
    s.needs[PET_NEED_CLEAN] = 100;
    s.cleans++;
    s_dirty = true;
    set_anim(PET_ANIM_CLEAN, 2.5f);
    unlock();
    play(SND_HAPPY);
    return true;
}

bool pet_medicine(void)
{
    lock();
    if (s.stage == PET_EGG || !s.sick) {
        unlock();
        return false;
    }
    s.medicine++;
    s.health = clampf(s.health + 25, 0, 100);
    if (s.medicine >= 2) {
        s.sick = false;
        s.health = s.health < 50 ? 50 : s.health;
    }
    s_dirty = true;
    set_anim(s.sick ? PET_ANIM_SICK : PET_ANIM_HAPPY, 2.5f);
    bool cured = !s.sick;
    unlock();
    play(cured ? SND_HAPPY : SND_SAD);
    return true;
}

bool pet_lights(bool off)
{
    lock();
    if (s.stage == PET_EGG) {
        unlock();
        return false;
    }
    s.lights_off = off;
    if (!off) {
        s_awake_until_us = esp_timer_get_time() + (int64_t)20 * 60 * 1000000;
    }
    update_sleep(esp_timer_get_time());
    s_mood = compute_mood(esp_timer_get_time());
    s_dirty = true;
    unlock();
    if (off) {
        play(SND_SLEEP);
    }
    return true;
}

void pet_tap(void)
{
    int64_t now = esp_timer_get_time();
    bool happy = false, egg = false;
    lock();
    if (s.stage == PET_EGG) {
        if (now - s_last_tap_us > EGG_TAP_GAP_US) {
            s.egg_warmth = clampf(s.egg_warmth + EGG_TAP_WARMTH, 0, 1);
            s_dirty = true;
            egg = true;
        }
    } else {
        if (s_asleep && !s.lights_off) {
            s_awake_until_us = now + (int64_t)10 * 60 * 1000000;
            update_sleep(now);
        }
        s_taps_recent = now - s_last_tap_us < 3000000 ? s_taps_recent + 1 : 1;
        if (s_taps_recent <= 6) {
            s.needs[PET_NEED_BOND] = clampf(s.needs[PET_NEED_BOND] + 4, 0, 100);
            set_anim(PET_ANIM_HAPPY, 1.6f);
            happy = true;
        } else {
            set_anim(PET_ANIM_SAD, 1.5f);   /* enough */
        }
        s_mood = compute_mood(now);
        s_dirty = true;
    }
    s_last_tap_us = now;
    unlock();
    if (happy && s_taps_recent <= 2) {
        play(SND_HAPPY);
    } else if (egg) {
        const note_t n[] = { { 0, 25 } };
        chirp(n, 1, 0.5f);
    }
}

bool pet_set_name(const char *name)
{
    if (!name || !name[0]) {
        return false;
    }
    lock();
    strlcpy(s.name, name, sizeof(s.name));
    for (char *p = s.name; *p; p++) {
        if ((unsigned char)*p < 0x20 || (unsigned char)*p > 0x7e) {
            *p = '?';
        }
    }
    save_now();   /* a name is worth keeping at once */
    unlock();
    play(SND_HAPPY);
    return true;
}

bool pet_set_mood(pet_mood_t mood, int minutes)
{
    if ((int)mood < 0 || (int)mood >= (int)PET_MOOD_COUNT) {
        return false;
    }
    lock();
    s_mood_override = mood;
    s_mood_override_until_us = esp_timer_get_time() + (int64_t)(minutes < 1 ? 1 : minutes > 240 ? 240 : minutes) * 60 * 1000000;
    s_mood = compute_mood(esp_timer_get_time());
    if (mood == PET_MOOD_HAPPY) {
        set_anim(PET_ANIM_HAPPY, 3.0f);
    } else if (mood == PET_MOOD_HUNGRY || mood == PET_MOOD_LONELY || mood == PET_MOOD_BORED) {
        set_anim(PET_ANIM_SAD, 3.0f);
    }
    unlock();
    return true;
}

bool pet_evolve(int variant)
{
    lock();
    if (s.stage == PET_EGG || s.stage >= PET_ELDER) {
        unlock();
        return false;
    }
    s.next_variant = (uint8_t)(variant < 0 ? 0 : variant > 3 ? 3 : variant);
    /* Only near the end of the stage; otherwise the pick waits for it. */
    uint32_t end = STAGE_END_MIN[s.stage];
    bool now = s.age_min >= end * 3 / 4;
    if (now) {
        evolve(s.next_variant);
    }
    s_dirty = true;
    unlock();
    if (now) {
        play(SND_EVOLVE);
    }
    return now;
}

void pet_new_egg(void)
{
    lock();
    pet_genome_t parent = s.g;
    lay_egg(&parent, s.generation + 1);
    save_now();
    unlock();
}

void pet_set_time_scale(int scale)
{
    s_time_scale = scale < 1 ? 1 : scale > 200 ? 200 : scale;
}

int pet_time_scale(void)
{
    return s_time_scale;
}

void pet_say(const char *text, int secs)
{
    if (!text || !text[0]) {
        return;
    }
    strlcpy(s_last_said, text, sizeof(s_last_said));
    pet_ui_bubble(text, secs < 2 ? 2 : secs > 120 ? 120 : secs);
    play(SND_SAY);
}

int pet_status_json(char *out, size_t cap)
{
    char desc[200];
    lock();
    describe(desc, sizeof(desc));
    int n = snprintf(out, cap,
                     "{\"name\":\"%s\",\"generation\":%u,\"stage\":\"%s\",\"age_min\":%u,\"mood\":\"%s\",\"asleep\":%s,"
                     "\"sick\":%s,\"lights_off\":%s,\"health\":%d,\"needs\":{\"food\":%d,\"energy\":%d,\"fun\":%d,"
                     "\"clean\":%d,\"bond\":%d},\"poops\":%u,\"care\":%d,\"egg_warmth\":%.2f,\"looks\":\"%s\","
                     "\"traits\":%u,\"feeds\":%u,\"plays\":%u,\"cleans\":%u,\"time_scale\":%d,\"power\":%d,\"theme\":%u,\"last_said\":\"%s\"}",
                     s.name, (unsigned)s.generation, pet_stage_name(s.stage), (unsigned)s.age_min, pet_mood_name(s_mood),
                     s_asleep ? "true" : "false", s.sick ? "true" : "false", s.lights_off ? "true" : "false", (int)s.health,
                     (int)s.needs[0], (int)s.needs[1], (int)s.needs[2], (int)s.needs[3], (int)s.needs[4], (unsigned)s.poops,
                     (int)(s.care_n > 0 ? s.care_acc / s.care_n : 60), (double)s.egg_warmth, desc, (unsigned)s.g.traits,
                     (unsigned)s.feeds, (unsigned)s.plays, (unsigned)s.cleans, s_time_scale, (int)s.power, (unsigned)s.theme,
                     s_last_said);
    unlock();
    return n;
}

bool pet_debug_stage(pet_stage_t stage)
{
    if ((int)stage < 0 || stage >= PET_STAGE_COUNT) {
        return false;
    }
    lock();
    if (stage == PET_EGG) {
        pet_genome_t parent = s.g;
        lay_egg(&parent, s.generation);
    } else {
        if (s.stage == PET_EGG) {
            hatch();
        }
        s.stage = (uint8_t)stage;
        s.age_min = stage > PET_BABY ? (float)STAGE_END_MIN[stage - 1] : 0;
        s.care_acc = s.care_n = 0;
        set_anim(PET_ANIM_EVOLVE, 3.5f);
    }
    save_now();
    unlock();
    play(SND_EVOLVE);
    return true;
}

bool pet_debug_species(int species)
{
    if (species < 0 || species >= PET_SP_COUNT) {
        return false;
    }
    lock();
    pet_genome_t parent = s.g, g;
    lay_egg(&parent, s.generation);
    for (int i = 0; i < 64; i++) {
        make_genome(&g, esp_random(), NULL);
        if (g.species == species) {
            break;
        }
    }
    g.species = (uint8_t)species;
    s.g = g;
    hatch();
    s.stage = PET_ADULT;
    s.age_min = (float)STAGE_END_MIN[PET_TEEN];
    s.care_acc = s.care_n = 0;
    set_anim(PET_ANIM_IDLE, 0);
    save_now();
    unlock();
    return true;
}

const char *pet_stage_name(pet_stage_t st)
{
    static const char *const NAMES[PET_STAGE_COUNT] = { "egg", "baby", "kid", "teen", "adult", "elder" };
    return st < PET_STAGE_COUNT ? NAMES[st] : "?";
}

const char *pet_mood_name(pet_mood_t m)
{
    static const char *const NAMES[PET_MOOD_COUNT] = { "content", "happy", "hungry", "tired", "bored",
                                                       "dirty", "lonely", "sick", "asleep" };
    return m < PET_MOOD_COUNT ? NAMES[m] : "?";
}

const char *pet_need_name(pet_need_t n)
{
    static const char *const NAMES[PET_NEED_COUNT] = { "food", "energy", "fun", "clean", "bond" };
    return n < PET_NEED_COUNT ? NAMES[n] : "?";
}

/* The UI replaces this; without a screen the bubble is just a log line. */
__attribute__((weak)) void pet_ui_bubble(const char *text, int secs)
{
    (void)secs;
    ESP_LOGI(TAG, "says: %s", text);
}
