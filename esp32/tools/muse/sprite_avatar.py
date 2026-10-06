#!/usr/bin/env python3
"""Builds components/muse/avatar/muse_pixel.c from BonziBuddy's real sprite sheet.

The sheet (Documents/BonziBuddy-sprites/bonzi-sprite-sheet.png, 801x976) is not
a regular grid, so each figure is found on the black background by splitting at
gaps, cut on its own outline, bottom-aligned and centred in one common frame.
The frames are quantised to a shared 255-colour palette; near-black is the
keyed background (index 0). The generated C file implements muse_pixel.h: a
frame per mode, idle glances, a chattering jaw while speaking, exact 2x
nearest-neighbour scaling on the canvas.

    python tools/muse/sprite_avatar.py [sheet.png] [out.c] [--preview out.png]
"""
import os
import sys

from PIL import Image

SHEET = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith("--") else r"C:\Users\mat_n\Documents\BonziBuddy-sprites\bonzi-sprite-sheet.png"
OUT = sys.argv[2] if len(sys.argv) > 2 and not sys.argv[2].startswith("--") else os.path.join(os.path.dirname(__file__), "..", "..", "components", "muse", "avatar", "muse_pixel.c")
PREVIEW = sys.argv[sys.argv.index("--preview") + 1] if "--preview" in sys.argv else None

FRAME_W, FRAME_H = 128, 112   # the common frame: 2x = 256 x 224, the canvas
GAP_TH = 70                   # gap finding ignores faint fringes
KEY_TH = 26                   # darker than this is background

# which sprite (index in the sheet's reading order, see the numbered map) plays which part
PICKS = [
    ("idle_a", 0),
    ("idle_b", 1),
    ("glance", 2),
    ("listen", 6),
    ("think", 9),
    ("talk_o", 4),
    ("talk_c", 0),
    ("wave", 7),
    ("happy", 13),
    ("error", 11),
    ("sleep", 14),
]


