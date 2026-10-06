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
#include "muse_voice_link.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"

#include "muse_board.h"
#include "muse_chat.h"
#include "muse_voice.h"
#include "muse_wifi.h"

static const char *TAG = "voice_link";

#define PORT CONFIG_MUSE_VOICE_LINK_PORT
#define VL_LINE_MAX 320
#define CHUNK 1024
#define MAX_SAY (1024 * 1024)
#define READY_WAIT_MS 10000
#define DATA_WAIT_MS 3000
#define BEACON_S 5

static volatile int s_client = -1;
static SemaphoreHandle_t s_send_lock;
static int s_rate = 11025;
static bool s_authed;   /* AUTH <key> seen on this connection (or no key configured) */

static void reply(int fd, const char *line)
{
    xSemaphoreTake(s_send_lock, portMAX_DELAY);
    send(fd, line, strlen(line), 0);
    xSemaphoreGive(s_send_lock);
}

void muse_voice_link_console_line(const char *line, size_t n)
{
    int fd = s_client;
    if (fd < 0 || !s_send_lock) {
        return;
    }
    if (xSemaphoreTake(s_send_lock, pdMS_TO_TICKS(200)) == pdTRUE) {
        send(fd, line, n, MSG_DONTWAIT);
        xSemaphoreGive(s_send_lock);
    }
}

/* Exactly n bytes, or fewer at a timeout / close. */
static size_t recv_all(int fd, uint8_t *buf, size_t n, int wait_ms)
{
    size_t got = 0;
    int idle = 0;
    while (got < n) {
        int r = recv(fd, buf + got, n - got, 0);
        if (r > 0) {
            got += r;
            idle = 0;
        } else if (r == 0) {
            break;
        } else if (errno == EWOULDBLOCK || errno == EAGAIN) {
            idle += 100;
            if (idle >= wait_ms) {
                break;
            }
        } else {
            break;
        }
    }
    return got;
}

static bool say(int fd, size_t n)
{
    if (!muse_voice_stream_active() && !muse_voice_stream_begin(s_rate)) {
        reply(fd, "ERR no stream\n");
        return true;
    }
    int waited = 0;
    while (muse_voice_stream_free() < n && waited < READY_WAIT_MS) {
        vTaskDelay(pdMS_TO_TICKS(20));
        waited += 20;
    }
    if (muse_voice_stream_free() < n) {
        reply(fd, "ERR buffer full\n");
        return true;
    }
    reply(fd, "READY\n");
    static uint8_t buf[CHUNK];
    size_t got = 0;
    while (got < n) {
        size_t want = n - got < sizeof(buf) ? n - got : sizeof(buf);
        size_t r = recv_all(fd, buf, want, DATA_WAIT_MS);
        if (!r) {
            reply(fd, "ERR timeout\n");
            return false;
        }
        got += r;
        muse_voice_stream_write(buf, r, 1000);
    }
    return true;
}

/* One command line; false when the connection is done for. */
static bool command(int fd, char *line)
{
    if (!strncmp(line, "SHOW ", 5)) {
        muse_voice_stream_show(line + 5);
        reply(fd, "OK\n");
    } else if (!strncmp(line, "SAY ", 4)) {
        char *end;
        long n = strtol(line + 4, &end, 10);
        if (n <= 0 || n > MAX_SAY || (n & 1) || *end) {
            reply(fd, "ERR bad SAY\n");
        } else {
            return say(fd, (size_t)n);
        }
    } else if (!strncmp(line, "RATE ", 5)) {
        int r = atoi(line + 5);
        if (r >= 8000 && r <= 48000) {
            s_rate = r;
            reply(fd, "OK\n");
        } else {
            reply(fd, "ERR bad RATE\n");
        }
    } else if (!strcmp(line, "END")) {
        muse_voice_stream_end();
        reply(fd, "OK\n");
    } else if (!strcmp(line, "PING")) {
        reply(fd, "PONG\n");
    } else if (!strncmp(line, "AUTH ", 5)) {
        s_authed = CONFIG_MUSE_VOICE_LINK_KEY[0] == '\0' || !strcmp(line + 5, CONFIG_MUSE_VOICE_LINK_KEY);
        reply(fd, s_authed ? "OK\n" : "ERR bad key\n");
    } else if (!strncmp(line, "CHAT ", 5)) {
        /* A typed turn, as the USB console's ">chat=" does; the reply comes back as @chat frames. */
        if (!s_authed) {
            reply(fd, "ERR AUTH first\n");
        } else if (!muse_hatch_ready()) {
            reply(fd, "ERR Muse not reachable\n");
        } else {
            char *text = strdup(line + 5);
            if (text) {
                muse_hatch_unescape(text);
                muse_hatch_text_turn(text);   /* frees it */
                reply(fd, "OK\n");
            } else {
                reply(fd, "ERR no memory\n");
            }
        }
    } else if (line[0]) {
        reply(fd, "ERR unknown\n");
    }
    return true;
}

