// Radiotchi — host unit tests for the Analysis Core.
//
// The whole point of the layered architecture is that this hard, RF-ish logic is
// deterministic and testable OFF-device (docs/testing-strategy.md). These tests
// link only lib/analysis_core/ + the C stdlib — no furi, no hardware, no libm.
//
// Build & run (needs a host C compiler):  make -C test            (see test/Makefile)

#include "analysis_core.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, msg)                                  \
    do {                                                  \
        g_checks++;                                       \
        if(!(cond)) {                                     \
            g_failures++;                                 \
            printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
        }                                                 \
    } while(0)

static RawCapture make_raw(uint32_t freq, int16_t rssi, Modulation mod, const uint8_t* p, uint16_t n) {
    RawCapture r;
    memset(&r, 0, sizeof(r));
    r.frequency_hz = freq;
    r.rssi_dbm = rssi;
    r.modulation = mod;
    if(p && n) {
        if(n > RADIOTCHI_PAYLOAD_MAX) n = RADIOTCHI_PAYLOAD_MAX;
        memcpy(r.payload, p, n);
        r.payload_len = n;
        r.bit_count = (uint16_t)(n * 8);
    }
    return r;
}

// One OOK PWM fixed-code frame as captured in fixtures/synthetic/structured_ook_433.sub
// (+mark / -space µs): short=350, long=1050, sync gap=10500. Bits (mark>space => 1):
// 0 0 1 0 1 0 0 1 0 1 0 0 = 0x294 (12 bits). The .sub repeats the frame, so we send it
// twice here too (the decoder uses the repeat as a confidence check).
#define OOK_FRAME                                                                              \
    350, -1050, 350, -1050, 1050, -350, 350, -1050, 1050, -350, 350, -1050, 350, -1050, 1050,  \
        -350, 350, -1050, 1050, -350, 350, -1050, 350, -10500
static const int16_t FIXTURE_PULSES[] = {OOK_FRAME, OOK_FRAME};

static void attach_pulses(RawCapture* r, const int16_t* p, uint16_t n) {
    if(n > RADIOTCHI_PULSES_MAX) n = RADIOTCHI_PULSES_MAX;
    memcpy(r->pulses, p, (size_t)n * sizeof(int16_t));
    r->pulse_count = n;
}

// Append an OOK PWM frame for `nbits` of `code` (MSB first): bit 0 = short mark + long
// space, bit 1 = long mark + short space; the last bit's space is the sync `gap`.
static uint16_t build_pwm_frame(
    int16_t* out, uint16_t at, uint32_t code, uint8_t nbits, int16_t s, int16_t l, int16_t gap) {
    for(int i = (int)nbits - 1; i >= 0; i--) {
        int bit = (int)((code >> i) & 1u);
        int16_t mark = bit ? l : s;
        int16_t space = (i == 0) ? gap : (bit ? s : l);
        out[at++] = mark;
        out[at++] = (int16_t)(-space);
    }
    return at;
}

// Append an NRZ run-length frame for `nbits` of `bytes` (MSB-first), coalescing equal
// consecutive bits into one run of (count * period) µs (sign + for a 1, - for a 0), then a
// long inter-frame gap. Mirrors how the firmware async-RAW slicer emits a 2FSK PCM frame.
static uint16_t build_fsk_frame(
    int16_t* out, uint16_t at, const uint8_t* bytes, uint16_t nbits, int16_t period, int16_t gap) {
    int run = 0;
    int cur = -1;
    for(uint16_t b = 0; b < nbits; b++) {
        int bit = (bytes[b >> 3] >> (7 - (b & 7))) & 1;
        if(bit == cur) {
            run++;
        } else {
            if(cur >= 0) out[at++] = (int16_t)((cur ? 1 : -1) * run * (int)period);
            cur = bit;
            run = 1;
        }
    }
    if(cur >= 0) out[at++] = (int16_t)((cur ? 1 : -1) * run * (int)period);
    out[at++] = (int16_t)(-gap);
    return at;
}

// Append an OOK Manchester frame for `nbits` of `code` (MSB first), G.E. Thomas convention:
// bit 1 = low half then high half, bit 0 = high half then low half (`h` = one HALF-bit µs).
// Consecutive equal half-bits across a bit boundary coalesce into one 2x run (sign + for a high
// half, - for a low half) — exactly as a real Manchester line looks after slicing — then a long
// inter-frame gap. NOTE: a low (space) half-bit at a frame edge would merge into the low gap on a
// real capture, so for clean repeat-confirmable frames build codes that start on bit 0 (high-first)
// and end on bit 1 (high-last) — both edges stay marks and do not get absorbed by the gap.
static uint16_t build_manchester_frame(
    int16_t* out, uint16_t at, uint32_t code, uint8_t nbits, int16_t h, int16_t gap) {
    int run = 0;
    int cur = -1; // current half-bit level (1 high / 0 low), -1 = none yet
    for(int i = (int)nbits - 1; i >= 0; i--) {
        int bit = (int)((code >> i) & 1u);
        int halves[2] = {bit ? 0 : 1, bit ? 1 : 0}; // 1 => low,high ; 0 => high,low
        for(int hbi = 0; hbi < 2; hbi++) {
            int lvl = halves[hbi];
            if(lvl == cur) {
                run++;
            } else {
                if(cur >= 0) out[at++] = (int16_t)((cur ? 1 : -1) * run * (int)h);
                cur = lvl;
                run = 1;
            }
        }
    }
    if(cur >= 0) out[at++] = (int16_t)((cur ? 1 : -1) * run * (int)h);
    out[at++] = (int16_t)(-gap);
    return at;
}

// --- entropy ---------------------------------------------------------------

static void test_entropy(void) {
    printf("entropy:\n");
    uint8_t zeros[64];
    memset(zeros, 0, sizeof(zeros));
    float e_struct = radiotchi_shannon_entropy(zeros, sizeof(zeros));
    CHECK(e_struct < 0.5f, "all-equal bytes => near-zero entropy");

    uint8_t ramp[256];
    for(int i = 0; i < 256; i++) ramp[i] = (uint8_t)i; // uniform distribution
    float e_rand = radiotchi_shannon_entropy(ramp, sizeof(ramp));
    CHECK(e_rand > 7.5f, "uniform bytes => near-max (8 bits) entropy");

    CHECK(radiotchi_shannon_entropy(NULL, 0) == 0.0f, "empty => 0 entropy");
    CHECK(e_rand > e_struct, "random more entropic than structured");
}

// --- axis behavior ---------------------------------------------------------

