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
 * Freenove ESP32-S3 Display 3.5" (FNK0104N). ESP32-S3-WROOM-1 N16R8: 16 MB flash, 8 MB octal
 * PSRAM. A 3.5" 320x480 ST77922 over QSPI with its touch built in (an in-cell controller at I2C
 * 0x55), an ES8311 codec driving the speaker header and reading the on-board MEMS mic, a WS2812
 * status LED, the BOOT button, and a battery sense divider. USB-C goes to the chip's own USB
 * Serial/JTAG. The sibling of board_freenove_s3_28.c.
 *
 * Pins from Freenove's own code and docs (docs.freenove.com/projects/fnk0104, the FNK0104N
 * branches):
 *   LCD:     Libraries/ST77922: QSPI CS 10, SCLK 12, D0 11, D1 13, D2 14, D3 9, BL 41 active
 *            high, 80 MHz; Freenove's init table below; MADCTL 0 is portrait 320x480.
 *   Touch:   ST77922_Touch: I2C SDA 38, SCL 39 at 0x55, RST 48, INT 47; 16-bit registers.
 *   Audio:   Chapter 7 Music: I2S MCLK 17, BCLK 18, WS 21, DOUT 15, DIN 16; ES8311 on the same
 *            I2C bus (0x18); amplifier enable GPIO1, driven LOW to turn on.
 *   LED:     WS2812 on GPIO40 (the gadget tools' light).
 *   Battery: GPIO8 reads half the battery voltage.
 *   Button:  BOOT on GPIO0, active low.
 *   SD:      CLK 5, CMD 4, D0 6, D1 7, D2 2, D3 3 (the gadget tools mount it).
 */
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "driver/usb_serial_jtag.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st77922.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "muse_audio.h"
#include "muse_board.h"
#include "muse_mem.h"
#include "touch_freenove35.h"

static const char *TAG = "board";

#define LCD_H_RES 320               /* as the UI draws it: portrait */
#define LCD_V_RES 480
#define LCD_HOST SPI2_HOST
#define LCD_CS GPIO_NUM_10
#define LCD_SCLK GPIO_NUM_12
#define LCD_D0 GPIO_NUM_11
#define LCD_D1 GPIO_NUM_13
#define LCD_D2 GPIO_NUM_14
#define LCD_D3 GPIO_NUM_9
#define LCD_BL GPIO_NUM_41
#define DRAW_BUF_LINES 10           /* two internal DMA buffers of 6.4 KB */

#define I2C_SDA GPIO_NUM_38
#define I2C_SCL GPIO_NUM_39
#define TP_RST GPIO_NUM_48
#define TP_INT GPIO_NUM_47

#define I2S_MCLK GPIO_NUM_17
#define I2S_BCLK GPIO_NUM_18
#define I2S_WS GPIO_NUM_21
#define I2S_DOUT GPIO_NUM_15
#define I2S_DIN GPIO_NUM_16
#define PA_EN GPIO_NUM_1            /* low = amplifier on */

#define TALK_GPIO GPIO_NUM_0        /* BOOT */
#define BATT_ADC ADC_CHANNEL_7      /* GPIO8, half the battery voltage */

static i2c_master_bus_handle_t s_i2c;
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_touch_handle_t s_tp;
static muse_gpio_button_t s_talk;
static adc_oneshot_unit_handle_t s_adc;
static esp_codec_dev_handle_t s_spk, s_mic;

/* Freenove's ST77922 bring-up, from Libraries/ST77922/ST77922.cpp, verbatim. */
static const st77922_lcd_init_cmd_t FREENOVE_INIT[] = {
    { 0xF1, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0x60, (uint8_t[]){ 0x00, 0x00, 0x00 }, 3, 0 },
    { 0x65, (uint8_t[]){ 0x80 }, 1, 0 },
    { 0x79, (uint8_t[]){ 0x06 }, 1, 0 },
    { 0x7B, (uint8_t[]){ 0x00, 0x08, 0x08 }, 3, 0 },
    { 0x80, (uint8_t[]){ 0x55, 0x62, 0x2F, 0x17, 0xF0, 0x52, 0x70, 0xD2, 0x52, 0x62, 0xEA }, 11, 0 },
    { 0x81, (uint8_t[]){ 0x26, 0x52, 0x72, 0x27 }, 4, 0 },
    { 0x84, (uint8_t[]){ 0x92, 0x25 }, 2, 0 },
    { 0x87, (uint8_t[]){ 0x10, 0x10, 0x58, 0x00, 0x02, 0x3A }, 6, 0 },
    { 0x88, (uint8_t[]){ 0x00, 0x00, 0x2C, 0x10, 0x04, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x06 }, 15, 0 },
    { 0x89, (uint8_t[]){ 0x00, 0x00, 0x00 }, 3, 0 },
    { 0x8A, (uint8_t[]){ 0x13, 0x00, 0x2C, 0x00, 0x00, 0x2C, 0x10, 0x10, 0x00, 0x3E, 0x19 }, 11, 0 },
    { 0x8B, (uint8_t[]){ 0x15, 0xB1, 0xB1, 0x44, 0x96, 0x2C, 0x10, 0x97, 0x8E }, 9, 0 },
    { 0x8C, (uint8_t[]){ 0x1D, 0xB1, 0xB1, 0x44, 0x96, 0x2C, 0x10, 0x50, 0x0F, 0x01, 0xC5, 0x12, 0x09 }, 13, 0 },
    { 0x8D, (uint8_t[]){ 0x0C }, 1, 0 },
    { 0x8E, (uint8_t[]){ 0x33, 0x01, 0x0C, 0x13, 0x01, 0x01 }, 6, 0 },
    { 0xB3, (uint8_t[]){ 0x00, 0x30 }, 2, 0 },
    { 0xF1, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0x71, (uint8_t[]){ 0xD0 }, 1, 0 },
    { 0x66, (uint8_t[]){ 0x02, 0x3F }, 2, 0 },
    { 0xBE, (uint8_t[]){ 0x26, 0x00, 0x9D }, 3, 0 },
    { 0x70, (uint8_t[]){ 0x01, 0xA0, 0x11, 0x40, 0xE0, 0x00, 0x11, 0x69, 0x11, 0x00, 0x00, 0x1A }, 12, 0 },
    { 0x90, (uint8_t[]){ 0x04, 0x04, 0x55, 0x74, 0x00, 0x40, 0x43, 0x27, 0x27 }, 9, 0 },
    { 0x91, (uint8_t[]){ 0x04, 0x04, 0x55, 0x75, 0x00, 0x40, 0x42, 0x27, 0x27 }, 9, 0 },
    { 0x92, (uint8_t[]){ 0x04, 0x44, 0x55, 0xC0, 0x06, 0x00, 0x07, 0x05, 0x90, 0x27 }, 10, 0 },
    { 0x93, (uint8_t[]){ 0x04, 0x43, 0x11, 0x00, 0x00, 0x00, 0x00, 0x05, 0x90, 0x27 }, 10, 0 },
    { 0x94, (uint8_t[]){ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, 6, 0 },
    { 0x95, (uint8_t[]){ 0x96, 0x16, 0x00, 0x00, 0xFF }, 5, 0 },
    { 0x96, (uint8_t[]){ 0x44, 0x53, 0x03, 0x12, 0x23, 0x24, 0x06, 0x05, 0x94, 0x27, 0x00, 0x44 }, 12, 0 },
    { 0x97, (uint8_t[]){ 0x44, 0x53, 0x47, 0x56, 0x20, 0x20, 0x02, 0x01, 0x94, 0x27, 0x00, 0x44 }, 12, 0 },
    { 0xBA, (uint8_t[]){ 0x55, 0x94, 0x2D, 0x94, 0x27 }, 5, 0 },
    { 0x9A, (uint8_t[]){ 0x40, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00 }, 7, 0 },
    { 0x9B, (uint8_t[]){ 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00 }, 7, 0 },
    { 0x9C, (uint8_t[]){ 0x5C, 0x12, 0x00, 0x00, 0x10, 0x12, 0x00, 0x00, 0x10, 0x02, 0x00, 0x00, 0x00 }, 13, 0 },
    { 0x9D, (uint8_t[]){ 0x8A, 0x51, 0x00, 0x00, 0x00, 0x80, 0x1E, 0x01 }, 8, 0 },
    { 0x9E, (uint8_t[]){ 0x51, 0x00, 0x00, 0x00, 0x80, 0x1E, 0x01 }, 7, 0 },
    { 0xB4, (uint8_t[]){ 0x1D, 0x1C, 0x1E, 0x0B, 0x14, 0x02, 0x13, 0x09, 0x1E, 0x00, 0x1E, 0x10 }, 12, 0 },
    { 0xB5, (uint8_t[]){ 0x1D, 0x1C, 0x1E, 0x0A, 0x15, 0x03, 0x11, 0x08, 0x1E, 0x01, 0x1E, 0x12 }, 12, 0 },
    { 0xB6, (uint8_t[]){ 0x77, 0x77, 0x00, 0x0A, 0xFF, 0x0A, 0xFF }, 7, 0 },
    { 0x86, (uint8_t[]){ 0xCD, 0x04, 0xB1, 0x02, 0x58, 0x12, 0x58, 0x0C, 0x13, 0x01, 0xA5, 0x00, 0xA5, 0xA5 }, 14, 0 },
    { 0xB7, (uint8_t[]){ 0x07, 0x0A, 0x0E, 0x06, 0x05, 0x03, 0x2B, 0x03, 0x03, 0x42, 0x07, 0x10, 0x10, 0x2E, 0x3F, 0x0D }, 16, 0 },
    { 0xB8, (uint8_t[]){ 0x07, 0x0A, 0x0D, 0x05, 0x05, 0x02, 0x2B, 0x02, 0x03, 0x42, 0x06, 0x10, 0x0F, 0x2E, 0x3F, 0x0D }, 16, 0 },
    { 0xB9, (uint8_t[]){ 0x23, 0x23 }, 2, 0 },
    { 0xBF, (uint8_t[]){ 0x10, 0x14, 0x14, 0x0B, 0x0B, 0x0B }, 6, 0 },
    { 0xF2, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0x73, (uint8_t[]){ 0x04, 0xDA, 0x12, 0x54, 0x47 }, 5, 0 },
    { 0x77, (uint8_t[]){ 0x6B, 0x5B, 0xFD, 0xC3, 0xC5 }, 5, 0 },
    { 0x7A, (uint8_t[]){ 0x15, 0x27 }, 2, 0 },
    { 0x7B, (uint8_t[]){ 0x04, 0x57 }, 2, 0 },
    { 0x7E, (uint8_t[]){ 0x01, 0x0E }, 2, 0 },
    { 0xBF, (uint8_t[]){ 0x36 }, 1, 0 },
    { 0xE3, (uint8_t[]){ 0x40, 0x40 }, 2, 0 },
    { 0xF0, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0xD0, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0x2A, (uint8_t[]){ 0x00, 0x00, 0x01, 0x3F }, 4, 0 },
    { 0x2B, (uint8_t[]){ 0x00, 0x00, 0x01, 0xDF }, 4, 0 },
    { 0x21, (uint8_t[]){ 0x00 }, 0, 0 },
    { 0x11, (uint8_t[]){ 0x00 }, 0, 120 },
    { 0x29, (uint8_t[]){ 0x00 }, 0, 0 },
    { 0x3A, (uint8_t[]){ 0x01 }, 1, 0 },
    { 0x36, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0x35, (uint8_t[]){ 0x01 }, 1, 20 },
};

static esp_err_t init(void)
{
    const i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &s_i2c), TAG, "i2c");
    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_talk, TALK_GPIO), TAG, "talk button");
    const adc_oneshot_unit_init_cfg_t adc_cfg = { .unit_id = ADC_UNIT_1 };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&adc_cfg, &s_adc), TAG, "adc");
    const adc_oneshot_chan_cfg_t ch_cfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
    return adc_oneshot_config_channel(s_adc, BATT_ADC, &ch_cfg);
}

