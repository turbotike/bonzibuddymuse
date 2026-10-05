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
 * A tappable menu over whatever is on the screen: a title and up to eight
 * buttons. The first tap, or the timeout, ends it and the callback gets the
 * choice (-1 for the timeout). One at a time. Used by the gadget tools'
 * screen.menu so Muse can ask the user something and get a real answer.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MUSE_PROMPT_MAX_ITEMS 8

typedef void (*muse_prompt_cb_t)(int choice, void *user);

/* Shows the menu; `items` and `title` are copied. False if one is already up
 * or the UI isn't ready. The callback runs on the UI task or the timer task,
 * so it should be quick and must not block on the display lock. */
bool muse_prompt_show(const char *title, const char *const *items, int n, int timeout_ms,
                      muse_prompt_cb_t cb, void *user);

bool muse_prompt_active(void);

/* Takes an open menu down; its callback gets -1. */
void muse_prompt_dismiss(void);

#ifdef __cplusplus
}
#endif
