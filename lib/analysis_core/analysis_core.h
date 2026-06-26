// Radiotchi — Analysis Core public API (pure functions; host + Flipper).
//
// These functions are deterministic given their inputs: no globals, no clock, no
// I/O. The timestamp and the dex rarity view are injected as data so the core
// stays pure (docs/architecture.md §2).

#pragma once

#include "radiotchi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Shannon entropy of a byte buffer, in bits/byte (0.0 .. 8.0). High => looks
// encrypted/whitened (junk); low => structured/legible. Empty buffer => 0.
float radiotchi_shannon_entropy(const uint8_t* data, size_t len);

// Provisional fingerprint-species id, bucketing an unknown signal by
// (frequency band + modulation + length). Two similar unknowns share a bucket;
// a later decoder graduates them to a named species. Writes a NUL-terminated id
// of at most RADIOTCHI_SPECIES_LEN bytes into `out`.
void radiotchi_fingerprint_species(
    uint32_t frequency_hz,
    Modulation modulation,
    uint16_t bit_count,
    char* out,
    size_t out_len);

// The headline pure function: raw observation -> learning record.
// Fills the decode-free feature fields (entropy, bit_count, fingerprint species)
// and the 5-axis label. Nourishment defaults to TIER_RAW when no decoder applies,
// so an unknown signal still yields a full, scoreable label (graceful degradation).
//
// `dex_rarity` may be NULL (treated as an unseen, maximally-rare signal).
// `timestamp` is injected (epoch/RTC seconds).
CaptureEvent analyze_capture(
    const RawCapture* raw,
    const RarityView* dex_rarity,
    uint64_t timestamp);

// Re-score an existing CaptureEvent (e.g. after a new decoder raised its tier, or
// the dex rarity shifted) WITHOUT re-decoding. This is how stored captures
// retroactively gain meaning. Pure.
Scores score_capture(const CaptureEvent* ev, const RarityView* dex_rarity);

// Classify the decode depth of an observation from its boundary features (band,
// modulation, structure via `entropy`, burst length). Returns the achieved
// DecodeTier and, at TIER_PROTOCOL+, names the protocol and a graduated species.
// Provisional / signature-based: it does NOT re-demodulate the lossless `.sub` (the
// in-struct payload is only a feature proxy), so it classifies the *family*, not exact
// values. `protocol_out` / `species_out` may be NULL if not wanted. Pure.
DecodeTier radiotchi_classify(
    uint32_t frequency_hz,
    Modulation modulation,
    uint16_t bit_count,
    float entropy,
    char* protocol_out,
    size_t protocol_len,
    char* species_out,
    size_t species_len);

// Map a firmware-decoded protocol NAME to a branded, family-level species id (`<brand>-<band>`,
// e.g. "keyfob-starline-433", "gate-came-433"). Many Sub-GHz remotes the firmware decodes carry a
// recognizable manufacturer/system brand in their protocol name (gate, garage, and car-alarm
// makers); graduating those to a maker-named family makes the dex read by make rather than by
// chip/cipher name. Matching is case-insensitive substring, so it tolerates minor naming
// differences across firmwares; an UNRECOGNIZED protocol keeps its own name as the species
// (unchanged behaviour). Writes a NUL-terminated species into `out`. Pure.
//
// Privacy (A5): this is a FAMILY/brand label, NEVER the per-device serial — the decoded id lives
// only in the one-way `individual` tag. The brand is a make, not an individual vehicle/device id.
void radiotchi_species_for_protocol(
    const char* protocol, uint32_t frequency_hz, char* out, size_t out_len);

// Privacy-safe individual fingerprint of a decoded device code: a short ONE-WAY hash
// ("id-XXXX") of (code, bit-width). Repeated captures of the SAME device share a stable
// tag for local longitudinal learning (which device, how often, over time), but the raw
// persistent identifier cannot be recovered from it — so the dex never enables tracking a
// vehicle/person in a usable form (A5/D6). Writes a NUL-terminated tag into `out`. Pure.
void radiotchi_individual_fingerprint(uint32_t code, uint8_t nbits, char* out, size_t out_len);

// The Nourishment axis value for a decode tier (the ladder: RAW 0, MODULATION, PROTOCOL,
// VALUES 1.0). Exposed so a re-grade that re-reads the `.sub` can set Nourishment without
// re-running the full scorer (which would disturb the other axes). Pure.
float radiotchi_tier_nourishment(DecodeTier tier);

// Parse a Flipper `.sub` "RAW_Data:" line, appending its signed pulse durations
// (+mark/-space µs, clamped to int16) into out[have..cap). Lines that are not RAW_Data
// (headers etc.) contribute nothing. Returns the new pulse count. Pure — lets a re-grade
// re-read the lossless `.sub` timing and reach TIER_VALUES retroactively.
uint16_t radiotchi_parse_raw_data(const char* line, int16_t* out, uint16_t cap, uint16_t have);

