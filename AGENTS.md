# AGENTS.md

Durable working instructions for AI agents (and humans) in this repository. Start with the
[Source Of Truth](#source-of-truth) order below; the [docs index](docs/README.md) maps the
full document set.

## Project Purpose

Radiotchi is a learning-first Tamagotchi-style virtual pet for the **Flipper Zero** (a C FAP)
that is fed by real Sub-GHz signals captured from the environment. Each catch is scored on a
5-axis "nutrition label" and logged in a longitudinal dex. The project's value is in
**RF understanding** and a **novel concept**, not in being a market product — re-implementing
known things is acceptable and often desirable.

## Source Of Truth

When files overlap, follow this order and call out any conflict instead of silently choosing:

1. The current user request
2. `docs/product-spec.md`
3. `docs/architecture.md`
4. `docs/data-model.md`
5. `docs/implementation-plan.md` / `docs/task-breakdown.md`
6. `README.md`

`docs/decision-log.md` explains *why* locked decisions hold; `docs/open-questions.md` lists
what is *not* decided yet and the current default to use meanwhile.

## Default Workflow

- Read the relevant docs before editing code (start with the Source Of Truth order above).
- Build the **MVP vertical slice first**, then deepen one axis/decoder at a time.
- **Host-test RF logic off-device** before porting; keep the Analysis Core pure.
- Preserve the declared architecture and schema. If a change requires altering them, update the
  affected docs in the same task.

## Boundaries (non-negotiable)

- **Layering:** `lib/analysis_core/` must have **no** `furi`/GUI/storage/clock/hardware
  dependency and must compile on a host. The Game Shell must **not** decode RF or compute
  scores — it only consumes `CaptureEvent`. The only legal cross-layer types are `RawCapture`
  and `CaptureEvent`.
- **RX-only.** Never add a transmit path (Sub-GHz / IR / BLE). No replay, jamming, or
  defeating others' systems.
- **Privacy.** Never store or surface persistent identifiers (TPMS/key-fob IDs) in a trackable
  form; the dex stays local, with no off-device export of identifiers.
- **Design to the CC1101**, not to richer SDR captures. Use rtl_433 only for learning/schema
  reference and calibrate scoring to what the Flipper actually sees.
- **Lossless capture** is an invariant: do not discard raw signal info; keep the `.sub` for
  interesting captures so old captures can be re-graded.
- Do not add dependencies, services, or firmware features without justification; prefer OFW.

## Validation

- Flipper app: build/run with **ufbt** (`ufbt launch`); manifest is `application.fam`.
- Analysis Core: build + unit-test on host against `fixtures/` (see
  [docs/testing-strategy.md](docs/testing-strategy.md)).
- Check changed work against its validation target in
  [docs/task-breakdown.md](docs/task-breakdown.md) and the relevant items in
  [docs/acceptance-criteria.md](docs/acceptance-criteria.md). If a validation cannot be run,
  state what was skipped and why.

## When To Ask Questions

Ask a short question before proceeding if a missing answer would materially change any of:

- firmware target (OFW vs CFW)
- storage or schema shape
- species-granularity taxonomy / scoring calibration
- anything touching the **RX-only** or **privacy** guardrails

Otherwise proceed with the documented default in
[docs/open-questions.md](docs/open-questions.md) and record any new assumption.

## Output Rules

- Write created files in English unless explicitly instructed otherwise.
- Keep chat replies in the user's language.
- Keep explanations concise and practical; call out assumptions, validation status, and
  tradeoffs.
