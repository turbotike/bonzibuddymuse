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
#include "gadget_canvas.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_heap_caps.h"
#include "sdkconfig.h"

#if CONFIG_HOMEHUB_DISPLAY_COMMANDS
#include "led_status.h"
#endif

struct canvas {
    int w, h;
    uint16_t *px;   /* RGB565, high byte first, as the picture path wants it */
};

static uint16_t to565be(uint32_t rgb)
{
    uint16_t v = (uint16_t)(((rgb >> 8) & 0xf800) | ((rgb >> 5) & 0x07e0) | ((rgb >> 3) & 0x001f));
    return (uint16_t)((v << 8) | (v >> 8));
}

canvas_t *canvas_create(int w, int h, uint32_t background)
{
    if (w <= 0 || h <= 0) {
        return NULL;
    }
    canvas_t *c = calloc(1, sizeof(*c));
    if (!c) {
        return NULL;
    }
    c->w = w;
    c->h = h;
    c->px = heap_caps_malloc((size_t)w * h * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!c->px) {
        free(c);
        return NULL;
    }
    uint16_t bg = to565be(background);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        c->px[i] = bg;
    }
    return c;
}

void canvas_free(canvas_t *c)
{
    if (c) {
        free(c->px);
        free(c);
    }
}

int canvas_width(const canvas_t *c)
{
    return c ? c->w : 0;
}

int canvas_height(const canvas_t *c)
{
    return c ? c->h : 0;
}

static inline void put(canvas_t *c, int x, int y, uint16_t v)
{
    if ((unsigned)x < (unsigned)c->w && (unsigned)y < (unsigned)c->h) {
        c->px[(size_t)y * c->w + x] = v;
    }
}

void canvas_clear(canvas_t *c, uint32_t background)
{
    if (c) {
        canvas_fill_rect(c, 0, 0, c->w, c->h, background);
    }
}

void canvas_fill_rect(canvas_t *c, int x, int y, int w, int h, uint32_t rgb)
{
    if (!c || w <= 0 || h <= 0) {
        return;
    }
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > c->w ? c->w : x + w, y1 = y + h > c->h ? c->h : y + h;
    uint16_t v = to565be(rgb);
    for (int yy = y0; yy < y1; yy++) {
        uint16_t *row = c->px + (size_t)yy * c->w;
        for (int xx = x0; xx < x1; xx++) {
            row[xx] = v;
        }
    }
}

void canvas_rect(canvas_t *c, int x, int y, int w, int h, uint32_t rgb, int thickness)
{
    int t = thickness < 1 ? 1 : thickness;
    canvas_fill_rect(c, x, y, w, t, rgb);
    canvas_fill_rect(c, x, y + h - t, w, t, rgb);
    canvas_fill_rect(c, x, y, t, h, rgb);
    canvas_fill_rect(c, x + w - t, y, t, h, rgb);
}

