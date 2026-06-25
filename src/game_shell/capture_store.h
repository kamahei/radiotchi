// Radiotchi — Capture persistence (Game Shell side).
//
// Honors the lossless-capture invariant: every received signal is written BOTH as
//   - a raw `.sub` (the original level-duration pulse train, re-decodable later), and
//   - an analysis record appended to a capture log (timestamp + features + scores).
//
// Layout on the SD card (FatFs):
//   /ext/apps_data/radiotchi/captures/<UTC-stamp>_<freq>.sub   (raw, lossless)
//   /ext/apps_data/radiotchi/capture_log.csv                   (append-only analysis)

#pragma once

#include <furi_hal.h>
#include <stddef.h>
#include <stdint.h>

#include "radiotchi_types.h"
#include "pet_growth.h"
#include "pet_mood.h" // PetCare (persisted alongside the growth layer)

#ifdef __cplusplus
extern "C" {
#endif

// Capacity (incl. NUL) of the user-given pet name.
#define RADIOTCHI_PET_NAME_CAP 16u

typedef struct CaptureStore CaptureStore;
typedef struct Storage Storage; // opaque; full type in <storage/storage.h>
typedef struct SpeciesIndex SpeciesIndex; // opaque; full type in species_index.h

// User-tunable detection settings, persisted so on-device tuning survives restart.
typedef struct {
    int16_t rssi_threshold_dbm;
    int16_t detection_margin_db;
} CaptureTuning;

// Open the store (acquires the Storage record, ensures directories exist).
CaptureStore* capture_store_alloc(void);
void capture_store_free(CaptureStore* store);

// The shared Storage handle, so sibling modules (species index, growth) reuse
// it instead of opening a second RECORD_STORAGE.
Storage* capture_store_storage(CaptureStore* store);

// Current RTC time as a UNIX timestamp. The caller injects this into
// analyze_capture() so the Analysis Core stays clock-free (pure).
uint32_t capture_store_now(void);

// Load persisted tuning into *out. Returns false if no config exists yet (then
// the caller keeps its defaults).
bool capture_store_load_tuning(CaptureStore* store, CaptureTuning* out);

// Persist tuning so the next launch starts where the user left off.
void capture_store_save_tuning(CaptureStore* store, const CaptureTuning* tuning);

// Load/save the growth layer (5 stats + EXP/level/type), the care/mood state (last-feed
// time + meal quality), and the user-given name. Stats are centi-units; `name` is copied up
// to `name_cap` (incl. NUL). `care` may be NULL (then it is not loaded/saved). load returns
// false if no growth file exists yet (caller keeps init defaults). Back-compat: a pre-care
// growth.txt loads its growth+name unchanged and leaves *care at pet_care_init() (never-fed
// grace), so upgrading an existing pet does not punish it for pre-feature idle time.
bool capture_store_load_growth(
    CaptureStore* store, PetGrowth* out, PetCare* care, char* name, size_t name_cap);
void capture_store_save_growth(
    CaptureStore* store, const PetGrowth* g, const PetCare* care, const char* name);

// Stream the append-only capture log. `cb` is called once per data row (header
// skipped) with the split CSV fields; if `species_filter` is non-NULL, only rows
// whose species_id (column 8) matches are reported. Used by the dex to browse a
// species' captures without loading the whole log into RAM. Column order:
// 0=epoch 1=datetime 2=frequency_hz 3=modulation 4=rssi_dbm 5=bit_count 6=entropy
// 7=decode_tier 8=species_id 9..13=scores 14=raw_sub_ref 15=individual (optional;
// absent in pre-individual logs).
typedef void (*CaptureLogRowCb)(void* ctx, const char* const* fields, int nfields);
void capture_store_for_each_row(
    CaptureStore* store,
    const char* species_filter,
    CaptureLogRowCb cb,
    void* ctx);

// Collect up to `cap` decoded frame payloads for a species by streaming the log and re-reading
// each matching row's `.sub` READ-ONLY, then re-demodulating it (OOK code bytes / FSK frame).
// Rows that do not decode are skipped. Returns the number collected. Feeds radiotchi_byte_diff
// for the diff-learning dex view. `payloads[i]` holds the i-th frame's bytes; `lens[i]` its
// length. Privacy (A5): the caller renders byte CLASSES from the diff, never these raw bytes.
uint8_t capture_store_collect_payloads(
    CaptureStore* store,
    const char* species_filter,
    uint8_t payloads[][RADIOTCHI_DIFF_BYTES_MAX],
    uint16_t* lens,
    uint8_t cap);

// As capture_store_collect_payloads, but scoped to a single device: only rows whose
// `individual` tag (column 15) equals `individual_filter` are collected, so frames of
// DIFFERENT devices of one band-level species are not aligned together (the family may mix
// frame layouts). `individual_filter`==NULL (or "") collects ALL rows — identical to
// capture_store_collect_payloads (the species-wide fallback for the D28 no-id case).
// Privacy (A5): groups by the one-way id-XXXX tag, never a raw code.
uint8_t capture_store_collect_payloads_for_individual(
    CaptureStore* store,
    const char* species_filter,
    const char* individual_filter,
    uint8_t payloads[][RADIOTCHI_DIFF_BYTES_MAX],
    uint16_t* lens,
    uint8_t cap);

// Persist one capture. Writes the raw `.sub` from the pulse train, sets
// ev->raw_sub_ref to its path, then appends the analysis record to the log.
// Returns true on full success. `pulses` may be NULL/0 (then only the log row is
// written and raw_sub_ref stays empty).
bool capture_store_save(
    CaptureStore* store,
    CaptureEvent* ev,
    const int32_t* pulses,
    size_t pulse_count,
    FuriHalSubGhzPreset preset);

// Re-grade the whole capture log: re-run the (current) decoder over every stored row
// via `radiotchi_redecode`, raising decode_tier / species_id / nourishment where a
// decoder now recognizes it — the other four axes (and every other column) are left
// byte-for-byte unchanged (the lossless invariant; A12). The append-only log is
// rewritten atomically (temp file + rename) and `idx` is rebuilt from the re-graded
// rows and saved (so graduated species re-aggregate). The `.sub` files are never
// touched. Returns the number of rows whose tier changed, or -1 on error.
//
// It raises tiers up to PROTOCOL from the stored features (signature pass) AND, for an OOK or
// 2FSK row that still has its `.sub`, re-reads the lossless timing READ-ONLY and re-runs the
// real decoders to reach TIER_VALUES retroactively (the other four axes stay byte-for-byte).
int capture_store_regrade(CaptureStore* store, SpeciesIndex* idx);

#ifdef __cplusplus
}
#endif
