// Radiotchi - pet growth: 5 stats, exp/level, and the 100-type morph (PURE).
//
// The pet layer over the diet stream (supersedes the retired Health x Class quadrant,
// D18). Each of the 5 nutrition axes feeds one pet stat as a long-memory EMA, so the
// stat vector stays a reversible reflection of diet (D7).
// Every meal also grants monotonic `exp` (a maturity clock, NOT power - D2); the
// level-exp curve maps cumulative exp -> level.
//
// At each level-5 checkpoint the pet's *character type* (one of exactly 100) is
// re-derived from the CURRENT stat vector, so the type is reversible: change the
// diet and the next checkpoint re-speciates the morph (supersedes the 5-form
// quadrant taxonomy; see docs/pet-growth-spec.md and decision-log D18).
//
//   type_id = family*20 + partner*5 + shape           (0..99)
//     family  (5) = which stat is max
//     partner (4) = which of the OTHER stats is 2nd
//     shape   (5) = silhouette from the max->2nd and max->mean gaps
//
// Numeric constants are PROVISIONAL (calibration deferred, open-questions). The
// curve uses an integer-friendly power of 2 (K*L^2) so this stays libm-free, like
// the rest of the host-testable core. Persistence will live in capture_store.c.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "radiotchi_types.h" // CaptureEvent, Scores

#ifdef __cplusplus
extern "C" {
#endif

// --- stat layout (1:1 with the 5 nutrition axes) ---------------------------
enum {
    ST_MASS = 0, // <- calories    (Volume)
    ST_VIGOR, //    <- freshness   (Strength/RSSI)
    ST_WILD, //     <- additives   (Entropy; high = feral/junk)
    ST_AURA, //     <- rarity      (personal rarity)
    ST_MIND, //     <- nourishment (Structure/decode depth)
    PET_STAT_COUNT
};

// --- calibration-deferred constants ----------------------------------------
// Stat EMA pull weights (novel catches pull harder than repeats; anti-grind via rarity).
#define PET_STAT_ALPHA_BASE  0.10f // repeat catch: gentle pull
#define PET_STAT_ALPHA_NOVEL 0.45f // novel (first-seen) species: strong pull
#define PET_STAT_RARE_BOOST  0.50f // extra pull scaled by rarity (anti-grind)
#define PET_STAT_ALPHA_MAX   0.95f

// exp = EXP_BASE * quality(0..1) * repeat_decay(0..1).
#define PET_EXP_BASE       64.0f
#define PET_EXP_DECAY_HALF 8.0f // repeat_decay = HALF/(HALF+seen_count)

// Level curve: cumulative exp to reach level L is EXP_K * L^2 (pow fixed at 2 so
// the lookup is integer-only and libm-free). Level is a maturity clock only.
#define PET_EXP_K       50u
#define PET_LEVEL_MAX   99u
#define PET_TYPE_PERIOD 5u // re-derive the type every 5 levels (checkpoint)

// Life-stage gates (display only; type derivation still runs every PET_TYPE_PERIOD).
// Egg below CHILD, a lineage-tinted child until ADULT, the layered morph at/after it.
#define PET_LEVEL_CHILD 5u  // egg  : level <  CHILD   (1..4)
#define PET_LEVEL_ADULT 10u // child: level <  ADULT   (5..9); adult morph from ADULT

// Type-shape thresholds on the [0,1] stat scale (PROVISIONAL).
#define PET_SHAPE_T_HI 0.30f // max-2nd >= T_HI  => Pure specialist
#define PET_SHAPE_T_LO 0.08f // max-2nd <  T_LO  => Sprout (secondary trait emerging)
#define PET_SHAPE_M_HI 0.25f // max-mean>= M_HI  => Crested (strongly dominant)
#define PET_SHAPE_M_LO 0.10f // max-mean<  M_LO  => Diffuse (flat/generalist)

// Type-space cardinality.
#define PET_FAMILY_COUNT  5u
#define PET_PARTNER_COUNT 4u
#define PET_SHAPE_COUNT   5u
#define PET_TYPE_COUNT    (PET_FAMILY_COUNT * PET_PARTNER_COUNT * PET_SHAPE_COUNT) // 100

// Reserved id for a pet that has not reached its first checkpoint (level < 5).
#define PET_TYPE_UNFORMED 0xFFu

// Shape silhouettes (index == shape value).
typedef enum {
    SHAPE_PURE = 0, // one stat dominates cleanly
    SHAPE_SPROUT, //  secondary trait is close enough to bud
    SHAPE_CRESTED, //  dominant, with a clear single peak above the mean
    SHAPE_WOVEN, //    moderate spread, no single runaway
    SHAPE_DIFFUSE, //  flat profile / generalist
} PetShape;

// Visible life stage, derived from `level` (not persisted separately). The egg and
// child precede the full 100-type morph so the renderer can reuse a few shared/
// lineage-tinted sprites before composing the adult from layered parts.
typedef enum {
    LIFE_EGG = 0, // level < PET_LEVEL_CHILD : one shared egg
    LIFE_CHILD, //  level < PET_LEVEL_ADULT : lineage-tinted child (by family)
    LIFE_ADULT, //  level >= PET_LEVEL_ADULT: layered 100-type morph
} PetLifeStage;

typedef struct {
    float stat[PET_STAT_COUNT]; // 0..1 EMA per axis
    uint32_t total_exp; // monotonic
    uint16_t level; // derived from total_exp (maturity clock)
    uint8_t type_id; // 0..99, or PET_TYPE_UNFORMED before the first checkpoint
} PetGrowth;

// Initialize a fresh pet: zeroed stats, no exp, level 0, Unformed.
void pet_growth_init(PetGrowth* g);

// exp granted by one meal. Favors delicacies (low additives, high rarity/
// nourishment) and decays with how often this species was already seen (D1/D2).
uint32_t pet_growth_exp_gain(const CaptureEvent* ev, uint32_t seen_count);

// Cumulative-exp -> level (largest L with EXP_K*L^2 <= total_exp), capped.
uint16_t pet_level_for_exp(uint32_t total_exp);

// Visible life stage for a level (egg < CHILD <= child < ADULT <= adult). Pure.
PetLifeStage pet_life_stage(uint16_t level);

// Apply one meal: EMA the 5 stats toward this catch's axes, add exp, recompute
// level, and re-derive type_id when a new level-5 checkpoint is crossed.
// `seen_count` is the species' prior occurrence count (0 => novel/first-seen),
// driving both the EMA pull strength and the exp repeat-decay.
void pet_growth_feed(PetGrowth* g, const CaptureEvent* ev, uint32_t seen_count);

// Map a 5-stat vector to one of the 100 type ids (0..99). Deterministic; ties are
// broken by ascending stat index (MASS < VIGOR < WILD < AURA < MIND).
uint8_t pet_type_resolve(const float stat[PET_STAT_COUNT]);

// Decompose a type id (0..99). UB for PET_TYPE_UNFORMED; check first.
uint8_t pet_type_family(uint8_t type_id); // 0..4  (a ST_* index)
uint8_t pet_type_partner(uint8_t type_id); // 0..3 (slot among the other 4 stats)
uint8_t pet_type_shape(uint8_t type_id); // 0..4  (a PetShape)

// The actual stat index (ST_*) of the partner, given family + partner slot.
uint8_t pet_type_partner_stat(uint8_t family, uint8_t partner);

// Human-readable name, e.g. "Wild-Aura Pure" (or "Unformed"). Always NUL-terminated.
void pet_type_name(uint8_t type_id, char* buf, size_t n);

#ifdef __cplusplus
}
#endif
