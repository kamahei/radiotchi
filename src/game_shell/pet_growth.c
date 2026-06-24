// Radiotchi - pet growth: 5 stats, exp/level, 100-type morph (pure, libm-free).

#include "pet_growth.h"

static float clampf(float v, float lo, float hi) {
    if(v < lo) return lo;
    if(v > hi) return hi;
    return v;
}

// Round a non-negative float to the nearest uint (avoids libm roundf).
static uint32_t round_u32(float v) {
    if(v <= 0.0f) return 0u;
    return (uint32_t)(v + 0.5f);
}

void pet_growth_init(PetGrowth* g) {
    if(g == NULL) return;
    for(int i = 0; i < PET_STAT_COUNT; i++) g->stat[i] = 0.0f;
    g->total_exp = 0u;
    g->level = 0u;
    g->type_id = PET_TYPE_UNFORMED;
}

// Pull the 5 axes off a CaptureEvent in stat order (ST_*).
static void scores_to_axes(const CaptureEvent* ev, float out[PET_STAT_COUNT]) {
    out[ST_MASS] = ev->scores.calories;
    out[ST_VIGOR] = ev->scores.freshness;
    out[ST_WILD] = ev->scores.additives;
    out[ST_AURA] = ev->scores.rarity;
    out[ST_MIND] = ev->scores.nourishment;
}

uint32_t pet_growth_exp_gain(const CaptureEvent* ev, uint32_t seen_count) {
    if(ev == NULL) return 0u;
    const Scores* s = &ev->scores;

    // Delicacy-favoring quality: low entropy (legible), high rarity + structure
    // are worth the most; junk yields little (D1). Weights sum to 1.
    float quality = 0.15f * s->calories + 0.15f * s->freshness +
                    0.20f * (1.0f - s->additives) + 0.25f * s->rarity +
                    0.25f * s->nourishment;
    quality = clampf(quality, 0.0f, 1.0f);

    // Repeat-decay: the Nth identical catch feeds less, on top of personal Rarity
    // already discounting it (anti-grind, D2). 1.0 when first-seen.
    float decay = PET_EXP_DECAY_HALF / (PET_EXP_DECAY_HALF + (float)seen_count);

    return round_u32(PET_EXP_BASE * quality * decay);
}

PetLifeStage pet_life_stage(uint16_t level) {
    if(level < PET_LEVEL_CHILD) return LIFE_EGG;
    if(level < PET_LEVEL_ADULT) return LIFE_CHILD;
    return LIFE_ADULT;
}

uint16_t pet_level_for_exp(uint32_t total_exp) {
    // Largest L (<= PET_LEVEL_MAX) with EXP_K * L^2 <= total_exp.
    uint16_t level = 0u;
    while(level < PET_LEVEL_MAX) {
        uint32_t next = level + 1u;
        uint32_t need = (uint32_t)PET_EXP_K * next * next;
        if(total_exp < need) break;
        level = (uint16_t)next;
    }
    return level;
}

void pet_growth_feed(PetGrowth* g, const CaptureEvent* ev, uint32_t seen_count) {
    if(g == NULL || ev == NULL) return;

    float axis[PET_STAT_COUNT];
    scores_to_axes(ev, axis);

    // Novel/rare catches pull the stat EMA harder so the ambient junk floor cannot
    // pin the profile; the EMA keeps every stat reversible (mirrors pet_state.c).
    bool was_novel = (seen_count == 0u);
    float alpha = (was_novel ? PET_STAT_ALPHA_NOVEL : PET_STAT_ALPHA_BASE);
    alpha += PET_STAT_RARE_BOOST * ev->scores.rarity;
    if(alpha > PET_STAT_ALPHA_MAX) alpha = PET_STAT_ALPHA_MAX;

    for(int i = 0; i < PET_STAT_COUNT; i++) {
        g->stat[i] = clampf((1.0f - alpha) * g->stat[i] + alpha * axis[i], 0.0f, 1.0f);
    }

    // exp is monotonic; level is a pure function of it.
    uint16_t old_checkpoint = (uint16_t)(g->level / PET_TYPE_PERIOD);
    uint32_t gain = pet_growth_exp_gain(ev, seen_count);
    // Saturating add (total_exp is a lifelong accumulator).
    if(g->total_exp > 0xFFFFFFFFu - gain) {
        g->total_exp = 0xFFFFFFFFu;
    } else {
        g->total_exp += gain;
    }
    g->level = pet_level_for_exp(g->total_exp);

    // Re-derive the morph only when a new level-5 checkpoint is crossed, so the
    // type is stable between checkpoints but reversible across them (D18).
    uint16_t new_checkpoint = (uint16_t)(g->level / PET_TYPE_PERIOD);
    if(g->level >= PET_TYPE_PERIOD && new_checkpoint != old_checkpoint) {
        g->type_id = pet_type_resolve(g->stat);
    }
}

