// Radiotchi — host unit tests for the pet care/mood layer (pure, game-side).
//
// Mood is a presentation + soft-pressure layer over the diet-driven growth: hunger rises
// with time since the last meal, a fresh delicacy reads content, long neglect only ever
// SLOWS exp. "Now" is injected as data (clock-free), so these are deterministic off-device.
//
// Build & run: make -C test  (needs a host C compiler; libm-free).

#include "pet_mood.h"

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

static CaptureEvent ev_with(float cal, float fre, float add, float rar, float nou) {
    CaptureEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.scores.calories = cal;
    ev.scores.freshness = fre;
    ev.scores.additives = add;
    ev.scores.rarity = rar;
    ev.scores.nourishment = nou;
    return ev;
}

static void test_init(void) {
    printf("care init:\n");
    PetCare c;
    pet_care_init(&c);
    CHECK(c.last_feed_time == 0u, "fresh care: never fed");
    CHECK(c.last_meal_quality == 0u, "fresh care: no remembered meal");
    CHECK(pet_mood(&c, BASE) == MOOD_NEUTRAL, "never-fed pet reads NEUTRAL");
    CHECK(pet_hunger(&c, BASE) == 0.0f, "never-fed pet has no hunger");
}

static void test_hunger(void) {
    printf("hunger:\n");
    PetCare c;
    pet_care_init(&c);
    c.last_feed_time = BASE;

    CHECK(pet_hunger(&c, BASE) == 0.0f, "hunger 0 at the moment of feeding");
    CHECK(pet_hunger(&c, BASE + PET_HUNGER_FULL_SECS) == 0.0f, "no hunger before FULL_SECS");
    CHECK(pet_hunger(&c, BASE + PET_HUNGER_FULL_SECS + 1u) > 0.0f, "hunger rises past FULL_SECS");
    CHECK(pet_hunger(&c, BASE + PET_HUNGER_STARVE_SECS) == 1.0f, "hunger saturates at STARVE_SECS");
    CHECK(pet_hunger(&c, BASE + 2u * PET_HUNGER_STARVE_SECS) == 1.0f, "hunger clamps at 1.0");

    // Monotonic ramp between FULL and STARVE.
    float prev = 0.0f;
    for(uint64_t e = PET_HUNGER_FULL_SECS; e <= PET_HUNGER_STARVE_SECS; e += 3600u) {
        float h = pet_hunger(&c, BASE + e);
        CHECK(h >= prev, "hunger never decreases as time passes");
        prev = h;
    }

    // A clock that ran backwards must not invent hunger.
    CHECK(pet_hunger(&c, BASE - 100u) == 0.0f, "backwards clock => no phantom hunger");
}

static void test_mood(void) {
    printf("mood:\n");
    PetCare c;
    pet_care_init(&c);

    // Fresh delicacy => content; a fresh ordinary meal => happy.
    CaptureEvent delicacy = ev_with(0.5f, 0.9f, 0.0f, 1.0f, 1.0f);
    pet_care_feed(&c, &delicacy, BASE);
    CHECK(c.last_feed_time == BASE, "feeding stamps the time");
    CHECK(c.last_meal_quality >= PET_DELICACY_QUALITY, "a delicacy is remembered as high quality");
    CHECK(pet_mood(&c, BASE) == MOOD_CONTENT, "fresh delicacy => CONTENT");

    CaptureEvent junk = ev_with(0.5f, 0.5f, 1.0f, 0.0f, 0.0f);
    pet_care_feed(&c, &junk, BASE);
    CHECK(c.last_meal_quality < PET_DELICACY_QUALITY, "junk is remembered as low quality");
    CHECK(pet_mood(&c, BASE) == MOOD_HAPPY, "fresh ordinary meal => HAPPY");

    // Time passing: HUNGRY past FULL, NEGLECTED past NEGLECT.
    CHECK(pet_mood(&c, BASE + PET_HUNGER_FULL_SECS) == MOOD_HUNGRY, "past FULL_SECS => HUNGRY");
    CHECK(pet_mood(&c, BASE + PET_NEGLECT_SECS) == MOOD_NEGLECTED, "past NEGLECT_SECS => NEGLECTED");
}

static void test_exp_pressure(void) {
    printf("exp pressure:\n");
    PetCare c;
    pet_care_init(&c);

    // Never fed: no penalty.
    CHECK(pet_mood_apply_exp_pressure(64u, &c, BASE) == 64u, "never-fed meal keeps full exp");

    c.last_feed_time = BASE;
    CHECK(pet_mood_apply_exp_pressure(64u, &c, BASE) == 64u, "a fresh pet keeps full exp");
    CHECK(
        pet_mood_apply_exp_pressure(64u, &c, BASE + PET_NEGLECT_SECS) == 32u,
        "a neglected meal grants halved exp");
    CHECK(
        pet_mood_apply_exp_pressure(64u, &c, BASE + PET_NEGLECT_SECS) <= 64u,
        "pressure never raises exp");
    CHECK(pet_mood_apply_exp_pressure(0u, &c, BASE + PET_NEGLECT_SECS) == 0u, "zero stays zero");
}

static void test_anim_frame(void) {
    printf("anim frame remap:\n");
    // Content/happy/neutral keep the full 4-frame sway.
    for(uint32_t f = 0; f < 8; f++) {
        CHECK(pet_mood_anim_frame(MOOD_CONTENT, f) == (f & 3u), "content => full sway");
    }
    // Hungry uses only the two rest poses (0 and 2).
    CHECK(pet_mood_anim_frame(MOOD_HUNGRY, 0) == 0u, "hungry frame 0 => rest");
    CHECK(pet_mood_anim_frame(MOOD_HUNGRY, 1) == 2u, "hungry frame 1 => the other rest");
    CHECK(pet_mood_anim_frame(MOOD_HUNGRY, 2) == 0u, "hungry frame 2 => rest");
    // Neglected is frozen.
    CHECK(pet_mood_anim_frame(MOOD_NEGLECTED, 3) == 0u, "neglected => frozen on rest");
}

int main(void) {
    printf("== Radiotchi pet_mood host tests ==\n");
    test_init();
    test_hunger();
    test_mood();
    test_exp_pressure();
    test_anim_frame();
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