// Demodulate an OOK PWM fixed-code frame (the EV1527/PT2262 remote family) from a
// signed pulse train (+mark / -space µs). Each bit is set by its MARK width (a long mark
// is 1, a short mark 0, split at ~1.5x the short unit — robust even for the last bit,
// whose space is the sync gap); frames are delimited by that long sync gap. On a
// self-consistent frame of 8..32 bits that is CONFIRMED by an immediately-following repeat
// with the same bits (real remotes retransmit; a lone/unconfirmed frame is rejected so
// ambient noise cannot fake a code) — returns true and writes the code and bit length. Real
// demodulation behind TIER_VALUES (vs. the signature-only classifier). Pure.
//
// Note (privacy, A5): the decoded `code` proves we can read the values (raising the
// tier); callers MUST NOT surface it as a trackable per-device identifier — the dex
// species stays at the family/protocol granularity.
bool radiotchi_ook_pwm_decode(const int16_t* pulses, uint16_t n, uint32_t* code, uint8_t* nbits);

// Demodulate an OOK Manchester fixed-code frame (the class of remote that encodes each bit as a
// mid-bit level transition rather than by mark width — many 315/433 MHz gate & keyfob remotes).
// From the signed pulse train: estimate the HALF-bit unit as the glitch-filtered robust-minimum
// run, expand each run to round(dur/half-unit) half-bit samples, then pair samples into bits by
// the phase whose every pair is a transition (G.E. Thomas: low->high = 1, high->low = 0); frames
// are split at the long inter-frame gap. As with the PWM/FSK decoders a frame is trusted only when
// an immediately-following repeat decodes to the SAME bits (real remotes retransmit; noise does
// not), so noise cannot fake a code. On success writes the code and bit length. This is the real
// bit-level path for Manchester remotes (vs. the coarse, id-less waveform fingerprint of
// radiotchi_repeating_frame). Pure.
//
// Note (privacy, A5): the decoded `code` only raises the tier and seeds a one-way per-device tag;
// callers MUST NOT surface it as a trackable identifier — the dex species stays family-level.
bool radiotchi_manchester_decode(const int16_t* pulses, uint16_t n, uint32_t* code, uint8_t* nbits);

// Demodulate a structured 2FSK sensor frame (the weather/telemetry/TPMS PCM/NRZ class)
// from a signed pulse train (+mark / -space µs, the firmware's async-RAW slicer output).
// The bit period is the robust-minimum run; each run expands to round(dur/period) NRZ bits
// (mark=1, space=0); frames are split at the long inter-frame gap. As with the OOK decoder,
// a frame is trusted only when an immediately-following repeat packs the SAME bytes (real
// sensors retransmit; ambient noise does not) — so noise cannot fake a frame. On success
// writes the packed frame bytes (MSB-first, up to `cap`) and their byte length. Scoped to
// the PCM/NRZ-with-repeat subclass; other FSK framings fall back to the signature PROTOCOL
// tier (never a regression). Pure.
//
// Note (privacy, A5): the frame proves we read the values (raising the tier); callers MUST
// NOT surface its bytes as a trackable per-device identifier — the dex species stays at the
// family granularity; only the one-way hashed tag below crosses to the dex.
bool radiotchi_fsk_sensor_decode(
    const int16_t* pulses, uint16_t n, uint8_t* frame, uint16_t cap, uint16_t* frame_len);

// Privacy-safe individual fingerprint of a multi-byte decoded frame: a short ONE-WAY hash
// ("id-XXXX") of (bytes, length). Same role/format as radiotchi_individual_fingerprint but
// for the byte-frame decoders (FSK): repeated captures of the same device share a stable tag
// for local longitudinal learning, while the raw frame cannot be recovered from it (A5).
// Writes a NUL-terminated tag into `out`. Pure.
void radiotchi_individual_fingerprint_bytes(
    const uint8_t* frame, uint16_t len, char* out, size_t out_len);

// Align N decoded payloads byte-by-byte and classify each position as STATIC (an id/fixed
// field), INCREMENTING (a rolling counter), VARYING (a sensor value) or ABSENT (beyond the
// shortest frame). Deterministic, integer-only; the foundation of the diff-learning dex view.
// `payloads[i]` has `lens[i]` bytes. With `count` < 2 the result has width 0. Pure — the
// caller renders only the resulting class markers, never the raw bytes (A5).
ByteDiff radiotchi_byte_diff(const uint8_t* const* payloads, const uint16_t* lens, uint8_t count);

// Select, from `count` parallel (tag, payload) rows, the subset whose device tag matches
// `want`, writing their payload pointers + lengths into the out arrays (capacity `cap`, order
// preserved). `want`==NULL selects ALL rows; `want`=="" selects only UNTAGGED rows
// (tag[0]=='\0'); otherwise an exact tag match (the one-way id-XXXX hash). Returns how many
// matched (clamped to `cap`). Pure — lets the device-scoped diff (group a species' frames by
// individual so different devices don't smear together) be host-tested end to end. Privacy
// (A5): groups by the already-hashed tag; never reads a raw code.
uint8_t radiotchi_select_by_individual(
    const char* const* tags,
    const uint8_t* const* payloads,
    const uint16_t* lens,
    uint8_t count,
    const char* want,
    const uint8_t** out_payloads,
    uint16_t* out_lens,
    uint8_t cap);

