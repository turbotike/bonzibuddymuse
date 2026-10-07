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
#include "gadget_pet.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "pet.h"

static const char *TAG = "gadget_pet";

static cJSON *ok_with(cJSON *payload)
{
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddItemToObject(result, "payload", payload ? payload : cJSON_CreateObject());
    return result;
}

static cJSON *fail(const char *code, const char *message)
{
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", false);
    cJSON *error = cJSON_CreateObject();
    cJSON_AddStringToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message);
    cJSON_AddItemToObject(result, "error", error);
    return result;
}

static const char *str_param(cJSON *params, const char *key)
{
    cJSON *item = params ? cJSON_GetObjectItem(params, key) : NULL;
    return cJSON_IsString(item) && item->valuestring && item->valuestring[0] ? item->valuestring : NULL;
}

static bool int_param(cJSON *params, const char *key, int *out)
{
    cJSON *item = params ? cJSON_GetObjectItem(params, key) : NULL;
    if (cJSON_IsNumber(item)) {
        *out = (int)item->valuedouble;
        return true;
    }
    if (cJSON_IsString(item) && item->valuestring) {
        char *end;
        long v = strtol(item->valuestring, &end, 10);
        if (end != item->valuestring && !*end) {
            *out = (int)v;
            return true;
        }
    }
    return false;
}

static bool bool_param(cJSON *params, const char *key, bool *out)
{
    cJSON *item = params ? cJSON_GetObjectItem(params, key) : NULL;
    if (cJSON_IsBool(item)) {
        *out = cJSON_IsTrue(item);
        return true;
    }
    if (cJSON_IsString(item) && item->valuestring) {
        *out = !strcmp(item->valuestring, "true") || !strcmp(item->valuestring, "1") || !strcmp(item->valuestring, "yes") ||
               !strcmp(item->valuestring, "on");
        return true;
    }
    return false;
}

static cJSON *param_spec(const char *type, const char *description)
{
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "type", type);
    cJSON_AddStringToObject(p, "description", description);
    return p;
}

static void add_command(cJSON *commands, const char *name, const char *description, cJSON *required, cJSON *optional)
{
    cJSON *command = cJSON_CreateObject();
    cJSON_AddStringToObject(command, "description", description);
    cJSON_AddItemToObject(command, "required", required ? required : cJSON_CreateObject());
    cJSON_AddItemToObject(command, "optional", optional ? optional : cJSON_CreateObject());
    cJSON_AddItemToObject(commands, name, command);
}

static cJSON *status_payload(void)
{
    char *buf = malloc(1024);
    if (!buf) {
        return cJSON_CreateObject();
    }
    pet_status_json(buf, 1024);
    cJSON *j = cJSON_Parse(buf);
    free(buf);
    return j ? j : cJSON_CreateObject();
}

void gadget_pet_init(void)
{
    pet_init();
    ESP_LOGI(TAG, "pet commands ready");
}

