// Copyright (c) Meta Platforms, Inc. and affiliates.

/*
 * AVATAR: BonziBuddy — the cheerful purple gorilla desktop buddy.
 *
 * The year-2000 sidekick, reborn in pixels: a chunky purple gorilla with a
 * big round head, small round ears, and a lighter lavender-grey face panel,
 * belly and paws. Big friendly eyes with white sclera, a heavy dark brow,
 * and a wide goofy grin with a pink tongue peeking out. Short stubby legs,
 * long arms, standing at about 34 px wide and 47 px tall.
 * Colours: deep/mid/light purple fur with a near-black purple outline,
 * lavender face tones, dark pupil, white teeth.
 * Personality: pure desktop-buddy mischief — he waves hello on boot, cups
 * his ears to listen, scratches his chin with a paw while he thinks, flaps
 * his jaw when he talks, and bounces with joy (hearts and all) when petted.
 */

#include "muse_pixel.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define W MUSE_PX_W
#define H MUSE_PX_H
#define TAU 6.2831853f

/* ---------------------------------------------------------------------------
 * Palette
 * ------------------------------------------------------------------------- */

enum {
    C_BG = 0,
    C_OUT,       /* outline (near-black purple) */
    C_OUT2,      /* soft outline where parts tuck */
    C_BD,        /* fur dark */
    C_BM,        /* fur mid */
    C_BL,        /* fur light */
    C_BH,        /* fur highlight */
    C_RIM,       /* state-tinted rim light */
    C_SKIND,     /* face/belly shade (lavender) */
    C_SKIN,      /* face/belly */
    C_SKINL,     /* face/belly light */
    C_IRIS,      /* pupil */
    C_SHINE,
    C_BROW,      /* heavy brow */
    C_BLUSH,
    C_BLUSHD,
    C_MOUTH,
    C_TEETH,
    C_TONGUE,
    C_G0,        /* state glow ramp, bright ... */
    C_G1,
    C_G2,
    C_G3,        /* ... deep */
    C_AURA1,
    C_AURA2,
    C_SPK,
    C_ACC,
    C_SHADOW,
    C_HEART,
    C_WHITE,
    C_COUNT,     /* 31 */
};

typedef struct {
    float r, g, b;
} rgb_t;

/* Per-mode glow ramp (bright -> deep) and accent. */
typedef struct {
    uint32_t f[4];
    uint32_t acc;
} scheme_t;

static const scheme_t SCHEMES[MUSE_MODE_COUNT] = {
    [MUSE_MODE_BOOT]      = { { 0xffffff, 0xcfe0ff, 0x8fa8ff, 0x5a5fe0 }, 0xa9c0ff },
    [MUSE_MODE_IDLE]      = { { 0xf4e8ff, 0xc7a4ff, 0x9a6bff, 0x5b3fd9 }, 0xa77dff },
    [MUSE_MODE_LISTENING] = { { 0xe8faff, 0x8fdcff, 0x3fa2ff, 0x2a5bd7 }, 0x5cb8ff },
    [MUSE_MODE_THINKING]  = { { 0xffe6ff, 0xff9cf0, 0xd35bff, 0x7a2bd9 }, 0xe07bff },
    [MUSE_MODE_SPEAKING]  = { { 0xeafff4, 0x9ff5cf, 0x3fd9a0, 0x1f9a7a }, 0x6ff0bf },
    [MUSE_MODE_ERROR]     = { { 0xffd6d6, 0xff6b6b, 0xc7304a, 0x6b1a3a }, 0xff5c5c },
    [MUSE_MODE_OFF]       = { { 0xd8d4ff, 0x8f86d9, 0x5a4fb0, 0x2e2870 }, 0x7c72d0 },
};

/* Purple fur, lavender face. */
static const uint32_t FIXED[C_COUNT] = {
    [C_BG] = 0x000000,
    [C_OUT] = 0x160a30,
    [C_OUT2] = 0x7a5fa8,
    [C_BD] = 0x4a2c86,
    [C_BM] = 0x6f4fb0,
    [C_BL] = 0x9678d6,
    [C_BH] = 0xbba6ec,
    [C_SKIND] = 0xa891d4,
    [C_SKIN] = 0xc3b3e8,
    [C_SKINL] = 0xdbd0f6,
    [C_IRIS] = 0x0c0916,
    [C_SHINE] = 0xffffff,
    [C_BROW] = 0x241243,
    [C_BLUSH] = 0xe8a0c8,
    [C_BLUSHD] = 0xd97fae,
    [C_MOUTH] = 0x2a1226,
    [C_TEETH] = 0xffffff,
    [C_TONGUE] = 0xe87a9a,
    [C_SHADOW] = 0x101018,
    [C_HEART] = 0xff4f8b,
    [C_WHITE] = 0xffffff,
};

static rgb_t s_scheme[5];      /* live, blended: f0..f3, acc */
static bool s_scheme_init;
static uint16_t s_pal[C_COUNT];
static uint16_t s_pal_dim[C_COUNT];

static uint8_t s_fb[W * H];
static uint8_t s_mask[W * H];

static const uint8_t BAYER4[4][4] = {
    { 0, 8, 2, 10 },
    { 12, 4, 14, 6 },
    { 3, 11, 1, 9 },
    { 15, 7, 13, 5 },
};

/*
 * The per-pixel work is fixed point (Q12: ONE = 1.0), with tables for the
 * powers and roots: chips without an FPU (ESP32-C6) emulate float in
 * software, which made a frame take 250 ms. At 64 px the quantisation is
 * invisible.
 */
#define Q 12
#define ONE (1 << Q)
#define QF(v) ((int32_t)((v) * ONE))
#define POW_LUT_N 256
#define POW_LUT_MAX_Q QF(1.2f)          /* se_hit() bails out beyond this */
#define SQRT_LUT_N 1024                 /* indexed by x >> 2 */
static int16_t s_pow_dome[POW_LUT_N + 1];   /* |u|^2.7 */
static int16_t s_sqrt[SQRT_LUT_N + 1];

