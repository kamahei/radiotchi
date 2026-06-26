// Radiotchi — pet care/mood (pure, libm-free). See pet_mood.h.

#include "pet_mood.h"

#include "pet_growth.h" // pet_growth_meal_quality (shared delicacy weighting)

void pet_care_init(PetCare* c) {
    if(c == NULL) return;
    c->last_feed_time = 0u;
    c->last_meal_quality = 0u;
}

// Seconds since the last meal, guarding a never-fed pet (0) and a backwards/reset clock
// (treat as just-fed) so neither produces phantom hunger.
static uint64_t elapsed_since_feed(const PetCare* c, uint64_t now) {
    if(c->last_feed_time == 0u) return 0u; // never fed => grace
    if(now <= c->last_feed_time) return 0u; // clock went backwards => treat as fresh
    return now - c->last_feed_time;
}

float pet_hunger(const PetCare* c, uint64_t now) {
    if(c == NULL || c->last_feed_time == 0u) return 0.0f;
    uint64_t e = elapsed_since_feed(c, now);
    if(e <= PET_HUNGER_FULL_SECS) return 0.0f;
    if(e >= PET_HUNGER_STARVE_SECS) return 1.0f;
    // Linear ramp between FULL and STARVE (integer numerator/denominator, libm-free).
    uint64_t num = e - PET_HUNGER_FULL_SECS;
    uint64_t den = (uint64_t)PET_HUNGER_STARVE_SECS - (uint64_t)PET_HUNGER_FULL_SECS;
    return (float)num / (float)den;
}

PetMood pet_mood(const PetCare* c, uint64_t now) {
    if(c == NULL || c->last_feed_time == 0u) return MOOD_NEUTRAL;
    uint64_t e = elapsed_since_feed(c, now);
    if(e >= PET_NEGLECT_SECS) return MOOD_NEGLECTED;
    if(e > PET_HUNGER_FULL_SECS) return MOOD_HUNGRY; // hunger starts AFTER FULL (matches pet_hunger)
    // Recently fed: a delicacy leaves it content, an ordinary meal merely happy.
    if(c->last_meal_quality >= PET_DELICACY_QUALITY) return MOOD_CONTENT;
    return MOOD_HAPPY;
}

void pet_care_feed(PetCare* c, const CaptureEvent* ev, uint64_t now) {
    if(c == NULL || ev == NULL) return;
    c->last_feed_time = now;
    float q = pet_growth_meal_quality(ev); // [0,1], same weighting as the exp curve
    if(q < 0.0f) q = 0.0f;
    if(q > 1.0f) q = 1.0f;
    c->last_meal_quality = (uint8_t)(q * 100.0f + 0.5f); // centi
}

uint32_t pet_mood_anim_frame(PetMood mood, uint32_t frame) {
    switch(mood) {
    case MOOD_HUNGRY:
        // Only the two rest poses (frames 0 and 2): a slow, sluggish sway.
        return (frame & 1u) ? 2u : 0u;
    case MOOD_NEGLECTED:
        return 0u; // listless: frozen on the rest pose
    default:
        return frame & 3u; // full 4-frame sway
    }
}

uint32_t pet_mood_apply_exp_pressure(uint32_t raw_gain, const PetCare* c, uint64_t now) {
    if(c == NULL || c->last_feed_time == 0u) return raw_gain; // never fed: no penalty
    uint64_t e = elapsed_since_feed(c, now);
    if(e < PET_NEGLECT_SECS) return raw_gain;
    return (uint32_t)((uint64_t)raw_gain * PET_NEGLECT_EXP_NUM / PET_NEGLECT_EXP_DEN);
}
