# Product Spec

This document defines functional and non-functional requirements. It is the second-highest
source of truth after the current user request. For the *why*, see
[project-overview.md](project-overview.md); for *how it is built*, see
[architecture.md](architecture.md) and [data-model.md](data-model.md).

## 1. Core Game Loop

1. Player triggers **Feed**.
2. The Flipper performs a **Sub-GHz scan / sweep** over a band preset.
3. The **single strongest signal** at that moment is captured. One feeding = one meal —
   no grazing.
4. The signal is analyzed and **displayed** ("what it was": frequency, modulation,
   device/protocol if identifiable, decoded payload if available).
5. A **nutrition label** (5 scores, §2) is computed and shown.
6. The pet **eats**; stats, evolution position, and dex update.

**Emergent design consequences of the "strongest signal only" rule (intended, keep them):**

- **Positioning is a skill.** To feed a rare/clean delicacy you must physically find a rare
  emitter and get close enough to make it the strongest signal.
- In dense urban RF the strongest ambient signal is usually common encrypted noise (junk),
  so good food *requires going out and exploring* — reinforcing the "carry it outside"
  core fantasy.

## 2. Signal Scoring — The Nutrition Label (5 axes)

Every captured signal gets a 5-value "nutrition label." This label is simultaneously the
**game stat source** and the **learning display**.

| # | Axis (label name) | Derived from | Teaches | Decode required? |
|---|---|---|---|---|
| 1 | **Calories** (Volume) | Data amount / burst length / bit count | Signal length, data-rate intuition | No |
| 2 | **Freshness** (Strength) | RSSI / proximity | dBm, propagation, distance, antennas | No |
| 3 | **Additives** (Entropy) | Shannon entropy of payload; high = encrypted/whitened = junk; low = structured/legible = wholesome | Detecting encryption *without* decrypting | No |
| 4 | **Rarity** | Band/protocol occurrence frequency, computed from the player's own accumulated dex ("personal rarity") | Band plan; what lives where | No |
| 5 | **Nourishment** (Structure) | How deeply it decoded: raw burst → known modulation → known protocol → actual values | Frame/protocol layering, decoding | **Yes** |

**Graceful degradation (a hard requirement):** 4 of the 5 axes need *no decoding* — they
work on raw captures. Only **Nourishment** requires a decoder. Unknown signals still receive
a full, scoreable label; the more you can decode, the richer the label becomes.
"Unknown × high-entropy × rare band" = a mysterious top-tier delicacy.

The exact numeric calibration of each axis is **deferred** until real RF distribution data is
collected (see [open-questions.md](open-questions.md)). The axis *definitions* above are fixed.

## 3. Evolution System

The pet **drifts fluidly on a 2D map** (reversible, not a one-way tree). As its diet changes,
its form slowly follows.

To avoid combinatorial explosion (5 axes × high/low = 32 forms), the **branch** is driven by
two derived dimensions; the remaining axes modulate *expression* rather than branch.

**Branch dimensions:**
- **Health axis (Wholesome ↔ Junk)** = f(Additives/entropy, Nourishment/structure)
- **Class axis (Common ↔ Exotic)** = f(Rarity)

**Forms (4 corners + center ≈ 5):**

| Position | Form (flavor) |
|---|---|
| Wholesome × Common | Humble, healthy field-type (raised on simple sensor data) |
| Wholesome × Exotic | **Gourmet** — rare + clean; hardest to achieve; apex candidate |
| Junk × Common | "City pigeon / rat" — ambient encrypted noise (lazy-play outcome) |
| Junk × Exotic | **Glitch-type** — rare encrypted diet; cool but unhealthy/cursed |
| Center (balanced) | Harmonized/generalist — hard, because the world pulls toward junk |

**Expression (non-branching):**
- **Calories (Volume)** → size / strength within the current branch.
- **Freshness (RSSI)** → daily mood / health.

**Position math:** the pet's map position is a **long-memory weighted moving average (EMA /
IIR filter) of recent diet**, with **novel/rare catches weighted much more heavily** than
repeats. This prevents the pet from being frozen in the "Junk × Common" corner by the ambient
junk floor, and couples *discovery* (collection) directly to *visible change* (evolution).
Conceptually, **the pet is an IIR filter over your foraging history.**

### Character types & growth (supersedes the 5-form taxonomy)

The EMA-of-diet engine above is kept, but the **visible form is now a 100-type morph** driven
by five pet stats (one per nutrition axis), experience, and levels. Full spec in
[pet-growth-spec.md](pet-growth-spec.md); locked-decision change in
[decision-log.md](decision-log.md) **D18**. In brief:

- **5 stats**, each an EMA of one axis (MASS←Calories, VIGOR←Freshness, WILD←Additives,
  AURA←Rarity, MIND←Nourishment).
- **EXP** per meal favors delicacies (legible/rare/decoded) and decays on repeats; cumulative
  EXP maps to a **level** (a maturity clock, *not* power — keeps collection the goal, D2).
- The **character type** (`family × partner × shape` = exactly **100**) is **re-derived from
  the current stats at each level-5 checkpoint**, so the morph stays **reversible** — a diet
  change re-speciates the pet. Max-stat magnitude and mood remain *expression*, not identity.

Pet graphics are **composed from layered 1-bit parts** (lineage base + body accent + head +
eyes), produced externally by `codex`, with shared egg / lineage-tinted child pre-morph stages
and the full morph revealed at Lv10 — not one sprite per type (D21); see
[pet-growth-spec.md](pet-growth-spec.md) §4-§5.

## 4. Collection & Dex (The Dex)

Two layers, separated by purpose and access pattern (schema in [data-model.md](data-model.md)):

- **Species layer (game — lightweight, hot state):** one record per device/protocol type,
  storing a **count** (+ first-seen / last-seen). Drives personal rarity, dex completion,
  and **repeat-decay** (the 500th identical junk catch feeds less → discourages grinding,
  reinforces novelty-seeking).
- **Capture-log layer (learning — rich, cold store):** one record per individual capture,
  with timestamp + actual decoded content + metadata (frequency, modulation, RSSI, entropy,
  scores, raw `.sub` reference).

**Learning payoff (why store decoded content + date):** the dex becomes **longitudinal**,
and longitudinal data is how you actually learn a protocol. Repeated captures of the same
device passively become **diffs** — the single most powerful RE technique, gathered for free:

- Static bytes → ID / address (also distinguishes *individuals* within a *species*).
- Incrementing bytes → counters / rolling codes.
- World-varying bytes → live sensor values.

Timestamps additionally reveal **temporal behavior** (periodicity, duty cycle).

**Upgradeable entries:** store the raw `.sub` for unknown/interesting captures. When better
decoders are written later, old captures **retroactively gain meaning** (today's "unknown
433 MHz blip" becomes next month's "thermometer"). Unknown signals are bucketed into
**provisional fingerprint-species** (frequency + modulation + length) that **graduate** to
named species once decoded.

**Privacy guardrail:** some payload values are persistent identifiers (TPMS IDs, key-fob IDs)
that can track a vehicle or person. Treat these as pseudo-PII: keep the dex local, and do
not surface them in a way that enables tracking.

## 5. Non-Functional Requirements

- **Deterministic core.** The Analysis/Scoring Core must be pure functions with no hardware,
  UI, or wall-clock dependence, so it is reproducible and host-testable.
- **On-device feasibility.** The Game Shell, dex, and UI must run within the Flipper's
  constraints (small RAM, 128×64 monochrome display, SD-card storage via FatFs).
- **Lossless capture.** Captures are stored losslessly so any future taxonomy/scoring can be
  re-derived. This is an invariant, not a feature toggle.
- **Designed to the real sensor.** All scoring and balance assume the CC1101's capabilities,
  not richer SDR captures. The gap between "rich truth" and "what Flipper sees" may itself
  become game flavor ("it looks hazy to your Radiotchi").

## 6. MVP (v0) Scope

A minimal but end-to-end vertical slice (detailed in [implementation-plan.md](implementation-plan.md)):

1. FAP boots, shows a stub pet on a ViewPort.
2. **Feed** triggers a Sub-GHz sweep over a small Japan-relevant band preset; captures the
   **strongest** signal.
3. Compute the **four decode-free axes** (Calories, Freshness, Additives/entropy,
   Rarity-from-dex); skip Nourishment initially.
4. Display the **nutrition label** + basic "what it was" (freq, modulation guess, RSSI).
5. Pet eats → simple stat update; append a **CaptureEvent** to the SD log; bump the
   **species count**.

## 7. Out of Scope (for v1)

- **Transmission of any kind** (TX-to-pay, replay, jamming, beacons that transmit). RX-only.
- **2.4 / 5 GHz** signals (CC1101 cannot see them).
- **Live P2P battles** and internet play — see [open-questions.md](open-questions.md) and the
  future-work notes in [architecture.md](architecture.md). Asynchronous "ghost battles" are a
  later candidate, not v1.
- **Multiple / re-raised pets** — one lifelong pet only.