static void init_luts(void)
{
    for (int i = 0; i <= POW_LUT_N; i++) {
        float u = (float)i * POW_LUT_MAX_Q / POW_LUT_N / ONE;
        s_pow_dome[i] = (int16_t)(powf(u, 2.7f) * ONE);
    }
    for (int i = 0; i <= SQRT_LUT_N; i++) {
        s_sqrt[i] = (int16_t)(sqrtf((float)i / SQRT_LUT_N) * ONE);
    }
}

/* a in [0, POW_LUT_MAX_Q] */
static inline int32_t pow_q(const int16_t *lut, int32_t a)
{
    return lut[(a * (POW_LUT_N * 65536 / POW_LUT_MAX_Q)) >> 16];
}

/* sqrt of x in [0, ONE] */
static inline int32_t sqrt_q(int32_t x)
{
    return s_sqrt[x >> 2];
}

static inline int32_t bayer_q(int x, int y)
{
    return BAYER4[y & 3][x & 3] * (ONE / 16) + ONE / 32;
}

static inline float bayer(int x, int y)
{
    return (BAYER4[y & 3][x & 3] + 0.5f) / 16.0f;
}

static inline int32_t qdiv(int32_t a, int32_t b)
{
    return b == 0 ? (a >= 0 ? INT32_MAX : INT32_MIN) : (int32_t)(((int64_t)a << Q) / b);
}

static inline rgb_t hex_rgb(uint32_t c)
{
    return (rgb_t){ (float)((c >> 16) & 0xff), (float)((c >> 8) & 0xff), (float)(c & 0xff) };
}

static inline rgb_t mix(rgb_t a, rgb_t b, float t)
{
    return (rgb_t){ a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t };
}

static inline rgb_t scale_rgb(rgb_t a, float k)
{
    return (rgb_t){ a.r * k, a.g * k, a.b * k };
}

