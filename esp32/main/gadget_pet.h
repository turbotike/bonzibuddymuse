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
/* The pet.* Home Link commands: Muse looks after the creature and speaks for it. */
#pragma once

#include <stdint.h>

#include "cJSON.h"

void gadget_pet_init(void);
void gadget_pet_add_commands(cJSON *commands);
/* NULL if the command isn't pet.*; else a result as gadget_tools_command returns. */
cJSON *gadget_pet_command(const char *command, cJSON *params, const char *request_id, uint32_t gen);
