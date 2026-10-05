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
 * Gadget tools (CONFIG_MUSE_GADGET_TOOLS): what a Muse board with a screen and
 * a speaker can do on its own, offered to Muse as Home Link commands.
 *
 *   screen.show_text / screen.clear    a message on the screen, with a chime
 *   sound.play                         chime, beep, alarm, doorbell, siren...
 *   timer.set / timer.cancel / timer.list   countdowns: shown while they run,
 *                                      alarm and message when they end
 *   alarm.set                          a timer that ends at a clock time
 *   clock.now                          local time (SNTP, CONFIG_MUSE_TOOLS_TZ)
 *   light.set                          the WS2812 (CONFIG_MUSE_TOOLS_RGB_LED_GPIO)
 *   mic.level                          how loud the room is
 *   face.happy                         make the avatar smile
 *   gadget.configure / gadget.status   volume, brightness, speaker, sleep;
 *                                      power, Wi-Fi, heap, temperature...
 *
 * noise_control.cpp adds the command specs at link.register and app.c hands
 * link.invoke requests here before its "unsupported" fallthrough.
 */
#pragma once

#include <stdbool.h>
#include "cJSON.h"
#include "noise_control.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Timers, the clock and the light. Call once, after noise_ctrl_init(). */
void gadget_tools_init(void);

/* Adds this module's command specs to a link.register "commands_v2" object. */
void gadget_tools_add_commands(cJSON *commands);

/* Handles a link.invoke; NULL when the command isn't one of ours. The result
 * has "ok" and "payload" or "error", or "_async" when it comes later through
 * noise_ctrl_send_command_result(). */
cJSON *gadget_tools_command(const char *command, cJSON *params, const char *request_id,
                            noise_ctrl_session_generation_t session_generation);

#ifdef __cplusplus
}
#endif
