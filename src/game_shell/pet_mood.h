// Radiotchi — pet care/mood: hunger and mood from time-since-last-meal (PURE).
//
// A presentation + soft-pressure layer ON TOP of the diet-driven growth (pet_growth.c).
// It deliberately does NOT touch the reversible identity (stat[]/exp/level/type_id): mood
// only changes how the pet LOOKS and, at most, how fast a neglected pet gains exp. Like
// the rest of the host-testable core this is pure and libm-free — "now" is injected as
// data (the clock lives only in capture_store.c), never read from a global.
//
// Numeric constants are PROVISIONAL (calibration deferred, like the PET_* constants in
// pet_growth.h); the BEHAVIOUR (hunger rises with neglect, a fresh delicacy reads content)
// is the contract.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "radiotchi_types.h" // CaptureEvent

#ifdef __cplusplus
extern "C" {
#endif

// Persisted care state — just two scalars; everything else is derived each frame.
typedef struct {
    uint64_t last_feed_time; // epoch seconds of the last meal (0 = never fed)
    uint8_t last_meal_quality; // 0..100, centi of pet_growth_meal_quality at that meal
} PetCare;

// Visible mood, derived from (last_feed_time, last_meal_quality, now). Display-only.
typedef enum {
    MOOD_CONTENT = 0, // recently fed AND the last meal was a delicacy
    MOOD_HAPPY, //       recently fed (ordinary meal)
    MOOD_NEUTRAL, //     never fed yet (grace) — no hunger pressure
    MOOD_HUNGRY, //      overdue for a meal
    MOOD_NEGLECTED, //   long neglect (also slows exp)
} PetMood;

// --- calibration-deferred constants (PROVISIONAL) --------------------------
#define PET_HUNGER_FULL_SECS   21600u // < 6h since a meal: not hungry yet
#define PET_HUNGER_STARVE_SECS 172800u // 48h: hunger saturates at 1.0
#define PET_NEGLECT_SECS       86400u // 24h: the pet looks neglected and meals grant less exp
#define PET_DELICACY_QUALITY   70u // last_meal_quality >= this => the content look
#define PET_NEGLECT_EXP_NUM    1u
#define PET_NEGLECT_EXP_DEN    2u // a neglected meal grants NUM/DEN of its exp

// Fresh care state: never fed (grace), no remembered meal.
void pet_care_init(PetCare* c);

// Hunger in [0, 1]: 0 until FULL_SECS, then a linear ramp to 1.0 at STARVE_SECS. A
// never-fed pet (or a clock that ran backwards) reads 0. Integer-derived, libm-free.
float pet_hunger(const PetCare* c, uint64_t now);

// Visible mood for (care, now). Never-fed => NEUTRAL; otherwise CONTENT/HAPPY when fresh,
// HUNGRY past FULL_SECS, NEGLECTED past NEGLECT_SECS. Pure.
PetMood pet_mood(const PetCare* c, uint64_t now);

// Record a meal: stamp `now` and remember its delicacy quality (reuses the EXACT
// pet_growth_meal_quality weighting). Call AFTER reading any neglect-based exp pressure.
void pet_care_feed(PetCare* c, const CaptureEvent* ev, uint64_t now);

// Remap the idle animation frame by mood without new sprites: a hungry pet sways
// sluggishly (rest poses only), a neglected pet is listless (frozen on rest); others use
// the full 4-frame sway. Pure.
uint32_t pet_mood_anim_frame(PetMood mood, uint32_t frame);

// Soft exp pressure: a neglected pet's meal grants NUM/DEN of its exp; otherwise the raw
// gain is returned unchanged. Only ever LOWERS the gain (never below the integer ratio),
// never touches stats/level — neglect slows growth, it never reverses it. Pure.
uint32_t pet_mood_apply_exp_pressure(uint32_t raw_gain, const PetCare* c, uint64_t now);

#ifdef __cplusplus
}
#endif
