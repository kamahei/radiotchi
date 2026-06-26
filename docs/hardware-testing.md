# Hardware capture & decoder validation guide

How to take real RF captures and turn them into confirmed, named decoders. The decode toolkit is
in place (see [decision-log.md](decision-log.md) D36–D38); what remains is **confirming on real
signals**, which is what this guide streamlines. Everything here is **RX-only** (D6).

## 1. Capture real signals

On the Flipper, capture target devices to `.sub` files (the app's Feed is RX-only; Flipper's
stock **Sub-GHz → Read RAW** also works and lets you pick the frequency/preset):

- **Weather / temp-humidity sensors** — 433.92 MHz (OOK: Acurite, Nexus/clones, LaCrosse TX) and
  868.3 MHz (FSK: Fine Offset / Ecowitt, LaCrosse). Press nothing — they beacon periodically.
- **TPMS** — 315 / 433 MHz (drive/roll the wheel or use a TPMS trigger tool).
- **Remotes / keyfobs / gates** — 315 / 433 MHz; capture a few **button presses of the same
  device** (repeats are what the decoders confirm against).

Capture **2–3 transmissions per device** so the repeat-confirm guards have material. Note the
device, band, and preset. Pull the `.sub` files off the SD card (qFlipper) into a local working
dir — keep them under `captures/` (gitignored; **never commit real captures** — A5).

## 2. Analyze offline (no reflashing)

```sh
make -C test sub_analyze
test/build/sub_analyze captures/**/*.sub
```

Each capture prints the **VERDICT** (the pure-core decode dispatch result: tier / protocol /
species / hashed individual) plus **per-slicer diagnostics** — what `pwm_to_bytes`,
`ppm_to_bytes`, `manchester_to_bytes`, `fsk_sensor`, and `repeating_frame` each recover, with bit
counts. This is how you see *why* something did or didn't decode:

- A slicer shows **clean, stable bytes that repeat** across captures → a real frame; read its layout.
- `(no demodulator produced a frame)` → noise, wrong preset, or an encoding not yet covered.

Caveats: the analyzer runs the **pure-core decoders only** — the on-device firmware Sub-GHz
registry (Princeton/KeeLoq/CAME/…) is additional. It decodes the **first 256 pulses** (matching the
on-device `RawCapture` / re-grade limit), and flags captures longer than that.

## 3. Turn a confirmed capture into a named decoder

When a capture decodes to a recognizable, repeating, byte-aligned frame:

1. **Identify the layout** from the slicer bytes + the public protocol spec (length, which bytes
   are the id / value / CRC, the CRC generator, any constant marker fields).
2. **Add a named decoder** next to `decode_acurite606` / `decode_nexus_th` in
   `lib/analysis_core/analysis_core.c`, gated on a **documented invariant** (exact length + CRC over
   the documented region, and/or a constant field, and/or band) so a non-matching frame falls
   through safely. Wire it into `radiotchi_decode_from_pulses` among the specific decoders. Species
   stays **family/brand-level**; the device id goes only into the hashed `individual` tag (A5), and
   field values (temperature, pressure) are **never surfaced**.
3. **Add a fixture + test**: encode the confirmed layout in `tools/gen_sub_fixtures.py`, regenerate
   the committed synthetic `.sub`, and add it to `test_named_sub_fixtures` plus an in-memory
   `test_named_sensors` case (build the frame from the spec, assert the species). Synthetic only —
   built from the layout, not the recorded capture.
4. **Verify**: `make -C test` (must stay green, including the random-pulse `test_fuzz_noise`) and
   `python -m ufbt` (FAP compiles). The fuzz is the safety net — a new decoder must not introduce
   false VALUES on noise.

## 4. Guardrails (always)

- **RX-only** (D6) — no TX path, ever.
- **Privacy (A5/D1)** — species are family/brand-level; serials live only in the one-way `id-XXXX`
  tag; encrypted/rolling codes stay recognition-only; field values are not surfaced.
- **Reimplement from public specs** — no third-party decoder source copied; the project stays MIT.
- Thresholds are provisional ([open-questions.md](open-questions.md) Q3) — tune against the real
  fixture distribution as captures accumulate.
