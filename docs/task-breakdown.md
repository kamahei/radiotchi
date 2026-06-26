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
  **2026-06-25: the real FSK value decoder landed** (D31) — `radiotchi_fsk_sensor_decode`
  demodulates the PCM/NRZ-with-repeat sensor subclass to **VALUES**, `analyze_capture` uses it
  for live 2FSK, and the re-grade now re-reads the `.sub` for **OOK *or* 2FSK** rows (shared
  `read_sub_pulses`). Host-tested + committed synthetic FSK fixture (`AA C5 3D`); on-device
  validation pending. **2026-06-26: three breadth additions landed** (D36) — a real **OOK
  Manchester** value decoder (`radiotchi_manchester_decode`, half-bit transition timing + phase
  search + repeat-confirm) that reaches **VALUES with a genuine per-device tag** for the
  bit-decodable Manchester subclass (closing D28's id gap), slotted into `analyze_capture` and the
  `.sub` re-grade between the PWM and repeating-frame paths; a **multi-modulation capture sweep**
  (`SubGhzCaptureConfig` candidate presets, default OOK650 + 2FSKDev476) so **FSK** signals are
  actually received (the RSSI sweep locks the frequency, then the source captures it under each
  preset and keeps the most-decoded); and **maker-named family species**
  (`radiotchi_species_for_protocol` maps brand-named firmware protocols → `<brand-family>-<band>`,
  e.g. `gate-came-433`, `keyfob-starline-433`; per-device serial stays in the hashed tag, A5).
  Host-tested (167 checks) + FAP builds clean; on-device FSK-reception + Manchester validation
  pending. Remaining: a real-capture FSK/Manchester regression fixture + more framings/families,
  and preamble/length-aware Manchester framing. **2026-06-26: decoder-extensibility foundation
  landed** (D37) — a pure **decoder toolkit** (`radiotchi_crc8`/`checksum8`/`xor8`/`bits_get`/
  `pwm_to_bytes`), a single shared **`radiotchi_decode_from_pulses`** dispatch used by both live
  capture and the `.sub` re-grade (existing decoders wrapped, zero behaviour change), and a
  **CRC-validated generic sensor decoder** that reaches VALUES on any standard-CRC sensor and
  graduates to a structural `sensor-<mod>-<n>B-<crc>-<band>` species (a new dex entry per device
  class), with the OOK length floor keeping fixed-code remotes in `ook-fixed`. `SPECIES_INDEX_MAX`
  128→256. Host-tested (185 checks). **2026-06-26: first named device decoders landed** (D38) —
  **Acurite-606** (433 OOK PWM, CRC-8/0x07 gated → `weather-acurite-433`) and **Nexus-TH** (433 OOK
  PPM, constant-0xF-nibble gated → `th-nexus-433`), plus a new `radiotchi_ppm_to_bytes` slicer.
  Each reimplemented from public layouts, gated on a documented invariant so non-matching frames
  fall through safely. A **random-pulse fuzz harness** (`test_fuzz_noise`, 4000 trials) then
  hardened `radiotchi_fsk_sensor_decode` against two false-positive paths (all-identical frames;
  trivially few-transition frames via `WV_FSK_MIN_RUNS`). Host-tested (201 checks). A spec-driven
  `.sub` fixture generator (`tools/gen_sub_fixtures.py`) + committed synthetic fixtures now exercise
  the named/CRC decoders through the real on-disk parse path (207 checks). **`radiotchi_manchester_to_bytes`
  + a generic Manchester-CRC sensor** (`sensor-manch-<n>B-<crc>-<band>`, the Oregon/TPMS-class
  encoding) landed, fuzz-confirmed false-positive-free (212 checks). Fast-follow: more named models
  (Oregon/LaCrosse/TPMS) once real captures confirm signatures, a **bit-level sync-word search** for
  preamble-framed protocols, and GFSK/MSK in the sweep. **`radiotchi_find_sync` (bit-level sync
  search) + a preamble/sync FSK sensor** (`sensor-2dd4-<n>B-<crc>-<band>`, the Fine Offset/Ecowitt/
  LaCrosse-class structure the whole-frame CRC sensor can't read) landed, fuzz-confirmed
  false-positive-free (220 checks). The pure decode toolkit is now crc8/checksum8/xor8/bits_get/
  find_sync + pwm/ppm/manchester→bytes slicers. An **offline `.sub` analyzer** (`tools/sub_analyze.c`)
  + a **hardware-testing guide** (`docs/hardware-testing.md`) prep the on-device validation loop.
  **2026-06-26: a high-effort multi-agent code review** of the decode work fixed the confirmed
  over-/under-acceptance bugs: all-same guards added to `decode_acurite606` and
  `radiotchi_manchester_decode` (degenerate all-0/all-1 frames no longer mint phantom species);
  `decode_manch_sensor` now also runs for **2FSK** (FSK-Manchester / TPMS class); the 0x2DD4 sync
  sensor is tied to the documented **CRC-8/0x31** only (no 3-poly multiplier); `decode_nexus_th`
  gains a **temperature-plausibility** gate; honest comments on the glitch-filtered min estimator and
  the ~3/256 multi-poly residual (Q3). Host-tested (225 checks). **Deferred re-grade follow-ups**
  (real but lower-urgency, retroactive-consistency only): the `.sub` re-grade re-dispatches a row
  only when its tier < VALUES, so a stored generic `ook-fixed`/`fsk-sensor` does not graduate to a
  newly-added named decoder; the firmware brand remap is not re-applied on re-grade; and the
  byte-diff learning collector (`capture_store.c`) still decodes only OOK-PWM/2FSK-NRZ, so PPM/
  Manchester families show an empty diff. **A second review pass (full source, not just the diff)**
  then fixed real bugs outside the decode work: the `.sub` reader truncated long RAW_Data lines
  (re-grade/diff recovered ~36 of ~512 pulses — now a heap-`FuriString` line accumulator); the
  re-grade rewrote the authoritative log with unchecked writes and removed-before-rename (a partial
  temp write could replace the live log — now writes are checked and a failure aborts the swap); a
  single-frequency capture plan could never clear the relative-margin gate (the noise floor included
  the winning sample — now estimated from the non-winning ones); plus latent `uint16` loop wraps
  (`repeating_frame`/`find_sync`), a `pulse_count` clamp, and a pet mood/hunger boundary. **The
  deferred re-grade follow-ups are now RESOLVED:** `regrade_row` re-dispatches generic-VALUES rows
  (`ook-fixed`/`fsk-sensor`) so they graduate to newly-added named decoders (but NOT firmware-decoded
  rows, which the offline dispatch would downgrade), re-applies the brand remap to reconcile old
  `Star Line`-style species, and rewrites on any field change (not just tier); the diff collector
  `decode_sub_payload` is now SPECIES-DIRECTED (ppm/manchester/pwm/fsk per family) so PPM/Manchester
  species populate the learning view. **2026-06-26: the named OOK decoders are now validated against
  REAL data** — `tools/rtl433_cu8_to_sub.py` bridges the rtl_433_tests corpus (cu8 IQ -> magnitude
  threshold -> Flipper `.sub`, OOK only) so real **Acurite-606** and **Nexus-TH** captures run through
  the pure core. This broke the synthetic circularity and found real bugs: (1) real RX/IQ slicers emit
  sub-us glitches that split a pulse and desync the pair-walking PWM/PPM slicers — fixed by a
  `radiotchi_coalesce_glitches` pre-pass at the dispatch entry (no-op on clean frames); (2) **Acurite-606
  was a fiction** (assumed mark-PWM + CRC-8); the REAL coding is gap/PPM, sync-gap-framed, with an
  **LFSR-8 digest** check (`radiotchi_lfsr_digest8(b,3,0x98,0xf1)==b[3]`) — `decode_acurite606` rewritten
  and confirmed on three real frames; (3) the named sensors hashed the whole (temperature-varying)
  frame for the per-device tag, minting a new individual every reading — now hash the ID byte only
  (stable per device). **2026-06-26 (FSK): the cu8 bridge gained a 2FSK mode** (FM discriminator =
  sign of `Im(z[i]*conj(z[i-1]))`, amplitude-gated, boxcar-smoothed to isolate the burst from IQ
  noise) and a real **LaCrosse-TX29** (868 MHz) capture now decodes end-to-end through
  `decode_fsk_sync_sensor` -> `sensor-2dd4-5B-c31-868`, byte-for-byte matching rtl_433 (id 10, 4.8 C,
  CRC-8 poly 0x31 over the 5-octet frame). Three more real bugs the FSK path hid: (4) the 60us
  glitch-coalesce ate 58us FSK bits — lowered to 24us (still kills the <=16us dips, under half a bit);
  (5) the sync-word decoder required a confirming repeat, but real sync-framed sensors transmit a
  SINGLE burst — added `fsk_slice_first` (sync+CRC is the guard, no repeat) while the no-CRC generic
  FSK path keeps its repeat; (6) the bit-period was the raw-minimum run, which an onset transient pulls
  below the true symbol and garbles the frame — now `fsk_estimate_bit_period` takes the smallest run
  with cluster support. **2026-06-26 (Manchester): bug (7) found validating a real Oregon-THN132N
  capture** — the Manchester slicer used a single raw-minimum half-bit unit and `round(mag/half_unit)`,
  but real OOK Manchester has a mark/space duty-cycle skew (measured ~424us vs ~556us half-bits) that
  mis-rounds the wider polarity's full bit to 3 half-bits and collapses the phase pairing (the slicer
  recovered NOTHING on the real capture). Fixed: estimate the half-bit width separately per polarity
  (`man_half_for`, cluster-supported) and classify each run as exactly 1 or 2 half-bits against its own
  width (clean Manchester is never wider). **A named `decode_oregon_v2` followed**: Oregon Scientific
  v2.1 is double-Manchester (an outer Manchester chip stream whose post-preamble payload is itself
  Manchester-encoded, then nibbles bit-reversed), checked by a nibble-sum checksum (not a CRC). The
  decoder slices the outer Manchester, finds the preamble end, and over a small offset window runs the
  inner Manchester + reflect + checksum, accepting only a known `sensor_id` with a valid checksum ->
  the matched type's species (id in the hashed tag, A5). VALIDATED byte-exact against two real rtl_433
  captures giving TWO distinct species: **THN132N** (temp only, id 231, 18.0 C) -> `weather-oregon-433`
  and **THGR122N** (temp+humidity, id 248, 18.8 C, 54%) -> `th-oregon-433`. The noisier THGR122N
  surfaced two more real robustness gaps, both fixed: the half-bit estimator (`man_half_for`) was the
  smallest cluster with >=3 support, which a sparse SUB-half-bit noise cluster could win — now it
  thresholds support at a quarter of the dominant cluster; and `decode_oregon_v2` now scans PAST
  leading noise (resetting on short fragments) instead of aborting on the first anomalous run. The
  cu8->sub converter also gained glitch-coalescing (a hard magnitude threshold splits a pulse on every
  1-sample dip, inflating the raw count so a real burst overflowed the 256-pulse buffer; real CC1101
  hysteresis does not). Both species are covered by committed synthetic fixtures. **The table then grew
  (matching the LOW 12 bits of sensor_id, since v3 sensors roll their top nibble) and species are now
  by VALUE CLASS**: RTHN129 (temp) and RTGN318 (temp+hum) join the existing species, and **UVR128 adds
  a new `uv-oregon-433`** — all real-validated via a parallel multi-agent capture survey. Longer Oregon
  frames whose check byte sits late (WGR968 wind ~idx17, BTHR918 pressure ~idx19) expand past the
  256-pulse buffer before the checksum, so wind/pressure are deferred until that buffer can grow (needs
  on-device stack validation). Host 237 checks. Remaining: wind/pressure (buffer), a dedicated Fine
  Offset WH2/WH24 decoder (custom mark-width OOK the generic slicers miss), Acurite-Tower (592TXR,
  sum-checksum), GFSK/MSK reception, and the benign input-handler mutex discipline in
  `radiotchi_app.c`.)*
