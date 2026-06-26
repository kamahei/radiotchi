// Radiotchi — Analysis Core implementation (pure; host + Flipper).
//
// No furi / GUI / storage / clock / hardware includes here — only the C stdlib.
// Numeric axis calibration is deliberately provisional (docs/open-questions.md
// Q3); the axis *definitions* are fixed and must not drift.

#include "analysis_core.h"

#include <stdio.h>
#include <string.h>

// --- helpers ---------------------------------------------------------------

static float clampf(float v, float lo, float hi) {
    if(v < lo) return lo;
    if(v > hi) return hi;
    return v;
}

// log2f via the IEEE-754 exponent + a small mantissa polynomial. The Flipper
// firmware does NOT export libm's log/log2f, and keeping the core libm-free also
// keeps it trivially portable for host tests. Accurate to ~0.01 bits, which is
// far finer than entropy scoring needs.
static float wv_log2f(float x) {
    if(x <= 0.0f) return 0.0f;
    union {
        float f;
        uint32_t i;
    } v = {x};
    float exponent = (float)((int)((v.i >> 23) & 0xFFu) - 127);
    v.i = (v.i & 0x007FFFFFu) | 0x3F800000u; // mantissa m in [1, 2)
    float m = v.f;
    float poly = (-0.34484843f * m + 2.02466578f) * m - 1.67487759f; // ~log2(m)
    return exponent + poly;
}

// Coarse band bucket for fingerprinting, so 433.92 and 433.90 share a species.
static uint32_t band_bucket_mhz(uint32_t frequency_hz) {
    return frequency_hz / 1000000u; // 1 MHz granularity
}

// Length bucket (power-of-two-ish) so near-equal lengths share a species.
static uint16_t length_bucket(uint16_t bit_count) {
    uint16_t bucket = 1;
    while(bucket < bit_count && bucket < 0x4000) bucket <<= 1;
    return bucket;
}

// --- entropy ---------------------------------------------------------------

float radiotchi_shannon_entropy(const uint8_t* data, size_t len) {
    if(data == NULL || len == 0) return 0.0f;

    uint32_t freq[256] = {0};
    for(size_t i = 0; i < len; i++) freq[data[i]]++;

    float entropy = 0.0f;
    const float inv_len = 1.0f / (float)len;
    for(int sym = 0; sym < 256; sym++) {
        if(freq[sym] == 0) continue;
        float p = (float)freq[sym] * inv_len;
        entropy -= p * wv_log2f(p); // -sum p*log2(p)
    }
    return entropy; // 0.0 .. 8.0 bits/byte
}

// --- fingerprint species ---------------------------------------------------

void radiotchi_fingerprint_species(
    uint32_t frequency_hz,
    Modulation modulation,
    uint16_t bit_count,
    char* out,
    size_t out_len) {
    if(out == NULL || out_len == 0) return;

    // e.g. "F433-M1-L0256": frequency band, modulation, length bucket. Stable for
    // similar unknowns; a decoder later replaces this with a named species.
    unsigned band = (unsigned)band_bucket_mhz(frequency_hz);
    unsigned mod = (unsigned)modulation;
    unsigned len = (unsigned)length_bucket(bit_count);

    snprintf(out, out_len, "F%u-M%u-L%04u", band, mod, len);
    out[out_len - 1] = '\0';
}

// --- axis scoring (provisional calibration; definitions fixed) -------------

// Calories (Volume): more bits => more calories. Saturates so a giant burst does
// not dominate. ~256 bits maps near full scale.
static float score_calories(uint16_t bit_count) {
    return clampf((float)bit_count / 256.0f, 0.0f, 1.0f);
}

// Freshness (Strength): map RSSI in [-100, -30] dBm to [0, 1].
static float score_freshness(int16_t rssi_dbm) {
    float v = ((float)rssi_dbm + 100.0f) / 70.0f;
    return clampf(v, 0.0f, 1.0f);
}

// Additives (Entropy): normalize 0..8 bits/byte to 0..1. High = junk.
static float score_additives(float entropy) {
    return clampf(entropy / 8.0f, 0.0f, 1.0f);
}

// Rarity (personal): rarer (fewer prior sightings, relative to the dex) scores
// higher. An empty/NULL view => maximally rare (1.0).
static float score_rarity(const RarityView* view) {
    if(view == NULL || view->total_captures == 0) return 1.0f;
    float frequency = (float)view->seen_count / (float)view->total_captures;
    return clampf(1.0f - frequency, 0.0f, 1.0f);
}

// Nourishment (Structure): decode depth ladder.
float radiotchi_tier_nourishment(DecodeTier tier) {
    switch(tier) {
    case TIER_RAW:
        return 0.0f;
    case TIER_MODULATION:
        return 0.33f;
    case TIER_PROTOCOL:
        return 0.66f;
    case TIER_VALUES:
        return 1.0f;
    default:
        return 0.0f;
    }
}

static float score_nourishment(DecodeTier tier) {
    return radiotchi_tier_nourishment(tier);
}

// --- real demodulation: OOK PWM fixed-code (EV1527/PT2262 family) ----------
//
// Unlike the signature classifier below, this actually reads the bits from the pulse
// timing, so it can reach TIER_VALUES. PWM fixed-code: each bit is one (mark, space)
// pair; a "1" has a long mark + short space, a "0" the reverse — so we decide each bit
// threshold-free by comparing the two magnitudes. Frames are separated by a long sync
// gap (a space much longer than a bit cell). If the burst repeats the frame (remotes
// usually do), the repeat must match for us to trust the decode.
#define WV_PWM_MIN_BITS 8u // a real fixed-code, not a stray blip
#define WV_PWM_MAX_BITS 32u // fits a uint32 code; longer is some other protocol
// A sync gap is a space at least GAP_MULT x the short unit OR GAP_FLOOR us, whichever is
// larger — so it still separates the gap from "long" bits when long >> short (real captures
// can have an 8:1 long:short ratio, where a pure 5x-short threshold misreads longs as gaps).
#define WV_PWM_GAP_MULT  8u
#define WV_PWM_GAP_FLOOR 2000u // a real fixed-code sync gap is several ms
#define WV_PWM_GLITCH_US 50u // ignore sub-50us spikes when estimating the short unit (real
                             // RX has ~30us noise glitches that would poison a raw min())

static uint32_t abs32(int32_t v) {
    return (uint32_t)(v < 0 ? -(int64_t)v : (int64_t)v);
}

// Decode the first frame starting at pulses[start]; returns bits via *code/*nbits and
// the index just past the frame via *next. false if no plausible frame is found.
static bool decode_pwm_frame(
    const int16_t* pulses,
    uint16_t n,
    uint16_t start,
    uint32_t short_unit,
    uint32_t* code,
    uint8_t* nbits,
    uint16_t* next) {
    uint32_t gap = short_unit * WV_PWM_GAP_MULT;
    if(gap < WV_PWM_GAP_FLOOR) gap = WV_PWM_GAP_FLOOR;
    // PWM bit value is set by the MARK width (short->0, long->1), not mark-vs-space: the last
    // bit before the sync gap has space==gap, so comparing to the space would misread a long
    // mark (a code ending in 1) as 0. Threshold at 1.5x the short unit cleanly splits short
    // (~1x) from long (~3x) marks.
    uint32_t mark_thresh = short_unit + short_unit / 2u;
    uint32_t bits = 0;
    uint8_t count = 0;
    uint16_t i = start;
    for(; i + 1 < n; i += 2) {
        uint32_t mark = abs32(pulses[i]);
        uint32_t space = abs32(pulses[i + 1]);
        if(count >= WV_PWM_MAX_BITS) return false; // overran a sane code length
        bits = (bits << 1) | (mark > mark_thresh ? 1u : 0u);
        count++;
        if(space >= gap) { // long sync gap: frame ends after this bit
            i += 2;
            break;
        }
    }
    if(count < WV_PWM_MIN_BITS) return false;
    if(code) *code = bits;
    if(nbits) *nbits = count;
    if(next) *next = i;
    return true;
}

