# fixtures/

Curated capture fixtures for host-testing the Analysis Core (Phase 0 recon, see
[../docs/implementation-plan.md](../docs/implementation-plan.md)).

- `synthetic/` — hand-built edge cases (committed): structured OOK, max-entropy,
  empty/short. These pin axis boundaries and graceful degradation; no real-world
  identifiers, so they are safe to commit.
  - `structured_ook_433.sub` — two OOK PWM fixed-code frames; decodes to `0x294` / 12 bits.
  - `structured_fsk_868.sub` — two 2FSK PCM/NRZ sensor frames; decodes to bytes `AA C5 3D`
    (exercises `radiotchi_fsk_sensor_decode` → TIER_VALUES, D31). Synthesized, no real id.
- `real/` — real Flipper `Read RAW` captures kept as regression fixtures. `noise_868.sub`
  is an ambient capture with **no transmitter present**; `test_real_noise_fixture` asserts the
  OOK decoder does **not** fake a VALUES code from it (the false-positive that on-device data
  revealed; see decision-log D24). Reviewed: pure noise, no decodable identifier.
- `sub/` — real Flipper `Read RAW` captures (`.sub`). **The ground truth** the game
  is calibrated to. Add your own from recon.
- `rtl_433/` — rtl_433 JSON for schema reference and learning breadth. Useful, but
  do **not** over-design scoring around richness the CC1101 cannot reproduce.

## Privacy

Real captures can contain persistent identifiers (TPMS / key-fob IDs). Per the
privacy guardrail, **do not commit personal real-world captures** — `.gitignore`
keeps `*.sub` out of git except under `fixtures/`. Only commit fixtures you have
deliberately reviewed and that contain no trackable identifiers.
