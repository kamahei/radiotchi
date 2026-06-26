// Radiotchi — Live CC1101 capture source implementation. RX-ONLY.

#include "subghz_capture_source.h"

#include <furi.h>
#include <lib/subghz/devices/devices.h>
#include <lib/subghz/devices/cc1101_int/cc1101_int_interconnect.h>
#include <lib/subghz/receiver.h>
#include <lib/subghz/environment.h>
#include <lib/subghz/subghz_protocol_registry.h>
#include <lib/subghz/protocols/base.h>

#include <string.h>

#define TAG "RadiotchiCapture"

// Ceiling on the level-duration pulse train recorded per capture. 8192 pairs at
// 4 bytes = 32 KiB; comfortably covers a half-second OOK burst.
#define CAPTURE_MAX_PULSES 8192u

struct SubGhzCaptureSource {
    const SubGhzDevice* device;
    bool radio_begun;
    FrequencyPlan plan;
    SubGhzCaptureConfig config;

    int32_t* pulses; // signed durations: + = high level, - = low level
    size_t pulse_cap;
    volatile size_t pulse_count;
    volatile bool capturing;

    int16_t noise_floor_dbm; // ambient floor from the last search (mean RSSI)
    int16_t last_best_rssi_dbm; // strongest frequency from the last search
    FuriHalSubGhzPreset last_preset;

    // Firmware Sub-GHz protocol decoders (the proper source of a real protocol + serial).
    SubGhzEnvironment* environment;
    SubGhzReceiver* receiver;
    char fw_protocol[RADIOTCHI_PROTOCOL_LEN]; // last decode's protocol name ("" if none)
    char fw_individual[RADIOTCHI_INDIVIDUAL_LEN]; // privacy-safe hashed serial ("" if none)
};

// Fires (thread context) when the firmware decodes a known protocol from the fed pulses.
// Records the protocol family and a PRIVACY-SAFE hash of the decoded string (its key/serial/
// button) — never the raw identifier (A5).
static void fw_decode_callback(
    SubGhzReceiver* receiver,
    SubGhzProtocolDecoderBase* decoder,
    void* context) {
    UNUSED(receiver);
    SubGhzCaptureSource* src = context;
    if(src == NULL || decoder == NULL || decoder->protocol == NULL ||
       decoder->protocol->name == NULL)
        return;
    strncpy(src->fw_protocol, decoder->protocol->name, sizeof(src->fw_protocol) - 1);
    src->fw_protocol[sizeof(src->fw_protocol) - 1] = '\0';

    FuriString* s = furi_string_alloc();
    if(subghz_protocol_decoder_base_get_string(decoder, s)) {
        uint32_t h = 2166136261u; // FNV-1a over the decoded string => stable per device+button
        for(const char* p = furi_string_get_cstr(s); *p != '\0'; p++) {
            h ^= (uint8_t)*p;
            h *= 16777619u;
        }
        snprintf(
            src->fw_individual, sizeof(src->fw_individual), "id-%04x",
            (unsigned)((h ^ (h >> 16)) & 0xFFFFu));
        src->fw_individual[sizeof(src->fw_individual) - 1] = '\0';
    }
    furi_string_free(s);
}

// --- small helpers ---------------------------------------------------------

SubGhzCaptureConfig subghz_capture_config_default(void) {
    SubGhzCaptureConfig c = {
        // -85 dBm sits in the CC1101 noise floor, so an absolute gate alone treats
        // ambient noise as a "signal". Pair a higher absolute floor with a
        // noise-floor-relative margin so only a signal that actually stands out is
        // captured.
        .rssi_threshold_dbm = -80,
        .detection_margin_db = 8,
        .hop_dwell_ms = 10,
        .capture_window_ms = 500,
        .preset = FuriHalSubGhzPresetOok650Async,
        // Try OOK first (most fixed-code remotes), then 2FSK so sensors/keyfobs are also received.
        .presets = {FuriHalSubGhzPresetOok650Async, FuriHalSubGhzPreset2FSKDev476Async},
        .preset_count = 2,
    };
    return c;
}

