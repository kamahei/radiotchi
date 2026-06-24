// Radiotchi — Analysis Core boundary types.
//
// PORTABILITY CONTRACT (see docs/architecture.md §1 boundary rules):
//   This header and the whole `lib/analysis_core/` tree MUST compile on a plain
//   host compiler. It therefore depends ONLY on the C standard library — never on
//   `furi`, the GUI, storage, the clock, or the capture hardware. "Now" is passed
//   in as data (`timestamp`), never read from a global clock.
//
// The only legal cross-layer types are `RawCapture` (Capture Source -> Analysis
// Core) and `CaptureEvent` (Analysis Core -> Game Shell). Keep it that way.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Max raw demodulated payload we carry in-struct. The lossless original lives in
// the linked `.sub` on disk (see the lossless-capture invariant); this in-memory
// buffer is a bounded working copy for feature extraction.
#define RADIOTCHI_PAYLOAD_MAX 256u

// Bounded copy of the raw pulse train (signed durations, +mark / -space, in µs),
// carried so the pure core can actually demodulate known protocols (e.g. the OOK
// PWM fixed-code family -> TIER_VALUES). The lossless original is still the `.sub`;
// this is a working prefix (a few frames is plenty for a fixed-code).
#define RADIOTCHI_PULSES_MAX 256u

// Field widths for the small fixed strings crossing the boundary.
#define RADIOTCHI_SUBREF_LEN     64u
#define RADIOTCHI_PROTOCOL_LEN   32u
#define RADIOTCHI_SPECIES_LEN    32u
#define RADIOTCHI_INDIVIDUAL_LEN 16u // privacy-safe per-device tag, e.g. "id-a3f2"

// Modulation guess. The decode-free pipeline can fill OOK/2FSK from the capture
// preset; richer decoders refine it later.
typedef enum {
    MOD_UNKNOWN = 0,
    MOD_OOK,
    MOD_2FSK,
    MOD_GFSK,
    MOD_MSK,
} Modulation;

// Decode depth — directly drives the Nourishment axis. The MVP capture stage
// produces TIER_RAW; decoders raise it later and old captures re-grade upward.
typedef enum {
    TIER_RAW = 0, // raw burst only
    TIER_MODULATION, // known modulation identified
    TIER_PROTOCOL, // known protocol identified
    TIER_VALUES, // actual values decoded
} DecodeTier;

// The 5-axis "nutrition label". Axis *definitions* are fixed; numeric calibration
// is deferred (docs/open-questions.md Q3), so the capture stage may leave these at
// provisional values.
typedef struct {
    float calories; // Volume:    data amount / burst length / bit count
    float freshness; // Strength:  RSSI / proximity
    float additives; // Entropy:   Shannon entropy (high = encrypted/junk)
    float rarity; // personal rarity, derived from the dex view
    float nourishment; // Structure: decode depth (the only decode-dependent axis)
} Scores;

// Boundary input: Capture Source -> Analysis Core. The minimal raw observation.
// Carries NO timestamp and NO scores — those are added by the Analysis Core.
typedef struct {
    uint32_t frequency_hz; // center frequency the signal was captured on
    int16_t rssi_dbm; // received signal strength
    Modulation modulation; // modulation implied by the capture preset (best guess)
    uint16_t bit_count; // number of valid payload bits/bytes captured
    uint8_t payload[RADIOTCHI_PAYLOAD_MAX]; // raw demodulated bits/bytes (bounded copy)
    uint16_t payload_len; // bytes used in `payload`
    int16_t pulses[RADIOTCHI_PULSES_MAX]; // raw pulse train (+mark/-space µs, clamped); 0 if none
    uint16_t pulse_count; // entries used in `pulses` (0 => no timing available)
    char raw_sub_ref[RADIOTCHI_SUBREF_LEN]; // path to the saved `.sub` ("" if none yet)
    // Optional firmware-decoded result: the Capture Source (hardware adapter) may run the
    // firmware's Sub-GHz protocol decoders and report the recognized protocol + a PRIVACY-SAFE
    // hashed per-device id (never the raw serial; A5). Both "" when nothing decoded / off-device.
    char fw_protocol[RADIOTCHI_PROTOCOL_LEN];
    char fw_individual[RADIOTCHI_INDIVIDUAL_LEN];
} RawCapture;

// Personal-rarity view: a dex occurrence snapshot passed INTO the core so it
// stays pure (it never reads the dex itself). The capture stage may pass an
// empty view (count 0 => maximally rare).
typedef struct {
    uint32_t seen_count; // times this species/bucket has been seen before
    uint32_t total_captures; // total captures in the dex (for normalization)
} RarityView;

// The learning record: Analysis Core -> Game Shell; persisted losslessly.
typedef struct {
    uint64_t timestamp; // epoch/RTC seconds; INJECTED, not read from a clock
    uint32_t frequency_hz;
    Modulation modulation;
    int16_t rssi_dbm;
    uint16_t bit_count;
    float entropy; // Shannon entropy of payload (bits/symbol)
    DecodeTier decode_tier;
    char protocol[RADIOTCHI_PROTOCOL_LEN]; // "" if unknown
    uint8_t payload[RADIOTCHI_PAYLOAD_MAX];
    uint16_t payload_len;
    char raw_sub_ref[RADIOTCHI_SUBREF_LEN]; // path to saved `.sub` (for re-decode)
    Scores scores;
    char species_id[RADIOTCHI_SPECIES_LEN]; // resolved or provisional fingerprint
    // Privacy-safe individual tag ("id-XXXX"), "" unless a stable device code was decoded
    // (VALUES). A one-way hash of the code — never the raw persistent identifier (A5).
    char individual[RADIOTCHI_INDIVIDUAL_LEN];
} CaptureEvent;

#ifdef __cplusplus
}
#endif