static void test_axes(void) {
    printf("axes:\n");
    uint8_t pay[32];
    memset(pay, 0xA5, sizeof(pay));

    RawCapture small = make_raw(433920000u, -60, MOD_OOK, pay, 8);
    RawCapture big = make_raw(433920000u, -60, MOD_OOK, pay, 32);
    CaptureEvent es = analyze_capture(&small, NULL, 1000);
    CaptureEvent eb = analyze_capture(&big, NULL, 1000);
    CHECK(eb.scores.calories >= es.scores.calories, "calories monotonic with length");

    RawCapture near = make_raw(433920000u, -40, MOD_OOK, pay, 16);
    RawCapture far = make_raw(433920000u, -95, MOD_OOK, pay, 16);
    CaptureEvent en = analyze_capture(&near, NULL, 1000);
    CaptureEvent ef = analyze_capture(&far, NULL, 1000);
    CHECK(en.scores.freshness > ef.scores.freshness, "freshness higher for stronger RSSI");

    // Rarity: unseen (NULL view) is maximally rare; common species less so.
    RarityView common = {.seen_count = 90, .total_captures = 100};
    CaptureEvent e_rare = analyze_capture(&near, NULL, 1000);
    CaptureEvent e_common = analyze_capture(&near, &common, 1000);
    CHECK(e_rare.scores.rarity > e_common.scores.rarity, "first-seen rarer than common");
}

// --- graceful degradation ---------------------------------------------------

static void test_degradation(void) {
    printf("degradation:\n");
    // A genuinely undecodable signal: whitened / high-entropy payload (the "encrypted
    // junk" of D1). It must still yield a full 4-axis label, with Nourishment at RAW.
    uint8_t pay[128];
    for(int i = 0; i < 128; i++) pay[i] = (uint8_t)i; // 128 distinct => ~7 bits/byte
    RawCapture r = make_raw(315000000u, -70, MOD_OOK, pay, 128);
    CaptureEvent ev = analyze_capture(&r, NULL, 42);

    CHECK(ev.decode_tier == TIER_RAW, "whitened/unknown signal stays at RAW tier");
    CHECK(ev.scores.nourishment == 0.0f, "RAW tier => zero nourishment");
    CHECK(ev.protocol[0] == '\0', "no protocol named for an unknown signal");
    // The other four axes must still be populated (full 4-axis label).
    CHECK(ev.scores.calories > 0.0f, "calories present without decoding");
    CHECK(ev.scores.freshness > 0.0f, "freshness present without decoding");
    CHECK(ev.scores.additives > 0.5f, "high-entropy payload reads as additives/junk");
    CHECK(ev.scores.rarity > 0.0f, "rarity present without decoding");
    CHECK(ev.timestamp == 42, "timestamp injected verbatim");
}

// --- protocol classifier ----------------------------------------------------

static void test_classify(void) {
    printf("classify:\n");
    uint8_t structured[24];
    memset(structured, 0x00, sizeof(structured)); // entropy 0 => clearly structured

    // OOK + structured + remote band => recognized fixed-code remote (PROTOCOL).
    RawCapture remote = make_raw(433920000u, -55, MOD_OOK, structured, 24);
    CaptureEvent ev = analyze_capture(&remote, NULL, 1);
    CHECK(ev.decode_tier == TIER_PROTOCOL, "structured OOK remote => PROTOCOL tier");
    CHECK(strcmp(ev.protocol, "OOK-FixedCode") == 0, "names the OOK fixed-code protocol");
    CHECK(strncmp(ev.species_id, "ook-fixed", 9) == 0, "graduates to a named species");
    CHECK(ev.scores.nourishment > 0.5f, "PROTOCOL tier nourishes more than half");
    CHECK(ev.scores.calories > 0.0f && ev.scores.freshness > 0.0f, "still a full label");

    // Structured 2FSK on a sensor band => recognized FSK sensor family (PROTOCOL).
    RawCapture fsk = make_raw(868350000u, -55, MOD_2FSK, structured, 24);
    CaptureEvent evf = analyze_capture(&fsk, NULL, 1);
    CHECK(evf.decode_tier == TIER_PROTOCOL, "structured 2FSK sensor => PROTOCOL tier");
    CHECK(strcmp(evf.protocol, "FSK-Sensor") == 0, "names the FSK sensor family");
    CHECK(strncmp(evf.species_id, "fsk-sensor", 10) == 0, "graduates to the sensor family");
    // Privacy (A5): the species is FAMILY-level (band), never the device's persistent id.
    CHECK(strstr(evf.species_id, "868") != NULL, "species carries the band, not an id");
    CHECK(strstr(evf.species_id, "3a1f9c44") == NULL, "never surfaces a per-device id");

    // A known modulation we have no recognizer for, still structured => MODULATION.
    RawCapture gfsk = make_raw(868350000u, -55, MOD_GFSK, structured, 24);
    CaptureEvent evg = analyze_capture(&gfsk, NULL, 1);
    CHECK(evg.decode_tier == TIER_MODULATION, "structured GFSK => MODULATION tier");
    CHECK(evg.protocol[0] == '\0', "no protocol named at MODULATION tier");
    CHECK(evg.scores.nourishment > 0.0f, "MODULATION nourishes more than RAW");
    CHECK(evg.scores.nourishment < ev.scores.nourishment, "MODULATION < PROTOCOL (ladder)");

    // Whitened OOK on a remote band => still RAW (entropy gate, not band).
    uint8_t whitened[128];
    for(int i = 0; i < 128; i++) whitened[i] = (uint8_t)(i * 7 + 3);
    RawCapture noise = make_raw(433920000u, -55, MOD_OOK, whitened, 128);
    CaptureEvent evn = analyze_capture(&noise, NULL, 1);
    CHECK(evn.decode_tier == TIER_RAW, "whitened OOK stays RAW despite the remote band");
}

// Pin the classifier's exact tier boundaries so future constant changes are deliberate.
// Calls radiotchi_classify directly (entropy as data, no payload needed).
static DecodeTier tier_of(uint32_t freq, Modulation mod, uint16_t bits, float entropy) {
    return radiotchi_classify(freq, mod, bits, entropy, NULL, 0, NULL, 0);
}

static void test_classify_boundaries(void) {
    printf("classify boundaries:\n");
    const uint32_t band = 433920000u; // a remote/sensor band

    // Unknown modulation is always RAW, however structured.
    CHECK(tier_of(band, MOD_UNKNOWN, 64, 0.0f) == TIER_RAW, "unknown modulation => RAW");

    // Entropy gate (whitened >= 6.0 => RAW; just below stays classifiable).
    CHECK(tier_of(band, MOD_OOK, 64, 6.0f) == TIER_RAW, "entropy 6.0 => whitened => RAW");
    CHECK(tier_of(band, MOD_OOK, 64, 5.9f) != TIER_RAW, "entropy 5.9 is not whitened");

    // Structured gate (< 2.5 => the OOK/FSK signatures can fire; >= 2.5 => MODULATION).
    CHECK(tier_of(band, MOD_OOK, 64, 2.4f) == TIER_PROTOCOL, "structured OOK => PROTOCOL");
    CHECK(tier_of(band, MOD_OOK, 64, 2.5f) == TIER_MODULATION, "entropy 2.5 not structured enough");

    // Burst-length gate (>= 16 pulses).
    CHECK(tier_of(band, MOD_OOK, 16, 0.0f) == TIER_PROTOCOL, "16 pulses => long enough");
    CHECK(tier_of(band, MOD_OOK, 15, 0.0f) == TIER_MODULATION, "15 pulses => too short");

    // Band edges (300-470 and 779-930 MHz are in; just outside => MODULATION).
    CHECK(tier_of(300000000u, MOD_OOK, 64, 0.0f) == TIER_PROTOCOL, "300 MHz in band");
    CHECK(tier_of(299000000u, MOD_OOK, 64, 0.0f) == TIER_MODULATION, "299 MHz out of band");
    CHECK(tier_of(930000000u, MOD_OOK, 64, 0.0f) == TIER_PROTOCOL, "930 MHz in band");
    CHECK(tier_of(931000000u, MOD_OOK, 64, 0.0f) == TIER_MODULATION, "931 MHz out of band");

    // 2FSK gets the sensor family on the same gates; GFSK/MSK have no recognizer.
    CHECK(tier_of(band, MOD_2FSK, 64, 0.0f) == TIER_PROTOCOL, "structured 2FSK => FSK-Sensor");
    CHECK(tier_of(band, MOD_GFSK, 64, 0.0f) == TIER_MODULATION, "GFSK has no protocol recognizer");
}