static void serve(int fd)
{
    char line[VL_LINE_MAX];
    size_t len = 0;
    char hello[96];
    snprintf(hello, sizeof(hello), "HELLO muse-voice 1 %s\n", muse_board ? muse_board->name : "gadget");
    reply(fd, hello);
    for (;;) {
        char c;
        int r = recv(fd, &c, 1, 0);
        if (r == 0) {
            break;
        }
        if (r < 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                continue;   /* the client may sit quiet between utterances */
            }
            break;
        }
        if (c == '\r') {
            continue;
        }
        if (c != '\n') {
            if (len < sizeof(line) - 1) {
                line[len++] = c;
            }
            continue;
        }
        line[len] = '\0';
        len = 0;
        if (!command(fd, line)) {
            break;
        }
    }
}

static void beacon(void)
{
    muse_wifi_status_t st;
    muse_wifi_status(&st);
    if (st.state != MUSE_WIFI_CONNECTED) {
        return;
    }
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return;
    }
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(PORT), .sin_addr.s_addr = htonl(INADDR_BROADCAST) };
    char msg[128];
    int n = snprintf(msg, sizeof(msg), "MUSEVOICE %s %d %s\n", st.ip, PORT, muse_board ? muse_board->name : "gadget");
    sendto(fd, msg, n, 0, (struct sockaddr *)&to, sizeof(to));
    close(fd);
}

static void link_task(void *arg)
{
    int srv = -1;
    int64_t last_beacon = 0;
    for (;;) {
        if (srv < 0) {
            srv = socket(AF_INET, SOCK_STREAM, 0);
            if (srv < 0) {
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
            int yes = 1;
            setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
            struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(PORT), .sin_addr.s_addr = htonl(INADDR_ANY) };
            if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(srv, 1) < 0) {
                close(srv);
                srv = -1;
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
            struct timeval tv = { .tv_sec = 1 };
            setsockopt(srv, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            ESP_LOGI(TAG, "listening on TCP %d (beacon on UDP %d)", PORT, PORT);
        }
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int fd = accept(srv, (struct sockaddr *)&peer, &plen);
        if (fd < 0) {
            int64_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
            if (now - last_beacon >= BEACON_S * 1000) {
                beacon();
                last_beacon = now;
            }
            continue;
        }
        int yes = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
        struct timeval tv = { .tv_usec = 100 * 1000 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        struct timeval stv = { .tv_sec = 2 };
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &stv, sizeof(stv));
        char who[16];
        inet_ntoa_r(peer.sin_addr, who, sizeof(who));
        ESP_LOGI(TAG, "client %s connected", who);
        s_client = fd;
        s_authed = CONFIG_MUSE_VOICE_LINK_KEY[0] == '\0';
        serve(fd);
        s_client = -1;
        muse_voice_stream_end();
        close(fd);
        ESP_LOGI(TAG, "client %s gone", who);
    }
}

void muse_voice_link_start(void)
{
    if (!s_send_lock) {
        s_send_lock = xSemaphoreCreateMutex();
    }
    xTaskCreate(link_task, "voice_link", 4096, NULL, 4, NULL);
}
