# Architecture

This document owns the **repository shape, module boundaries, data flow, and major technical
decisions**. Domain entities and schema live in [data-model.md](data-model.md); behavior lives
in [product-spec.md](product-spec.md).

## 1. Layered Architecture (enforced)

A clean three-layer split emerged repeatedly during design and **must** be enforced. Each
layer talks to the next only through a typed boundary, never by reaching across.

```
+-------------------------------------------------------------+
|  GAME SHELL  (on-device; deterministic; no RF needed)       |
|   - Pet state, growth (5 stats/EXP/level, 100-type morph)   |
|   - Dex (species counts + capture log)                    |
|   - UI / ViewPort, input handling                           |
|   - Storage (SD via FatFs, internal via LittleFS)          |
+----------------------------^--------------------------------+
                             | CaptureEvent records (typed)
+----------------------------+--------------------------------+
|  RF / ANALYSIS CORE  (portable C; pure; OFF-device testable)|
|   raw capture -> decode/classify -> feature-extract         |
|   -> 5-axis scoring. NO game/UI/hardware dependencies.      |
+----------------------------^--------------------------------+
                             | RawCapture (raw bits + freq + RSSI)
+----------------------------+--------------------------------+
|  CAPTURE SOURCE  (swappable)                                |
|   - Live: Flipper CC1101 (production)                       |
|   - Test: recorded .sub files / rtl_433 JSON / synthetic    |
+-------------------------------------------------------------+
```

### Layer responsibilities

- **Capture Source** — produces a `RawCapture` (raw bits/burst + frequency + RSSI + minimal
  metadata). It is *swappable*: a live adapter drives the CC1101; test adapters replay
  recorded `.sub` files, parse rtl_433 JSON, or emit synthetic captures. This is the only
  layer that knows about hardware or file formats.
- **RF / Analysis Core** — **pure functions** that take a `RawCapture` and produce a
  `CaptureEvent` (which embeds the 5-axis `Scores`). It decodes/classifies, extracts features,
  and scores. It has **no** dependency on `furi`, the GUI, storage, the clock, or the capture
  hardware. It compiles **both** on a host (for tests) and on the Flipper.
- **Game Shell** — consumes typed `CaptureEvent` records and owns everything stateful and
  device-facing: pet state, evolution position, dex, UI, input, and persistence. It is
  **data-driven** and can be developed against recorded/synthetic event streams, with live
  capture swapped in later.

### Boundary rules (non-negotiable)

- The Analysis Core never imports hardware, UI, storage, or clock APIs. If it needs "now",
  the timestamp is passed **in** as data.
- The Game Shell never decodes RF or computes scores itself; it only reads `CaptureEvent`.
- Rarity is *personal* and depends on dex state, which lives in the Game Shell. The Analysis
  Core therefore computes Rarity from a **dex view passed in as input** (e.g. a small
  rarity/occurrence table), keeping the core pure. See `score_capture` below.

## 2. Internal Contracts (the module "API")

There is no network API; the meaningful contracts are the **typed records** crossing layer
boundaries and the **pure-function signatures** of the Analysis Core. Concrete struct
definitions live in [data-model.md](data-model.md).

```c
// Capture Source -> Analysis Core
RawCapture capture_source_next(CaptureSource* src);   // live CC1101 or fixture replay

// Analysis Core (pure; host + Flipper)
CaptureEvent analyze_capture(const RawCapture* raw, const RarityView* dex_rarity,
                             uint64_t timestamp);     // raw -> CaptureEvent (incl. Scores)
Scores       score_capture(const CaptureEvent* ev,   // CaptureEvent -> 5-axis Scores
                           const RarityView* dex_rarity);

// Game Shell consumes CaptureEvent (no RF/scoring logic of its own)
void game_shell_feed(GameState* g, const CaptureEvent* ev);
```

- `analyze_capture` is the headline pure function: deterministic given its inputs. The
  timestamp and the rarity view are injected so the function stays pure.
- `score_capture` may be called standalone to re-grade an old `CaptureEvent` after a new
  decoder is added — this is how stored captures **retroactively gain meaning**.

## 3. Proposed Repository Layout

```
radiotchi/
  application.fam          # Flipper FAP manifest (APPID, name, entry point, stack)
  radiotchi_icon.png       # 10x10 1-bit app launcher icon referenced by application.fam
  src/
    game_shell/            # Flipper-only: pet, evolution, dex, UI/ViewPort, input, storage
    capture_source/        # adapters: live CC1101, .sub loader, rtl_433 JSON, synthetic
  lib/
    analysis_core/         # PORTABLE pure C: RawCapture -> CaptureEvent -> Scores (no HW/UI)
  test/                    # host test harness; builds lib/analysis_core off-device
  fixtures/                # .sub captures, rtl_433 JSON, synthetic capture fixtures
  docs/                    # this documentation pack
```