// --- fingerprint species ----------------------------------------------------

static void test_fingerprint(void) {
    printf("fingerprint:\n");
    char a[RADIOTCHI_SPECIES_LEN], b[RADIOTCHI_SPECIES_LEN], c[RADIOTCHI_SPECIES_LEN];
    radiotchi_fingerprint_species(433920000u, MOD_OOK, 64, a, sizeof(a));
    radiotchi_fingerprint_species(433900000u, MOD_OOK, 60, b, sizeof(b)); // nearby+similar
    radiotchi_fingerprint_species(315000000u, MOD_2FSK, 64, c, sizeof(c)); // different band+mod
    CHECK(strcmp(a, b) == 0, "two similar unknowns share a provisional species");
    CHECK(strcmp(a, c) != 0, "different band/mod => different species");
}

// --- re-grade regression ----------------------------------------------------

static void test_regrade(void) {
    printf("re-grade:\n");
    // A structured OOK remote that today's classifier recognizes.
    uint8_t pay[24];
    memset(pay, 0x00, sizeof(pay));
    RawCapture r = make_raw(433920000u, -55, MOD_OOK, pay, 24);
    CaptureEvent ev = analyze_capture(&r, NULL, 7);

    // Simulate an OLD stored capture, scored before this decoder existed: force it back
    // to RAW with a fingerprint species, as persistence would have left it.
    CaptureEvent legacy = ev;
    legacy.decode_tier = TIER_RAW;
    legacy.scores.nourishment = 0.0f;
    legacy.protocol[0] = '\0';
    radiotchi_fingerprint_species(
        legacy.frequency_hz, legacy.modulation, legacy.bit_count, legacy.species_id,
        sizeof(legacy.species_id));
    Scores before = legacy.scores;

    // Re-grade it with the current decoder set.
    bool changed = radiotchi_redecode(&legacy);

    CHECK(changed, "re-grade reports a change");
    CHECK(legacy.decode_tier == TIER_PROTOCOL, "re-grade raises RAW -> PROTOCOL");
    CHECK(strcmp(legacy.protocol, "OOK-FixedCode") == 0, "re-grade names the protocol");
    CHECK(strncmp(legacy.species_id, "ook-fixed", 9) == 0, "re-grade graduates the species");
    CHECK(legacy.scores.nourishment > before.nourishment, "re-grade raises nourishment");
    // The other four axes must be byte-for-byte unchanged (lossless re-grade).
    CHECK(legacy.scores.calories == before.calories, "re-grade preserves calories");
    CHECK(legacy.scores.freshness == before.freshness, "re-grade preserves freshness");
    CHECK(legacy.scores.additives == before.additives, "re-grade preserves additives");
    CHECK(legacy.scores.rarity == before.rarity, "re-grade preserves rarity");

    // Re-grading again is a no-op (tier only ever rises).
    CHECK(!radiotchi_redecode(&legacy), "second re-grade is a no-op");
}

// --- real OOK PWM demodulation (TIER_VALUES) --------------------------------

static void test_ook_decode(void) {
    printf("ook decode:\n");
    uint32_t code = 0;
    uint8_t nbits = 0;

    bool ok = radiotchi_ook_pwm_decode(
        FIXTURE_PULSES, (uint16_t)(sizeof(FIXTURE_PULSES) / sizeof(FIXTURE_PULSES[0])), &code, &nbits);
    CHECK(ok, "decodes the fixture OOK fixed-code frame");
    CHECK(nbits == 12, "recovers the 12-bit code length");
    CHECK(code == 0x294u, "recovers the exact code value (0x294)");

    // Too few pulses => no decode.
    int16_t tiny[4] = {350, -1050, 1050, -350};
    CHECK(!radiotchi_ook_pwm_decode(tiny, 4, &code, &nbits), "too-short burst does not decode");

    // Two frames that DISAGREE (repeat-confidence guard) => distrust.
    int16_t a = 350, b = 1050, g = 10500;
    int16_t disagree[] = {
        // frame 1: bits ...0 ; frame 2: bits ...1 (last mark/space swapped)
        a, -b, a, -b, a, -b, a, -b, a, -b, a, -b, a, -b, a, -g, // frame 1 (8 bits)
        b, -a, b, -a, b, -a, b, -a, b, -a, b, -a, b, -a, b, -g, // frame 2 (8 bits, differs)
    };
    CHECK(
        !radiotchi_ook_pwm_decode(disagree, (uint16_t)(sizeof(disagree) / sizeof(disagree[0])), &code, &nbits),
        "disagreeing repeat frames are distrusted");
}

static void test_decoder_robustness(void) {
    printf("decoder robustness:\n");
    int16_t buf[128];
    uint32_t code = 0;
    uint8_t nbits = 0;

    // A code ENDING IN 1 (long mark before the gap) must round-trip — the bit is set by the
    // mark width, not mark-vs-space (regression: the gap space used to force the last bit to 0).
    uint16_t n = build_pwm_frame(buf, 0, 0x295u, 12, 350, 1050, 10500);
    n = build_pwm_frame(buf, n, 0x295u, 12, 350, 1050, 10500); // repeat (confidence)
    CHECK(radiotchi_ook_pwm_decode(buf, n, &code, &nbits), "12-bit code ending in 1 decodes");
    CHECK(code == 0x295u && nbits == 12, "last-bit=1 recovered (not truncated to 0x294)");

    // A wider, 24-bit code round-trips.
    n = build_pwm_frame(buf, 0, 0xABCDEFu, 24, 350, 1050, 10500);
    n = build_pwm_frame(buf, n, 0xABCDEFu, 24, 350, 1050, 10500);
    CHECK(
        radiotchi_ook_pwm_decode(buf, n, &code, &nbits) && code == 0xABCDEFu && nbits == 24,
        "24-bit code round-trips");

    // Threshold-free across different absolute timings (500/1500/15000).
    n = build_pwm_frame(buf, 0, 0x5A3u, 12, 500, 1500, 15000);
    n = build_pwm_frame(buf, n, 0x5A3u, 12, 500, 1500, 15000);
    CHECK(
        radiotchi_ook_pwm_decode(buf, n, &code, &nbits) && code == 0x5A3u,
        "decodes with different short/long timings");

    // High long:short ratio (8:1) with a far sync gap. A pure "5x short" gap threshold
    // would misread the long bits' spaces as gaps (found on real 868 MHz captures); the
    // absolute gap floor handles it.
    n = build_pwm_frame(buf, 0, 0x2C7u, 12, 200, 1600, 20000);
    n = build_pwm_frame(buf, n, 0x2C7u, 12, 200, 1600, 20000);
    CHECK(
        radiotchi_ook_pwm_decode(buf, n, &code, &nbits) && code == 0x2C7u,
        "decodes a high long:short ratio code (gap floor, not 5x-short)");

    // A lone (un-repeated) frame is REJECTED: real remotes retransmit, noise does not, so an
    // unconfirmed single frame must not yield VALUES. This is the guard that blocks ambient
    // noise from faking a code (observed 13/13 distinct "codes" from noise before it).
    n = build_pwm_frame(buf, 0, 0x123u, 12, 350, 1050, 10500);
    CHECK(!radiotchi_ook_pwm_decode(buf, n, &code, &nbits), "a single un-repeated frame is rejected");

    // Leading silence (a space before the first mark) is skipped; two frames confirm.
    buf[0] = -4000;
    n = build_pwm_frame(buf, 1, 0x123u, 12, 350, 1050, 10500);
    n = build_pwm_frame(buf, n, 0x123u, 12, 350, 1050, 10500);
    CHECK(
        radiotchi_ook_pwm_decode(buf, n, &code, &nbits) && code == 0x123u,
        "leading silence is skipped (confirmed by a repeat)");
}