static inline uint16_t to565(rgb_t c)
{
    int r = (int)(c.r + 0.5f), g = (int)(c.g + 0.5f), b = (int)(c.b + 0.5f);
    r = r < 0 ? 0 : (r > 255 ? 255 : r);
    g = g < 0 ? 0 : (g > 255 ? 255 : g);
    b = b < 0 ? 0 : (b > 255 ? 255 : b);
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

uint32_t muse_pixel_accent(muse_mode_t mode)
{
    return SCHEMES[mode < MUSE_MODE_COUNT ? mode : MUSE_MODE_IDLE].acc;
}

static void update_palette(const scheme_t *target, float dt)
{
    rgb_t tgt[5];
    for (int i = 0; i < 4; i++) {
        tgt[i] = hex_rgb(target->f[i]);
    }
    tgt[4] = hex_rgb(target->acc);

    float k = s_scheme_init ? 1.0f - expf(-dt * 7.0f) : 1.0f;
    for (int i = 0; i < 5; i++) {
        s_scheme[i] = mix(s_scheme[i], tgt[i], k);
    }
    s_scheme_init = true;

    rgb_t pal[C_COUNT];
    for (int i = 0; i < C_COUNT; i++) {
        pal[i] = hex_rgb(FIXED[i]);
    }
    rgb_t acc = s_scheme[4];
    pal[C_G0] = s_scheme[0];
    pal[C_G1] = s_scheme[1];
    pal[C_G2] = s_scheme[2];
    pal[C_G3] = s_scheme[3];
    pal[C_ACC] = acc;
    pal[C_RIM] = mix(pal[C_BL], acc, 0.45f);
    pal[C_AURA1] = scale_rgb(acc, 0.16f);
    pal[C_AURA2] = scale_rgb(acc, 0.34f);
    pal[C_SPK] = mix(acc, pal[C_WHITE], 0.45f);

    for (int i = 0; i < C_COUNT; i++) {
        s_pal[i] = to565(pal[i]);
        /* The block edge shade gives the enlarged pixels a faint grid texture. */
        s_pal_dim[i] = to565(scale_rgb(pal[i], 0.72f));
    }
}

/* ---------------------------------------------------------------------------
 * Primitive helpers
 * ------------------------------------------------------------------------- */

static inline void px(int x, int y, uint8_t c)
{
    if ((unsigned)x < W && (unsigned)y < H) {
        s_fb[y * W + x] = c;
    }
}

static inline uint8_t get_px(int x, int y)
{
    if ((unsigned)x < W && (unsigned)y < H) {
        return s_fb[y * W + x];
    }
    return C_BG;
}

static inline int iround(float v)
{
    return (int)floorf(v + 0.5f);
}

static inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline float fracf(float v)
{
    return v - floorf(v);
}

/* Cheap deterministic pseudo-random for idle behaviour. */
static uint32_t s_rng = 0x9e3779b9u;
static float frand(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return (float)(s_rng & 0xffffff) / (float)0x1000000;
}

/* Draw a small bitmap given as rows of '.'/'#'/'o' characters. */
static void stamp(const char *const *rows, int nrows, int x0, int y0, uint8_t fill, uint8_t alt)
{
    for (int r = 0; r < nrows; r++) {
        for (int c = 0; rows[r][c]; c++) {
            char ch = rows[r][c];
            if (ch == '#') {
                px(x0 + c, y0 + r, fill);
            } else if (ch == 'o') {
                px(x0 + c, y0 + r, alt);
            }
        }
    }
}

/* ---------------------------------------------------------------------------
 * Idle behaviour: blinking and gaze
 * ------------------------------------------------------------------------- */

typedef struct {
    float next_blink;
    float blink_start;
    float next_gaze;
    float gx, gy;          /* current gaze, -1..1 */
    float tgx, tgy;        /* target gaze */
    float last_t;
} eyes_t;

static eyes_t s_eyes = { .next_blink = 1.5f, .blink_start = -10, .next_gaze = 1.0f };

static float eyes_update(const muse_pose_t *p, float dt)
{
    eyes_t *e = &s_eyes;

    if (p->t >= e->next_blink) {
        e->blink_start = p->t;
        /* Occasionally double-blink. */
        e->next_blink = p->t + (frand() < 0.2f ? 0.28f : 2.2f + frand() * 3.0f);
    }

    if (p->t >= e->next_gaze) {
        e->next_gaze = p->t + 1.2f + frand() * 2.4f;
        if (frand() < 0.35f) {
            e->tgx = 0;
            e->tgy = 0;
        } else {
            e->tgx = frand() * 2 - 1;
            e->tgy = (frand() * 2 - 1) * 0.6f;
        }
    }

    float tgx = e->tgx, tgy = e->tgy;
    switch (p->mode) {
    case MUSE_MODE_LISTENING:
        tgx = 0;
        tgy = 0.1f;
        break;
    case MUSE_MODE_THINKING:
        tgx = 0.75f * sinf(p->mode_t * 1.3f) + 0.25f;
        tgy = -0.85f;
        break;
    case MUSE_MODE_SPEAKING:
        tgx *= 0.3f;
        tgy = 0;
        break;
    default:
        break;
    }
    float k = 1.0f - expf(-dt * 14.0f);
    e->gx += (tgx - e->gx) * k;
    e->gy += (tgy - e->gy) * k;

    /* Blink curve: 0 = open, 1 = shut. */
    float bt = (p->t - e->blink_start) / 0.16f;
    if (bt < 0 || bt > 1) {
        return 0;
    }
    return 1.0f - fabsf(bt * 2 - 1);
}

/* ---------------------------------------------------------------------------
 * Scene layers
 * ------------------------------------------------------------------------- */

static void draw_aura(float cx, float cy, float radius, float strength)
{
    int x0 = (int)(cx - radius - 1), x1 = (int)(cx + radius + 1);
    int y0 = (int)(cy - radius - 1), y1 = (int)(cy + radius + 1);
    /* Distances in 1/16 px; the root of d2 / r2 comes from the table. */
    int32_t cx16 = (int32_t)(cx * 16), r2 = (int32_t)(radius * radius * 256);
    int32_t to_idx = (int32_t)((float)SQRT_LUT_N * 65536 / r2);
    int32_t str = QF(strength);
    for (int y = y0; y <= y1; y++) {
        int32_t dy = (int32_t)((y + 0.5f - cy) * 1.1f * 16);
        int32_t dy2 = dy * dy;
        if (dy2 >= r2) {
            continue;
        }
        for (int x = x0; x <= x1; x++) {
            int32_t dx = x * 16 + 8 - cx16;
            int32_t d2 = dx * dx + dy2;
            if (d2 >= r2) {
                continue;
            }
            int32_t i = ((ONE - s_sqrt[(d2 * to_idx) >> 16]) * str) >> Q;
            int32_t b = bayer_q(x, y);
            if (i > QF(0.55f) + ((b * QF(0.35f)) >> Q)) {
                px(x, y, C_AURA2);
            } else if (i > (b * QF(0.9f)) >> Q) {
                px(x, y, C_AURA1);
            }
        }
    }
}

/* Expanding dotted rings (listening / speaking). */
static void draw_rings(float cx, float cy, float t, float level, float speed)
{
    for (int k = 0; k < 2; k++) {
        float ph = fracf(t * speed + k * 0.5f);
        float r = 20 + ph * 11;
        float fade = (1 - ph) * (0.35f + level);
        int n = (int)(r * 2.2f);
        for (int i = 0; i < n; i++) {
            float a = i * TAU / n;
            int x = iround(cx + cosf(a) * r);
            int y = iround(cy + sinf(a) * r * 0.92f);
            if (get_px(x, y) == C_BG || get_px(x, y) == C_AURA1) {
                if (bayer(x, y) < fade) {
                    px(x, y, fade > 0.6f ? C_ACC : C_AURA2);
                }
            }
        }
    }
}

static void draw_shadow(float cx, float y, float half_w)
{
    for (int row = 0; row < 3; row++) {
        float hw = half_w * (row == 1 ? 1.0f : 0.72f);
        for (int x = iround(cx - hw); x <= iround(cx + hw); x++) {
            float edge = fabsf(x + 0.5f - cx) / hw;
            if (bayer(x, (int)y + row) > edge * 0.8f) {
                px(x, (int)y + row, C_SHADOW);
            }
        }
    }
}

/* Stable per-position hash (0..65535) so fur tufts don't shimmer as the avatar moves. */
static inline int32_t hash16(int x, int y)
{
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return (int32_t)((h ^ (h >> 16)) & 0xffff);
}

/* ---------------------------------------------------------------------------
 * BonziBuddy: analytic parts
 * ------------------------------------------------------------------------- */

enum { M_NONE, M_EAR, M_TORSO, M_BELLY, M_HEAD, M_FACEP, M_ARM, M_HAND, M_FOOT };

typedef struct {
    float cx;
    float head_y, hrx, hry;     /* head superellipse */
    float torso_y, trx, tryy;   /* torso superellipse */
    float bob, hop;
} bonzi_t;

typedef struct {
    float x, y;
} pt_t;

/* Superellipse hit test (|u|^2.7 + |v|^2.7 <= 1), Q12 in/out. */
static inline bool se_hit(int32_t dx, int32_t dy, int32_t rx, int32_t ry, int32_t *uo, int32_t *vo)
{
    int32_t u = qdiv(dx, rx), v = qdiv(dy, ry);
    if (u < -POW_LUT_MAX_Q || u > POW_LUT_MAX_Q || v < -POW_LUT_MAX_Q || v > POW_LUT_MAX_Q) {
        return false;
    }
    int32_t au = u < 0 ? -u : u, av = v < 0 ? -v : v;
    *uo = u;
    *vo = v;
    return pow_q(s_pow_dome, au) + pow_q(s_pow_dome, av) <= ONE;
}

/* Ellipse hit test (u^2 + v^2 <= 1), Q12 in/out. */
static inline bool ell_hit(int32_t dx, int32_t dy, int32_t rx, int32_t ry, int32_t *uo, int32_t *vo)
{
    int32_t u = qdiv(dx, rx), v = qdiv(dy, ry);
    int64_t q = (int64_t)u * u + (int64_t)v * v;
    *uo = u;
    *vo = v;
    return q <= (int64_t)ONE * ONE;
}

/* Purple fur tone from a surface normal, with short-hair streak texture. */
static uint8_t fur(int32_t nx, int32_t ny, int x, int y, int ox, int oy)
{
    int32_t r2 = (nx * nx + ny * ny) >> Q;
    int32_t nz = r2 >= ONE ? 0 : sqrt_q(ONE - r2);
    int32_t l = (-QF(0.40f) * nx - QF(0.50f) * ny + QF(0.76f) * nz) >> Q;
    int32_t b = bayer_q(x, y);
    int rx = x - ox, ry = y - oy;
    int32_t streak = (hash16(rx, (ry + (rx & 1) * 2) / 3) >> 4) - ONE / 2;
    int32_t lv = l + (((b - ONE / 2) * QF(0.2f)) >> Q) + ((streak * QF(0.22f)) >> Q);
    if (r2 > QF(0.86f) && ((nx * QF(0.6f) + ny * QF(0.8f)) >> Q) > QF(0.72f) && b < QF(0.4f)) {
        return C_RIM;
    }
    if (lv > QF(0.95f)) {
        return C_BH;
    }
    if (lv > QF(0.62f)) {
        return C_BL;
    }
    if (lv > QF(0.28f)) {
        return C_BM;
    }
    return C_BD;
}

/* Lavender skin tone (face panel, belly, paws) from a surface normal. */
static uint8_t belly_q(int32_t nx, int32_t ny, int x, int y)
{
    int32_t r2 = (nx * nx + ny * ny) >> Q;
    int32_t nz = r2 >= ONE ? 0 : sqrt_q(ONE - r2);
    int32_t l = (-QF(0.40f) * nx - QF(0.50f) * ny + QF(0.76f) * nz) >> Q;
    int32_t b = bayer_q(x, y);
    int32_t lv = l + (((b - ONE / 2) * QF(0.15f)) >> Q);
    if (r2 > QF(0.86f) && ((nx * QF(0.6f) + ny * QF(0.8f)) >> Q) > QF(0.72f) && b < QF(0.4f)) {
        return C_RIM;
    }
    if (lv > QF(0.9f)) {
        return C_SKINL;
    }
    if (lv > QF(0.55f)) {
        return C_SKIN;
    }
    return C_SKIND;
}

typedef struct {
    float x, y;
    float angle;    /* radians; positive tips the bottom outward to the right */
} limb_t;

/* A limb ellipse, set up for Q12 hit tests. */
typedef struct {
    int32_t x, y, c, s, inv_rx, inv_ry, r;
} limb_q_t;

static void limb_setup(const limb_t *l, float rx, float ry, limb_q_t *q)
{
    q->x = QF(l->x);
    q->y = QF(l->y);
    q->c = QF(cosf(l->angle));
    q->s = QF(sinf(l->angle));
    q->inv_rx = QF(1.0f / rx);
    q->inv_ry = QF(1.0f / ry);
    q->r = QF(rx > ry ? rx : ry);
}

static inline bool in_limb(const limb_q_t *l, int32_t x, int32_t y, int32_t *lx, int32_t *ly)
{
    int32_t dx = x - l->x, dy = y - l->y;
    if (dx > l->r || dx < -l->r || dy > l->r || dy < -l->r) {
        return false;
    }
    int32_t u = ((((dx * l->c) >> Q) + ((dy * l->s) >> Q)) * l->inv_rx) >> Q;
    int32_t v = ((((dy * l->c) >> Q) - ((dx * l->s) >> Q)) * l->inv_ry) >> Q;
    *lx = u;
    *ly = v;
    return (int64_t)u * u + (int64_t)v * v <= (int64_t)ONE * ONE;
}

static void draw_bonzi(const bonzi_t *b, const limb_q_t *al, const pt_t *hands, const limb_t *ft)
{
    memset(s_mask, 0, sizeof(s_mask));
    limb_q_t foot_q[2];
    for (int i = 0; i < 2; i++) {
        limb_setup(&ft[i], 4.4f, 2.6f, &foot_q[i]);
    }

    int32_t cx = QF(b->cx);
    int32_t hcy = QF(b->head_y), hrx = QF(b->hrx), hry = QF(b->hry);
    int32_t tcy = QF(b->torso_y), trx = QF(b->trx), tryy = QF(b->tryy);
    int32_t fcy = hcy + QF(1.0f), fa = QF(10.0f), fb = QF(8.8f);
    int ox = iround(b->cx), oy = iround(b->head_y);

    int x0 = (int)(b->cx - 23), x1 = (int)(b->cx + 23);
    int y0 = (int)(b->head_y - b->hry - 6), y1 = 60;
    if (x0 < 0) {
        x0 = 0;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (x1 >= W) {
        x1 = W - 1;
    }
    if (y1 >= H) {
        y1 = H - 1;
    }

    for (int y = y0; y <= y1; y++) {
        int32_t fy = (int32_t)y * ONE + ONE / 2;
        for (int x = x0; x <= x1; x++) {
            int32_t fx = (int32_t)x * ONE + ONE / 2;
            int32_t u, v;
            uint8_t m = M_NONE, c = C_BG;

            /* Small round ears sit behind the head. */
            for (int e = -1; e <= 1; e += 2) {
                int32_t ex = cx + e * QF(13.5f), ey = hcy - QF(2.0f);
                if (ell_hit(fx - ex, fy - ey, QF(3.4f), QF(4.2f), &u, &v)) {
                    m = M_EAR;
                    c = fur((u * QF(0.9f)) >> Q, (v * QF(0.9f)) >> Q, x, y, ox, oy);
                    /* Lighter inner ear. */
                    if (ell_hit(fx - ex, fy - ey, QF(1.6f), QF(2.2f), &u, &v)) {
                        c = C_SKIND;
                    }
                }
            }
            /* Chunky torso. */
            if (se_hit(fx - cx, fy - tcy, trx, tryy, &u, &v)) {
                m = M_TORSO;
                c = fur((u * QF(0.9f)) >> Q, (v * QF(0.9f)) >> Q, x, y, ox, oy);
            }
            /* Lavender belly patch. */
            if (ell_hit(fx - cx, fy - (tcy + QF(0.5f)), QF(7.5f), QF(7.0f), &u, &v)) {
                m = M_BELLY;
                c = belly_q((u * QF(0.9f)) >> Q, (v * QF(0.9f)) >> Q, x, y);
            }
            /* Short stubby feet. */
            for (int f = 0; f < 2; f++) {
                if (in_limb(&foot_q[f], fx, fy, &u, &v)) {
                    m = M_FOOT;
                    c = fur((u * QF(0.85f)) >> Q, (v * QF(0.85f)) >> Q, x, y, ox, oy);
                }
            }
            /* Big round head. */
            if (se_hit(fx - cx, fy - hcy, hrx, hry, &u, &v)) {
                int32_t nx = (u * QF(0.9f)) >> Q, ny = (v * QF(0.9f)) >> Q;
                m = M_HEAD;
                c = fur(nx, ny, x, y, ox, oy);
                /* Lavender face panel (u^4 + v^4 superellipse). */
                int32_t fu = qdiv(fx - cx, fa);
                int32_t fvv = qdiv(fy - fcy, fb);
                if (fu < -3 * ONE) {
                    fu = -3 * ONE;
                } else if (fu > 3 * ONE) {
                    fu = 3 * ONE;
                }
                if (fvv < -3 * ONE) {
                    fvv = -3 * ONE;
                } else if (fvv > 3 * ONE) {
                    fvv = 3 * ONE;
                }
                int32_t fu2 = (int32_t)(((int64_t)fu * fu) >> Q);
                int32_t fv2 = (int32_t)(((int64_t)fvv * fvv) >> Q);
                int32_t ff = (int32_t)(((int64_t)fu2 * fu2) >> Q)
                           + (int32_t)(((int64_t)fv2 * fv2) >> Q);
                if (ff <= ONE) {
                    m = M_FACEP;
                    c = belly_q(nx, ny, x, y);
                }
            }
            /* Long arms. */
            for (int a = 0; a < 2; a++) {
                if (in_limb(&al[a], fx, fy, &u, &v)) {
                    m = M_ARM;
                    c = fur((u * QF(0.85f)) >> Q, (v * QF(0.85f)) >> Q, x, y, ox, oy);
                }
            }
            /* Lighter paws at the arm ends. */
            for (int a = 0; a < 2; a++) {
                if (ell_hit(fx - QF(hands[a].x), fy - QF(hands[a].y), QF(3.4f), QF(3.4f), &u, &v)) {
                    m = M_HAND;
                    c = belly_q((u * QF(0.85f)) >> Q, (v * QF(0.85f)) >> Q, x, y);
                }
            }

            if (m != M_NONE) {
                s_mask[y * W + x] = m;
                px(x, y, c);
            }
        }
    }

    /* Hard outline on the silhouette, and seams where parts overlap. */
    static const int8_t N4[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint8_t m = s_mask[y * W + x];
            if (m == M_NONE || m == M_FACEP || m == M_BELLY) {
                continue;
            }
            for (int k = 0; k < 4; k++) {
                int xx = x + N4[k][0], yy = y + N4[k][1];
                uint8_t n = ((unsigned)xx < W && (unsigned)yy < H) ? s_mask[yy * W + xx] : M_NONE;
                if (n == M_NONE
                    || (m == M_ARM && (n == M_HEAD || n == M_FACEP || n == M_TORSO || n == M_BELLY))
                    || (m == M_HAND && n == M_ARM)
                    || (m == M_HEAD && (n == M_TORSO || n == M_BELLY))
                    || (m == M_FOOT && n == M_TORSO)
                    || (m == M_EAR && n == M_HEAD)) {
                    px(x, y, C_OUT);
                    break;
                }
            }
        }
    }
}

typedef enum {
    EYES_NORMAL,
    EYES_WIDE,
    EYES_HAPPY,
    EYES_X,
} eye_style_t;

/* Big friendly eyes: white sclera, dark pupil with a glossy highlight. */
static void draw_eye(float ex, float ey, float openness, eye_style_t style, float gx, float gy)
{
    if (style == EYES_HAPPY) {
        static const char *const HAPPY[] = { ".##.", "#..#" };
        stamp(HAPPY, 2, iround(ex) - 2, iround(ey), C_IRIS, C_IRIS);
        return;
    }
    if (style == EYES_X) {
        static const char *const XS[] = { "#..#", ".##.", ".##.", "#..#" };
        stamp(XS, 4, iround(ex) - 2, iround(ey) - 1, C_IRIS, C_IRIS);
        return;
    }
    if (openness < 0.3f) {
        static const char *const SHUT[] = { "#..#", ".##." };
        stamp(SHUT, 2, iround(ex) - 2, iround(ey) + 1, C_IRIS, C_IRIS);
        return;
    }

    static const char *const SCL[] = {
        "..###..",
        ".#####.",
        "#######",
        "#######",
        ".#####.",
        "..###..",
    };
    static const char *const PUP[] = { "###", "#o#", "###" };
    /* Lids close from the top: skip the upper rows as openness drops. */
    int skip = iround((1 - openness) * 4);
    if (skip > 4) {
        skip = 4;
    }
    stamp(SCL + skip, 6 - skip, iround(ex) - 3, iround(ey) - 3 + skip, C_WHITE, C_WHITE);
    float cgx = clampf(gx, -1, 1), cgy = clampf(gy, -1, 1);
    int py = iround(ey + cgy) - 1 + (skip > 2 ? skip - 2 : 0);
    stamp(PUP, 3, iround(ex + cgx) - 1, py, C_IRIS, C_SHINE);
}

/* Bonzi's heavy dark brow; lifts, knits and raises per mode. */
static void draw_brows(int xl, int xr, int y, int lift_l, int lift_r)
{
    for (int r = 0; r < 2; r++) {
        for (int i = -3; i <= 3; i++) {
            px(xl + i, y + r + lift_l, C_BROW);
            px(xr + i, y + r + lift_r, C_BROW);
        }
    }
}

static void draw_blush(int x, int y, float strength)
{
    static const char *const CHEEK[] = { ".##.", "####", ".##." };
    for (int j = 0; j < 3; j++) {
        for (int i = 0; i < 4; i++) {
            if (CHEEK[j][i] != '#') {
                continue;
            }
            float b = bayer(x + i, y + j);
            if (b < strength) {
                px(x - 2 + i, y + j, (j == 1 && b < strength * 0.5f) ? C_BLUSHD : C_BLUSH);
            }
        }
    }
}

typedef enum {
    MOUTH_SMILE,
    MOUTH_O,
    MOUTH_HMM,
    MOUTH_TALK,
    MOUTH_GRIN,
    MOUTH_FLAT,
} mouth_t;

static void draw_mouth(int x, int y, mouth_t m, float open)
{
    switch (m) {
    case MOUTH_SMILE: {
        static const char *const S[] = { "#..#", ".##." };
        stamp(S, 2, x - 2, y, C_MOUTH, C_MOUTH);
        break;
    }
    case MOUTH_O: {
        static const char *const S[] = { ".##.", "#oo#", ".##." };
        stamp(S, 3, x - 2, y - 1, C_MOUTH, C_TONGUE);
        break;
    }
    case MOUTH_HMM: {
        static const char *const S[] = { "..#", "##." };
        stamp(S, 2, x - 1, y, C_MOUTH, C_MOUTH);
        break;
    }
    case MOUTH_TALK: {
        int h = 1 + iround(clampf(open, 0, 1) * 3.0f);
        int w = h > 2 ? 4 : 3;
        for (int j = 0; j < h; j++) {
            int inset = (j == 0 || j == h - 1) && h > 2 ? 1 : 0;
            for (int i = inset; i < w - inset; i++) {
                bool tongue = h >= 3 && j == h - 2 && i > inset && i < w - inset - 1;
                px(x - w / 2 + i, y + j, tongue ? C_TONGUE : C_MOUTH);
            }
        }
        break;
    }
    case MOUTH_GRIN: {
        /* Bonzi's big goofy grin: white teeth with a pink tongue peeking out. */
        static const char *const S[] = { ".########.", "#oooooooo#", "#oooooooo#", ".########." };
        stamp(S, 4, x - 5, y - 1, C_MOUTH, C_TEETH);
        for (int i = -2; i <= 2; i++) {
            px(x + i, y + 2, C_TONGUE);
        }
        break;
    }
    case MOUTH_FLAT: {
        static const char *const S[] = { "##" };
        stamp(S, 1, x - 1, y + 1, C_MOUTH, C_MOUTH);
        break;
    }
    }
}

static void draw_sparkle(int x, int y, float twinkle, bool front)
{
    uint8_t arm = front ? C_ACC : C_AURA2;
    uint8_t core = front ? C_WHITE : C_SPK;
    if (twinkle > 0.8f) {
        px(x, y, core);
        for (int k = 1; k <= 2; k++) {
            uint8_t c = k == 1 ? (front ? C_SPK : arm) : arm;
            px(x + k, y, c);
            px(x - k, y, c);
            px(x, y + k, c);
            px(x, y - k, c);
        }
    } else if (twinkle > 0.45f) {
        px(x, y, front ? C_SPK : arm);
        px(x + 1, y, arm);
        px(x - 1, y, arm);
        px(x, y + 1, arm);
        px(x, y - 1, arm);
    } else if (twinkle > 0.15f) {
        px(x, y, arm);
    }
}

static void draw_sparkles(const muse_pose_t *p, float cx, float cy, bool front, float speed, int count)
{
    for (int i = 0; i < count; i++) {
        float a = p->t * speed + i * TAU / count;
        float s = sinf(a);
        if ((s > 0) != front) {
            continue;
        }
        float rr = 25.0f + 2.0f * sinf(i * 1.9f + p->t * 0.7f);
        int x = iround(cx + cosf(a) * rr);
        int y = iround(cy - 3 + s * rr * 0.42f);
        float tw = 0.5f + 0.5f * sinf(p->t * 5.0f + i * 1.7f);
        draw_sparkle(x, y, tw, front);
    }
}

/* Sound waves either side of the head. */
static void draw_waves(float cx, float cy, float body_rx, float level, float t)
{
    int n = 1 + (int)(clampf(level, 0, 1) * 3.2f);
    if (n > 3) {
        n = 3;
    }
    for (int k = 0; k < n; k++) {
        float r = body_rx + 5.0f + k * 3.0f;
        int span = 2 + k;
        float flick = 0.5f + 0.5f * sinf(t * 12.0f - k * 1.4f);
        for (int side = -1; side <= 1; side += 2) {
            for (int j = -span; j <= span; j++) {
                float xo = r - (float)(j * j) / (2.0f * r) * 3.0f;
                int x = iround(cx + side * xo);
                int y = iround(cy - 2 + j);
                if (k == 0 || bayer(x, y) < 0.35f + flick * 0.65f) {
                    px(x, y, k == 0 ? C_ACC : (k == 1 ? C_G1 : C_G2));
                }
            }
        }
    }
}

static void draw_thought_dots(float x, float y, float t)
{
    int active = (int)(fracf(t * 1.6f) * 3.0f);
    for (int i = 0; i < 3; i++) {
        int bx = iround(x + i * 4);
        int by = iround(y - i * 3) - (i == active ? 1 : 0);
        uint8_t c = i == active ? C_G0 : C_ACC;
        px(bx, by, c);
        px(bx + 1, by, c);
        px(bx, by + 1, c);
        px(bx + 1, by + 1, i == active ? C_G1 : C_G2);
    }
}

static void draw_hearts(float cx, float top, float t, float amount)
{
    static const char *const HEART[] = { ".#.#.", "#o###", "#####", ".###.", "..#.." };
    for (int i = 0; i < 2; i++) {
        float ph = fracf(t * 0.9f + i * 0.5f);
        if (ph > amount) {
            continue;
        }
        int hx = iround(cx + (i ? 13 : -18) + sinf(ph * TAU + i) * 2);
        int hy = iround(top - ph * 10);
        stamp(HEART, 5, hx, hy, C_HEART, C_WHITE);
    }
}

static void draw_alert(int x, int y)
{
    static const char *const BANG[] = { ".##.", ".##.", ".##.", ".##.", "....", ".##." };
    stamp(BANG, 6, x - 2, y, C_ACC, C_ACC);
}

/* ---------------------------------------------------------------------------
 * Frame
 * ------------------------------------------------------------------------- */

/*
 * Screen pixel -> grid cell, with 0x80 set on a cell's last screen pixel. When
 * cells are 3+ pixels, that edge is drawn dimmer so the pixel grid shows.
 */
#define MAP_MAX 512
static uint8_t s_map[MAP_MAX];
static int s_size;

void muse_pixel_set_size(int px)
{
    s_size = px < MAP_MAX ? px : MAP_MAX;
    bool grid = s_size >= 3 * W;
    for (int i = 0; i < s_size; i++) {
        int cell = i * W / s_size;
        bool edge = grid && (i + 1) * W / s_size != cell;
        s_map[i] = (uint8_t)(cell | (edge ? 0x80 : 0));
    }
}

void muse_pixel_scale(uint16_t *dst, int stride_px, int x0, int x1, int y0, int y1)
{
    int n = x1 - x0 + 1;
    const uint8_t *xmap = &s_map[x0];
    const uint16_t *prev = NULL;
    uint8_t prev_m = 0;
    for (int y = y0; y <= y1; y++, dst += stride_px) {
        uint8_t m = s_map[y];
        if (prev && m == prev_m) {
            memcpy(dst, prev, n * sizeof(uint16_t));
            continue;
        }
        const uint8_t *row = &s_fb[(m & 0x7f) * W];
        if (m & 0x80) {
            for (int i = 0; i < n; i++) {
                dst[i] = s_pal_dim[row[xmap[i] & 0x7f]];
            }
        } else {
            for (int i = 0; i < n; i++) {
                uint8_t xm = xmap[i];
                uint8_t c = row[xm & 0x7f];
                dst[i] = xm & 0x80 ? s_pal_dim[c] : s_pal[c];
            }
        }
        prev = dst;
        prev_m = m;
    }
}

/* Arm from the shoulder toward a hand target; the long axis follows. */
static void arm_pose(float sx, float sy, float hx, float hy, limb_t *lt, limb_q_t *lq)
{
    lt->x = (sx + hx) * 0.5f;
    lt->y = (sy + hy) * 0.5f;
    lt->angle = atan2f(hx - sx, hy - sy);
    float d = sqrtf((hx - sx) * (hx - sx) + (hy - sy) * (hy - sy));
    limb_setup(lt, 3.1f, d * 0.5f + 2.2f, lq);
}

void muse_pixel_render(const muse_pose_t *p)
{
    static bool s_luts;
    if (!s_luts) {
        init_luts();
        s_luts = true;
    }
    float dt = s_eyes.last_t > 0 ? clampf(p->t - s_eyes.last_t, 0, 0.2f) : 0.04f;
    s_eyes.last_t = p->t;

    muse_mode_t mode = p->mode;
    /* Powering down: Bonzi waves, then nods off as the glow fades. */
    float fade = mode == MUSE_MODE_OFF ? clampf(1.0f - p->mode_t / 1.3f, 0, 1) : 1.0f;
    float happy = p->happy;
    float level = p->level;
    float t = p->t;

    update_palette(&SCHEMES[mode], dt);
    float blink = eyes_update(p, dt);

    memset(s_fb, C_BG, sizeof(s_fb));

    /* ---- body motion ---- */
    float bob, breathe_rate = 2.0f, lean = 0, hop = 0;
    switch (mode) {
    case MUSE_MODE_LISTENING:
        bob = sinf(t * 3.0f) * 0.6f;
        break;
    case MUSE_MODE_THINKING:
        bob = sinf(t * 2.4f) * 0.8f;
        lean = sinf(t * 1.3f) * 1.2f;
        break;
    case MUSE_MODE_SPEAKING:
        bob = sinf(t * 5.0f) * 0.6f - level * 1.5f;
        break;
    case MUSE_MODE_ERROR:
        bob = 1.0f;
        lean = sinf(t * 18.0f) * (p->mode_t < 0.6f ? 1.0f : 0.0f);
        break;
    default:
        bob = sinf(t * 1.8f) * 1.0f;
        break;
    }
    if (happy > 0) {
        hop = fabsf(sinf(t * 9.0f)) * 3.0f * happy;
    }

    /* Boot: Bonzi pops up from a squash, then opens his eyes. */
    float boot = mode == MUSE_MODE_BOOT ? clampf(p->mode_t / 1.4f, 0, 1) : 1.0f;
    float pop = mode == MUSE_MODE_BOOT ? clampf(p->mode_t / 0.6f, 0, 1) : 1.0f;
    float squash = 1.0f - (1.0f - pop) * 0.35f + sinf(pop * 3.1416f) * 0.06f;

    float breathe = sinf(t * breathe_rate + 1.0f) * 0.03f;
    bonzi_t b;
    b.cx = 32.0f + lean;
    b.head_y = 24.0f + bob * 0.6f - hop;
    b.hrx = 13.5f * (1 + breathe) * (2.0f - squash);
    b.hry = 12.0f * (1 - breathe * 0.6f) * squash;
    b.torso_y = 44.0f + bob * 0.4f - hop * 0.6f;
    b.trx = 12.5f * (1 + breathe * 0.8f) * (2.0f - squash);
    b.tryy = 9.0f * squash;
    b.bob = bob;
    b.hop = hop;

    /* ---- background layers ---- */
    float aura_r = 29.0f + level * 4.0f + sinf(t * 1.5f) * 1.0f;
    float aura_s = (0.75f * boot + level * 0.4f) * fade;
    draw_aura(b.cx, b.torso_y - 8, aura_r, aura_s);
    if (mode == MUSE_MODE_LISTENING) {
        draw_rings(b.cx, b.head_y + 2, t, level, 0.9f);
    } else if (mode == MUSE_MODE_SPEAKING) {
        draw_rings(b.cx, b.head_y + 2, t, level, 0.6f);
    }
    draw_shadow(b.cx, 58.5f, 13.0f - hop * 0.8f);

    float spk_speed = mode == MUSE_MODE_THINKING ? 2.8f : mode == MUSE_MODE_LISTENING ? 1.2f
                    : mode == MUSE_MODE_SPEAKING ? 1.5f : 0.6f;
    int spk_count = mode == MUSE_MODE_BOOT ? (int)(boot * 6) : (int)(6 * fade);
    draw_sparkles(p, b.cx, b.torso_y - 8, false, spk_speed, spk_count);

    /* ---- arms and paws ---- */
    limb_t al_t[2];
    limb_q_t al[2];
    pt_t hands[2];
    float shx = b.cx, shy = 38.0f + bob * 0.4f;
    float hy = b.head_y;
    switch (mode) {
    case MUSE_MODE_LISTENING:
        /* Paws raised beside the face, like cupping an ear. */
        hands[0] = (pt_t){ b.cx - 11.5f, hy + 5.0f };
        hands[1] = (pt_t){ b.cx + 11.5f, hy + 5.0f };
        break;
    case MUSE_MODE_THINKING:
        /* One paw up to the chin. */
        hands[0] = (pt_t){ b.cx - 13.5f, 50.0f + bob * 0.4f };
        hands[1] = (pt_t){ b.cx + 6.5f, hy + 10.0f };
        break;
    case MUSE_MODE_OFF: {
        float wv = sinf(t * 12.0f) * 2.0f * fade;
        hands[0] = (pt_t){ b.cx - 13.5f, 50.0f + bob * 0.4f };
        hands[1] = (pt_t){ b.cx + 15.5f, hy - 1.0f + wv };
        break;
    }
    case MUSE_MODE_SPEAKING: {
        float gw = sinf(t * 7.0f) * (1.0f + level * 2.0f);
        hands[0] = (pt_t){ b.cx - 15.0f, 48.5f + gw };
        hands[1] = (pt_t){ b.cx + 15.0f, 48.5f - gw };
        break;
    }
    default:
        if (happy > 0) {
            /* Arms up and wiggling with joy. */
            float wig = sinf(t * 14.0f) * 1.5f;
            hands[0] = (pt_t){ b.cx - 13.5f, hy - 3.0f + wig };
            hands[1] = (pt_t){ b.cx + 13.5f, hy - 3.0f - wig };
        } else {
            float sway = sinf(t * 1.8f + 0.6f) * 0.8f;
            hands[0] = (pt_t){ b.cx - 14.5f, 50.5f + bob * 0.4f + sway };
            hands[1] = (pt_t){ b.cx + 14.5f, 50.5f + bob * 0.4f - sway };
        }
        break;
    }
    if (mode == MUSE_MODE_ERROR) {
        hands[0] = (pt_t){ b.cx - 15.0f, 50.0f };
        hands[1] = (pt_t){ b.cx + 15.0f, 50.0f };
    }
    arm_pose(shx - 11.0f, shy, hands[0].x, hands[0].y, &al_t[0], &al[0]);
    arm_pose(shx + 11.0f, shy, hands[1].x, hands[1].y, &al_t[1], &al[1]);

    /* ---- short stubby feet ---- */
    limb_t ft[2];
    float step = mode == MUSE_MODE_SPEAKING ? sinf(t * 5.0f) * 0.6f : 0.0f;
    ft[0].x = b.cx - 6.5f;
    ft[0].y = 54.5f + (happy > 0 ? hop * 0.3f : step);
    ft[0].angle = -0.1f;
    ft[1].x = b.cx + 6.5f;
    ft[1].y = 54.5f + (happy > 0 ? hop * 0.3f : -step);
    ft[1].angle = 0.1f;

    draw_bonzi(&b, al, hands, ft);

    /* ---- face ---- */
    float eye_y = b.head_y + 0.5f;
    float eye_dx = 5.2f;
    eye_style_t style = EYES_NORMAL;
    float open = 1.0f - blink;
    mouth_t mouth = MOUTH_GRIN;
    float mouth_open = 0;

    switch (mode) {
    case MUSE_MODE_BOOT:
        open = p->mode_t < 0.9f ? 0.0f : clampf((p->mode_t - 0.9f) / 0.3f, 0, 1);
        mouth = MOUTH_SMILE;
        break;
    case MUSE_MODE_LISTENING:
        style = EYES_WIDE;
        mouth = MOUTH_O;
        break;
    case MUSE_MODE_THINKING:
        open *= 0.85f;
        mouth = MOUTH_HMM;
        break;
    case MUSE_MODE_SPEAKING:
        mouth = MOUTH_TALK;
        mouth_open = level * 1.3f + 0.1f * (0.5f + 0.5f * sinf(t * 22.0f));
        break;
    case MUSE_MODE_ERROR:
        style = EYES_X;
        mouth = MOUTH_FLAT;
        break;
    case MUSE_MODE_OFF:
        open = clampf((1.0f - p->mode_t / 1.0f) * 1.5f, 0, 1);
        mouth = MOUTH_SMILE;
        break;
    default:
        break;
    }
    if (happy > 0.2f && mode != MUSE_MODE_ERROR) {
        style = EYES_HAPPY;
        mouth = MOUTH_GRIN;
    }

    draw_eye(b.cx - eye_dx, eye_y, open, style, s_eyes.gx, s_eyes.gy);
    draw_eye(b.cx + eye_dx, eye_y, open, style, s_eyes.gx, s_eyes.gy);

    /* Bonzi's heavy brow is always on duty. */
    int bl = iround(b.cx - eye_dx), br = iround(b.cx + eye_dx), by = iround(eye_y) - 6;
    if (mode == MUSE_MODE_THINKING) {
        draw_brows(bl, br, by, 1, -1);
    } else if (mode == MUSE_MODE_LISTENING) {
        draw_brows(bl, br, by - 1, 0, 0);
    } else {
        draw_brows(bl, br, by, 0, 0);
    }

    float blush = 0.55f + happy * 0.45f + (mode == MUSE_MODE_SPEAKING ? 0.15f : 0.0f);
    draw_blush(iround(b.cx - 7.0f), iround(eye_y + 3), blush);
    draw_blush(iround(b.cx + 7.0f), iround(eye_y + 3), blush);

    draw_mouth(iround(b.cx), iround(eye_y + 6), mouth, mouth_open);

    /* ---- foreground ---- */
    draw_sparkles(p, b.cx, b.torso_y - 8, true, spk_speed, spk_count);

    float top = b.head_y - b.hry;
    if (mode == MUSE_MODE_LISTENING || mode == MUSE_MODE_SPEAKING) {
        draw_waves(b.cx, b.head_y + 2, b.trx, level, t);
    }
    if (mode == MUSE_MODE_THINKING) {
        draw_thought_dots(b.cx + 15, top + 2, t);
    }
    if (happy > 0) {
        draw_hearts(b.cx, top + 1, t, happy);
    }
    if (mode == MUSE_MODE_ERROR) {
        draw_alert(iround(b.cx + 18), iround(top - 1));
    }
}