void gadget_pet_add_commands(cJSON *commands)
{
    cJSON *req, *opt;

    add_command(commands, "pet.status",
                "The creature living on the gadget: name, stage (egg/baby/kid/teen/adult/elder), age, mood, needs "
                "(food, energy, fun, clean, bond; 0-100, low is bad), health, what it looks like, what it last said.",
                NULL, NULL);

    opt = cJSON_CreateObject();
    cJSON_AddItemToObject(opt, "snack", param_spec("boolean", "A snack (small, fun, unhealthy when full) instead of a meal."));
    add_command(commands, "pet.feed", "Feed the creature a meal (or a snack). Not while it sleeps.", NULL, opt);

    opt = cJSON_CreateObject();
    cJSON_AddItemToObject(opt, "hits", param_spec("integer", "Hits out of 5 (default 3)."));
    add_command(commands, "pet.train", "Train the creature (Digimon style): raises fun, bond and POWER, which shapes its next evolution.", NULL, opt);
    add_command(commands, "pet.play", "Same as pet.train with 3 hits: a bit of fun.", NULL, NULL);
    opt = cJSON_CreateObject();
    cJSON_AddItemToObject(opt, "theme", param_spec("integer", "0 NEON, 1 TOXIC, 2 LAVA, 3 ARCADE; omit to cycle."));
    add_command(commands, "pet.theme", "Switch the screen's neon colour scheme.", NULL, opt);

    add_command(commands, "pet.clean", "Clean up after it (poop on the floor, dirt) and bathe it.", NULL, NULL);
    add_command(commands, "pet.medicine", "Give medicine when it is sick (two doses cure it).", NULL, NULL);

    req = cJSON_CreateObject();
    cJSON_AddItemToObject(req, "off", param_spec("boolean", "true: lights off, it sleeps; false: lights on, it wakes."));
    add_command(commands, "pet.lights", "Lights out (it sleeps and regains energy) or back on.", req, NULL);

    req = cJSON_CreateObject();
    cJSON_AddItemToObject(req, "name", param_spec("string", "Its name, up to 15 characters."));
    add_command(commands, "pet.name", "Name the creature (it keeps the name through its life).", req, NULL);

    req = cJSON_CreateObject();
    cJSON_AddItemToObject(req, "text", param_spec("string", "What it says, under 120 characters, in its own small voice."));
    opt = cJSON_CreateObject();
    cJSON_AddItemToObject(opt, "seconds", param_spec("integer", "How long the bubble stays (default 12)."));
    add_command(commands, "pet.say", "The creature says something in a speech bubble on the screen, with a chirp.", req, opt);

    req = cJSON_CreateObject();
    cJSON_AddItemToObject(req, "mood", param_spec("string", "content, happy, hungry, tired, bored, dirty, lonely, sick or asleep."));
    opt = cJSON_CreateObject();
    cJSON_AddItemToObject(opt, "minutes", param_spec("integer", "How long the mood holds, 1-240 (default 10)."));
    add_command(commands, "pet.set_mood", "Set how the creature feels and acts for a while, over its needs.", req, opt);

    opt = cJSON_CreateObject();
    cJSON_AddItemToObject(opt, "variant", param_spec("string", "noble, cute or feral; default: decided by how it was cared for."));
    add_command(commands, "pet.evolve",
                "Choose the shape of its next evolution; evolves now if its stage is nearly over, else at the stage's end.",
                NULL, opt);

    req = cJSON_CreateObject();
    cJSON_AddItemToObject(req, "confirm", param_spec("boolean", "Must be true: the current creature is gone for good."));
    add_command(commands, "pet.new_egg", "Replace the creature with a new egg of the next generation. Only if Mat asks.", req, NULL);

    req = cJSON_CreateObject();
    cJSON_AddItemToObject(req, "scale", param_spec("integer", "1 = real time; up to 200 (an hour of life every 18 s). Not saved."));
    add_command(commands, "pet.time_scale", "Speed its life up to watch it grow (for testing).", req, NULL);

    add_command(commands, "pet.pet", "Pet it (a stroke), as a tap on the screen does; warms an egg.", NULL, NULL);

    req = cJSON_CreateObject();
    cJSON_AddItemToObject(req, "stage", param_spec("string", "egg, baby, kid, teen, adult or elder."));
    add_command(commands, "pet.debug_stage", "Testing only: jump the creature to a life stage (its age moves with it).", req, NULL);
}

static pet_mood_t parse_mood(const char *s)
{
    for (int i = 0; i < PET_MOOD_COUNT; i++) {
        if (!strcmp(s, pet_mood_name((pet_mood_t)i))) {
            return (pet_mood_t)i;
        }
    }
    return PET_MOOD_COUNT;
}

