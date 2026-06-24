// Radiotchi - shared human-readable label mappers.
//
// One source of truth for enum -> string so the storage writer, the UI, and the
// dex reader never diverge.

#pragma once

#include "radiotchi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// "OOK" / "2FSK" / "GFSK" / "MSK" / "UNKNOWN".
const char* radiotchi_modulation_str(Modulation m);

// Inverse of radiotchi_modulation_str (e.g. for re-parsing a stored capture-log row).
// Unrecognized text maps to MOD_UNKNOWN.
Modulation radiotchi_modulation_from_str(const char* s);

// "RAW" / "MOD" / "PROTO" / "VALUES".
const char* radiotchi_tier_str(DecodeTier t);

#ifdef __cplusplus
}
#endif
