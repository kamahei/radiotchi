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
// boundary). We estimate the half-bit unit as the glitch-filtered minimum run, expand each run
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

    // Half-bit unit = the glitch-filtered MINIMUM run (gap-scale runs are far larger). Note: a
    // spurious run just above the glitch floor can bias this low; the run-cap + repeat-confirm
    // absorb mild cases, but a truly robust estimator is provisional (open-questions Q3).
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

    // Reject a degenerate all-0 / all-1 code: a periodic half-bit-rate tone (or noise that repeats
    // that shape) phase-locks and confirms, but carries no device code — the same false-positive
    // the FSK/byte slicers' all-same guards reject.
    uint32_t allones = (nbits0 >= 32u) ? 0xFFFFFFFFu : ((1u << nbits0) - 1u);
    if(code0 == 0u || code0 == allones) return false;

    // REQUIRE a confirming repeat with identical bits (the OOK/FSK noise guard, for Manchester).
    if(next + WV_MAN_MIN_BITS * 2u > n) return false; // no room for a confirming repeat
    if(!decode_manchester_frame(pulses, n, next, half_unit, &code1, &nbits1, &dummy)) return false;
    if(code1 != code0 || nbits1 != nbits0) return false; // frames disagree => distrust

    if(code) *code = code0;
    if(nbits) *nbits = nbits0;
    return true;
}

// --- Manchester -> byte frame (for multi-byte Manchester sensors) -----------
#define WV_MANB_SAMP_MAX 144u // half-bit samples buffered (<= 8 bytes * 8 bits * 2 + slack)

// Robust half-bit width for ONE polarity (marks if want_mark, else spaces): the smallest run of that
// sign with cluster support (>=3 runs within 25%). Real OOK Manchester has a mark/space duty-cycle
// asymmetry (the slicer threshold is not at the pulse midpoint), so a single half-bit unit
// mis-rounds the longer polarity's full-bit to 3 half-bits and corrupts the phase pairing; estimating
// each polarity separately fixes that. A lone onset/glitch run has no cluster and can't drag the
// estimate down. Falls back to the raw minimum. O(n^2), n<=256. Pure.
static uint32_t man_half_for(const int16_t* pulses, uint16_t n, bool want_mark) {
    uint32_t best = 0, raw = 0;
    for(uint16_t i = 0; i < n; i++) {
        if((pulses[i] > 0) != want_mark) continue;
        uint32_t mi = abs32(pulses[i]);
        if(mi < WV_MAN_GLITCH_US) continue;
        if(raw == 0u || mi < raw) raw = mi;
        uint16_t support = 0;
        for(uint16_t j = 0; j < n; j++) {
            if((pulses[j] > 0) != want_mark) continue;
            uint32_t mj = abs32(pulses[j]);
            if(mj < WV_MAN_GLITCH_US) continue;
            uint32_t d = (mj > mi) ? (mj - mi) : (mi - mj);
            if(d * 4u <= mi) support++; // within 25% of mi
        }
        if(support >= 3u && (best == 0u || mi < best)) best = mi;
    }
    return best != 0u ? best : raw;
}

