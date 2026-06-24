// Radiotchi - host unit tests for the pet growth layer (pure, game-side).
//
// Covers the new 5-stat / exp-level / 100-type morph system (docs/pet-growth-spec.md,
// decision-log D18): exp favors delicacies and decays on repeats, the level curve is
// monotonic, the 100-type space is a clean bijection, type resolution is deterministic
// (lowest-index tie-break), the morph is reversible across level-5 checkpoints, and a
// young pet stays Unformed until its first checkpoint.
//
// Build & run: make -C test  (needs a host C compiler; libm-free).

#include "pet_growth.h"

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

// exp favors delicacies over junk and decays as a species repeats.
static void test_exp(void) {
    printf("exp gain:\n");
    CaptureEvent delicacy = ev_with(0.5f, 0.9f, 0.0f, 1.0f, 1.0f); // legible, rare, decoded
    CaptureEvent junk = ev_with(0.5f, 0.5f, 1.0f, 0.0f, 0.0f); // high entropy, common

    uint32_t e_del = pet_growth_exp_gain(&delicacy, 0u);
    uint32_t e_junk = pet_growth_exp_gain(&junk, 0u);
    CHECK(e_del > e_junk, "a delicacy is worth more exp than junk");
    CHECK(e_junk < e_del && e_del > 0u, "both yield some exp");

    uint32_t e_fresh = pet_growth_exp_gain(&delicacy, 0u);
    uint32_t e_repeat = pet_growth_exp_gain(&delicacy, 100u);
    CHECK(e_repeat < e_fresh, "repeat-decay lowers exp for an oft-seen species");
}

// Level curve is monotonic with the documented K*L^2 boundaries.
static void test_level_curve(void) {
    printf("level curve:\n");
    CHECK(pet_level_for_exp(0u) == 0u, "0 exp -> level 0");
    // EXP_K = 50: level 1 needs 50, level 2 needs 200.
    CHECK(pet_level_for_exp(49u) == 0u, "just under level 1");
    CHECK(pet_level_for_exp(50u) == 1u, "exactly level 1");
    CHECK(pet_level_for_exp(199u) == 1u, "just under level 2");
    CHECK(pet_level_for_exp(200u) == 2u, "exactly level 2");
    CHECK(pet_level_for_exp(0xFFFFFFFFu) == PET_LEVEL_MAX, "huge exp clamps to max level");

    uint16_t prev = 0u;
    for(uint32_t e = 0; e < 6000u; e += 37u) {
        uint16_t l = pet_level_for_exp(e);
        CHECK(l >= prev, "level never decreases as exp grows");
        prev = l;
    }
}

// The 100 type ids decompose and recompose exactly; fields stay in range.
static void test_type_bijection(void) {
    printf("type bijection:\n");
    CHECK(PET_TYPE_COUNT == 100u, "exactly 100 types");
    int ok = 1;
    for(uint8_t id = 0; id < PET_TYPE_COUNT; id++) {
        uint8_t f = pet_type_family(id);
        uint8_t p = pet_type_partner(id);
        uint8_t s = pet_type_shape(id);
        if(f >= PET_FAMILY_COUNT || p >= PET_PARTNER_COUNT || s >= PET_SHAPE_COUNT) ok = 0;
        uint8_t recomposed = (uint8_t)(f * (PET_PARTNER_COUNT * PET_SHAPE_COUNT) + p * PET_SHAPE_COUNT + s);
        if(recomposed != id) ok = 0;
        // partner slot maps to a real, distinct-from-family stat.
        uint8_t pstat = pet_type_partner_stat(f, p);
        if(pstat == f || pstat >= PET_STAT_COUNT) ok = 0;
    }
    CHECK(ok, "every id 0..99 round-trips through family/partner/shape");
}

