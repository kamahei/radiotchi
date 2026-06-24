// Radiotchi — Frequency plan & hopping.
//
// The capture stage is deliberately NOT fixed to 433.92 MHz. A FrequencyPlan
// describes which Sub-GHz frequencies to search and how to hop across them while
// looking for the strongest signal. Two construction modes:
//
//   - PRESET LIST: a curated set of Japan-relevant bands (and the global ISM
//     anchors), each a discrete frequency to dwell on.
//   - RANGE HOP:   a [start, end] sweep with a fixed step, for exploring a band
//     the presets do not cover.
//
// Only CC1101-valid frequencies are kept (300-348 / 387-464 / 779-928 MHz). The
// plan is plain data with no hardware dependency, so it is cheap to test and to
// reconfigure from the UI later.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FREQUENCY_PLAN_MAX 48u

// A labelled frequency the searcher can dwell on.
typedef struct {
    uint32_t frequency_hz;
    const char* label; // short human tag, e.g. "TPMS 315" (may be "")
} FrequencyEntry;

typedef struct {
    FrequencyEntry entries[FREQUENCY_PLAN_MAX];
    uint32_t count;
    uint32_t cursor; // round-robin hop position
} FrequencyPlan;

// Initialize an empty plan.
void frequency_plan_init(FrequencyPlan* plan);

// Append one frequency. Returns false if invalid (out of CC1101 bands) or full.
bool frequency_plan_add(FrequencyPlan* plan, uint32_t frequency_hz, const char* label);

// Append a [start_hz, end_hz] sweep with `step_hz` spacing. Invalid steps are
// skipped; returns the number actually added.
uint32_t frequency_plan_add_range(
    FrequencyPlan* plan,
    uint32_t start_hz,
    uint32_t end_hz,
    uint32_t step_hz);

// Load the default Japan-relevant preset set (315 TPMS/keyless, 426-430 特小,
// 433.92 ISM, 868 SRD, 920 Wi-SUN/IoT). This is the search plan used when the
// user has not customized one.
void frequency_plan_load_japan_defaults(FrequencyPlan* plan);

// True if a frequency is within a CC1101-supported band.
bool frequency_plan_is_valid(uint32_t frequency_hz);

// Number of frequencies in the plan.
uint32_t frequency_plan_count(const FrequencyPlan* plan);

// Read entry `index` (NULL if out of range).
const FrequencyEntry* frequency_plan_get(const FrequencyPlan* plan, uint32_t index);

// Advance the hop cursor and return the next frequency to dwell on. Wraps around.
// Returns NULL only if the plan is empty.
const FrequencyEntry* frequency_plan_hop_next(FrequencyPlan* plan);

// Reset the hop cursor to the start of the plan.
void frequency_plan_hop_reset(FrequencyPlan* plan);

#ifdef __cplusplus
}
#endif
