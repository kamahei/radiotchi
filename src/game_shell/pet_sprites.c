// Radiotchi - monolithic sprite roster (real 1-bit Codex art). See pet_sprites.h.
//
// One coherent 64x64 creature per (family, shape) drawn by reference with
// canvas_draw_icon (D22; docs/codex-sprite-brief.md) - nothing large rides the GUI
// draw stack. Every creature carries a 4-frame idle sway (suffix _0.._3: rest, pose A,
// rest, pose B); `frame & 3` cycles them. Art is ingested by tools/convert_art.py into
// icons/ and compiled in by fbt (icons/<name>.png -> radiotchi_icons.h as I_<name>).

#include "pet_sprites.h"

#include <radiotchi_icons.h>

#define PET_CELL  64 // shared authoring grid; every sprite is a full 64x64 frame
#define PET_POSES 4  // idle sway loop: rest, pose A, rest, pose B - cycled by frame&3

// --- sprite lookup tables (the icon symbol I_<name> == the PNG file stem) -------
// CHAR_ICON[family][shape][pose]; shape order matches PetShape
// (Pure, Sprout, Crested, Woven, Diffuse); family order matches ST_* indices.
static const Icon* const CHAR_ICON[PET_FAMILY_COUNT][PET_SHAPE_COUNT][PET_POSES] = {
    [ST_MASS] =
        {{&I_char_mass_pure_0, &I_char_mass_pure_1, &I_char_mass_pure_2, &I_char_mass_pure_3},
         {&I_char_mass_sprout_0,
          &I_char_mass_sprout_1,
          &I_char_mass_sprout_2,
          &I_char_mass_sprout_3},
         {&I_char_mass_crested_0,
          &I_char_mass_crested_1,
          &I_char_mass_crested_2,
          &I_char_mass_crested_3},
         {&I_char_mass_woven_0, &I_char_mass_woven_1, &I_char_mass_woven_2, &I_char_mass_woven_3},
         {&I_char_mass_diffuse_0,
          &I_char_mass_diffuse_1,
          &I_char_mass_diffuse_2,
          &I_char_mass_diffuse_3}},
    [ST_VIGOR] =
        {{&I_char_vigor_pure_0, &I_char_vigor_pure_1, &I_char_vigor_pure_2, &I_char_vigor_pure_3},
         {&I_char_vigor_sprout_0,
          &I_char_vigor_sprout_1,
          &I_char_vigor_sprout_2,
          &I_char_vigor_sprout_3},
         {&I_char_vigor_crested_0,
          &I_char_vigor_crested_1,
          &I_char_vigor_crested_2,
          &I_char_vigor_crested_3},
         {&I_char_vigor_woven_0,
          &I_char_vigor_woven_1,
          &I_char_vigor_woven_2,
          &I_char_vigor_woven_3},
         {&I_char_vigor_diffuse_0,
          &I_char_vigor_diffuse_1,
          &I_char_vigor_diffuse_2,
          &I_char_vigor_diffuse_3}},
    [ST_WILD] =
        {{&I_char_wild_pure_0, &I_char_wild_pure_1, &I_char_wild_pure_2, &I_char_wild_pure_3},
         {&I_char_wild_sprout_0,
          &I_char_wild_sprout_1,
          &I_char_wild_sprout_2,
          &I_char_wild_sprout_3},
         {&I_char_wild_crested_0,
          &I_char_wild_crested_1,
          &I_char_wild_crested_2,
          &I_char_wild_crested_3},
         {&I_char_wild_woven_0, &I_char_wild_woven_1, &I_char_wild_woven_2, &I_char_wild_woven_3},
         {&I_char_wild_diffuse_0,
          &I_char_wild_diffuse_1,
          &I_char_wild_diffuse_2,
          &I_char_wild_diffuse_3}},
    [ST_AURA] =
        {{&I_char_aura_pure_0, &I_char_aura_pure_1, &I_char_aura_pure_2, &I_char_aura_pure_3},
         {&I_char_aura_sprout_0,
          &I_char_aura_sprout_1,
          &I_char_aura_sprout_2,
          &I_char_aura_sprout_3},
         {&I_char_aura_crested_0,
          &I_char_aura_crested_1,
          &I_char_aura_crested_2,
          &I_char_aura_crested_3},
         {&I_char_aura_woven_0, &I_char_aura_woven_1, &I_char_aura_woven_2, &I_char_aura_woven_3},
         {&I_char_aura_diffuse_0,
          &I_char_aura_diffuse_1,
          &I_char_aura_diffuse_2,
          &I_char_aura_diffuse_3}},
    [ST_MIND] =
        {{&I_char_mind_pure_0, &I_char_mind_pure_1, &I_char_mind_pure_2, &I_char_mind_pure_3},
         {&I_char_mind_sprout_0,
          &I_char_mind_sprout_1,
          &I_char_mind_sprout_2,
          &I_char_mind_sprout_3},
         {&I_char_mind_crested_0,
          &I_char_mind_crested_1,
          &I_char_mind_crested_2,
          &I_char_mind_crested_3},
         {&I_char_mind_woven_0, &I_char_mind_woven_1, &I_char_mind_woven_2, &I_char_mind_woven_3},
         {&I_char_mind_diffuse_0,
          &I_char_mind_diffuse_1,
          &I_char_mind_diffuse_2,
          &I_char_mind_diffuse_3}},
};
static const Icon* const CHILD_ICON[PET_FAMILY_COUNT][PET_POSES] = {
    [ST_MASS] = {&I_child_mass_0, &I_child_mass_1, &I_child_mass_2, &I_child_mass_3},
    [ST_VIGOR] = {&I_child_vigor_0, &I_child_vigor_1, &I_child_vigor_2, &I_child_vigor_3},
    [ST_WILD] = {&I_child_wild_0, &I_child_wild_1, &I_child_wild_2, &I_child_wild_3},
    [ST_AURA] = {&I_child_aura_0, &I_child_aura_1, &I_child_aura_2, &I_child_aura_3},
    [ST_MIND] = {&I_child_mind_0, &I_child_mind_1, &I_child_mind_2, &I_child_mind_3},
};
static const Icon* const EGG_ICON[PET_POSES] = {&I_egg_0, &I_egg_1, &I_egg_2, &I_egg_3};

