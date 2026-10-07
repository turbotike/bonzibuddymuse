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
 * The pet: a creature that lives on the gadget. Its life (needs that decay,
 * sleep, growth through stages, sickness) runs here on the board, once a
 * second, and survives power-off in NVS. Muse is its spirit: the pet reports
 * to it now and then and when something happens, and Muse answers in the
 * pet's voice (a speech bubble) and steers it through the pet.* commands.
 *
 * The creature's body comes from a genome (pet_genome_t), drawn by the avatar
 * renderer (avatar/muse_pixel.c) from the snapshot pet_view() hands out.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { PET_EGG, PET_BABY, PET_KID, PET_TEEN, PET_ADULT, PET_ELDER, PET_STAGE_COUNT } pet_stage_t;

typedef enum { PET_NEED_FOOD, PET_NEED_ENERGY, PET_NEED_FUN, PET_NEED_CLEAN, PET_NEED_BOND, PET_NEED_COUNT } pet_need_t;

typedef enum {
    PET_MOOD_CONTENT,
    PET_MOOD_HAPPY,
    PET_MOOD_HUNGRY,
    PET_MOOD_TIRED,
    PET_MOOD_BORED,
    PET_MOOD_DIRTY,
    PET_MOOD_LONELY,
    PET_MOOD_SICK,
    PET_MOOD_ASLEEP,
    PET_MOOD_COUNT
} pet_mood_t;

/* What the body is doing right now; the renderer animates it from anim_t. */
typedef enum {
    PET_ANIM_IDLE,
    PET_ANIM_SLEEP,
    PET_ANIM_EAT,
    PET_ANIM_PLAY,
    PET_ANIM_HAPPY,
    PET_ANIM_SAD,
    PET_ANIM_SICK,
    PET_ANIM_CLEAN,
    PET_ANIM_HATCH,
    PET_ANIM_EVOLVE,
    PET_ANIM_LEAVE,
    PET_ANIM_COUNT
} pet_anim_t;

/* Trait bits: how the needs move and how it behaves. */
#define PET_TRAIT_BOLD 0x01
#define PET_TRAIT_LAZY 0x02
#define PET_TRAIT_SOCIAL 0x04
#define PET_TRAIT_GREEDY 0x08

typedef struct {
    uint32_t seed;
    uint8_t body_w, body_h;   /* adult size, art pixels (of the 64-px grid) */
    uint8_t shape;            /* 0 round, 1 tall, 2 wide, 3 pear, 4 boxy */
    uint8_t eye_n;            /* 1..3 */
    uint8_t eye_size;         /* 2..5 */
    uint8_t mouth;            /* 0 smile, 1 beak, 2 fangs, 3 flat */
    uint8_t head;             /* 0 none, 1 ears, 2 horns, 3 antennae, 4 crest, 5 fin */
    uint8_t limb;             /* 0 none, 1 stubs, 2 legs, 3 arms and legs, 4 fins, 5 wings */
    uint8_t tail;             /* 0 none, 1 stub, 2 long, 3 flame */
    uint8_t pattern;          /* 0 plain, 1 belly, 2 spots, 3 stripes */
    uint8_t hue, hue2, eye_hue, sat;   /* 0..255 */
    uint8_t voice;            /* chirp pitch */
    uint8_t traits;           /* PET_TRAIT_* */
} pet_genome_t;

/* A snapshot for the renderer and the UI; copied out under the pet's lock. */
typedef struct {
    pet_genome_t g;
    pet_stage_t stage;
    pet_anim_t anim;
    float anim_t;             /* seconds into the animation */
    pet_mood_t mood;
    uint8_t needs[PET_NEED_COUNT];   /* 0..100, 100 = satisfied */
    uint8_t health;           /* 0..100 */
    bool asleep;
    bool sick;
    bool lights_off;
    uint8_t poops;            /* piles on the floor */
    float egg_warmth;         /* egg: 0..1 towards hatching */
    uint32_t age_min;         /* minutes since hatching (egg: since laid) */
    uint16_t generation;
    char name[16];
    uint8_t care;             /* 0..100, how well it has been looked after this stage */
} pet_view_t;

void pet_init(void);
void pet_view(pet_view_t *out);

/* Care, from the screen, from Muse's commands, or the pet's own routine. False: not now. */
bool pet_feed(bool snack);
bool pet_play(int score);       /* a finished game, score 0..10 */
bool pet_clean(void);
bool pet_medicine(void);
bool pet_lights(bool off);
void pet_tap(void);             /* petting; warms an egg */
bool pet_set_name(const char *name);
bool pet_set_mood(pet_mood_t mood, int minutes);   /* Muse's say over the mood, for a while */
bool pet_evolve(int variant);   /* 0 auto, 1 noble, 2 cute, 3 feral; only near the end of a stage */
void pet_new_egg(void);
/* Faster life for watching it grow: 1 = real time, up to 200. Not saved. */
void pet_set_time_scale(int scale);
int pet_time_scale(void);

/* A speech bubble on the screen (the UI shows it) plus a chirp. */
void pet_say(const char *text, int secs);

/* Status as JSON (for pet.status and the console); returns the length. */
int pet_status_json(char *out, size_t cap);

const char *pet_stage_name(pet_stage_t s);
const char *pet_mood_name(pet_mood_t m);
const char *pet_need_name(pet_need_t n);

/* The pet asks Muse something (a report); the typed reply comes back through
 * muse_hatch_typed_reply(). Rate limited; false if it didn't go. */
bool pet_report(const char *reason);

/* Implemented by the UI: show a speech bubble under the creature. */
void pet_ui_bubble(const char *text, int secs);

#ifdef __cplusplus
}
#endif
