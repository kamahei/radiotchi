// Radiotchi - pet sprite rendering (monochrome).
//
// Draws the 100-type morph as a life-stage machine; the art is image-generated 1-bit
// creatures composed in pet_sprites.c (D22). Allocation-free draw.

#pragma once

#include <gui/gui.h>

#include "pet_growth.h"

#ifdef __cplusplus
extern "C" {
#endif

// Draw the growth-layer pet centered at (cx, cy). A life-stage machine: a shared egg
// (level < PET_LEVEL_CHILD), a lineage-tinted child (< PET_LEVEL_ADULT), then the
// adult morph as ONE monolithic 64x64 creature keyed to (family, shape) - the 2nd-stat
// partner is shown as text, not on the silhouette (see pet_sprites.h /
// docs/codex-sprite-brief.md). `frame & 3` cycles the 4-frame idle sway.
// Allocation-free; the sprite is drawn by reference (D22).
void pet_render_draw_growth(Canvas* canvas, const PetGrowth* g, int cx, int cy, uint32_t frame);

#ifdef __cplusplus
}
#endif
