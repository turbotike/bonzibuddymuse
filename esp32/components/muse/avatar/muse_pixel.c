// Copyright (c) Meta Platforms, Inc. and affiliates.

/*
 * AVATAR: the pet (pet.h), drawn from its genome.
 *
 * A creature on the 64 px grid: a body whose shape, size, eyes, mouth, head
 * features, limbs, tail, pattern and colours all come from pet_genome_t,
 * gaining features as it grows from an egg through baby, kid, teen, adult
 * and elder. It breathes, blinks, looks about, hops when happy, chomps when
 * fed, snores Zs, sweats when sick, cries when sad, and its mouth follows
 * Muse's speech. Everything is drawn with ellipses, lines and spikes into an
 * indexed frame and outlined as one silhouette, so any genome reads as a
 * single chunky pixel character.
 */

#include "muse_pixel.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"

#include "pet.h"

#define W MUSE_PX_W
#define H MUSE_PX_H
#define FLOOR_Y 58
#define TAU 6.2831853f

enum {
    C_BG,
    C_OUT,
    C_DARK,
    C_BASE,
    C_LIGHT,
    C_SEC,
    C_SEC_L,
    C_EYE_W,
    C_IRIS,
    C_PUPIL,
    C_MOUTH,
    C_TONGUE,
    C_WHITE,
    C_SHELL,
    C_SHELL2,
    C_HEART,
    C_ZZ,
    C_FOOD,
    C_FOOD2,
    C_POOP,
    C_DROP,
    C_STAR,
    C_SHADOW,
    C_BLUSH,
    C_FLAME,
    C_COUNT
};

EXT_RAM_BSS_ATTR static uint8_t s_fb[W * H];
EXT_RAM_BSS_ATTR static uint8_t s_mask[W * H];   /* body pixels: shaded, patterned and outlined as one */
EXT_RAM_BSS_ATTR static uint8_t s_edge[W * H];
static uint16_t s_pal[C_COUNT];
static uint32_t s_pal_key = 0xffffffffu;
static pet_view_t s_v;

/* ---- colours ------------------------------------------------------------- */

static uint16_t rgb565(int r, int g, int b)
{
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    g = g < 0 ? 0 : g > 255 ? 255 : g;
    b = b < 0 ? 0 : b > 255 ? 255 : b;
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

/* h, s, v in 0..255. */
static uint16_t hsv565(int h, int s, int v)
{
    h &= 255;
    s = s < 0 ? 0 : s > 255 ? 255 : s;
    v = v < 0 ? 0 : v > 255 ? 255 : v;
    int region = h * 6 / 256;
    int f = (h * 6) % 256;
    int p = v * (255 - s) / 255;
    int q = v * (255 - s * f / 255) / 255;
    int t = v * (255 - s * (255 - f) / 255) / 255;
    int r, g, b;
    switch (region) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
    }
    return rgb565(r, g, b);
}

static void build_palette(const pet_view_t *v)
{
    const pet_genome_t *g = &v->g;
    int h = g->hue, s = g->sat, h2 = g->hue2;
    if (v->sick) {
        /* Off colour: towards a queasy green, washed out. */
        h = (h * 2 + 85) / 3;
        s = s * 2 / 3;
    }
    if (v->stage == PET_ELDER) {
        s = s / 2;   /* greying */
    }
    s_pal[C_BG] = 0;
    s_pal[C_OUT] = hsv565(h, s * 3 / 4, 38);
    s_pal[C_DARK] = hsv565(h, s, 150);
    s_pal[C_BASE] = hsv565(h, s, 215);
    s_pal[C_LIGHT] = hsv565(h, s * 3 / 4, 245);
    s_pal[C_SEC] = hsv565(h2, s, 225);
    s_pal[C_SEC_L] = hsv565(h2, s / 2, 255);
    s_pal[C_EYE_W] = rgb565(250, 250, 255);
    s_pal[C_IRIS] = hsv565(g->eye_hue, 210, 220);
    s_pal[C_PUPIL] = rgb565(12, 9, 22);
    s_pal[C_MOUTH] = rgb565(42, 18, 38);
    s_pal[C_TONGUE] = rgb565(232, 122, 154);
    s_pal[C_WHITE] = rgb565(255, 255, 255);
    s_pal[C_SHELL] = hsv565(g->hue, g->sat / 3, 240);
    s_pal[C_SHELL2] = hsv565(g->hue2, g->sat / 2, 200);
    s_pal[C_HEART] = rgb565(255, 79, 139);
    s_pal[C_ZZ] = rgb565(140, 170, 255);
    s_pal[C_FOOD] = rgb565(220, 60, 50);
    s_pal[C_FOOD2] = rgb565(80, 160, 60);
    s_pal[C_POOP] = rgb565(110, 72, 40);
    s_pal[C_DROP] = rgb565(90, 160, 255);
    s_pal[C_STAR] = rgb565(255, 220, 80);
    s_pal[C_SHADOW] = rgb565(16, 12, 28);
    s_pal[C_BLUSH] = rgb565(240, 140, 170);
    s_pal[C_FLAME] = rgb565(255, 140, 40);
}

