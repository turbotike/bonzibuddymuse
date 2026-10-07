// Copyright (c) Meta Platforms, Inc. and affiliates.

/*
 * AVATAR: the pet (pet.h), a dinosaur drawn from its genome, Digimon style.
 *
 * On the 64 px grid, side view, facing right. As a baby it is an in-training
 * blob: a round head with eyes, a mouth and a nub of tail. From kid on it is
 * a chibi dino that grows into its species: rex, raptor, long-neck, stego,
 * tri-horn, ankylo or ptero, with its crest, the plates, spikes, sail or bumps
 * on its back, its tail's club, spikes or tuft, teeth, a cream belly and its
 * markings. It breathes, blinks, looks about, hops when happy, chomps when
 * fed, snores Zs, sweats when sick, cries when sad, and its jaw follows
 * Muse's speech. Everything is ellipses, capsules and triangles into an
 * indexed frame, outlined as one silhouette, so any genome reads as one bold
 * pixel character.
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
    C_BELLY,
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
EXT_RAM_BSS_ATTR static uint8_t s_mask[W * H];   /* the silhouette: outlined as one */
EXT_RAM_BSS_ATTR static uint8_t s_edge[W * H];
static uint16_t s_pal[C_COUNT];
static uint32_t s_pal_key = 0xffffffffu;
static pet_view_t s_v;
static uint16_t s_bg;   /* the LCD's colour */

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
        h = (h * 2 + 85) / 3;   /* off colour: towards a queasy green */
        s = s * 2 / 3;
    }
    if (v->stage == PET_ELDER) {
        s = s / 2;   /* greying */
    }
    s_pal[C_BG] = s_bg;
    s_pal[C_OUT] = hsv565(h, s * 3 / 4, 36);
    s_pal[C_DARK] = hsv565(h, s, 140);
    s_pal[C_BASE] = hsv565(h, s, 215);
    s_pal[C_LIGHT] = hsv565(h, s * 3 / 4, 245);
    s_pal[C_SEC] = hsv565(h2, s, 225);
    s_pal[C_SEC_L] = hsv565(h2, s / 2, 255);
    s_pal[C_BELLY] = v->sick ? rgb565(210, 230, 190) : rgb565(248, 232, 190);
    s_pal[C_EYE_W] = rgb565(250, 250, 255);
    s_pal[C_IRIS] = hsv565(g->eye_hue, 210, 220);
    s_pal[C_PUPIL] = rgb565(12, 9, 22);
    s_pal[C_MOUTH] = rgb565(60, 18, 40);
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

static inline void put(int x, int y, uint8_t c, bool body)
{
    if ((unsigned)x < W && (unsigned)y < H) {
        s_fb[y * W + x] = c;
        if (body) {
            s_mask[y * W + x] = 1;
        }
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

static float fminf_(float a, float b)
{
    return a < b ? a : b;
}

static float fmaxf_(float a, float b)
{
    return a > b ? a : b;
}

/* A filled ellipse, turned by `ang` radians. */
static void ellipse_rot(float cx, float cy, float rx, float ry, float ang, uint8_t c, bool body)
{
    if (rx < 0.5f || ry < 0.5f) {
        return;
    }
    float r = fmaxf_(rx, ry);
    float ca = cosf(ang), sa = sinf(ang);
    int x0 = (int)floorf(cx - r), x1 = (int)ceilf(cx + r);
    int y0 = (int)floorf(cy - r), y1 = (int)ceilf(cy + r);
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            float u = (dx * ca + dy * sa) / rx, v = (-dx * sa + dy * ca) / ry;
            if (u * u + v * v <= 1.0f) {
                put(x, y, c, body);
            }
        }
    }
}

static void ellipse(float cx, float cy, float rx, float ry, uint8_t c, bool body)
{
    ellipse_rot(cx, cy, rx, ry, 0, c, body);
}

/* A thick line with round ends. */
static void capsule(float x0, float y0, float x1, float y1, float r, uint8_t c, bool body)
{
    float vx = x1 - x0, vy = y1 - y0;
    float len2 = vx * vx + vy * vy;
    int bx0 = (int)floorf(fminf_(x0, x1) - r), bx1 = (int)ceilf(fmaxf_(x0, x1) + r);
    int by0 = (int)floorf(fminf_(y0, y1) - r), by1 = (int)ceilf(fmaxf_(y0, y1) + r);
    for (int y = by0; y <= by1; y++) {
        for (int x = bx0; x <= bx1; x++) {
            float px_ = x + 0.5f - x0, py = y + 0.5f - y0;
            float t = len2 > 0 ? clampf((px_ * vx + py * vy) / len2, 0, 1) : 0;
            float dx = px_ - vx * t, dy = py - vy * t;
            if (dx * dx + dy * dy <= r * r) {
                put(x, y, c, body);
            }
        }
    }
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

/* A filled triangle. */
static void tri(float ax, float ay, float bx, float by, float cx, float cy, uint8_t c, bool body)
{
    int x0 = (int)floorf(fminf_(ax, fminf_(bx, cx))), x1 = (int)ceilf(fmaxf_(ax, fmaxf_(bx, cx)));
    int y0 = (int)floorf(fminf_(ay, fminf_(by, cy))), y1 = (int)ceilf(fmaxf_(ay, fmaxf_(by, cy)));
    float area = (bx - ax) * (cy - ay) - (cx - ax) * (by - ay);
    if (fabsf(area) < 0.01f) {
        return;
    }
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            float px_ = x + 0.5f, py = y + 0.5f;
            float w0 = ((bx - px_) * (cy - py) - (cx - px_) * (by - py)) / area;
            float w1 = ((cx - px_) * (ay - py) - (ax - px_) * (cy - py)) / area;
            float w2 = 1.0f - w0 - w1;
            if (w0 >= -0.02f && w1 >= -0.02f && w2 >= -0.02f) {
                put(x, y, c, body);
            }
        }
    }
}

