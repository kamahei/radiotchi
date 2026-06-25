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
    for(uint16_t i = 0; i < nbytes; i++) {
        if(f0[i] != f1[i]) return false; // frames disagree => distrust
    }
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
        // Species = the protocol family (privacy-safe; never the per-device serial).
        strncpy(ev.species_id, raw->fw_protocol, sizeof(ev.species_id) - 1);
        ev.species_id[sizeof(ev.species_id) - 1] = '\0';
    } else if(raw->modulation == MOD_OOK && raw->pulse_count > 0) {
        uint32_t code = 0;
        uint8_t nbits = 0;
        bool got = false;
        if(radiotchi_ook_pwm_decode(raw->pulses, raw->pulse_count, &code, &nbits)) {
            // Clean PWM: we read the ACTUAL code -> a stable, privacy-safe per-device tag.
            radiotchi_individual_fingerprint(code, nbits, ev.individual, sizeof(ev.individual));
            got = true;
        } else if(radiotchi_repeating_frame(raw->pulses, raw->pulse_count, NULL)) {
            // A confirmed repeating remote of an encoding we don't bit-decode (e.g. Manchester):
            // reach VALUES, but emit NO individual tag. The waveform fingerprint is too coarse
            // to be a trustworthy per-device id (it collides across different buttons of one
            // remote, validated on hardware), so surfacing it would be misleading. A real
            // per-device id for these needs the firmware's protocol decoders (D28; TB.1).
            got = true; // ev.individual stays ""
        }
        if(got) {
            ev.decode_tier = TIER_VALUES;
            strncpy(ev.protocol, WV_PROTO_OOK_FIXED, sizeof(ev.protocol) - 1);
            ev.protocol[sizeof(ev.protocol) - 1] = '\0';
            snprintf(
                ev.species_id, sizeof(ev.species_id), "ook-fixed-%u",
                (unsigned)band_bucket_mhz(ev.frequency_hz));
            ev.species_id[sizeof(ev.species_id) - 1] = '\0';
        }
    } else if(raw->modulation == MOD_2FSK && raw->pulse_count > 0) {
        // Structured 2FSK sensor (weather/telemetry/TPMS): demodulate the PCM/NRZ frame from
        // the pulse timing -> TIER_VALUES. Privacy (A5): the species stays family-level
        // (fsk-sensor-<band>); the decoded frame only justifies the tier and a one-way tag.
        uint8_t frame[RADIOTCHI_FSK_FRAME_MAX];
        uint16_t flen = 0;
        if(radiotchi_fsk_sensor_decode(raw->pulses, raw->pulse_count, frame, sizeof(frame), &flen)) {
            ev.decode_tier = TIER_VALUES;
            strncpy(ev.protocol, WV_PROTO_FSK_SENSOR, sizeof(ev.protocol) - 1);
            ev.protocol[sizeof(ev.protocol) - 1] = '\0';
            snprintf(
                ev.species_id, sizeof(ev.species_id), "fsk-sensor-%u",
                (unsigned)band_bucket_mhz(ev.frequency_hz));
            ev.species_id[sizeof(ev.species_id) - 1] = '\0';
            radiotchi_individual_fingerprint_bytes(frame, flen, ev.individual, sizeof(ev.individual));
        }
    }

    // Carry the raw .sub reference across the boundary so the Game Shell can link
    // the lossless recording to this event.
    strncpy(ev.raw_sub_ref, raw->raw_sub_ref, sizeof(ev.raw_sub_ref) - 1);
    ev.raw_sub_ref[sizeof(ev.raw_sub_ref) - 1] = '\0';

    ev.scores = score_capture(&ev, dex_rarity);
    return ev;
}
