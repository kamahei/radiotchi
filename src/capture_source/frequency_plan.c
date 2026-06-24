// Radiotchi — Frequency plan & hopping implementation.

#include "frequency_plan.h"

#include <string.h>

// CC1101-supported bands (Hz). Mirrors what OFW Sub-GHz allows for RX. We do our
// own range check so the plan can be validated off-device without furi_hal.
typedef struct {
    uint32_t low;
    uint32_t high;
} Band;

static const Band kCc1101Bands[] = {
    {300000000u, 348000000u},
    {387000000u, 464000000u},
    {779000000u, 928000000u},
};

bool frequency_plan_is_valid(uint32_t frequency_hz) {
    for(size_t i = 0; i < sizeof(kCc1101Bands) / sizeof(kCc1101Bands[0]); i++) {
        if(frequency_hz >= kCc1101Bands[i].low && frequency_hz <= kCc1101Bands[i].high) {
            return true;
        }
    }
    return false;
}

void frequency_plan_init(FrequencyPlan* plan) {
    if(plan == NULL) return;
    memset(plan, 0, sizeof(*plan));
}

bool frequency_plan_add(FrequencyPlan* plan, uint32_t frequency_hz, const char* label) {
    if(plan == NULL) return false;
    if(plan->count >= FREQUENCY_PLAN_MAX) return false;
    if(!frequency_plan_is_valid(frequency_hz)) return false;

    plan->entries[plan->count].frequency_hz = frequency_hz;
    plan->entries[plan->count].label = (label != NULL) ? label : "";
    plan->count++;
    return true;
}

uint32_t frequency_plan_add_range(
    FrequencyPlan* plan,
    uint32_t start_hz,
    uint32_t end_hz,
    uint32_t step_hz) {
    if(plan == NULL || step_hz == 0 || end_hz < start_hz) return 0;

    uint32_t added = 0;
    for(uint32_t f = start_hz; f <= end_hz; f += step_hz) {
        if(frequency_plan_add(plan, f, "sweep")) added++;
        if(plan->count >= FREQUENCY_PLAN_MAX) break;
        // Guard against wrap-around on the increment for very large ranges.
        if(f + step_hz < f) break;
    }
    return added;
}

void frequency_plan_load_japan_defaults(FrequencyPlan* plan) {
    if(plan == NULL) return;
    frequency_plan_init(plan);
    // Japan band awareness (docs/architecture.md §4) + global ISM anchors.
    frequency_plan_add(plan, 315000000u, "TPMS/keyless 315");
    frequency_plan_add(plan, 426000000u, "特小 426");
    frequency_plan_add(plan, 429000000u, "特小 429");
    frequency_plan_add(plan, 433920000u, "ISM 433.92");
    frequency_plan_add(plan, 434420000u, "ISM 434.42");
    frequency_plan_add(plan, 868350000u, "SRD 868");
    frequency_plan_add(plan, 920000000u, "Wi-SUN 920");
    frequency_plan_add(plan, 922000000u, "IoT 922");
}

uint32_t frequency_plan_count(const FrequencyPlan* plan) {
    return (plan != NULL) ? plan->count : 0;
}

const FrequencyEntry* frequency_plan_get(const FrequencyPlan* plan, uint32_t index) {
    if(plan == NULL || index >= plan->count) return NULL;
    return &plan->entries[index];
}

const FrequencyEntry* frequency_plan_hop_next(FrequencyPlan* plan) {
    if(plan == NULL || plan->count == 0) return NULL;
    const FrequencyEntry* e = &plan->entries[plan->cursor];
    plan->cursor = (plan->cursor + 1) % plan->count;
    return e;
}

void frequency_plan_hop_reset(FrequencyPlan* plan) {
    if(plan != NULL) plan->cursor = 0;
}
