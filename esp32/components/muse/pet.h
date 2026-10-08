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

/* Species: the body plan. */
typedef enum { PET_SP_REX, PET_SP_RAPTOR, PET_SP_SAUROPOD, PET_SP_STEGO, PET_SP_CERATOPS, PET_SP_ANKYLO, PET_SP_PTERO, PET_SP_COUNT } pet_species_t;

/* A dinosaur, Digimon style: an in-training blob as a baby, then a chibi dino
 * that grows into its species, picking up its crest, back and tail features. */
typedef struct {
    uint32_t seed;
    uint8_t species;          /* pet_species_t */
    uint8_t size;             /* 0..255: build, around 128 */
    uint8_t head_size;        /* 0..255 */
    uint8_t neck;             /* 0..255: neck length (the species sets the range) */
    uint8_t tail_len;         /* 0..255 */
    uint8_t eye_n;            /* 1..3 */
    uint8_t eye_size;         /* 2..5 */
    uint8_t jaw;              /* 0 round snout, 1 long snout, 2 beak */
    uint8_t teeth;            /* 0 none, 1 some, 2 fangs */
    uint8_t crest;            /* 0 none, 1 nose horn, 2 brow horns, 3 feather crest, 4 frill and horns, 5 long head crest */
    uint8_t back;             /* 0 smooth, 1 spikes, 2 plates, 3 sail, 4 bumps */
    uint8_t tail_tip;         /* 0 plain, 1 club, 2 spikes, 3 tuft */
    uint8_t pattern;          /* 0 plain, 1 spots, 2 stripes (all have a cream belly) */
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
    uint8_t power;            /* 0..100, built by training; shapes the evolution */
    uint8_t theme;            /* the screen's colour scheme, kept with the pet */
    /* A battle, while one is on. */
    bool battle;
    bool wild;                /* a wild one turned up (else a challenge) */
    uint8_t battle_phase;     /* pet_battle_phase_t */
    uint8_t battle_anim;      /* pet_battle_anim_t */
    float battle_anim_t;
    pet_genome_t enemy;
    uint8_t enemy_stage;
    uint8_t elem, enemy_elem; /* pet_element_t */
    int16_t hp, hp_max, enemy_hp, enemy_hp_max;
    char battle_msg[48];
    char enemy_name[20];
} pet_view_t;

/* Battles: fire beats leaf beats rock beats wind beats fire. */
typedef enum { PET_EL_FIRE, PET_EL_WIND, PET_EL_LEAF, PET_EL_ROCK, PET_EL_COUNT } pet_element_t;
typedef enum { PET_BT_NONE, PET_BT_INTRO, PET_BT_MENU, PET_BT_ACTING, PET_BT_WON, PET_BT_LOST, PET_BT_RAN, PET_BT_FLED } pet_battle_phase_t;
typedef enum { PET_BA_NONE, PET_BA_PET_LUNGE, PET_BA_ENEMY_LUNGE, PET_BA_PET_HURT, PET_BA_ENEMY_HURT, PET_BA_PET_FAINT, PET_BA_ENEMY_FAINT } pet_battle_anim_t;
enum { PET_MOVE_BITE, PET_MOVE_SPECIAL, PET_MOVE_GUARD, PET_MOVE_RUN };

/* A wild dino appears (or the pet picks a fight); false if it can't now (egg, baby, asleep, busy). */
bool pet_battle_start(bool wild);
/* At the menu: PET_MOVE_*; false if it isn't the pet's turn to choose. */
bool pet_battle_choose(int move);
/* Closes a finished battle (or sends an ignored wild one away). */
void pet_battle_dismiss(void);
pet_element_t pet_species_element(pet_species_t sp);
const char *pet_element_name(pet_element_t e);
const char *pet_special_name(pet_element_t e);

void pet_init(void);
void pet_view(pet_view_t *out);

/* Care, from the screen, from Muse's commands, or the pet's own routine. False: not now. */
bool pet_feed(bool snack);
bool pet_play(int score);       /* a finished game, score 0..10 */
bool pet_train(int hits, int rounds);   /* Digimon-style training: hits of rounds */
void pet_set_theme(int theme);  /* saved with the pet */
int pet_theme(void);
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
/* Testing: jump to a stage (the age moves with it). */
bool pet_debug_stage(pet_stage_t stage);
/* Testing: a fresh adult of that species (pet_species_t), a new creature. */
bool pet_debug_species(int species);

const char *pet_species_name(pet_species_t sp);

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