static void test_repeating_frame(void) {
    printf("repeating frame:\n");
    // A synthetic long OOK frame: a sync header (long mark) + a 28-element payload + the
    // inter-frame gap. This stands in for a non-PWM encoding (e.g. Manchester) the bit decoder
    // can not read; the generic detector recognizes the REPEAT, not the bits.
    static const int16_t FRAME[] = {
        400, -100, 100, -100, 100, -100, 200, -100, 100, -200, 100, -100, 100, -100, 200,
        -200, 100, -100, 100, -200, 200, -100, 100, -100, 100, -100, 100, -100, 100, -8000,
    };
    const uint16_t FL = (uint16_t)(sizeof(FRAME) / sizeof(FRAME[0]));

    int16_t buf[256];
    uint16_t n = 0;
    for(int rep = 0; rep < 3; rep++)
        for(uint16_t k = 0; k < FL; k++) buf[n++] = FRAME[k];

    uint16_t fp = 0, fp2 = 0;
    CHECK(radiotchi_repeating_frame(buf, n, &fp), "3 identical frames => confirmed repeat");
    CHECK(fp != 0, "a per-device fingerprint is produced");
    radiotchi_repeating_frame(buf, n, &fp2);
    CHECK(fp == fp2, "fingerprint is stable for the same transmission (recurrence)");

    // A LONE frame (no repeat) is NOT confirmed — same guard as the PWM path, blocks noise.
    CHECK(!radiotchi_repeating_frame(FRAME, FL, &fp), "a single un-repeated frame is rejected");
}

// --- real OOK Manchester demodulation (TIER_VALUES) -------------------------

static void test_manchester_decode(void) {
    printf("manchester decode:\n");
    int16_t buf[256];
    uint32_t code = 0;
    uint8_t nbits = 0;

    // 0x4A5 starts on bit 0 (high-first) and ends on bit 1 (high-last), so neither frame edge is a
    // low half-bit that the inter-frame gap would absorb — the two frames are byte-identical and
    // confirm each other.
    uint16_t n = build_manchester_frame(buf, 0, 0x4A5u, 12, 250, 8000);
    n = build_manchester_frame(buf, n, 0x4A5u, 12, 250, 8000);
    CHECK(radiotchi_manchester_decode(buf, n, &code, &nbits), "two Manchester frames decode");
    CHECK(nbits == 12, "recovers the 12-bit code length");
    CHECK(code == 0x4A5u, "recovers the exact code value (0x4A5)");

    // Determinism: decoding the same train twice yields the same code (idempotent re-grade).
    uint32_t code2 = 0;
    uint8_t nbits2 = 0;
    radiotchi_manchester_decode(buf, n, &code2, &nbits2);
    CHECK(code == code2 && nbits == nbits2, "decode is deterministic");

    // A different half-bit unit round-trips to the same code (the unit is estimated, not assumed).
    n = build_manchester_frame(buf, 0, 0x4A5u, 12, 400, 12000);
    n = build_manchester_frame(buf, n, 0x4A5u, 12, 400, 12000);
    CHECK(
        radiotchi_manchester_decode(buf, n, &code, &nbits) && code == 0x4A5u && nbits == 12,
        "decodes with a different half-bit unit");

    // A wider, 16-bit code (first bit 0, last bit 1) round-trips.
    n = build_manchester_frame(buf, 0, 0x6C39u, 16, 250, 8000);
    n = build_manchester_frame(buf, n, 0x6C39u, 16, 250, 8000);
    CHECK(
        radiotchi_manchester_decode(buf, n, &code, &nbits) && code == 0x6C39u && nbits == 16,
        "16-bit Manchester code round-trips");

    // A lone (un-repeated) frame is rejected — the noise guard (real remotes retransmit).
    uint16_t one = build_manchester_frame(buf, 0, 0x4A5u, 12, 250, 8000);
    CHECK(
        !radiotchi_manchester_decode(buf, one, &code, &nbits),
        "a single un-repeated Manchester frame is rejected");

    // Two frames that DISAGREE => distrust.
    n = build_manchester_frame(buf, 0, 0x4A5u, 12, 250, 8000);
    n = build_manchester_frame(buf, n, 0x4B5u, 12, 250, 8000); // differs
    CHECK(
        !radiotchi_manchester_decode(buf, n, &code, &nbits),
        "disagreeing Manchester repeat frames are distrusted");
}

static void test_parse_raw_data(void) {
    printf("parse raw_data:\n");
    int16_t buf[64];
    uint16_t n = radiotchi_parse_raw_data("RAW_Data: 350 -1050 1050 -350", buf, 64, 0);
    CHECK(n == 4, "parses four pulses");
    CHECK(buf[0] == 350 && buf[1] == -1050 && buf[2] == 1050 && buf[3] == -350, "values + signs");

    // Non-RAW_Data lines (headers) contribute nothing.
    CHECK(radiotchi_parse_raw_data("Frequency: 433920000", buf, 64, 0) == 0, "header ignored");
    CHECK(radiotchi_parse_raw_data("Protocol: RAW", buf, 64, 0) == 0, "non-data line ignored");

    // Appends from `have`, and clamps to int16.
    n = radiotchi_parse_raw_data("RAW_Data: 40000 -40000", buf, 64, 2);
    CHECK(n == 4, "appends starting at have");
    CHECK(buf[2] == 32767 && buf[3] == -32768, "clamps to int16 range");

    // Parse the fixture frame TWICE (two RAW_Data lines = a frame + its confirming repeat),
    // then decode -> the same code as the raw array.
    const char* frame = "RAW_Data: 350 -1050 350 -1050 1050 -350 350 -1050 1050 -350 "
                        "350 -1050 350 -1050 1050 -350 350 -1050 1050 -350 350 -1050 350 -10500";
    int16_t pl[64];
    uint16_t c = 0;
    c = radiotchi_parse_raw_data(frame, pl, 64, c);
    c = radiotchi_parse_raw_data(frame, pl, 64, c);
    uint32_t code = 0;
    uint8_t nbits = 0;
    CHECK(radiotchi_ook_pwm_decode(pl, c, &code, &nbits), "parsed fixture frames decode");
    CHECK(code == 0x294u && nbits == 12, "parse->decode recovers 0x294/12 bits");
}