// Draw a full-cell sprite at the shared origin (the 64x64 grid centered on cx,cy),
// by reference - the icon pointer is all that touches the (tiny) GUI draw stack.
//
// Bitmap mode MUST be transparent (alpha=true) or the sprite's white pixels paint the
// background color instead of letting it show through. The canvas default is opaque,
// so set it per draw and restore it afterward.
static void blit(Canvas* canvas, const Icon* icon, int cx, int cy) {
    if(icon == NULL) return;
    canvas_set_color(canvas, ColorBlack); // black ink; white interior stays transparent
    canvas_set_bitmap_mode(canvas, true);
    canvas_draw_icon(canvas, cx - PET_CELL / 2, cy - PET_CELL / 2, icon);
    canvas_set_bitmap_mode(canvas, false);
    canvas_set_color(canvas, ColorBlack); // restore default for the next widget
}

// --- pre-morph stages ---------------------------------------------------------

void pet_sprite_egg(Canvas* canvas, uint8_t frame, int cx, int cy, int r) {
    (void)r;
    if(canvas == NULL) return;
    blit(canvas, EGG_ICON[frame & 3u], cx, cy);
}

void pet_sprite_child(Canvas* canvas, uint8_t family, uint8_t frame, int cx, int cy, int r) {
    (void)r;
    if(canvas == NULL || family >= PET_FAMILY_COUNT) return;
    blit(canvas, CHILD_ICON[family][frame & 3u], cx, cy);
}

// --- adult morph --------------------------------------------------------------

void pet_sprite_character(
    Canvas* canvas,
    uint8_t family,
    uint8_t shape,
    uint8_t frame,
    int cx,
    int cy,
    int r) {
    (void)r;
    if(canvas == NULL || family >= PET_FAMILY_COUNT || shape >= PET_SHAPE_COUNT) return;
    blit(canvas, CHAR_ICON[family][shape][frame & 3u], cx, cy);
}