static lv_display_t *display_start(lv_indev_t **touch)
{
    const ledc_timer_config_t bl_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 20000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    const ledc_channel_config_t bl_ch = {
        .gpio_num = LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
    };
    if (ledc_timer_config(&bl_timer) != ESP_OK || ledc_channel_config(&bl_ch) != ESP_OK) {
        return NULL;
    }

    const spi_bus_config_t bus = ST77922_PANEL_BUS_QSPI_CONFIG(LCD_SCLK, LCD_D0, LCD_D1, LCD_D2, LCD_D3,
                                                              LCD_H_RES * DRAW_BUF_LINES * 2);
    if (spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        return NULL;
    }
    esp_lcd_panel_io_handle_t io;
    const esp_lcd_panel_io_spi_config_t io_cfg = ST77922_PANEL_IO_QSPI_CONFIG(LCD_CS, NULL, NULL);
    if (esp_lcd_new_panel_io_spi(LCD_HOST, &io_cfg, &io) != ESP_OK) {
        return NULL;
    }
    st77922_vendor_config_t vendor = {
        .init_cmds = FREENOVE_INIT,
        .init_cmds_size = sizeof(FREENOVE_INIT) / sizeof(FREENOVE_INIT[0]),
        .flags = { .use_qspi_interface = 1 },
    };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = GPIO_NUM_NC,      /* on the board's reset line */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor,
    };
    if (esp_lcd_new_panel_st77922(io, &panel_cfg, &s_panel) != ESP_OK) {
        return NULL;
    }
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_disp_on_off(s_panel, true);

    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_cfg.task_core_id = MUSE_UI_CORE;
    adapter_cfg.task_priority = MUSE_UI_PRIORITY;
    if (esp_lv_adapter_init(&adapter_cfg) != ESP_OK) {
        return NULL;
    }
    const esp_lv_adapter_display_config_t disp_cfg = {
        .panel = s_panel,
        .panel_io = io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation = ESP_LV_ADAPTER_ROTATE_0,
            .hor_res = LCD_H_RES,
            .ver_res = LCD_V_RES,
            .buffer_height = DRAW_BUF_LINES,
            .use_psram = false,
            .require_double_buffer = true,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    lv_display_t *disp = esp_lv_adapter_register_display(&disp_cfg);
    if (!disp) {
        return NULL;
    }

    /* The panel's own touch controller on the shared I2C bus; portrait 320x480 like the panel. */
    esp_lcd_panel_io_handle_t tp_io;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_FREENOVE35_CONFIG();
    tp_io_cfg.scl_speed_hz = 100000;
    if (esp_lcd_new_panel_io_i2c(s_i2c, &tp_io_cfg, &tp_io) != ESP_OK) {
        ESP_LOGW(TAG, "touch io failed; running without touch");
        *touch = NULL;
        return esp_lv_adapter_start() == ESP_OK ? disp : NULL;
    }
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_H_RES,
        .y_max = LCD_V_RES,
        .rst_gpio_num = TP_RST,
        .int_gpio_num = GPIO_NUM_NC,
        .levels = { .reset = 0, .interrupt = 0 },
        .flags = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
    };
    if (esp_lcd_touch_new_i2c_freenove35(tp_io, &tp_cfg, &s_tp) != ESP_OK) {
        ESP_LOGW(TAG, "touch controller not responding; running without touch");
        *touch = NULL;
        return esp_lv_adapter_start() == ESP_OK ? disp : NULL;
    }
    esp_lv_adapter_touch_config_t lv_tp_cfg = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, s_tp);
    *touch = esp_lv_adapter_register_touch(&lv_tp_cfg);
    if (!*touch || esp_lv_adapter_start() != ESP_OK) {
        return NULL;
    }
    return disp;
}