// Load the REAL .sub fixture from disk and run the full parse -> decode path, so the
// on-disk format contract (T1.2 / T0.1) is exercised, not just inline arrays.
static void test_fixture_sub(void) {
    printf("fixture .sub:\n");
    FILE* fp = fopen("../fixtures/synthetic/structured_ook_433.sub", "r");
    if(fp == NULL) fp = fopen("fixtures/synthetic/structured_ook_433.sub", "r");
    if(fp == NULL) {
        printf("  (skip: fixture not reachable from cwd)\n");
        return;
    }
    int16_t pulses[256];
    uint16_t n = 0;
    char line[2048];
    while(n < 256 && fgets(line, sizeof(line), fp) != NULL) {
        n = radiotchi_parse_raw_data(line, pulses, 256, n); // header lines contribute nothing
    }
    fclose(fp);

    uint32_t code = 0;
    uint8_t nbits = 0;
    CHECK(n > 0, "parsed pulses from the on-disk .sub fixture");
    CHECK(radiotchi_ook_pwm_decode(pulses, n, &code, &nbits), "the real fixture decodes");
    CHECK(code == 0x294u && nbits == 12, "the real fixture yields 0x294 / 12 bits");
}

// Regression for the real-device finding: ambient noise (a real Flipper capture with no
// transmitter present) must NOT decode to a false fixed-code. Before the repeat-frame guard,
// captures like this yielded bogus VALUES with all-distinct "codes".
static void test_real_noise_fixture(void) {
    printf("real-noise fixture:\n");
    FILE* fp = fopen("../fixtures/real/noise_868.sub", "r");
    if(fp == NULL) fp = fopen("fixtures/real/noise_868.sub", "r");
    if(fp == NULL) {
        printf("  (skip: fixture not reachable from cwd)\n");
        return;
    }
    int16_t pulses[RADIOTCHI_PULSES_MAX];
    uint16_t n = 0;
    char line[4096];
    while(n < RADIOTCHI_PULSES_MAX && fgets(line, sizeof(line), fp) != NULL) {
        n = radiotchi_parse_raw_data(line, pulses, RADIOTCHI_PULSES_MAX, n);
    }
    fclose(fp);

    uint32_t code = 0;
    uint8_t nbits = 0;
    uint16_t rfp = 0;
    CHECK(n > 0, "parsed pulses from the real noise capture");
    CHECK(
        !radiotchi_ook_pwm_decode(pulses, n, &code, &nbits),
        "real ambient noise does NOT decode to a (false) VALUES code");
    CHECK(
        !radiotchi_manchester_decode(pulses, n, &code, &nbits),
        "real ambient noise does NOT decode to a (false) Manchester code");
    CHECK(
        !radiotchi_repeating_frame(pulses, n, &rfp),
        "real ambient noise has no confirmed repeating frame (no false VALUES)");
}

// Replica of the capture source's pulse->payload proxy (subghz_capture_source.c
// pulses_to_payload): slice each pulse against the mean magnitude (long=1, short=0). It
// only feeds entropy/bit_count, never decoding; kept in sync by hand for this integration test.
static void proxy_from_pulses(const int16_t* p, uint16_t n, RawCapture* out) {
    if(n == 0) return;
    uint64_t sum = 0;
    for(uint16_t i = 0; i < n; i++) sum += (uint32_t)(p[i] < 0 ? -p[i] : p[i]);
    uint32_t mean = (uint32_t)(sum / n);
    uint16_t bits = n;
    if(bits > RADIOTCHI_PAYLOAD_MAX * 8) bits = RADIOTCHI_PAYLOAD_MAX * 8;
    for(uint16_t i = 0; i < bits; i++) {
        uint32_t dur = (uint32_t)(p[i] < 0 ? -p[i] : p[i]);
        if(dur > mean) out->payload[i >> 3] |= (uint8_t)(0x80u >> (i & 7u));
    }
    out->bit_count = bits;
    out->payload_len = (uint16_t)((bits + 7u) / 8u);
}

// End-to-end: run the headline analyze_capture over a real ambient-noise capture (as the
// device assembles it: pulses + the entropy/bit_count proxy) and confirm the whole pipeline
// neither promotes noise to VALUES nor emits a false individual tag.
static void test_pipeline_real_noise(void) {
    printf("pipeline on real noise:\n");
    FILE* fp = fopen("../fixtures/real/noise_868.sub", "r");
    if(fp == NULL) fp = fopen("fixtures/real/noise_868.sub", "r");
    if(fp == NULL) {
        printf("  (skip: fixture not reachable from cwd)\n");
        return;
    }
    int16_t pulses[RADIOTCHI_PULSES_MAX];
    uint16_t n = 0;
    char line[4096];
    while(n < RADIOTCHI_PULSES_MAX && fgets(line, sizeof(line), fp) != NULL) {
        n = radiotchi_parse_raw_data(line, pulses, RADIOTCHI_PULSES_MAX, n);
    }
    fclose(fp);

    RawCapture r;
    memset(&r, 0, sizeof(r));
    r.frequency_hz = 868350000u;
    r.rssi_dbm = -60;
    r.modulation = MOD_OOK;
    memcpy(r.pulses, pulses, (size_t)n * sizeof(int16_t));
    r.pulse_count = n;
    proxy_from_pulses(pulses, n, &r);

    CaptureEvent ev = analyze_capture(&r, NULL, 1000);
    CHECK(ev.decode_tier != TIER_VALUES, "pipeline: real noise never reaches VALUES");
    CHECK(ev.individual[0] == '\0', "pipeline: no individual tag fabricated from noise");
    CHECK(ev.scores.calories > 0.0f, "pipeline: still a full 4-axis label");
}

static void test_individual_fingerprint(void) {
    printf("individual fingerprint:\n");
    char a[RADIOTCHI_INDIVIDUAL_LEN], b[RADIOTCHI_INDIVIDUAL_LEN], c[RADIOTCHI_INDIVIDUAL_LEN];
    radiotchi_individual_fingerprint(0x294u, 12, a, sizeof(a));
    radiotchi_individual_fingerprint(0x294u, 12, b, sizeof(b)); // same device, same tag
    radiotchi_individual_fingerprint(0x295u, 12, c, sizeof(c)); // different code
    CHECK(strcmp(a, b) == 0, "same code => stable tag (recurrence is observable)");
    CHECK(strcmp(a, c) != 0, "a different code => a different tag");
    CHECK(strncmp(a, "id-", 3) == 0, "tag is the id- form");
    // Privacy (A5): the raw code never appears in the tag (it is a one-way hash).
    CHECK(strstr(a, "294") == NULL, "tag does not leak the raw code");
    // Width is folded in: same bits value, different widths => different tags.
    radiotchi_individual_fingerprint(0x294u, 24, c, sizeof(c));
    CHECK(strcmp(a, c) != 0, "bit width disambiguates equal numeric codes");
}

