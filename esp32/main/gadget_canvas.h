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
 * A full-screen RGB565 canvas for the gadget tools: rectangles, lines,
 * circles and a scalable 5x7 pixel font, shown through the same picture path
 * as display.draw_url (so a tap or screen.clear takes it down). Colours are
 * 0xRRGGBB; coordinates are screen pixels.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct canvas canvas_t;

#define CANVAS_GLYPH_W 5
#define CANVAS_GLYPH_H 7
#define CANVAS_CELL_W 6     /* glyph plus one column of space, before scaling */
#define CANVAS_CELL_H 8

canvas_t *canvas_create(int w, int h, uint32_t background);
void canvas_free(canvas_t *c);
int canvas_width(const canvas_t *c);
int canvas_height(const canvas_t *c);

void canvas_clear(canvas_t *c, uint32_t background);
void canvas_fill_rect(canvas_t *c, int x, int y, int w, int h, uint32_t rgb);
void canvas_rect(canvas_t *c, int x, int y, int w, int h, uint32_t rgb, int thickness);
void canvas_line(canvas_t *c, int x0, int y0, int x1, int y1, uint32_t rgb, int thickness);
void canvas_circle(canvas_t *c, int cx, int cy, int r, uint32_t rgb, bool fill, int thickness);

/* One line of text (stops at a newline); lowercase is drawn as capitals. */
void canvas_text(canvas_t *c, int x, int y, const char *s, int scale, uint32_t rgb);
int canvas_text_width(const char *s, int scale);

/* Pushes the canvas to the screen. False if the screen can't show pictures. */
bool canvas_show(const canvas_t *c);

/* Parses "red", "#ff8800" or "ff8800"; false if it isn't a colour. */
bool canvas_parse_color(const char *s, uint32_t *rgb);

#ifdef __cplusplus
}
#endif
