# Implementation Plan

Delivery order, dependencies, and validation gates. The strategy follows the architecture's
testability split (see [architecture.md](architecture.md) and [testing-strategy.md](testing-strategy.md)):
do the hard RF logic off-device first, then port to hardware. Build **downward into the stack
one axis/decoder at a time**, starting from a working end-to-end slice.

## Phase 0 — Recon (two-pronged data collection)

Collect real RF data before locking any scoring or taxonomy. Two sources, deliberately:

- **rtl_433 + RTL-SDR** (cheap; or HackRF later, shipping ~July 2026): broad environmental
  distribution + rich decoded JSON + learning. Its JSON is a good schema reference.
- **Flipper `Read RAW` (`.sub`)**: what the *actual production sensor* can grab and what the
  firmware decodes.

> ⚠️ **Do not over-design around rtl_433 richness the Flipper cannot reproduce.** Calibrate to
> the Flipper-captured ground truth.

**Output:** a curated `fixtures/` set (`.sub` + rtl_433 JSON + a few synthetic edge cases).
**Gate:** enough fixtures to exercise every axis and several distinct species/bands.

## Phase 1 — Analysis / Scoring Core, off-device

Build `lib/analysis_core/` as **pure functions** with host unit tests against the fixtures.
This is where non-deterministic logic is made deterministic.

- Define `RawCapture`, `CaptureEvent`, `Scores`, enums (see [data-model.md](data-model.md)).
- Implement the **four decode-free axes** (Calories, Freshness, Additives/entropy, Rarity) and
  the Nourishment tier ladder (RAW → MODULATION → PROTOCOL → VALUES) as decoders allow.
- Implement provisional fingerprint-species bucketing.

**Gate:** host tests green for every axis, graceful degradation (unknown → full 4-axis label),
and a re-grade regression test (see [testing-strategy.md](testing-strategy.md)).

## Phase 2 — Calibration & deferred decisions

With real distribution data in hand, resolve the deferred decisions (see
[open-questions.md](open-questions.md)):

- **Species granularity** (protocol/model = species, ID = individual?).
- **Evolution-map calibration** (Health/Class axis thresholds; EMA weighting of novel vs.
  repeat catches).

**Gate:** calibration constants chosen from the real distribution and recorded in
[decision-log.md](decision-log.md); scores produce a sensible spread on the fixture set.

## Phase 3 — Port to FAP + build the Game Shell, on-device

Port the core into the Flipper FAP and build the device-facing layer.

- `application.fam` manifest; `ufbt` build/launch.
- Live **Capture Source** adapter (CC1101 sweep → strongest signal → `RawCapture`).
- **Game Shell**: pet state, evolution EMA, dex (species index + capture log), UI/ViewPort
  (see [ui-spec.md](ui-spec.md)), storage (SD/FatFs + internal LittleFS).

**Gate:** the MVP vertical slice (below) runs on hardware and persists across reboot.

## MVP (v0) Vertical Slice

The first milestone is a minimal but complete pipeline (capture → score → display → feed →
persist) with **zero decoding**, then grown downward one axis/decoder at a time:

1. FAP boots, shows a stub pet on a ViewPort.
2. **Feed** triggers a Sub-GHz sweep over a small Japan-relevant band preset; captures the
   **strongest** signal.
3. Compute the **four decode-free axes**; skip Nourishment.
4. Display the nutrition label + basic "what it was" (freq, modulation guess, RSSI).
5. Pet eats → simple stat update; append a `CaptureEvent` to the SD log; bump the species
   count.

Acceptance detail in [acceptance-criteria.md](acceptance-criteria.md); concrete tasks in
[task-breakdown.md](task-breakdown.md).

## Dependency Summary

```
Phase 0 (recon/fixtures)
   └─> Phase 1 (Analysis Core off-device)
          └─> Phase 2 (calibration)   ── can overlap with early Phase 3 scaffolding
                 └─> Phase 3 (FAP port + Game Shell)  ──> MVP slice
```

The MVP can be reached by sequencing the **decode-free** parts of Phases 1–3; the Nourishment
axis and richer decoders are added afterward without changing the architecture.

## Post-MVP (not on the critical path)

- Additional decoders → higher Nourishment tiers; re-grade old captures.
- Diff-based learning views in the dex.
- Asynchronous "ghost battles", then a P2P channel (IR first). See
  [architecture.md](architecture.md) §7 and [open-questions.md](open-questions.md).