uint32_t muse_pixel_accent(muse_mode_t mode)
{
    switch (mode) {
    case MUSE_MODE_LISTENING: return 0x5ad1ff;
    case MUSE_MODE_THINKING: return 0xffc857;
    case MUSE_MODE_SPEAKING: return 0xff7ad9;
    case MUSE_MODE_ERROR: return 0xff5c5c;
    case MUSE_MODE_BOOT:
    case MUSE_MODE_OFF: return 0x5b5277;
    default: return 0xa77dff;
    }
}

/* ---- primitives ------------------------------------------------------------ */

static inline void px(int x, int y, uint8_t c)
{
    if ((unsigned)x < W && (unsigned)y < H) {
        s_fb[y * W + x] = c;
    }
}

static inline void bpx(int x, int y, uint8_t c)
{
    if ((unsigned)x < W && (unsigned)y < H) {
        s_fb[y * W + x] = c;
        s_mask[y * W + x] = 1;
    }
}

static inline void put(int x, int y, uint8_t c, bool body)
{
    if (body) {
        bpx(x, y, c);
    } else {
        px(x, y, c);
    }
}

static int iround(float v)
{
    return (int)floorf(v + 0.5f);
}

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* A filled (super)ellipse; n = 2 is round, 4 is boxy. */
static void ellipse_n(float cx, float cy, float rx, float ry, float n, uint8_t c, bool body)
{
    if (rx < 0.5f || ry < 0.5f) {
        return;
    }
    int x0 = (int)floorf(cx - rx), x1 = (int)ceilf(cx + rx);
    int y0 = (int)floorf(cy - ry), y1 = (int)ceilf(cy + ry);
    for (int y = y0; y <= y1; y++) {
        float dy = fabsf((y + 0.5f - cy) / ry);
        for (int x = x0; x <= x1; x++) {
            float dx = fabsf((x + 0.5f - cx) / rx);
            float d = n == 2 ? dx * dx + dy * dy : powf(dx, n) + powf(dy, n);
            if (d <= 1.0f) {
                put(x, y, c, body);
            }
        }
    }
}

static void ellipse(float cx, float cy, float rx, float ry, uint8_t c, bool body)
{
    ellipse_n(cx, cy, rx, ry, 2, c, body);
}

static void line(int x0, int y0, int x1, int y1, uint8_t c, bool body)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (int i = 0; i < 256; i++) {
        put(x0, y0, c, body);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

/* A spike: base centred at (bx, by), `w` wide, rising `h` rows (negative: downwards). */
static void spike(float bx, int by, float w, int h, uint8_t c, bool body)
{
    int dir = h < 0 ? 1 : -1;
    int n = abs(h);
    for (int i = 0; i <= n; i++) {
        float hw = w * 0.5f * (1.0f - (float)i / (float)(n + 1));
        int y = by + dir * i;
        for (int x = (int)floorf(bx - hw); x <= (int)ceilf(bx + hw) - 1; x++) {
            put(x, y, c, body);
        }
    }
}

static void stamp(const char *const *rows, int nrows, int x0, int y0, uint8_t c)
{
    for (int r = 0; r < nrows; r++) {
        for (int i = 0; rows[r][i]; i++) {
            if (rows[r][i] == '1') {
                px(x0 + i, y0 + r, c);
            }
        }
    }
}

static uint32_t hash(uint32_t a, uint32_t b)
{
    uint32_t x = a * 0x9e3779b1u ^ (b + 0x7f4a7c15u) * 0x85ebca6bu;
    x ^= x >> 15;
    x *= 0x2c1b3c6du;
    x ^= x >> 12;
    return x;
}

/* Body pixels at the silhouette's edge become the outline. */
static void outline(void)
{
    uint8_t *edge = s_edge;
    memset(edge, 0, W * H);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (!s_mask[y * W + x]) {
                continue;
            }
            bool e = x == 0 || y == 0 || x == W - 1 || y == H - 1 || !s_mask[y * W + x - 1] ||
                     !s_mask[y * W + x + 1] || !s_mask[(y - 1) * W + x] || !s_mask[(y + 1) * W + x];
            edge[y * W + x] = e;
        }
    }
    for (int i = 0; i < W * H; i++) {
        if (edge[i]) {
            s_fb[i] = C_OUT;
        }
    }
}

/* Light from the upper left, shade at the lower right, on the base colour only. */
static void shade(float cx, float cy, float rx, float ry)
{
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (!s_mask[y * W + x] || s_fb[y * W + x] != C_BASE) {
                continue;
            }
            float nx = (x + 0.5f - cx) / rx, ny = (y + 0.5f - cy) / ry;
            float d = nx * 0.7f + ny;
            if (d < -0.95f) {
                s_fb[y * W + x] = C_LIGHT;
            } else if (d > 0.85f) {
                s_fb[y * W + x] = C_DARK;
            }
        }
    }
}

