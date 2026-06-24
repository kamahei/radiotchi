# Task Breakdown

Bounded, independently implementable tasks, ordered to reach the MVP vertical slice first
(see [implementation-plan.md](implementation-plan.md)). Each task lists **scope**,
**dependencies**, and a **validation target**. IDs are stable references.

## Phase 0 — Recon & Fixtures

### T0.1 — Collect rtl_433 + Flipper `.sub` captures
- **Scope:** record ambient signals with rtl_433/RTL-SDR (JSON) and Flipper `Read RAW`
  (`.sub`) across a few Japan-relevant bands; curate into `fixtures/`.
- **Depends on:** none.
- **Validation:** `fixtures/` contains `.sub` + rtl_433 JSON covering multiple species/bands,
  plus a few synthetic edge cases (empty/short, max-entropy, known protocol).

## Phase 1 — Analysis Core (off-device, `lib/analysis_core/`)

### T1.1 — Define core types
- **Scope:** `RawCapture`, `CaptureEvent`, `Scores`, `Modulation`, `DecodeTier`, `RarityView`
  per [data-model.md](data-model.md). No hardware/UI deps.
- **Depends on:** none.
- **Validation:** compiles with a host compiler; struct sizes sane for the device.

### T1.2 — Host test harness + fixture loaders
- **Scope:** `test/` harness; loaders for `.sub` and rtl_433 JSON → `RawCapture`.
- **Depends on:** T0.1, T1.1.
- **Validation:** a fixture round-trips into a `RawCapture`; harness runs and reports pass/fail.

### T1.3 — Calories axis (Volume)
- **Scope:** score from data amount / burst length / bit count.
- **Depends on:** T1.1, T1.2.
- **Validation:** unit tests assert monotonic-with-length behavior on fixtures.

### T1.4 — Freshness axis (Strength)
- **Scope:** score from RSSI/proximity.
- **Depends on:** T1.1, T1.2.
- **Validation:** unit tests across a range of `rssi_dbm` values.

### T1.5 — Additives axis (Entropy)
- **Scope:** Shannon entropy of payload; high = encrypted/junk, low = structured.
- **Depends on:** T1.1, T1.2.
- **Validation:** known-structured fixture scores low, whitened/encrypted fixture scores high.

### T1.6 — Rarity axis (personal)
- **Scope:** compute rarity from a `RarityView` (dex occurrence table) passed in as input;
  core stays pure.
- **Depends on:** T1.1, T1.2.
- **Validation:** first-seen species scores rare; repeated species scores common.

### T1.7 — Provisional fingerprint-species + bucketing
- **Scope:** bucket unknown signals by `(frequency + modulation + length)`; assign provisional
  `species_id`; define graduation when later decoded.
- **Depends on:** T1.1.
- **Validation:** two similar unknowns share a provisional species; a decoded one graduates.

### T1.8 — `analyze_capture` assembly + graceful degradation
- **Scope:** wire axes into `analyze_capture(raw, rarity, ts) -> CaptureEvent`; Nourishment
  defaults to `TIER_RAW` when no decoder applies.
- **Depends on:** T1.3–T1.7.
- **Validation:** an **unknown** signal still yields a full **4-axis** label (Nourishment may
  be lowest tier); deterministic given inputs.

### T1.9 — Nourishment axis + first decoder(s) + re-grade
- **Scope:** Nourishment from `DecodeTier`; add one or more firmware-known protocol decoders;
  expose `score_capture` for standalone re-grading.
- **Depends on:** T1.8.
- **Validation:** re-grading an old fixture after adding a decoder raises Nourishment without
  changing the other four axes (regression test).
- **Status (2026-06-23):** ✅ classifier + real decoder landed. `radiotchi_classify` assigns the
  RAW/MODULATION/PROTOCOL ladder from boundary features (D23); `radiotchi_ook_pwm_decode` then
  **really demodulates** the OOK fixed-code family from the pulse train (`RawCapture.pulses`) to
  reach **TIER_VALUES** (D24). `radiotchi_redecode` is the re-grade entry point; host tests green
  (`test_classify`, `test_regrade`, `test_ook_decode`, `test_values_tier`). *Remaining:* more
  protocol families, and wiring an on-device dex **re-grade pass** that re-reads the `.sub` so
  stored captures can reach VALUES retroactively (TB.1).

## Phase 2 — Calibration

### T2.1 — Resolve species granularity
- **Scope:** decide protocol/model = species, ID = individual (or revise) from real
  distribution; record in [decision-log.md](decision-log.md).
- **Depends on:** T0.1, T1.7.
- **Validation:** taxonomy applied to fixtures yields a sensible species/individual split.