static void test_values_tier(void) {
    printf("values tier:\n");
    // OOK capture WITH the pulse train => real decode => TIER_VALUES.
    RawCapture r;
    memset(&r, 0, sizeof(r));
    r.frequency_hz = 433920000u;
    r.rssi_dbm = -55;
    r.modulation = MOD_OOK;
    r.bit_count = 24; // a real burst length (so the no-timing path still reaches PROTOCOL)
    attach_pulses(&r, FIXTURE_PULSES, (uint16_t)(sizeof(FIXTURE_PULSES) / sizeof(FIXTURE_PULSES[0])));
    CaptureEvent ev = analyze_capture(&r, NULL, 1);
    CHECK(ev.decode_tier == TIER_VALUES, "decoded pulse train reaches TIER_VALUES");
    CHECK(strcmp(ev.protocol, "OOK-FixedCode") == 0, "names the protocol");
    CHECK(ev.scores.nourishment == 1.0f, "VALUES tier => full nourishment");
    // Privacy (A5): the species stays family-level, never a per-code identifier.
    CHECK(strcmp(ev.species_id, "ook-fixed-433") == 0, "species is family-level, not the code");
    // A stable code yields a privacy-safe individual tag (for longitudinal recurrence).
    CHECK(strncmp(ev.individual, "id-", 3) == 0, "VALUES sets a privacy-safe individual tag");

    // SAME signal WITHOUT the pulse train => signature classifier only => PROTOCOL.
    r.pulse_count = 0;
    CaptureEvent ev2 = analyze_capture(&r, NULL, 1);
    CHECK(ev2.decode_tier == TIER_PROTOCOL, "no timing => classifier stops at PROTOCOL");
    CHECK(ev2.scores.nourishment < ev.scores.nourishment, "VALUES nourishes more than PROTOCOL");
    CHECK(ev2.individual[0] == '\0', "no value decoded => no individual tag");
}

static void test_fw_decode(void) {
    printf("firmware decode:\n");
    // The Capture Source decoded a real protocol + a privacy-safe hashed id; the core must
    // take it as VALUES with that protocol/id/species, ahead of the heuristics.
    RawCapture r;
    memset(&r, 0, sizeof(r));
    r.frequency_hz = 433920000u;
    r.rssi_dbm = -55;
    r.modulation = MOD_OOK;
    r.bit_count = 24;
    strncpy(r.fw_protocol, "Princeton", sizeof(r.fw_protocol) - 1);
    strncpy(r.fw_individual, "id-1a2b", sizeof(r.fw_individual) - 1);
    CaptureEvent ev = analyze_capture(&r, NULL, 1);
    CHECK(ev.decode_tier == TIER_VALUES, "firmware decode => VALUES");
    CHECK(strcmp(ev.protocol, "Princeton") == 0, "uses the firmware protocol name");
    CHECK(strcmp(ev.individual, "id-1a2b") == 0, "uses the firmware (hashed) per-device id");
    CHECK(strcmp(ev.species_id, "Princeton") == 0, "species = protocol family (privacy-safe)");
    CHECK(ev.scores.nourishment == 1.0f, "VALUES => full nourishment");
}

static void test_species_branding(void) {
    printf("species branding:\n");
    char sp[RADIOTCHI_SPECIES_LEN];

    // A branded car-alarm protocol -> a maker-named family species (with a band suffix).
    radiotchi_species_for_protocol("Star Line", 433920000u, sp, sizeof(sp));
    CHECK(strcmp(sp, "keyfob-starline-433") == 0, "Star Line -> keyfob-starline-433");

    // Case-insensitive substring match + a different band.
    radiotchi_species_for_protocol("CAME 12bit", 315000000u, sp, sizeof(sp));
    CHECK(strcmp(sp, "gate-came-315") == 0, "CAME -> gate-came-315 (case/substring tolerant)");

    radiotchi_species_for_protocol("KeeLoq", 433920000u, sp, sizeof(sp));
    CHECK(strcmp(sp, "rolling-keeloq-433") == 0, "KeeLoq -> rolling-keeloq-433");

    // An unbranded protocol keeps its own name (no regression vs the old verbatim behaviour).
    radiotchi_species_for_protocol("Princeton", 433920000u, sp, sizeof(sp));
    CHECK(strcmp(sp, "Princeton") == 0, "unbranded protocol keeps its name");

    // Empty / null inputs are safe.
    radiotchi_species_for_protocol("", 433920000u, sp, sizeof(sp));
    CHECK(sp[0] == '\0', "empty protocol => empty species");

    // End-to-end: analyze_capture applies the branding to species_id while the per-device id
    // stays the hashed firmware tag (A5) and the protocol name is preserved.
    RawCapture r;
    memset(&r, 0, sizeof(r));
    r.frequency_hz = 433920000u;
    r.modulation = MOD_OOK;
    strncpy(r.fw_protocol, "Star Line", sizeof(r.fw_protocol) - 1);
    strncpy(r.fw_individual, "id-1234", sizeof(r.fw_individual) - 1);
    CaptureEvent ev = analyze_capture(&r, NULL, 1);
    CHECK(ev.decode_tier == TIER_VALUES, "firmware decode => VALUES");
    CHECK(strcmp(ev.species_id, "keyfob-starline-433") == 0, "analyze_capture brands the species");
    CHECK(strcmp(ev.individual, "id-1234") == 0, "the per-device id stays the hashed fw tag");
    CHECK(strcmp(ev.protocol, "Star Line") == 0, "the protocol name is preserved");
}

// --- real 2FSK sensor demodulation (TIER_VALUES) ----------------------------

