// Radiotchi - shared label mappers.

#include "radiotchi_labels.h"

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