static bool display_lock(int timeout_ms)
{
    return esp_lv_adapter_lock(timeout_ms) == ESP_OK;
}

static void set_brightness(int pct)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, pct * 1023 / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void panel_sleep(bool sleep)
{
    esp_lcd_panel_disp_sleep(s_panel, sleep);
}

static void display_pause(bool pause)
{
    if (pause) {
        esp_lv_adapter_pause(-1);
    } else {
        esp_lv_adapter_resume();
    }
}

static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    i2s_chan_handle_t tx, rx;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &tx, &rx), TAG, "i2s channel");
    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MUSE_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCLK,
            .bclk = I2S_BCLK,
            .ws = I2S_WS,
            .dout = I2S_DOUT,
            .din = I2S_DIN,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &std_cfg), TAG, "i2s tx");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx, &std_cfg), TAG, "i2s rx");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx), TAG, "i2s tx on");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(rx), TAG, "i2s rx on");

    audio_codec_i2s_cfg_t i2s_cfg = { .port = I2S_NUM_0, .rx_handle = rx, .tx_handle = tx };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    audio_codec_i2c_cfg_t i2c_cfg = { .port = I2C_NUM_0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = s_i2c };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(data_if && ctrl_if && gpio_if, ESP_ERR_NO_MEM, TAG, "codec interfaces");
    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = PA_EN,
        .pa_reverted = true,            /* Freenove drives the amplifier enable low to turn it on */
        .use_mclk = true,               /* MCLK is wired, on GPIO17 */
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    const audio_codec_if_t *codec = es8311_codec_new(&es_cfg);
    ESP_RETURN_ON_FALSE(codec, ESP_FAIL, TAG, "ES8311 not responding");
    esp_codec_dev_cfg_t out_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = codec, .data_if = data_if };
    esp_codec_dev_cfg_t in_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = codec, .data_if = data_if };
    *spk = s_spk = esp_codec_dev_new(&out_cfg);
    *mic = s_mic = esp_codec_dev_new(&in_cfg);
    return *spk && *mic ? ESP_OK : ESP_FAIL;
}