static void test_fsk_decode(void) {
    printf("fsk decode:\n");
    const uint8_t want[3] = {0xAA, 0xC5, 0x3D};
    int16_t buf[256];
    uint8_t frame[RADIOTCHI_FSK_FRAME_MAX];
    uint16_t flen = 0;

    // Two identical NRZ frames (a sensor retransmits) => decode the exact bytes.
    uint16_t n = build_fsk_frame(buf, 0, want, 24, 100, 8000);
    n = build_fsk_frame(buf, n, want, 24, 100, 8000);
    CHECK(radiotchi_fsk_sensor_decode(buf, n, frame, sizeof(frame), &flen), "two FSK frames decode");
    CHECK(flen == 3, "recovers the 3-byte frame length");
    CHECK(frame[0] == 0xAA && frame[1] == 0xC5 && frame[2] == 0x3D, "recovers the exact frame bytes");

    // Determinism: decoding the same train twice yields identical bytes (idempotent re-grade).
    uint8_t frame2[RADIOTCHI_FSK_FRAME_MAX];
    uint16_t flen2 = 0;
    radiotchi_fsk_sensor_decode(buf, n, frame2, sizeof(frame2), &flen2);
    CHECK(flen == flen2 && memcmp(frame, frame2, flen) == 0, "decode is deterministic");

    // A lone (un-repeated) frame is rejected — the noise guard (real sensors retransmit).
    uint16_t one = build_fsk_frame(buf, 0, want, 24, 100, 8000);
    CHECK(
        !radiotchi_fsk_sensor_decode(buf, one, frame, sizeof(frame), &flen),
        "a single un-repeated frame is rejected");

    // Two frames that DISAGREE => distrust.
    const uint8_t other[3] = {0xAA, 0xC5, 0x3C};
    n = build_fsk_frame(buf, 0, want, 24, 100, 8000);
    n = build_fsk_frame(buf, n, other, 24, 100, 8000);
    CHECK(
        !radiotchi_fsk_sensor_decode(buf, n, frame, sizeof(frame), &flen),
        "disagreeing repeat frames are distrusted");

    // A different bit period round-trips to the same bytes (the period is estimated, not assumed).
    n = build_fsk_frame(buf, 0, want, 24, 200, 12000);
    n = build_fsk_frame(buf, n, want, 24, 200, 12000);
    CHECK(
        radiotchi_fsk_sensor_decode(buf, n, frame, sizeof(frame), &flen) && flen == 3 &&
            frame[0] == 0xAA && frame[1] == 0xC5 && frame[2] == 0x3D,
        "decodes with a different bit period");

    // A sub-glitch spike inside the frame is ignored (does not corrupt the bitstream).
    int16_t gbuf[300];
    uint16_t gn = build_fsk_frame(gbuf, 0, want, 24, 100, 8000);
    for(uint16_t k = gn; k > 1; k--) gbuf[k] = gbuf[k - 1]; // open a slot at index 1
    gbuf[1] = 20; // < WV_FSK_GLITCH_US
    gn += 1;
    gn = build_fsk_frame(gbuf, gn, want, 24, 100, 8000);
    CHECK(
        radiotchi_fsk_sensor_decode(gbuf, gn, frame, sizeof(frame), &flen) && flen == 3 &&
            frame[0] == 0xAA,
        "a sub-glitch spike is ignored");
}

static void test_fsk_values(void) {
    printf("fsk values tier:\n");
    const uint8_t want[3] = {0xAA, 0xC5, 0x3D};
    int16_t buf[256];
    uint16_t n = build_fsk_frame(buf, 0, want, 24, 100, 8000);
    n = build_fsk_frame(buf, n, want, 24, 100, 8000);

    RawCapture r;
    memset(&r, 0, sizeof(r));
    r.frequency_hz = 868350000u;
    r.rssi_dbm = -55;
    r.modulation = MOD_2FSK;
    r.bit_count = 24; // a real burst length (so the no-timing path still reaches PROTOCOL)
    attach_pulses(&r, buf, n);
    CaptureEvent ev = analyze_capture(&r, NULL, 1);
    CHECK(ev.decode_tier == TIER_VALUES, "decoded 2FSK frame reaches TIER_VALUES");
    CHECK(strcmp(ev.protocol, "FSK-Sensor") == 0, "names the FSK-Sensor family");
    CHECK(ev.scores.nourishment == 1.0f, "VALUES tier => full nourishment");
    // Privacy (A5): the species stays family-level (band), never the decoded frame.
    CHECK(strcmp(ev.species_id, "fsk-sensor-868") == 0, "species is family-level, not the frame");
    CHECK(strncmp(ev.individual, "id-", 3) == 0, "VALUES sets a privacy-safe individual tag");

    // SAME signal WITHOUT the pulse train => signature classifier only => PROTOCOL.
    r.pulse_count = 0;
    CaptureEvent ev2 = analyze_capture(&r, NULL, 1);
    CHECK(ev2.decode_tier == TIER_PROTOCOL, "no timing => 2FSK stops at PROTOCOL");
    CHECK(ev2.individual[0] == '\0', "no frame decoded => no individual tag");
}

static void test_fsk_fixture_sub(void) {
    printf("fsk fixture .sub:\n");
    FILE* fp = fopen("../fixtures/synthetic/structured_fsk_868.sub", "r");
    if(fp == NULL) fp = fopen("fixtures/synthetic/structured_fsk_868.sub", "r");
    if(fp == NULL) {
        printf("  (skip: fixture not reachable from cwd)\n");
        return;
    }
    int16_t pulses[256];
    uint16_t n = 0;
    char line[2048];
    while(n < 256 && fgets(line, sizeof(line), fp) != NULL) {
        n = radiotchi_parse_raw_data(line, pulses, 256, n);
    }
    fclose(fp);

    uint8_t frame[RADIOTCHI_FSK_FRAME_MAX];
    uint16_t flen = 0;
    CHECK(n > 0, "parsed pulses from the on-disk FSK fixture");
    CHECK(radiotchi_fsk_sensor_decode(pulses, n, frame, sizeof(frame), &flen), "the FSK fixture decodes");
    CHECK(
        flen == 3 && frame[0] == 0xAA && frame[1] == 0xC5 && frame[2] == 0x3D,
        "the FSK fixture yields AA C5 3D");
}

// Regression: ambient noise (no transmitter) must NOT decode to a false FSK frame either.
static void test_fsk_noise_gate(void) {
    printf("fsk noise gate:\n");
    FILE* fp = fopen("../fixtures/real/noise_868.sub", "r");
    if(fp == NULL) fp = fopen("fixtures/real/noise_868.sub", "r");
    if(fp == NULL) {
        printf("  (skip: fixture not reachable from cwd)\n");
        return;
    }
    int16_t pulses[RADIOTCHI_PULSES_MAX];
    uint16_t n = 0;
    char line[4096];
    while(n < RADIOTCHI_PULSES_MAX && fgets(line, sizeof(line), fp) != NULL) {
        n = radiotchi_parse_raw_data(line, pulses, RADIOTCHI_PULSES_MAX, n);
    }
    fclose(fp);

    uint8_t frame[RADIOTCHI_FSK_FRAME_MAX];
    uint16_t flen = 0;
    CHECK(n > 0, "parsed pulses from the real noise capture");
    CHECK(
        !radiotchi_fsk_sensor_decode(pulses, n, frame, sizeof(frame), &flen),
        "real ambient noise does NOT decode to a (false) FSK frame");
}

static void test_individual_fingerprint_bytes(void) {
    printf("individual fingerprint (bytes):\n");
    const uint8_t a_frame[3] = {0xAA, 0xC5, 0x3D};
    const uint8_t b_frame[3] = {0xAA, 0xC5, 0x3C};
    char a[RADIOTCHI_INDIVIDUAL_LEN], b[RADIOTCHI_INDIVIDUAL_LEN], c[RADIOTCHI_INDIVIDUAL_LEN];
    radiotchi_individual_fingerprint_bytes(a_frame, 3, a, sizeof(a));
    radiotchi_individual_fingerprint_bytes(a_frame, 3, b, sizeof(b)); // same frame, same tag
    radiotchi_individual_fingerprint_bytes(b_frame, 3, c, sizeof(c)); // different frame
    CHECK(strcmp(a, b) == 0, "same frame => stable tag (recurrence is observable)");
    CHECK(strcmp(a, c) != 0, "a different frame => a different tag");
    CHECK(strncmp(a, "id-", 3) == 0, "tag is the id- form");
    // Length is folded in: same leading bytes, different length => different tag.
    radiotchi_individual_fingerprint_bytes(a_frame, 2, c, sizeof(c));
    CHECK(strcmp(a, c) != 0, "frame length disambiguates a shared prefix");
}

