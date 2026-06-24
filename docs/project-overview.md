# Project Overview

## Problem & Vision

Radiotchi is a Tamagotchi-style virtual pet that lives on a **Flipper Zero**. You carry it
into the real world and **feed it by scanning the radio spectrum**. Each "meal" is a real
Sub-GHz signal captured from the environment, and the pet's growth, mood, and evolution are
driven by the **analyzed content** of the signals it eats. Every catch is recorded in a
**dex (dex)** that doubles as a personal RF field guide.

The fantasy is simple: *a creature of the airwaves that grows into a mirror of where you
have been and what you have caught.*

## Why This Project Exists (read this first)

Radiotchi is a **learning / research vehicle**, not a utility or a market product. The author
is an application-layer game engineer who wants to **descend gradually into the low-level RF
layers** (modulation, encoding, framing, protocols) through play. Educational value and the
novelty of the concept are the point.

A direct consequence, which shapes every decision in this repository:

> "This already exists" and "a commercial product does it better" are **not** disqualifiers.
> Re-implementing known things is acceptable and often desirable, because the value is in
> *understanding* and in the *novel fusion* (RF-content-as-nutrition + a learning dex).

## Positioning & Novelty

Radiotchi is best summarized as **"the dolphin done deep"**: it cares about *what* you caught,
not merely *that* you used the radio.

| Existing thing | How its pet grows | What's missing vs. Radiotchi |
|---|---|---|
| Flipper built-in **dolphin** | From *activity* — that you used a tool | No analysis of *what* was received; no scoring; no dex |
| **Matagotchi** (community FAP) | From time + generic feeding | No RF input at all |
| **Tamagotchi P1 emulator** | Runs the original ROM | Unrelated |

No existing Flipper app drives a pet from the **analyzed content of received RF** or keeps a
learning dex of decoded captures. This sits squarely in the device's DNA while occupying
empty design space.

## Design Pillars

- **Encrypted data = junk** (it is common). **Rare communication data = delicacy** (珍味).
- **Collection is the main pillar.** Evolution is a *reflection / byproduct* of collection,
  not a target the player optimizes toward. The ambient RF environment is uncontrollable, so
  making evolution a goal would only frustrate.
- **One lifelong pet.** No repeated re-raising. The pet is a persistent companion that
  mirrors the player's RF journey over time.
- **Learning is first-class.** The dex is a genuine, longitudinal RF field guide.

## Goals & Success Signals

- A working **end-to-end vertical slice** on real hardware: capture → score → display →
  feed → persist (see [implementation-plan.md](implementation-plan.md) and
  [acceptance-criteria.md](acceptance-criteria.md)).
- A **deterministic, host-testable Analysis Core** that turns raw captures into a 5-axis
  nutrition label without any hardware or UI dependency.
- A **longitudinal dex** that captures losslessly, so that taxonomy and scoring can be
  re-derived retroactively as better decoders are written.
- Demonstrable **RF learning**: the player (and the author) understand modulation, framing,
  band plans, entropy/encryption, and diff-based reverse engineering better through play.

## Non-Goals & Guardrails (do not violate)

- **Not** a utility or market product; learning-first. Re-implementing existing ideas is fine.
- **RX-only.** No transmission-to-pay, no replay attacks, no jamming, no defeating others'
  systems.
- Do **not** store or surface trackable persistent identifiers (e.g. TPMS IDs, key-fob IDs)
  in a way that enables surveillance.
- Design to the **Flipper CC1101's real capabilities**, not to richer external SDR captures.

See [decision-log.md](decision-log.md) for locked decisions and [open-questions.md](open-questions.md)
for intentionally deferred ones.

## Stakeholders

- **Author / sole developer** — application-layer game engineer, primary user and learner.
- **Implementation agents** (human or AI) — should read [../AGENTS.md](../AGENTS.md) and the
  [docs index](README.md) before changing code.

## Constraints Summary

- **Platform:** Flipper Zero, custom application (FAP), language **C**.
- **Sensor:** narrowband **CC1101** (~300–928 MHz, one frequency at a time, no 2.4/5 GHz).
- **Legal:** RX-only by design; TX in Japan is constrained by the Radio Act / 技適 (giteki).
- **Status:** design locked through the conceptual phase; several balance parameters are
  intentionally deferred until real RF data is collected.
