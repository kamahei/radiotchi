# Data Model

This document owns **domain entities, schema, invariants, relationships, and access
patterns**. It is a sketch to refine after recon; the *shapes* and *invariants* are stable,
the exact field widths and calibration are not. See [architecture.md](architecture.md) for how
these records cross layer boundaries and [open-questions.md](open-questions.md) for deferred
decisions.

## Core Invariant: Capture Losslessly

> Captures are stored **losslessly** so that any future taxonomy or scoring can be re-derived
> retroactively. Never discard raw signal information on the assumption that the current
> decoder/scoring is final.

This invariant is what makes the dex a genuine longitudinal learning instrument: a decoder
written next month can re-grade a capture taken today. It also drives the decision to keep the
raw `.sub` on disk and to keep `score_capture` callable standalone (re-grading), see
[architecture.md](architecture.md).

## Entities

### RawCapture (boundary input — Capture Source → Analysis Core)

The minimal raw observation. Produced by any Capture Source (live CC1101 or a fixture
adapter); consumed only by the Analysis Core.

| Field | Type (sketch) | Notes |
|---|---|---|
| `frequency_hz` | `uint32_t` | Center frequency of the captured signal |
| `rssi_dbm` | `int16_t` | Received signal strength |
| `bits` / `burst` | byte buffer + length | Raw demodulated bits or burst samples (a feature *proxy*; see below) |
| `pulses` / `pulse_count` | `int16_t[256]` + len | Bounded prefix of the raw pulse train (+mark/-space µs, clamped); `0` if no timing. Lets the pure core actually demodulate known protocols (OOK PWM fixed-code → **TIER_VALUES**, D24). The lossless original is still the `.sub`. |
| `raw_sub_ref` | `char[64]` | Path to the saved `.sub` (empty if not persisted yet) |

`RawCapture` carries **no** timestamp and **no** scores; those are added by the Analysis Core
(timestamp injected as data, scores computed), keeping the core pure.

### CaptureEvent (the learning record — Analysis Core → Game Shell; persisted)

One real capture, stored losslessly. This is the archival/learning record.

```c
typedef struct {
    uint64_t   timestamp;        // epoch or RTC; injected, not read from a global clock
    uint32_t   frequency_hz;
    Modulation modulation;       // enum: OOK, FSK/2-FSK, UNKNOWN, ...
    int16_t    rssi_dbm;
    uint16_t   bit_count;
    float      entropy;          // Shannon entropy of payload
    DecodeTier decode_tier;      // RAW | MODULATION | PROTOCOL | VALUES
    char       protocol[32];     // "" if unknown
    uint8_t    payload[N];       // decoded or raw bits
    char       raw_sub_ref[64];  // path to saved .sub (for re-decode later)
    Scores     scores;           // the 5 axes (see below)
    char       species_id[32];   // resolved or provisional fingerprint (family-level)
    char       individual[16];   // privacy-safe per-device tag "id-XXXX" ("" unless a stable
                                 // code was decoded); a one-way hash, never the raw id (D27)
} CaptureEvent;
```

### Scores (the 5-axis nutrition label — embedded in CaptureEvent)

```c
typedef struct {
    float calories;     // Volume:    data amount / burst length / bit count
    float freshness;    // Strength:  RSSI / proximity
    float additives;    // Entropy:   Shannon entropy (high = encrypted/junk)
    float rarity;       // personal rarity, derived from the dex
    float nourishment;  // Structure: decode depth (the only decode-dependent axis)
} Scores;
```

Axis definitions are fixed; numeric calibration is deferred (see
[product-spec.md](product-spec.md) §2 and [open-questions.md](open-questions.md)).

### SpeciesRecord (game-side aggregate — hot state)

Lightweight, fast, one per device/protocol *type*. Drives personal rarity, dex completion,
and repeat-decay.

```c
typedef struct {
    char     species_id[32];
    uint32_t count;
    uint64_t first_seen, last_seen;
} SpeciesRecord;
```

### Pet / GameState (hot state — Game Shell only)