void canvas_line(canvas_t *c, int x0, int y0, int x1, int y1, uint32_t rgb, int thickness)
{
    if (!c) {
        return;
    }
    int t = thickness < 1 ? 1 : thickness > 20 ? 20 : thickness;
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    uint16_t v = to565be(rgb);
    for (int guard = 0; guard < 4096; guard++) {
        if (t == 1) {
            put(c, x0, y0, v);
        } else {
            for (int oy = -(t / 2); oy < t - t / 2; oy++) {
                for (int ox = -(t / 2); ox < t - t / 2; ox++) {
                    put(c, x0 + ox, y0 + oy, v);
                }
            }
        }
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

void canvas_circle(canvas_t *c, int cx, int cy, int r, uint32_t rgb, bool fill, int thickness)
{
    if (!c || r <= 0) {
        return;
    }
    int t = thickness < 1 ? 1 : thickness;
    int inner = fill ? 0 : (r - t < 0 ? 0 : r - t);
    int r2 = r * r, i2 = inner * inner;
    uint16_t v = to565be(rgb);
    for (int y = -r; y <= r; y++) {
        for (int x = -r; x <= r; x++) {
            int d2 = x * x + y * y;
            if (d2 <= r2 && d2 >= i2) {
                put(c, cx + x, cy + y, v);
            }
        }
    }
}

/* ---- the font: 5 wide, 7 tall, one row per byte, bit 4 is the left column ---- */

typedef struct {
    char ch;
    uint8_t row[CANVAS_GLYPH_H];
} glyph_t;

static const glyph_t FONT[] = {
    { ' ', { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } },
    { '!', { 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04 } },
    { '"', { 0x0a, 0x0a, 0x0a, 0x00, 0x00, 0x00, 0x00 } },
    { '#', { 0x0a, 0x0a, 0x1f, 0x0a, 0x1f, 0x0a, 0x0a } },
    { '$', { 0x04, 0x0f, 0x14, 0x0e, 0x05, 0x1e, 0x04 } },
    { '%', { 0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03 } },
    { '&', { 0x0c, 0x12, 0x14, 0x08, 0x15, 0x12, 0x0d } },
    { '\'', { 0x0c, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00 } },
    { '(', { 0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02 } },
    { ')', { 0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08 } },
    { '*', { 0x00, 0x04, 0x15, 0x0e, 0x15, 0x04, 0x00 } },
    { '+', { 0x00, 0x04, 0x04, 0x1f, 0x04, 0x04, 0x00 } },
    { ',', { 0x00, 0x00, 0x00, 0x00, 0x0c, 0x04, 0x08 } },
    { '-', { 0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00 } },
    { '.', { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x0c } },
    { '/', { 0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x00 } },
    { '0', { 0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e } },
    { '1', { 0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e } },
    { '2', { 0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f } },
    { '3', { 0x1f, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0e } },
    { '4', { 0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02 } },
    { '5', { 0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e } },
    { '6', { 0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e } },
    { '7', { 0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 } },
    { '8', { 0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e } },
    { '9', { 0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c } },
    { ':', { 0x00, 0x0c, 0x0c, 0x00, 0x0c, 0x0c, 0x00 } },
    { ';', { 0x00, 0x0c, 0x0c, 0x00, 0x0c, 0x04, 0x08 } },
    { '<', { 0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02 } },
    { '=', { 0x00, 0x00, 0x1f, 0x00, 0x1f, 0x00, 0x00 } },
    { '>', { 0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08 } },
    { '?', { 0x0e, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04 } },
    { '@', { 0x0e, 0x11, 0x01, 0x0d, 0x15, 0x15, 0x0e } },
    { 'A', { 0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11 } },
    { 'B', { 0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e } },
    { 'C', { 0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e } },
    { 'D', { 0x1c, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1c } },
    { 'E', { 0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f } },
    { 'F', { 0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10 } },
    { 'G', { 0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0f } },
    { 'H', { 0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11 } },
    { 'I', { 0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e } },
    { 'J', { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0c } },
    { 'K', { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 } },
    { 'L', { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f } },
    { 'M', { 0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11 } },
    { 'N', { 0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11 } },
    { 'O', { 0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e } },
    { 'P', { 0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10 } },
    { 'Q', { 0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d } },
    { 'R', { 0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11 } },
    { 'S', { 0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e } },
    { 'T', { 0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } },
    { 'U', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e } },
    { 'V', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04 } },
    { 'W', { 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0a } },
    { 'X', { 0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11 } },
    { 'Y', { 0x11, 0x11, 0x11, 0x0a, 0x04, 0x04, 0x04 } },
    { 'Z', { 0x1f, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1f } },
    { '[', { 0x0e, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0e } },
    { ']', { 0x0e, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0e } },
    { '^', { 0x0c, 0x12, 0x0c, 0x00, 0x00, 0x00, 0x00 } },   /* a degree sign */
    { '_', { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1f } },
    { '|', { 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } },
};

static const glyph_t *glyph(char ch)
{
    ch = (char)toupper((unsigned char)ch);
    for (size_t i = 0; i < sizeof(FONT) / sizeof(FONT[0]); i++) {
        if (FONT[i].ch == ch) {
            return &FONT[i];
        }
    }
    return glyph('?');
}

int canvas_text_width(const char *s, int scale)
{
    int n = 0;
    for (; s && *s && *s != '\n'; s++) {
        n++;
    }
    return n ? (n * CANVAS_CELL_W - 1) * scale : 0;
}

void canvas_text(canvas_t *c, int x, int y, const char *s, int scale, uint32_t rgb)
{
    if (!c || !s || scale < 1) {
        return;
    }
    for (; *s && *s != '\n'; s++, x += CANVAS_CELL_W * scale) {
        if (*s == ' ') {
            continue;
        }
        const glyph_t *g = glyph(*s);
        for (int r = 0; r < CANVAS_GLYPH_H; r++) {
            for (int col = 0; col < CANVAS_GLYPH_W; col++) {
                if (g->row[r] & (0x10 >> col)) {
                    canvas_fill_rect(c, x + col * scale, y + r * scale, scale, scale, rgb);
                }
            }
        }
    }
}

bool canvas_show(const canvas_t *c)
{
#if CONFIG_HOMEHUB_DISPLAY_COMMANDS
    if (!c) {
        return false;
    }
    bool ok = true;
    for (int y = 0; y < c->h && ok; y += 40) {
        int rows = c->h - y < 40 ? c->h - y : 40;
        ok = led_status_draw_rect(0, y, c->w, rows, c->px + (size_t)y * c->w);
    }
    led_status_draw_done();
    return ok;
#else
    return false;
#endif
}

bool canvas_parse_color(const char *s, uint32_t *rgb)
{
    static const struct { const char *name; uint32_t rgb; } NAMES[] = {
        { "black", 0x000000 }, { "white", 0xffffff }, { "red", 0xff0000 }, { "green", 0x00e000 },
        { "blue", 0x2060ff }, { "yellow", 0xffd000 }, { "orange", 0xff7000 }, { "purple", 0xa040ff },
        { "pink", 0xff40a0 }, { "cyan", 0x00e0ff }, { "grey", 0x808080 }, { "gray", 0x808080 },
        { "lime", 0x00fc00 }, { "amber", 0xffb000 }, { "navy", 0x101840 }, { "brown", 0x804020 },
    };
    if (!s || !*s) {
        return false;
    }
    for (size_t i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); i++) {
        if (!strcasecmp(s, NAMES[i].name)) {
            *rgb = NAMES[i].rgb;
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