static void pattern(const pet_genome_t *g, float cx, float cy, float rx, float ry)
{
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint8_t c = s_fb[y * W + x];
            if (!s_mask[y * W + x] || (c != C_BASE && c != C_LIGHT && c != C_DARK)) {
                continue;
            }
            float nx = (x + 0.5f - cx) / rx, ny = (y + 0.5f - cy) / ry;
            if (nx * nx + ny * ny > 0.92f) {
                continue;   /* not out to the edge: the outline needs its rim */
            }
            switch (g->pattern) {
            case 1: {   /* belly */
                float bx = nx / 0.6f, by = (ny - 0.35f) / 0.55f;
                if (bx * bx + by * by <= 1.0f) {
                    s_fb[y * W + x] = C_SEC_L;
                }
                break;
            }
            case 2: {   /* spots, in a fixed scatter from the seed */
                for (int k = 0; k < 7; k++) {
                    uint32_t hsh = hash(g->seed, k);
                    float sx = ((hsh & 0xff) / 255.0f - 0.5f) * 1.5f, sy = (((hsh >> 8) & 0xff) / 255.0f - 0.5f) * 1.5f;
                    float r = 0.16f + ((hsh >> 16) & 0x3) * 0.03f;
                    float ddx = (nx - sx) / r, ddy = (ny - sy) / r * (rx / ry);
                    if (ddx * ddx + ddy * ddy <= 1.0f) {
                        s_fb[y * W + x] = C_SEC;
                    }
                }
                break;
            }
            case 3:   /* stripes */
                if (((int)floorf((y + 0.5f - cy) / 3.0f) & 1) == 0 && fabsf(ny) < 0.75f) {
                    s_fb[y * W + x] = C_SEC;
                }
                break;
            default:
                break;
            }
        }
    }
}

/* ---- the egg ---------------------------------------------------------------- */

static void draw_egg(const pet_view_t *v, float t)
{
    float cx = W / 2.0f, ry = 17.0f, rx = 13.0f;
    float cy = FLOOR_Y - ry;
    float shake = 0;
    if (v->anim == PET_ANIM_HATCH) {
        shake = sinf(t * 40.0f) * 1.5f;
    } else if (v->egg_warmth > 0.66f) {
        shake = sinf(t * 7.0f) * 0.8f;
    } else if (v->egg_warmth > 0.33f) {
        shake = sinf(t * 2.0f) * 0.4f;
    }
    cx += shake;
    ellipse(cx, cy + 1, rx * 0.9f, 1.5f, C_SHADOW, false);
    ellipse(cx, cy, rx, ry, C_SHELL, true);
    /* A fatter bottom. */
    ellipse(cx, cy + 3, rx * 1.05f, ry * 0.75f, C_SHELL, true);
    /* Markings from the second colour. */
    for (int k = 0; k < 6; k++) {
        uint32_t hsh = hash(v->g.seed, 100 + k);
        float sx = cx + ((hsh & 0xff) / 255.0f - 0.5f) * rx * 1.4f;
        float sy = cy + (((hsh >> 8) & 0xff) / 255.0f - 0.5f) * ry * 1.4f;
        ellipse(sx, sy, 1.6f, 1.6f, C_SHELL2, true);
    }
    outline();
    /* Cracks as it warms. */
    if (v->egg_warmth > 0.3f) {
        line(iround(cx - 3), iround(cy - 6), iround(cx), iround(cy - 2), C_OUT, false);
        line(iround(cx), iround(cy - 2), iround(cx - 2), iround(cy + 1), C_OUT, false);
    }
    if (v->egg_warmth > 0.6f) {
        line(iround(cx + 4), iround(cy - 9), iround(cx + 2), iround(cy - 4), C_OUT, false);
        line(iround(cx + 2), iround(cy - 4), iround(cx + 5), iround(cy - 1), C_OUT, false);
        line(iround(cx - 6), iround(cy + 4), iround(cx - 2), iround(cy + 6), C_OUT, false);
    }
    if (v->egg_warmth > 0.85f) {
        line(iround(cx - 1), iround(cy + 2), iround(cx + 3), iround(cy + 5), C_OUT, false);
        px(iround(cx + 1), iround(cy - 11), C_OUT);
    }
    /* Shine. */
    px(iround(cx - 4), iround(cy - 8), C_WHITE);
    px(iround(cx - 5), iround(cy - 7), C_WHITE);
}

/* ---- overlays ------------------------------------------------------------------ */

static void draw_z(int x, int y, uint8_t c)
{
    static const char *const Z[] = { "111", "001", "010", "100", "111" };
    stamp(Z, 5, x, y, c);
}

static void draw_heart(int x, int y, uint8_t c)
{
    static const char *const HEART[] = { "01010", "11111", "11111", "01110", "00100" };
    stamp(HEART, 5, x, y, c);
}

