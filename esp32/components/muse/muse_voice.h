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

#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/*
 * Push-to-talk turn loop: hold -> stream speech to Hatch, release -> think -> speak.
 * Out of Hatch's reach (Wi-Fi down, say) a note is saved to PSRAM and goes once
 * it's back, on boards with PSRAM. Consumes muse_input_event_t from `queue` and
 * drives muse_state for the UI.
 */
esp_err_t muse_voice_start(QueueHandle_t queue);

/* While on (settings' Sound page), idle mic audio feeds muse_voice_monitor_db(). */
void muse_voice_set_monitor(bool on);
/* Smoothed mic level in dBFS (fast attack, slow release). */
float muse_voice_monitor_db(void);

/* Plays a short chirp at the current volume (when idle). */
void muse_voice_request_chirp(void);
/* Plays a muse_sound_t `times` over (1..10) at the next idle moment; a press cuts repeats short.
 * False if one is already waiting. Wakes a resting Muse for it. */
bool muse_voice_request_sound(int sound, int times);
/* Plays 16 kHz mono PCM `times` over at the next idle moment (the mouth moves with it), then free()s it.
 * A press stops it. False if a clip is already waiting (the caller keeps the buffer). */
bool muse_voice_request_pcm(int16_t *pcm, size_t frames, int times);
/* Fills `buf` (a multiple of MUSE_AUDIO_CHUNK frames) from the mic at the next idle moment; the
 * face shows LISTENING / RECORDING meanwhile. False if one is already waiting. */
bool muse_voice_request_capture(int16_t *buf, size_t frames);
/* True once a capture has finished; `got` is how many frames were read. */
bool muse_voice_capture_done(size_t *got);

/* A live PCM stream from the serial voice link (s16le mono at rate_hz, resampled to the
 * codec): begin, write chunks as they arrive, end. The voice task plays it as it comes with
 * the speaking face and goes idle after END or when the data stops. Bytes, not frames. */
bool muse_voice_stream_begin(int rate_hz);
size_t muse_voice_stream_write(const void *data, size_t bytes, int wait_ms);
size_t muse_voice_stream_free(void);
void muse_voice_stream_end(void);
bool muse_voice_stream_active(void);
/* A caption for what the stream is about to say (SHOW). */
void muse_voice_stream_show(const char *text);

/* Runs muse_audio_loopback_test() at the current volume (when idle); results go to the log. */
void muse_voice_request_loopback(void);

/* Bench test: decodes and plays a built-in MP3 reply. */
void muse_voice_request_mp3test(void);

/* Asleep with nothing to play: codecs off, Wi-Fi dozing. */
bool muse_voice_resting(void);

/* Voice notes recorded out of Hatch's reach wait to go, the oldest from the
 * last half hour: worth keeping Wi-Fi up for. */
bool muse_voice_notes_waiting(void);