// Encoding-AGNOSTIC path to VALUES: detect a CONFIRMED repeating fixed transmission by
// finding >= 2 identical quantized frames in the pulse train. A real remote retransmits a
// fixed frame several times per press (so frames match), while ambient noise does not repeat
// — this is what reaches VALUES on protocols the bit-level PWM decoder cannot read (e.g.
// Manchester remotes). It recognizes the stable WAVEFORM, not protocol bits. Returns true on
// a confirmed repeat. `fp` (may be NULL) gets a 16-bit hash of the canonical frame — but note
// it is a COARSE, preamble-dominated fingerprint that collides across different buttons of one
// remote (hardware-validated), so it is NOT currently surfaced as a per-device id (D28); a
// real per-device id for these encodings needs the firmware's protocol decoders (TB.1). Pure.
bool radiotchi_repeating_frame(const int16_t* pulses, uint16_t n, uint16_t* fp);

// --- decoder toolkit (pure building blocks for device-protocol decoders) ------
//
// Small, reusable primitives so each new device decoder is a few lines: slice a frame's bytes,
// validate a checksum, pull a bit field. All pure and host-tested. These are what make adding
// many sensor/remote decoders cheap (the dex grows with each new recognized family).

// MSB-first CRC-8 over `data[0..len)` with generator `poly` and initial value `init` (no final
// XOR / no reflection — the common rtl_433-class sensor CRC form). Pure.
uint8_t radiotchi_crc8(const uint8_t* data, uint16_t len, uint8_t poly, uint8_t init);

// 8-bit additive checksum (sum of bytes mod 256) over `data[0..len)`. Pure.
uint8_t radiotchi_checksum8(const uint8_t* data, uint16_t len);

// 8-bit XOR of `data[0..len)`. Pure.
uint8_t radiotchi_xor8(const uint8_t* data, uint16_t len);

// Extract a big-endian bit field of `nbits` (1..32) starting at bit offset `bit_off` from a
// MSB-first byte buffer (bit 0 = MSB of byte 0). Bits beyond the buffer read as 0 — the caller
// is expected to have length-checked. Pure.
uint32_t radiotchi_bits_get(const uint8_t* bytes, uint16_t nbytes, uint16_t bit_off, uint8_t nbits);

// Demodulate an OOK PWM byte frame (bit set by MARK width: long mark = 1, short = 0; the Acurite/
// generic OOK-sensor coding), packing MSB-first into `out` (capacity `cap` bytes). Frames split at
// the long sync gap; a frame is trusted only when an immediately-following repeat packs the SAME
// bytes (the noise guard). Returns the bit count via *nbits and true on a confirmed frame. Pure.
bool radiotchi_pwm_to_bytes(
    const int16_t* pulses, uint16_t n, uint8_t* out, uint16_t cap, uint16_t* nbits);

// Demodulate an OOK PPM byte frame where the BIT is set by the SPACE (gap) width after a fixed
// sync pulse (short gap = 0, long gap = 1; the Nexus/weather-sensor coding), packing MSB-first into
// `out` (capacity `cap` bytes). The short-gap unit is estimated over the SPACES only (the fixed
// mark would poison it); frames split at the long inter-frame gap (its terminating bit reads as 1).
// Repeat-confirmed like the others. Returns the bit count via *nbits and true on a confirmed frame.
// Pure.
bool radiotchi_ppm_to_bytes(
    const int16_t* pulses, uint16_t n, uint8_t* out, uint16_t cap, uint16_t* nbits);

// Run the pulse-based VALUES decoders in priority order on a captured pulse train, filling
// ev->decode_tier (TIER_VALUES), protocol, species_id and individual on the first that succeeds.
// Specific device decoders (CRC-validated sensors) are tried BEFORE the generic fixed-code /
// sensor families, so a recognized device graduates to its named species (more dex breadth).
// Returns true if any decoder matched (ev updated); false otherwise (ev untouched). Shared by
// live capture (`analyze_capture`) and the `.sub` re-grade so both reach identical results — adding
// a decoder here lights it up on new captures AND retroactively on stored ones. Pure.
bool radiotchi_decode_from_pulses(
    uint32_t frequency_hz,
    Modulation modulation,
    const int16_t* pulses,
    uint16_t n,
    CaptureEvent* ev);

// Re-run the classifier on a stored event using its retained features, RAISING its
// DecodeTier (never lowering it), protocol, species, and Nourishment if a decoder now
// recognizes it. The other four axes are untouched. This is the re-grade entry point:
// stored captures retroactively gain meaning as decoders are added. Returns true if
// the event changed. Pure.
bool radiotchi_redecode(CaptureEvent* ev);

#ifdef __cplusplus
}
#endif