static void draw_bang(int x, int y, uint8_t c)
{
    static const char *const BANG[] = { "1", "1", "1", "1", "0", "1" };
    stamp(BANG, 6, x, y, c);
}

static void draw_question(int x, int y, uint8_t c)
{
    static const char *const Q[] = { "0110", "1001", "0001", "0010", "0100", "0000", "0100" };
    stamp(Q, 7, x, y, c);
}

static void draw_note(int x, int y, uint8_t c)
{
    static const char *const NOTE[] = { "0011", "0101", "0100", "0100", "1100", "1100" };
    stamp(NOTE, 6, x, y, c);
}

static void draw_star(int x, int y, uint8_t c)
{
    px(x, y, c);
    px(x - 1, y, c);
    px(x + 1, y, c);
    px(x, y - 1, c);
    px(x, y + 1, c);
}

static void draw_poops(int n)
{
    for (int i = 0; i < n && i < 4; i++) {
        int x = 6 + i * 8 + (i & 1) * 2;
        int y = FLOOR_Y - 1;
        ellipse(x, y, 3, 1.6f, C_POOP, false);
        ellipse(x, y - 2, 2, 1.4f, C_POOP, false);
        px(x, y - 4, C_POOP);
        px(x + 1, y - 2, C_OUT);
    }
}

/* ---- the creature ------------------------------------------------------------------ */

static const float STAGE_K[PET_STAGE_COUNT] = { 0, 0.6f, 0.75f, 0.88f, 1.0f, 0.95f };