> **Implemented as the growth layer `PetGrowth`** (`pet_growth.h`, persisted to `growth.txt`):
> 5 per-axis stat EMAs + EXP/level + the 100-type morph (D18) + the user-given name. The
> original Health×Class quadrant `GameState` below was the design sketch; its code
> (`pet_state.c` + `gamestate.txt`) was **retired** once the morph superseded it (D30). The
> **EMA/IIR-over-diet** principle is unchanged — the stat vector is the filter's accumulator.

| Field (original sketch) | Purpose |
|---|---|
| `health_axis` | Wholesome ↔ Junk, EMA of f(additives, nourishment) |
| `class_axis` | Common ↔ Exotic, EMA of f(rarity) |
| `size` / `strength` | Expression from Calories (Volume) |
| `mood` / `daily_health` | Expression from Freshness (RSSI) |
| `birth_time`, `last_fed` | Lifecycle of the single lifelong pet |
| `name` | User-given pet name (editable in Pet Detail / Tune; default e.g. `Radiotchi`). Identity-only, not growth math. |

**Growth layer (the 100-type morph, see [pet-growth-spec.md](pet-growth-spec.md), D18).** The
visible form is now derived from five per-axis stats plus an EXP/level clock, superseding the
5-form quadrant taxonomy. `health_axis`/`class_axis` remain as a derived legacy view; the
growth fields below are the identity going forward:

| Field | Purpose |
|---|---|
| `stat[5]` | EMA `[0,1]` per nutrition axis (MASS, VIGOR, WILD, AURA, MIND) |
| `total_exp` | Monotonic lifelong experience (maturity clock, not power) |
| `level` | Derived from `total_exp` via `EXP_K·L²`; capped at `LEVEL_MAX` |
| `type_id` | `0..99` (`family·20 + partner·5 + shape`), or `UNFORMED` before level 5; re-derived at each level-5 checkpoint. Read as **anatomical layers** for rendering — `family`→lineage base, 2nd stat→body accent, `shape`→head (D21) |

The **visible life stage** (egg / child / adult morph) is **derived from `level`** at draw time
(`pet_life_stage`: egg `<5`, lineage-tinted child `<10`, layered morph `≥10`; D21) — it is
**not** a stored field. Pet graphics are composed from layered 1-bit parts keyed by `type_id`,
not one sprite per type; see [pet-growth-spec.md](pet-growth-spec.md) §5.

**Care/mood layer (`PetCare`, `pet_mood.h`, D33).** A thin presentation + soft-pressure layer
over the growth identity — it never touches `stat[]`/`exp`/`level`/`type_id`. Just two persisted
scalars, derived into hunger and a mood (content/happy/neutral/hungry/neglected) against an
injected "now":

| Field | Purpose |
|---|---|
| `last_feed_time` | Epoch seconds of the last meal (`0` = never fed → grace, no hunger) |
| `last_meal_quality` | `0..100`, centi of `pet_growth_meal_quality` at that meal (drives the content look) |

Persisted as a **back-compatible third line** in `growth.txt`: `care=<last_feed_time> <quality>`.
A pre-care file (no `care=` line) loads its growth+name unchanged and seeds `PetCare` to never-fed
grace, so upgrading an existing pet never triggers false starvation.

## Enumerations

```c
typedef enum { MOD_UNKNOWN = 0, MOD_OOK, MOD_2FSK, MOD_GFSK, /* ... */ } Modulation;

// Decode depth — directly drives the Nourishment axis.
typedef enum {
    TIER_RAW = 0,     // raw burst only
    TIER_MODULATION,  // known modulation identified
    TIER_PROTOCOL,    // known protocol identified
    TIER_VALUES       // actual values decoded
} DecodeTier;
```

The tier is assigned by `radiotchi_classify` inside `analyze_capture` (decision-log D23). It is
**signature-based, not a re-demodulation**: the in-struct `payload` is only a feature proxy (the
lossless bits live in the linked `.sub`), so the classifier reads the reliable boundary features
— band, modulation, structure (entropy), burst length — and assigns: **RAW** for an unknown
modulation or a whitened/high-entropy payload (encrypted junk, D1); **MODULATION** for a known
modulation carrying a structured, non-whitened burst; **PROTOCOL** when the burst matches a known
protocol family's signature (the OOK fixed-code remote family, and a structured 2FSK sensor
class `FSK-Sensor`; D23/D26), which also graduates the fingerprint to a **family-level** species
(`ook-fixed-<band>` / `fsk-sensor-<band>` — never a per-device id; A5). **VALUES** is reached when the actual
code is demodulated from the **pulse train** (`RawCapture.pulses`): `radiotchi_ook_pwm_decode`
reads the OOK fixed-code bits and, on a self-consistent frame, raises the tier to VALUES (D24).
Privacy (A5): the decoded code only justifies the tier — it is **not** surfaced as a per-device
species; the species stays family-level. `radiotchi_redecode` re-runs the *signature* classifier
on a stored event to raise its tier as decoders are added — the re-grade path — leaving the other
four axes untouched (the lossless invariant in action); a full VALUES re-grade re-reads the `.sub`
timing (the on-device dex re-grade pass, TB.1).