static Modulation mod_from_preset(FuriHalSubGhzPreset p) {
    switch(p) {
    case FuriHalSubGhzPresetOok270Async:
    case FuriHalSubGhzPresetOok650Async:
        return MOD_OOK;
    case FuriHalSubGhzPreset2FSKDev238Async:
    case FuriHalSubGhzPreset2FSKDev476Async:
        return MOD_2FSK;
    case FuriHalSubGhzPresetGFSK9_99KbAsync:
        return MOD_GFSK;
    case FuriHalSubGhzPresetMSK99_97KbAsync:
        return MOD_MSK;
    default:
        return MOD_UNKNOWN;
    }
}

// Derive a bounded payload proxy from the pulse train for feature extraction
// (entropy / bit_count). The lossless original is the linked `.sub`; this is just
// a working copy. Slice each pulse against the mean duration: long => 1, short => 0.
// Carry a bounded prefix of the raw pulse train into the boundary type so the pure
// Analysis Core can demodulate known protocols (OOK PWM fixed-code -> TIER_VALUES).
// Signed durations are clamped to int16 µs; magnitude is all the decoder needs and
// the sync gap stays "very long" after clamping. The lossless original is the `.sub`.
static void copy_pulses(const int32_t* pulses, size_t n, RawCapture* out) {
    if(n > RADIOTCHI_PULSES_MAX) n = RADIOTCHI_PULSES_MAX;
    for(size_t i = 0; i < n; i++) {
        int32_t d = pulses[i];
        if(d > INT16_MAX) d = INT16_MAX;
        if(d < INT16_MIN) d = INT16_MIN;
        out->pulses[i] = (int16_t)d;
    }
    out->pulse_count = (uint16_t)n;
}

static void pulses_to_payload(const int32_t* pulses, size_t n, RawCapture* out) {
    memset(out->payload, 0, sizeof(out->payload));
    out->payload_len = 0;
    out->bit_count = 0;
    if(n == 0) return;

    uint64_t sum = 0;
    for(size_t i = 0; i < n; i++) {
        int32_t d = pulses[i];
        sum += (uint64_t)(d < 0 ? -(int64_t)d : (int64_t)d);
    }
    uint32_t mean = (uint32_t)(sum / n);

    size_t bits = n;
    if(bits > (size_t)RADIOTCHI_PAYLOAD_MAX * 8) bits = (size_t)RADIOTCHI_PAYLOAD_MAX * 8;
    for(size_t i = 0; i < bits; i++) {
        int32_t d = pulses[i];
        uint32_t dur = (uint32_t)(d < 0 ? -d : d);
        if(dur > mean) out->payload[i >> 3] |= (uint8_t)(0x80u >> (i & 7u));
    }
    out->bit_count = (uint16_t)bits;
    out->payload_len = (uint16_t)((bits + 7u) / 8u);
}

// Async RX callback — runs in interrupt context, so keep it minimal: append the
// signed duration and bail on overflow. RX-only; never touches a TX path.
static void capture_rx_callback(bool level, uint32_t duration, void* context) {
    SubGhzCaptureSource* src = context;
    if(src == NULL || !src->capturing) return;
    size_t i = src->pulse_count;
    if(i >= src->pulse_cap) return;
    src->pulses[i] = level ? (int32_t)duration : -(int32_t)duration;
    src->pulse_count = i + 1;
}

// --- lifecycle -------------------------------------------------------------