void radiotchi_individual_fingerprint(uint32_t code, uint8_t nbits, char* out, size_t out_len) {
    if(out == NULL || out_len == 0) return;
    // FNV-1a over the code bytes + bit width, folded to 16 bits. One-way: the dex shows a
    // stable "id-XXXX" per device for diff-learning, but the raw code is unrecoverable (A5).
    uint32_t h = 2166136261u;
    for(int b = 0; b < 4; b++) {
        h ^= (code >> (b * 8)) & 0xFFu;
        h *= 16777619u;
    }
    h ^= nbits;
    h *= 16777619u;
    uint16_t tag = (uint16_t)((h ^ (h >> 16)) & 0xFFFFu);
    snprintf(out, out_len, "id-%04x", (unsigned)tag);
    out[out_len - 1] = '\0';
}

uint16_t radiotchi_parse_raw_data(const char* line, int16_t* out, uint16_t cap, uint16_t have) {
    if(line == NULL || out == NULL) return have;

    const char* p = line;
    while(*p == ' ' || *p == '\t') p++;
    const char* kw = "RAW_Data:";
    for(uint8_t i = 0; kw[i] != '\0'; i++) {
        if(p[i] != kw[i]) return have; // not a pulse line (header etc.)
    }
    p += 9; // past "RAW_Data:"

    uint16_t k = have;
    while(*p != '\0' && k < cap) {
        while(*p == ' ' || *p == '\t') p++;
        if(*p == '\0') break;
        int sign = 1;
        if(*p == '-') {
            sign = -1;
            p++;
        } else if(*p == '+') {
            p++;
        }
        if(*p < '0' || *p > '9') { // stray token; skip to the next space
            while(*p != '\0' && *p != ' ' && *p != '\t') p++;
            continue;
        }
        long v = 0;
        while(*p >= '0' && *p <= '9') {
            v = v * 10 + (*p - '0');
            if(v > 1000000) v = 1000000; // guard before the int16 clamp
            p++;
        }
        v *= sign;
        if(v > INT16_MAX) v = INT16_MAX;
        if(v < INT16_MIN) v = INT16_MIN;
        out[k++] = (int16_t)v;
    }
    return k;
}

bool radiotchi_ook_pwm_decode(const int16_t* pulses, uint16_t n, uint32_t* code, uint8_t* nbits) {
    if(pulses == NULL || n < WV_PWM_MIN_BITS * 2u) return false;

    // Skip a leading space (silence) so we start on a mark, and find the short unit
    // (the smallest bit-cell duration) to set the sync-gap threshold.
    uint16_t start = 0;
    if(pulses[0] < 0) start = 1; // first entry is a space => begin at the next mark
    uint32_t short_unit = 0;
    for(uint16_t i = start; i < n; i++) {
        uint32_t m = abs32(pulses[i]);
        if(m < WV_PWM_GLITCH_US) continue; // skip noise spikes so they can't poison the min
        if(short_unit == 0 || m < short_unit) short_unit = m;
    }
    if(short_unit == 0) return false;

    uint32_t code0 = 0;
    uint8_t nbits0 = 0;
    uint16_t next = 0;
    if(!decode_pwm_frame(pulses, n, start, short_unit, &code0, &nbits0, &next)) return false;

    // REQUIRE a second frame that agrees. Real fixed-code remotes transmit the code several
    // times per button press; ambient noise does not repeat. Rejecting lone/unconfirmed
    // frames is what stops noise from yielding a false VALUES — on real captures the old
    // lone-accept rule produced 13/13 distinct "codes" from pure noise.
    if(next + WV_PWM_MIN_BITS * 2u > n) return false; // no room for a confirming repeat
    uint32_t code1 = 0;
    uint8_t nbits1 = 0;
    uint16_t dummy = 0;
    if(!decode_pwm_frame(pulses, n, next, short_unit, &code1, &nbits1, &dummy)) return false;
    if(code1 != code0 || nbits1 != nbits0) return false; // frames disagree => distrust

    if(code) *code = code0;
    if(nbits) *nbits = nbits0;
    return true;
}

// --- real demodulation: OOK Manchester fixed-code ---------------------------
//
// Manchester encodes each bit as a mid-bit level transition, so the line carries runs of one
// HALF-bit (the bit's two halves differ) or two half-bits (a half shared across a same-level bit
// boundary). We estimate the half-bit unit as the glitch-filtered robust-min run, expand each run
// back into round(dur/half-unit) half-bit samples, then recover bits by pairing samples at the
// phase whose every pair is a transition (the data framing). Two candidate phases exist; the wrong
// one hits a non-transition (a==b) at the first "10"/"01" bit adjacency, which is how we lock the
// right framing. As with the PWM/FSK decoders, only a frame confirmed by an immediately-following
// identical repeat is trusted (real remotes retransmit; noise does not).
#define WV_MAN_GLITCH_US 50u // ignore sub-50us spikes when estimating the half-bit unit
#define WV_MAN_MIN_BITS  8u // a real fixed-code, not a stray blip
#define WV_MAN_MAX_BITS  32u // fits a uint32 code
#define WV_MAN_GAP_MULT  5u // a run >= this x the half-bit unit (or the floor) ends a frame
#define WV_MAN_GAP_FLOOR 2000u // ...or an absolute several-ms inter-frame gap
#define WV_MAN_RUN_CAP   3u // a sub-gap run longer than 3 half-bits is not clean Manchester
#define WV_MAN_SAMP_MAX  72u // half-bit samples buffered per frame (>= 2*MAX_BITS + slack)

// Decode the first Manchester frame at pulses[start]: rebuild its half-bit samples (to the gap),
// then phase-search them into bits. Returns bits via *code/*nbits and the index past the frame via
// *next. false if no plausible frame (too few bits, an anomalous run, or no valid phase).
static bool decode_manchester_frame(
    const int16_t* pulses,
    uint16_t n,
    uint16_t start,
    uint32_t half_unit,
    uint32_t* code,
    uint8_t* nbits,
    uint16_t* next) {
    uint32_t gap = half_unit * WV_MAN_GAP_MULT;
    if(gap < WV_MAN_GAP_FLOOR) gap = WV_MAN_GAP_FLOOR;

    uint8_t samp[WV_MAN_SAMP_MAX];
    uint16_t m = 0;
    uint16_t i = start;
    bool started = false;
    for(; i < n; i++) {
        uint32_t mag = abs32(pulses[i]);
        if(mag < WV_MAN_GLITCH_US) continue; // drop a noise spike, stay in the frame
        if(mag >= gap) { // inter-frame gap
            if(started) {
                i++; // consume the gap so the next frame starts past it
                break;
            }
            continue; // leading silence before the frame: skip it
        }
        uint32_t q = (mag + half_unit / 2u) / half_unit; // round to whole half-bits
        if(q < 1u) q = 1u;
        if(q > WV_MAN_RUN_CAP) return false; // not a clean Manchester run
        uint8_t level = (pulses[i] > 0) ? 1u : 0u;
        started = true;
        for(uint32_t r = 0; r < q && m < WV_MAN_SAMP_MAX; r++) samp[m++] = level;
    }
    if(m < WV_MAN_MIN_BITS * 2u) return false;

    // Phase search: the correct framing pairs every (a,b) half-bit as a transition.
    for(uint8_t phase = 0; phase < 2; phase++) {
        uint32_t bits = 0;
        uint8_t cnt = 0;
        bool ok = true;
        for(uint16_t k = phase; (uint16_t)(k + 1) < m; k += 2) {
            uint8_t a = samp[k];
            uint8_t b = samp[k + 1];
            if(a == b) { // not a valid mid-bit transition for this phase
                ok = false;
                break;
            }
            if(cnt >= WV_MAN_MAX_BITS) {
                ok = false;
                break;
            }
            bits = (bits << 1) | (uint32_t)((a == 0u && b == 1u) ? 1u : 0u); // low->high = 1
            cnt++;
        }
        if(ok && cnt >= WV_MAN_MIN_BITS) {
            if(code) *code = bits;
            if(nbits) *nbits = cnt;
            if(next) *next = i;
            return true;
        }
    }
    return false;
}