// Decode one Manchester frame at pulses[start] into MSB-first bytes (G.E. Thomas: low->high = 1).
// Each run is classified as 1 or 2 half-bits by its OWN polarity's half-bit width (mark_half /
// space_half) — clean Manchester never has a run wider than 2 half-bits, and per-polarity widths
// absorb the OOK duty-cycle asymmetry. A run that is neither ~1 nor ~2 of its polarity's half-bits is
// not clean Manchester. Returns the bit count via *nbits, the index past the frame via *next.
static bool decode_manchester_bytes_frame(
    const int16_t* pulses,
    uint16_t n,
    uint16_t start,
    uint32_t mark_half,
    uint32_t space_half,
    uint8_t* out,
    uint16_t cap_bytes,
    uint16_t* nbits,
    uint16_t* next) {
    uint32_t gap = space_half * WV_MAN_GAP_MULT; // gaps are spaces; scale off the space half-bit
    if(gap < WV_MAN_GAP_FLOOR) gap = WV_MAN_GAP_FLOOR;

    uint8_t samp[WV_MANB_SAMP_MAX];
    uint16_t m = 0;
    uint16_t i = start;
    bool started = false;
    for(; i < n; i++) {
        uint32_t mag = abs32(pulses[i]);
        if(mag < WV_MAN_GLITCH_US) continue;
        if(mag >= gap) {
            if(started) {
                i++;
                break;
            }
            continue;
        }
        uint8_t level = (pulses[i] > 0) ? 1u : 0u;
        uint32_t half = level ? mark_half : space_half;
        uint32_t q;
        if(mag < (half * 3u) / 2u) {
            q = 1u; // ~1 half-bit
        } else if(mag < half * 3u) {
            q = 2u; // ~2 half-bits (full bit) — tolerant of the asymmetry up to the gap scale
        } else {
            return false; // neither 1 nor 2 half-bits wide => not clean Manchester
        }
        started = true;
        for(uint32_t r = 0; r < q && m < WV_MANB_SAMP_MAX; r++) samp[m++] = level;
    }
    if(m < 16u) return false; // >= 8 bits

    uint16_t cap_bits = (uint16_t)(cap_bytes * 8u);
    for(uint8_t phase = 0; phase < 2; phase++) {
        for(uint16_t b = 0; b < cap_bytes; b++) out[b] = 0;
        uint16_t cnt = 0;
        bool ok = true;
        for(uint16_t k = phase; (uint16_t)(k + 1) < m; k += 2) {
            uint8_t a = samp[k];
            uint8_t b = samp[k + 1];
            if(a == b) {
                ok = false;
                break;
            }
            if(cnt >= cap_bits) {
                ok = false;
                break;
            }
            if(a == 0u && b == 1u) out[cnt >> 3] |= (uint8_t)(0x80u >> (cnt & 7u));
            cnt++;
        }
        if(ok && cnt >= 16u) {
            if(nbits) *nbits = cnt;
            if(next) *next = i;
            return true;
        }
    }
    return false;
}

