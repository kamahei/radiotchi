# Pet Growth Spec — 5 Stats, EXP/Level, and the 100-Type Morph

This document owns the **pet growth layer**: how the 5-axis nutrition label drives five
pet stats, how feeding grants experience and levels, and how the pet's visible *character
type* (one of exactly 100) is derived. It refines [product-spec.md](product-spec.md) §3 and
adds fields to [data-model.md](data-model.md). The rationale and the locked-decision change
are recorded in [decision-log.md](decision-log.md) **D18**; deferred calibration lives in
[open-questions.md](open-questions.md).

Pure, host-tested implementation: `src/game_shell/pet_growth.{h,c}`, tests in
`test/test_pet_growth.c`. The module is **libm-free** and `furi`-free, like the rest of the
host-testable core.

## Why this layer exists (it replaced the EMA quadrant)

The original evolution system (D7/D8) drifted the pet on a 2D Health×Class map as an EMA of
diet — reversible, ~5 forms. That stays as the *engine concept*, but this layer **replaced the
visible 5-form taxonomy with a richer 100-type morph** (D18; the old `pet_state.c` quadrant
renderer + `gamestate.txt` have since been retired) while preserving its two core properties:

- **Reversible (honors §3 / D7).** Stats are EMAs of diet, and the type is *re-derived* from
  the current stat vector at each checkpoint — change your foraging and the morph follows.
- **Collection-first (reinforces D2).** Level is only a *maturity clock*, never power. The
  100 morphs are a **morph-dex** to discover, so progression is collection, not a grind.

> One pet, lifelong (D3). EXP only ever increases; the *type* can move in any direction.

## 1. The Five Stats (1:1 with the nutrition axes)

Each stat is a long-memory EMA in `[0,1]` of one nutrition axis, with novel/rare catches
pulling harder so the ambient junk floor cannot pin the profile.

| Stat | Axis (Scores field) | Flavor |
|---|---|---|
| `MASS`  | `calories` (Volume)        | bulk / data volume |
| `VIGOR` | `freshness` (Strength/RSSI)| energy / proximity |
| `WILD`  | `additives` (Entropy)      | feral / encrypted-junk character |
| `AURA`  | `rarity`                   | prestige / exotic-ness |
| `MIND`  | `nourishment` (Structure)  | intellect / decode depth |

Update per meal: `stat[i] = (1-α)·stat[i] + α·axis[i]`, with
`α = (novel ? ALPHA_NOVEL : ALPHA_BASE) + RARE_BOOST·rarity`, clamped to `ALPHA_MAX`. A catch
is *novel* when its species `seen_count == 0`. Constants mirror the EMA layer and are
**PROVISIONAL** (see `pet_growth.h`).

## 2. Experience (monotonic, delicacy-favoring)

```
exp_gain = round( EXP_BASE · quality · repeat_decay )
quality  = 0.15·calories + 0.15·freshness + 0.20·(1 − additives)
         + 0.25·rarity   + 0.25·nourishment            (clamped to [0,1])
repeat_decay = EXP_DECAY_HALF / (EXP_DECAY_HALF + seen_count)     // 1.0 when first-seen
```

`quality` rewards **delicacies** — legible (low entropy), rare, well-decoded — and starves
junk (D1). `repeat_decay` discounts the Nth identical catch on top of personal Rarity,
discouraging grinding (D2). `EXP_BASE`, `EXP_DECAY_HALF` are deferred-calibration placeholders.

## 3. Level Curve (maturity clock)

Cumulative EXP to *reach* level `L` is `EXP_K · L²`. The exponent is fixed at **2** so the
lookup is integer-only and libm-free; `EXP_K` is the deferred tuning knob.

```
level = max L (≤ LEVEL_MAX) such that EXP_K·L² ≤ total_exp
```

With the placeholder `EXP_K = 50`: L1 at 50 exp, L2 at 200, L3 at 450, L10 at 5 000, L50 at
125 000. `LEVEL_MAX = 99`. Level conveys *maturity*, not combat power.

## 4. The 100-Type Morph

The type is a pure function of the 5-stat vector:

```
type_id = family·20 + partner·5 + shape          // 0..99
```

| Field | Count | Derived from | Meaning |
|---|---|---|---|
| `family`  | 5 | `argmax(stat)` | which stat is **max** → the dominant element |
| `partner` | 4 | `argmax` of the **other 4** | which stat is **2nd** (slot 0..3 among the non-family stats, ascending index) |
| `shape`   | 5 | the gap features below | silhouette / build |

