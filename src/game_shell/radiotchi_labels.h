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

// Render a short, human-readable dex label for a `species_id` (display only — the species_id stays
// the stable dex key). Recognized families become friendly names with their band, e.g.
// "weather-acurite-433" -> "Acurite 433", "sensor-2dd4-4B-c31-868" -> "FSK sns 868",
// "ook-fixed-433" -> "Fixed 433", "F433-M1-L0256" -> "OOK? 433". Unrecognized ids (firmware
// protocol names, keyfob/gate brands which already read well) are copied verbatim. Writes a
// NUL-terminated label into `out`. Pure.
void radiotchi_species_label(const char* species_id, char* out, size_t out_len);

#ifdef __cplusplus
}
#endif
