// Radiotchi - species index implementation.

#include "species_index.h"

#include <furi.h>
#include <storage/storage.h>

#include <stdio.h>
#include <string.h>

#define TAG "RadiotchiSpecies"

#define RADIOTCHI_DIR          "/ext/apps_data/radiotchi"
#define RADIOTCHI_SPECIES_PATH RADIOTCHI_DIR "/species.csv"

struct SpeciesIndex {
    Storage* storage; // borrowed
    SpeciesRecord records[SPECIES_INDEX_MAX];
    uint32_t count;
};

SpeciesIndex* species_index_alloc(Storage* storage) {
    SpeciesIndex* idx = malloc(sizeof(SpeciesIndex));
    if(idx == NULL) return NULL;
    memset(idx, 0, sizeof(*idx));
    idx->storage = storage;
    return idx;
}

void species_index_free(SpeciesIndex* idx) {
    if(idx != NULL) free(idx);
}

// --- lookup / accessors ----------------------------------------------------

static SpeciesRecord* find_mut(SpeciesIndex* idx, const char* species_id) {
    for(uint32_t i = 0; i < idx->count; i++) {
        if(strncmp(idx->records[i].species_id, species_id, RADIOTCHI_SPECIES_LEN) == 0) {
            return &idx->records[i];
        }
    }
    return NULL;
}

const SpeciesRecord* species_index_find(SpeciesIndex* idx, const char* species_id) {
    if(idx == NULL || species_id == NULL) return NULL;
    return find_mut(idx, species_id);
}

uint32_t species_index_total(SpeciesIndex* idx) {
    if(idx == NULL) return 0;
    uint32_t total = 0;
    for(uint32_t i = 0; i < idx->count; i++) total += idx->records[i].count;
    return total;
}

uint32_t species_index_count(SpeciesIndex* idx) {
    return (idx != NULL) ? idx->count : 0;
}

const SpeciesRecord* species_index_get(SpeciesIndex* idx, uint32_t i) {
    if(idx == NULL || i >= idx->count) return NULL;
    return &idx->records[i];
}

bool species_index_bump(SpeciesIndex* idx, const char* species_id, uint64_t now) {
    if(idx == NULL || species_id == NULL) return false;
    SpeciesRecord* rec = find_mut(idx, species_id);
    if(rec != NULL) {
        rec->count++;
        rec->last_seen = now;
        return true;
    }
    if(idx->count >= SPECIES_INDEX_MAX) {
        FURI_LOG_W(TAG, "species index full; count not bumped for %s", species_id);
        return false;
    }
    rec = &idx->records[idx->count++];
    strncpy(rec->species_id, species_id, RADIOTCHI_SPECIES_LEN - 1);
    rec->species_id[RADIOTCHI_SPECIES_LEN - 1] = '\0';
    rec->count = 1;
    rec->first_seen = now;
    rec->last_seen = now;
    return true;
}

// --- persistence -----------------------------------------------------------

void species_index_clear(SpeciesIndex* idx) {
    if(idx != NULL) idx->count = 0;
}

bool species_index_load(SpeciesIndex* idx) {
    if(idx == NULL) return false;
    idx->count = 0;
    if(!storage_file_exists(idx->storage, RADIOTCHI_SPECIES_PATH)) return false;

    File* f = storage_file_alloc(idx->storage);
    bool ok = false;
    if(storage_file_open(f, RADIOTCHI_SPECIES_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        // Read the whole (small) file and split into lines.
        FuriString* content = furi_string_alloc();
        char chunk[128];
        size_t n;
        while((n = storage_file_read(f, chunk, sizeof(chunk))) > 0) {
            for(size_t i = 0; i < n; i++) furi_string_push_back(content, chunk[i]);
        }

        FuriString* line = furi_string_alloc();
        size_t len = furi_string_size(content);
        bool header_skipped = false;
        for(size_t i = 0; i <= len; i++) {
            char c = (i < len) ? furi_string_get_char(content, i) : '\n';
            if(c == '\n') {
                if(furi_string_size(line) > 0) {
                    if(!header_skipped) {
                        header_skipped = true; // first line is the column header
                    } else if(idx->count < SPECIES_INDEX_MAX) {
                        SpeciesRecord* r = &idx->records[idx->count];
                        char id[RADIOTCHI_SPECIES_LEN] = {0};
                        unsigned long cnt = 0;
                        unsigned long long first = 0, last = 0;
                        // species_id,count,first_seen,last_seen
                        if(sscanf(
                               furi_string_get_cstr(line),
                               "%31[^,],%lu,%llu,%llu",
                               id,
                               &cnt,
                               &first,
                               &last) == 4) {
                            strncpy(r->species_id, id, RADIOTCHI_SPECIES_LEN - 1);
                            r->species_id[RADIOTCHI_SPECIES_LEN - 1] = '\0';
                            r->count = (uint32_t)cnt;
                            r->first_seen = (uint64_t)first;
                            r->last_seen = (uint64_t)last;
                            idx->count++;
                        }
                    }
                }
                furi_string_reset(line);
            } else if(c != '\r') {
                furi_string_push_back(line, c);
            }
        }
        furi_string_free(line);
        furi_string_free(content);
        ok = true;
    }
    storage_file_close(f);
    storage_file_free(f);
    return ok;
}

bool species_index_save(SpeciesIndex* idx) {
    if(idx == NULL) return false;
    File* f = storage_file_alloc(idx->storage);
    bool ok = false;
    if(storage_file_open(f, RADIOTCHI_SPECIES_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        FuriString* s = furi_string_alloc();
        furi_string_set(s, "species_id,count,first_seen,last_seen\n");
        storage_file_write(f, furi_string_get_cstr(s), furi_string_size(s));
        for(uint32_t i = 0; i < idx->count; i++) {
            const SpeciesRecord* r = &idx->records[i];
            furi_string_printf(
                s,
                "%s,%lu,%llu,%llu\n",
                r->species_id,
                (unsigned long)r->count,
                (unsigned long long)r->first_seen,
                (unsigned long long)r->last_seen);
            storage_file_write(f, furi_string_get_cstr(s), furi_string_size(s));
        }
        furi_string_free(s);
        ok = true;
    }
    storage_file_close(f);
    storage_file_free(f);
    return ok;
}