`shape` reads two gaps off the profile:

- `gap2 = max − 2nd`  — **purity** (large = specialist, small = secondary trait emerging)
- `gapM = max − mean` — **dominance** (large = one runaway peak, small = flat/generalist)

```
if   gapM <  M_LO         → SHAPE_DIFFUSE   // flat profile dominates the read
elif gap2 >= T_HI         → SHAPE_PURE      // clean single specialist
elif gap2 <  T_LO         → SHAPE_SPROUT      // secondary trait is close enough to bud
elif gapM >= M_HI         → SHAPE_CRESTED   // one clear peak above the mean
else                      → SHAPE_WOVEN     // moderate spread, no runaway
```

Thresholds (`T_HI/T_LO/M_HI/M_LO`) are **PROVISIONAL**. The space is **exactly 100** and a
clean bijection: `family·20 + partner·5 + shape` ⇄ `(family, partner, shape)` round-trips
(tested). **Ties break to the lowest stat index** (`MASS<VIGOR<WILD<AURA<MIND`) so resolution
is deterministic.

### When the type is (re)derived — reversibility

- Below **level 5** the pet is `PET_TYPE_UNFORMED` (a hatchling; no morph yet).
- The type is **re-snapshotted only when a new level-5 checkpoint is crossed**
  (`floor(level/5)` increases). Between checkpoints the morph is stable; across them it
  re-reads the *current* stats — so a diet change re-speciates the pet at its next checkpoint.

### Life stages (egg → child → adult morph)

The visible form passes through three stages, **derived from `level`** (not stored
separately) via `pet_life_stage(level)`. The adult morph (§5) is **revealed at Lv10**; two
shared pre-morph stages precede it so early play needs only a few sprites:

| Stage | Level | What renders | Count |
|---|---|---|---|
| **Egg** (`LIFE_EGG`)   | `1–4`  (`< PET_LEVEL_CHILD`) | one shared egg | 1 |
| **Child** (`LIFE_CHILD`) | `5–9` (`< PET_LEVEL_ADULT`) | a **lineage-tinted** child (by `family`) | 5 |
| **Adult** (`LIFE_ADULT`) | `10+` (`≥ PET_LEVEL_ADULT`) | one monolithic creature per `family×shape` | 25 |

Type derivation is **unchanged** — it still snapshots at every level-5 checkpoint (5, 10,
15…), so `family` is already available to tint the child at Lv5; the renderer only gates
*what it draws* by stage. Constants `PET_LEVEL_CHILD = 5`, `PET_LEVEL_ADULT = 10` live in
`pet_growth.h`. (D21; supersedes the earlier "morph appears at Lv5".)

### "Max value" and mood are expression, not identity

The **magnitude of the top stat** and the **latest mood (Freshness)** do **not** change the
type id. Like §3's branch-vs-expression split, they modulate *appearance* (size/glow,
expression) within the chosen morph. This keeps the discrete identity space at exactly 100
while still letting two same-type pets look distinct.

## 5. Monolithic image-generated art (one creature per `family×shape`)

The Flipper renders **one coherent 64×64 1-bit sprite per creature** — head and body in a
single drawing (D22, superseding D21's layered compose, which left a visible head/body gap
and read as low-quality primitives). The visible roster is keyed to **two** of the three type
fields:

| Field | Drives | Distinct looks |
|---|---|---|
| `family` (max stat) | which animal motif | 5 |
| `shape` (the max→2nd / max→mean deviation) | the silhouette variation | 5 |
| `partner` (2nd stat) | **not drawn** — shown as text on Pet Detail | — |

