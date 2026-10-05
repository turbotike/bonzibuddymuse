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
 * The touch controller built into the Freenove 3.5" display (FNK0104N): an
 * in-cell (TDDI) touch at I2C 0x55 with 16-bit registers, as Freenove's own
 * ST77922_Touch library reads it. An esp_lcd_touch driver so the LVGL adapter
 * can use it like any other.
 */
#pragma once

#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_LCD_TOUCH_IO_I2C_FREENOVE35_ADDRESS 0x55

#define ESP_LCD_TOUCH_IO_I2C_FREENOVE35_CONFIG()      \
    {                                                 \
        .dev_addr = ESP_LCD_TOUCH_IO_I2C_FREENOVE35_ADDRESS, \
        .control_phase_bytes = 1,                     \
        .dc_bit_offset = 0,                           \
        .lcd_cmd_bits = 16,                           \
        .lcd_param_bits = 0,                          \
        .flags = { .disable_control_phase = 1 },      \
    }

esp_err_t esp_lcd_touch_new_i2c_freenove35(const esp_lcd_panel_io_handle_t io, const esp_lcd_touch_config_t *config,
                                           esp_lcd_touch_handle_t *out_touch);

#ifdef __cplusplus
}
#endif