## Species Granularity & Fingerprint-Species

- **Species granularity is DEFERRED** until the real distribution is seen (see
  [open-questions.md](open-questions.md)). Current default: protocol/model = *species*, a stable
  ID within the payload = *individual*.
- **Provisional fingerprint-species:** unknown signals are bucketed by
  `(frequency + modulation + length)` into a provisional species. When a decoder later
  identifies them, they **graduate** to a named species, and existing capture-log entries can
  be re-resolved (their `species_id` updated) because the raw `.sub` is retained.

## Relationships

- `SpeciesRecord 1 — N CaptureEvent` via `species_id` (one species, many individual captures).
- `CaptureEvent N — 1 .sub file` via `raw_sub_ref` (each interesting capture links its raw
  recording).
- `GameState` derives from the *stream* of `CaptureEvent`s (EMA), not from a foreign key.

## Storage Layout & Access Patterns

| Store | Medium | Contents | Access pattern |
|---|---|---|---|
| Species index | SD (FatFs) | `SpeciesRecord` table | Hot: read/written every feed; small; kept fully in RAM-friendly form |
| Capture log | SD (FatFs) | Append-only `CaptureEvent` records | Cold: append on feed, scan on dex browse / re-grade |
| Raw `.sub` files | SD (FatFs) | Original recordings | Cold: written for interesting captures, re-read on re-decode |
| Pet / small state | Internal flash (LittleFS) | `GameState` accumulators | Hot but tiny; survives without SD |

- **Write path (feed):** `RawCapture` → `analyze_capture` → `CaptureEvent` → append to capture
  log + (optionally) persist `.sub` → update `SpeciesRecord` (count/last_seen) → update
  `GameState` EMA.
- **Read paths:** dex browse (scan species index, then drill into capture log by
  `species_id`); rarity lookup (species index → `RarityView` passed into the Analysis Core);
  re-grade (scan capture log, re-run `score_capture` / re-decode from `.sub`).
- **Indexing need:** the capture log is append-only; if dex browsing by species becomes
  slow, add a per-species offset index rather than reordering the log (keep it append-only to
  preserve the lossless/longitudinal property).

## Privacy Constraints on the Schema

Some payload values (TPMS IDs, key-fob IDs) are persistent identifiers. They may be stored
locally for diff-based learning, but:

- The dex stays **local**; no export path may transmit identifiers off-device.
- UI must **not** surface raw persistent identifiers in a way that enables tracking a vehicle
  or person. Prefer hashed/aggregated presentation in dex views.
- The protocol classifier enforces this at the source (D26): recognized families graduate only
  to a **family-level** species (`ook-fixed-<band>`, `fsk-sensor-<band>`); it never extracts the
  TPMS/key-fob id from the payload, so no per-device identifier ever enters a `species_id`.
- Individual tracking (D27) uses a **one-way hashed** tag (`CaptureEvent.individual`, `id-XXXX`):
  the same device recurs under the same tag for local diff-learning, but the raw code cannot be
  recovered from it — the dex can show recurrence without becoming a device/vehicle tracker.
- The **diff-learning view** (`ByteDiff` from `radiotchi_byte_diff`, D32) is a **derived,
  non-persisted** result: frames are re-decoded from the lossless `.sub` on demand
  (`capture_store_collect_payloads`), classified per byte (id / counter / value / absent), and
  the UI renders only the **class markers, never the raw byte values** — it *locates* the id
  bytes for learning without surfacing the trackable id.

See [acceptance-criteria.md](acceptance-criteria.md) for the verifiable form of these rules.
