#!/usr/bin/env python3
"""Generate Flipper `.sub` (SubGhz RAW) fixtures for the host decoder tests.

These are SYNTHETIC frames built from public protocol layouts — no recorded device data — so they
are safe to commit (see fixtures/README.md). The pulse encodings mirror the C test builders in
test/test_analysis_core.c exactly (build_pwm_bytes_frame / build_ppm_frame / build_fsk_frame), so a
fixture that the decoder accepts here is the same waveform the unit tests use, now exercised through
the real on-disk parse path (radiotchi_parse_raw_data).

Pulses are signed microseconds: + = mark (RF on), - = space (gap). Each protocol emits the frame
twice (real devices retransmit; the decoders require a confirming repeat).

Run from the repo root:  python tools/gen_sub_fixtures.py
"""

import os

HEADER = (
    "Filetype: Flipper SubGhz RAW File\n"
    "Version: 1\n"
    "Frequency: {freq}\n"
    "Preset: {preset}\n"
    "Protocol: RAW\n"
)


def crc8(data, poly=0x07, init=0x00):
    """MSB-first CRC-8 (matches radiotchi_crc8)."""
    crc = init
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ poly) & 0xFF if (crc & 0x80) else (crc << 1) & 0xFF
    return crc


def bits_msb(data, nbits):
    """Yield the first `nbits` bits of `data` (bytes), MSB-first."""
    for i in range(nbits):
        yield (data[i >> 3] >> (7 - (i & 7))) & 1


def pwm_bytes_frame(data, nbits, short=350, long=1050, gap=10500):
    """Mark-coded OOK PWM: bit 1 = long mark + short space, bit 0 = short mark + long space;
    the last bit's space is the sync gap. Mirrors build_pwm_bytes_frame."""
    out = []
    bs = list(bits_msb(data, nbits))
    for i, bit in enumerate(bs):
        mark = long if bit else short
        space = gap if i == nbits - 1 else (short if bit else long)
        out += [mark, -space]
    return out


def ppm_frame(data, nbits, pulse=500, s0=1000, s1=2000, gap=4000):
    """Space-coded OOK PPM: fixed mark then a gap (short=0, long=1); the last bit's gap is the sync.
    Mirrors build_ppm_frame (the last data bit should be 1 for a clean round-trip)."""
    out = []
    bs = list(bits_msb(data, nbits))
    for i, bit in enumerate(bs):
        space = gap if i == nbits - 1 else (s1 if bit else s0)
        out += [pulse, -space]
    return out


def fsk_nrz_frame(data, nbits, period=100, gap=8000):
    """2FSK PCM/NRZ run-length: coalesce equal consecutive bits into one run of count*period
    (sign + for 1, - for 0), then a long inter-frame gap. Mirrors build_fsk_frame."""
    out = []
    bs = list(bits_msb(data, nbits))
    run, cur = 0, None
    for bit in bs:
        if bit == cur:
            run += 1
        else:
            if cur is not None:
                out.append((1 if cur else -1) * run * period)
            cur, run = bit, 1
    if cur is not None:
        out.append((1 if cur else -1) * run * period)
    out.append(-gap)
    return out


def write_sub(path, freq, preset, frames):
    lines = HEADER.format(freq=freq, preset=preset)
    for pulses in frames:
        lines += "RAW_Data: " + " ".join(str(p) for p in pulses) + "\n"
    with open(path, "w", newline="\n") as f:
        f.write(lines)
    print("wrote", path, "(%d frames)" % len(frames))


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    out_dir = os.path.normpath(os.path.join(here, "..", "fixtures", "synthetic"))
    os.makedirs(out_dir, exist_ok=True)
    ook = "FuriHalSubGhzPresetOok650Async"
    fsk = "FuriHalSubGhzPreset2FSKDev476Async"

    # Acurite-606TX: 32-bit OOK PWM, byte 3 = CRC-8/0x07 over bytes 0..2 -> weather-acurite-433.
    acu = bytes([0x3A, 0x21, 0x84, 0x00])
    acu = bytes(acu[:3]) + bytes([crc8(acu[:3])])
    f = pwm_bytes_frame(acu, 32)
    write_sub(os.path.join(out_dir, "acurite_606_433.sub"), 433920000, ook, [f, f])

    # Nexus-TH: 36-bit OOK PPM, const 0xF nibble at bits 24..27, humidity 0x37 -> th-nexus-433.
    nx = bytes([0x3A, 0x11, 0x23, 0xF3, 0x70])
    f = ppm_frame(nx, 36)
    write_sub(os.path.join(out_dir, "nexus_th_433.sub"), 433920000, ook, [f, f])

    # Generic CRC FSK sensor: 5 bytes, byte 4 = CRC-8/0x31 over bytes 0..3 -> sensor-fsk-5B-c31-868.
    s = bytes([0xA1, 0xB2, 0xC3, 0xD4, 0x00])
    s = bytes(s[:4]) + bytes([crc8(s[:4], poly=0x31)])
    f = fsk_nrz_frame(s, 40)
    write_sub(os.path.join(out_dir, "sensor_crc_868.sub"), 868350000, fsk, [f, f])

    # Preamble + 0x2DD4 sync FSK sensor (Fine Offset/Ecowitt-class): 0xAA 0xAA 0x2D 0xD4 + 3 data +
    # CRC-8/0x31 over the data -> sensor-2dd4-4B-c31-868.
    sd = bytes([0x11, 0x22, 0x33])
    sframe = bytes([0xAA, 0xAA, 0x2D, 0xD4]) + sd + bytes([crc8(sd, poly=0x31)])
    f = fsk_nrz_frame(sframe, len(sframe) * 8)
    write_sub(os.path.join(out_dir, "sensor_2dd4_868.sub"), 868350000, fsk, [f, f])


if __name__ == "__main__":
    main()