// --- diff-learning byte classifier ------------------------------------------

static void test_byte_diff(void) {
    printf("byte diff:\n");
    // STATIC: identical payloads => every byte is an identifier/fixed field.
    const uint8_t s0[4] = {0x11, 0x22, 0x33, 0x44};
    const uint8_t s1[4] = {0x11, 0x22, 0x33, 0x44};
    const uint8_t s2[4] = {0x11, 0x22, 0x33, 0x44};
    const uint8_t* sp[3] = {s0, s1, s2};
    uint16_t sl[3] = {4, 4, 4};
    ByteDiff ds = radiotchi_byte_diff(sp, sl, 3);
    CHECK(ds.width == 4 && ds.count == 3, "static: 4 positions over 3 frames");
    CHECK(ds.cls[0] == BYTE_STATIC && ds.cls[3] == BYTE_STATIC, "identical bytes => STATIC");

    // INCREMENTING (incl. uint8 wrap) vs VARYING in one set.
    const uint8_t i0[3] = {0x10, 0xFE, 0x20};
    const uint8_t i1[3] = {0x11, 0xFF, 0x55};
    const uint8_t i2[3] = {0x12, 0x00, 0x33};
    const uint8_t* ip[3] = {i0, i1, i2};
    uint16_t il[3] = {3, 3, 3};
    ByteDiff di = radiotchi_byte_diff(ip, il, 3);
    CHECK(di.cls[0] == BYTE_INCREMENTING, "constant +1 step => INCREMENTING");
    CHECK(di.cls[1] == BYTE_INCREMENTING, "0xFE,0xFF,0x00 wraps cleanly => INCREMENTING");
    CHECK(di.cls[2] == BYTE_VARYING, "no constant step => VARYING");

    // ABSENT: ragged lengths => positions past the shortest frame are absent.
    const uint8_t r0[2] = {0xAB, 0xCD};
    const uint8_t r1[4] = {0xAB, 0xCD, 0xEF, 0x01};
    const uint8_t* rp[2] = {r0, r1};
    uint16_t rl[2] = {2, 4};
    ByteDiff dr = radiotchi_byte_diff(rp, rl, 2);
    CHECK(dr.width == 4, "width spans the longest frame");
    CHECK(dr.cls[0] == BYTE_STATIC, "the shared prefix is classified normally");
    CHECK(dr.cls[2] == BYTE_ABSENT && dr.cls[3] == BYTE_ABSENT, "bytes past the shortest => ABSENT");

    // Fewer than 2 frames => empty diff.
    ByteDiff d1 = radiotchi_byte_diff(sp, sl, 1);
    CHECK(d1.width == 0, "a single frame has no diff");

    // Determinism: same inputs => byte-identical result.
    ByteDiff a = radiotchi_byte_diff(ip, il, 3);
    ByteDiff b = radiotchi_byte_diff(ip, il, 3);
    CHECK(memcmp(&a, &b, sizeof(ByteDiff)) == 0, "byte diff is deterministic");
}

// --- individual-scoped diff (group a species' frames by device tag) ----------

static void test_select_by_individual(void) {
    printf("select by individual:\n");
    // Two devices of one band-species: A = {fixed id 0xA1, incrementing counter},
    // B = {fixed id 0xB7, ...}. Plus one UNTAGGED row (a D28 no-id VALUES capture).
    const uint8_t a0[3] = {0xA1, 0x10, 0x00};
    const uint8_t a1[3] = {0xA1, 0x11, 0x00};
    const uint8_t b0[3] = {0xB7, 0x55, 0x00};
    const uint8_t u0[3] = {0xC3, 0x99, 0x00};
    const uint8_t* pay[4] = {a0, b0, a1, u0}; // interleaved, as the log might store them
    uint16_t len[4] = {3, 3, 3, 3};
    const char* tags[4] = {"id-aaaa", "id-bbbb", "id-aaaa", ""};

    const uint8_t* out[4];
    uint16_t ol[4];

    // Device-scoped: only device A's two frames, in order.
    uint8_t na = radiotchi_select_by_individual(tags, pay, len, 4, "id-aaaa", out, ol, 4);
    CHECK(na == 2, "select id-aaaa => exactly device A's 2 frames");
    CHECK(out[0] == a0 && out[1] == a1, "matched frames are in log order");
    ByteDiff da = radiotchi_byte_diff(out, ol, na);
    CHECK(da.cls[0] == BYTE_STATIC, "device-scoped: the id byte reads STATIC");
    CHECK(da.cls[1] == BYTE_INCREMENTING, "device-scoped: the counter reads INCREMENTING");

    // Regression guard: WITHOUT scoping, A+B's differing id bytes smear to VARYING —
    // the exact bug device-scoping fixes (decision-log D34).
    ByteDiff dmix = radiotchi_byte_diff(pay, len, 4);
    CHECK(dmix.cls[0] == BYTE_VARYING, "mixed devices: the id byte mis-reads as VARYING");

    // Untagged passthrough: want=="" selects only the empty-tag row.
    uint8_t nu = radiotchi_select_by_individual(tags, pay, len, 4, "", out, ol, 4);
    CHECK(nu == 1 && out[0] == u0, "want=\"\" => only the untagged row");

    // want==NULL selects ALL rows (the species-wide path).
    uint8_t nall = radiotchi_select_by_individual(tags, pay, len, 4, NULL, out, ol, 4);
    CHECK(nall == 4, "want==NULL => every row (species-wide)");

    // Capacity clamp: never writes past `cap`.
    uint8_t nclamp = radiotchi_select_by_individual(tags, pay, len, 4, NULL, out, ol, 2);
    CHECK(nclamp == 2, "cap clamps the match count");

    // Determinism: re-running the same selection yields the same frames in the same order.
    const uint8_t* o2[4];
    uint16_t l2[4];
    uint8_t n2 = radiotchi_select_by_individual(tags, pay, len, 4, "id-aaaa", o2, l2, 4);
    CHECK(n2 == 2 && o2[0] == a0 && o2[1] == a1, "selection is deterministic");
}

int main(void) {
    printf("== Radiotchi analysis_core host tests ==\n");
    test_entropy();
    test_axes();
    test_degradation();
    test_classify();
    test_classify_boundaries();
    test_fingerprint();
    test_regrade();
    test_ook_decode();
    test_decoder_robustness();
    test_repeating_frame();
    test_manchester_decode();
    test_parse_raw_data();
    test_fixture_sub();
    test_real_noise_fixture();
    test_pipeline_real_noise();
    test_fw_decode();
    test_species_branding();
    test_individual_fingerprint();
    test_values_tier();
    test_fsk_decode();
    test_fsk_values();
    test_fsk_fixture_sub();
    test_fsk_noise_gate();
    test_individual_fingerprint_bytes();
    test_byte_diff();
    test_select_by_individual();
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