SubGhzCaptureSource*
    subghz_capture_source_alloc(const FrequencyPlan* plan, SubGhzCaptureConfig config) {
    SubGhzCaptureSource* src = malloc(sizeof(SubGhzCaptureSource));
    if(src == NULL) return NULL;
    memset(src, 0, sizeof(*src));

    src->config = config;
    src->last_preset = config.preset;
    if(plan != NULL) src->plan = *plan;

    src->pulse_cap = CAPTURE_MAX_PULSES;
    src->pulses = malloc(sizeof(int32_t) * src->pulse_cap);
    if(src->pulses == NULL) {
        free(src);
        return NULL;
    }

    subghz_devices_init();
    src->device = subghz_devices_get_by_name(SUBGHZ_DEVICE_CC1101_INT_NAME);
    if(src->device == NULL) {
        FURI_LOG_E(TAG, "CC1101 device not found");
        subghz_devices_deinit();
        free(src->pulses);
        free(src);
        return NULL;
    }

    src->radio_begun = subghz_devices_begin(src->device);
    if(!src->radio_begun) {
        FURI_LOG_E(TAG, "subghz_devices_begin failed");
    }
    subghz_devices_reset(src->device);
    subghz_devices_idle(src->device);

    // Firmware Sub-GHz protocol decoders (best-effort: capture still works without them).
    src->environment = subghz_environment_alloc();
    if(src->environment != NULL) {
        subghz_environment_set_protocol_registry(
            src->environment, (void*)&subghz_protocol_registry);
        src->receiver = subghz_receiver_alloc_init(src->environment);
        if(src->receiver != NULL) {
            subghz_receiver_set_filter(src->receiver, SubGhzProtocolFlag_Decodable);
            subghz_receiver_set_rx_callback(src->receiver, fw_decode_callback, src);
        }
    }
    return src;
}

void subghz_capture_source_free(SubGhzCaptureSource* src) {
    if(src == NULL) return;
    if(src->receiver != NULL) subghz_receiver_free(src->receiver);
    if(src->environment != NULL) subghz_environment_free(src->environment);
    if(src->device != NULL) {
        subghz_devices_idle(src->device);
        if(src->radio_begun) subghz_devices_end(src->device);
    }
    subghz_devices_deinit();
    free(src->pulses);
    free(src);
}

void subghz_capture_source_set_plan(SubGhzCaptureSource* src, const FrequencyPlan* plan) {
    if(src == NULL || plan == NULL) return;
    src->plan = *plan;
    frequency_plan_hop_reset(&src->plan);
}

// --- search (frequency hopping) --------------------------------------------

bool subghz_capture_source_search(
    SubGhzCaptureSource* src,
    uint32_t* out_freq,
    int16_t* out_rssi) {
    if(src == NULL || src->device == NULL) return false;
    uint32_t count = frequency_plan_count(&src->plan);
    if(count == 0) return false;

    subghz_devices_idle(src->device);
    subghz_devices_load_preset(src->device, src->config.preset, NULL);
    src->last_preset = src->config.preset;

    uint32_t best_freq = 0;
    float best_rssi = -200.0f;
    float rssi_sum = 0.0f;
    uint32_t sampled = 0;

    for(uint32_t i = 0; i < count; i++) {
        const FrequencyEntry* e = frequency_plan_get(&src->plan, i);
        if(e == NULL) continue;
        if(!subghz_devices_is_frequency_valid(src->device, e->frequency_hz)) continue;

        subghz_devices_idle(src->device);
        subghz_devices_set_frequency(src->device, e->frequency_hz);
        subghz_devices_set_rx(src->device);
        furi_delay_ms(src->config.hop_dwell_ms);
        float rssi = subghz_devices_get_rssi(src->device);
        subghz_devices_idle(src->device);

        rssi_sum += rssi;
        sampled++;
        if(rssi > best_rssi) {
            best_rssi = rssi;
            best_freq = e->frequency_hz;
        }
    }

    if(best_freq == 0 || sampled == 0) return false;
    // Ambient noise floor for the relative-detection gate in next(): the mean of the NON-winning
    // samples. Including the strongest sample (the candidate signal) biases the floor up by ~1/N and,
    // for a single-frequency plan, makes floor == best so `best - floor >= margin` can NEVER clear —
    // a single-frequency sweep could never capture. With only one sample there is no ambient
    // estimate, so use a sentinel far below any real RSSI: the relative gate becomes a no-op and the
    // absolute threshold alone decides.
    src->noise_floor_dbm =
        (sampled > 1) ? (int16_t)((rssi_sum - best_rssi) / (float)(sampled - 1)) : (int16_t)(-200);
    src->last_best_rssi_dbm = (int16_t)best_rssi;
    if(out_freq != NULL) *out_freq = best_freq;
    if(out_rssi != NULL) *out_rssi = (int16_t)best_rssi;
    return true;
}

