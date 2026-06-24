// Radiotchi — Live CC1101 capture source.
//
// Drives the Flipper's internal CC1101 as an RX-ONLY narrowband receiver:
//
//   1. SEARCH by hopping across a FrequencyPlan, sampling RSSI on each frequency,
//      and remembering the single strongest signal (one feeding = one meal).
//   2. CAPTURE the strongest frequency as a RAW level-duration burst, keeping the
//      lossless pulse train so a `.sub` can be written and re-decoded later.
//   3. Derive a bounded RawCapture (freq, RSSI, modulation guess, a payload proxy)
//      for the Analysis Core.
//
// NO TX path exists here, by design (RX-only is a hard guardrail).

#pragma once

#include <furi_hal.h>

#include "capture_source.h"
#include "frequency_plan.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SubGhzCaptureSource SubGhzCaptureSource;

typedef struct {
    int16_t rssi_threshold_dbm; // absolute floor: a signal must beat this to count
    int16_t detection_margin_db; // AND it must exceed the ambient noise floor by this
    uint32_t hop_dwell_ms; // RSSI settle time per hopped frequency
    uint32_t capture_window_ms; // how long to record the RAW burst once locked
    FuriHalSubGhzPreset preset; // demod preset (OOK/2FSK/...) used while listening
} SubGhzCaptureConfig;

// Sensible defaults: scan-friendly threshold, fast hops, half-second capture, OOK.
SubGhzCaptureConfig subghz_capture_config_default(void);

// Allocate a live source over `plan` (copied in) with `config`. Returns NULL on
// failure (e.g. no CC1101). Call subghz_capture_source_free when done.
SubGhzCaptureSource*
    subghz_capture_source_alloc(const FrequencyPlan* plan, SubGhzCaptureConfig config);

void subghz_capture_source_free(SubGhzCaptureSource* src);

// Replace the active frequency plan (copied in). Resets the hop cursor.
void subghz_capture_source_set_plan(SubGhzCaptureSource* src, const FrequencyPlan* plan);

// Read / update the detection config at runtime (so the UI can tune the RSSI
// threshold and margin live).
SubGhzCaptureConfig subghz_capture_source_get_config(const SubGhzCaptureSource* src);
void subghz_capture_source_set_config(SubGhzCaptureSource* src, SubGhzCaptureConfig config);

// Diagnostics from the most recent search sweep, for on-screen tuning feedback:
// the ambient noise floor (mean RSSI) and the single strongest frequency seen.
int16_t subghz_capture_source_noise_floor(const SubGhzCaptureSource* src);
int16_t subghz_capture_source_last_best_rssi(const SubGhzCaptureSource* src);

// Hop once across every frequency in the plan, sampling RSSI, and report the
// strongest. Returns true and fills *out_freq / *out_rssi if any frequency was
// sampled (regardless of threshold); false only if the plan is empty.
bool subghz_capture_source_search(
    SubGhzCaptureSource* src,
    uint32_t* out_freq,
    int16_t* out_rssi);

// Record a RAW burst on `frequency_hz` for the configured window and fill *out.
// Returns true if any pulses were captured.
bool subghz_capture_source_capture(
    SubGhzCaptureSource* src,
    uint32_t frequency_hz,
    RawCapture* out);

// Access the last capture's raw level-duration pulse train (signed: positive =
// high level, negative = low level), for writing the lossless `.sub`. Valid until
// the next capture. Returns the pair count; sets *out to the internal buffer.
size_t subghz_capture_source_last_pulses(SubGhzCaptureSource* src, const int32_t** out);

// The preset the last capture used (so the `.sub` header can name it).
FuriHalSubGhzPreset subghz_capture_source_preset(const SubGhzCaptureSource* src);

// Wrap this live source in the generic CaptureSource interface. The returned
// struct borrows `src` (do not free `src` while the wrapper is in use). next()
// performs SEARCH then CAPTURE-if-above-threshold.
CaptureSource subghz_capture_source_as_source(SubGhzCaptureSource* src);

#ifdef __cplusplus
}
#endif
