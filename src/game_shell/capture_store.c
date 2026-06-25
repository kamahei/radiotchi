// Radiotchi — Capture persistence implementation.

#include "capture_store.h"
#include "radiotchi_labels.h"
#include "species_index.h"

#include "analysis_core.h" // radiotchi_redecode (re-grade)

#include <furi.h>
#include <storage/storage.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "RadiotchiStore"

#define RADIOTCHI_DIR        "/ext/apps_data/radiotchi"
#define RADIOTCHI_RAW_DIR    RADIOTCHI_DIR "/captures"
#define RADIOTCHI_LOG_PATH   RADIOTCHI_DIR "/capture_log.csv"
#define RADIOTCHI_LOG_TMP    RADIOTCHI_DIR "/capture_log.tmp"
#define RADIOTCHI_TUNING_PATH RADIOTCHI_DIR "/tuning.txt"
#define RADIOTCHI_GROWTH_PATH RADIOTCHI_DIR "/growth.txt"

// Values per RAW_Data line in the .sub (matches OFW's chunking convention).
#define SUB_RAW_LINE_VALUES 512u

struct CaptureStore {
    Storage* storage;
};

// --- helpers ---------------------------------------------------------------

static const char* preset_to_string(FuriHalSubGhzPreset p) {
    switch(p) {
    case FuriHalSubGhzPresetOok270Async:
        return "FuriHalSubGhzPresetOok270Async";
    case FuriHalSubGhzPresetOok650Async:
        return "FuriHalSubGhzPresetOok650Async";
    case FuriHalSubGhzPreset2FSKDev238Async:
        return "FuriHalSubGhzPreset2FSKDev238Async";
    case FuriHalSubGhzPreset2FSKDev476Async:
        return "FuriHalSubGhzPreset2FSKDev476Async";
    case FuriHalSubGhzPresetMSK99_97KbAsync:
        return "FuriHalSubGhzPresetMSK99_97KbAsync";
    case FuriHalSubGhzPresetGFSK9_99KbAsync:
        return "FuriHalSubGhzPresetGFSK9_99KbAsync";
    default:
        return "FuriHalSubGhzPresetOok650Async";
    }
}

static void file_write_str(File* f, const char* s) {
    storage_file_write(f, s, strlen(s));
}

// --- lifecycle -------------------------------------------------------------

CaptureStore* capture_store_alloc(void) {
    CaptureStore* store = malloc(sizeof(CaptureStore));
    if(store == NULL) return NULL;
    store->storage = furi_record_open(RECORD_STORAGE);

    storage_simply_mkdir(store->storage, RADIOTCHI_DIR);
    storage_simply_mkdir(store->storage, RADIOTCHI_RAW_DIR);
    return store;
}

void capture_store_free(CaptureStore* store) {
    if(store == NULL) return;
    furi_record_close(RECORD_STORAGE);
    free(store);
}

Storage* capture_store_storage(CaptureStore* store) {
    return (store != NULL) ? store->storage : NULL;
}

uint32_t capture_store_now(void) {
    return furi_hal_rtc_get_timestamp();
}

// --- tuning persistence ----------------------------------------------------

