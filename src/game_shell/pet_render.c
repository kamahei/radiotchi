// Radiotchi - pet sprite rendering (monochrome).
//
// The growth pet (pet_render_draw_growth) is a life-stage machine that draws the 100-type
// morph as monolithic image-generated creatures (egg -> lineage-tinted child -> adult
// keyed to family x shape; decision-log D22). The legacy Health x Class quadrant renderer
// was retired once the morph (D18) superseded the 5-form taxonomy.

#include "pet_render.h"

#include "pet_sprites.h"

// Draw the growth-layer pet: a life-stage machine (egg -> lineage-tinted child -> adult
// morph) drawing one monolithic 64x64 creature per (family, shape) from pet_sprites.c.
void pet_render_draw_growth(Canvas* canvas, const PetGrowth* g, int cx, int cy, uint32_t frame) {
    if(canvas == NULL || g == NULL) return;

    canvas_set_color(canvas, ColorBlack);

    // `r` is vestigial (every sprite is a fixed 64x64 cell), kept only for the API
    // symmetry of pet_sprite_*; the idle motion now lives in the art's 4-frame sway,
    // cycled inside the sprite lookups by `frame & 3`.
    int r = 0;

    PetLifeStage stage = pet_life_stage(g->level);

    // Egg, or no morph derived yet: one shared egg.
    if(stage == LIFE_EGG || g->type_id == PET_TYPE_UNFORMED || g->type_id >= PET_TYPE_COUNT) {
        pet_sprite_egg(canvas, (uint8_t)frame, cx, cy, r);
        canvas_set_color(canvas, ColorBlack);
        return;
    }

    uint8_t family = pet_type_family(g->type_id);

    // Child: a lineage-tinted baby before the full morph (foreshadows the family).
    if(stage == LIFE_CHILD) {
        pet_sprite_child(canvas, family, (uint8_t)frame, cx, cy, r);
        canvas_set_color(canvas, ColorBlack);
        return;
    }

    // Adult: one monolithic creature keyed to (family, shape). The 2nd-stat partner
    // is shown as text (Pet Detail), not on the silhouette; mood is baked into the art.
    uint8_t shape = pet_type_shape(g->type_id);
    pet_sprite_character(canvas, family, shape, (uint8_t)frame, cx, cy, r);

    canvas_set_color(canvas, ColorBlack); // restore default for later widgets
}