/* A spike from base (bx, by), `w` wide at the base, `len` long in the direction (dx, dy). */
static void spike(float bx, float by, float dx, float dy, float len, float w, uint8_t c, bool body)
{
    float n = sqrtf(dx * dx + dy * dy);
    if (n < 0.01f) {
        return;
    }
    dx /= n;
    dy /= n;
    float px_ = -dy * w * 0.5f, py = dx * w * 0.5f;
    tri(bx + px_, by + py, bx - px_, by - py, bx + dx * len, by + dy * len, c, body);
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

/* Silhouette pixels at the edge become the outline. */
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
    ellipse(cx, cy + 3, rx * 1.05f, ry * 0.75f, C_SHELL, true);
    for (int k = 0; k < 6; k++) {
        uint32_t hsh = hash(v->g.seed, 100 + k);
        float sx = cx + ((hsh & 0xff) / 255.0f - 0.5f) * rx * 1.4f;
        float sy = cy + (((hsh >> 8) & 0xff) / 255.0f - 0.5f) * ry * 1.4f;
        ellipse(sx, sy, 1.6f, 1.6f, C_SHELL2, true);
    }
    outline();
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
    px(iround(cx - 4), iround(cy - 10), C_WHITE);
    px(iround(cx - 5), iround(cy - 9), C_WHITE);
}

/* ---- glyphs and overlays --------------------------------------------------------- */

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
        int x = 5 + i * 7 + (i & 1) * 2;
        int y = FLOOR_Y - 1;
        ellipse(x, y, 3, 1.6f, C_POOP, false);
        ellipse(x, y - 2, 2, 1.4f, C_POOP, false);
        px(x, y - 4, C_POOP);
        px(x + 1, y - 2, C_OUT);
    }
}

/* ---- the face: shared by the blob and the dino ------------------------------------ */

typedef struct {
    bool asleep, blink, arcs, half, talking, thinking, listening, sad, eating;
    float gx, gy;   /* gaze */
    float level;    /* speech level */
    float t, at;
} face_t;

static void draw_eye(float ex, float ey, float er, const face_t *f)
{
    if (f->asleep || f->blink) {
        line(iround(ex - er), iround(ey), iround(ex + er), iround(ey), C_OUT, false);
        return;
    }
    if (f->arcs) {
        line(iround(ex - er), iround(ey + 1), iround(ex), iround(ey - er * 0.7f), C_OUT, false);
        line(iround(ex), iround(ey - er * 0.7f), iround(ex + er), iround(ey + 1), C_OUT, false);
        return;
    }
    ellipse(ex, ey, er, er, C_EYE_W, false);
    float ir = er * 0.62f;
    ellipse(ex + f->gx * er * 0.3f, ey + f->gy * er * 0.3f, ir, ir, C_IRIS, false);
    float pr = er * 0.33f;
    pr = pr < 1 ? 1 : pr;
    ellipse(ex + f->gx * er * 0.4f, ey + f->gy * er * 0.4f, pr, pr, C_PUPIL, false);
    px(iround(ex - er * 0.4f), iround(ey - er * 0.4f), C_WHITE);
    if (f->half) {
        for (int y = (int)floorf(ey - er); y <= iround(ey - er * 0.15f); y++) {
            for (int x = (int)floorf(ex - er); x <= (int)ceilf(ex + er); x++) {
                float dx = (x + 0.5f - ex) / er, dy = (y + 0.5f - ey) / er;
                if (dx * dx + dy * dy <= 1.0f) {
                    px(x, y, C_BASE);
                }
            }
        }
        line(iround(ex - er), iround(ey - er * 0.15f), iround(ex + er), iround(ey - er * 0.15f), C_OUT, false);
    }
}

/* ---- the dinosaur --------------------------------------------------------------------- */

static const float STAGE_K[PET_STAGE_COUNT] = { 0, 0.55f, 0.62f, 0.8f, 1.0f, 0.95f };

typedef struct {
    float hx, hy, hrx, hry;   /* the head, for the overlays */
    float top;                /* the figure's highest row */
    float right;              /* the snout's x */
    float cx;                 /* the figure's centre */
} figure_t;

static void face_state(face_t *f, const pet_view_t *v, const muse_pose_t *p)
{
    const pet_genome_t *g = &v->g;
    pet_anim_t anim = v->anim;
    f->t = p->t;
    f->at = v->anim_t;
    f->talking = p->mode == MUSE_MODE_SPEAKING;
    f->listening = p->mode == MUSE_MODE_LISTENING;
    f->thinking = p->mode == MUSE_MODE_THINKING;
    f->asleep = v->asleep && anim != PET_ANIM_HATCH && anim != PET_ANIM_EVOLVE && !f->talking && !f->listening && !f->thinking;
    f->level = p->level;
    f->eating = anim == PET_ANIM_EAT;
    f->sad = anim == PET_ANIM_SAD || v->mood == PET_MOOD_TIRED;
    f->gx = sinf(p->t * 0.7f) * 0.6f + sinf(p->t * 0.23f) * 0.4f;
    f->gy = sinf(p->t * 0.5f + 1.0f) * 0.5f;
    if (f->thinking) {
        f->gx = 0.8f;
        f->gy = -0.9f;
    } else if (f->sad) {
        f->gy = 0.8f;
    } else if (f->eating) {
        f->gx = 0.9f;
        f->gy = 0.3f;
    }
    f->blink = fmodf(p->t + (g->seed & 0xff) * 0.37f, 4.3f) < 0.12f && !f->asleep;
    f->arcs = anim == PET_ANIM_HAPPY || (v->mood == PET_MOOD_HAPPY && anim == PET_ANIM_IDLE && fmodf(p->t, 9.0f) < 3.0f);
    f->half = v->sick || anim == PET_ANIM_SICK;
}