static unsigned poll_buttons(void)
{
    return muse_gpio_button_poll(&s_talk);
}

static void wait_buttons(int timeout_ms)
{
    muse_gpio_buttons_wait((muse_gpio_button_t *const[]){ &s_talk }, 1, timeout_ms);
}

static esp_err_t read_power(muse_power_t *out)
{
    int sum = 0;
    for (int i = 0; i < 8; i++) {
        int v;
        ESP_RETURN_ON_ERROR(adc_oneshot_read(s_adc, BATT_ADC, &v), TAG, "adc read");
        sum += v;
    }
    /* 12 dB attenuation reads about 3100 mV full scale; the divider halves the battery. */
    int mv = (sum / 8) * 3100 / 4095 * 2;
    out->usb = usb_serial_jtag_is_connected();
    out->charging = false;              /* the charger's status pin is not on a GPIO */
    if (mv < 2800) {                    /* nothing on the battery header */
        out->battery_pct = -1;
        out->battery_mv = 0;
        return ESP_OK;
    }
    int pct = (mv - 3300) * 100 / (4200 - 3300);
    out->battery_pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    out->battery_mv = mv;
    return ESP_OK;
}

static esp_err_t power_off(void)
{
    set_brightness(0);
    esp_lcd_panel_disp_on_off(s_panel, false);
    if (s_spk) {
        esp_codec_dev_close(s_spk);
    }
    if (s_mic) {
        esp_codec_dev_close(s_mic);
    }
    gpio_set_level(PA_EN, 1);           /* amplifier off */
    /* No power switch on this board: deep sleep until BOOT is pressed. */
    while (gpio_get_level(TALK_GPIO) == 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    esp_sleep_enable_ext0_wakeup(TALK_GPIO, 0);
    esp_deep_sleep_start();
    return ESP_FAIL;
}

static const muse_board_t s_board = {
    .name = "Freenove ESP32-S3 Display 3.5",
    .width = LCD_H_RES,
    .height = LCD_V_RES,
    .round = false,
    .touch = true,
    .diagonal_in = 3.5f,
    .talk_button = "boot",
    .talk_hint = { LV_ALIGN_BOTTOM_LEFT, 8, -6 },
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    .panel_sleep = panel_sleep,
    .display_pause = display_pause,
    .audio_init = audio_init,
    .mic_slot = 0,                      /* the ES8311's one ADC, on the left slot */
    .poll_buttons = poll_buttons,
    .wait_buttons = wait_buttons,
    .read_power = read_power,
    .power_off = power_off,
};

const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