So there are **25 visually distinct adults** (`family × shape`); the `partner` slot that fans
them into the full 100 type ids is surfaced as **text** (`pet_type_name`, the "*Mass-**Vigor**
Pure*" middle term), not on the silhouette. **Mood is baked into the art** — there is no
separate eye/expression overlay, and top-stat magnitude no longer scales the sprite (`r` is
vestigial). Plus the two pre-morph stages (§4): one **egg** and a **lineage-tinted child** (5).

```
egg 1 + child 5 + adult 25 = 31 creatures × 4 idle frames = 124 ingested 1-bit frames
```

Every creature is a **full 64×64 frame** drawn at one shared cell origin (centered on the
home pet position; see [ui-spec.md](ui-spec.md)). Each carries a gentle **4-frame idle sway**
(rest → pose A → rest → pose B; the rest beat at frame 2 duplicates frame 0 by design); the
renderer cycles them with `frame & 3` at `ANIM_PERIOD_MS`. Special/autonomous animations may
use the full **128×64** later; the MVP ships this Idle loop only.

**Art source & motifs.** The art is produced by **image generation (Codex CLI)** to a minimal,
discretion-leaving image-generation brief; Codex chose a
per-family animal motif and varies the silhouette by shape:

| `family` | motif | `shape` variation (pure · sprout · crested · woven · diffuse) |
|---|---|---|
| MASS  | turtle | plain → sprouted shell → crested → patterned shell → soft/round |
| VIGOR | rabbit | the shape adds a horn / bud / crest / woven ears / flower per column |
| WILD  | cat    | " |
| AURA  | chick  | " |
| MIND  | owl    | " |

**Format.** Pure black-on-white, 64×64, mode-`1` PNG. The composite is **1-bit additive**:
black pixels are ink, white is transparent (the renderer sets `canvas_set_bitmap_mode(true)`
so the white interior shows the page rather than erasing layers beneath).

**Ingestion.** Source art lives in `art/` (committed): `art/char_<family>_<shape>.png` (25),
`art/child_<family>.png` (5), `art/egg.png`, plus the 4-frame idle loops under `art/idle/`.
`tools/convert_art.py` normalizes the **4 idle frames** into `icons/<name>_{0..3}.png`
(124 files); fbt compiles `icons/*.png` → `radiotchi_icons.h` (`I_<name>`). The C side looks up
the sprite by `(family, shape, frame&3)` in `pet_sprites.c` (`pet_sprite_character` /
`pet_sprite_child` / `pet_sprite_egg`) and draws it by reference with `canvas_draw_icon` — it
no longer composes parts or maps `type_id → one filename`. A PC viewer of the whole roster
(animated) is built by `tools/preview_sprites.py` → `preview/index.html`.

## 6. API surface (`pet_growth.h`)

```c
void     pet_growth_init(PetGrowth*);
uint32_t pet_growth_exp_gain(const CaptureEvent*, uint32_t seen_count);
uint16_t pet_level_for_exp(uint32_t total_exp);
PetLifeStage pet_life_stage(uint16_t level);                   // EGG / CHILD / ADULT
void     pet_growth_feed(PetGrowth*, const CaptureEvent*, uint32_t seen_count);
uint8_t  pet_type_resolve(const float stat[PET_STAT_COUNT]);   // 0..99
uint8_t  pet_type_family(uint8_t), pet_type_partner(uint8_t), pet_type_shape(uint8_t);
uint8_t  pet_type_partner_stat(uint8_t family, uint8_t partner);
void     pet_type_name(uint8_t, char* buf, size_t n);          // e.g. "Wild-Aura Pure"
```

## 7. Status & next steps

- **Done:** pure stat/exp/level/type logic + `pet_life_stage` + host tests (`make -C test`).
- **Wired (on-device):** `PetGrowth` + name persist via `capture_store_*_growth`
  (`growth.txt`); `do_eat` in `radiotchi_app.c` calls `pet_growth_feed`; Home draws the pet
  via `pet_render_draw_growth`, a **life-stage machine** (egg → lineage-tinted child → one
  monolithic `family×shape` adult; 2-pose idle) and the Tamagotchi command menu; Pet Detail
  shows name/level/exp/type/5 stats; an inline editor sets the name.
- **Art (on-device):** the **62 codex-generated 1-bit frames** (31 creatures × 2 idle poses)
  ship in `icons/` and are compiled in by fbt (`fap_icon_assets` → `radiotchi_icons.h`);
  `pet_sprites.c` draws them with `canvas_draw_icon` (by reference). Pipeline: §5 +
  `tools/convert_art.py`. FAP builds clean (Target 7 / API 87.1).
- **Deferred (calibration):** `EXP_BASE/EXP_K/EXP_DECAY_HALF`, shape thresholds, stat α, and
  the per-mood / 128×64 special-animation set — see [open-questions.md](open-questions.md) Q8.
- **Follow-up:** regenerate art into `art/` (Codex) and re-run `tools/convert_art.py` — no code
  change needed; per-mood variants and richer (full 128×64) animations. Keep sprites compiled-in
  and drawn **by reference** — never copy a large sprite buffer onto the ViewPort draw stack
  (the GUI-thread stack is tiny).