/* The in-training blob: a round head with a face, a nub of tail and little feet. */
static void draw_blob(const pet_view_t *v, const face_t *f, figure_t *fig)
{
    const pet_genome_t *g = &v->g;
    float t = f->t, at = f->at;
    pet_anim_t anim = v->anim;
    float k = STAGE_K[PET_BABY] * (0.9f + g->size / 255.0f * 0.3f);
    if (anim == PET_ANIM_HATCH) {
        k *= clampf((at - 1.5f) / 1.5f, 0.3f, 1.0f);
    }
    float r = 13.0f * k;
    float breath = sinf(t * (f->asleep ? 1.3f : 2.4f));
    float bounce = (anim == PET_ANIM_HAPPY || anim == PET_ANIM_PLAY) ? fabsf(sinf(at * 7.0f)) * 5.0f : 0;
    float rx = r * (1.0f - 0.03f * breath), ry = r * (1.0f + 0.04f * breath);
    if (f->asleep) {
        ry *= 0.85f;
        rx *= 1.1f;
    }
    if (f->eating) {
        float ch = sinf(at * 9.0f);
        ry *= 1.0f + 0.05f * ch;
    }
    float cx = W / 2.0f + (anim == PET_ANIM_PLAY ? sinf(at * 3.5f) * 6.0f : 0);
    float cy = FLOOR_Y - ry - bounce;
    ellipse(cx, FLOOR_Y + 1.5f, rx * 0.9f, 1.6f, C_SHADOW, false);
    /* A nub of tail to the left, little feet, and the species' hint on top. */
    ellipse(cx - rx * 0.95f, cy + ry * 0.45f, 3.0f * k + 1, 2.2f * k + 1, C_BASE, true);
    ellipse(cx - rx * 0.45f, FLOOR_Y - 1.5f - bounce, 3.0f * k + 1, 2.0f, C_DARK, true);
    ellipse(cx + rx * 0.45f, FLOOR_Y - 1.5f - bounce, 3.0f * k + 1, 2.0f, C_DARK, true);
    ellipse(cx, cy, rx, ry, C_BASE, true);
    float top = cy - ry;
    switch (g->crest) {
    case 1: spike(cx + rx * 0.5f, top + 2, 0.3f, -1, 5.0f * k + 1, 3.5f, C_SEC, true); break;
    case 2:
        spike(cx - rx * 0.35f, top + 2, -0.3f, -1, 4.5f * k + 1, 3.0f, C_SEC, true);
        spike(cx + rx * 0.35f, top + 2, 0.3f, -1, 4.5f * k + 1, 3.0f, C_SEC, true);
        break;
    case 3:
        for (int i = 0; i < 3; i++) {
            spike(cx - rx * 0.3f + i * rx * 0.3f, top + 2, -0.5f + i * 0.2f, -1, 5.0f * k + 1 - i, 3.0f, C_SEC, true);
        }
        break;
    case 4: ellipse(cx, top + 3, rx * 0.8f, 4.0f * k + 1, C_SEC, true); break;
    case 5: spike(cx - rx * 0.2f, top + 2, -0.8f, -0.7f, 7.0f * k + 1, 3.5f, C_SEC, true); break;
    default:
        if (g->species == PET_SP_STEGO || g->species == PET_SP_ANKYLO) {
            for (int i = -1; i <= 1; i++) {
                ellipse(cx + i * rx * 0.4f, top + 2, 2.0f, 2.0f, C_SEC, true);
            }
        }
        break;
    }
    if (g->species == PET_SP_PTERO) {   /* little wing stubs */
        float flap = fabsf(sinf(t * 10.0f)) * 0.5f + 0.5f;
        ellipse(cx - rx * 0.95f, cy - ry * 0.2f, 3.5f * k + 1, (2.5f * k + 1) * flap + 1, C_SEC, true);
        ellipse(cx + rx * 0.95f, cy - ry * 0.2f, 3.5f * k + 1, (2.5f * k + 1) * flap + 1, C_SEC, true);
    }
    /* The belly. */
    ellipse(cx, cy + ry * 0.45f, rx * 0.55f, ry * 0.45f, C_BELLY, true);
    outline();
    if (anim == PET_ANIM_EVOLVE && at < 2.2f) {
        if ((int)(at * 10) & 1) {
            for (int i = 0; i < W * H; i++) {
                if (s_mask[i]) {
                    s_fb[i] = C_WHITE;
                }
            }
        }
        fig->hx = cx;
        fig->hy = cy;
        fig->hrx = rx;
        fig->hry = ry;
        fig->top = top;
        fig->right = cx + rx;
        fig->cx = cx;
        return;
    }
    /* The face, 3/4 on. */
    float er = clampf(g->eye_size * 0.6f * k * 1.6f, 1.5f, rx * 0.28f);
    if (f->listening) {
        er += 0.5f;
    }
    float ey = cy - ry * 0.2f;
    if (g->eye_n == 1) {
        draw_eye(cx, ey, er * 1.3f, f);
    } else {
        draw_eye(cx - rx * 0.4f, ey, er, f);
        draw_eye(cx + rx * 0.4f, ey, er, f);
        if (g->eye_n == 3) {
            draw_eye(cx, ey - er * 1.9f, er * 0.8f, f);
        }
    }
    if (f->arcs) {
        px(iround(cx - rx * 0.65f), iround(ey + er + 1), C_BLUSH);
        px(iround(cx + rx * 0.65f), iround(ey + er + 1), C_BLUSH);
    }
    float my = cy + ry * 0.3f;
    float open = f->talking ? 1.0f + f->level * 2.5f : f->eating ? (sinf(at * 9.0f) > 0 ? 2.0f : 0) : (anim == PET_ANIM_HAPPY ? 1.2f : 0);
    if (f->asleep) {
        px(iround(cx), iround(my), C_OUT);
        px(iround(cx + 1), iround(my), C_OUT);
    } else if (open > 0) {
        ellipse(cx, my + open * 0.5f, 2.0f + open * 0.4f, open, C_MOUTH, false);
        if (open > 1.5f) {
            px(iround(cx), iround(my + open), C_TONGUE);
        }
        if (g->teeth) {
            px(iround(cx - 2), iround(my), C_WHITE);
            px(iround(cx + 2), iround(my), C_WHITE);
        }
    } else if (g->jaw == 2) {
        spike(cx, my - 1, 0, 1, 3.0f, 5.0f, C_STAR, false);
        px(iround(cx - 2), iround(my - 1), C_OUT);
        px(iround(cx + 2), iround(my - 1), C_OUT);
    } else if (f->sad || anim == PET_ANIM_SICK || v->mood == PET_MOOD_HUNGRY) {
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
        if (g->teeth == 2) {
            px(iround(cx - 1), iround(my + 1), C_WHITE);
            px(iround(cx + 1), iround(my + 1), C_WHITE);
        }
    }
    fig->hx = cx;
    fig->hy = cy;
    fig->hrx = rx;
    fig->hry = ry;
    fig->top = top - (g->crest ? 6.0f * k : 0);
    fig->right = cx + rx;
    fig->cx = cx;
}