static void draw_creature(const pet_view_t *v, const muse_pose_t *p)
{
    const pet_genome_t *g = &v->g;
    float t = p->t;
    float at = v->anim_t;
    pet_anim_t anim = v->anim;
    bool asleep = v->asleep && anim != PET_ANIM_HATCH && anim != PET_ANIM_EVOLVE;
    bool talking = p->mode == MUSE_MODE_SPEAKING;
    bool listening = p->mode == MUSE_MODE_LISTENING;
    bool thinking = p->mode == MUSE_MODE_THINKING;
    if (listening || thinking || talking) {
        asleep = false;
    }

    float k = STAGE_K[v->stage];
    if (anim == PET_ANIM_HATCH) {
        k *= clampf((at - 1.5f) / 1.5f, 0.3f, 1.0f);   /* grows into view */
    }
    float rx = g->body_w * 0.575f * k, ry = g->body_h * 0.575f * k;   /* a touch bigger than the genome says: the screen has room */
    float n = 2;
    switch (g->shape) {
    case 1: ry *= 1.25f; rx *= 0.85f; break;
    case 2: rx *= 1.25f; ry *= 0.8f; break;
    case 4: n = 3.2f; break;
    default: break;
    }
    if (rx < 4) {
        rx = 4;
    }
    if (ry < 4) {
        ry = 4;
    }
    /* Breathing, squash and bounce. */
    float breath = sinf(t * (asleep ? 1.3f : 2.2f));
    float bounce = 0;
    if (anim == PET_ANIM_HAPPY || (anim == PET_ANIM_PLAY)) {
        bounce = fabsf(sinf(at * 7.0f)) * 5.0f * k;
    }
    if (asleep) {
        ry *= 0.88f;
        rx *= 1.1f;
    }
    if (anim == PET_ANIM_SAD || v->mood == PET_MOOD_TIRED) {
        ry *= 0.95f;
    }
    if (anim == PET_ANIM_EAT) {
        float chomp = sinf(at * 9.0f);
        ry *= 1.0f + 0.04f * chomp;
        rx *= 1.0f - 0.03f * chomp;
    }
    ry *= 1.0f + 0.03f * breath;
    rx *= 1.0f - 0.02f * breath;

    /* Limbs by stage: babies have none, kids stubs, then the genome's. */
    int limb = v->stage <= PET_BABY ? 0 : v->stage == PET_KID ? (g->limb ? 1 : 0) : g->limb;
    bool legs = limb == 2 || limb == 3;
    float legs_h = legs ? 5.0f * k + 1 : limb == 1 ? 2.0f : 0.0f;
    float cx = W / 2.0f + (anim == PET_ANIM_PLAY ? sinf(at * 3.5f) * 6.0f : 0);
    float cy = FLOOR_Y - legs_h - ry - bounce;
    int head = v->stage >= PET_TEEN ? g->head : 0;
    int tail = v->stage >= PET_ADULT ? g->tail : 0;

    /* Shadow on the floor. */
    ellipse(cx, FLOOR_Y + 1.5f, rx * 0.85f, 1.6f, C_SHADOW, false);

    /* Behind: tail and wings. */
    if (tail == 1) {
        ellipse(cx + rx * 0.95f, cy + ry * 0.45f, 3.0f * k + 1, 2.5f * k + 1, C_BASE, true);
    } else if (tail == 2) {
        float sw = sinf(t * 2.3f) * 2.0f;
        for (int i = 0; i < 5; i++) {
            float f = i / 4.0f;
            ellipse(cx + rx * 0.8f + f * 9.0f * k, cy + ry * 0.4f - f * f * 9.0f * k + sw * f, 2.6f - f * 1.0f,
                    2.6f - f * 1.0f, i == 4 ? C_SEC : C_BASE, true);
        }
    } else if (tail == 3) {
        float fl = sinf(t * 13.0f) * 1.5f;
        spike(cx + rx * 0.95f, iround(cy + ry * 0.3f), 5.0f * k + 1, iround(9.0f * k + 2 + fl), C_FLAME, true);
        spike(cx + rx * 0.95f, iround(cy + ry * 0.3f), 2.5f * k + 1, iround(5.0f * k + 1 + fl), C_STAR, true);
    }
    if (limb == 5) {
        float flap = (anim == PET_ANIM_HAPPY || anim == PET_ANIM_PLAY || talking) ? fabsf(sinf(t * 12.0f)) : 0.6f + 0.1f * breath;
        ellipse(cx - rx * 0.95f, cy - ry * 0.35f, rx * 0.5f, ry * 0.4f * flap + 1, C_SEC, true);
        ellipse(cx + rx * 0.95f, cy - ry * 0.35f, rx * 0.5f, ry * 0.4f * flap + 1, C_SEC, true);
    }

    /* The body. */
    if (g->shape == 3) {   /* pear: a rounder bottom */
        ellipse(cx, cy - ry * 0.25f, rx * 0.8f, ry * 0.75f, C_BASE, true);
        ellipse(cx, cy + ry * 0.3f, rx, ry * 0.7f, C_BASE, true);
    } else {
        ellipse_n(cx, cy, rx, ry, n, C_BASE, true);
    }

    /* Head features. */
    float hy = cy - ry;
    switch (head) {
    case 1:   /* ears */
        ellipse(cx - rx * 0.55f, hy + 2, 3.0f * k + 1, 4.5f * k + 1, C_BASE, true);
        ellipse(cx + rx * 0.55f, hy + 2, 3.0f * k + 1, 4.5f * k + 1, C_BASE, true);
        break;
    case 2:   /* horns */
        spike(cx - rx * 0.45f, iround(hy + 2), 4.0f * k + 1, iround(7.0f * k + 2), C_SEC, true);
        spike(cx + rx * 0.45f, iround(hy + 2), 4.0f * k + 1, iround(7.0f * k + 2), C_SEC, true);
        break;
    case 3: {   /* antennae */
        float sw = sinf(t * 3.0f) * 1.5f;
        int top = iround(hy - 8.0f * k - 2);
        line(iround(cx - rx * 0.3f), iround(hy + 1), iround(cx - rx * 0.5f + sw), top, C_OUT, true);
        line(iround(cx + rx * 0.3f), iround(hy + 1), iround(cx + rx * 0.5f + sw), top, C_OUT, true);
        ellipse(cx - rx * 0.5f + sw, top, 1.8f, 1.8f, C_SEC, true);
        ellipse(cx + rx * 0.5f + sw, top, 1.8f, 1.8f, C_SEC, true);
        break;
    }
    case 4:   /* crest */
        for (int i = -1; i <= 1; i++) {
            spike(cx + i * rx * 0.35f, iround(hy + 2 + abs(i)), 3.5f * k + 1, iround(5.5f * k + 2 - abs(i)), C_SEC, true);
        }
        break;
    case 5:   /* fin */
        spike(cx, iround(hy + 2), 6.0f * k + 1, iround(7.0f * k + 2), C_SEC, true);
        break;
    default:
        break;
    }

    /* Limbs in front. */
    float foot_y = FLOOR_Y;
    if (legs) {
        float step = anim == PET_ANIM_PLAY ? sinf(at * 7.0f) * 2.0f : 0;
        float lw = 2.0f * k + 1.2f;
        ellipse_n(cx - rx * 0.45f, foot_y - legs_h / 2 - (step > 0 ? step : 0), lw, legs_h / 2 + 1, 3, C_DARK, true);
        ellipse_n(cx + rx * 0.45f, foot_y - legs_h / 2 - (step < 0 ? -step : 0), lw, legs_h / 2 + 1, 3, C_DARK, true);
    } else if (limb == 1) {
        ellipse(cx - rx * 0.5f, foot_y - 1.5f, 3.0f * k + 1, 2.0f, C_DARK, true);
        ellipse(cx + rx * 0.5f, foot_y - 1.5f, 3.0f * k + 1, 2.0f, C_DARK, true);
    }
    if (limb == 3) {
        float raise = (anim == PET_ANIM_HAPPY || anim == PET_ANIM_PLAY) ? ry * 0.5f : 0;
        float ay = cy + ry * 0.15f - raise;
        ellipse(cx - rx * 1.0f, ay, 2.5f * k + 1, 2.0f * k + 1, C_DARK, true);
        ellipse(cx + rx * 1.0f, ay, 2.5f * k + 1, 2.0f * k + 1, C_DARK, true);
    } else if (limb == 4) {
        for (int i = 0; i < iround(4.0f * k + 2); i++) {
            int w = iround((4.0f * k + 2 - i) * 1.2f);
            for (int x = 0; x < w; x++) {
                bpx(iround(cx - rx) - 1 - x, iround(cy + ry * 0.1f) - 2 + i, C_SEC);
                bpx(iround(cx + rx) + x, iround(cy + ry * 0.1f) - 2 + i, C_SEC);
            }
        }
    }

    /* Shading, markings, and one outline round everything. */
    shade(cx, cy, rx, ry);
    if (v->stage >= PET_ADULT) {
        pattern(g, cx, cy, rx, ry);
    }
    if (anim == PET_ANIM_EVOLVE && at < 2.2f && ((int)(at * 10) & 1)) {
        for (int i = 0; i < W * H; i++) {
            if (s_mask[i]) {
                s_fb[i] = C_WHITE;   /* the flashing silhouette */
            }
        }
    }
    outline();
    bool face = !(anim == PET_ANIM_EVOLVE && at < 2.2f);   /* no face while it changes */

    /* The face. Eyes look where the gaze wanders, or up when thinking. Their size follows
     * the body's, so a baby's eyes don't swallow its head. */
    float er = g->eye_size * 0.35f * (0.6f + 0.4f * k) * (rx / 10.0f);
    float er_max = rx * (g->eye_n == 1 ? 0.42f : g->eye_n == 2 ? 0.26f : 0.2f);
    if (er > er_max) {
        er = er_max;
    }
    if (listening) {
        er += 0.6f;
    }
    if (er < 1.3f) {
        er = 1.3f;
    }
    float ey = cy - ry * 0.22f;
    float my = cy + ry * 0.32f;
    float gx = sinf(t * 0.7f) * 0.6f + sinf(t * 0.23f) * 0.4f, gy = sinf(t * 0.5f + 1.0f) * 0.5f;
    if (!face) {
        goto overlays;
    }
    if (thinking) {
        gx = 0.8f;
        gy = -0.9f;
    } else if (anim == PET_ANIM_SAD || v->mood == PET_MOOD_TIRED) {
        gy = 0.8f;
    } else if (anim == PET_ANIM_EAT) {
        gx = 0.9f;
        gy = 0.2f;
    }
    uint32_t blink_salt = g->seed & 0xff;
    bool blink = fmodf(t + blink_salt * 0.37f, 4.3f) < 0.12f;
    bool closed = asleep || blink;
    bool arcs = anim == PET_ANIM_HAPPY || (v->mood == PET_MOOD_HAPPY && anim == PET_ANIM_IDLE && fmodf(t, 9.0f) < 3.0f);
    bool half = v->sick || (anim == PET_ANIM_SICK);
    int eyes = g->eye_n;
    for (int i = 0; i < eyes; i++) {
        float ex;
        float eyy = ey;
        if (eyes == 1) {
            ex = cx;
        } else if (eyes == 2) {
            ex = cx + (i ? 1 : -1) * (rx * 0.38f);
        } else {
            ex = cx + (i - 1) * (rx * 0.5f);
            eyy = i == 1 ? ey - er * 1.6f : ey;
        }
        if (closed) {
            line(iround(ex - er), iround(eyy), iround(ex + er), iround(eyy), C_OUT, false);
            continue;
        }
        if (arcs) {
            line(iround(ex - er), iround(eyy + 1), iround(ex), iround(eyy - er * 0.6f), C_OUT, false);
            line(iround(ex), iround(eyy - er * 0.6f), iround(ex + er), iround(eyy + 1), C_OUT, false);
            continue;
        }
        ellipse(ex, eyy, er, er, C_EYE_W, false);
        float ir = er * 0.62f;
        ellipse(ex + gx * er * 0.3f, eyy + gy * er * 0.3f, ir, ir, C_IRIS, false);
        float pr = er * 0.33f;
        ellipse(ex + gx * er * 0.4f, eyy + gy * er * 0.4f, pr < 1 ? 1 : pr, pr < 1 ? 1 : pr, C_PUPIL, false);
        px(iround(ex - er * 0.4f), iround(eyy - er * 0.4f), C_WHITE);
        if (half) {
            /* Lids half down. */
            for (int y = (int)floorf(eyy - er); y <= iround(eyy - er * 0.15f); y++) {
                for (int x = (int)floorf(ex - er); x <= (int)ceilf(ex + er); x++) {
                    float dx = (x + 0.5f - ex) / er, dy = (y + 0.5f - eyy) / er;
                    if (dx * dx + dy * dy <= 1.0f) {
                        px(x, y, C_BASE);
                    }
                }
            }
            line(iround(ex - er), iround(eyy - er * 0.15f), iround(ex + er), iround(eyy - er * 0.15f), C_OUT, false);
        }
    }
    if (arcs || anim == PET_ANIM_HAPPY) {
        px(iround(cx - rx * 0.6f), iround(ey + er + 1), C_BLUSH);
        px(iround(cx + rx * 0.6f), iround(ey + er + 1), C_BLUSH);
    }

    /* The mouth. */
    float open = 0;
    if (talking) {
        open = 1.0f + p->level * 2.5f;
    } else if (anim == PET_ANIM_EAT) {
        open = sinf(at * 9.0f) > 0 ? 2.0f : 0;
    } else if (anim == PET_ANIM_HAPPY || anim == PET_ANIM_PLAY) {
        open = 1.2f;
    }
    if (asleep) {
        px(iround(cx), iround(my), C_OUT);
        px(iround(cx + 1), iround(my), C_OUT);
    } else if (open > 0) {
        ellipse(cx, my + open * 0.5f, 2.0f + open * 0.4f, open, C_MOUTH, false);
        if (open > 1.5f) {
            px(iround(cx), iround(my + open), C_TONGUE);
            px(iround(cx + 1), iround(my + open), C_TONGUE);
        }
        if (g->mouth == 2) {
            px(iround(cx - 2), iround(my), C_WHITE);
            px(iround(cx + 2), iround(my), C_WHITE);
        }
    } else {
        switch (g->mouth) {
        case 1:   /* beak */
            spike(cx, iround(my - 1), 5.0f, -3, C_STAR, false);
            px(iround(cx - 2), iround(my - 1), C_OUT);
            px(iround(cx + 2), iround(my - 1), C_OUT);
            break;
        case 2:   /* fangs */
            line(iround(cx - 3), iround(my), iround(cx + 3), iround(my), C_OUT, false);
            px(iround(cx - 2), iround(my + 1), C_WHITE);
            px(iround(cx + 2), iround(my + 1), C_WHITE);
            break;
        case 3:   /* flat */
            line(iround(cx - 2), iround(my), iround(cx + 2), iround(my), C_OUT, false);
            break;
        default:   /* smile, or a frown when sad */
            if (anim == PET_ANIM_SAD || anim == PET_ANIM_SICK || v->mood == PET_MOOD_HUNGRY) {
                px(iround(cx - 2), iround(my + 1), C_OUT);
                px(iround(cx - 1), iround(my), C_OUT);
                px(iround(cx), iround(my), C_OUT);
                px(iround(cx + 1), iround(my), C_OUT);
                px(iround(cx + 2), iround(my + 1), C_OUT);
            } else {
                px(iround(cx - 2), iround(my - 1), C_OUT);
                px(iround(cx - 1), iround(my), C_OUT);
                px(iround(cx), iround(my), C_OUT);
                px(iround(cx + 1), iround(my), C_OUT);
                px(iround(cx + 2), iround(my - 1), C_OUT);
            }
            break;
        }
    }

overlays:
    /* Overlays. */
    if (asleep) {
        for (int i = 0; i < 3; i++) {
            float ph = fmodf(t * 3.5f + i * 5.0f, 15.0f);
            int zx = iround(cx + rx + 3 + i * 4 + ph * 0.3f), zy = iround(cy - ry - 2 - ph);
            draw_z(zx, zy, C_ZZ);
        }
    }
    if (anim == PET_ANIM_HAPPY || (anim == PET_ANIM_IDLE && v->mood == PET_MOOD_HAPPY && fmodf(t, 9.0f) < 1.5f)) {
        for (int i = 0; i < 2; i++) {
            float ph = fmodf(at * 10.0f + i * 7.0f, 16.0f);
            draw_heart(iround(cx - rx - 4 + i * (2 * rx + 6)), iround(cy - ry - 3 - ph), C_HEART);
        }
    }
    if (anim == PET_ANIM_SAD && !asleep) {
        float ph = fmodf(at * 6.0f, 6.0f);
        px(iround(cx - rx * 0.36f - er), iround(ey + er + ph), C_DROP);
        px(iround(cx - rx * 0.36f - er), iround(ey + er + ph + 1), C_DROP);
        if (v->mood == PET_MOOD_HUNGRY && fmodf(t, 1.0f) < 0.6f) {
            draw_bang(iround(cx), iround(hy - 12.0f * k - 4), C_STAR);
        }
    }
    if ((v->sick || anim == PET_ANIM_SICK) && !asleep) {
        float ph = fmodf(t * 2.0f, 5.0f);
        px(iround(cx + rx * 0.75f), iround(cy - ry * 0.6f + ph), C_DROP);
        px(iround(cx + rx * 0.75f), iround(cy - ry * 0.6f + ph + 1), C_DROP);
        px(iround(cx + rx * 0.75f + 1), iround(cy - ry * 0.6f + ph + 1), C_DROP);
    }
    if (v->needs[PET_NEED_CLEAN] < 30 && anim != PET_ANIM_CLEAN) {
        for (int i = 0; i < 4; i++) {
            uint32_t hsh = hash(g->seed, 300 + i);
            px(iround(cx + ((hsh & 0xff) / 255.0f - 0.5f) * rx * 1.2f), iround(cy + (((hsh >> 8) & 0xff) / 255.0f) * ry * 0.8f),
               C_POOP);
        }
    }
    if (anim == PET_ANIM_EAT) {
        float left = clampf(1.0f - at / 3.0f, 0, 1);
        float fr = 1.0f + 3.0f * left;
        float fx = cx + rx + 2 + fr, fy = my - 1;
        ellipse(fx, fy, fr, fr, C_FOOD, false);
        px(iround(fx), iround(fy - fr) - 1, C_FOOD2);
        px(iround(fx + 1), iround(fy - fr) - 1, C_FOOD2);
    }
    if (anim == PET_ANIM_CLEAN) {
        for (int i = 0; i < 7; i++) {
            uint32_t hsh = hash(g->seed, 400 + i);
            float ph = fmodf(at * 8.0f + (hsh & 0xf), 20.0f);
            float bx = cx + ((hsh & 0xff) / 255.0f - 0.5f) * (2 * rx + 10), by = FLOOR_Y - ph - ((hsh >> 8) & 0x7);
            px(iround(bx), iround(by - 1), C_DROP);
            px(iround(bx - 1), iround(by), C_DROP);
            px(iround(bx + 1), iround(by), C_DROP);
            px(iround(bx), iround(by + 1), C_DROP);
        }
    }
    if (anim == PET_ANIM_PLAY) {
        float ph = fmodf(at * 6.0f, 10.0f);
        draw_note(iround(cx + rx + 3), iround(cy - ry - 4 - ph), C_STAR);
    }
    if (anim == PET_ANIM_HATCH || anim == PET_ANIM_EVOLVE || (anim == PET_ANIM_PLAY && at < 1.0f)) {
        for (int i = 0; i < 8; i++) {
            uint32_t hsh = hash(g->seed, 500 + i);
            float ph = fmodf(at * 5.0f + (hsh & 0xf), 7.0f) / 7.0f;
            float a = (hsh >> 4 & 0xff) / 255.0f * TAU;
            float rr = (rx + 6) * (0.6f + ph * 0.8f);
            if (((int)(at * 8 + i) & 1) == 0) {
                draw_star(iround(cx + cosf(a) * rr), iround(cy + sinf(a) * rr * 0.9f), C_STAR);
            }
        }
    }
    if (thinking) {
        for (int i = 0; i < 3; i++) {
            if (fmodf(t * 2.0f, 3.0f) >= i) {
                ellipse(cx + rx * 0.7f + i * 4, hy - 4 - i * 3, 1.0f + i * 0.4f, 1.0f + i * 0.4f, C_WHITE, false);
            }
        }
    }
    if (listening) {
        draw_question(iround(cx + rx * 0.8f + 2), iround(hy - 12.0f * k - 3), C_STAR);
    }
    draw_poops(v->poops);
}