bool radiotchi_manchester_to_bytes(
    const int16_t* pulses, uint16_t n, uint8_t* out, uint16_t cap, uint16_t* nbits) {
    if(pulses == NULL || out == NULL || cap == 0 || n < 16u) return false;

    uint32_t mark_half = man_half_for(pulses, n, true);
    uint32_t space_half = man_half_for(pulses, n, false);
    if(mark_half == 0u || space_half == 0u) return false;

    uint8_t f0[8];
    uint8_t f1[8];
    uint16_t fcap = cap < 8u ? cap : 8u;
    uint16_t nb0 = 0, nb1 = 0, next = 0, dummy = 0;
    if(!decode_manchester_bytes_frame(pulses, n, 0, mark_half, space_half, f0, fcap, &nb0, &next))
        return false;

    if(next >= n) return false;
    if(!decode_manchester_bytes_frame(pulses, n, next, mark_half, space_half, f1, fcap, &nb1, &dummy))
        return false;
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

// Robust NRZ bit period: the SMALLEST run magnitude that has supporting neighbors of similar width
// (within 25%). A real frame's preamble (0xAAAA...) emits many 1-bit runs, so the true symbol width
// always has a cluster; a lone onset transient or a clipped half-bit has no cluster and so can't pull
// the estimate below the real period (which a plain minimum would, mis-rounding 1-bit runs to 2 and
// garbling the frame). Falls back to the raw minimum if nothing clusters. Glitch-filtered. O(n^2),
// n<=256. Pure.
static uint32_t fsk_estimate_bit_period(const int16_t* pulses, uint16_t n) {
    uint32_t best = 0, raw_min = 0;
    for(uint16_t i = 0; i < n; i++) {
        uint32_t mi = abs32(pulses[i]);
        if(mi < WV_FSK_GLITCH_US) continue;
        if(raw_min == 0 || mi < raw_min) raw_min = mi;
        uint16_t support = 0;
        for(uint16_t j = 0; j < n; j++) {
            uint32_t mj = abs32(pulses[j]);
            if(mj < WV_FSK_GLITCH_US) continue;
            uint32_t diff = (mj > mi) ? (mj - mi) : (mi - mj);
            if(diff * 4u <= mi) support++; // within 25% of mi
        }
        if(support >= 3u && (best == 0u || mi < best)) best = mi;
    }
    return best != 0u ? best : raw_min;
}

bool radiotchi_fsk_sensor_decode(
    const int16_t* pulses, uint16_t n, uint8_t* frame, uint16_t cap, uint16_t* frame_len) {
    if(pulses == NULL || frame == NULL || cap == 0 || n < WV_FSK_MIN_BITS) return false;

    uint32_t bit_period = fsk_estimate_bit_period(pulses, n);
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

// Slice only the FIRST NRZ frame to bytes — NO confirming repeat required. The sync-word path uses
// this because its 16-bit sync + 8-bit CRC is the integrity guard, and real sync-framed sensors
// (e.g. LaCrosse-TX29) transmit a single burst that a repeat requirement would wrongly reject. The
// generic no-CRC FSK path still demands a repeat (its only guard) via radiotchi_fsk_sensor_decode.
static bool fsk_slice_first(
    const int16_t* pulses, uint16_t n, uint8_t* frame, uint16_t cap, uint16_t* frame_len) {
    if(pulses == NULL || frame == NULL || cap == 0 || n < WV_FSK_MIN_BITS) return false;
    uint32_t bit_period = fsk_estimate_bit_period(pulses, n);
    if(bit_period == 0) return false;
    uint16_t fcap = cap < RADIOTCHI_FSK_FRAME_MAX ? cap : RADIOTCHI_FSK_FRAME_MAX;
    uint16_t nb = 0, next = 0;
    if(!decode_fsk_frame(pulses, n, 0, bit_period, frame, fcap, &nb, &next)) return false;
    uint16_t nbytes = (uint16_t)((nb + 7u) / 8u);
    if(nbytes > fcap) nbytes = fcap;
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

    for(uint32_t i = 0; i <= n; i++) { // uint32 index: `i <= n` would never terminate at n==UINT16_MAX
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

uint8_t radiotchi_lfsr_digest8(const uint8_t* data, uint16_t len, uint8_t gen, uint8_t init) {
    uint8_t sum = 0;
    uint8_t key = init;
    if(data == NULL) return 0;
    for(uint16_t b = 0; b < len; b++) {
        for(int i = 7; i >= 0; i--) {
            if((data[b] >> i) & 1u) sum ^= key;
            key = (key & 1u) ? (uint8_t)((key >> 1) ^ gen) : (uint8_t)(key >> 1);
        }
    }
    return sum;
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

// Dispatch pre-pass: drop sub-24us spikes. Real slicer/IQ dips seen in captures are <=16us (Acurite
// ~4us, LaCrosse onset ~16us); the shortest real bit unit is ~58us (17kbps FSK), so 24us sits well
// under half a bit and never merges genuine symbols.
#define WV_COALESCE_GLITCH_US 24u

uint16_t radiotchi_coalesce_glitches(
    const int16_t* in, uint16_t n, int16_t* out, uint16_t cap, uint16_t glitch_us) {
    if(in == NULL || out == NULL || cap == 0) return 0;
    uint16_t m = 0;
    for(uint16_t i = 0; i < n; i++) {
        int32_t d = in[i];
        uint32_t mag = (uint32_t)(d < 0 ? -d : d);
        if(mag < glitch_us) continue; // drop a sub-glitch run; its same-sign neighbours then merge
        bool neg = d < 0;
        if(m > 0 && ((out[m - 1] < 0) == neg)) { // same sign as the kept previous run -> merge
            int32_t sum = (int32_t)out[m - 1] + d;
            if(sum > 32767) sum = 32767;
            if(sum < -32768) sum = -32768;
            out[m - 1] = (int16_t)sum;
        } else {
            if(m >= cap) break;
            out[m++] = (int16_t)d;
        }
    }
    return m;
}

int32_t radiotchi_find_sync(
    const uint8_t* bytes, uint16_t nbytes, uint16_t nbits, uint32_t sync, uint8_t sync_nbits) {
    if(bytes == NULL || sync_nbits == 0 || sync_nbits > 32 || nbits < sync_nbits) return -1;
    uint32_t mask = (sync_nbits == 32u) ? 0xFFFFFFFFu : ((1u << sync_nbits) - 1u);
    uint32_t want = sync & mask;
    for(uint32_t off = 0; off + sync_nbits <= nbits; off++) { // uint32: no off+sync wrap near UINT16_MAX
        if((radiotchi_bits_get(bytes, nbytes, (uint16_t)off, sync_nbits) & mask) == want) {
            return (int32_t)(off + sync_nbits);
        }
    }
    return -1;
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
// writes a short tag ("c07"/"c31"/"c2f") and returns true. CRC-only (no weak sum/xor). NOTE: trying
// 3 polynomials means a repeat-confirmed, non-all-same NON-sensor frame still has ~3/256 (~1.2%)
// chance to pass coincidentally; the length floors in decode_crc_sensor keep most fixed-code remotes
// out, and the residual is an accepted provisional tradeoff (open-questions Q3). Pure.
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

// CRC-validated MANCHESTER sensor decoder (the Oregon/TPMS-class encoding the byte CRC sensor above
// can not slice). Demodulate the Manchester frame to bytes (repeat-confirmed), then accept only when
// a known CRC validates over a >=5-byte frame (the floor keeps Manchester fixed-code remotes in
// ook-fixed). Same all-same + CRC guards as the OOK/FSK sensor paths -> "sensor-manch-<n>B-<crc>
// -<band>". Privacy (A5): structural family + hashed per-device tag, no surfaced values. Pure.
static bool
    decode_manch_sensor(uint32_t band, const int16_t* pulses, uint16_t n, CaptureEvent* ev) {
    uint8_t frame[8];
    uint16_t nbits = 0;
    if(!radiotchi_manchester_to_bytes(pulses, n, frame, sizeof(frame), &nbits)) return false;
    uint16_t nbytes = (uint16_t)((nbits + 7u) / 8u);
    if(nbytes < 5u) return false; // below this is the fixed-code remote regime

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
        ev->species_id, sizeof(ev->species_id), "sensor-manch-%uB-%s-%u", (unsigned)nbytes, tag,
        (unsigned)band);
    ev->species_id[sizeof(ev->species_id) - 1] = '\0';
    radiotchi_individual_fingerprint_bytes(frame, nbytes, ev->individual, sizeof(ev->individual));
    return true;
}

// Preamble + sync-word framed FSK sensor (the Fine Offset / Ecowitt / LaCrosse-class structure the
// whole-frame CRC sensor can't read, because the preamble breaks a frame-wide CRC). Demodulate the
// whole NRZ frame, locate the widely-used 0x2DD4 sync word in the bit stream, then CRC-validate the
// post-sync payload (trying each plausible data length, since the demod may carry trailing bytes).
// The 16-bit documented sync + an 8-bit CRC make a non-matching frame practically impossible to
// accept. -> "sensor-2dd4-<n>B-<crc>-<band>". Privacy (A5): structural family + hashed tag. Pure.
#define WV_FSK_SYNC      0x2DD4u
#define WV_FSK_SYNC_BITS 16u
static bool decode_fsk_sync_sensor(uint32_t band, const int16_t* pulses, uint16_t n, CaptureEvent* ev) {
    uint8_t f[RADIOTCHI_FSK_FRAME_MAX];
    uint16_t flen = 0;
    // Single-frame slice (sync + CRC is the guard, not a repeat -> single-burst sensors decode).
    if(!fsk_slice_first(pulses, n, f, sizeof(f), &flen)) return false;
    uint16_t nbits = (uint16_t)(flen * 8u);
    int32_t doff = radiotchi_find_sync(f, flen, nbits, WV_FSK_SYNC, WV_FSK_SYNC_BITS);
    if(doff < 0) return false;

    // Pull the byte-aligned payload that follows the sync (find_sync gives a bit offset).
    uint16_t avail = (uint16_t)(nbits - (uint16_t)doff);
    uint16_t dbytes = (uint16_t)(avail / 8u);
    if(dbytes < 4u) return false; // >= 3 data + 1 check
    uint8_t data[RADIOTCHI_FSK_FRAME_MAX];
    for(uint16_t i = 0; i < dbytes; i++) {
        data[i] = (uint8_t)radiotchi_bits_get(f, flen, (uint16_t)(doff + (int32_t)(i * 8u)), 8);
    }

    // The demod may carry trailing bytes past the CRC; try each candidate frame length. The 0x2DD4
    // family (Fine Offset / Ecowitt / LaCrosse) uses CRC-8 generator 0x31, so we check ONLY that
    // poly (not the 3-poly generic set) — tying the check to the documented protocol keeps the
    // sync(16-bit) + CRC(8-bit) false-accept rate negligible despite the length loop.
    for(uint16_t len = 4; len <= dbytes; len++) {
        bool all_same = true;
        for(uint16_t i = 1; i < len; i++) {
            if(data[i] != data[0]) {
                all_same = false;
                break;
            }
        }
        if(all_same) continue;
        if(radiotchi_crc8(data, (uint16_t)(len - 1u), 0x31u, 0x00u) != data[len - 1u]) continue;
        ev->decode_tier = TIER_VALUES;
        strncpy(ev->protocol, "FSK-Sync", sizeof(ev->protocol) - 1);
        ev->protocol[sizeof(ev->protocol) - 1] = '\0';
        snprintf(
            ev->species_id, sizeof(ev->species_id), "sensor-2dd4-%uB-c31-%u", (unsigned)len,
            (unsigned)band);
        ev->species_id[sizeof(ev->species_id) - 1] = '\0';
        radiotchi_individual_fingerprint_bytes(data, len, ev->individual, sizeof(ev->individual));
        return true;
    }
    return false;
}

// Named device decoder — Acurite 606TX / Technoline TX960 thermometer (433.92 MHz OOK). VALIDATED
// against real rtl_433_tests captures: the coding is gap/PPM (a fixed mark, then the GAP carries the
// bit: short = 0, long = 1), and the 32-bit data frame [id:8][BbCC|temp_hi:4][temp_lo:8][digest:8]
// is delimited by a long sync gap (~2x the long data gap). The check byte is an LFSR-8 digest
// (`lfsr_digest8(b,3, gen 0x98, key 0xf1) == b[3]`), NOT a CRC/sum — confirmed on three real frames
// (A3 80 65 EA / A3 80 6C 29 / A3 80 CD 22). Thresholds are estimated from the sync scale, so the
// decoder is sample-rate-relative. Privacy (A5): species is family-level; the id only seeds the
// one-way tag; the temperature is never surfaced. Pure.
static bool decode_acurite606(uint32_t band, const int16_t* pulses, uint16_t n, CaptureEvent* ev) {
    if(band != 433u || n < 32u) return false;

    // Sync-gap scale = the largest space below the inter-message silence (>=30 ms after clamping).
    uint32_t maxsp = 0;
    for(uint16_t i = 0; i < n; i++) {
        if(pulses[i] >= 0) continue;
        uint32_t m = (uint32_t)(-pulses[i]);
        if(m < 30000u && m > maxsp) maxsp = m;
    }
    if(maxsp < 4000u) return false; // no sync gap of the right scale -> not Acurite framing
    uint32_t sync_thr = (maxsp * 6u) / 10u; // a sync gap; a long data gap stays below this
    uint32_t bit_thr = maxsp / 3u; // between the short(0) and long(1) data gaps

    // Decode the first complete 32-bit frame that sits between two sync gaps (gap = the bit).
    uint8_t f[4] = {0, 0, 0, 0};
    bool after_sync = false;
    uint16_t count = 0;
    for(uint16_t i = 0; i + 1 < n;) {
        if(pulses[i] < 0) { // not on a mark (e.g. the sync space itself) -> step to the next
            i++;
            continue;
        }
        if(pulses[i + 1] >= 0) { // expected a (mark, space) pair
            i++;
            continue;
        }
        uint32_t space = (uint32_t)(-pulses[i + 1]);
        i += 2;
        if(space >= sync_thr) { // a sync gap delimits the frame
            if(after_sync && count == 32u) break; // a full frame just completed
            after_sync = true; // (re)start a frame after this sync
            count = 0;
            f[0] = f[1] = f[2] = f[3] = 0;
            continue;
        }
        if(!after_sync) continue; // still before the first sync (mid-stream start)
        if(count >= 32u) { // overran a clean 32-bit frame -> wait for the next sync
            after_sync = false;
            continue;
        }
        if(space > bit_thr) f[count >> 3] |= (uint8_t)(0x80u >> (count & 7u));
        count++;
    }
    if(!(after_sync && count == 32u)) return false;
    if(f[0] == 0 && f[1] == 0 && f[2] == 0 && f[3] == 0) return false; // blank
    if(radiotchi_lfsr_digest8(f, 3, 0x98u, 0xf1u) != f[3]) return false; // the integrity gate

    ev->decode_tier = TIER_VALUES;
    strncpy(ev->protocol, "Acurite-606", sizeof(ev->protocol) - 1);
    ev->protocol[sizeof(ev->protocol) - 1] = '\0';
    strncpy(ev->species_id, "weather-acurite-433", sizeof(ev->species_id) - 1);
    ev->species_id[sizeof(ev->species_id) - 1] = '\0';
    // Per-device tag = hash of the ID byte ONLY (byte 0): the rest of the frame carries the varying
    // temperature, so hashing it would mint a new "individual" every reading instead of tracking the
    // one device over time (A5/D27 recurrence).
    radiotchi_individual_fingerprint_bytes(f, 1, ev->individual, sizeof(ev->individual));
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
    // Humidity 0..100 (allow +1: the PPM terminating gap forces the last bit, the humidity LSB, to 1).
    if(radiotchi_bits_get(f, nbytes, 28, 8) > 101u) return false;
    // Temperature plausibility (a second structural gate so ordinary 433 PPM remotes don't pass the
    // 4-bit marker by luck): 12-bit signed (bits 12..23), 0.1 C units, within roughly [-40, 70] C.
    int32_t traw = (int32_t)radiotchi_bits_get(f, nbytes, 12, 12);
    if(traw & 0x800) traw -= 0x1000; // sign-extend the 12-bit field
    if(traw < -400 || traw > 700) return false;

    ev->decode_tier = TIER_VALUES;
    strncpy(ev->protocol, "Nexus-TH", sizeof(ev->protocol) - 1);
    ev->protocol[sizeof(ev->protocol) - 1] = '\0';
    strncpy(ev->species_id, "th-nexus-433", sizeof(ev->species_id) - 1);
    ev->species_id[sizeof(ev->species_id) - 1] = '\0';
    // Per-device tag = the ID byte only (byte 0); the rest varies with temp/humidity each reading.
    radiotchi_individual_fingerprint_bytes(f, 1, ev->individual, sizeof(ev->individual));
    return true;
}

// Named device decoder — Oregon Scientific v2.1 weather sensors (433.92 MHz OOK, double-Manchester).
// Public layout: an OUTER Manchester chip stream (alternating 0x55/0xAA preamble) whose post-preamble
// payload is ITSELF Manchester-encoded; after the inner decode the nibbles are bit-reversed (reflect),
// giving msg = [sensor_id:16][channel:4][device_id+flags][BCD temperature ...][nibble-sum checksum].
// We slice the outer Manchester to chips (per-polarity half-bit widths), find the preamble end, then
// over a small offset window run the inner Manchester + nibble-reflect and accept ONLY when the
// sensor_id is a known type AND the Oregon nibble-sum checksum validates (16-bit type + 8-bit checksum
// => negligible false accept). VALIDATED against a real rtl_433 THN132N capture (id 231, ch 4, 18.0 C).
// -> "weather-oregon-433"; the device_id only seeds the one-way per-device tag (A5), values are never
// surfaced. Pure, libm-free.
#define ORE_SAMP_MAX 512u
#define ORE_BIT(arr, i) (((arr)[(i) >> 3] >> (7u - ((i) & 7u))) & 1u)

// Reverse the bit order within each of the two nibbles of a byte (rtl_433 reflect_nibbles).
static uint8_t ore_reflect_nibbles(uint8_t x) {
    uint8_t r = 0;
    for(int s = 0; s <= 4; s += 4) {
        uint8_t nib = (uint8_t)((x >> s) & 0x0fu);
        uint8_t rev =
            (uint8_t)(((nib & 1u) << 3) | ((nib & 2u) << 1) | ((nib & 4u) >> 1) | ((nib & 8u) >> 3));
        r = (uint8_t)(r | (rev << s));
    }
    return r;
}

// Oregon v2.1/v3 integrity: a 1-byte sum-of-nibbles over the first `idx` nibbles, compared to the
// checksum byte (its two nibbles swapped). Mirrors rtl_433 validate_os_checksum exactly.
static bool ore_checksum_ok(const uint8_t* msg, int idx) {
    uint32_t sum = 0;
    for(int i = 0; i + 1 < idx; i += 2) {
        uint8_t val = msg[i >> 1];
        sum += (uint32_t)((val >> 4) + (val & 0x0fu));
    }
    uint8_t ck;
    if(idx & 1) {
        sum += (uint32_t)(msg[idx >> 1] >> 4);
        ck = (uint8_t)((msg[idx >> 1] & 0x0fu) | (msg[(idx + 1) >> 1] & 0xf0u));
    } else {
        ck = (uint8_t)((msg[idx >> 1] >> 4) | ((msg[idx >> 1] & 0x0fu) << 4));
    }
    return (uint8_t)(sum & 0xffu) == ck;
}

static bool decode_oregon_v2(uint32_t band, const int16_t* pulses, uint16_t n, CaptureEvent* ev) {
    if(band != 433u || n < 64u) return false;
    uint32_t mark_half = man_half_for(pulses, n, true);
    uint32_t space_half = man_half_for(pulses, n, false);
    if(mark_half == 0u || space_half == 0u) return false;
    uint32_t gap = space_half * WV_MAN_GAP_MULT;
    if(gap < WV_MAN_GAP_FLOOR) gap = WV_MAN_GAP_FLOOR;

    // Outer Manchester -> half-bit sample stream (packed, MSB-first), first frame only.
    uint8_t samp[ORE_SAMP_MAX / 8u];
    memset(samp, 0, sizeof(samp));
    uint16_t m = 0;
    bool started = false;
    for(uint16_t i = 0; i < n && m < ORE_SAMP_MAX; i++) {
        uint32_t mag = abs32(pulses[i]);
        if(mag < WV_MAN_GLITCH_US) continue;
        if(mag >= gap) {
            if(started) break;
            continue;
        }
        uint8_t level = (pulses[i] > 0) ? 1u : 0u;
        uint32_t half = level ? mark_half : space_half;
        uint32_t q = (mag < (half * 3u) / 2u) ? 1u : ((mag < half * 3u) ? 2u : 0u);
        if(q == 0u) return false; // not clean Manchester
        started = true;
        for(uint32_t r = 0; r < q && m < ORE_SAMP_MAX; r++) {
            if(level) samp[m >> 3] |= (uint8_t)(0x80u >> (m & 7u));
            m++;
        }
    }
    if(m < 64u) return false;

    // Phase-pair the samples into the chip stream b (low->high = 1); the correct framing pairs every
    // (a,b) as a transition, so take the phase with the longer unbroken run.
    uint8_t b[ORE_SAMP_MAX / 16u];
    uint16_t blen = 0;
    for(uint8_t phase = 0; phase < 2; phase++) {
        uint8_t tmp[ORE_SAMP_MAX / 16u];
        memset(tmp, 0, sizeof(tmp));
        uint16_t cnt = 0;
        for(uint16_t k = phase; (uint16_t)(k + 1) < m; k += 2) {
            uint8_t a = (uint8_t)ORE_BIT(samp, k), bb = (uint8_t)ORE_BIT(samp, (uint16_t)(k + 1));
            if(a == bb) break;
            if(a == 0u) tmp[cnt >> 3] |= (uint8_t)(0x80u >> (cnt & 7u));
            cnt++;
        }
        if(cnt > blen) {
            blen = cnt;
            memcpy(b, tmp, sizeof(b));
        }
    }
    if(blen < 56u) return false;

    // Preamble end = the first repeated chip after >= 16 alternating chips.
    uint16_t P = 0;
    for(uint16_t i = 0; (uint16_t)(i + 1) < blen; i++) {
        if(i >= 16u && ORE_BIT(b, i) == ORE_BIT(b, (uint16_t)(i + 1))) {
            P = i;
            break;
        }
    }
    if(P == 0u) return false;

    // Known Oregon v2.1 temperature sensors: sensor_id -> checksum nibble index.
    static const uint16_t ore_ids[] = {0xEC40u, 0xCC43u}; // THN132N, THN129 (temp-only, 12-nibble cksum)
    static const int ore_idx[] = {12, 12};

    for(uint16_t off = P; off <= (uint16_t)(P + 20u) && (uint16_t)(off + 2u) < blen; off++) {
        // Inner Manchester (high->low = 1) -> message bits -> reflected nibbles.
        uint8_t msg[16];
        memset(msg, 0, sizeof(msg));
        uint16_t mc = 0;
        for(uint16_t k = off; (uint16_t)(k + 1) < blen && mc < 128u; k += 2) {
            uint8_t a = (uint8_t)ORE_BIT(b, k), bb = (uint8_t)ORE_BIT(b, (uint16_t)(k + 1));
            if(a == bb) break;
            if(a == 1u) msg[mc >> 3] |= (uint8_t)(0x80u >> (mc & 7u));
            mc++;
        }
        if(mc < 56u) continue;
        uint16_t mbytes = (uint16_t)(mc / 8u);
        for(uint16_t i = 0; i < mbytes; i++) msg[i] = ore_reflect_nibbles(msg[i]);

        uint16_t sid = (uint16_t)((msg[0] << 8) | msg[1]);
        for(size_t t = 0; t < sizeof(ore_ids) / sizeof(ore_ids[0]); t++) {
            if(sid != ore_ids[t]) continue;
            int idx = ore_idx[t];
            if((uint16_t)((idx + 2) / 2) > mbytes) continue;
            if(!ore_checksum_ok(msg, idx)) continue;
            ev->decode_tier = TIER_VALUES;
            strncpy(ev->protocol, "Oregon-v2", sizeof(ev->protocol) - 1);
            ev->protocol[sizeof(ev->protocol) - 1] = '\0';
            strncpy(ev->species_id, "weather-oregon-433", sizeof(ev->species_id) - 1);
            ev->species_id[sizeof(ev->species_id) - 1] = '\0';
            // Per-device tag = sensor type + house code only (A5); BCD values are never surfaced.
            uint8_t idbytes[3] = {msg[0], msg[1], (uint8_t)((msg[2] & 0x0fu) | (msg[3] & 0xf0u))};
            radiotchi_individual_fingerprint_bytes(idbytes, 3, ev->individual, sizeof(ev->individual));
            return true;
        }
    }
    return false;
}

bool radiotchi_decode_from_pulses(
    uint32_t frequency_hz,
    Modulation modulation,
    const int16_t* pulses,
    uint16_t n,
    CaptureEvent* ev) {
    if(ev == NULL || pulses == NULL || n == 0) return false;

    // Glitch-coalesce the train ONCE so the pair-walking PWM/PPM slicers don't desync on a slicer
    // dip that split a pulse (real IQ/RX captures have these; the synthetic test frames did not, so
    // it is a no-op there). 60us is below every protocol's real bit unit (>=100us) and above the
    // spikes. The element-by-element decoders already glitch-filter, so a cleaned train is harmless.
    int16_t clean[RADIOTCHI_PULSES_MAX];
    n = radiotchi_coalesce_glitches(pulses, n, clean, RADIOTCHI_PULSES_MAX, WV_COALESCE_GLITCH_US);
    pulses = clean;
    if(n == 0) return false;
    uint32_t band = band_bucket_mhz(frequency_hz);

    // Order = specificity; the first match wins. Named device decoders (documented signatures)
    // first, then the CRC-validated generic sensor, then the generic fixed-code / sensor families.
    if(modulation == MOD_OOK && decode_acurite606(band, pulses, n, ev)) return true;
    if(modulation == MOD_OOK && decode_nexus_th(band, pulses, n, ev)) return true;
    if(modulation == MOD_OOK && decode_oregon_v2(band, pulses, n, ev)) return true;
    if(modulation == MOD_OOK && decode_manch_sensor(band, pulses, n, ev)) return true;
    if(modulation == MOD_2FSK && decode_fsk_sync_sensor(band, pulses, n, ev)) return true;
    if(decode_crc_sensor(band, modulation, pulses, n, ev)) return true;
    // FSK-Manchester sensors (TPMS class): after the NRZ sync/CRC paths, so a genuine NRZ sensor
    // isn't mis-sliced as Manchester first; the Manchester run-cap + CRC keep this safe.
    if(modulation == MOD_2FSK && decode_manch_sensor(band, pulses, n, ev)) return true;

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
        // Clamp the pulse count to the in-struct array bound (mirrors the payload clamp above), so a
        // mis-filled RawCapture can never read past raw->pulses[].
        uint16_t pc = raw->pulse_count;
        if(pc > RADIOTCHI_PULSES_MAX) pc = RADIOTCHI_PULSES_MAX;
        radiotchi_decode_from_pulses(raw->frequency_hz, raw->modulation, raw->pulses, pc, &ev);
    }

    // Carry the raw .sub reference across the boundary so the Game Shell can link
    // the lossless recording to this event.
    strncpy(ev.raw_sub_ref, raw->raw_sub_ref, sizeof(ev.raw_sub_ref) - 1);
    ev.raw_sub_ref[sizeof(ev.raw_sub_ref) - 1] = '\0';

    ev.scores = score_capture(&ev, dex_rarity);
    return ev;
}