def find_sprites(sheet):
    """Boxes of every figure, in reading order (row bands of about 50 px, then x)."""
    W, H = sheet.size
    px = sheet.load()

    def fg(x, y):
        return max(px[x, y]) >= GAP_TH

    def runs(flags, mingap):
        out, start, gap, end = [], None, 0, None
        for i, f in enumerate(flags + [False] * (mingap + 1)):
            if f:
                if start is None:
                    start = i
                gap, end = 0, i
            elif start is not None:
                gap += 1
                if gap > mingap:
                    out.append((start, end))
                    start = None
        return out

    def tighten(b):
        x0, y0, x1, y1 = b
        ys = [y for y in range(y0, y1 + 1) if any(fg(x, y) for x in range(x0, x1 + 1))]
        xs = [x for x in range(x0, x1 + 1) if any(fg(x, y) for y in range(y0, y1 + 1))]
        return (xs[0], ys[0], xs[-1], ys[-1]) if xs and ys else None

    def split(b, depth=0):
        b = tighten(b)
        if not b:
            return []
        x0, y0, x1, y1 = b
        rows = runs([any(fg(x, y) for x in range(x0, x1 + 1)) for y in range(y0, y1 + 1)], 2)
        if len(rows) > 1 and depth < 8:
            return [s for (a, c) in rows for s in split((x0, y0 + a, x1, y0 + c), depth + 1)]
        cols = runs([any(fg(x, y) for y in range(y0, y1 + 1)) for x in range(x0, x1 + 1)], 2)
        if len(cols) > 1 and depth < 8:
            return [s for (a, c) in cols for s in split((x0 + a, y0, x0 + c, y1), depth + 1)]
        return [b]

    boxes = [b for b in split((0, 0, W - 1, H - 1)) if (b[2] - b[0] + 1) * (b[3] - b[1] + 1) > 500]
    boxes.sort(key=lambda b: ((b[1] + b[3]) // 2 // 50, b[0]))
    return boxes


def frame_of(sheet, box):
    """The sprite cut at its outline (with a 2 px margin), bottom-aligned and centred in the common frame."""
    x0, y0, x1, y1 = box
    crop = sheet.crop((max(0, x0 - 2), max(0, y0 - 2), min(sheet.width, x1 + 3), min(sheet.height, y1 + 3)))
    if crop.width > FRAME_W:   # the lying-down pose is wider than the frame: trim its edges evenly
        cut = (crop.width - FRAME_W) // 2
        crop = crop.crop((cut, 0, cut + FRAME_W, crop.height))
    if crop.height > FRAME_H:
        crop = crop.crop((0, crop.height - FRAME_H, crop.width, crop.height))
    frame = Image.new("RGB", (FRAME_W, FRAME_H))
    frame.paste(crop, ((FRAME_W - crop.width) // 2, FRAME_H - crop.height))
    return frame


sheet = Image.open(SHEET).convert("RGB")
boxes = find_sprites(sheet)
print("%d sprites on the sheet" % len(boxes))
images = [frame_of(sheet, boxes[i]) for _, i in PICKS]

if PREVIEW:
    cols = len(images)
    strip = Image.new("RGB", (FRAME_W * cols, FRAME_H))
    for i, im in enumerate(images):
        strip.paste(im, (i * FRAME_W, 0))
    strip.resize((strip.width * 2, strip.height * 2), Image.NEAREST).save(PREVIEW)
    print("preview:", PREVIEW)

# one palette for all frames: quantise them side by side, then split again
sheet_q = Image.new("RGB", (FRAME_W * len(images), FRAME_H))
for i, im in enumerate(images):
    sheet_q.paste(im, (i * FRAME_W, 0))
quant = sheet_q.quantize(colors=255, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
pal = quant.getpalette()[: 255 * 3]
qpx = quant.load()
spx = sheet_q.load()


def rgb565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


palette565 = [0] + [rgb565(pal[i * 3], pal[i * 3 + 1], pal[i * 3 + 2]) for i in range(255)]
frames = []
for i in range(len(images)):
    data = bytearray(FRAME_W * FRAME_H)
    for y in range(FRAME_H):
        for x in range(FRAME_W):
            r, g, b = spx[i * FRAME_W + x, y]
            data[y * FRAME_W + x] = 0 if max(r, g, b) < KEY_TH else qpx[i * FRAME_W + x, y] + 1
    frames.append(bytes(data))


def c_bytes(b, per_line=32):
    return "\n".join("    " + ", ".join("%d" % v for v in b[i:i + per_line]) + "," for i in range(0, len(b), per_line))


out = []
out.append("""// Copyright (c) Meta Platforms, Inc. and affiliates.

/*
 * AVATAR: BonziBuddy, from the real sprites.
 *
 * The genuine purple gorilla: figures cut from BonziBuddy's own sprite sheet,
 * bottom-aligned in one frame, quantised to a shared 255-colour palette and
 * shown at an exact 2x. He sits and breathes, glances about now and then,
 * raises a hand to listen, reads his book to think, chatters his big grin in
 * time with the audio when he talks, waves on boot, throws his arms up when
 * petted and lies flat to sleep. Generated by tools/muse/sprite_avatar.py:
 * edit that, not this.
 */

#include "muse_pixel.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define SPR_W %d
#define SPR_H %d
#define MAP_MAX 512
#define BG565 0x0000   /* the face screen is black; index 0 is the keyed background */
""" % (FRAME_W, FRAME_H))

out.append("static const uint16_t PAL[256] = {\n" + c_bytes(palette565, 16) + "\n};\n")
for (name, _), data in zip(PICKS, frames):
    out.append("static const uint8_t F_%s[SPR_W * SPR_H] = {\n%s\n};\n" % (name.upper(), c_bytes(data)))

out.append(r"""
/* ---- which frame, and where ------------------------------------------------- */

static const uint8_t *s_frame = F_IDLE_A;
static int s_bob;            /* vertical offset in screen pixels (breathing) */
static int s_size = 256;
static int16_t s_xmap[MAP_MAX], s_ymap[MAP_MAX];   /* canvas x/y -> sprite x/y, -1 outside */
static float s_glance_until, s_next_glance = 4.0f;
static int s_talk_phase;
static float s_talk_t;

uint32_t muse_pixel_accent(muse_mode_t mode)
{
    switch (mode) {
    case MUSE_MODE_BOOT: return 0xa9c0ff;
    case MUSE_MODE_LISTENING: return 0x5cb8ff;
    case MUSE_MODE_THINKING: return 0xe07bff;
    case MUSE_MODE_SPEAKING: return 0x6ff0bf;
    case MUSE_MODE_ERROR: return 0xff5c5c;
    case MUSE_MODE_OFF: return 0x7c72d0;
    default: return 0xa77dff;
    }
}

void muse_pixel_render(const muse_pose_t *p)
{
    float t = p->t;
    /* Breathing: a slow two-pixel bob everywhere but asleep. */
    s_bob = p->mode == MUSE_MODE_OFF ? 0 : (int)lroundf(sinf(t * 1.6f) * 2.0f);
    if (p->happy > 0.3f) {
        s_frame = F_HAPPY;
        return;
    }
    switch (p->mode) {
    case MUSE_MODE_BOOT:
        s_frame = fmodf(t, 1.0f) < 0.5f ? F_WAVE : F_IDLE_A;
        return;
    case MUSE_MODE_LISTENING:
        s_frame = F_LISTEN;
        return;
    case MUSE_MODE_THINKING:
        s_frame = F_THINK;
        return;
    case MUSE_MODE_ERROR:
        s_frame = F_ERROR;
        return;
    case MUSE_MODE_OFF:
        s_frame = F_SLEEP;
        return;
    case MUSE_MODE_SPEAKING:
        /* The jaw follows the sound: the big grin while there is level, flipping every ~110 ms so it chatters. */
        if (p->level > 0.06f) {
            if (t - s_talk_t > 0.11f) {
                s_talk_phase ^= 1;
                s_talk_t = t;
            }
            s_frame = s_talk_phase ? F_TALK_O : F_TALK_C;
        } else {
            s_frame = F_TALK_C;
        }
        return;
    default:
        break;
    }
    /* Idle: two poses swapped every few seconds, a glance elsewhere now and then. */
    if (t >= s_next_glance) {
        s_glance_until = t + 0.9f;
        s_next_glance = t + 5.0f + fmodf(t * 7.31f, 6.0f);
    }
    if (t < s_glance_until) {
        s_frame = F_GLANCE;
    } else {
        s_frame = fmodf(t, 6.0f) < 3.0f ? F_IDLE_A : F_IDLE_B;
    }
}

void muse_pixel_set_size(int px)
{
    s_size = px < MAP_MAX ? px : MAP_MAX;
    /* Whole screen pixels per sprite pixel (2x on a 256 px canvas), centred; a canvas too small
     * for that fits the frame's width instead. */
    int k = s_size / SPR_W;
    float kf = k >= 1 ? (float)k : (float)s_size / SPR_W;
    int w = (int)(SPR_W * kf + 0.5f), h = (int)(SPR_H * kf + 0.5f);
    int x0 = (s_size - w) / 2, y0 = (s_size - h) / 2;
    for (int x = 0; x < s_size; x++) {
        int sx = (int)((x - x0) / kf);
        s_xmap[x] = (int16_t)(x < x0 || sx < 0 || sx >= SPR_W ? -1 : sx);
    }
    for (int y = 0; y < s_size; y++) {
        int sy = (int)((y - y0) / kf);
        s_ymap[y] = (int16_t)(y < y0 || sy < 0 || sy >= SPR_H ? -1 : sy);
    }
}

void muse_pixel_scale(uint16_t *dst, int stride_px, int x0, int x1, int y0, int y1)
{
    const uint8_t *frame = s_frame;
    int bob = s_bob;
    for (int y = y0; y <= y1; y++, dst += stride_px) {
        int yy = y - bob;
        int sy = yy >= 0 && yy < s_size ? s_ymap[yy] : -1;
        if (sy < 0) {
            for (int i = 0; i <= x1 - x0; i++) {
                dst[i] = BG565;
            }
            continue;
        }
        const uint8_t *row = frame + sy * SPR_W;
        for (int x = x0, i = 0; x <= x1; x++, i++) {
            int sx = x >= 0 && x < s_size ? s_xmap[x] : -1;
            dst[i] = sx < 0 ? BG565 : PAL[row[sx]];
        }
    }
}
""")

os.makedirs(os.path.dirname(os.path.abspath(OUT)), exist_ok=True)
with open(OUT, "w", encoding="utf-8", newline="\n") as f:
    f.write("\n".join(out))
print("wrote %s: %d frames of %dx%d, %d bytes of sprite data" % (OUT, len(frames), FRAME_W, FRAME_H, sum(len(fr) for fr in frames)))
