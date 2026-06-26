# Open Questions

Intentionally deferred decisions. Each has a **current default** so implementation can proceed
without blocking; the default is what an agent should use until the question is resolved. Many
are deliberately deferred **until real RF data is collected** (recon, Phase 0 of
[implementation-plan.md](implementation-plan.md)). Locked decisions live in
[decision-log.md](decision-log.md).

| # | Question | Current default (use this until resolved) | Resolve when |
|---|---|---|---|
| Q1 | **Species granularity** — is protocol/model the *species* and an in-payload ID the *individual*? | Yes: protocol/model = species, stable ID byte(s) = individual. Unknowns use provisional fingerprint-species `(frequency + modulation + length)`. | After recon shows the real distribution (Phase 2). |
| Q2 | **Evolution-map calibration** — exact Health/Class axis thresholds and EMA weighting of novel vs. repeat catches. | Defer numeric constants; implement the EMA structure (D7) with placeholder weights that keep novel ≫ repeat and avoid a frozen junk corner. | After recon yields real entropy/rarity distributions (Phase 2). |
| Q3 | **Score calibration** — numeric scaling of each of the 5 axes. | Use provisional scaling that produces a sensible spread on the fixture set; keep axis *definitions* fixed. This now also covers the **classifier thresholds** (D23): the entropy gates (whitened ≥ 6.0, structured < 2.5 bits/byte), the remote-band ranges, and the min-burst length that decide the RAW/MODULATION/PROTOCOL tier — plus the **FSK value-decoder gates** (D31): the bit-period glitch floor, the inter-frame gap mult/floor, the min-frame-bits, and the per-run bit cap in `radiotchi_fsk_sensor_decode`; and the **Manchester gates** (D36): the half-bit glitch floor, the inter-frame gap mult/floor, the per-run half-bit cap, and the min/max code bits in `radiotchi_manchester_decode`; and the **CRC-sensor gates** (D37): the accepted CRC-8 generator set (0x07/0x31/0x2F), the OOK ≥5-byte / FSK ≥4-byte length floors, and the `sensor-<mod>-<n>B-<crc>-<band>` species granularity in `decode_crc_sensor`; the **PPM gates** (D38): the short-gap glitch floor, the sync-gap mult/floor in `radiotchi_ppm_to_bytes`; and the **FSK noise floors** (D38): `WV_FSK_MIN_RUNS` and the all-same-frame rejection that the fuzz harness drove. They are placeholders pending the real fixture distribution. | Phase 2, from the fixture distribution. |
| Q4 | **Battle priorities** — which P2P channel first, and live vs. ghost-battle ordering. | IR first (legal, simple, low-bandwidth-OK); start with asynchronous "ghost battles" before any live comms. Off the v1 critical path. | When battles are scheduled (post-MVP). |
| Q5 | **Single vs. multiple pets** — one lifelong pet, or more? | Single, lifelong (D3). | If a strong reason to support multiple pets appears. |
| Q6 | **Project name** — keep the working title? **RESOLVED** | **Renamed to Radiotchi (ラジオっち)** on 2026-06-22: `radio` (the 電波/RF it forages) + `-tchi`, whose `-otchi` ending echoes Tamagotchi; reads cleanly in English and Japanese. Full rename applied across code (appid `radiotchi`, `RadiotchiApp`/`radiotchi_*`/`RADIOTCHI_*`), on-device paths (`/ext/apps_data/radiotchi`), the GitHub repo, and all docs. See [decision-log.md](decision-log.md) D20. | ✅ Resolved 2026-06-22 |
| Q7 | **Firmware target** — OFW only, or support CFW (Momentum/Unleashed) for extended RX bands? | Prefer OFW; treat CFW as optional and only if extended RX bands are wanted. Stay RX-only either way. | If a needed band is OFW-inaccessible. |
| Q8 | **Growth-layer calibration** (D18) — EXP curve (`EXP_BASE`, `EXP_K`, `EXP_DECAY_HALF`), the type-shape thresholds (`T_HI/T_LO/M_HI/M_LO`), the stat-EMA α, the life-stage gates (`PET_LEVEL_CHILD/ADULT`), and the eye/special-state set. | Use the placeholders in `src/game_shell/pet_growth.h` (`EXP_BASE=64`, `EXP_K=50`, `EXP_DECAY_HALF=8`, shape gaps 0.30/0.08/0.25/0.10, stat-EMA α in `pet_growth.h`, child@5 / adult@10). The **art approach is decided** (D22): one monolithic image-generated 64×64 creature per `family×shape` (25) with a 2-pose idle loop; mood baked in, 2nd-stat shown as text. They give a sensible spread and pacing on synthetic feeds; the type *structure* (100, `family·20+partner·5+shape`) is fixed. | After recon yields real axis distributions (Phase 2) and a feed-pacing playtest. |
| Q9 | **Care/mood calibration** (D33) — the hunger ramp (`PET_HUNGER_FULL_SECS`/`STARVE_SECS`), the neglect gate (`PET_NEGLECT_SECS`) and its exp ratio (`PET_NEGLECT_EXP_NUM/DEN`), and the delicacy-mood quality bar (`PET_DELICACY_QUALITY`). | Use the placeholders in `src/game_shell/pet_mood.h` (full@6 h, starve@48 h, neglect@24 h, neglect exp ×½, delicacy quality ≥70/100). Hunger/mood is a presentation + *soft* pressure layer that never touches the reversible identity, so wrong constants are cosmetic, not corrupting. | After a real-world wear-it-around playtest of feed cadence (Phase 2). |

## Notes

- **Decided & kept** (not open): encrypted = junk (common) / rare = delicacy. See
  [decision-log.md](decision-log.md) D1.
- **Quest / feedback calibration (D35)** — the achievement thresholds (`QUEST_*_N` in
  `pet_quests.h`: 10 meals, 5 species, streak 3/7) and the sound/vibro **cue set** in
  `pet_feedback.c` are PROVISIONAL like the care/mood constants (Q9 family); the *behaviour*
  (streaks count on-time meals within `PET_NEGLECT_SECS`, achievements latch once earned) is the
  contract. Tune after a feed-cadence playtest. Sound and vibration both **default OFF**.
- When resolving any question above, move the outcome into [decision-log.md](decision-log.md)
  and update the affected docs ([data-model.md](data-model.md),
  [product-spec.md](product-spec.md), [acceptance-criteria.md](acceptance-criteria.md)) in the
  same change.