// --- capture (RAW burst) ---------------------------------------------------

bool subghz_capture_source_capture(
    SubGhzCaptureSource* src,
    uint32_t frequency_hz,
    RawCapture* out) {
    if(src == NULL || src->device == NULL || out == NULL) return false;
    if(!subghz_devices_is_frequency_valid(src->device, frequency_hz)) return false;

    memset(out, 0, sizeof(*out));
    out->frequency_hz = frequency_hz;
    out->modulation = mod_from_preset(src->config.preset);

    subghz_devices_idle(src->device);
    subghz_devices_load_preset(src->device, src->config.preset, NULL);
    src->last_preset = src->config.preset;
    subghz_devices_set_frequency(src->device, frequency_hz);

    // Record the pulse train for the configured window.
    src->pulse_count = 0;
    src->capturing = true;
    subghz_devices_start_async_rx(src->device, capture_rx_callback, src);

    // Sample RSSI shortly after RX engages, then wait out the rest of the window.
    furi_delay_ms(5);
    float rssi = subghz_devices_get_rssi(src->device);
    uint32_t remaining = src->config.capture_window_ms > 5 ? src->config.capture_window_ms - 5 : 0;
    furi_delay_ms(remaining);

    subghz_devices_stop_async_rx(src->device);
    src->capturing = false;
    subghz_devices_idle(src->device);

    out->rssi_dbm = (int16_t)rssi;
    pulses_to_payload(src->pulses, src->pulse_count, out);
    copy_pulses(src->pulses, src->pulse_count, out); // carry timing for protocol decode

    // Replay the captured pulse train through the firmware's Sub-GHz decoders (thread context).
    // fw_decode_callback fills src->fw_* on a successful decode; carry the result to `out`.
    src->fw_protocol[0] = '\0';
    src->fw_individual[0] = '\0';
    if(src->receiver != NULL) {
        subghz_receiver_reset(src->receiver);
        for(size_t i = 0; i < src->pulse_count; i++) {
            int32_t d = src->pulses[i];
            subghz_receiver_decode(src->receiver, d > 0, (uint32_t)(d < 0 ? -d : d));
        }
    }
    strncpy(out->fw_protocol, src->fw_protocol, sizeof(out->fw_protocol) - 1);
    out->fw_protocol[sizeof(out->fw_protocol) - 1] = '\0';
    strncpy(out->fw_individual, src->fw_individual, sizeof(out->fw_individual) - 1);
    out->fw_individual[sizeof(out->fw_individual) - 1] = '\0';

    out->raw_sub_ref[0] = '\0'; // storage layer fills this once the .sub is written

    return src->pulse_count > 0;
}

size_t subghz_capture_source_last_pulses(SubGhzCaptureSource* src, const int32_t** out) {
    if(src == NULL) return 0;
    if(out != NULL) *out = src->pulses;
    return src->pulse_count;
}

FuriHalSubGhzPreset subghz_capture_source_preset(const SubGhzCaptureSource* src) {
    return (src != NULL) ? src->last_preset : FuriHalSubGhzPresetOok650Async;
}

SubGhzCaptureConfig subghz_capture_source_get_config(const SubGhzCaptureSource* src) {
    if(src != NULL) return src->config;
    return subghz_capture_config_default();
}

