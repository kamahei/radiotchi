# Dev Setup

How to build, flash, and host-test Radiotchi. Matches the toolchain in
[architecture.md](architecture.md) §6.

## Prerequisites

- **Python 3.9+** (for `ufbt`).
- A **host C compiler** (`gcc`, `clang`, or MSVC) — only needed to run the
  Analysis Core host tests; not needed to build the FAP.
- A **Flipper Zero** on OFW to actually capture RF (the FAP builds without one).

## 1. Flipper build environment (ufbt)

`ufbt` is the per-app Flipper build tool. It downloads its own ARM toolchain and
firmware SDK, so it is the only thing you must install.

```sh
python -m venv .venv
# Windows:  .venv\Scripts\activate         POSIX:  source .venv/bin/activate
pip install ufbt
ufbt update            # fetch the firmware SDK (one-time, ~minutes)
```

Build / run from the repo root (the `application.fam` is auto-discovered):

```sh
ufbt              # build dist/radiotchi.fap
ufbt launch       # build, upload, and start on a connected Flipper
ufbt cli          # serial console (watch FURI_LOG output while scanning)
```

**Dev sprite gallery:** the default build is a release build (no "Debug" menu). To enable the
sprite-browser gallery, uncomment `cdefines=["RADIOTCHI_DEBUG=1"]` in `application.fam` and
rebuild; a "Debug" command appears on the Home menu.

> Windows note: `application.fam` is parsed as cp932 by fbt — keep it **ASCII-only**
> (no em-dashes / Japanese). Source `.c/.h` files are UTF-8 and may contain non-ASCII.

## 2. Host tests for the Analysis Core

The Analysis Core (`lib/analysis_core/`) is pure, libm-free C and builds off-device:

```sh
make -C test          # or:  make -C test CC=clang
```

This runs `test/test_analysis_core.c`: entropy bounds, axis behavior, graceful
degradation (unknown signal still yields a full 4-axis label), provisional
fingerprint-species bucketing, and the re-grade regression.

## 3. What the app does today (MVP game loop)

A pet that eats real RF. On the Flipper, launch **Radiotchi** (category *Sub-GHz*):

- **Home**: shows the pet (its form is an EMA of its RF diet) + `Dex:` (species
  seen) / `Fed:` counts. **OK** = Feed, **Right** = Dex, **Left** = Tuning.
- **Feed (OK)**: hops the frequency plan (315 / 426 / 429 / 433.92 / 434.42 / 868 /
  920 / 922 MHz by default — *not* fixed to 433), locks the **single strongest**
  signal, captures RAW, and previews **"what it was"** (freq / modulation / RSSI /
  protocol) then the **5-axis nutrition label** (Nourishment shows `-` until a
  decoder exists). **OK** = Eat, **Back** = discard.
- **Eat**: persists losslessly + bumps the species (Rarity is now personal) +
  evolves the pet, all timestamped from the RTC.
- **Dex (Right)**: species list -> per-species captures (timestamps) -> capture
  detail (entropy / tier / scores).
- **Tuning (Left)**: live RSSI threshold / margin (Up/Dn, L/R), persisted.

Files on the SD card (`/ext/apps_data/radiotchi/`): `captures/<stamp>_<freq>.sub`
(raw, lossless), `capture_log.csv` (analysis rows), `species.csv` (species index),
`growth.txt` (the pet: 5 stats + EXP/level/type + name), `tuning.txt`. All survive
reboot. Pull them over qFlipper / the `ufbt`/`storage.py` serial tools to inspect on a host.

## 4. Layout

```
application.fam          FAP manifest (ufbt)
radiotchi_icon.png       10x10 1-bit app launcher icon
src/game_shell/          Flipper-only: app entry, UI, storage (capture_store)
src/capture_source/      CC1101 adapter + frequency plan / hopping
lib/analysis_core/       portable pure C: types + analyze/score (host-testable)
test/                    host unit tests for the Analysis Core
fixtures/                .sub / rtl_433 JSON / synthetic captures
```
