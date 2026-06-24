// Radiotchi - species index (the hot "species layer" of the dex).
//
// One record per device/protocol type (or provisional fingerprint-species). Held
// in a small in-RAM array, persisted to SD `species.csv`. Drives personal Rarity:
// the feed path looks up a species' prior count to build a RarityView before
// scoring, and bumps the count when the pet eats.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "radiotchi_types.h" // RADIOTCHI_SPECIES_LEN

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Storage Storage; // opaque; borrowed from CaptureStore

#define SPECIES_INDEX_MAX 128u // RAM cap; real distinct-species count is small

typedef struct {
    char species_id[RADIOTCHI_SPECIES_LEN];
    uint32_t count;
    uint64_t first_seen;
    uint64_t last_seen;
} SpeciesRecord;

typedef struct SpeciesIndex SpeciesIndex;

// Allocate over a borrowed Storage handle (does not own it). NULL on OOM.
SpeciesIndex* species_index_alloc(Storage* storage);
void species_index_free(SpeciesIndex* idx);

// Drop all in-RAM records (count -> 0) without touching the file. Used to rebuild
// the index from the capture log during a re-grade pass.
void species_index_clear(SpeciesIndex* idx);

// Parse species.csv into RAM (false if the file does not exist yet — that is fine).
bool species_index_load(SpeciesIndex* idx);
// Rewrite species.csv from RAM.
bool species_index_save(SpeciesIndex* idx);

// Lookup by id (NULL if unseen). Result valid until the next bump/load.
const SpeciesRecord* species_index_find(SpeciesIndex* idx, const char* species_id);

// Sum of all counts (the RarityView.total_captures normalizer).
uint32_t species_index_total(SpeciesIndex* idx);
// Number of distinct species (dex list length).
uint32_t species_index_count(SpeciesIndex* idx);
// Record at list position i (NULL if out of range).
const SpeciesRecord* species_index_get(SpeciesIndex* idx, uint32_t i);

// Create-or-increment a species: bumps count, sets first_seen on creation, updates
// last_seen. Returns false only if the table is full (caller should still keep the
// lossless capture log — the count bump is the only thing skipped).
bool species_index_bump(SpeciesIndex* idx, const char* species_id, uint64_t now);

#ifdef __cplusplus
}
#endif