/* The dino proper, side view facing right. */
static void draw_dino(const pet_view_t *v, const face_t *f, figure_t *fig)
{
    const pet_genome_t *g = &v->g;
    float t = f->t, at = f->at;
    pet_anim_t anim = v->anim;
    bool biped = g->species == PET_SP_REX || g->species == PET_SP_RAPTOR || g->species == PET_SP_PTERO;
    bool ptero = g->species == PET_SP_PTERO;
    float k = STAGE_K[v->stage] * (0.85f + g->size / 255.0f * 0.3f) * 1.15f;   /* the canvas has room */
    /* Chibi proportions early: a big head on a small body. */
    float head_k = v->stage == PET_KID ? 1.45f : v->stage == PET_TEEN ? 1.15f : 1.0f;
    head_k *= 0.8f + g->head_size / 255.0f * 0.4f;
    float body_k = v->stage == PET_KID ? 0.8f : 1.0f;
    float breath = sinf(t * (f->asleep ? 1.3f : 2.2f));
    float bounce = (anim == PET_ANIM_HAPPY || anim == PET_ANIM_PLAY) ? fabsf(sinf(at * 7.0f)) * 5.0f * k : 0;
    if (ptero && !f->asleep) {
        bounce += 2.0f + sinf(t * 3.0f) * 1.5f;   /* hovering a little */
    }
    float walk = anim == PET_ANIM_PLAY ? sinf(at * 7.0f) : 0;

    /* The body. */
    float brx, bry, tilt;
    switch (g->species) {
    case PET_SP_REX: brx = 11; bry = 8; tilt = -0.35f; break;
    case PET_SP_RAPTOR: brx = 10; bry = 6; tilt = -0.15f; break;
    case PET_SP_SAUROPOD: brx = 13; bry = 9; tilt = 0; break;
    case PET_SP_STEGO: brx = 13; bry = 9; tilt = 0.1f; break;
    case PET_SP_CERATOPS: brx = 12; bry = 8; tilt = 0; break;
    case PET_SP_ANKYLO: brx = 14; bry = 7; tilt = 0; break;
    default: brx = 7.5f; bry = 5; tilt = -0.5f; break;
    }
    brx *= k * body_k;
    bry *= k * body_k * (1.0f + 0.03f * breath);
    if (f->asleep) {
        bry *= 0.9f;
        tilt *= 0.5f;
    }
    float leg_h = (biped ? 9.0f : 6.5f) * k * body_k + 1.5f;
    if (ptero) {
        leg_h = 5.0f * k + 1;
    }
    float bx = 29.0f + (anim == PET_ANIM_PLAY ? sinf(at * 3.5f) * 4.0f : 0);
    float by = FLOOR_Y - leg_h - bry * 0.9f - bounce;
    float ca = cosf(tilt), sa = sinf(tilt);
    /* Points on the body: front-top (neck base), rear (tail base), underside. */
    float fx = bx + brx * 0.75f * ca - (-bry * 0.45f) * sa * 0, fy = by - bry * 0.45f + brx * 0.75f * sa;
    float rxp = bx - brx * 0.85f * ca, ryp = by + bry * 0.1f - brx * 0.85f * sa;

    /* The tail: a chain of discs from the rear, curving up and back. */
    float tail_len = (10.0f + g->tail_len / 255.0f * 14.0f) * k * (v->stage == PET_KID ? 0.6f : 1.0f);
    if (v->stage == PET_KID) {
        tail_len = fmaxf_(tail_len, 5.0f);
    }
    int segs = 7;
    float sway = sinf(t * 2.3f) * (f->asleep ? 0.3f : 1.0f);
    float tx = rxp, ty = ryp, tr0 = bry * 0.55f;
    float last_tx = tx, last_ty = ty;
    for (int i = 1; i <= segs; i++) {
        float u = (float)i / segs;
        float ang = 0.15f + u * 0.9f + sway * 0.12f * u;   /* bending up */
        float step = tail_len / segs;
        tx -= cosf(ang) * step;
        ty -= sinf(ang) * step * (g->species == PET_SP_ANKYLO ? 0.35f : g->species == PET_SP_SAUROPOD ? 0.5f : 0.8f);
        float r = tr0 * (1.0f - u * 0.8f) + 0.8f;
        capsule(last_tx, last_ty, tx, ty, r, C_BASE, true);
        last_tx = tx;
        last_ty = ty;
    }
    if (v->stage >= PET_TEEN) {
        switch (g->tail_tip) {
        case 1: ellipse(tx, ty, 3.2f * k + 1, 2.8f * k + 1, C_SEC, true); break;
        case 2:
            spike(tx, ty, -0.6f, -1, 5.0f * k + 1, 3.0f, C_SEC, true);
            spike(tx + 2.5f, ty + 1, -0.2f, -1, 4.0f * k + 1, 2.5f, C_SEC, true);
            break;
        case 3: ellipse(tx - 1, ty - 1, 3.0f * k + 1, 2.0f * k + 1, C_SEC, true); break;
        default: break;
        }
    }

    /* The far legs, and the far wing. */
    float leg_r = (biped ? 2.4f : 2.0f) * k * body_k + 0.6f;
    float foot_y = FLOOR_Y - bounce;
    float hip_x = biped ? bx - brx * 0.1f : bx - brx * 0.55f, front_x = bx + brx * 0.55f;
    if (ptero) {
        float flap = f->asleep ? 0.2f : (anim == PET_ANIM_HAPPY || anim == PET_ANIM_PLAY) ? sinf(t * 14.0f) : sinf(t * 5.0f) * 0.6f;
        float wl = 16.0f * k + 4;
        tri(bx - brx * 0.2f, by - bry * 0.4f, bx - brx * 0.2f - wl * 0.9f, by - bry * 0.4f - wl * (0.5f + flap * 0.5f), bx - brx * 0.9f, by,
            C_SEC, true);
    }
    capsule(hip_x + 1.5f, by + bry * 0.3f, hip_x + 1.5f + walk * 2, foot_y - 1.5f, leg_r, C_DARK, true);
    ellipse(hip_x + 3.0f + walk * 2, foot_y - 1.2f, leg_r * 1.5f + 0.5f, 1.4f, C_DARK, true);
    if (!biped) {
        capsule(front_x + 1.5f, by + bry * 0.3f, front_x + 1.5f - walk * 2, foot_y - 1.5f, leg_r, C_DARK, true);
        ellipse(front_x + 3.0f - walk * 2, foot_y - 1.2f, leg_r * 1.5f + 0.5f, 1.4f, C_DARK, true);
    }

    /* The body itself, with its back features on top. */
    ellipse_rot(bx, by, brx, bry, tilt, C_BASE, true);
    if (v->stage >= PET_TEEN) {
        int nb = v->stage == PET_TEEN ? 4 : 6;
        for (int i = 0; i < nb; i++) {
            float u = -0.7f + 1.3f * i / (nb - 1);
            /* A point on the body's top edge. */
            float ox = bx + u * brx * ca - (-bry) * sa, oy = by - bry * ca + u * brx * sa;
            float nx = -sa * 0 + sinf(tilt) * 0, ny = -1;
            (void)nx;
            switch (g->back) {
            case 1: spike(ox, oy + 1, 0.15f, ny, 4.5f * k + 1, 2.6f, C_SEC, true); break;
            case 2: {
                float h = (4.0f + 3.0f * sinf(u * 2.0f + 1.6f)) * k + 1;
                tri(ox - 2.2f, oy + 1, ox + 2.2f, oy + 1, ox + 0.3f, oy - h, C_SEC, true);
                break;
            }
            case 3: {
                float h = (7.0f + 5.0f * cosf(u * 1.8f)) * k + 1;
                tri(ox - 2.5f, oy + 1, ox + 2.5f, oy + 1, ox, oy - h, C_SEC, true);
                break;
            }
            case 4: ellipse(ox, oy + 0.5f, 1.8f, 1.5f, C_SEC, true); break;
            default: break;
            }
        }
    }
    /* The belly. */
    ellipse_rot(bx + brx * 0.15f, by + bry * 0.35f, brx * 0.62f, bry * 0.5f, tilt, C_BELLY, true);

    /* The neck and head. */
    float neck_len = (3.0f + g->neck / 255.0f * 16.0f) * k * (v->stage == PET_KID ? 0.5f : 1.0f);
    float neck_r = (bry * 0.45f) * (g->species == PET_SP_SAUROPOD ? 0.7f : 1.0f) + 0.5f;
    float nang = g->species == PET_SP_SAUROPOD ? -1.2f : biped ? -0.9f : -0.6f;   /* up and forward */
    if (f->asleep) {
        nang += 0.5f;
    }
    if (f->eating) {
        nang += 0.25f + (sinf(at * 9.0f) > 0 ? 0.1f : 0);
    }
    float hx = fx + cosf(nang) * neck_len, hy = fy + sinf(nang) * neck_len - 0.5f * breath * 0.3f;
    float hrx = (g->species == PET_SP_SAUROPOD ? 4.0f : g->species == PET_SP_CERATOPS ? 7.5f : 6.0f) * k * head_k;
    float hry = hrx * (g->species == PET_SP_RAPTOR || g->species == PET_SP_PTERO ? 0.62f : 0.78f);
    capsule(fx, fy, hx, hy, neck_r, C_BASE, true);
    if (g->crest == 4 && v->stage >= PET_TEEN) {   /* the frill, behind the head */
        ellipse(hx - hrx * 0.5f, hy - hry * 0.3f, hrx * 1.15f, hry * 1.35f, C_SEC, true);
        ellipse(hx - hrx * 0.5f, hy - hry * 0.3f, hrx * 0.85f, hry * 1.0f, C_BASE, true);
    }
    ellipse(hx, hy, hrx, hry, C_BASE, true);
    /* The snout and jaw. */
    float sx = hx + hrx * 0.75f, sy = hy + hry * 0.15f;
    float srx = hrx * (g->jaw == 1 ? 0.95f : 0.6f), sry = hry * 0.55f;
    float open = f->talking ? 0.8f + f->level * 2.2f : f->eating ? (sinf(at * 9.0f) > 0 ? 2.2f : 0) : (anim == PET_ANIM_HAPPY ? 0.8f : 0);
    if (f->asleep) {
        open = 0;
    }
    if (g->jaw == 2) {
        /* A beak: upper and lower, in the second colour. */
        tri(sx - srx * 0.4f, sy - sry, sx + srx * 1.3f, sy + 0.5f, sx - srx * 0.4f, sy + sry * 0.4f, C_STAR, true);
        tri(sx - srx * 0.4f, sy + sry * 0.2f + open, sx + srx * 1.0f, sy + sry * 0.4f + open, sx - srx * 0.4f, sy + sry * 1.1f + open,
            C_STAR, true);
    } else {
        ellipse(sx, sy, srx, sry, C_BASE, true);   /* the upper jaw */
        ellipse(sx - srx * 0.15f, sy + sry * 0.75f + open, srx * 0.85f, sry * 0.5f + 0.4f, C_BASE, true);   /* the lower jaw */
    }
    /* Head crest. */
    float top = hy - hry;
    if (v->stage >= PET_TEEN) {
        switch (g->crest) {
        case 1: spike(sx + srx * 0.2f, sy - sry + 1, 0.2f, -1, 5.0f * k + 1, 3.0f, C_SEC, true); break;
        case 2:
            spike(hx + hrx * 0.1f, top + 1.5f, 0.35f, -1, 6.0f * k + 1, 3.0f, C_SEC, true);
            spike(hx + hrx * 0.5f, top + 2.5f, 0.45f, -1, 5.0f * k + 1, 2.6f, C_SEC, true);
            break;
        case 3:
            for (int i = 0; i < 3; i++) {
                spike(hx - hrx * 0.5f + i * hrx * 0.35f, top + 1.5f + i * 0.5f, -0.7f + i * 0.25f, -1, 6.0f * k + 1 - i * 0.6f, 2.8f,
                      C_SEC, true);
            }
            break;
        case 4:
            spike(hx + hrx * 0.25f, top + 1.5f, 0.3f, -1, 6.0f * k + 1, 2.8f, C_SEC, true);
            spike(sx + srx * 0.2f, sy - sry + 1, 0.2f, -1, 3.5f * k + 1, 2.4f, C_SEC, true);
            break;
        case 5: spike(hx - hrx * 0.4f, top + 1.5f, -0.85f, -0.5f, 10.0f * k + 2, 4.0f, C_SEC, true); break;
        default: break;
        }
    }
    /* The near legs, the arms, the near wing. */
    capsule(hip_x - 1.5f, by + bry * 0.3f, hip_x - 1.5f - walk * 2, foot_y - 1.5f, leg_r, C_BASE, true);
    ellipse(hip_x + 0.5f - walk * 2, foot_y - 1.2f, leg_r * 1.6f + 0.5f, 1.5f, C_BASE, true);
    if (!biped) {
        capsule(front_x - 1.5f, by + bry * 0.3f, front_x - 1.5f + walk * 2, foot_y - 1.5f, leg_r, C_BASE, true);
        ellipse(front_x + 0.5f + walk * 2, foot_y - 1.2f, leg_r * 1.6f + 0.5f, 1.5f, C_BASE, true);
    } else if (!ptero) {
        float raise = (anim == PET_ANIM_HAPPY || anim == PET_ANIM_PLAY) ? -3.0f * k : 0;
        float ax = bx + brx * 0.55f, ay = by - bry * 0.1f;
        capsule(ax, ay, ax + 3.0f * k + 1.5f, ay + 3.0f * k + 1 + raise, 1.4f * k + 0.5f, C_BASE, true);
    } else {
        float flap = f->asleep ? 0.2f : (anim == PET_ANIM_HAPPY || anim == PET_ANIM_PLAY) ? sinf(t * 14.0f) : sinf(t * 5.0f) * 0.6f;
        float wl = 15.0f * k + 4;
        tri(bx + brx * 0.1f, by - bry * 0.3f, bx + brx * 0.1f - wl * 0.7f, by - bry * 0.3f - wl * (0.45f + flap * 0.5f), bx - brx * 0.6f,
            by + bry * 0.4f, C_SEC, true);
    }

    /* Markings, then the outline round everything. */
    if (v->stage >= PET_ADULT && g->pattern) {
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                if (!s_mask[y * W + x] || s_fb[y * W + x] != C_BASE) {
                    continue;
                }
                float dx = x + 0.5f - bx, dy = y + 0.5f - by;
                float u = (dx * ca + dy * sa) / brx, vv = (-dx * sa + dy * ca) / bry;
                bool on_body = u * u + vv * vv <= 0.85f;
                bool on_tail = x < bx - brx * 0.6f && y < FLOOR_Y - leg_h + 2;
                if (!on_body && !on_tail) {
                    continue;
                }
                if (g->pattern == 1) {
                    for (int kx = 0; kx < 8; kx++) {
                        uint32_t hsh = hash(g->seed, kx);
                        float spx = bx + ((hsh & 0xff) / 255.0f - 0.5f) * (brx * 2.4f) - brx * 0.3f;
                        float spy = by + (((hsh >> 8) & 0xff) / 255.0f - 0.5f) * (bry * 1.6f) - bry * 0.2f;
                        float r = 1.3f + ((hsh >> 16) & 0x3) * 0.4f;
                        if ((x + 0.5f - spx) * (x + 0.5f - spx) + (y + 0.5f - spy) * (y + 0.5f - spy) <= r * r) {
                            s_fb[y * W + x] = C_SEC;
                        }
                    }
                } else if (((int)floorf((x + 0.5f - bx + (y - by) * 0.3f) / 3.0f) & 1) == 0 && vv < 0.35f) {
                    s_fb[y * W + x] = C_SEC;
                }
            }
        }
    }
    outline();
    /* Claws and teeth, over the outline. */
    px(iround(hip_x + 1.5f + leg_r * 1.6f - walk * 2), iround(foot_y - 1), C_WHITE);
    if (!biped) {
        px(iround(front_x + 1.5f + leg_r * 1.6f + walk * 2), iround(foot_y - 1), C_WHITE);
    }
    if (anim == PET_ANIM_EVOLVE && at < 2.2f) {
        if ((int)(at * 10) & 1) {
            for (int i = 0; i < W * H; i++) {
                if (s_mask[i]) {
                    s_fb[i] = C_WHITE;
                }
            }
        }
    } else {
        if (g->jaw != 2 && (g->teeth || open > 0.5f)) {
            int ty0 = iround(sy + sry * 0.35f);
            if (open > 0.5f) {
                ellipse(sx - srx * 0.1f, sy + sry * 0.4f + open * 0.5f, srx * 0.7f, open * 0.45f + 0.5f, C_MOUTH, false);
                if (open > 1.5f) {
                    px(iround(sx - srx * 0.3f), iround(sy + sry * 0.4f + open * 0.8f), C_TONGUE);
                }
            }
            if (g->teeth && !f->asleep) {
                int n = g->teeth == 2 ? 3 : 2;
                for (int i = 0; i < n; i++) {
                    int tx_ = iround(sx + srx * 0.7f - i * (srx * 0.6f / n) * 1.3f);
                    px(tx_, ty0, C_WHITE);
                    if (g->teeth == 2 && i == 0) {
                        px(tx_, ty0 + 1, C_WHITE);
                    }
                }
            }
        }
        /* The eye(s). */
        float er = clampf(g->eye_size * 0.42f * k * head_k * 1.3f, 1.3f, hry * 0.42f);
        if (f->listening) {
            er += 0.5f;
        }
        float ex = hx + hrx * 0.3f, ey = hy - hry * 0.15f;
        draw_eye(ex, ey, er, f);
        if (g->eye_n >= 2 && hrx > 5) {
            draw_eye(hx - hrx * 0.45f, ey - hry * 0.1f, er * 0.75f, f);   /* the far eye, peeking round */
        }
        if (g->eye_n == 3) {
            draw_eye(hx, top + er * 1.1f, er * 0.7f, f);
        }
        if (f->arcs) {
            px(iround(ex + er + 1), iround(ey + er), C_BLUSH);
        }
    }
    fig->hx = hx;
    fig->hy = hy;
    fig->hrx = hrx;
    fig->hry = hry;
    fig->top = fminf_(top - (v->stage >= PET_TEEN && g->crest ? 6.0f * k + 1 : 0), by - bry - (v->stage >= PET_TEEN && g->back ? 8.0f * k : 0));
    fig->right = sx + srx;
    fig->cx = bx;
}

