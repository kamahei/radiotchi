// Radiotchi — Capture Source abstraction (the swappable seam).
//
// A CaptureSource produces a RawCapture. It is the ONLY layer that knows about
// hardware or file formats (docs/architecture.md §1). The live adapter drives the
// CC1101; future test adapters can replay `.sub`/rtl_433 fixtures behind the same
// interface so the rest of the stack is identical on-device and on-host.

#pragma once

#include <stdbool.h>

#include "radiotchi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Fill `*out` with the next captured signal. Returns true if a signal was
// captured, false if nothing met the detection threshold this round.
typedef bool (*CaptureSourceNext)(void* impl, RawCapture* out);

typedef struct {
    void* impl; // concrete source (e.g. SubGhzCaptureSource*)
    CaptureSourceNext next;
} CaptureSource;

// Convenience: invoke the source's next().
static inline bool capture_source_next(CaptureSource* src, RawCapture* out) {
    if(src == NULL || src->next == NULL || out == NULL) return false;
    return src->next(src->impl, out);
}

#ifdef __cplusplus
}
#endif
