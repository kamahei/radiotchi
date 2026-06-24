# Acceptance Criteria

Project-level and feature-level "done" criteria, mapped to the capabilities in
[product-spec.md](product-spec.md). Each item is meant to be verifiable (host test or on-device
observation). Edge cases are called out explicitly.

## Project-Level (must hold throughout)

- **A1 — Layering enforced.** `lib/analysis_core/` has no `furi`/GUI/storage/clock/hardware
  dependency and compiles on a host; the Game Shell never decodes RF or computes scores
  itself. (Verify: host build of the core links with no firmware libs.)
- **A2 — Determinism.** Given the same `RawCapture`, timestamp, and `RarityView`,
  `analyze_capture` always returns the same `CaptureEvent`. (Verify: repeated host runs match.)
- **A3 — RX-only.** No code path transmits. No replay, jamming, or interfering with others'
  systems exists anywhere. (Verify: no Sub-GHz/IR/BLE TX call in the tree.)
- **A4 — Lossless capture.** Raw signal info (and the `.sub` for interesting captures) is
  retained so scoring/taxonomy can be re-derived. (Verify: re-grade test in A12.)
- **A5 — Privacy.** Persistent identifiers (TPMS/key-fob IDs) are never surfaced in a
  trackable form and never leave the device. (Verify: dex views show truncated/hashed IDs;
  no export/transmit path. Enforced at the source — the classifier graduates recognized
  families only to a **family-level** species and never extracts the payload id, D26;
  `test_classify` asserts the species carries the band, never a per-device id.)
- **A6 — Designed to CC1101.** Scoring/balance assume narrowband, one-frequency-at-a-time,
  no 2.4/5 GHz; no logic depends on capabilities only an SDR has. (Verify: review against
  [architecture.md](architecture.md) §4.)

## Nutrition Label (5 axes)

- **A7 — Four decode-free axes always present.** Calories, Freshness, Additives, and Rarity
  are computed for *every* capture, including completely unknown signals.
- **A8 — Graceful degradation.** An unknown, undecodable signal still yields a full 4-axis
  label; Nourishment falls back to the lowest tier (`TIER_RAW`) rather than failing.
  *Edge case:* "Unknown × high-entropy × rare band" produces a high-tier delicacy label.
- **A9 — Entropy direction.** High-entropy (encrypted/whitened) payloads score Additives as
  *junk*; low-entropy structured payloads score *wholesome*.
- **A10 — Personal rarity.** A first-seen species scores rare; after many repeats of the same
  species, its Rarity drops and repeat-decay reduces feeding value (anti-grind).
- **A11 — Nourishment tiers.** Nourishment increases monotonically across
  RAW → MODULATION → PROTOCOL → VALUES as decode depth increases. *(Exercised by
  `test/test_analysis_core.c`: `test_classify` covers RAW < MODULATION < PROTOCOL via
  `radiotchi_classify` (D23); `test_values_tier` covers PROTOCOL < VALUES via the real
  OOK PWM demodulation (`radiotchi_ook_pwm_decode`, D24).)*
- **A12 — Retroactive re-grade.** Adding a new decoder and re-running `score_capture` on an old
  stored capture raises its Nourishment tier *without* changing the other four axes. *(Exercised
  by `test_regrade`: `radiotchi_redecode` raises a legacy RAW event to PROTOCOL while the other
  four axes stay byte-for-byte equal. On-device, the **Re-grade** Home command applies this over
  the whole capture log (`capture_store_regrade`, D25) — atomically, re-reading each OOK row's
  `.sub` to reach VALUES retroactively, never *writing* the `.sub`. **Validated on hardware
  2026-06-24**: re-grading a real 53-row log rewrote tiers, added the `individual` column, and
  rebuilt the species index (2→9 species) with no corruption; idempotent on re-run.)*

## Capture & Game Loop

- **A13 — Strongest-signal-only.** One Feed captures exactly one meal: the single strongest
  signal during the sweep. *Edge case:* if nothing is detected, Feed reports "nothing to eat"
  cleanly rather than fabricating a capture.
- **A14 — Honest readout.** The "what it was" screen shows real facts (frequency, modulation
  guess or `?`, RSSI, protocol or `Unknown`).
- **A15 — Feed persists.** Eating appends a `CaptureEvent` to the SD capture log, links the
  `.sub` where applicable, and bumps the `SpeciesRecord` count/last-seen.

## Evolution

- **A16 — IIR/EMA drift.** The pet's 2D map position is an EMA of diet with novel/rare catches
  weighted more heavily; feeding shifts it gradually and reversibly.
- **A17 — Junk floor does not freeze evolution.** A run of ambient junk catches does not pin
  the pet permanently in "Junk × Common"; a rare/clean catch visibly moves it. (Verify with a
  fixture sequence in host/sim tests where possible.)
- **A18 — Expression vs. branch.** Calories drive size/strength and Freshness drives daily
  mood/health *within* the current branch; only Health and Class axes change the branch.

## Dex (Dex)

- **A19 — Two layers.** A hot species index (counts, first/last-seen) and a cold append-only
  capture log (per-capture decoded content + metadata) both exist and stay consistent.
- **A20 — Provisional → graduated species.** Unknown captures bucket into a provisional
  fingerprint-species `(frequency + modulation + length)`; once decoded they graduate to a
  named species and existing log entries re-resolve.
- **A21 — Longitudinal value.** Repeated captures of one device are browsable over time,
  enabling diff observation (static / incrementing / world-varying bytes). *(2026-06-24: the
  **individual recurrence** layer landed — a privacy-safe `id-XXXX` tag (D27) groups captures of
  the same decoded device in the dex list/detail. Byte-level static/incrementing diffs await
  multi-field decoders.)*

## MVP (v0) Done

The MVP is accepted when, on real hardware:

1. The FAP boots and shows a stub pet (A-Home).
2. Feed runs a Sub-GHz sweep over a Japan-relevant band preset and captures the strongest
   signal (A13).
3. The four decode-free axes are computed and shown; Nourishment may display `—` (A7, A8).
4. The nutrition label + "what it was" are displayed (A14).
5. Eating updates a stat, appends a `CaptureEvent`, and bumps the species count, surviving a
   reboot (A15, A19).

No TX path exists (A3) and no persistent identifier is surfaced trackably (A5).