void subghz_capture_source_set_config(SubGhzCaptureSource* src, SubGhzCaptureConfig config) {
    if(src != NULL) src->config = config;
}

int16_t subghz_capture_source_noise_floor(const SubGhzCaptureSource* src) {
    return (src != NULL) ? src->noise_floor_dbm : 0;
}

int16_t subghz_capture_source_last_best_rssi(const SubGhzCaptureSource* src) {
    return (src != NULL) ? src->last_best_rssi_dbm : 0;
}

// --- generic CaptureSource wrapper -----------------------------------------

static bool subghz_source_next_impl(void* impl, RawCapture* out) {
    SubGhzCaptureSource* src = impl;
    uint32_t freq = 0;
    int16_t rssi = 0;
    if(!subghz_capture_source_search(src, &freq, &rssi)) return false;

    // A real signal must clear BOTH gates: an absolute floor and a margin above the
    // ambient noise floor measured this sweep. This is what makes "no signal"
    // actually report no signal instead of capturing noise.
    if(rssi < src->config.rssi_threshold_dbm) return false;
    if((int)rssi - (int)src->noise_floor_dbm < src->config.detection_margin_db) return false;

    // Build the candidate demod-preset set (fall back to the single primary preset).
    FuriHalSubGhzPreset candidates[RADIOTCHI_PRESETS_MAX];
    uint8_t ncand = 0;
    for(uint8_t i = 0; i < src->config.preset_count && ncand < RADIOTCHI_PRESETS_MAX; i++) {
        candidates[ncand++] = src->config.presets[i];
    }
    if(ncand == 0) candidates[ncand++] = src->config.preset;

    FuriHalSubGhzPreset saved = src->config.preset;

    // Single modulation: capture once (no behavioural change vs. the original single-preset path).
    if(ncand == 1) {
        src->config.preset = candidates[0];
        bool ok = subghz_capture_source_capture(src, freq, out);
        src->config.preset = saved;
        return ok;
    }

    // Multiple modulations: PROBE the locked frequency under each candidate to find the
    // most-decoded preset (a firmware-protocol decode beats a louder-but-undecoded capture; ties
    // break on RSSI), then do ONE FINAL capture in that preset. The final capture is what leaves
    // the source's internal pulse buffer + last preset (which do_feed stages into the lossless
    // `.sub`) consistent with *out, regardless of any intermediate probe that caught nothing. This
    // is what lets an FSK signal be received even though the RSSI sweep listened in OOK.
    // The probe scratch RawCapture (~0.9 KB) lives on the heap, not the 4 KB app stack.
    RawCapture* cur = malloc(sizeof(RawCapture));
    if(cur == NULL) { // out of memory: fall back to a single capture in the primary preset
        src->config.preset = saved;
        return subghz_capture_source_capture(src, freq, out);
    }
    bool have_best = false;
    int best_rank = -1;
    int16_t best_rssi = INT16_MIN;
    FuriHalSubGhzPreset best_preset = candidates[0];
    for(uint8_t i = 0; i < ncand; i++) {
        src->config.preset = candidates[i];
        if(!subghz_capture_source_capture(src, freq, cur)) continue;
        int rank = (cur->fw_protocol[0] != '\0') ? 1 : 0;
        if(!have_best || rank > best_rank || (rank == best_rank && cur->rssi_dbm > best_rssi)) {
            have_best = true;
            best_rank = rank;
            best_rssi = cur->rssi_dbm;
            best_preset = candidates[i];
        }
    }
    free(cur);
    if(!have_best) {
        src->config.preset = saved;
        return false;
    }

    src->config.preset = best_preset;
    bool ok = subghz_capture_source_capture(src, freq, out);
    src->config.preset = saved;
    return ok;
}

CaptureSource subghz_capture_source_as_source(SubGhzCaptureSource* src) {
    CaptureSource cs = {.impl = src, .next = subghz_source_next_impl};
    return cs;
}
