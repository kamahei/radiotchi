// Radiotchi — pet quests implementation (pure; host + Flipper).

#include "pet_quests.h"

#include "pet_growth.h" // pet_growth_meal_quality (the EXACT delicacy weighting)
#include "pet_mood.h" //   PET_NEGLECT_SECS, PET_DELICACY_QUALITY (one definition each)

void pet_quests_init(PetQuests* q) {
    if(q == NULL) return;
    q->total_feeds = 0;
    q->delicacy_feeds = 0;
    q->decoded_feeds = 0;
    q->distinct_species = 0;
    q->feed_streak = 0;
    q->best_streak = 0;
    q->last_feed_time = 0;
    q->unlocked_mask = 0;
}

const char* pet_quest_name(QuestId id) {
    switch(id) {
    case QUEST_FIRST_MEAL: return "First Meal";
    case QUEST_TEN_MEALS: return "Ten Meals";
    case QUEST_FIRST_DELICACY: return "Delicacy";
    case QUEST_FIVE_SPECIES: return "5 Species";
    case QUEST_FIRST_DECODE: return "Decoded!";
    case QUEST_STREAK_3: return "Streak 3";
    case QUEST_STREAK_7: return "Streak 7";
    default: return "?";
    }
}

bool pet_quest_unlocked(const PetQuests* q, QuestId id) {
    if(q == NULL || (unsigned)id >= (unsigned)QUEST_COUNT) return false;
    return (q->unlocked_mask & (1u << (unsigned)id)) != 0u;
}

uint32_t pet_quests_feed(
    PetQuests* q, const CaptureEvent* ev, uint32_t distinct_species, uint64_t now) {
    if(q == NULL || ev == NULL) return 0;

    // --- counters ---
    q->total_feeds++;
    uint8_t quality = (uint8_t)(pet_growth_meal_quality(ev) * 100.0f + 0.5f);
    if(quality >= PET_DELICACY_QUALITY) q->delicacy_feeds++;
    if(ev->decode_tier == TIER_VALUES) q->decoded_feeds++;
    if(distinct_species > q->distinct_species) q->distinct_species = distinct_species;

    // --- feed streak (on-time = within the neglect window of the previous meal) ---
    if(q->last_feed_time == 0u) {
        q->feed_streak = 1u; // first meal ever
    } else if(now < q->last_feed_time) {
        // clock ran backwards: leave the streak untouched (never invent or break it)
    } else if((now - q->last_feed_time) < PET_NEGLECT_SECS) {
        if(q->feed_streak < 0xFFFFu) q->feed_streak++; // on-time meal extends the streak
    } else {
        q->feed_streak = 1u; // a neglect gap broke the streak; this meal starts a new one
    }
    if(q->feed_streak > q->best_streak) q->best_streak = q->feed_streak;
    q->last_feed_time = now;

    // --- re-evaluate every threshold; return only the bits NEWLY set this meal ---
    uint32_t before = q->unlocked_mask;
    if(q->total_feeds >= 1u) q->unlocked_mask |= (1u << QUEST_FIRST_MEAL);
    if(q->total_feeds >= QUEST_TEN_MEALS_N) q->unlocked_mask |= (1u << QUEST_TEN_MEALS);
    if(q->delicacy_feeds >= 1u) q->unlocked_mask |= (1u << QUEST_FIRST_DELICACY);
    if(q->distinct_species >= QUEST_FIVE_SPECIES_N) q->unlocked_mask |= (1u << QUEST_FIVE_SPECIES);
    if(q->decoded_feeds >= 1u) q->unlocked_mask |= (1u << QUEST_FIRST_DECODE);
    if(q->best_streak >= QUEST_STREAK_3_N) q->unlocked_mask |= (1u << QUEST_STREAK_3);
    if(q->best_streak >= QUEST_STREAK_7_N) q->unlocked_mask |= (1u << QUEST_STREAK_7);

    return q->unlocked_mask & ~before;
}
