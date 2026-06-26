// Radiotchi — host unit tests for the dex label mapper (radiotchi_species_label).
//
// Pure string mapping (species_id -> friendly display name), so it is host-testable like the
// analysis core. Build & run:  make -C test

#include "radiotchi_labels.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, msg)                                             \
    do {                                                             \
        g_checks++;                                                  \
        if(!(cond)) {                                                \
            g_failures++;                                            \
            printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
        }                                                            \
    } while(0)

// Render into a buffer the size of a real species_id (the full label).
static const char* lbl(const char* id) {
    static char b[32];
    radiotchi_species_label(id, b, sizeof(b));
    return b;
}

int main(void) {
    printf("== Radiotchi label host tests ==\n");
    printf("species labels:\n");

    // Named families -> friendly name + band.
    CHECK(strcmp(lbl("weather-acurite-433"), "Acurite 433") == 0, "acurite -> Acurite 433");
    CHECK(strcmp(lbl("th-nexus-433"), "Nexus 433") == 0, "nexus -> Nexus 433");

    // Generic structural sensors -> a short, readable type + band.
    CHECK(strcmp(lbl("sensor-2dd4-4B-c31-868"), "FSK sns 868") == 0, "2dd4 -> FSK sns 868");
    CHECK(strcmp(lbl("sensor-manch-5B-c07-433"), "Manch sns 433") == 0, "manch -> Manch sns 433");
    CHECK(strcmp(lbl("sensor-ook-5B-c07-433"), "OOK sns 433") == 0, "ook sensor -> OOK sns 433");
    CHECK(strcmp(lbl("fsk-sensor-868"), "FSK sns 868") == 0, "fsk family -> FSK sns 868");
    CHECK(strcmp(lbl("ook-fixed-433"), "Fixed 433") == 0, "ook-fixed -> Fixed 433");

    // Provisional fingerprint buckets -> "<mod>? <band>".
    CHECK(strcmp(lbl("F433-M1-L0256"), "OOK? 433") == 0, "fingerprint OOK -> OOK? 433");
    CHECK(strcmp(lbl("F868-M2-L0064"), "2FSK? 868") == 0, "fingerprint 2FSK -> 2FSK? 868");

    // Firmware protocol names stay verbatim; branded families drop the class word to fit the row.
    CHECK(strcmp(lbl("Princeton"), "Princeton") == 0, "firmware name kept verbatim");
    CHECK(strcmp(lbl("keyfob-starline-433"), "starline-433") == 0, "keyfob brand -> make + band");
    CHECK(strcmp(lbl("gate-came-433"), "came-433") == 0, "gate brand -> make + band");

    // Empty / NULL are safe.
    char b[18];
    radiotchi_species_label("", b, sizeof(b));
    CHECK(b[0] == '\0', "empty id => empty label");
    radiotchi_species_label(NULL, b, sizeof(b));
    CHECK(b[0] == '\0', "NULL id => empty label");

    // The label always fits the dex-row buffer.
    CHECK(strlen(lbl("sensor-manch-5B-c07-433")) < 18, "label fits the dex row");

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
