#!/usr/bin/env python3
"""Convert an rtl_433 `.cu8` (uint8 IQ) capture to a Flipper `.sub` RAW file (OOK or 2FSK).

This bridges the rtl_433_tests corpus (real device captures of Acurite / Nexus / Fine Offset /
LaCrosse / ...) into the Flipper RAW pulse format, so the pure-core decoders can be validated
against REAL signals — without hardware and without the rtl_433 binary. Pair it with
tools/sub_analyze.c.

The `.cu8` is interleaved unsigned-8-bit I,Q centered at ~128. Two demod modes:

  ook  — amplitude (ASK): |z|^2 = (I-128)^2+(Q-128)^2, threshold at a fraction of the peak, then
         run-length encode above/below into +mark/-space us. For Acurite/Nexus/etc.

  fsk  — frequency (2FSK): an FM discriminator d = Im(z[i] * conj(z[i-1])) = Q[i]*I[i-1]-I[i]*Q[i-1]
         whose sign tracks which tone is on. Gate on amplitude (no carrier -> forced low) so the
         inter-frame silence becomes one long space, then run-length encode the tone sign. This is
         what the Flipper's CC1101 2FSK async slicer produces, so the NRZ/sync-word FSK decoders
         (LaCrosse-TX29, Fine Offset) can be exercised. `--invert` swaps the mark/space tone sense
         (the deviation-sign polarity is radio-dependent; flip if the sync word reads inverted).

Sample rate defaults to 250 ksps; pass the real rate if the filename encodes one (e.g. `868.2M_1000k`).

Usage: python tools/rtl433_cu8_to_sub.py IN.cu8 OUT.sub [freq_hz] [samp_rate] [thresh_frac] [ook|fsk] [--invert]
NOTE: downloaded/converted captures are real device data — keep them under captures/ (gitignored);
never commit them (privacy A5).
"""
import sys


def _clamp(v):
    return 32767 if v > 32767 else (-32768 if v < -32768 else v)


def _rle(levels, us_per):
    """Run-length encode a list of booleans (True=mark) into signed +mark/-space microseconds."""
    pulses = []
    cur = levels[0]
    run = 1
    for s in levels[1:]:
        if s == cur:
            run += 1
        else:
            d = int(round(run * us_per))
            pulses.append(_clamp(d if cur else -d))
            cur, run = s, 1
    d = int(round(run * us_per))
    pulses.append(_clamp(d if cur else -d))
    return pulses


def cu8_to_sub(path_in, path_out, freq=433920000, rate=250000, thr_frac=0.4, mode="ook", invert=False):
    with open(path_in, "rb") as fh:
        data = fh.read()
    n = len(data) // 2
    if n == 0:
        print("empty input")
        return 0
    us_per = 1_000_000.0 / rate

    mag2 = [0] * n
    peak = 0
    for i in range(n):
        di = data[2 * i] - 128
        dq = data[2 * i + 1] - 128
        m = di * di + dq * dq
        mag2[i] = m
        if m > peak:
            peak = m
    if peak == 0:
        print("no signal")
        return 0

    thr = peak * (thr_frac * thr_frac)  # compare squared magnitude to (frac*peak_amplitude)^2

    if mode == "fsk":
        # Smooth |z|^2 with a short boxcar before gating: raw IQ noise throws scattered single-sample
        # spikes above a fractional threshold (a saturated peak makes thr high), which would fragment
        # the carrier-off silence into thousands of noise pulses. Averaging over ~half a bit keeps the
        # sustained carrier above thr while pushing isolated spikes below it, isolating the real burst.
        w = 6
        sm = [0] * n
        run = sum(mag2[:w])
        for i in range(n):
            if i + w <= n:
                sm[i] = run // w
                run += (mag2[i + w] if i + w < n else 0) - mag2[i]
            else:
                sm[i] = mag2[i]
        # FM discriminator: sign of Im(z[i] * conj(z[i-1])) tracks the tone; gate on the smoothed
        # amplitude so the carrier-off gaps read as one long space (mark, otherwise).
        levels = [False] * n
        for i in range(1, n):
            if sm[i] <= thr:
                continue  # no carrier -> space (the inter-frame gap)
            ii, qi = data[2 * i] - 128, data[2 * i + 1] - 128
            ip, qp = data[2 * i - 2] - 128, data[2 * i - 1] - 128
            disc = qi * ip - ii * qp  # Im(z[i] * conj(z[i-1]))
            hi = disc > 0
            levels[i] = (not hi) if invert else hi
        preset = "FuriHalSubGhzPreset2FSKDev476Async"
    else:
        levels = [mag2[i] > thr for i in range(n)]
        preset = "FuriHalSubGhzPresetOok650Async"

    pulses = _rle(levels, us_per)

    with open(path_out, "w", newline="\n") as fh:
        fh.write("Filetype: Flipper SubGhz RAW File\nVersion: 1\n")
        fh.write("Frequency: %d\nPreset: %s\nProtocol: RAW\n" % (freq, preset))
        for i in range(0, len(pulses), 512):
            fh.write("RAW_Data: " + " ".join(str(p) for p in pulses[i:i + 512]) + "\n")
    print(
        "wrote %s: %d IQ samples @%dksps %s -> %d pulses"
        % (path_out, n, rate // 1000, mode, len(pulses)))
    return len(pulses)


if __name__ == "__main__":
    a = [x for x in sys.argv if x != "--invert"]
    invert = "--invert" in sys.argv
    if len(a) < 3:
        print(
            "usage: rtl433_cu8_to_sub.py IN.cu8 OUT.sub [freq_hz] [samp_rate]"
            " [thresh_frac] [ook|fsk] [--invert]")
        sys.exit(2)
    cu8_to_sub(
        a[1], a[2],
        int(a[3]) if len(a) > 3 else 433920000,
        int(a[4]) if len(a) > 4 else 250000,
        float(a[5]) if len(a) > 5 else 0.4,
        a[6] if len(a) > 6 else "ook",
        invert)
