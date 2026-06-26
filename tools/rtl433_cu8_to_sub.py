#!/usr/bin/env python3
"""Convert an rtl_433 `.cu8` (uint8 IQ) **OOK** capture to a Flipper `.sub` RAW file.

This bridges the rtl_433_tests corpus (real device captures of Acurite / Nexus / Fine Offset / ...)
into the Flipper RAW pulse format, so the pure-core OOK decoders can be validated against REAL
signals — without hardware and without the rtl_433 binary (OOK only: magnitude threshold + run-length,
no FM/FSK demod). Pair it with tools/sub_analyze.c.

The `.cu8` is interleaved unsigned-8-bit I,Q centered at ~128. We compute |z|^2 = (I-128)^2+(Q-128)^2,
threshold at a fraction of the peak, and run-length encode above/below runs into signed +mark/-space
microseconds. Sample rate defaults to 250 ksps (the rtl_433 historical default for the old
`gfileNNN.cu8` captures); pass the real rate if the filename encodes one (e.g. `_1000k_`).

Usage: python tools/rtl433_cu8_to_sub.py IN.cu8 OUT.sub [freq_hz] [samp_rate] [thresh_frac]
NOTE: downloaded/converted captures are real device data — keep them under captures/ (gitignored);
never commit them (privacy A5).
"""
import sys


def cu8_to_sub(path_in, path_out, freq=433920000, rate=250000, thr_frac=0.4):
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

    def clamp(v):
        return 32767 if v > 32767 else (-32768 if v < -32768 else v)

    pulses = []
    cur = mag2[0] > thr
    run = 1
    for i in range(1, n):
        s = mag2[i] > thr
        if s == cur:
            run += 1
        else:
            d = int(round(run * us_per))
            pulses.append(clamp(d if cur else -d))
            cur, run = s, 1
    d = int(round(run * us_per))
    pulses.append(clamp(d if cur else -d))

    with open(path_out, "w", newline="\n") as fh:
        fh.write("Filetype: Flipper SubGhz RAW File\nVersion: 1\n")
        fh.write("Frequency: %d\nPreset: FuriHalSubGhzPresetOok650Async\nProtocol: RAW\n" % freq)
        for i in range(0, len(pulses), 512):
            fh.write("RAW_Data: " + " ".join(str(p) for p in pulses[i:i + 512]) + "\n")
    print("wrote %s: %d IQ samples @%dksps -> %d pulses" % (path_out, n, rate // 1000, len(pulses)))
    return len(pulses)


if __name__ == "__main__":
    a = sys.argv
    if len(a) < 3:
        print("usage: rtl433_cu8_to_sub.py IN.cu8 OUT.sub [freq_hz] [samp_rate] [thresh_frac]")
        sys.exit(2)
    cu8_to_sub(
        a[1], a[2],
        int(a[3]) if len(a) > 3 else 433920000,
        int(a[4]) if len(a) > 4 else 250000,
        float(a[5]) if len(a) > 5 else 0.4)