bool radiotchi_manchester_decode(
    const int16_t* pulses, uint16_t n, uint32_t* code, uint8_t* nbits) {
    if(pulses == NULL || n < WV_MAN_MIN_BITS * 2u) return false;

    // Half-bit unit = the glitch-filtered robust-minimum run (gap-scale runs are far larger).
    uint32_t half_unit = 0;
    for(uint16_t i = 0; i < n; i++) {
        uint32_t m = abs32(pulses[i]);
        if(m < WV_MAN_GLITCH_US) continue;
        if(half_unit == 0 || m < half_unit) half_unit = m;
    }
    if(half_unit == 0) return false;

    uint32_t code0 = 0, code1 = 0;
    uint8_t nbits0 = 0, nbits1 = 0;
    uint16_t next = 0, dummy = 0;
    if(!decode_manchester_frame(pulses, n, 0, half_unit, &code0, &nbits0, &next)) return false;

    // REQUIRE a confirming repeat with identical bits (the OOK/FSK noise guard, for Manchester).
    if(next + WV_MAN_MIN_BITS * 2u > n) return false; // no room for a confirming repeat
    if(!decode_manchester_frame(pulses, n, next, half_unit, &code1, &nbits1, &dummy)) return false;
    if(code1 != code0 || nbits1 != nbits0) return false; // frames disagree => distrust

    if(code) *code = code0;
    if(nbits) *nbits = nbits0;
    return true;
}

// --- real demodulation: 2FSK sensor frame (PCM/NRZ weather/telemetry/TPMS) -----
//
// The firmware's async-RAW slicer turns the 2FSK discriminator output into the SAME
// signed level/duration pulse train shape as OOK, so we demodulate the bit-level
// transition timing here (not I/Q), keeping the core libm-free. The sensor class is
// overwhelmingly PCM/NRZ at a fixed bit period: estimate the period as the robust-min
// run, expand each run into round(dur/period) NRZ bits (mark=1, space=0), and split
// frames at the long inter-frame gap. As with the OOK decoder we only trust a frame an
// immediately-following repeat confirms byte-for-byte (real sensors retransmit; ambient
// noise does not) — that repeat guard is what stops noise from faking a frame.
#define WV_FSK_GLITCH_US 40u // ignore sub-40us spikes when estimating the bit period
#define WV_FSK_GAP_MULT  16u // a run >= this x the bit period (or the floor) ends a frame
#define WV_FSK_GAP_FLOOR 4000u // ...or an absolute several-ms inter-frame gap
#define WV_FSK_MIN_BITS  16u // a real sensor frame, not a stray blip
#define WV_FSK_RUN_CAP   32u // one NRZ run expanding past this is anomalous (not clean PCM)
#define WV_FSK_MIN_RUNS  6u // a real PCM frame toggles often; reject trivial 1-2 run noise frames
                            // (coarse run-quantization lets two such frames collide — fuzz finding)

// Decode the first NRZ frame at pulses[start] into packed bytes (MSB-first). A frame ends
// at the long inter-frame gap. Returns the bit length via *nbits and the index just past the
// frame via *next; false if no plausible frame (too few bits, or an anomalous long run).
static bool decode_fsk_frame(
    const int16_t* pulses,
    uint16_t n,
    uint16_t start,
    uint32_t bit_period,
    uint8_t* bytes,
    uint16_t cap,
    uint16_t* nbits,
    uint16_t* next) {
    uint32_t gap = bit_period * WV_FSK_GAP_MULT;
    if(gap < WV_FSK_GAP_FLOOR) gap = WV_FSK_GAP_FLOOR;
    for(uint16_t b = 0; b < cap; b++) bytes[b] = 0;

    uint16_t count = 0; // bits accumulated
    uint16_t runs = 0; // level runs (transitions) consumed in this frame
    uint16_t i = start;
    bool started = false;
    for(; i < n; i++) {
        uint32_t mag = abs32(pulses[i]);
        if(mag < WV_FSK_GLITCH_US) continue; // drop a noise spike, stay in the frame
        if(mag >= gap) { // inter-frame gap
            if(started) {
                i++; // consume the gap so the next frame starts past it
                break;
            }
            continue; // leading silence before the frame: skip it
        }
        started = true;
        runs++;
        uint32_t runbits = (mag + bit_period / 2u) / bit_period; // round to whole bits
        if(runbits < 1u) runbits = 1u;
        if(runbits > WV_FSK_RUN_CAP) return false; // not a clean NRZ run
        uint8_t bit = (pulses[i] > 0) ? 1u : 0u; // mark=1, space=0
        for(uint32_t r = 0; r < runbits && count < (uint16_t)(cap * 8u); r++) {
            if(bit) bytes[count >> 3] |= (uint8_t)(0x80u >> (count & 7u));
            count++;
        }
    }
    if(count < WV_FSK_MIN_BITS) return false;
    if(runs < WV_FSK_MIN_RUNS) return false; // too few transitions => not a real PCM frame
    if(nbits) *nbits = count;
    if(next) *next = i;
    return true;
}

bool radiotchi_fsk_sensor_decode(
    const int16_t* pulses, uint16_t n, uint8_t* frame, uint16_t cap, uint16_t* frame_len) {
    if(pulses == NULL || frame == NULL || cap == 0 || n < WV_FSK_MIN_BITS) return false;

    // Bit period = the robust-minimum run (glitch-filtered); gap-scale runs are far larger,
    // so they never pull the min down.
    uint32_t bit_period = 0;
    for(uint16_t i = 0; i < n; i++) {
        uint32_t m = abs32(pulses[i]);
        if(m < WV_FSK_GLITCH_US) continue;
        if(bit_period == 0 || m < bit_period) bit_period = m;
    }
    if(bit_period == 0) return false;

    uint16_t fcap = cap < RADIOTCHI_FSK_FRAME_MAX ? cap : RADIOTCHI_FSK_FRAME_MAX;
    uint8_t f0[RADIOTCHI_FSK_FRAME_MAX];
    uint8_t f1[RADIOTCHI_FSK_FRAME_MAX];
    uint16_t nb0 = 0, nb1 = 0, next = 0, dummy = 0;
    if(!decode_fsk_frame(pulses, n, 0, bit_period, f0, fcap, &nb0, &next)) return false;

    // REQUIRE a confirming repeat with identical bytes (the OOK noise guard, for FSK). A lone
    // or disagreeing frame is distrusted, so ambient noise cannot reach VALUES.
    if(next >= n) return false;
    if(!decode_fsk_frame(pulses, n, next, bit_period, f1, fcap, &nb1, &dummy)) return false;
    if(nb0 != nb1) return false;
    uint16_t nbytes = (uint16_t)((nb0 + 7u) / 8u);
    if(nbytes > fcap) nbytes = fcap;
    bool all_same = true;
    for(uint16_t i = 0; i < nbytes; i++) {
        if(f0[i] != f1[i]) return false; // frames disagree => distrust
        if(f0[i] != f0[0]) all_same = false;
    }
    // Reject an all-identical frame (all-0 / all-1): a single long mark/space run between two gaps
    // expands to a uniform byte string, and noise that happens to repeat that shape would otherwise
    // fake a frame. A real sensor payload varies. (Found by the random-pulse fuzz harness.)
    if(all_same) return false;
    for(uint16_t i = 0; i < nbytes; i++) frame[i] = f0[i];
    if(frame_len) *frame_len = nbytes;
    return true;
}

void radiotchi_individual_fingerprint_bytes(
    const uint8_t* frame, uint16_t len, char* out, size_t out_len) {
    if(out == NULL || out_len == 0) return;
    // FNV-1a over the frame bytes + length, folded to 16 bits (same one-way scheme as
    // radiotchi_individual_fingerprint): a stable per-device "id-XXXX" the raw frame cannot
    // be recovered from (A5).
    uint32_t h = 2166136261u;
    if(frame != NULL) {
        for(uint16_t i = 0; i < len; i++) {
            h ^= frame[i];
            h *= 16777619u;
        }
    }
    h ^= (uint8_t)(len & 0xFFu);
    h *= 16777619u;
    uint16_t tag = (uint16_t)((h ^ (h >> 16)) & 0xFFFFu);
    snprintf(out, out_len, "id-%04x", (unsigned)tag);
    out[out_len - 1] = '\0';
}

