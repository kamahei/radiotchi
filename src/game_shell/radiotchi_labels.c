// Radiotchi - shared label mappers.

#include "radiotchi_labels.h"

#include <stdio.h>
#include <string.h>

const char* radiotchi_modulation_str(Modulation m) {
    switch(m) {
    case MOD_OOK:
        return "OOK";
    case MOD_2FSK:
        return "2FSK";
    case MOD_GFSK:
        return "GFSK";
    case MOD_MSK:
        return "MSK";
    default:
        return "UNKNOWN";
    }
}

Modulation radiotchi_modulation_from_str(const char* s) {
    if(s == NULL) return MOD_UNKNOWN;
    if(strcmp(s, "OOK") == 0) return MOD_OOK;
    if(strcmp(s, "2FSK") == 0) return MOD_2FSK;
    if(strcmp(s, "GFSK") == 0) return MOD_GFSK;
    if(strcmp(s, "MSK") == 0) return MOD_MSK;
    return MOD_UNKNOWN;
}

const char* radiotchi_tier_str(DecodeTier t) {
    switch(t) {
    case TIER_RAW:
        return "RAW";
    case TIER_MODULATION:
        return "MOD";
    case TIER_PROTOCOL:
        return "PROTO";
    case TIER_VALUES:
        return "VALUES";
    default:
        return "RAW";
    }
}

// Trailing band = the digits after the final '-' (e.g. "...-433" -> 433); 0 if none.
static unsigned trailing_band(const char* id) {
    const char* dash = NULL;
    for(const char* q = id; *q != '\0'; q++) {
        if(*q == '-') dash = q;
    }
    if(dash == NULL || dash[1] == '\0') return 0;
    unsigned band = 0;
    for(const char* q = dash + 1; *q != '\0'; q++) {
        if(*q < '0' || *q > '9') return 0; // not a pure-numeric band
        band = band * 10u + (unsigned)(*q - '0');
    }
    return band;
}

void radiotchi_species_label(const char* species_id, char* out, size_t out_len) {
    if(out == NULL || out_len == 0) return;
    out[0] = '\0';
    if(species_id == NULL || species_id[0] == '\0') return;

    // Provisional fingerprint "F<band>-M<mod>-L<len>" -> "<mod>? <band>" (an un-decoded bucket).
    if(species_id[0] == 'F' && species_id[1] >= '0' && species_id[1] <= '9') {
        unsigned band = 0;
        const char* p = species_id + 1;
        while(*p >= '0' && *p <= '9') band = band * 10u + (unsigned)(*p++ - '0');
        const char* mp = strstr(species_id, "-M");
        char mc = mp ? mp[2] : '0';
        const char* mod = (mc == '1') ? "OOK" :
                          (mc == '2') ? "2FSK" :
                          (mc == '3') ? "GFSK" :
                          (mc == '4') ? "MSK" :
                                        "RF";
        snprintf(out, out_len, "%s? %u", mod, band);
        out[out_len - 1] = '\0';
        return;
    }

    // Recognized families -> a short friendly base name (else fall back to the id verbatim).
    static const struct {
        const char* prefix;
        const char* name;
    } kFamilies[] = {
        {"weather-acurite", "Acurite"}, {"th-nexus", "Nexus"},
        {"weather-oregon", "Oregon"},   {"th-oregon", "Oregon TH"},
        {"uv-oregon", "Oregon UV"},     {"th-fineoffset", "FineOfst"},
        {"sensor-2dd4", "FSK sns"},     {"sensor-ook", "OOK sns"},
        {"sensor-fsk", "FSK sns"},      {"sensor-manch", "Manch sns"},
        {"fsk-sensor", "FSK sns"},      {"ook-fixed", "Fixed"},
    };
    const char* base = NULL;
    for(size_t i = 0; i < sizeof(kFamilies) / sizeof(kFamilies[0]); i++) {
        size_t plen = strlen(kFamilies[i].prefix);
        if(strncmp(species_id, kFamilies[i].prefix, plen) == 0) {
            base = kFamilies[i].name;
            break;
        }
    }
    if(base == NULL) {
        // Branded families "<class>-<make>-<band>" (D36) -> drop the class word so the make + band
        // fit the dex row (e.g. "keyfob-starline-433" -> "starline-433").
        static const char* const kStrip[] = {"keyfob-", "gate-", "rolling-"};
        for(size_t i = 0; i < sizeof(kStrip) / sizeof(kStrip[0]); i++) {
            size_t plen = strlen(kStrip[i]);
            if(strncmp(species_id, kStrip[i], plen) == 0) {
                strncpy(out, species_id + plen, out_len - 1);
                out[out_len - 1] = '\0';
                return;
            }
        }
        // Firmware protocol names etc. already read well — keep verbatim.
        strncpy(out, species_id, out_len - 1);
        out[out_len - 1] = '\0';
        return;
    }

    unsigned band = trailing_band(species_id);
    if(band != 0) {
        snprintf(out, out_len, "%s %u", base, band);
    } else {
        strncpy(out, base, out_len - 1);
    }
    out[out_len - 1] = '\0';
}
