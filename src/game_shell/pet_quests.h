// Radiotchi — pet quests: lifetime achievements + feed streak (PURE).
//
// A collection-reinforcing layer over the meal stream (D2: collection is the pillar, not
// power). Every meal bumps a few small counters and a feed streak (consecutive on-time meals);
// thresholds latch stable achievements. Like the rest of the host-testable core this is pure
// and libm-free — "now" and the dex breadth are injected as data, never read from a global.
//
// Numeric thresholds are PROVISIONAL (calibration deferred, like the PET_* / care constants);
// the BEHAVIOUR (a streak counts on-time meals, an unlocked achievement latches) is the contract.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "radiotchi_types.h" // CaptureEvent, DecodeTier

#ifdef __cplusplus
extern "C" {
#endif

// Stable achievement ids => bit positions in `unlocked_mask`. APPEND-ONLY: the mask is
// persisted, so never renumber or remove an id (keep < 32).
typedef enum {
    QUEST_FIRST_MEAL = 0, // first catch eaten
    QUEST_TEN_MEALS, //      10 catches eaten
    QUEST_FIRST_DELICACY, // a high-quality (legible/rare/decoded) meal
    QUEST_FIVE_SPECIES, //   5 distinct species in the dex
    QUEST_FIRST_DECODE, //   a meal decoded all the way to VALUES
    QUEST_STREAK_3, //       3 on-time meals in a row
    QUEST_STREAK_7, //       7 on-time meals in a row
    QUEST_COUNT, //          keep last
} QuestId;

// --- calibration-deferred thresholds (PROVISIONAL) -------------------------
#define QUEST_TEN_MEALS_N    10u
#define QUEST_FIVE_SPECIES_N 5u
#define QUEST_STREAK_3_N     3u
#define QUEST_STREAK_7_N     7u

// Persisted progress — small fixed counters; everything else is derived.
typedef struct {
    uint32_t total_feeds; //     lifetime meals eaten
    uint32_t delicacy_feeds; //  meals with quality >= the delicacy bar
    uint32_t decoded_feeds; //   meals that reached TIER_VALUES
    uint32_t distinct_species; // best dex breadth seen (snapshot at a meal)
    uint16_t feed_streak; //     consecutive on-time meals (no neglect gap)
    uint16_t best_streak; //     longest streak ever
    uint64_t last_feed_time; //  for streak continuity (quest-owned; epoch seconds, 0 = none)
    uint32_t unlocked_mask; //   bit per achievement already earned
} PetQuests;

// Fresh quest state: nothing fed, nothing unlocked.
void pet_quests_init(PetQuests* q);

// Short display name for an achievement (for the Achievements screen). Pure, no allocation.
const char* pet_quest_name(QuestId id);

// Is this achievement earned?
bool pet_quest_unlocked(const PetQuests* q, QuestId id);

// Register a meal: bump the counters, update the streak (using last_feed_time vs now and the
// PET_NEGLECT_SECS gate), then re-evaluate every threshold. `distinct_species` is the dex
// breadth at this meal (injected — the module never reads the dex). Returns a bitmask of the
// achievements NEWLY unlocked by THIS meal (0 = none), so the caller can fire a cue/banner.
// A backwards clock never corrupts the streak. Pure.
uint32_t pet_quests_feed(
    PetQuests* q, const CaptureEvent* ev, uint32_t distinct_species, uint64_t now);

#ifdef __cplusplus
}
#endif