- `lib/analysis_core/` is deliberately separate from `src/` to signal portability: it must
  build with both the Flipper toolchain (via `application.fam`) and a plain host compiler
  (via `test/`).
- `capture_source/` is the seam that lets the same Game Shell + Analysis Core run against live
  RF or recorded fixtures.

## 4. RF Sensor Model & Constraints

The production sensor is the Flipper's **CC1101**, and the game **must** be designed around
*its* real capabilities — not around what rtl_433/HackRF can see.

- Narrowband transceiver, ~300–928 MHz; listens **one frequency at a time** (RSSI stepping /
  sweep). **Not** a wideband SDR; there is no waterfall snapshot.
- **No 2.4 / 5 GHz** (most modern Wi-Fi/BLE cameras/bugs are invisible).
- On-device decoding is limited to the **firmware's known Sub-GHz protocols** (not rtl_433's
  hundreds), which caps the **Nourishment** axis on-device.
- **RX-only by design.** TX in Japan is constrained by the Radio Act / 技適 (giteki) and
  region-locked bands. Avoid TX in the core game.

**Japan band awareness** (for presets & rarity priors):

- 426–430 MHz — 特定小電力 (specified low-power radio)
- 920 MHz — Wi-SUN (smart meters), IoT
- 315 MHz — TPMS / keyless (Japan)

The ambient spectrum is **saturated with legitimate emitters**, so the genuinely hard and
valuable problem is **classification** (expected vs. anomalous), not mere detection. This is
the intellectual core the game gamifies.

## 5. Storage Architecture

- **SD card (FatFs):** a species index + an append-only capture log + linked raw `.sub` files.
  This is the archival/learning store.
- **Internal flash (LittleFS):** small, fast state only. Bulk data lives on the SD card.
- Keep the game-side species table small and hot; treat the capture log as cold/archival.

Schema details and the lossless-capture invariant are in [data-model.md](data-model.md).

## 6. Technology & Build

- **Language:** C.
- **SDK:** Flipper firmware SDK — `furi` (core/OS), **GUI / ViewPort** (UI), **Sub-GHz
  subsystem** (capture/decode), **Storage** (SD via FatFs, internal via LittleFS).
- **Build:** **ufbt** (`pip install ufbt`; `ufbt create APPID=...`; `ufbt launch`).
- **Manifest:** `application.fam`.
- **Firmware target:** prefer **OFW** compatibility; CFW (Momentum / Unleashed) optional only
  if extended RX bands are wanted. Keep **RX-only**.
- **Analysis Core build:** portable C, no hardware/UI deps, compiled on host (for tests) and
  on the Flipper. Host harness in Python or C runs the core over `.sub` + rtl_433 JSON
  fixtures with unit tests (see [testing-strategy.md](testing-strategy.md)).

## 7. Future Work (not v1, kept off the critical path)

P2P battles need very little bandwidth (pet stats, RNG seed, turn actions), so even a slow
channel suffices. Candidate channels, in rough priority:

| Channel | Use case | Notes |
|---|---|---|
| **Infrared (IR)** | Local "point & link" | Best first choice: legal (no TX restriction), simple, low-bandwidth-OK, link-cable nostalgia |
| **Sub-GHz** | Wireless battle across a room | Thematic, but TX raises 技適 / region-band concerns |
| **GPIO UART "link cable"** | Wired, reliable | Great for development; needs a cable |
| **BLE** | Modern local P2P | STM32WB55 has BLE, but the stack is oriented to the mobile app; peer BLE is significant extra work |

Internet P2P requires an **ESP32 Wi-Fi devboard** or a **host bridge** (phone/PC), mapping
onto the author's prior **peersh** pattern (P2P signaling + NAT traversal) with the Flipper as
the endpoint. Recommended v1-adjacent starting point: **asynchronous "ghost battles"** —
exchange a pet snapshot (file / NFC tag) and battle it offline, sidestepping live-comms
complexity. Ordering and channel choice are tracked in [open-questions.md](open-questions.md).

## 8. Guardrails Recap (architecture-level)

- Keep the three layers strictly separated; the `CaptureEvent` and `RawCapture` boundaries are
  the only legal cross-layer dependencies.
- **RX-only.** No code path may transmit. No replay, jamming, or defeating others' systems.
- Persistent identifiers are pseudo-PII: keep the dex local and never surface them for
  tracking.
- Design to the CC1101, not to richer SDR captures.
