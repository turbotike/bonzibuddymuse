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
 * The voice link over Wi-Fi (devices/SERIAL_VOICE.md, "Over the network"): the
 * same SHOW / RATE / SAY / END lines as the USB console, on a TCP port, and the
 * console's @chat frames (the replies to speak) sent back on the same socket.
 * A UDP beacon on the same port says where the gadget is. One client at a time.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void muse_voice_link_start(void);

/* A network client is connected (the Pi's daemon): it speaks the replies, so the board doesn't. */
bool muse_voice_link_connected(void);

/* A console frame (one line, "\n" included) for the connected client, if any. */
void muse_voice_link_console_line(const char *line, size_t n);

#ifdef __cplusplus
}
#endif
