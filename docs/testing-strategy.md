# Testing Strategy

RF-dependent logic is painful to test on-device: ambient input is non-deterministic, there is
no real debugger, and the screen is tiny. The architecture exists largely to make this
testable. The rule:

> **Test the hard, non-deterministic logic OFF-device as pure functions. Test game/UI/storage
> logic ON-device where it behaves deterministically.**

## What Is Tested Where

| Layer | Where | How |
|---|---|---|
| **Analysis / Scoring Core** (`lib/analysis_core/`) | **Host** (off-device) | Pure-function unit tests over recorded + synthetic fixtures. This is where non-deterministic RF logic is made deterministic. |
| **Capture Source adapters** (`src/capture_source/`) | Host (fixture adapters) + on-device (live adapter) | Fixture adapters are unit-tested on host; the live CC1101 adapter is smoke-tested on hardware. |
| **Game Shell** (`src/game_shell/`: pet, evolution, dex, UI, storage) | **On-device** | Deterministic given a `CaptureEvent` stream; test with recorded/synthetic events, no RF needed. |

## Host Test Harness

- The Analysis Core compiles with a plain host compiler (no `furi`/GUI/HW). The harness
  (Python or C, see [architecture.md](architecture.md) §6) feeds it fixtures and asserts on the
  resulting `CaptureEvent` / `Scores`.
- Tests must be **deterministic**: timestamps and the `RarityView` are passed in as data, so a
  given fixture + dex view always yields the same scores.
- Cover, at minimum: each decode-free axis (Calories, Freshness, Additives/entropy, Rarity),
  the Nourishment tiers (RAW → MODULATION → PROTOCOL → VALUES), graceful degradation
  (unknown signal still yields a full 4-axis label), and provisional fingerprint-species
  bucketing + graduation.

## Fixture Taxonomy (`fixtures/`)

Captured during recon (see [implementation-plan.md](implementation-plan.md)) and curated:

- **`.sub` files** — what the *actual production sensor* (Flipper `Read RAW`) can grab and what
  the firmware decodes. **This is the ground truth the game is calibrated to.**
- **rtl_433 JSON** — broad environmental distribution + rich decoded values. Useful as a schema
  reference and for *learning*, but **do not over-design** scoring around richness the Flipper
  cannot reproduce.
- **Synthetic captures** — hand-built edge cases (empty/short burst, max entropy, known
  protocol, malformed frame) to pin down axis boundaries and degradation behavior.

> ⚠️ Calibrate the game to the **Flipper-captured ground truth**. The gap between "rich truth"
> (rtl_433) and "what Flipper sees" can itself become game flavor ("it looks hazy to your
> Radiotchi") — but it must not silently inflate scores.

## Re-grading Tests (regression for the lossless invariant)

Because captures are stored losslessly and `score_capture` is callable standalone, add tests
that re-grade an old fixture after a new decoder is introduced and assert the Nourishment tier
*increases* without disturbing the other four axes. This protects the "retroactive meaning"
property in [data-model.md](data-model.md).

## On-Device Validation

- Game/UI/storage: verify feed → score display → eat → append `CaptureEvent` → bump species
  count → persist across reboot, using fixture-backed or live captures.
- Build/run via **ufbt** (`ufbt launch`); see [architecture.md](architecture.md) §6.

## Explicitly Avoided

- A **two-Flipper `.sub` TX→RX replay rig** can give repeatable RF input but is fiddly and
  raises **TX / 技適** issues. Prefer host fixtures. (RX-only is a hard guardrail; see
  [project-overview.md](project-overview.md).)