// Resolution is deterministic and hits each shape band as designed.
static void test_type_resolve(void) {
    printf("type resolve:\n");
    // Mass dominant, Vigor second, huge gap -> family Mass, partner=Vigor, Pure.
    float pure[PET_STAT_COUNT] = {0};
    pure[ST_MASS] = 1.0f;
    pure[ST_VIGOR] = 0.0f;
    uint8_t id = pet_type_resolve(pure);
    CHECK(pet_type_family(id) == ST_MASS, "max stat -> family");
    CHECK(pet_type_partner_stat(pet_type_family(id), pet_type_partner(id)) == ST_VIGOR,
          "2nd stat -> partner");
    CHECK(pet_type_shape(id) == SHAPE_PURE, "clean dominance -> Pure");

    // Top two nearly tied -> Sprout.
    float sprout[PET_STAT_COUNT] = {0};
    sprout[ST_MASS] = 1.0f;
    sprout[ST_VIGOR] = 0.98f;
    CHECK(pet_type_shape(pet_type_resolve(sprout)) == SHAPE_SPROUT,
          "near co-dominance -> Sprout");

    // Flat profile -> Diffuse.
    float flat[PET_STAT_COUNT] = {0.5f, 0.5f, 0.5f, 0.5f, 0.5f};
    CHECK(pet_type_shape(pet_type_resolve(flat)) == SHAPE_DIFFUSE, "flat -> Diffuse");

    // Single clear peak above the mean, mid gap to 2nd -> Crested.
    float crest[PET_STAT_COUNT] = {0};
    crest[ST_MASS] = 0.6f;
    crest[ST_VIGOR] = 0.45f;
    CHECK(pet_type_shape(pet_type_resolve(crest)) == SHAPE_CRESTED, "one peak, mid gap -> Crested");

    // Moderate spread, no runaway -> Woven.
    float woven[PET_STAT_COUNT] = {0.6f, 0.45f, 0.4f, 0.4f, 0.4f};
    CHECK(pet_type_shape(pet_type_resolve(woven)) == SHAPE_WOVEN, "moderate spread -> Woven");

    // Determinism + tie-break: all equal -> lowest index family, same every call.
    float tie[PET_STAT_COUNT] = {0.7f, 0.7f, 0.7f, 0.7f, 0.7f};
    uint8_t a = pet_type_resolve(tie);
    uint8_t b = pet_type_resolve(tie);
    CHECK(a == b, "resolution is deterministic");
    CHECK(pet_type_family(a) == ST_MASS, "all-equal ties break to the lowest stat index");
}

// Names must match the generated catalog (docs/sprite-prompts.md) exactly so the
// runtime label and the per-type asset never drift.
static void test_type_names(void) {
    printf("type names:\n");
    char buf[32];
    pet_type_name(0, buf, sizeof(buf));
    CHECK(strcmp(buf, "Mass-Vigor Pure") == 0, "id 0 -> Mass-Vigor Pure");
    pet_type_name(1, buf, sizeof(buf));
    CHECK(strcmp(buf, "Mass-Vigor Sprout") == 0, "id 1 -> Mass-Vigor Sprout");
    pet_type_name(40, buf, sizeof(buf));
    CHECK(strcmp(buf, "Wild-Mass Pure") == 0, "id 40 -> Wild-Mass Pure");
    pet_type_name(64, buf, sizeof(buf));
    CHECK(strcmp(buf, "Aura-Mass Diffuse") == 0, "id 64 -> Aura-Mass Diffuse");
    pet_type_name(79, buf, sizeof(buf));
    CHECK(strcmp(buf, "Aura-Mind Diffuse") == 0, "id 79 -> Aura-Mind Diffuse");
    pet_type_name(99, buf, sizeof(buf));
    CHECK(strcmp(buf, "Mind-Aura Diffuse") == 0, "id 99 -> Mind-Aura Diffuse");
}