// argmax over the stats, considering only indices with mask bit set. Ties go to
// the lowest index (strict >), making the result deterministic.
static int argmax_masked(const float stat[PET_STAT_COUNT], unsigned mask) {
    int best = -1;
    for(int i = 0; i < PET_STAT_COUNT; i++) {
        if(!(mask & (1u << i))) continue;
        if(best < 0 || stat[i] > stat[best]) best = i;
    }
    return best;
}

uint8_t pet_type_resolve(const float stat[PET_STAT_COUNT]) {
    if(stat == NULL) return 0u;

    unsigned all = (1u << PET_STAT_COUNT) - 1u;
    int family = argmax_masked(stat, all);
    int second = argmax_masked(stat, all & ~(1u << family));

    // Partner slot: position of `second` among the 4 non-family stats, in
    // ascending stat-index order (0..3).
    uint8_t partner = 0u;
    for(int i = 0; i < second; i++) {
        if(i != family) partner++;
    }

    float maxv = stat[family];
    float secondv = stat[second];
    float sum = 0.0f;
    for(int i = 0; i < PET_STAT_COUNT; i++) sum += stat[i];
    float mean = sum / (float)PET_STAT_COUNT;

    float gap2 = maxv - secondv; // purity: large => specialist
    float gapM = maxv - mean; // dominance: large => one runaway peak

    PetShape shape;
    if(gapM < PET_SHAPE_M_LO) {
        shape = SHAPE_DIFFUSE; // flat profile dominates the classification
    } else if(gap2 >= PET_SHAPE_T_HI) {
        shape = SHAPE_PURE;
    } else if(gap2 < PET_SHAPE_T_LO) {
        shape = SHAPE_SPROUT;
    } else if(gapM >= PET_SHAPE_M_HI) {
        shape = SHAPE_CRESTED;
    } else {
        shape = SHAPE_WOVEN;
    }

    return (uint8_t)(family * (PET_PARTNER_COUNT * PET_SHAPE_COUNT) + partner * PET_SHAPE_COUNT + shape);
}

uint8_t pet_type_family(uint8_t type_id) {
    return (uint8_t)(type_id / (PET_PARTNER_COUNT * PET_SHAPE_COUNT));
}

uint8_t pet_type_partner(uint8_t type_id) {
    return (uint8_t)((type_id / PET_SHAPE_COUNT) % PET_PARTNER_COUNT);
}

uint8_t pet_type_shape(uint8_t type_id) {
    return (uint8_t)(type_id % PET_SHAPE_COUNT);
}

uint8_t pet_type_partner_stat(uint8_t family, uint8_t partner) {
    // The partner-th stat index (ascending) that is not `family`.
    uint8_t slot = 0u;
    for(uint8_t i = 0; i < PET_STAT_COUNT; i++) {
        if(i == family) continue;
        if(slot == partner) return i;
        slot++;
    }
    return family; // unreachable for valid inputs
}

static const char* const STAT_NAME[PET_STAT_COUNT] = {
    "Mass", "Vigor", "Wild", "Aura", "Mind"};
static const char* const SHAPE_NAME[PET_SHAPE_COUNT] = {
    "Pure", "Sprout", "Crested", "Woven", "Diffuse"};

// Minimal NUL-terminated copy (avoids depending on strlcpy availability on host).
static void copy_str(char* buf, size_t n, const char* src) {
    if(n == 0) return;
    size_t i = 0;
    for(; src[i] != '\0' && i + 1 < n; i++) buf[i] = src[i];
    buf[i] = '\0';
}

void pet_type_name(uint8_t type_id, char* buf, size_t n) {
    if(buf == NULL || n == 0) return;
    if(type_id == PET_TYPE_UNFORMED || type_id >= PET_TYPE_COUNT) {
        copy_str(buf, n, "Unformed");
        return;
    }
    uint8_t family = pet_type_family(type_id);
    uint8_t partner_stat = pet_type_partner_stat(family, pet_type_partner(type_id));
    uint8_t shape = pet_type_shape(type_id);

    // "Family-Partner Shape", e.g. "Wild-Aura Pure".
    const char* a = STAT_NAME[family];
    const char* b = STAT_NAME[partner_stat];
    const char* c = SHAPE_NAME[shape];
    size_t i = 0;
    for(size_t k = 0; a[k] && i + 1 < n; k++) buf[i++] = a[k];
    if(i + 1 < n) buf[i++] = '-';
    for(size_t k = 0; b[k] && i + 1 < n; k++) buf[i++] = b[k];
    if(i + 1 < n) buf[i++] = ' ';
    for(size_t k = 0; c[k] && i + 1 < n; k++) buf[i++] = c[k];
    buf[i] = '\0';
}