// --- diff-learning: classify aligned decoded frames byte-by-byte ---------------
//
// Given N captures' decoded frames of the same device, classify each byte position so the dex
// can teach reverse-engineering by play: a byte equal across all frames is an identifier/fixed
// field (STATIC); one stepping by a constant non-zero amount (mod 256, so a wrapping counter
// reads cleanly) is a rolling counter (INCREMENTING); anything else is a sensor value/payload
// (VARYING); positions past the shortest frame are ABSENT. Integer-only & deterministic.
ByteDiff radiotchi_byte_diff(const uint8_t* const* payloads, const uint16_t* lens, uint8_t count) {
    ByteDiff d;
    memset(&d, 0, sizeof(d));
    d.count = count;
    if(payloads == NULL || lens == NULL || count < 2) {
        d.width = 0;
        return d;
    }

    uint16_t minlen = lens[0];
    uint16_t maxlen = lens[0];
    for(uint8_t i = 1; i < count; i++) {
        if(lens[i] < minlen) minlen = lens[i];
        if(lens[i] > maxlen) maxlen = lens[i];
    }
    uint16_t width = maxlen > RADIOTCHI_DIFF_BYTES_MAX ? RADIOTCHI_DIFF_BYTES_MAX : maxlen;
    d.width = width;

    for(uint16_t p = 0; p < width; p++) {
        if(p >= minlen) { // ragged: at least one frame lacks this byte
            d.cls[p] = BYTE_ABSENT;
            continue;
        }
        bool all_equal = true;
        for(uint8_t i = 1; i < count; i++) {
            if(payloads[i][p] != payloads[0][p]) {
                all_equal = false;
                break;
            }
        }
        if(all_equal) {
            d.cls[p] = BYTE_STATIC;
            continue;
        }
        uint8_t step = (uint8_t)(payloads[1][p] - payloads[0][p]);
        bool incrementing = (step != 0);
        for(uint8_t i = 1; incrementing && (uint8_t)(i + 1) < count; i++) {
            uint8_t s = (uint8_t)(payloads[i + 1][p] - payloads[i][p]);
            if(s != step) incrementing = false;
        }
        d.cls[p] = incrementing ? BYTE_INCREMENTING : BYTE_VARYING;
    }
    return d;
}

uint8_t radiotchi_select_by_individual(
    const char* const* tags,
    const uint8_t* const* payloads,
    const uint16_t* lens,
    uint8_t count,
    const char* want,
    const uint8_t** out_payloads,
    uint16_t* out_lens,
    uint8_t cap) {
    if(tags == NULL || payloads == NULL || lens == NULL || out_payloads == NULL ||
       out_lens == NULL || cap == 0)
        return 0;
    uint8_t k = 0;
    for(uint8_t i = 0; i < count && k < cap; i++) {
        bool match;
        if(want == NULL) {
            match = true; // species-wide: every row
        } else if(want[0] == '\0') {
            match = (tags[i] == NULL || tags[i][0] == '\0'); // untagged rows only
        } else {
            match = (tags[i] != NULL && strcmp(tags[i], want) == 0); // exact device tag
        }
        if(!match) continue;
        out_payloads[k] = payloads[i];
        out_lens[k] = lens[i];
        k++;
    }
    return k;
}

// --- encoding-agnostic repeating-frame detector (the general VALUES path) ----
//
// The PWM bit decoder only reads one encoding. Real remotes use many (Manchester, PCM, ...),
// but they ALL retransmit a fixed frame several times per button press, while ambient noise
// never repeats. So: split the pulse train into frames at the long inter-frame gap, quantize
// each frame to coarse time units (absorbing jitter), and if >= 2 frames are identical it is
// a confirmed fixed transmission -> VALUES, fingerprinted by hashing the canonical frame. The
// jittery sync header (the first, variable-length pulse) is excluded so the fingerprint is
// stable across captures of the same remote (validated on real 315 MHz captures).
#define WV_FRAME_GLITCH 40u // ignore sub-40us noise spikes
#define WV_FRAME_GAP 1500u // a space this long ends a frame (inter-frame sync gap)
#define WV_FRAME_QUANT 100u // quantize durations to 100us buckets (jitter tolerance)
#define WV_FRAME_PREFIX 24 // canonical = this many units after the sync header
#define WV_FRAME_MAXFRAMES 16u // distinct frame hashes tracked per capture

bool radiotchi_repeating_frame(const int16_t* pulses, uint16_t n, uint16_t* fp) {
    if(pulses == NULL || n < (WV_FRAME_PREFIX + 1)) return false;

    uint32_t seen[WV_FRAME_MAXFRAMES];
    uint8_t nseen = 0;
    int8_t buf[WV_FRAME_PREFIX + 1]; // sync header (buf[0]) + canonical (buf[1..PREFIX])
    int blen = 0; // total elements in the current frame (only first PREFIX+1 are buffered)

    for(uint16_t i = 0; i <= n; i++) {
        bool boundary = (i == n); // flush the final frame at the end
        uint32_t mag = 0;
        if(!boundary) {
            int16_t d = pulses[i];
            mag = (uint32_t)(d < 0 ? -d : d);
            if(mag < WV_FRAME_GLITCH) continue; // drop a noise spike, stay in the frame
            boundary = (mag > WV_FRAME_GAP);
        }

        if(boundary) {
            if(blen >= WV_FRAME_PREFIX + 1 && buf[0] >= 3) { // a full frame with a sync header
                uint32_t h = 2166136261u;
                for(int k = 1; k <= WV_FRAME_PREFIX; k++) {
                    h ^= (uint8_t)(buf[k] + 16);
                    h *= 16777619u;
                }
                for(uint8_t s = 0; s < nseen; s++) {
                    if(seen[s] == h) { // a second identical frame: confirmed
                        if(fp) *fp = (uint16_t)((h ^ (h >> 16)) & 0xFFFFu);
                        return true;
                    }
                }
                if(nseen < WV_FRAME_MAXFRAMES) seen[nseen++] = h;
            }
            blen = 0;
            continue;
        }

        int u = (int)((mag + WV_FRAME_QUANT / 2u) / WV_FRAME_QUANT); // round to 100us units
        if(u < 1) u = 1;
        if(u > 15) u = 15;
        if(blen < WV_FRAME_PREFIX + 1) buf[blen] = (int8_t)(pulses[i] < 0 ? -u : u);
        blen++;
    }
    return false;
}

// --- decoder toolkit (reusable building blocks for device-protocol decoders) -----