### T2.2 — Calibrate axes + evolution map
- **Scope:** choose axis scaling and Health/Class thresholds + EMA weighting (novel ≫ repeat)
  from the real entropy/rarity distribution.
- **Depends on:** T1.8, T0.1.
- **Validation:** fixtures produce a sensible score spread; junk floor does not pin evolution.

## Phase 3 — FAP Port & Game Shell (on-device)

### T3.1 — FAP scaffold + manifest
- **Scope:** `application.fam`; `ufbt` build/launch; empty app boots.
- **Depends on:** none (can start early).
- **Validation:** `ufbt launch` runs a stub app on hardware.

### T3.2 — Live Capture Source (CC1101)
- **Scope:** sweep band preset, capture the **single strongest** signal → `RawCapture`.
- **Depends on:** T3.1, T1.1.
- **Validation:** Feed produces a `RawCapture` with plausible freq/RSSI on hardware. RX-only.

### T3.3 — Storage (dex)
- **Scope:** SD/FatFs species index + append-only capture log + linked `.sub`; internal
  LittleFS for small `GameState`. Lossless capture invariant honored.
- **Depends on:** T1.1.
- **Validation:** append a `CaptureEvent`, bump `SpeciesRecord`, survive reboot.

### T3.4 — Pet state + evolution EMA
- **Scope:** `GameState`; Health/Class EMA from `CaptureEvent`s; expression from Calories/
  Freshness. One lifelong pet.
- **Depends on:** T1.8, T2.2, T3.3.
- **Validation:** feeding shifts the pet's map position per the EMA; novel catches move it more.

### T3.5 — UI / ViewPort screens
- **Scope:** Home/Pet, Feed Flow ("what it was" + nutrition label + eat), Dex browser
  (privacy-safe ID presentation), per [ui-spec.md](ui-spec.md).
- **Depends on:** T3.2, T3.3, T3.4.
- **Validation:** full loop is navigable on the 128×64 display; label shows 4 axes (Nourishment
  may be `—` in MVP).

### T3.6 — MVP wiring & persistence check
- **Scope:** connect Feed → sweep → strongest → `analyze_capture` (4 axes) → display → eat →
  append + count → persist.
- **Depends on:** T3.2–T3.5.
- **Validation:** the full MVP acceptance criteria pass on hardware (see
  [acceptance-criteria.md](acceptance-criteria.md)).

## Post-MVP (backlog)

- **TB.1** more decoders → higher Nourishment tiers + re-grade pass. *(2026-06-23: the
  **re-grade pass** landed — `capture_store_regrade` rewrites the log + rebuilds the species
  index, via the **Re-grade** Home-menu command; D25. It re-grades by signature AND re-reads each
  OOK row's `.sub` timing to reach **VALUES** retroactively (`radiotchi_parse_raw_data` +
  `radiotchi_ook_pwm_decode`, host-tested). A second protocol family — a structured **2FSK
  sensor** class (`FSK-Sensor`), privacy-safe family-level species — also landed (D26). The
  FatFs rewrite was **validated on hardware** (2026-06-24) and the OOK decoder was hardened
  against real ambient noise (require a confirmed repeat; glitch-filtered short unit; gap floor).
  The **firmware Sub-GHz decoder integration landed** (D29): the capture source replays the
  pulse train through a `SubGhzReceiver`/`subghz_protocol_registry` and reports the decoded
  protocol + a privacy-safe hashed serial in `RawCapture.fw_protocol`/`fw_individual`, which the
  core uses ahead of the heuristics. Builds + links clean; on-device runtime validation pending.
  Remaining: more pure-core families + a real FSK value decoder.)*
- **TB.2** dex diff-based learning views (static/incrementing/world-varying bytes). *(2026-06-24:
  **individual recurrence** landed — a privacy-safe one-way `id-XXXX` fingerprint of a decoded
  stable code (`CaptureEvent.individual` + log column), shown in the dex captures list/detail so
  the same device is scannable over time; D27. **Hardware finding:** the individual tag only
  works for clean **PWM** (where we read the actual code); a non-PWM remote (e.g. Manchester)
  reaches VALUES via the repeating-frame detector but the waveform fingerprint is too coarse
  (it collides across the *buttons* of one remote), so it deliberately emits **no** id (D28).
  Remaining: a **firmware-Sub-GHz-decoder** integration for a genuine per-device serial on
  non-PWM protocols (TB.1); byte-level diffs; dex UI display visual check.)*
- **TB.3** asynchronous "ghost battles"; later a P2P channel (IR first). See
  [architecture.md](architecture.md) §7 and [open-questions.md](open-questions.md).