- **TB.2** dex diff-based learning views (static/incrementing/world-varying bytes). *(2026-06-24:
  **individual recurrence** landed — a privacy-safe one-way `id-XXXX` fingerprint of a decoded
  stable code (`CaptureEvent.individual` + log column), shown in the dex captures list/detail so
  the same device is scannable over time; D27. **Hardware finding:** the individual tag only
  works for clean **PWM** (where we read the actual code); a non-PWM remote (e.g. Manchester)
  reaches VALUES via the repeating-frame detector but the waveform fingerprint is too coarse
  (it collides across the *buttons* of one remote), so it deliberately emits **no** id (D28).
  Remaining: a **firmware-Sub-GHz-decoder** integration for a genuine per-device serial on
  non-PWM protocols (TB.1); byte-level diffs; dex UI display visual check. **2026-06-25:
  byte-level diffs landed** (D32) — `radiotchi_byte_diff` classifies a species' aligned decoded
  frames per byte (static=id / incrementing=counter / varying=value / absent), reconstructed from
  each row's `.sub` (`capture_store_collect_payloads`); a new `ScreenDexDiff` (Right from the
  captures list) renders class-only glyphs (privacy A5). Host-tested; on-device check pending.
  **2026-06-25: individual-scoped diff landed** (D34) — the diff groups a species' frames by the
  one-way `id-XXXX` tag (pure `radiotchi_select_by_individual` + the new
  `capture_store_collect_payloads_for_individual`), so different devices of one family no longer
  smear to VARYING; **Left/Right** step the devices + a species-wide slot, and no-id rows fall back
  to species-wide. Host-tested (separation + mixed-set VARYING regression); FAP builds clean;
  on-device visual check pending.)*
- **TB.3** asynchronous "ghost battles"; later a P2P channel (IR first). See
  [architecture.md](architecture.md) §7 and [open-questions.md](open-questions.md).