static void draw_overlays(const pet_view_t *v, const face_t *f, const figure_t *fig)
{
    const pet_genome_t *g = &v->g;
    pet_anim_t anim = v->anim;
    float t = f->t, at = f->at;
    if (f->asleep) {
        for (int i = 0; i < 3; i++) {
            float ph = fmodf(t * 3.5f + i * 5.0f, 15.0f);
            draw_z(iround(fig->hx + fig->hrx + 3 + i * 4 + ph * 0.3f), iround(fig->hy - fig->hry - 2 - ph), C_ZZ);
        }
    }
    if (anim == PET_ANIM_HAPPY || (anim == PET_ANIM_IDLE && v->mood == PET_MOOD_HAPPY && fmodf(t, 9.0f) < 1.5f)) {
        for (int i = 0; i < 2; i++) {
            float ph = fmodf(at * 10.0f + i * 7.0f, 16.0f);
            draw_heart(iround(fig->hx - fig->hrx - 6 + i * (2 * fig->hrx + 8)), iround(fig->top - 3 - ph), C_HEART);
        }
    }
    if (anim == PET_ANIM_SAD && !f->asleep) {
        float ph = fmodf(at * 6.0f, 6.0f);
        px(iround(fig->hx + fig->hrx * 0.3f + 2), iround(fig->hy + ph), C_DROP);
        px(iround(fig->hx + fig->hrx * 0.3f + 2), iround(fig->hy + ph + 1), C_DROP);
        if (v->mood == PET_MOOD_HUNGRY && fmodf(t, 1.0f) < 0.6f) {
            draw_bang(iround(fig->hx), iround(fig->top - 9), C_STAR);
        }
    }
    if ((v->sick || anim == PET_ANIM_SICK) && !f->asleep) {
        float ph = fmodf(t * 2.0f, 5.0f);
        int sx = iround(fig->hx - fig->hrx - 2), sy = iround(fig->hy - fig->hry * 0.5f + ph);
        px(sx, sy, C_DROP);
        px(sx, sy + 1, C_DROP);
        px(sx + 1, sy + 1, C_DROP);
    }
    if (v->needs[PET_NEED_CLEAN] < 30 && anim != PET_ANIM_CLEAN) {
        for (int i = 0; i < 4; i++) {
            uint32_t hsh = hash(g->seed, 300 + i);
            px(iround(fig->cx + ((hsh & 0xff) / 255.0f - 0.5f) * 14), iround(FLOOR_Y - 8 - (((hsh >> 8) & 0xff) / 255.0f) * 8), C_POOP);
        }
    }
    if (anim == PET_ANIM_EAT) {
        float left = clampf(1.0f - at / 3.0f, 0, 1);
        float fr = 1.0f + 3.0f * left;
        float fx = fig->right + 2 + fr, fy = fig->hy + fig->hry * 0.3f;
        ellipse(fx, fy, fr, fr, C_FOOD, false);
        px(iround(fx), iround(fy - fr) - 1, C_FOOD2);
        px(iround(fx + 1), iround(fy - fr) - 1, C_FOOD2);
    }
    if (anim == PET_ANIM_CLEAN) {
        for (int i = 0; i < 7; i++) {
            uint32_t hsh = hash(g->seed, 400 + i);
            float ph = fmodf(at * 8.0f + (hsh & 0xf), 20.0f);
            float bx = fig->cx + ((hsh & 0xff) / 255.0f - 0.5f) * 40, by = FLOOR_Y - ph - ((hsh >> 8) & 0x7);
            px(iround(bx), iround(by - 1), C_DROP);
            px(iround(bx - 1), iround(by), C_DROP);
            px(iround(bx + 1), iround(by), C_DROP);
            px(iround(bx), iround(by + 1), C_DROP);
        }
    }
    if (anim == PET_ANIM_PLAY) {
        float ph = fmodf(at * 6.0f, 10.0f);
        draw_note(iround(fig->right + 2), iround(fig->top - 4 - ph), C_STAR);
    }
    if (anim == PET_ANIM_HATCH || anim == PET_ANIM_EVOLVE || (anim == PET_ANIM_PLAY && at < 1.0f)) {
        for (int i = 0; i < 8; i++) {
            uint32_t hsh = hash(g->seed, 500 + i);
            float ph = fmodf(at * 5.0f + (hsh & 0xf), 7.0f) / 7.0f;
            float a = (hsh >> 4 & 0xff) / 255.0f * TAU;
            float rr = 18.0f * (0.6f + ph * 0.8f);
            if (((int)(at * 8 + i) & 1) == 0) {
                draw_star(iround(fig->cx + cosf(a) * rr), iround((fig->top + FLOOR_Y) * 0.5f + sinf(a) * rr * 0.8f), C_STAR);
            }
        }
    }
    if (f->thinking) {
        for (int i = 0; i < 3; i++) {
            if (fmodf(t * 2.0f, 3.0f) >= i) {
                ellipse(fig->hx + fig->hrx * 0.5f + i * 4, fig->top - 4 - i * 3, 1.0f + i * 0.4f, 1.0f + i * 0.4f, C_WHITE, false);
            }
        }
    }
    if (f->listening) {
        draw_question(iround(fig->hx + fig->hrx * 0.6f + 2), iround(fig->top - 9), C_STAR);
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
        return;
    }
    face_t f;
    face_state(&f, &s_v, p);
    figure_t fig;
    if (s_v.stage == PET_BABY) {
        draw_blob(&s_v, &f, &fig);
    } else {
        ellipse(29.0f, FLOOR_Y + 1.5f, 16.0f, 1.8f, C_SHADOW, false);
        draw_dino(&s_v, &f, &fig);
    }
    draw_overlays(&s_v, &f, &fig);
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
                dst[i] = s_bg;
            }
            continue;
        }
        const uint8_t *row = &s_fb[s_map[y] * W];
        for (int x = x0, i = 0; x <= x1; x++, i++) {
            dst[i] = (unsigned)x < (unsigned)s_size ? s_pal[row[s_map[x]]] : s_bg;
        }
    }
}

void muse_pixel_set_background(uint16_t rgb565)
{
    s_bg = rgb565;
    s_pal_key = 0xffffffffu;   /* rebuild the palette */
}

void muse_pixel_blank_rows(int px_size, int *top, int *bottom)
{
    *top = 0;
    *bottom = (H - FLOOR_Y - 3) * px_size / W;
}