uint8_t radiotchi_crc8(const uint8_t* data, uint16_t len, uint8_t poly, uint8_t init) {
    uint8_t crc = init;
    if(data == NULL) return crc;
    for(uint16_t i = 0; i < len; i++) {
        crc ^= data[i];
        for(uint8_t b = 0; b < 8; b++) {
            crc = (crc & 0x80u) ? (uint8_t)((crc << 1) ^ poly) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

uint8_t radiotchi_checksum8(const uint8_t* data, uint16_t len) {
    uint8_t sum = 0;
    if(data == NULL) return 0;
    for(uint16_t i = 0; i < len; i++) sum = (uint8_t)(sum + data[i]);
    return sum;
}

uint8_t radiotchi_xor8(const uint8_t* data, uint16_t len) {
    uint8_t x = 0;
    if(data == NULL) return 0;
    for(uint16_t i = 0; i < len; i++) x ^= data[i];
    return x;
}

uint32_t radiotchi_bits_get(
    const uint8_t* bytes, uint16_t nbytes, uint16_t bit_off, uint8_t nbits) {
    uint32_t v = 0;
    if(bytes == NULL || nbits == 0 || nbits > 32) return 0;
    for(uint8_t k = 0; k < nbits; k++) {
        uint16_t pos = (uint16_t)(bit_off + k);
        uint8_t bit = 0;
        if((pos >> 3) < nbytes) bit = (uint8_t)((bytes[pos >> 3] >> (7u - (pos & 7u))) & 1u);
        v = (v << 1) | bit;
    }
    return v;
}

// Decode one OOK PWM (mark-coded) frame at pulses[start] into MSB-first bytes (long mark = 1, short
// = 0; split at 1.5x the short unit, robust even for a 1-terminated last bit since the bit is the
// MARK not the gap space). Ends at a space >= gap. Returns the bit count via *nbits, the index past
// the frame via *next; false if too short or overrun. Mirrors decode_pwm_frame but packs bytes.
static bool decode_pwm_bytes_frame(
    const int16_t* pulses,
    uint16_t n,
    uint16_t start,
    uint32_t short_unit,
    uint8_t* out,
    uint16_t cap_bytes,
    uint16_t* nbits,
    uint16_t* next) {
    uint32_t gap = short_unit * WV_PWM_GAP_MULT;
    if(gap < WV_PWM_GAP_FLOOR) gap = WV_PWM_GAP_FLOOR;
    uint32_t mark_thresh = short_unit + short_unit / 2u;
    uint16_t cap_bits = (uint16_t)(cap_bytes * 8u);
    for(uint16_t b = 0; b < cap_bytes; b++) out[b] = 0;

    uint16_t count = 0;
    uint16_t i = start;
    for(; i + 1 < n; i += 2) {
        uint32_t mark = abs32(pulses[i]);
        uint32_t space = abs32(pulses[i + 1]);
        if(count >= cap_bits) return false; // overran the byte buffer
        if(mark > mark_thresh) out[count >> 3] |= (uint8_t)(0x80u >> (count & 7u));
        count++;
        if(space >= gap) { // sync gap ends the frame
            i += 2;
            break;
        }
    }
    if(count < 8u) return false; // need at least a byte
    if(nbits) *nbits = count;
    if(next) *next = i;
    return true;
}

bool radiotchi_pwm_to_bytes(
    const int16_t* pulses, uint16_t n, uint8_t* out, uint16_t cap, uint16_t* nbits) {
    if(pulses == NULL || out == NULL || cap == 0 || n < 16u) return false;

    uint16_t start = 0;
    if(pulses[0] < 0) start = 1; // begin on a mark
    uint32_t short_unit = 0;
    for(uint16_t i = start; i < n; i++) {
        uint32_t m = abs32(pulses[i]);
        if(m < WV_PWM_GLITCH_US) continue;
        if(short_unit == 0 || m < short_unit) short_unit = m;
    }
    if(short_unit == 0) return false;

    uint8_t f0[RADIOTCHI_SENSOR_FRAME_MAX];
    uint8_t f1[RADIOTCHI_SENSOR_FRAME_MAX];
    uint16_t fcap = cap < RADIOTCHI_SENSOR_FRAME_MAX ? cap : RADIOTCHI_SENSOR_FRAME_MAX;
    uint16_t nb0 = 0, nb1 = 0, next = 0, dummy = 0;
    if(!decode_pwm_bytes_frame(pulses, n, start, short_unit, f0, fcap, &nb0, &next)) return false;

    // REQUIRE a confirming repeat with identical bytes (the noise guard, as the other decoders).
    if(next >= n) return false;
    if(!decode_pwm_bytes_frame(pulses, n, next, short_unit, f1, fcap, &nb1, &dummy)) return false;
    if(nb0 != nb1) return false;
    uint16_t nbytes = (uint16_t)((nb0 + 7u) / 8u);
    if(nbytes > fcap) nbytes = fcap;
    for(uint16_t i = 0; i < nbytes; i++) {
        if(f0[i] != f1[i]) return false;
    }
    for(uint16_t i = 0; i < nbytes; i++) out[i] = f0[i];
    if(nbits) *nbits = nb0;
    return true;
}

// --- OOK PPM (space/gap-coded) byte slicer ---------------------------------
#define WV_PPM_GLITCH_US 40u // ignore sub-40us spikes when estimating the short gap
#define WV_PPM_GAP_MULT  3u // a space >= this x the short gap (or the floor) ends a frame
#define WV_PPM_GAP_FLOOR 2000u // ...or an absolute several-ms inter-frame gap

// Decode one PPM frame at pulses[start] (bit = SPACE width: long = 1, short = 0) into MSB-first
// bytes. Ends at a space >= the sync gap (that terminating bit reads as 1). Returns the bit count
// via *nbits, the index past the frame via *next; false if too short / overrun.
static bool decode_ppm_bytes_frame(
    const int16_t* pulses,
    uint16_t n,
    uint16_t start,
    uint32_t short_space,
    uint8_t* out,
    uint16_t cap_bytes,
    uint16_t* nbits,
    uint16_t* next) {
    uint32_t sync_gap = short_space * WV_PPM_GAP_MULT;
    if(sync_gap < WV_PPM_GAP_FLOOR) sync_gap = WV_PPM_GAP_FLOOR;
    uint32_t space_thresh = short_space + short_space / 2u; // 1.5x splits short(0) from long(1)
    uint16_t cap_bits = (uint16_t)(cap_bytes * 8u);
    for(uint16_t b = 0; b < cap_bytes; b++) out[b] = 0;

    uint16_t count = 0;
    uint16_t i = start;
    for(; i + 1 < n; i += 2) {
        uint32_t space = abs32(pulses[i + 1]); // pulses[i] is the fixed mark; the gap carries the bit
        if(count >= cap_bits) return false;
        if(space > space_thresh) out[count >> 3] |= (uint8_t)(0x80u >> (count & 7u));
        count++;
        if(space >= sync_gap) { // inter-frame gap ends the frame
            i += 2;
            break;
        }
    }
    if(count < 8u) return false;
    if(nbits) *nbits = count;
    if(next) *next = i;
    return true;
}

bool radiotchi_ppm_to_bytes(
    const int16_t* pulses, uint16_t n, uint8_t* out, uint16_t cap, uint16_t* nbits) {
    if(pulses == NULL || out == NULL || cap == 0 || n < 16u) return false;

    uint16_t start = 0;
    if(pulses[0] < 0) start = 1; // begin on a mark
    // Short gap = the glitch-filtered minimum over SPACES only (the fixed mark would poison a
    // whole-train min and make every gap read as a "1").
    uint32_t short_space = 0;
    for(uint16_t i = start; i < n; i++) {
        if(pulses[i] >= 0) continue;
        uint32_t s = abs32(pulses[i]);
        if(s < WV_PPM_GLITCH_US) continue;
        if(short_space == 0 || s < short_space) short_space = s;
    }
    if(short_space == 0) return false;

    uint8_t f0[RADIOTCHI_SENSOR_FRAME_MAX];
    uint8_t f1[RADIOTCHI_SENSOR_FRAME_MAX];
    uint16_t fcap = cap < RADIOTCHI_SENSOR_FRAME_MAX ? cap : RADIOTCHI_SENSOR_FRAME_MAX;
    uint16_t nb0 = 0, nb1 = 0, next = 0, dummy = 0;
    if(!decode_ppm_bytes_frame(pulses, n, start, short_space, f0, fcap, &nb0, &next)) return false;

    if(next >= n) return false;
    if(!decode_ppm_bytes_frame(pulses, n, next, short_space, f1, fcap, &nb1, &dummy)) return false;
    if(nb0 != nb1) return false;
    uint16_t nbytes = (uint16_t)((nb0 + 7u) / 8u);
    if(nbytes > fcap) nbytes = fcap;
    for(uint16_t i = 0; i < nbytes; i++) {
        if(f0[i] != f1[i]) return false;
    }
    for(uint16_t i = 0; i < nbytes; i++) out[i] = f0[i];
    if(nbits) *nbits = nb0;
    return true;
}

// --- protocol classifier (provisional, signature-based) --------------------
//
// The Nourishment ladder needs *something* to raise the tier above RAW. We can not
// re-demodulate here: the in-struct `payload` is only a feature proxy (the lossless
// bits live in the linked `.sub`; see RawCapture). So this classifies the decode
// *family* from the reliable boundary features instead of fabricating exact values:
//
//   RAW        - unknown modulation, OR whitened/high-entropy payload (encrypted junk,
//                D1): we captured a burst but recognize no structure to decode.
//   MODULATION - a known modulation carrying a non-whitened, structured burst, but no
//                known protocol matched.
//   PROTOCOL   - the burst matches a known protocol *family's* signature; names it and
//                graduates the fingerprint to a FAMILY-level species. Two families today:
//                an OOK fixed-code remote (EV1527/PT2262) and a structured 2FSK sensor
//                packet (weather/telemetry, e.g. the LaCrosse class in fixtures/).
//   VALUES     - the actual code is demodulated from the pulse train (OOK PWM today).
//
// Privacy (A5/D1): species ids are deliberately FAMILY-level (`ook-fixed-<band>`,
// `fsk-sensor-<band>`) — never a per-device identifier. Some of these families (TPMS,
// keyless sensors) carry a persistent id in their payload; the classifier never extracts
// or surfaces it, so the dex cannot be used to track an individual device/vehicle.
//
// Calibration (the entropy gates, the bands/length window) is PROVISIONAL
// (open-questions Q3); the ladder and the recognizers' existence are the contract.
#define WV_ENTROPY_WHITENED 6.0f // >= this (bits/byte) => looks encrypted/whitened (junk)
#define WV_ENTROPY_STRUCTURED 2.5f // <  this => clearly structured (fixed-code-like)
#define WV_REMOTE_MIN_PULSES 16u // a real burst, not a stray blip
#define WV_PROTO_OOK_FIXED "OOK-FixedCode"
#define WV_PROTO_FSK_SENSOR "FSK-Sensor"

// True for the common Sub-GHz remote-control bands the CC1101 covers (Japan-aware:
// 315 / 426-430 / 433.92, plus 868/915). 1 MHz granularity is enough here.
static bool is_remote_band(uint32_t frequency_hz) {
    uint32_t mhz = band_bucket_mhz(frequency_hz);
    return (mhz >= 300u && mhz <= 470u) || (mhz >= 779u && mhz <= 930u);
}

// Emit a PROTOCOL match: name the protocol and graduate to a FAMILY-level species
// (`<prefix>-<band>`, never a per-device id — privacy, A5). Returns TIER_PROTOCOL.
static DecodeTier emit_protocol(
    const char* proto,
    const char* species_prefix,
    uint32_t frequency_hz,
    char* protocol_out,
    size_t protocol_len,
    char* species_out,
    size_t species_len) {
    if(protocol_out && protocol_len) {
        strncpy(protocol_out, proto, protocol_len - 1);
        protocol_out[protocol_len - 1] = '\0';
    }
    if(species_out && species_len) {
        snprintf(
            species_out, species_len, "%s-%u", species_prefix, (unsigned)band_bucket_mhz(frequency_hz));
        species_out[species_len - 1] = '\0';
    }
    return TIER_PROTOCOL;
}

DecodeTier radiotchi_classify(
    uint32_t frequency_hz,
    Modulation modulation,
    uint16_t bit_count,
    float entropy,
    char* protocol_out,
    size_t protocol_len,
    char* species_out,
    size_t species_len) {
    if(protocol_out && protocol_len) protocol_out[0] = '\0';
    if(species_out && species_len) species_out[0] = '\0';

    // Unknown modulation or whitened/encrypted noise: nothing to decode -> RAW.
    if(modulation == MOD_UNKNOWN || entropy >= WV_ENTROPY_WHITENED) return TIER_RAW;

    bool structured = entropy < WV_ENTROPY_STRUCTURED && bit_count >= WV_REMOTE_MIN_PULSES &&
                      is_remote_band(frequency_hz);

    // OOK fixed-code remote (EV1527/PT2262 family): a structured OOK burst.
    if(modulation == MOD_OOK && structured) {
        return emit_protocol(
            WV_PROTO_OOK_FIXED, "ook-fixed", frequency_hz, protocol_out, protocol_len, species_out,
            species_len);
    }

    // Structured 2FSK sensor packet (weather/telemetry/TPMS class). Family-level only —
    // these carry persistent device ids we deliberately never surface (A5).
    if(modulation == MOD_2FSK && structured) {
        return emit_protocol(
            WV_PROTO_FSK_SENSOR, "fsk-sensor", frequency_hz, protocol_out, protocol_len,
            species_out, species_len);
    }

    // Known modulation carrying a structured (non-whitened) burst, protocol unknown.
    return TIER_MODULATION;
}

// Case-insensitive "does haystack contain needle" (needle is already lowercase, ASCII).
static bool contains_ci(const char* hay, const char* needle) {
    if(hay == NULL || needle == NULL) return false;
    for(const char* h = hay; *h != '\0'; h++) {
        const char* a = h;
        const char* b = needle;
        while(*a != '\0' && *b != '\0') {
            char ca = *a;
            if(ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
            if(ca != *b) break;
            a++;
            b++;
        }
        if(*b == '\0') return true; // matched the whole needle
    }
    return false;
}

void radiotchi_species_for_protocol(
    const char* protocol, uint32_t frequency_hz, char* out, size_t out_len) {
    if(out == NULL || out_len == 0) return;
    out[0] = '\0';
    if(protocol == NULL || protocol[0] == '\0') return;

    // Brand families: a substring of the firmware protocol name -> a maker/system-named family.
    // These are real manufacturer/brand names for gate, garage and car-alarm remotes; matching a
    // brand graduates the species from a chip/cipher name to a maker-named family (A5: a make, not
    // a per-device id). Keep ASCII, lowercase needles. Extensible — add a row to cover more makes;
    // car-keyfob make discrimination beyond what the firmware names is deliberately left to real
    // protocol decoders (it cannot be guessed from coarse RF features without mislabeling).
    static const struct {
        const char* needle;
        const char* prefix;
    } kBrands[] = {
        {"starline", "keyfob-starline"}, {"star line", "keyfob-starline"},
        {"scher", "keyfob-scherkhan"}, // Scher-Khan car alarm
        {"came", "gate-came"},           {"nice", "gate-nice"},
        {"hormann", "gate-hormann"},     {"holtek", "gate-holtek"},
        {"chamberlain", "gate-chamberlain"}, {"security+", "gate-chamberlain"},
        {"liftmaster", "gate-chamberlain"}, {"somfy", "gate-somfy"},
        {"faac", "gate-faac"},           {"bft", "gate-bft"},
        {"doorhan", "gate-doorhan"},     {"an-motors", "gate-anmotors"},
        {"keeloq", "rolling-keeloq"},
    };
    unsigned band = (unsigned)band_bucket_mhz(frequency_hz);
    for(size_t i = 0; i < sizeof(kBrands) / sizeof(kBrands[0]); i++) {
        if(contains_ci(protocol, kBrands[i].needle)) {
            snprintf(out, out_len, "%s-%u", kBrands[i].prefix, band);
            out[out_len - 1] = '\0';
            return;
        }
    }

    // Unrecognized: keep the protocol name as the species (unchanged behaviour).
    strncpy(out, protocol, out_len - 1);
    out[out_len - 1] = '\0';
}

bool radiotchi_redecode(CaptureEvent* ev) {
    if(ev == NULL) return false;
    char protocol[RADIOTCHI_PROTOCOL_LEN];
    char species[RADIOTCHI_SPECIES_LEN];
    DecodeTier tier = radiotchi_classify(
        ev->frequency_hz,
        ev->modulation,
        ev->bit_count,
        ev->entropy,
        protocol,
        sizeof(protocol),
        species,
        sizeof(species));

    if(tier <= ev->decode_tier) return false; // re-grade only ever raises a tier

    ev->decode_tier = tier;
    if(protocol[0]) {
        strncpy(ev->protocol, protocol, sizeof(ev->protocol) - 1);
        ev->protocol[sizeof(ev->protocol) - 1] = '\0';
    }
    if(tier >= TIER_PROTOCOL && species[0]) {
        strncpy(ev->species_id, species, sizeof(ev->species_id) - 1);
        ev->species_id[sizeof(ev->species_id) - 1] = '\0';
    }
    ev->scores.nourishment = score_nourishment(ev->decode_tier); // other 4 axes untouched
    return true;
}

Scores score_capture(const CaptureEvent* ev, const RarityView* dex_rarity) {
    Scores s = {0};
    if(ev == NULL) return s;
    s.calories = score_calories(ev->bit_count);
    s.freshness = score_freshness(ev->rssi_dbm);
    s.additives = score_additives(ev->entropy);
    s.rarity = score_rarity(dex_rarity);
    s.nourishment = score_nourishment(ev->decode_tier);
    return s;
}

// --- unified pulse-based decode dispatch -----------------------------------
//
// One ordered place that runs every pulse-based VALUES decoder, shared by live capture and the
// `.sub` re-grade. Specific CRC-validated device decoders run BEFORE the generic fixed-code /
// sensor families, so a recognized sensor/remote graduates to its named species instead of the
// generic bucket. Adding a decoder = add it here (and it lights up on new + stored captures).

// Generic OOK fixed-code family (the EV1527/PT2262 + Manchester + repeating-frame ladder).
// Sets ev to TIER_VALUES / "ook-fixed-<band>" and a per-device tag (none for the coarse
// repeating-frame path). Returns true on a confirmed decode.
static bool decode_ook_fixed(uint32_t band, const int16_t* pulses, uint16_t n, CaptureEvent* ev) {
    uint32_t code = 0;
    uint8_t nbits = 0;
    bool got = false;
    if(radiotchi_ook_pwm_decode(pulses, n, &code, &nbits)) {
        radiotchi_individual_fingerprint(code, nbits, ev->individual, sizeof(ev->individual));
        got = true;
    } else if(radiotchi_manchester_decode(pulses, n, &code, &nbits)) {
        radiotchi_individual_fingerprint(code, nbits, ev->individual, sizeof(ev->individual));
        got = true;
    } else if(radiotchi_repeating_frame(pulses, n, NULL)) {
        got = true; // coarse waveform fingerprint: no trustworthy per-device tag (D28)
    }
    if(!got) return false;
    ev->decode_tier = TIER_VALUES;
    strncpy(ev->protocol, WV_PROTO_OOK_FIXED, sizeof(ev->protocol) - 1);
    ev->protocol[sizeof(ev->protocol) - 1] = '\0';
    snprintf(ev->species_id, sizeof(ev->species_id), "ook-fixed-%u", (unsigned)band);
    ev->species_id[sizeof(ev->species_id) - 1] = '\0';
    return true;
}

// Generic structured-2FSK sensor family (PCM/NRZ-with-repeat). Sets ev to TIER_VALUES /
// "fsk-sensor-<band>" + a one-way per-device tag. Returns true on a confirmed frame.
static bool decode_fsk_sensor(uint32_t band, const int16_t* pulses, uint16_t n, CaptureEvent* ev) {
    uint8_t frame[RADIOTCHI_FSK_FRAME_MAX];
    uint16_t flen = 0;
    if(!radiotchi_fsk_sensor_decode(pulses, n, frame, sizeof(frame), &flen)) return false;
    ev->decode_tier = TIER_VALUES;
    strncpy(ev->protocol, WV_PROTO_FSK_SENSOR, sizeof(ev->protocol) - 1);
    ev->protocol[sizeof(ev->protocol) - 1] = '\0';
    snprintf(ev->species_id, sizeof(ev->species_id), "fsk-sensor-%u", (unsigned)band);
    ev->species_id[sizeof(ev->species_id) - 1] = '\0';
    radiotchi_individual_fingerprint_bytes(frame, flen, ev->individual, sizeof(ev->individual));
    return true;
}

// CRC-validate a sensor frame whose LAST byte is a check over the preceding bytes. Tries the
// CRC-8 generators common to the rtl_433-class sensor catalog (0x07/0x31/0x2F, init 0). On a match
// writes a short tag ("c07"/"c31"/"c2f") and returns true. CRC-only (no weak sum/xor) so a
// non-sensor frame is very unlikely (~1/256 per poly) to be mislabeled. Pure.
static bool sensor_crc_valid(const uint8_t* frame, uint16_t n, char* out_tag, size_t tag_len) {
    if(frame == NULL || n < 4u) return false; // >= 3 data bytes + 1 check
    uint16_t dlen = (uint16_t)(n - 1u);
    uint8_t chk = frame[n - 1u];
    static const uint8_t polys[] = {0x07u, 0x31u, 0x2Fu};
    static const char* const tags[] = {"c07", "c31", "c2f"};
    for(size_t i = 0; i < sizeof(polys) / sizeof(polys[0]); i++) {
        if(radiotchi_crc8(frame, dlen, polys[i], 0x00u) == chk) {
            strncpy(out_tag, tags[i], tag_len - 1);
            out_tag[tag_len - 1] = '\0';
            return true;
        }
    }
    return false;
}

// CRC-validated device-sensor decoder: slice the frame to bytes (mark-coded OOK PWM, or the 2FSK
// NRZ-with-repeat demod), and ONLY accept it as a sensor when a known CRC validates over the frame.
// Because the bytes are repeat-confirmed AND CRC-checked, this reaches VALUES with high confidence
// and graduates to a structurally-named family `sensor-<mod>-<n>B-<crc>-<band>` (e.g.
// "sensor-fsk-5B-c31-868") — a new dex species per (modulation, length, CRC, band) class. The OOK
// length floor keeps fixed-code remotes (<= 4 bytes) in the `ook-fixed` family, not here.
// Privacy (A5): species is a structural family; the frame only seeds a one-way per-device tag.
static bool
    decode_crc_sensor(uint32_t band, Modulation mod, const int16_t* pulses, uint16_t n, CaptureEvent* ev) {
    uint8_t frame[RADIOTCHI_SENSOR_FRAME_MAX];
    uint16_t nbits = 0;
    uint16_t nbytes = 0;
    const char* modtag = NULL;

    if(mod == MOD_OOK) {
        if(!radiotchi_pwm_to_bytes(pulses, n, frame, sizeof(frame), &nbits)) return false;
        nbytes = (uint16_t)((nbits + 7u) / 8u);
        if(nbytes < 5u) return false; // below this is the fixed-code remote regime (avoid overlap)
        modtag = "ook";
    } else if(mod == MOD_2FSK) {
        if(!radiotchi_fsk_sensor_decode(pulses, n, frame, RADIOTCHI_FSK_FRAME_MAX, &nbytes))
            return false;
        if(nbytes < 4u) return false;
        modtag = "fsk";
    } else {
        return false;
    }

    // Reject a degenerate all-identical frame (e.g. a fixed-mark PPM sensor mis-sliced here as
    // all-zero bytes, where CRC-8 of zeros trivially equals the zero check byte). A real sensor
    // payload varies; an all-same frame carries no value and must not be a false `sensor-*`.
    bool all_same = true;
    for(uint16_t i = 1; i < nbytes; i++) {
        if(frame[i] != frame[0]) {
            all_same = false;
            break;
        }
    }
    if(all_same) return false;

    char tag[8];
    if(!sensor_crc_valid(frame, nbytes, tag, sizeof(tag))) return false;

    ev->decode_tier = TIER_VALUES;
    strncpy(ev->protocol, "Sensor-CRC", sizeof(ev->protocol) - 1);
    ev->protocol[sizeof(ev->protocol) - 1] = '\0';
    snprintf(
        ev->species_id, sizeof(ev->species_id), "sensor-%s-%uB-%s-%u", modtag, (unsigned)nbytes,
        tag, (unsigned)band);
    ev->species_id[sizeof(ev->species_id) - 1] = '\0';
    radiotchi_individual_fingerprint_bytes(frame, nbytes, ev->individual, sizeof(ev->individual));
    return true;
}

// Named device decoder — Acurite 606TX outdoor thermometer (433.92 MHz OOK PWM). Documented
// public layout: a 32-bit frame [id:8][flags:8][temp:8 (+4 high bits in flags)][crc:8] whose last
// byte is CRC-8 (generator 0x07, init 0) over the first three. We gate on the exact length AND a
// valid CRC over the documented region — a documented signature, not a guess — so a non-Acurite
// frame practically never matches (the CRC must hold over exactly those 3 bytes). Reaches VALUES /
// "weather-acurite-433"; the id lives only in the hashed `individual` tag (A5). We never surface
// the decoded temperature, so the field math is irrelevant to what the dex shows. Pure.
static bool decode_acurite606(uint32_t band, const int16_t* pulses, uint16_t n, CaptureEvent* ev) {
    if(band != 433u) return false;
    uint8_t f[RADIOTCHI_SENSOR_FRAME_MAX];
    uint16_t nbits = 0;
    if(!radiotchi_pwm_to_bytes(pulses, n, f, sizeof(f), &nbits)) return false;
    if(nbits != 32u) return false; // exactly 4 bytes
    if(radiotchi_crc8(f, 3, 0x07u, 0x00u) != f[3]) return false; // CRC-8/0x07 over bytes 0..2

    ev->decode_tier = TIER_VALUES;
    strncpy(ev->protocol, "Acurite-606", sizeof(ev->protocol) - 1);
    ev->protocol[sizeof(ev->protocol) - 1] = '\0';
    strncpy(ev->species_id, "weather-acurite-433", sizeof(ev->species_id) - 1);
    ev->species_id[sizeof(ev->species_id) - 1] = '\0';
    radiotchi_individual_fingerprint_bytes(f, 4, ev->individual, sizeof(ev->individual));
    return true;
}

// Named device decoder — Nexus-TH (and its many clones) outdoor temperature/humidity sensor
// (433.92 MHz OOK PPM). Documented public layout: 36 bits [id:8][flags:4][temp:12][const:4=0xF]
// [humidity:8]. The constant 0xF nibble at bits 24..27 is the distinctive validity marker; we gate
// on it AND a plausible humidity (<= 100) AND the repeat-confirmed PPM framing — so a non-Nexus
// frame practically never matches. Reaches VALUES / "th-nexus-433"; the id stays in the hashed
// `individual` tag (A5), the sensor values are never surfaced. Pure.
static bool decode_nexus_th(uint32_t band, const int16_t* pulses, uint16_t n, CaptureEvent* ev) {
    if(band != 433u) return false;
    uint8_t f[RADIOTCHI_SENSOR_FRAME_MAX];
    uint16_t nbits = 0;
    if(!radiotchi_ppm_to_bytes(pulses, n, f, sizeof(f), &nbits)) return false;
    if(nbits < 36u) return false; // Nexus is a 36-bit frame
    uint16_t nbytes = (uint16_t)((nbits + 7u) / 8u);
    if(radiotchi_bits_get(f, nbytes, 24, 4) != 0xFu) return false; // the constant 0xF marker
    if(radiotchi_bits_get(f, nbytes, 28, 8) > 100u) return false; // humidity in 0..100

    ev->decode_tier = TIER_VALUES;
    strncpy(ev->protocol, "Nexus-TH", sizeof(ev->protocol) - 1);
    ev->protocol[sizeof(ev->protocol) - 1] = '\0';
    strncpy(ev->species_id, "th-nexus-433", sizeof(ev->species_id) - 1);
    ev->species_id[sizeof(ev->species_id) - 1] = '\0';
    radiotchi_individual_fingerprint_bytes(f, nbytes, ev->individual, sizeof(ev->individual));
    return true;
}

bool radiotchi_decode_from_pulses(
    uint32_t frequency_hz,
    Modulation modulation,
    const int16_t* pulses,
    uint16_t n,
    CaptureEvent* ev) {
    if(ev == NULL || pulses == NULL || n == 0) return false;
    uint32_t band = band_bucket_mhz(frequency_hz);

    // Order = specificity; the first match wins. Named device decoders (documented signatures)
    // first, then the CRC-validated generic sensor, then the generic fixed-code / sensor families.
    if(modulation == MOD_OOK && decode_acurite606(band, pulses, n, ev)) return true;
    if(modulation == MOD_OOK && decode_nexus_th(band, pulses, n, ev)) return true;
    if(decode_crc_sensor(band, modulation, pulses, n, ev)) return true;

    if(modulation == MOD_OOK) {
        return decode_ook_fixed(band, pulses, n, ev);
    } else if(modulation == MOD_2FSK) {
        return decode_fsk_sensor(band, pulses, n, ev);
    }
    return false;
}

// --- assembly --------------------------------------------------------------

CaptureEvent analyze_capture(
    const RawCapture* raw,
    const RarityView* dex_rarity,
    uint64_t timestamp) {
    CaptureEvent ev = {0};
    if(raw == NULL) return ev;

    ev.timestamp = timestamp;
    ev.frequency_hz = raw->frequency_hz;
    ev.modulation = raw->modulation;
    ev.rssi_dbm = raw->rssi_dbm;
    ev.bit_count = raw->bit_count;

    // Copy the bounded payload working-copy (the lossless original is the .sub).
    uint16_t n = raw->payload_len;
    if(n > RADIOTCHI_PAYLOAD_MAX) n = RADIOTCHI_PAYLOAD_MAX;
    memcpy(ev.payload, raw->payload, n);
    ev.payload_len = n;

    ev.entropy = radiotchi_shannon_entropy(ev.payload, ev.payload_len);

    // Classify the decode depth from the boundary features. Start from a provisional
    // fingerprint-species; if the classifier recognizes a protocol it overrides the
    // species with a named (graduated) one. Unknown/whitened signals stay TIER_RAW with
    // the fingerprint, so an unrecognized signal still yields a full 4-axis label.
    radiotchi_fingerprint_species(
        ev.frequency_hz, ev.modulation, ev.bit_count, ev.species_id, sizeof(ev.species_id));
    char graduated[RADIOTCHI_SPECIES_LEN];
    ev.decode_tier = radiotchi_classify(
        ev.frequency_hz,
        ev.modulation,
        ev.bit_count,
        ev.entropy,
        ev.protocol,
        sizeof(ev.protocol),
        graduated,
        sizeof(graduated));
    if(ev.decode_tier >= TIER_PROTOCOL && graduated[0]) {
        strncpy(ev.species_id, graduated, sizeof(ev.species_id) - 1);
        ev.species_id[sizeof(ev.species_id) - 1] = '\0';
    }

    // If we have the pulse timing for an OOK burst, actually demodulate it: a valid
    // PWM fixed-code frame means we read the values -> TIER_VALUES (the top of the
    // ladder). Privacy (A5): the decoded code only justifies the tier; we do NOT turn
    // it into a per-device species — the species stays at the family granularity.
    if(raw->fw_protocol[0] != '\0') {
        // The Capture Source ran the firmware's Sub-GHz protocol decoders and recognized a real
        // protocol — the most reliable result. Use its protocol family + privacy-safe per-device
        // id (already hashed by the adapter, never the raw serial; A5). Takes precedence over the
        // pure-core heuristics below. (D29)
        ev.decode_tier = TIER_VALUES;
        strncpy(ev.protocol, raw->fw_protocol, sizeof(ev.protocol) - 1);
        ev.protocol[sizeof(ev.protocol) - 1] = '\0';
        strncpy(ev.individual, raw->fw_individual, sizeof(ev.individual) - 1);
        ev.individual[sizeof(ev.individual) - 1] = '\0';
        // Species = a maker-named family when the protocol name carries a recognizable brand
        // (e.g. "Star Line" -> "keyfob-starline-433"), else the protocol family name verbatim.
        // Privacy-safe (A5): a make/brand label, never the per-device serial (that stays hashed
        // in ev.individual).
        radiotchi_species_for_protocol(
            raw->fw_protocol, ev.frequency_hz, ev.species_id, sizeof(ev.species_id));
    } else if(
        (raw->modulation == MOD_OOK || raw->modulation == MOD_2FSK) && raw->pulse_count > 0) {
        // Demodulate the pulse train through the shared decoder dispatch: specific CRC-validated
        // device decoders first, then the generic fixed-code / sensor families -> TIER_VALUES.
        // Privacy (A5): species stay family/brand-level; any decoded code only justifies the tier
        // and a one-way per-device tag. (Same path the `.sub` re-grade uses, so results match.)
        radiotchi_decode_from_pulses(
            raw->frequency_hz, raw->modulation, raw->pulses, raw->pulse_count, &ev);
    }

    // Carry the raw .sub reference across the boundary so the Game Shell can link
    // the lossless recording to this event.
    strncpy(ev.raw_sub_ref, raw->raw_sub_ref, sizeof(ev.raw_sub_ref) - 1);
    ev.raw_sub_ref[sizeof(ev.raw_sub_ref) - 1] = '\0';

    ev.scores = score_capture(&ev, dex_rarity);
    return ev;
}
