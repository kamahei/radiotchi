// Radiotchi — offline `.sub` decode analyzer (host tool).
//
// Feed it Flipper SubGhz RAW `.sub` captures and it runs the WHOLE pure-core decode dispatch plus
// per-slicer diagnostics, so when a real device does or does not decode you can see exactly why
// (estimated bit timing, each demodulator's bytes, which decoder fired) without reflashing.
//
// NOTE: this covers the PURE-CORE decoders only (lib/analysis_core). The on-device firmware Sub-GHz
// registry (Princeton/KeeLoq/CAME/... via the CC1101 adapter) is additional and not runnable here.
//
// Build (from repo root):
//   cc -std=c99 -Wall -I lib/analysis_core tools/sub_analyze.c lib/analysis_core/analysis_core.c -o sub_analyze
//   (or: make -C test sub_analyze  ->  test/build/sub_analyze)
// Run:
//   ./sub_analyze captures/**/*.sub

#include "analysis_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXP 4096

// Map a `.sub` "Preset:" string to the modulation the demod path expects (host mirror of the
// capture source's mod_from_preset; substring match so minor naming differences still resolve).
static Modulation mod_from_preset(const char* preset) {
    if(strstr(preset, "Ook")) return MOD_OOK;
    if(strstr(preset, "2FSK")) return MOD_2FSK;
    if(strstr(preset, "GFSK")) return MOD_GFSK;
    if(strstr(preset, "MSK")) return MOD_MSK;
    return MOD_UNKNOWN;
}

static const char* mod_name(Modulation m) {
    switch(m) {
    case MOD_OOK: return "OOK";
    case MOD_2FSK: return "2FSK";
    case MOD_GFSK: return "GFSK";
    case MOD_MSK: return "MSK";
    default: return "UNKNOWN";
    }
}

static const char* tier_name(DecodeTier t) {
    switch(t) {
    case TIER_RAW: return "RAW";
    case TIER_MODULATION: return "MODULATION";
    case TIER_PROTOCOL: return "PROTOCOL";
    case TIER_VALUES: return "VALUES";
    default: return "?";
    }
}

static void hexdump(const uint8_t* b, uint16_t n) {
    for(uint16_t i = 0; i < n; i++) printf("%02X ", b[i]);
}

static void analyze(const char* path) {
    FILE* f = fopen(path, "r");
    if(f == NULL) {
        printf("== %s ==\n  (cannot open)\n", path);
        return;
    }

    uint32_t freq = 0;
    char preset[64] = {0};
    int16_t pulses[MAXP];
    uint16_t n = 0;
    char line[8192];
    while(fgets(line, sizeof(line), f) != NULL) {
        if(strncmp(line, "Frequency:", 10) == 0) {
            freq = (uint32_t)strtoul(line + 10, NULL, 10);
        } else if(strncmp(line, "Preset:", 7) == 0) {
            sscanf(line + 7, "%63s", preset);
        } else if(n < MAXP) {
            n = radiotchi_parse_raw_data(line, pulses, MAXP, n);
        }
    }
    fclose(f);

    Modulation mod = mod_from_preset(preset);
    unsigned band = freq / 1000000u;
    // On device only the first RADIOTCHI_PULSES_MAX pulses cross into the decoder (RawCapture /
    // the .sub re-grade), so decode the same prefix here for a verdict that matches the device.
    uint16_t dn = n < RADIOTCHI_PULSES_MAX ? n : RADIOTCHI_PULSES_MAX;
    printf("== %s ==\n", path);
    printf(
        "  freq=%lu (%u MHz)  preset=%s  mod=%s  pulses=%u%s\n",
        (unsigned long)freq, band, preset[0] ? preset : "?", mod_name(mod), n,
        n > RADIOTCHI_PULSES_MAX ? " (on-device decodes first 256)" : "");

    // Glitch-coalesce the train (the same pre-pass the dispatch applies) so the diagnostics match
    // the verdict and a real capture's slicer dips don't desync the pair-walking PWM/PPM slicers.
    int16_t clean[RADIOTCHI_PULSES_MAX];
    dn = radiotchi_coalesce_glitches(pulses, dn, clean, RADIOTCHI_PULSES_MAX, 24u);

    // The headline verdict: the shared pulse-decode dispatch (specific decoders -> generic families).
    CaptureEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.frequency_hz = freq;
    ev.modulation = mod;
    bool decoded = radiotchi_decode_from_pulses(freq, mod, clean, dn, &ev);
    if(decoded) {
        printf(
            "  VERDICT: tier=%s protocol=%s species=%s indiv=%s\n", tier_name(ev.decode_tier),
            ev.protocol, ev.species_id, ev.individual[0] ? ev.individual : "-");
    } else {
        printf("  VERDICT: no pure-core VALUES decode (would stay at classifier tier on device)\n");
    }

    // Per-demodulator diagnostics — what each slicer recovers, so a near-miss is visible.
    printf("  demod:\n");
    int shown = 0;
    uint32_t code = 0;
    uint8_t nbits = 0;
    if(radiotchi_ook_pwm_decode(clean, dn, &code, &nbits)) {
        printf("    ook_pwm_decode:      code=0x%lX nbits=%u\n", (unsigned long)code, nbits);
        shown++;
    }
    if(radiotchi_manchester_decode(clean, dn, &code, &nbits)) {
        printf("    manchester_decode:   code=0x%lX nbits=%u\n", (unsigned long)code, nbits);
        shown++;
    }

    uint8_t bytes[64];
    uint16_t nb = 0;
    if(radiotchi_pwm_to_bytes(clean, dn, bytes, sizeof(bytes), &nb)) {
        printf("    pwm_to_bytes (%2ub):  ", nb);
        hexdump(bytes, (uint16_t)((nb + 7) / 8));
        printf("\n");
        shown++;
    }
    if(radiotchi_ppm_to_bytes(clean, dn, bytes, sizeof(bytes), &nb)) {
        printf("    ppm_to_bytes (%2ub):  ", nb);
        hexdump(bytes, (uint16_t)((nb + 7) / 8));
        printf("\n");
        shown++;
    }
    if(radiotchi_manchester_to_bytes(clean, dn, bytes, sizeof(bytes), &nb)) {
        printf("    manch_to_bytes(%2ub): ", nb);
        hexdump(bytes, (uint16_t)((nb + 7) / 8));
        printf("\n");
        shown++;
    }
    uint16_t flen = 0;
    if(radiotchi_fsk_sensor_decode(clean, dn, bytes, sizeof(bytes), &flen)) {
        printf("    fsk_sensor (%2uB):    ", flen);
        hexdump(bytes, flen);
        printf("\n");
        shown++;
    }
    uint16_t fp = 0;
    if(radiotchi_repeating_frame(clean, dn, &fp)) {
        printf("    repeating_frame:     id-%04x\n", fp);
        shown++;
    }
    if(shown == 0)
        printf("    (no demodulator produced a frame — likely noise or an unsupported encoding)\n");
}

int main(int argc, char** argv) {
    if(argc < 2) {
        printf("usage: %s <capture.sub> [more.sub ...]\n", argv[0]);
        return 2;
    }
    for(int a = 1; a < argc; a++) analyze(argv[a]);
    return 0;
}