cJSON *gadget_pet_command(const char *command, cJSON *params, const char *request_id, uint32_t gen)
{
    (void)request_id;
    (void)gen;
    if (strncmp(command, "pet.", 4) != 0) {
        return NULL;
    }
    const char *sub = command + 4;
    if (!strcmp(sub, "status")) {
        return ok_with(status_payload());
    }
    if (!strcmp(sub, "feed")) {
        bool snack = false;
        bool_param(params, "snack", &snack);
        if (!pet_feed(snack)) {
            return fail("not_now", "It can't eat now (asleep, still eating, or an egg).");
        }
        return ok_with(status_payload());
    }
    if (!strcmp(sub, "train")) {
        int hits = 3;
        int_param(params, "hits", &hits);
        if (!pet_train(hits, 5)) {
            return fail("not_now", "It can't train now (asleep or an egg).");
        }
        return ok_with(status_payload());
    }
    if (!strcmp(sub, "theme")) {
        int theme;
        if (!int_param(params, "theme", &theme)) {
            theme = (pet_theme() + 1) % 4;
        }
        pet_set_theme(theme % 4);
        return ok_with(status_payload());
    }
    if (!strcmp(sub, "play")) {
        int score = 5;
        int_param(params, "score", &score);
        if (!pet_train(score / 2, 5)) {
            return fail("not_now", "It can't play now (asleep or an egg).");
        }
        return ok_with(status_payload());
    }
    if (!strcmp(sub, "clean")) {
        if (!pet_clean()) {
            return fail("not_now", "Nothing to clean: it's an egg.");
        }
        return ok_with(status_payload());
    }
    if (!strcmp(sub, "medicine")) {
        if (!pet_medicine()) {
            return fail("not_now", "It isn't sick.");
        }
        return ok_with(status_payload());
    }
    if (!strcmp(sub, "lights")) {
        bool off;
        if (!bool_param(params, "off", &off)) {
            return fail("bad_param", "off (boolean) is required");
        }
        if (!pet_lights(off)) {
            return fail("not_now", "An egg has no lights.");
        }
        return ok_with(status_payload());
    }
    if (!strcmp(sub, "name")) {
        const char *name = str_param(params, "name");
        if (!name || !pet_set_name(name)) {
            return fail("bad_param", "name is required");
        }
        return ok_with(status_payload());
    }
    if (!strcmp(sub, "say")) {
        const char *text = str_param(params, "text");
        if (!text) {
            return fail("bad_param", "text is required");
        }
        int secs = 12;
        int_param(params, "seconds", &secs);
        pet_say(text, secs);
        return ok_with(NULL);
    }
    if (!strcmp(sub, "set_mood")) {
        const char *mood = str_param(params, "mood");
        pet_mood_t m = mood ? parse_mood(mood) : PET_MOOD_COUNT;
        if (m == PET_MOOD_COUNT) {
            return fail("bad_param", "mood must be content, happy, hungry, tired, bored, dirty, lonely, sick or asleep");
        }
        int minutes = 10;
        int_param(params, "minutes", &minutes);
        pet_set_mood(m, minutes);
        return ok_with(status_payload());
    }
    if (!strcmp(sub, "evolve")) {
        const char *v = str_param(params, "variant");
        int variant = !v ? 0 : !strcmp(v, "noble") ? 1 : !strcmp(v, "cute") ? 2 : !strcmp(v, "feral") ? 3 : 0;
        bool now = pet_evolve(variant);
        cJSON *payload = status_payload();
        cJSON_AddBoolToObject(payload, "evolved_now", now);
        if (!now) {
            cJSON_AddStringToObject(payload, "note", "Too early in its stage; the choice is kept for when the stage ends.");
        }
        return ok_with(payload);
    }
    if (!strcmp(sub, "new_egg")) {
        bool confirm = false;
        bool_param(params, "confirm", &confirm);
        if (!confirm) {
            return fail("confirm", "Set confirm=true to replace the creature with a new egg.");
        }
        pet_new_egg();
        return ok_with(status_payload());
    }
    if (!strcmp(sub, "time_scale")) {
        int scale;
        if (!int_param(params, "scale", &scale)) {
            return fail("bad_param", "scale (1-200) is required");
        }
        pet_set_time_scale(scale);
        cJSON *payload = cJSON_CreateObject();
        cJSON_AddNumberToObject(payload, "time_scale", pet_time_scale());
        return ok_with(payload);
    }
    if (!strcmp(sub, "pet")) {
        pet_tap();
        return ok_with(status_payload());
    }
    if (!strcmp(sub, "debug_stage")) {
        const char *st = str_param(params, "stage");
        int stage = -1;
        for (int i = 0; st && i < PET_STAGE_COUNT; i++) {
            if (!strcmp(st, pet_stage_name((pet_stage_t)i))) {
                stage = i;
            }
        }
        if (stage < 0 || !pet_debug_stage((pet_stage_t)stage)) {
            return fail("bad_param", "stage must be egg, baby, kid, teen, adult or elder");
        }
        return ok_with(status_payload());
    }
    return fail("unknown_command", "No such pet command.");
}
