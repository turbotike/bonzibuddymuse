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
 * More gadget tools, on top of gadget_tools.c:
 *
 *   screen.big_text / screen.clock / screen.draw   the canvas: huge text, a
 *                                      live clock, shapes and charts
 *   event.watch / event.cancel          when the room gets loud or a pin
 *                                      changes, say something in the chat
 *   net.ping                            is that host up?
 *   log.append                          a timestamped line on the SD card
 *   stopwatch.start/stop/reset/read     a stopwatch on the idle caption
 *   gadget.identify / gadget.reboot
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "cJSON.h"
#include "noise_control.h"

#ifdef __cplusplus
extern "C" {
#endif

void gadget_more_init(void);
void gadget_more_add_commands(cJSON *commands);
cJSON *gadget_more_command(const char *command, cJSON *params, const char *request_id,
                           noise_ctrl_session_generation_t session_generation);

/* From gadget_tools' 1 s tick: picture expiry, the clock's minute hand. */
void gadget_more_tick(int64_t now_us);

/* Something to show on the idle caption when no timer is running (the
 * stopwatch); false when there is nothing. */
bool gadget_more_caption(char *out, size_t cap);

/* Takes a canvas picture or clock down (screen.clear, a message going up). */
void gadget_more_screen_clear(void);

/* gadget_tools' GPIO allow-list, for event.watch. */
bool gadget_tools_gpio_allowed(int pin);

#ifdef __cplusplus
}
#endif