/* ---- the interface ---------------------------------------------------------------- */

void muse_pixel_render(const muse_pose_t *p)
{
    pet_view(&s_v);
    uint32_t key = s_v.g.seed ^ (s_v.stage << 1) ^ (s_v.sick ? 0x80000000u : 0);
    if (key != s_pal_key) {
        build_palette(&s_v);
        s_pal_key = key;
    }
    memset(s_fb, C_BG, sizeof(s_fb));
    memset(s_mask, 0, sizeof(s_mask));
    if (s_v.stage == PET_EGG) {
        draw_egg(&s_v, s_v.anim == PET_ANIM_HATCH ? s_v.anim_t : p->t);
        if (s_v.anim == PET_ANIM_LEAVE) {
            for (int i = 0; i < 6; i++) {
                float ph = fmodf(s_v.anim_t * 4.0f + i * 3.0f, 20.0f);
                draw_star(10 + i * 9, FLOOR_Y - 20 - iround(ph), C_STAR);
            }
        }
    } else {
        draw_creature(&s_v, p);
    }
}

#define MAP_MAX 512
static uint8_t s_map[MAP_MAX];
static int s_size = 256;

void muse_pixel_set_size(int px_size)
{
    s_size = px_size < MAP_MAX ? px_size : MAP_MAX;
    for (int i = 0; i < s_size; i++) {
        s_map[i] = (uint8_t)(i * W / s_size);
    }
}

void muse_pixel_scale(uint16_t *dst, int stride_px, int x0, int x1, int y0, int y1)
{
    for (int y = y0; y <= y1; y++, dst += stride_px) {
        if ((unsigned)y >= (unsigned)s_size) {
            for (int i = 0; i <= x1 - x0; i++) {
                dst[i] = 0;
            }
            continue;
        }
        const uint8_t *row = &s_fb[s_map[y] * W];
        for (int x = x0, i = 0; x <= x1; x++, i++) {
            dst[i] = (unsigned)x < (unsigned)s_size ? s_pal[row[s_map[x]]] : 0;
        }
    }
}

void muse_pixel_blank_rows(int px_size, int *top, int *bottom)
{
    *top = 0;
    *bottom = (H - FLOOR_Y - 3) * px_size / W;
}
