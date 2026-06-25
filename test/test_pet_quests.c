// Radiotchi — host unit tests for the pet quests layer (pure, game-side).
//
// Achievements + a feed streak over the meal stream: counters latch achievements, a streak
// counts consecutive on-time meals (within the neglect window). "Now" and the dex breadth are
// injected as data (clock-free), so these are deterministic off-device.
//
// Build & run: make -C test  (needs a host C compiler; libm-free).

#include "pet_quests.h"
#include "pet_mood.h" // PET_NEGLECT_SECS (the streak window)

#include <stdio.h>
#include <string.h>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, msg)                                              \
    do {                                                              \
        g_checks++;                                                   \
        if(!(cond)) {                                                 \
            g_failures++;                                             \
            printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
        }                                                             \
    } while(0)

// A fixed reference epoch — the tests only ever use deltas from it.
#define BASE 1000000000ull

static CaptureEvent meal(float cal, float fre, float add, float rar, float nou, DecodeTier tier) {
    CaptureEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.scores.calories = cal;
    ev.scores.freshness = fre;
    ev.scores.additives = add;
    ev.scores.rarity = rar;
    ev.scores.nourishment = nou;
    ev.decode_tier = tier;
    return ev;
}

// Quality ~0 (high entropy, common, undecoded) — the junk floor.
static CaptureEvent junk(void) {
    return meal(0.0f, 0.0f, 1.0f, 0.0f, 0.0f, TIER_RAW);
}
// Quality ~1.0 (legible, rare, fully decoded) — a delicacy, decoded to VALUES.
static CaptureEvent delicacy(void) {
    return meal(1.0f, 1.0f, 0.0f, 1.0f, 1.0f, TIER_VALUES);
}

static bool bit(uint32_t mask, QuestId id) {
    return (mask & (1u << (unsigned)id)) != 0u;
}

static void test_init(void) {
    printf("quests init:\n");
    PetQuests q;
    pet_quests_init(&q);
    CHECK(q.total_feeds == 0u && q.unlocked_mask == 0u, "fresh quests: nothing fed/unlocked");
    bool any = false;
    for(int i = 0; i < QUEST_COUNT; i++)
        if(pet_quest_unlocked(&q, (QuestId)i)) any = true;
    CHECK(!any, "nothing is unlocked on a fresh state");
}

static void test_first_and_idempotent(void) {
    printf("first meal + idempotency:\n");
    PetQuests q;
    pet_quests_init(&q);
    CaptureEvent j = junk();
    uint32_t n1 = pet_quests_feed(&q, &j, 0, BASE);
    CHECK(bit(n1, QUEST_FIRST_MEAL), "the first meal unlocks First Meal");
    CHECK(pet_quest_unlocked(&q, QUEST_FIRST_MEAL), "First Meal is latched");
    uint32_t n2 = pet_quests_feed(&q, &j, 0, BASE + 3600u);
    CHECK(!bit(n2, QUEST_FIRST_MEAL), "a later meal does NOT re-report First Meal (mask idempotent)");
}

static void test_delicacy_and_decode(void) {
    printf("delicacy + decode:\n");
    PetQuests q;
    pet_quests_init(&q);
    CaptureEvent j = junk();
    uint32_t n = pet_quests_feed(&q, &j, 0, BASE);
    CHECK(!bit(n, QUEST_FIRST_DELICACY), "junk is not a delicacy");
    CHECK(!bit(n, QUEST_FIRST_DECODE), "RAW junk is not decoded");
    CaptureEvent d = delicacy();
    uint32_t m = pet_quests_feed(&q, &d, 0, BASE + 3600u);
    CHECK(bit(m, QUEST_FIRST_DELICACY), "a high-quality meal unlocks Delicacy");
    CHECK(bit(m, QUEST_FIRST_DECODE), "a VALUES meal unlocks Decoded");
}

static void test_species_breadth(void) {
    printf("species breadth:\n");
    PetQuests q;
    pet_quests_init(&q);
    CaptureEvent j = junk();
    uint32_t n = pet_quests_feed(&q, &j, 4, BASE);
    CHECK(!bit(n, QUEST_FIVE_SPECIES), "4 distinct species: not yet");
    uint32_t m = pet_quests_feed(&q, &j, 5, BASE + 3600u);
    CHECK(bit(m, QUEST_FIVE_SPECIES), "5 distinct species unlocks the breadth quest");
    // best breadth is monotonic: a later lower snapshot keeps it unlocked.
    pet_quests_feed(&q, &j, 1, BASE + 7200u);
    CHECK(pet_quest_unlocked(&q, QUEST_FIVE_SPECIES), "breadth quest stays unlocked");
}

static void test_streak(void) {
    printf("feed streak:\n");
    PetQuests q;
    pet_quests_init(&q);
    CaptureEvent j = junk();
    uint64_t t = BASE;
    pet_quests_feed(&q, &j, 0, t);
    CHECK(q.feed_streak == 1u, "meal 1 => streak 1");
    t += PET_NEGLECT_SECS - 1u;
    pet_quests_feed(&q, &j, 0, t);
    CHECK(q.feed_streak == 2u, "on-time meal => streak 2");
    t += PET_NEGLECT_SECS - 1u;
    uint32_t m3 = pet_quests_feed(&q, &j, 0, t);
    CHECK(q.feed_streak == 3u, "on-time meal => streak 3");
    CHECK(bit(m3, QUEST_STREAK_3), "streak 3 unlocks Streak 3");

    // A long gap breaks the streak back to 1.
    t += PET_NEGLECT_SECS + 1u;
    pet_quests_feed(&q, &j, 0, t);
    CHECK(q.feed_streak == 1u, "a neglect gap resets the streak to 1");
    CHECK(q.best_streak == 3u, "best_streak remembers the peak");
    CHECK(pet_quest_unlocked(&q, QUEST_STREAK_3), "Streak 3 stays unlocked after a reset");

    // A backwards clock must not corrupt the streak.
    uint16_t before = q.feed_streak;
    pet_quests_feed(&q, &j, 0, t - 10000u);
    CHECK(q.feed_streak == before, "a backwards clock leaves the streak unchanged");
}

static void test_streak7(void) {
    printf("streak 7:\n");
    PetQuests q;
    pet_quests_init(&q);
    CaptureEvent j = junk();
    uint64_t t = BASE;
    uint32_t newly = 0;
    for(int i = 0; i < 7; i++) {
        newly = pet_quests_feed(&q, &j, 0, t);
        t += PET_NEGLECT_SECS - 1u;
    }
    CHECK(q.feed_streak == 7u, "7 on-time meals => streak 7");
    CHECK(bit(newly, QUEST_STREAK_7), "the 7th on-time meal unlocks Streak 7");
    CHECK(!bit(newly, QUEST_STREAK_3), "Streak 3 was already unlocked earlier (not re-reported)");
}

int main(void) {
    printf("== Radiotchi pet_quests host tests ==\n");
    test_init();
    test_first_and_idempotent();
    test_delicacy_and_decode();
    test_species_breadth();
    test_streak();
    test_streak7();
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