// A fresh / young pet has no morph until it reaches its first level-5 checkpoint.
static void test_unformed(void) {
    printf("unformed:\n");
    PetGrowth g;
    pet_growth_init(&g);
    CHECK(g.type_id == PET_TYPE_UNFORMED, "fresh pet is Unformed");
    CHECK(g.level == 0u, "fresh pet is level 0");

    CaptureEvent small = ev_with(0.2f, 0.2f, 0.5f, 0.1f, 0.0f);
    for(int i = 0; i < 3; i++) pet_growth_feed(&g, &small, 0u);
    CHECK(g.level < PET_TYPE_PERIOD, "still below the first checkpoint");
    CHECK(g.type_id == PET_TYPE_UNFORMED, "no morph before the first checkpoint");

    char name[32];
    pet_type_name(PET_TYPE_UNFORMED, name, sizeof(name));
    CHECK(strcmp(name, "Unformed") == 0, "Unformed names cleanly");
}

// Life stage is a pure gate on level: egg (1..4), child (5..9), adult (10+). The
// morph is only revealed at the adult stage; the type still derives at every
// level-5 checkpoint so the child's lineage tint is already available at level 5.
static void test_life_stage(void) {
    printf("life stage:\n");
    CHECK(pet_life_stage(0u) == LIFE_EGG, "level 0 -> egg");
    CHECK(pet_life_stage(PET_LEVEL_CHILD - 1u) == LIFE_EGG, "level 4 -> egg");
    CHECK(pet_life_stage(PET_LEVEL_CHILD) == LIFE_CHILD, "level 5 -> child");
    CHECK(pet_life_stage(PET_LEVEL_ADULT - 1u) == LIFE_CHILD, "level 9 -> child");
    CHECK(pet_life_stage(PET_LEVEL_ADULT) == LIFE_ADULT, "level 10 -> adult");
    CHECK(pet_life_stage(PET_LEVEL_MAX) == LIFE_ADULT, "max level -> adult");
    // Stages line up with the type checkpoints: child begins at the first checkpoint.
    CHECK(PET_LEVEL_CHILD == PET_TYPE_PERIOD, "child starts at the first type checkpoint");
    CHECK(PET_LEVEL_ADULT % PET_TYPE_PERIOD == 0u, "adult begins on a checkpoint boundary");
}

// The morph is reversible: a WILD diet makes a Wild-type, then a clean/rare diet
// re-speciates it at a later checkpoint (D18 / product-spec reversibility).
static void test_reversible_morph(void) {
    printf("reversible morph:\n");
    PetGrowth g;
    pet_growth_init(&g);

    // High-entropy (Wild) diet; novel each time so exp keeps flowing.
    CaptureEvent wild = ev_with(0.3f, 0.3f, 1.0f, 0.2f, 0.0f);
    for(int i = 0; i < 400; i++) pet_growth_feed(&g, &wild, 0u);
    CHECK(g.level >= PET_TYPE_PERIOD, "reached the first checkpoint on a wild diet");
    CHECK(g.type_id != PET_TYPE_UNFORMED, "got a morph");
    CHECK(pet_type_family(g.type_id) == ST_WILD, "wild diet -> Wild family");

    uint8_t wild_type = g.type_id;

    // Switch to a clean, rare, well-decoded diet.
    CaptureEvent clean = ev_with(0.3f, 0.3f, 0.0f, 1.0f, 1.0f);
    for(int i = 0; i < 200; i++) pet_growth_feed(&g, &clean, 0u);
    CHECK(g.stat[ST_WILD] < g.stat[ST_AURA], "Wild stat decayed below Aura");
    CHECK(pet_type_family(g.type_id) != ST_WILD, "re-speciated away from Wild");
    CHECK(g.type_id != wild_type, "the morph actually changed");
}

int main(void) {
    printf("== Radiotchi pet_growth host tests ==\n");
    test_exp();
    test_level_curve();
    test_type_bijection();
    test_type_resolve();
    test_type_names();
    test_unformed();
    test_life_stage();
    test_reversible_morph();
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
