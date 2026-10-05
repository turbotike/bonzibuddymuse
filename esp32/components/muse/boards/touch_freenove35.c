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
#include "touch_freenove35.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "touch35";

/* Registers (16-bit addresses), from Freenove's ST77922_Touch.h. */
#define REG_STATUS 0x0001
#define REG_MAX_TOUCHES 0x0009
#define REG_TOUCH_INFO 0x0010      /* bit 3: a new report is ready */
#define REG_POINT0 0x0014          /* 7 bytes a point: [0] bit 7 valid, bits 5..0 x high; [1] x low; [2] y high; [3] y low */
#define POINT_BYTES 7
#define POINTS_MAX 10
#define RELEASE_US 150000          /* no new report this long: the finger is gone */

static uint8_t s_max_points = 5;
static int64_t s_last_report_us;

static esp_err_t rd(esp_lcd_touch_handle_t tp, uint16_t reg, uint8_t *buf, size_t len)
{
    return esp_lcd_panel_io_rx_param(tp->io, reg, buf, len);
}

static esp_err_t read_data(esp_lcd_touch_handle_t tp)
{
    uint8_t info = 0;
    ESP_RETURN_ON_ERROR(rd(tp, REG_TOUCH_INFO, &info, 1), TAG, "info");
    int64_t now = esp_timer_get_time();
    if (!(info & 0x08)) {
        /* Nothing new. Keep the last points briefly (reports come at the panel's rate), then release. */
        if (now - s_last_report_us > RELEASE_US) {
            portENTER_CRITICAL(&tp->data.lock);
            tp->data.points = 0;
            portEXIT_CRITICAL(&tp->data.lock);
        }
        return ESP_OK;
    }
    s_last_report_us = now;
    uint8_t d[POINT_BYTES * POINTS_MAX];
    int n = s_max_points;
    ESP_RETURN_ON_ERROR(rd(tp, REG_POINT0, d, POINT_BYTES * n), TAG, "points");
    portENTER_CRITICAL(&tp->data.lock);
    tp->data.points = 0;
    for (int i = 0; i < n && tp->data.points < CONFIG_ESP_LCD_TOUCH_MAX_POINTS; i++) {
        const uint8_t *p = d + i * POINT_BYTES;
        if (!(p[0] & 0x80)) {
            continue;
        }
        uint8_t k = tp->data.points++;
        tp->data.coords[k].x = (uint16_t)(((p[0] & 0x3f) << 8) | p[1]);
        tp->data.coords[k].y = (uint16_t)(((p[2] & 0x3f) << 8) | p[3]);
        tp->data.coords[k].strength = 1;
    }
    portEXIT_CRITICAL(&tp->data.lock);
    return ESP_OK;
}

static bool get_xy(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y, uint16_t *strength, uint8_t *point_num,
                   uint8_t max_point_num)
{
    portENTER_CRITICAL(&tp->data.lock);
    *point_num = tp->data.points > max_point_num ? max_point_num : tp->data.points;
    for (size_t i = 0; i < *point_num; i++) {
        x[i] = tp->data.coords[i].x;
        y[i] = tp->data.coords[i].y;
        if (strength) {
            strength[i] = tp->data.coords[i].strength;
        }
    }
    tp->data.points = 0;
    portEXIT_CRITICAL(&tp->data.lock);
    return *point_num > 0;
}

static esp_err_t del(esp_lcd_touch_handle_t tp)
{
    if (tp->config.int_gpio_num != GPIO_NUM_NC) {
        gpio_reset_pin(tp->config.int_gpio_num);
    }
    if (tp->config.rst_gpio_num != GPIO_NUM_NC) {
        gpio_reset_pin(tp->config.rst_gpio_num);
    }
    free(tp);
    return ESP_OK;
}

static esp_err_t reset(esp_lcd_touch_handle_t tp)
{
    if (tp->config.rst_gpio_num == GPIO_NUM_NC) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(gpio_set_level(tp->config.rst_gpio_num, tp->config.levels.reset), TAG, "rst");
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_RETURN_ON_ERROR(gpio_set_level(tp->config.rst_gpio_num, !tp->config.levels.reset), TAG, "rst");
    vTaskDelay(pdMS_TO_TICKS(100));
    return ESP_OK;
}

esp_err_t esp_lcd_touch_new_i2c_freenove35(const esp_lcd_panel_io_handle_t io, const esp_lcd_touch_config_t *config,
                                           esp_lcd_touch_handle_t *out_touch)
{
    ESP_RETURN_ON_FALSE(io && config && out_touch, ESP_ERR_INVALID_ARG, TAG, "args");
    esp_lcd_touch_handle_t tp = heap_caps_calloc(1, sizeof(esp_lcd_touch_t), MALLOC_CAP_DEFAULT);
    ESP_RETURN_ON_FALSE(tp, ESP_ERR_NO_MEM, TAG, "no mem");
    tp->io = io;
    tp->read_data = read_data;
    tp->get_xy = get_xy;
    tp->del = del;
    tp->data.lock.owner = portMUX_FREE_VAL;
    memcpy(&tp->config, config, sizeof(*config));

    esp_err_t ret = ESP_OK;
    if (tp->config.int_gpio_num != GPIO_NUM_NC) {
        const gpio_config_t int_cfg = { .mode = GPIO_MODE_INPUT, .pin_bit_mask = BIT64(tp->config.int_gpio_num) };
        ESP_GOTO_ON_ERROR(gpio_config(&int_cfg), err, TAG, "int gpio");
    }
    if (tp->config.rst_gpio_num != GPIO_NUM_NC) {
        const gpio_config_t rst_cfg = { .mode = GPIO_MODE_OUTPUT, .pin_bit_mask = BIT64(tp->config.rst_gpio_num) };
        ESP_GOTO_ON_ERROR(gpio_config(&rst_cfg), err, TAG, "rst gpio");
    }
    ESP_GOTO_ON_ERROR(reset(tp), err, TAG, "reset");

    /* Wait for the controller to settle, then ask how many fingers it tracks. */
    uint8_t status = 0xff;
    for (int i = 0; i < 50 && (status & 0x0f); i++) {
        if (rd(tp, REG_STATUS, &status, 1) != ESP_OK) {
            status = 0xff;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_GOTO_ON_FALSE(!(status & 0x0f), ESP_ERR_TIMEOUT, err, TAG, "not ready (status 0x%02x)", status);
    uint8_t maxp = 0;
    ESP_GOTO_ON_ERROR(rd(tp, REG_MAX_TOUCHES, &maxp, 1), err, TAG, "max touches");
    s_max_points = maxp < 1 ? 1 : maxp > POINTS_MAX ? POINTS_MAX : maxp;
    ESP_LOGI(TAG, "ready: up to %d points", s_max_points);
    *out_touch = tp;
    return ESP_OK;
err:
    ESP_LOGE(TAG, "init failed: %s", esp_err_to_name(ret));
    del(tp);
    return ret;
}