bool capture_store_load_tuning(CaptureStore* store, CaptureTuning* out) {
    if(store == NULL || out == NULL) return false;
    if(!storage_file_exists(store->storage, RADIOTCHI_TUNING_PATH)) return false;

    File* f = storage_file_alloc(store->storage);
    bool ok = false;
    if(storage_file_open(f, RADIOTCHI_TUNING_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        char buf[64] = {0};
        size_t n = storage_file_read(f, buf, sizeof(buf) - 1);
        buf[n] = '\0';
        int thr = 0, mgn = 0;
        if(sscanf(buf, "threshold=%d margin=%d", &thr, &mgn) == 2) {
            out->rssi_threshold_dbm = (int16_t)thr;
            out->detection_margin_db = (int16_t)mgn;
            // Optional "sound=" / "vibro=" tokens. Absent in pre-feedback configs => default OFF.
            out->sound_enabled = 0;
            out->vibro_enabled = 0;
            unsigned snd = 0, vib = 0;
            const char* ps = strstr(buf, "sound=");
            if(ps != NULL && sscanf(ps + 6, "%u", &snd) == 1) out->sound_enabled = snd ? 1u : 0u;
            const char* pv = strstr(buf, "vibro=");
            if(pv != NULL && sscanf(pv + 6, "%u", &vib) == 1) out->vibro_enabled = vib ? 1u : 0u;
            ok = true;
        }
    }
    storage_file_close(f);
    storage_file_free(f);
    return ok;
}

void capture_store_save_tuning(CaptureStore* store, const CaptureTuning* tuning) {
    if(store == NULL || tuning == NULL) return;
    File* f = storage_file_alloc(store->storage);
    if(storage_file_open(f, RADIOTCHI_TUNING_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        FuriString* s = furi_string_alloc();
        furi_string_printf(
            s,
            "threshold=%d margin=%d sound=%u vibro=%u\n",
            (int)tuning->rssi_threshold_dbm,
            (int)tuning->detection_margin_db,
            (unsigned)(tuning->sound_enabled ? 1u : 0u),
            (unsigned)(tuning->vibro_enabled ? 1u : 0u));
        file_write_str(f, furi_string_get_cstr(s));
        furi_string_free(s);
    }
    storage_file_close(f);
    storage_file_free(f);
}

// --- growth + name persistence ---------------------------------------------

// Float -> integer centi-units for the compact text persistence below.
static int to_centi(float v) {
    return (int)(v * 100.0f + (v >= 0.0f ? 0.5f : -0.5f));
}

bool capture_store_load_growth(
    CaptureStore* store, PetGrowth* out, PetCare* care, PetQuests* quests, char* name, size_t name_cap) {
    if(store == NULL || out == NULL) return false;
    if(care != NULL) pet_care_init(care); // never-fed default; overwritten if a care= line exists
    if(quests != NULL) pet_quests_init(quests); // no-progress default; overwritten by a quest= line
    if(!storage_file_exists(store->storage, RADIOTCHI_GROWTH_PATH)) return false;

    File* f = storage_file_alloc(store->storage);
    bool ok = false;
    if(storage_file_open(f, RADIOTCHI_GROWTH_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        char buf[384] = {0}; // growth + name + care + quest lines (bumped from 256 for quest=)
        size_t n = storage_file_read(f, buf, sizeof(buf) - 1);
        buf[n] = '\0';
        int s0 = 0, s1 = 0, s2 = 0, s3 = 0, s4 = 0;
        unsigned long exp = 0;
        unsigned level = 0, type = PET_TYPE_UNFORMED;
        if(sscanf(
               buf,
               "s0=%d s1=%d s2=%d s3=%d s4=%d exp=%lu level=%u type=%u",
               &s0,
               &s1,
               &s2,
               &s3,
               &s4,
               &exp,
               &level,
               &type) == 8) {
            out->stat[0] = (float)s0 / 100.0f;
            out->stat[1] = (float)s1 / 100.0f;
            out->stat[2] = (float)s2 / 100.0f;
            out->stat[3] = (float)s3 / 100.0f;
            out->stat[4] = (float)s4 / 100.0f;
            out->total_exp = (uint32_t)exp;
            out->level = (uint16_t)level;
            out->type_id = (uint8_t)type;
            ok = true;
        }
        // Optional "name=<...>" line (may contain spaces; read to end of line).
        if(ok && name != NULL && name_cap > 0) {
            name[0] = '\0';
            const char* p = strstr(buf, "name=");
            if(p != NULL) {
                p += 5; // past "name="
                size_t i = 0;
                while(p[i] != '\0' && p[i] != '\n' && p[i] != '\r' && i + 1 < name_cap) {
                    name[i] = p[i];
                    i++;
                }
                name[i] = '\0';
            }
        }
        // Optional "care=<last_feed_time> <last_meal_quality>" line. Absent in pre-care files,
        // which then keep the never-fed grace set above (no false starvation on upgrade).
        if(ok && care != NULL) {
            const char* p = strstr(buf, "care=");
            if(p != NULL) {
                unsigned long long lft = 0;
                unsigned q = 0;
                if(sscanf(p + 5, "%llu %u", &lft, &q) >= 1) {
                    care->last_feed_time = (uint64_t)lft;
                    care->last_meal_quality = (uint8_t)(q > 100u ? 100u : q);
                }
            }
        }
        // Optional "quest=<total> <delicacy> <decoded> <species> <streak> <best> <last> <mask>"
        // line. Absent in pre-quest files, which keep the fresh pet_quests_init above.
        if(ok && quests != NULL) {
            const char* p = strstr(buf, "quest=");
            if(p != NULL) {
                unsigned long tf = 0, df = 0, dc = 0, sp = 0, mask = 0;
                unsigned fs = 0, bs = 0;
                unsigned long long lft = 0;
                if(sscanf(p + 6, "%lu %lu %lu %lu %u %u %llu %lu", &tf, &df, &dc, &sp, &fs, &bs, &lft, &mask) ==
                   8) {
                    quests->total_feeds = (uint32_t)tf;
                    quests->delicacy_feeds = (uint32_t)df;
                    quests->decoded_feeds = (uint32_t)dc;
                    quests->distinct_species = (uint32_t)sp;
                    quests->feed_streak = (uint16_t)fs;
                    quests->best_streak = (uint16_t)bs;
                    quests->last_feed_time = (uint64_t)lft;
                    quests->unlocked_mask = (uint32_t)mask;
                }
            }
        }
    }
    storage_file_close(f);
    storage_file_free(f);
    return ok;
}

void capture_store_save_growth(
    CaptureStore* store,
    const PetGrowth* g,
    const PetCare* care,
    const PetQuests* quests,
    const char* name) {
    if(store == NULL || g == NULL) return;
    File* f = storage_file_alloc(store->storage);
    if(storage_file_open(f, RADIOTCHI_GROWTH_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        FuriString* s = furi_string_alloc();
        furi_string_printf(
            s,
            "s0=%d s1=%d s2=%d s3=%d s4=%d exp=%lu level=%u type=%u\nname=%s\ncare=%llu %u\n"
            "quest=%lu %lu %lu %lu %u %u %llu %lu\n",
            to_centi(g->stat[0]),
            to_centi(g->stat[1]),
            to_centi(g->stat[2]),
            to_centi(g->stat[3]),
            to_centi(g->stat[4]),
            (unsigned long)g->total_exp,
            (unsigned)g->level,
            (unsigned)g->type_id,
            (name != NULL ? name : ""),
            (unsigned long long)(care != NULL ? care->last_feed_time : 0u),
            (unsigned)(care != NULL ? care->last_meal_quality : 0u),
            (unsigned long)(quests != NULL ? quests->total_feeds : 0u),
            (unsigned long)(quests != NULL ? quests->delicacy_feeds : 0u),
            (unsigned long)(quests != NULL ? quests->decoded_feeds : 0u),
            (unsigned long)(quests != NULL ? quests->distinct_species : 0u),
            (unsigned)(quests != NULL ? quests->feed_streak : 0u),
            (unsigned)(quests != NULL ? quests->best_streak : 0u),
            (unsigned long long)(quests != NULL ? quests->last_feed_time : 0u),
            (unsigned long)(quests != NULL ? quests->unlocked_mask : 0u));
        file_write_str(f, furi_string_get_cstr(s));
        furi_string_free(s);
    }
    storage_file_close(f);
    storage_file_free(f);
}

// --- raw .sub --------------------------------------------------------------

// Build a UTC-ish stamped filename from the RTC: 20260622_154233_433920000.sub
static void build_sub_path(char* out, size_t out_len, uint32_t frequency_hz) {
    DateTime dt;
    furi_hal_rtc_get_datetime(&dt);
    snprintf(
        out,
        out_len,
        RADIOTCHI_RAW_DIR "/%04u%02u%02u_%02u%02u%02u_%lu.sub",
        (unsigned)dt.year,
        (unsigned)dt.month,
        (unsigned)dt.day,
        (unsigned)dt.hour,
        (unsigned)dt.minute,
        (unsigned)dt.second,
        (unsigned long)frequency_hz);
}

static bool write_sub_file(
    CaptureStore* store,
    const char* path,
    uint32_t frequency_hz,
    FuriHalSubGhzPreset preset,
    const int32_t* pulses,
    size_t pulse_count) {
    File* f = storage_file_alloc(store->storage);
    if(!storage_file_open(f, path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        FURI_LOG_E(TAG, "open .sub failed: %s", path);
        storage_file_free(f);
        return false;
    }

    FuriString* hdr = furi_string_alloc();
    furi_string_printf(
        hdr,
        "Filetype: Flipper SubGhz RAW File\n"
        "Version: 1\n"
        "Frequency: %lu\n"
        "Preset: %s\n"
        "Protocol: RAW\n",
        (unsigned long)frequency_hz,
        preset_to_string(preset));
    file_write_str(f, furi_string_get_cstr(hdr));
    furi_string_free(hdr);

    // Emit the signed pulse train in chunked RAW_Data lines.
    FuriString* line = furi_string_alloc();
    for(size_t i = 0; i < pulse_count;) {
        furi_string_set(line, "RAW_Data:");
        size_t end = i + SUB_RAW_LINE_VALUES;
        if(end > pulse_count) end = pulse_count;
        for(; i < end; i++) {
            furi_string_cat_printf(line, " %ld", (long)pulses[i]);
        }
        furi_string_cat(line, "\n");
        file_write_str(f, furi_string_get_cstr(line));
    }
    furi_string_free(line);

    storage_file_close(f);
    storage_file_free(f);
    return true;
}

// --- analysis log ----------------------------------------------------------

static bool append_log_row(CaptureStore* store, const CaptureEvent* ev) {
    File* f = storage_file_alloc(store->storage);

    bool existed = storage_file_exists(store->storage, RADIOTCHI_LOG_PATH);
    if(!storage_file_open(f, RADIOTCHI_LOG_PATH, FSAM_WRITE, FSOM_OPEN_APPEND)) {
        FURI_LOG_E(TAG, "open log failed");
        storage_file_free(f);
        return false;
    }

    if(!existed) {
        file_write_str(
            f,
            "epoch,datetime,frequency_hz,modulation,rssi_dbm,bit_count,entropy,"
            "decode_tier,species_id,calories,freshness,additives,rarity,nourishment,"
            "raw_sub_ref,individual\n");
    }

    DateTime dt;
    furi_hal_rtc_get_datetime(&dt);

    FuriString* row = furi_string_alloc();
    furi_string_printf(
        row,
        "%llu,%04u-%02u-%02uT%02u:%02u:%02u,%lu,%s,%d,%u,%.3f,%d,%s,"
        "%.3f,%.3f,%.3f,%.3f,%.3f,%s,%s\n",
        (unsigned long long)ev->timestamp,
        (unsigned)dt.year,
        (unsigned)dt.month,
        (unsigned)dt.day,
        (unsigned)dt.hour,
        (unsigned)dt.minute,
        (unsigned)dt.second,
        (unsigned long)ev->frequency_hz,
        radiotchi_modulation_str(ev->modulation),
        (int)ev->rssi_dbm,
        (unsigned)ev->bit_count,
        (double)ev->entropy,
        (int)ev->decode_tier,
        ev->species_id,
        (double)ev->scores.calories,
        (double)ev->scores.freshness,
        (double)ev->scores.additives,
        (double)ev->scores.rarity,
        (double)ev->scores.nourishment,
        ev->raw_sub_ref,
        ev->individual);
    file_write_str(f, furi_string_get_cstr(row));
    furi_string_free(row);

    storage_file_close(f);
    storage_file_free(f);
    return true;
}

// --- capture log streaming reader ------------------------------------------

// Columns (see append_log_row): 0=epoch, 1=datetime, ..., 8=species_id.
void capture_store_for_each_row(
    CaptureStore* store,
    const char* species_filter,
    CaptureLogRowCb cb,
    void* ctx) {
    if(store == NULL || cb == NULL) return;
    if(!storage_file_exists(store->storage, RADIOTCHI_LOG_PATH)) return;

    File* f = storage_file_alloc(store->storage);
    if(storage_file_open(f, RADIOTCHI_LOG_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        char chunk[256];
        char line[256];
        size_t line_len = 0;
        bool header_skipped = false;
        size_t n;
        while((n = storage_file_read(f, chunk, sizeof(chunk))) > 0) {
            for(size_t i = 0; i < n; i++) {
                char c = chunk[i];
                if(c == '\n' || line_len >= sizeof(line) - 1) {
                    line[line_len] = '\0';
                    if(line_len > 0) {
                        if(!header_skipped) {
                            header_skipped = true;
                        } else {
                            // Split a private copy on ',' manually (firmware has
                            // no strtok). Handles empty fields.
                            char buf[256];
                            strncpy(buf, line, sizeof(buf) - 1);
                            buf[sizeof(buf) - 1] = '\0';
                            char* fields[16];
                            int nf = 0;
                            fields[nf++] = buf;
                            for(char* pp = buf; *pp && nf < 16; pp++) {
                                if(*pp == ',') {
                                    *pp = '\0';
                                    fields[nf++] = pp + 1;
                                }
                            }
                            if(nf >= 9) {
                                const char* species = fields[8];
                                if(species_filter == NULL ||
                                   strncmp(species, species_filter, RADIOTCHI_SPECIES_LEN) == 0) {
                                    cb(ctx, (const char* const*)fields, nf);
                                }
                            }
                        }
                    }
                    line_len = 0;
                } else if(c != '\r') {
                    line[line_len++] = c;
                }
            }
        }
    }
    storage_file_close(f);
    storage_file_free(f);
}

// --- diff-learning payload collection --------------------------------------

// Decode one stored `.sub` to its frame bytes for the diff view: the OOK fixed-code (packed
// MSB-first) or the 2FSK sensor frame. READ-ONLY. Returns the byte length (0 if it doesn't
// decode). Note: read_sub_pulses is defined in the re-grade section below; forward-declared.
static uint16_t read_sub_pulses(Storage* storage, const char* sub_path, int16_t* pulses, uint16_t cap);

static uint16_t decode_sub_payload(
    Storage* storage, const char* sub_path, Modulation mod, uint8_t* out, uint16_t cap) {
    int16_t pulses[RADIOTCHI_PULSES_MAX];
    uint16_t pc = read_sub_pulses(storage, sub_path, pulses, RADIOTCHI_PULSES_MAX);
    if(pc == 0) return 0;

    if(mod == MOD_2FSK) {
        uint8_t frame[RADIOTCHI_FSK_FRAME_MAX];
        uint16_t flen = 0;
        if(!radiotchi_fsk_sensor_decode(pulses, pc, frame, sizeof(frame), &flen)) return 0;
        if(flen > cap) flen = cap;
        memcpy(out, frame, flen);
        return flen;
    }

    uint32_t code = 0;
    uint8_t nbits = 0;
    if(!radiotchi_ook_pwm_decode(pulses, pc, &code, &nbits)) return 0;
    uint16_t nbytes = (uint16_t)((nbits + 7u) / 8u);
    if(nbytes > cap) nbytes = cap;
    for(uint16_t i = 0; i < nbytes; i++) { // pack the code MSB-first (stable across captures)
        uint8_t shift = (uint8_t)((nbytes - 1u - i) * 8u);
        out[i] = (uint8_t)((code >> shift) & 0xFFu);
    }
    return nbytes;
}

typedef struct {
    Storage* storage;
    uint8_t (*payloads)[RADIOTCHI_DIFF_BYTES_MAX];
    uint16_t* lens;
    uint8_t cap;
    uint8_t count;
    const char* individual_filter; // NULL => all rows; else require col-15 == this tag
} CollectCtx;

static void collect_payload_row(void* ctx, const char* const* fields, int nf) {
    CollectCtx* c = ctx;
    if(c->count >= c->cap || nf < 15) return;
    const char* sub_ref = fields[14];
    if(sub_ref[0] == '\0') return; // no lossless `.sub` to re-read
    if(c->individual_filter != NULL && c->individual_filter[0] != '\0') {
        // device-scoped: the `individual` tag is the optional column 15 (absent in old logs).
        if(nf < 16 || strcmp(fields[15], c->individual_filter) != 0) return;
    }
    Modulation mod = radiotchi_modulation_from_str(fields[3]);
    uint16_t len =
        decode_sub_payload(c->storage, sub_ref, mod, c->payloads[c->count], RADIOTCHI_DIFF_BYTES_MAX);
    if(len == 0) return; // undecodable row: skip (only comparable frames make the diff)
    c->lens[c->count] = len;
    c->count++;
}

uint8_t capture_store_collect_payloads_for_individual(
    CaptureStore* store,
    const char* species_filter,
    const char* individual_filter,
    uint8_t payloads[][RADIOTCHI_DIFF_BYTES_MAX],
    uint16_t* lens,
    uint8_t cap) {
    if(store == NULL || payloads == NULL || lens == NULL || cap == 0) return 0;
    CollectCtx ctx = {store->storage, payloads, lens, cap, 0, individual_filter};
    capture_store_for_each_row(store, species_filter, collect_payload_row, &ctx);
    return ctx.count;
}

uint8_t capture_store_collect_payloads(
    CaptureStore* store,
    const char* species_filter,
    uint8_t payloads[][RADIOTCHI_DIFF_BYTES_MAX],
    uint16_t* lens,
    uint8_t cap) {
    return capture_store_collect_payloads_for_individual(
        store, species_filter, NULL, payloads, lens, cap);
}

// --- re-grade pass ---------------------------------------------------------

// Read a stored `.sub`'s pulse train (its RAW_Data lines) into `pulses`, READ-ONLY (the
// lossless invariant is never at risk). Returns the pulse count. Shared by the re-grade
// (try_values_from_sub) and the diff-learning payload collector.
static uint16_t read_sub_pulses(Storage* storage, const char* sub_path, int16_t* pulses, uint16_t cap) {
    File* f = storage_file_alloc(storage);
    uint16_t pc = 0;
    if(storage_file_open(f, sub_path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        char chunk[256];
        char line[256];
        size_t line_len = 0;
        size_t n;
        while(pc < cap && (n = storage_file_read(f, chunk, sizeof(chunk))) > 0) {
            for(size_t i = 0; i < n; i++) {
                char c = chunk[i];
                if(c == '\n' || line_len >= sizeof(line) - 1) {
                    line[line_len] = '\0';
                    pc = radiotchi_parse_raw_data(line, pulses, cap, pc);
                    line_len = 0;
                    if(pc >= cap) break;
                } else if(c != '\r') {
                    line[line_len++] = c;
                }
            }
        }
        if(line_len > 0 && pc < cap) { // a final line with no newline
            line[line_len] = '\0';
            pc = radiotchi_parse_raw_data(line, pulses, cap, pc);
        }
    }
    storage_file_close(f);
    storage_file_free(f);
    return pc;
}

// Retroactive VALUES: re-read the lossless `.sub` timing and run the real decoder for the
// capture's modulation (OOK PWM / repeating-frame, or the 2FSK sensor demod). On success the
// event jumps to TIER_VALUES with the family-level protocol/species (never a per-device id).
static void try_values_from_sub(Storage* storage, const char* sub_path, CaptureEvent* ev) {
    int16_t pulses[RADIOTCHI_PULSES_MAX];
    uint16_t pc = read_sub_pulses(storage, sub_path, pulses, RADIOTCHI_PULSES_MAX);
    if(pc == 0) return;

    if(ev->modulation == MOD_2FSK) {
        uint8_t frame[RADIOTCHI_FSK_FRAME_MAX];
        uint16_t flen = 0;
        if(radiotchi_fsk_sensor_decode(pulses, pc, frame, sizeof(frame), &flen)) {
            radiotchi_individual_fingerprint_bytes(frame, flen, ev->individual, sizeof(ev->individual));
            ev->decode_tier = TIER_VALUES;
            strncpy(ev->protocol, "FSK-Sensor", sizeof(ev->protocol) - 1);
            ev->protocol[sizeof(ev->protocol) - 1] = '\0';
            snprintf(
                ev->species_id, sizeof(ev->species_id), "fsk-sensor-%lu",
                (unsigned long)(ev->frequency_hz / 1000000u));
            ev->scores.nourishment = radiotchi_tier_nourishment(TIER_VALUES);
        }
        return;
    }

    // OOK fixed-code: the PWM bit decoder (with a per-device tag), else a confirmed repeating
    // remote of an encoding we don't bit-decode (VALUES, but no misleading individual).
    uint32_t code = 0;
    uint8_t nbits = 0;
    bool got = false;
    if(radiotchi_ook_pwm_decode(pulses, pc, &code, &nbits)) {
        radiotchi_individual_fingerprint(code, nbits, ev->individual, sizeof(ev->individual));
        got = true;
    } else if(radiotchi_repeating_frame(pulses, pc, NULL)) {
        got = true; // ev->individual stays as-is (empty for a fresh re-grade)
    }
    if(got) {
        ev->decode_tier = TIER_VALUES;
        strncpy(ev->protocol, "OOK-FixedCode", sizeof(ev->protocol) - 1);
        ev->protocol[sizeof(ev->protocol) - 1] = '\0';
        snprintf(
            ev->species_id, sizeof(ev->species_id), "ook-fixed-%lu",
            (unsigned long)(ev->frequency_hz / 1000000u));
        ev->scores.nourishment = radiotchi_tier_nourishment(TIER_VALUES);
    }
}

// Process one capture-log data line: bump `idx` with its (possibly re-graded) species
// and render the row to write into `out` — rebuilt when the tier rose (cols 7/8/13:
// decode_tier, species_id, nourishment), else byte-for-byte verbatim. Returns true if
// the tier changed. Reconstructs only the fields the decoders need; an OOK row with a
// `.sub` is re-demodulated for retroactive TIER_VALUES.
static bool regrade_row(Storage* storage, const char* line, SpeciesIndex* idx, FuriString* out) {
    char buf[256];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char* fields[16];
    int nf = 0;
    fields[nf++] = buf;
    for(char* p = buf; *p && nf < 16; p++) {
        if(*p == ',') {
            *p = '\0';
            fields[nf++] = p + 1;
        }
    }

    // Malformed/short row: pass through untouched (still index its species if present).
    if(nf < 15) {
        furi_string_set(out, line);
        furi_string_cat(out, "\n");
        if(nf >= 9 && idx) species_index_bump(idx, fields[8], strtoull(fields[0], NULL, 10));
        return false;
    }

    DecodeTier original = (DecodeTier)strtol(fields[7], NULL, 10);

    CaptureEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.frequency_hz = (uint32_t)strtoul(fields[2], NULL, 10);
    ev.modulation = radiotchi_modulation_from_str(fields[3]);
    ev.bit_count = (uint16_t)strtoul(fields[5], NULL, 10);
    ev.entropy = strtof(fields[6], NULL);
    ev.decode_tier = original;
    strncpy(ev.species_id, fields[8], sizeof(ev.species_id) - 1);
    ev.scores.nourishment = strtof(fields[13], NULL);
    if(nf >= 16) strncpy(ev.individual, fields[15], sizeof(ev.individual) - 1); // preserve existing

    radiotchi_redecode(&ev); // signature pass: may raise up to PROTOCOL
    // Retroactive VALUES: re-read the .sub timing for an OOK or 2FSK capture and really decode.
    if(ev.decode_tier < TIER_VALUES &&
       (ev.modulation == MOD_OOK || ev.modulation == MOD_2FSK) && fields[14][0] != '\0') {
        try_values_from_sub(storage, fields[14], &ev);
    }
    bool changed = (ev.decode_tier != original);
    if(idx) species_index_bump(idx, ev.species_id, strtoull(fields[0], NULL, 10));

    if(changed) {
        furi_string_printf(
            out,
            "%s,%s,%s,%s,%s,%s,%s,%d,%s,%s,%s,%s,%s,%.3f,%s,%s\n",
            fields[0], fields[1], fields[2], fields[3], fields[4], fields[5], fields[6],
            (int)ev.decode_tier, ev.species_id, fields[9], fields[10], fields[11], fields[12],
            (double)ev.scores.nourishment, fields[14], ev.individual);
    } else {
        furi_string_set(out, line);
        furi_string_cat(out, "\n");
    }
    return changed;
}

int capture_store_regrade(CaptureStore* store, SpeciesIndex* idx) {
    if(store == NULL) return -1;
    if(!storage_file_exists(store->storage, RADIOTCHI_LOG_PATH)) return 0;

    File* in = storage_file_alloc(store->storage);
    File* out = storage_file_alloc(store->storage);
    int changed = -1;

    if(storage_file_open(in, RADIOTCHI_LOG_PATH, FSAM_READ, FSOM_OPEN_EXISTING) &&
       storage_file_open(out, RADIOTCHI_LOG_TMP, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        changed = 0;
        if(idx) species_index_clear(idx); // rebuild it from the re-graded rows
        file_write_str(
            out,
            "epoch,datetime,frequency_hz,modulation,rssi_dbm,bit_count,entropy,"
            "decode_tier,species_id,calories,freshness,additives,rarity,nourishment,"
            "raw_sub_ref,individual\n");

        FuriString* emit = furi_string_alloc();
        char chunk[256];
        char line[256];
        size_t line_len = 0;
        bool header_skipped = false;
        size_t n;
        while((n = storage_file_read(in, chunk, sizeof(chunk))) > 0) {
            for(size_t i = 0; i < n; i++) {
                char c = chunk[i];
                if(c == '\n' || line_len >= sizeof(line) - 1) {
                    line[line_len] = '\0';
                    if(line_len > 0) {
                        if(!header_skipped) {
                            header_skipped = true; // drop the original header
                        } else {
                            if(regrade_row(store->storage, line, idx, emit)) changed++;
                            file_write_str(out, furi_string_get_cstr(emit));
                        }
                    }
                    line_len = 0;
                } else if(c != '\r') {
                    line[line_len++] = c;
                }
            }
        }
        furi_string_free(emit);
    }

    storage_file_close(in);
    storage_file_free(in);
    storage_file_close(out);
    storage_file_free(out);

    if(changed < 0) { // could not open one of the files
        storage_common_remove(store->storage, RADIOTCHI_LOG_TMP);
        return -1;
    }

    // Swap the rebuilt log in atomically; the lossless `.sub` files are never touched.
    storage_common_remove(store->storage, RADIOTCHI_LOG_PATH);
    if(storage_common_rename(store->storage, RADIOTCHI_LOG_TMP, RADIOTCHI_LOG_PATH) != FSE_OK) {
        FURI_LOG_E(TAG, "regrade: rename temp log failed");
        return -1;
    }
    if(idx) species_index_save(idx);
    return changed;
}

// --- public save -----------------------------------------------------------

bool capture_store_save(
    CaptureStore* store,
    CaptureEvent* ev,
    const int32_t* pulses,
    size_t pulse_count,
    FuriHalSubGhzPreset preset) {
    if(store == NULL || ev == NULL) return false;

    bool raw_ok = true;
    if(pulses != NULL && pulse_count > 0) {
        char path[RADIOTCHI_SUBREF_LEN];
        build_sub_path(path, sizeof(path), ev->frequency_hz);
        raw_ok = write_sub_file(store, path, ev->frequency_hz, preset, pulses, pulse_count);
        if(raw_ok) {
            strncpy(ev->raw_sub_ref, path, sizeof(ev->raw_sub_ref) - 1);
            ev->raw_sub_ref[sizeof(ev->raw_sub_ref) - 1] = '\0';
        }
    }

    bool log_ok = append_log_row(store, ev);
    return raw_ok && log_ok;
}
