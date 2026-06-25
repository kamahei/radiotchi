// Radiotchi - FAP entry point (MVP game loop).
//
// Home shows the pet (an EMA of its RF diet). OK = Feed: hop the frequency plan
// (NOT fixed to 433.92), lock the single strongest signal, capture RAW, score the
// 5-axis nutrition label, then preview "what it was" + the label. OK again = Eat:
// persist the capture losslessly (.sub + analysis row), bump the species index
// (making Rarity personal), and grow the pet (5-stat EMA + 100-type morph, persisted
// to growth.txt). Right = Dex browser; Left = Settings (live RSSI tuning). RX-only.

#include <furi.h>
#include <gui/gui.h>
#include <input/input.h>

#include <stdlib.h>
#include <string.h>

#include "analysis_core.h" // private lib (on FAP include path)
#include "capture_store.h"
#include "pet_growth.h"
#include "pet_mood.h"
#include "pet_render.h"
#include "species_index.h"
#include "radiotchi_labels.h"
// Cross-directory includes resolve from the app root.
#include "src/capture_source/frequency_plan.h"
#include "src/capture_source/subghz_capture_source.h"

#define TAG "Radiotchi"

// Tuning bounds / steps (Settings screen).
#define THRESHOLD_MIN  -110
#define THRESHOLD_MAX  -30
#define THRESHOLD_STEP 1
#define MARGIN_MIN     0
#define MARGIN_MAX     40
#define MARGIN_STEP    1

// Dex capture-list view: newest N rows held in RAM.
#define DEX_CAPTURES_MAX 32u
#define LIST_VISIBLE_ROWS  4u

// Diff-learning view: how many of a species' captures to align/compare at once.
#define DEX_DIFF_MAX 8u

// Home command menu (Tamagotchi-style; raised by any button, see ui-spec.md §2).
// A vertical panel on the RIGHT of the wide screen; Up/Down select, scrolls when
// the item count exceeds the visible rows.
// The "Debug" sprite gallery is a dev-only tool, compiled in only when RADIOTCHI_DEBUG is
// defined (see application.fam). Release builds ship a 5-item menu without it.
#ifdef RADIOTCHI_DEBUG
#define MENU_COUNT 6u
enum { MENU_DETAIL = 0, MENU_FEED, MENU_DEX, MENU_REGRADE, MENU_TUNE, MENU_DEBUG };
static const char* const MENU_LABELS[MENU_COUNT] =
    {"Detail", "Feed", "Dex", "Re-grade", "Tune", "Debug"};

// Debug character gallery: egg + one child per family + every 100-type adult morph.
#define DEBUG_GALLERY_COUNT (1u + PET_FAMILY_COUNT + PET_TYPE_COUNT) // 106
#define DEBUG_GALLERY_STEP  10u // Up/Down fast-jump stride
#else
#define MENU_COUNT 5u
enum { MENU_DETAIL = 0, MENU_FEED, MENU_DEX, MENU_REGRADE, MENU_TUNE };
static const char* const MENU_LABELS[MENU_COUNT] = {"Detail", "Feed", "Dex", "Re-grade", "Tune"};
#endif

#define MENU_PANEL_X      76 // left edge of the menu panel (pet sits to its left)
#define MENU_ROW_H        12
#define MENU_VISIBLE_ROWS 5u

// Name editor charset (cycled per cursor cell).
#define NAME_CHARSET " ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"

// Idle animation tick.
#define ANIM_PERIOD_MS 400u

// Eat-celebration animation: bars/exp tween, then a level-up / evolve / delicacy flash.
enum { EAT_PHASE_BARS = 0, EAT_PHASE_FLASH };
#define EAT_BARS_FRAMES  20
#define EAT_BARS_MS      35u
#define EAT_FLASH_FRAMES 10
#define EAT_FLASH_MS     90u

// What this meal triggered (bitmask; drives the flash banner + procedural effects).
#define EAT_EVT_LEVEL  0x1u // crossed a level boundary
#define EAT_EVT_EVOLVE 0x2u // the 100-type morph changed (checkpoint re-speciation)
#define EAT_EVT_RARE   0x4u // a novel / delicacy catch
#define EAT_RARE_SCORE 0.8f // rarity at/above this counts as a delicacy

typedef enum {
    ScreenHome,
    ScreenSettings,
    ScreenFeedScan,
    ScreenFeedReadout,
    ScreenFeedLabel,
    ScreenNoSignal,
    ScreenDexSpecies,
    ScreenDexCaptures,
    ScreenCaptureDetail,
    ScreenDexDiff,
    ScreenPetDetail,
    ScreenNameEdit,
    ScreenEatAnim,
#ifdef RADIOTCHI_DEBUG
    ScreenDebugGallery,
#endif
    ScreenRegradeDone,
} Screen;

// One row in the per-species capture list (parsed from the capture log).
typedef struct {
    char datetime[20]; // "YYYY-MM-DDThh:mm:ss"
    uint32_t freq;
    int16_t rssi;
    uint16_t bits;
    int16_t entropy_centi;
    uint8_t tier;
    char modulation[8];
    char individual[RADIOTCHI_INDIVIDUAL_LEN]; // privacy-safe per-device tag ("" if none)
} DexCaptureRow;

typedef struct {
    Screen screen;

    // pet + dex counts
    PetGrowth growth; // 5 stats + EXP/level/100-type morph (drives display + persistence)
    PetCare care; // last-feed time + meal quality (drives hunger/mood; persisted)
    char name[RADIOTCHI_PET_NAME_CAP];
    uint32_t dex_count;
    int regrade_result; // rows changed by the last dex re-grade (-1 = error)

    // Home command menu + idle animation
    bool menu_open;
    uint32_t menu_sel;
    uint32_t menu_scroll;
    uint32_t anim_frame;
    uint32_t now_cache; // RTC seconds, refreshed on the anim tick (mood off the draw path)

#ifdef RADIOTCHI_DEBUG
    // debug character gallery (browse every sprite)
    uint16_t dbg_index; // 0..DEBUG_GALLERY_COUNT-1
    bool dbg_tired; // adult eye overlay: lively vs tired
#endif

    // name editor scratch
    char name_edit[RADIOTCHI_PET_NAME_CAP];
    uint8_t name_cursor;

    // eat-celebration animation (tween from eat_before -> growth)
    PetGrowth eat_before;
    float eat_t; // 0..1 tween progress
    uint8_t eat_phase; // EAT_PHASE_BARS | EAT_PHASE_FLASH
    uint8_t eat_event; // EAT_EVT_* bitmask for the flash phase

    // detection tuning + last-sweep diagnostics
    int16_t threshold;
    int16_t margin;
    bool have_sweep;
    int16_t floor;
    int16_t best;

    // last captured/scored event (readout, label, detail)
    CaptureEvent ev;

    // dex navigation
    uint32_t species_sel;
    uint32_t species_scroll;
    char browse_species[RADIOTCHI_SPECIES_LEN];
    DexCaptureRow captures[DEX_CAPTURES_MAX];
    uint32_t capture_count;
    uint32_t capture_sel;
    uint32_t capture_scroll;

    // diff-learning view: byte classes across the species' decoded captures
    ByteDiff diff;
    uint8_t diff_count; // frames compared (0/1 => "need >= 2 decoded")
} AppModel;

typedef struct {
    Gui* gui;
    ViewPort* view_port;
    FuriMessageQueue* input_queue;
    FuriMutex* mutex;
    FuriTimer* anim_timer;

    AppModel model;
    FrequencyPlan plan;
    SubGhzCaptureSource* capture;
    CaptureStore* store;
    SpeciesIndex* index;

    // staged feed (between readout/label and eat); main-loop thread only
    const int32_t* pending_pulses;
    size_t pending_pulse_count;
    FuriHalSubGhzPreset pending_preset;
    uint32_t pending_seen_count;
} RadiotchiApp;

// ========================= drawing =========================================

static void draw_scroll_list(
    Canvas* canvas,
    uint32_t count,
    uint32_t sel,
    uint32_t scroll_top,
    void (*row_text)(void* ctx, uint32_t i, char* out, size_t out_len),
    void* ctx) {
    canvas_set_font(canvas, FontSecondary);
    if(count == 0) {
        canvas_draw_str(canvas, 4, 30, "(empty)");
        return;
    }
    char row[40];
    for(uint32_t r = 0; r < LIST_VISIBLE_ROWS; r++) {
        uint32_t i = scroll_top + r;
        if(i >= count) break;
        int y = 22 + (int)r * 11;
        if(i == sel) {
            canvas_draw_box(canvas, 0, y - 9, 128, 11);
            canvas_set_color(canvas, ColorWhite);
        }
        row[0] = '\0';
        row_text(ctx, i, row, sizeof(row));
        canvas_draw_str(canvas, 3, y, row);
        if(i == sel) canvas_set_color(canvas, ColorBlack);
    }
}

// One labeled nutrition bar; show=false renders "-" (graceful degradation).
static void draw_axis_bar(Canvas* canvas, int y, const char* label, float score, bool show) {
    canvas_draw_str(canvas, 2, y, label);
    if(!show) {
        canvas_draw_str(canvas, 40, y, "-");
        return;
    }
    int x = 40, w = 80;
    canvas_draw_frame(canvas, x, y - 6, w, 6);
    int fill = (int)(score * (float)(w - 2) + 0.5f);
    if(fill > w - 2) fill = w - 2;
    if(fill > 0) canvas_draw_box(canvas, x + 1, y - 5, fill, 4);
}

static void species_row_text(void* ctx, uint32_t i, char* out, size_t out_len) {
    RadiotchiApp* app = ctx;
    const SpeciesRecord* r = species_index_get(app->index, i);
    if(r == NULL) return;
    char id[18];
    strncpy(id, r->species_id, sizeof(id) - 1);
    id[sizeof(id) - 1] = '\0';
    snprintf(out, out_len, "%s%s x%lu", (r->count <= 1 ? "*" : " "), id, (unsigned long)r->count);
}

static void captures_row_text(void* ctx, uint32_t i, char* out, size_t out_len) {
    RadiotchiApp* app = ctx;
    if(i >= app->model.capture_count) return;
    // Date + time, plus the privacy-safe device tag when present so a recurring device is
    // scannable down the list (the longitudinal "same device over time" view, TB.2).
    const DexCaptureRow* r = &app->model.captures[i];
    const char* dt = r->datetime;
    if(r->individual[0]) {
        snprintf(out, out_len, "%.5s %.5s %s", dt + 5, dt + 11, r->individual); // MM-DD hh:mm id-xxxx
    } else {
        snprintf(out, out_len, "%.10s %.8s", dt, dt + 11);
    }
}

// Home at rest: just the living pet, idle-animating (ui-spec.md §1). When the
// menu is open the pet shifts left to make room for the right-side panel.
static void draw_home(Canvas* canvas, const AppModel* m) {
    int cx = m->menu_open ? 38 : 64;
    PetMood mood = pet_mood(&m->care, m->now_cache);
    pet_render_draw_growth_mood(
        canvas, &m->growth, mood, cx, 30, pet_mood_anim_frame(mood, m->anim_frame));
    if(!m->menu_open) {
        // A faint hint that a button opens the menu; no chrome otherwise.
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 30, 63, "[press a key]");
    }
}

// Command menu: a vertical panel down the right side; pet visible to its left.
// Up/Down move the selection; the window scrolls when items exceed the rows.
static void draw_menu_panel(Canvas* canvas, const AppModel* m) {
    const int x0 = MENU_PANEL_X;
    const int w = 128 - x0;
    canvas_set_color(canvas, ColorWhite);
    canvas_draw_box(canvas, x0, 0, w, 64);
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_line(canvas, x0, 0, x0, 63);
    canvas_set_font(canvas, FontSecondary);

    uint32_t scroll = m->menu_scroll;
    for(uint32_t r = 0; r < MENU_VISIBLE_ROWS; r++) {
        uint32_t i = scroll + r;
        if(i >= MENU_COUNT) break;
        int y = 1 + (int)r * MENU_ROW_H;
        if(i == m->menu_sel) {
            canvas_draw_box(canvas, x0 + 2, y, w - 4, MENU_ROW_H - 1);
            canvas_set_color(canvas, ColorWhite);
        }
        canvas_draw_str(canvas, x0 + 5, y + MENU_ROW_H - 3, MENU_LABELS[i]);
        if(i == m->menu_sel) canvas_set_color(canvas, ColorBlack);
    }
    // Scroll affordances when there is more above/below the window.
    if(scroll > 0) canvas_draw_str(canvas, 122, 7, "^");
    if(scroll + MENU_VISIBLE_ROWS < MENU_COUNT) canvas_draw_str(canvas, 122, 63, "v");
}

// Labeled stat bar + a 0..100 numeric readout (numbers make growth legible).
static void draw_stat_bar_xy(
    Canvas* canvas, int lx, int y, int barx, int barw, const char* label, float v) {
    canvas_draw_str(canvas, lx, y, label);
    canvas_draw_frame(canvas, barx, y - 5, barw, 5);
    int inner = barw - 2;
    int fill = (int)(v * (float)inner + 0.5f);
    if(fill < 0) fill = 0;
    if(fill > inner) fill = inner;
    if(fill > 0) canvas_draw_box(canvas, barx + 1, y - 4, fill, 3);
    int val = (int)(v * 100.0f + 0.5f);
    if(val < 0) val = 0;
    if(val > 100) val = 100;
    char num[5];
    snprintf(num, sizeof(num), "%d", val);
    canvas_draw_str(canvas, barx + barw + 3, y, num);
}

static void draw_pet_detail(Canvas* canvas, const AppModel* m) {
    const PetGrowth* g = &m->growth;
    char line[40], type_name[24];
    pet_type_name(g->type_id, type_name, sizeof(type_name));

    canvas_set_font(canvas, FontPrimary);
    snprintf(line, sizeof(line), "%s", (m->name[0] ? m->name : "(unnamed)"));
    canvas_draw_str(canvas, 2, 10, line);

    canvas_set_font(canvas, FontSecondary);
    uint32_t cur = (uint32_t)PET_EXP_K * (uint32_t)g->level * (uint32_t)g->level;
    uint32_t nxt = (uint32_t)PET_EXP_K * (uint32_t)(g->level + 1u) * (uint32_t)(g->level + 1u);
    snprintf(
        line,
        sizeof(line),
        "Lv%u  exp %lu/%lu",
        (unsigned)g->level,
        (unsigned long)(g->total_exp > cur ? g->total_exp - cur : 0),
        (unsigned long)(nxt - cur));
    canvas_draw_str(canvas, 2, 19, line);
    snprintf(line, sizeof(line), "%s", type_name);
    canvas_draw_str(canvas, 2, 28, line);

    // All five parameters with numeric values (MAS..MIND order).
    draw_stat_bar_xy(canvas, 2, 36, 30, 64, "MAS", g->stat[ST_MASS]);
    draw_stat_bar_xy(canvas, 2, 43, 30, 64, "VIG", g->stat[ST_VIGOR]);
    draw_stat_bar_xy(canvas, 2, 50, 30, 64, "WLD", g->stat[ST_WILD]);
    draw_stat_bar_xy(canvas, 2, 57, 30, 64, "AUR", g->stat[ST_AURA]);
    draw_stat_bar_xy(canvas, 2, 64, 30, 64, "MND", g->stat[ST_MIND]);
}

static void draw_name_edit(Canvas* canvas, const AppModel* m) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 11, "Name");
    canvas_set_font(canvas, FontSecondary);
    // Draw the editable name with a caret under the active cell.
    int x0 = 6, cw = 7, y = 34;
    for(uint32_t i = 0; i + 1 < RADIOTCHI_PET_NAME_CAP; i++) {
        char c = m->name_edit[i];
        char s[2] = {(c ? c : '_'), '\0'};
        int x = x0 + (int)i * cw;
        canvas_draw_str(canvas, x, y, s);
        if(i == m->name_cursor) canvas_draw_line(canvas, x, y + 2, x + cw - 2, y + 2);
    }
    canvas_draw_str(canvas, 2, 52, "U/D:char L/R:move");
    canvas_draw_str(canvas, 2, 62, "OK:save  Back:cancel");
}

static void draw_settings(Canvas* canvas, const AppModel* m) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 11, "Tuning");
    canvas_set_font(canvas, FontSecondary);
    char line[40];
    snprintf(line, sizeof(line), "Thr %ddBm  Mgn %ddB", (int)m->threshold, (int)m->margin);
    canvas_draw_str(canvas, 2, 26, line);
    if(m->have_sweep) {
        snprintf(line, sizeof(line), "floor %d  best %d", (int)m->floor, (int)m->best);
        canvas_draw_str(canvas, 2, 38, line);
    }
    canvas_draw_str(canvas, 2, 52, "Up/Dn:thr  L/R:mgn");
    canvas_draw_str(canvas, 2, 62, "Back:home");
}

static void draw_readout(Canvas* canvas, const AppModel* m) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 11, "What it was");
    canvas_set_font(canvas, FontSecondary);
    char line[40];
    unsigned long mhz = (unsigned long)(m->ev.frequency_hz / 1000000u);
    unsigned long khz = (unsigned long)((m->ev.frequency_hz / 1000u) % 1000u);
    snprintf(line, sizeof(line), "%lu.%03lu MHz", mhz, khz);
    canvas_draw_str(canvas, 2, 25, line);
    snprintf(line, sizeof(line), "%s  %ddBm", radiotchi_modulation_str(m->ev.modulation), (int)m->ev.rssi_dbm);
    canvas_draw_str(canvas, 2, 37, line);
    snprintf(line, sizeof(line), "Proto: %s", (m->ev.protocol[0] ? m->ev.protocol : "Unknown"));
    canvas_draw_str(canvas, 2, 49, line);
    canvas_draw_str(canvas, 2, 62, "OK:label  Back:drop");
}

static void draw_label(Canvas* canvas, const AppModel* m) {
    canvas_set_font(canvas, FontSecondary);
    const Scores* s = &m->ev.scores;
    bool nourish = (m->ev.decode_tier != TIER_RAW);
    draw_axis_bar(canvas, 9, "Cal", s->calories, true);
    draw_axis_bar(canvas, 19, "Fre", s->freshness, true);
    draw_axis_bar(canvas, 29, "Add", s->additives, true);
    draw_axis_bar(canvas, 39, "Rar", s->rarity, true);
    draw_axis_bar(canvas, 49, "Nou", s->nourishment, nourish);
    canvas_draw_str(canvas, 2, 62, "OK:Eat  Back:drop");
}

static void draw_detail(Canvas* canvas, const AppModel* m) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 11, "Capture");
    canvas_set_font(canvas, FontSecondary);
    if(m->capture_sel >= m->capture_count) {
        canvas_draw_str(canvas, 2, 30, "(no data)");
        return;
    }
    const DexCaptureRow* r = &m->captures[m->capture_sel];
    char line[40];
    canvas_draw_str(canvas, 2, 22, r->datetime);
    unsigned long mhz = (unsigned long)(r->freq / 1000000u);
    unsigned long khz = (unsigned long)((r->freq / 1000u) % 1000u);
    snprintf(line, sizeof(line), "%lu.%03lu %s %ddBm", mhz, khz, r->modulation, (int)r->rssi);
    canvas_draw_str(canvas, 2, 33, line);
    snprintf(
        line,
        sizeof(line),
        "ent %d.%02d  bits %u",
        r->entropy_centi / 100,
        r->entropy_centi % 100,
        (unsigned)r->bits);
    canvas_draw_str(canvas, 2, 44, line);
    if(r->individual[0]) {
        // Privacy-safe per-device tag: lets you spot the SAME device recurring over time.
        snprintf(line, sizeof(line), "tier %s  %s", radiotchi_tier_str((DecodeTier)r->tier), r->individual);
    } else {
        snprintf(line, sizeof(line), "tier %s  (Back)", radiotchi_tier_str((DecodeTier)r->tier));
    }
    canvas_draw_str(canvas, 2, 55, line);
}

// Diff-learning view: one class glyph per byte position across this species' decoded captures.
// S=static (an id/fixed field), I=incrementing (a rolling counter), V=varying (a sensor value),
// -=absent. Privacy (A5): only the CLASS is drawn, never the raw byte value.
static void draw_diff(Canvas* canvas, const AppModel* m) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 11, "Learning");
    canvas_set_font(canvas, FontSecondary);

    char head[28];
    snprintf(head, sizeof(head), "%.15s x%u", m->browse_species, (unsigned)m->diff_count);
    canvas_draw_str(canvas, 2, 21, head);

    if(m->diff_count < 2 || m->diff.width == 0) {
        canvas_draw_str(canvas, 2, 36, "Need >=2 decoded");
        canvas_draw_str(canvas, 2, 46, "captures to compare");
        canvas_draw_str(canvas, 2, 62, "Back: captures");
        return;
    }

    const int per_row = 16;
    for(uint16_t p = 0; p < m->diff.width && p < RADIOTCHI_DIFF_BYTES_MAX; p++) {
        int x = 4 + (int)(p % per_row) * 7;
        int y = 33 + (int)(p / per_row) * 9;
        char g;
        switch(m->diff.cls[p]) {
        case BYTE_STATIC: g = 'S'; break;
        case BYTE_INCREMENTING: g = 'I'; break;
        case BYTE_VARYING: g = 'V'; break;
        default: g = '-'; break;
        }
        char s[2] = {g, '\0'};
        canvas_draw_str(canvas, x, y, s);
    }

    canvas_draw_str(canvas, 2, 62, "S:id I:cnt V:val");
}

static float lerpf(float a, float b, float t) {
    return a + (b - a) * t;
}

// N procedural twinkle sparkles on a fixed pseudo-pattern keyed by `frame` (no art assets).
static void draw_sparkles(Canvas* canvas, uint32_t frame, int n) {
    static const uint8_t PX[8][2] = {
        {28, 30}, {100, 28}, {92, 52}, {18, 46}, {110, 40}, {40, 20}, {72, 54}, {54, 24}};
    for(int i = 0; i < n && i < 8; i++) {
        if(((frame + (uint32_t)i) & 1u) == 0) continue; // twinkle on alternating frames
        int px = PX[i][0], py = PX[i][1];
        canvas_draw_dot(canvas, px, py);
        canvas_draw_dot(canvas, px - 1, py);
        canvas_draw_dot(canvas, px + 1, py);
        canvas_draw_dot(canvas, px, py - 1);
        canvas_draw_dot(canvas, px, py + 1);
    }
}

// Eat celebration: bars/exp tween from eat_before -> growth (pet hops), then a
// level-up / evolve flash. Drawn each frame while play_eat_animation() steps the
// tween on the main-loop thread.
static void draw_eat_anim(Canvas* canvas, const AppModel* m) {
    const PetGrowth* a = &m->eat_before;
    const PetGrowth* b = &m->growth;

    if(m->eat_phase == EAT_PHASE_FLASH) {
        uint8_t evt = m->eat_event;
        bool on = (m->anim_frame & 1u) != 0;

        // EVOLVE shakes the pet ±2px; lesser events just hop.
        int sx = 0, sy = on ? -3 : 0;
        if(evt & EAT_EVT_EVOLVE) {
            static const int8_t SH[4][2] = {{-2, 0}, {2, -1}, {-1, 2}, {2, 1}};
            sx = SH[m->anim_frame & 3u][0];
            sy = SH[m->anim_frame & 3u][1];
        }
        pet_render_draw_growth(canvas, b, 64 + sx, 42 + sy, m->anim_frame);

        // Evolution burst: a ring expanding over the first few frames.
        if((evt & EAT_EVT_EVOLVE) && m->anim_frame < 6u) {
            canvas_draw_circle(canvas, 64, 42, 8 + (int)(m->anim_frame * 4u));
        }

        if(on) canvas_draw_box(canvas, 0, 0, 128, 16);
        canvas_set_color(canvas, on ? ColorWhite : ColorBlack);
        canvas_set_font(canvas, FontPrimary);
        const char* banner = (evt & EAT_EVT_EVOLVE) ? "EVOLVED!" :
                             (evt & EAT_EVT_LEVEL)  ? "LEVEL UP!" :
                                                      "DELICACY!";
        canvas_draw_str_aligned(canvas, 64, 8, AlignCenter, AlignCenter, banner);
        canvas_set_color(canvas, ColorBlack);
        if(on) {
            int nspark = (evt & EAT_EVT_EVOLVE) ? 6 : (evt & EAT_EVT_LEVEL) ? 4 : 3;
            draw_sparkles(canvas, m->anim_frame, nspark);
        }
        return;
    }

    // Bars phase. Triangle hop peaks at t=0.5.
    float t = m->eat_t;
    int hop = (t < 0.5f) ? (int)(14.0f * t) : (int)(14.0f * (1.0f - t));
    pet_render_draw_growth(canvas, b, 30, 36 - hop, m->anim_frame);

    float exp_f = lerpf((float)a->total_exp, (float)b->total_exp, t);
    uint32_t exp_now = (uint32_t)(exp_f + 0.5f);
    uint16_t lvl = pet_level_for_exp(exp_now);
    uint32_t cur = (uint32_t)PET_EXP_K * (uint32_t)lvl * (uint32_t)lvl;
    uint32_t nxt = (uint32_t)PET_EXP_K * (uint32_t)(lvl + 1u) * (uint32_t)(lvl + 1u);
    uint32_t gain = (b->total_exp > a->total_exp) ? (b->total_exp - a->total_exp) : 0;

    canvas_set_font(canvas, FontSecondary);
    char line[24];
    snprintf(line, sizeof(line), "Lv%u  +%lu", (unsigned)lvl, (unsigned long)gain);
    canvas_draw_str(canvas, 62, 8, line);
    int ex = 62, ew = 64;
    canvas_draw_frame(canvas, ex, 11, ew, 5);
    uint32_t span = (nxt > cur) ? (nxt - cur) : 1u;
    int efill = (int)(((float)(exp_now - cur) / (float)span) * (float)(ew - 2) + 0.5f);
    if(efill < 0) efill = 0;
    if(efill > ew - 2) efill = ew - 2;
    if(efill > 0) canvas_draw_box(canvas, ex + 1, 12, efill, 3);

    static const char* const L[PET_STAT_COUNT] = {"MAS", "VIG", "WLD", "AUR", "MND"};
    for(int i = 0; i < PET_STAT_COUNT; i++) {
        float v = lerpf(a->stat[i], b->stat[i], t);
        draw_stat_bar_xy(canvas, 62, 26 + i * 8, 84, 22, L[i], v);
    }
}

// ---- debug character gallery (browse every sprite: egg, children, 100 morphs) ----
// Dev-only (RADIOTCHI_DEBUG); compiled out of release builds.
#ifdef RADIOTCHI_DEBUG

// Build a synthetic pet that renders gallery item `index` (0..COUNT-1):
//   0      -> egg
//   1..5   -> child of family (index-1)
//   6..105 -> adult type_id (index-6), 0..99
// Only level (life stage) and type_id matter; the sprite ignores size/`r`. `tired` is
// now inert (mood is baked into the monolithic art) but left wired for future variants.
static PetGrowth debug_gallery_growth(uint16_t index, bool tired) {
    PetGrowth g;
    pet_growth_init(&g);
    if(index == 0u) {
        g.level = 1u; // < CHILD -> egg
        g.type_id = PET_TYPE_UNFORMED;
    } else if(index <= PET_FAMILY_COUNT) {
        uint8_t family = (uint8_t)(index - 1u);
        g.level = PET_LEVEL_CHILD; // child stage; family read from type_id
        g.type_id = (uint8_t)(family * PET_PARTNER_COUNT * PET_SHAPE_COUNT); // family*20
    } else {
        g.level = PET_LEVEL_ADULT; // adult morph
        g.type_id = (uint8_t)(index - 1u - PET_FAMILY_COUNT); // 0..99
    }
    g.stat[ST_VIGOR] = tired ? 0.0f : 1.0f;
    return g;
}

static const char* const DBG_FAMILY[PET_FAMILY_COUNT] = {"Mass", "Vigor", "Wild", "Aura", "Mind"};

static void debug_gallery_label(uint16_t index, char* out, size_t n) {
    if(index == 0u) {
        snprintf(out, n, "Egg");
    } else if(index <= PET_FAMILY_COUNT) {
        snprintf(out, n, "Child: %s", DBG_FAMILY[index - 1u]);
    } else {
        char tn[24];
        pet_type_name((uint8_t)(index - 1u - PET_FAMILY_COUNT), tn, sizeof(tn));
        snprintf(out, n, "%s", tn);
    }
}

// Debug viewer (not in ui-spec): L/R step +-1, U/D jump +-10 through every sprite,
// OK toggles the (now inert) mood flag, Back returns Home. Title/position sit in
// inverted strips so they stay readable over the pet.
static void draw_debug_gallery(Canvas* canvas, const AppModel* m) {
    PetGrowth g = debug_gallery_growth(m->dbg_index, m->dbg_tired);
    // Sit a touch low (cy=37) so the head crest clears the title strip while the body
    // stays above the footer strip (the tall Mass family is the tight case).
    pet_render_draw_growth(canvas, &g, 64, 37, m->anim_frame);

    // Top strip: the character name.
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_box(canvas, 0, 0, 128, 9);
    canvas_set_color(canvas, ColorWhite);
    canvas_set_font(canvas, FontSecondary);
    char label[40];
    debug_gallery_label(m->dbg_index, label, sizeof(label));
    canvas_draw_str(canvas, 2, 7, label);

    // Bottom strip: position, the adult eye state, and the Back hint.
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_box(canvas, 0, 55, 128, 9);
    canvas_set_color(canvas, ColorWhite);
    char pos[12];
    snprintf(pos, sizeof(pos), "%u/%u", (unsigned)(m->dbg_index + 1u), (unsigned)DEBUG_GALLERY_COUNT);
    canvas_draw_str(canvas, 2, 62, pos);
    if(m->dbg_index > PET_FAMILY_COUNT) // adult: OK toggles the eye overlay
        canvas_draw_str(canvas, 50, 62, m->dbg_tired ? "tired" : "lively");
    canvas_draw_str(canvas, 104, 62, "Back");
    canvas_set_color(canvas, ColorBlack);
}

#endif // RADIOTCHI_DEBUG

// Distinct device tags (id-XXXX) among the loaded captures of the current species —
// "how many different devices of this kind have I caught". n is small (<= DEX_CAPTURES_MAX).
static uint32_t count_distinct_individuals(const AppModel* m) {
    uint32_t distinct = 0;
    for(uint32_t i = 0; i < m->capture_count; i++) {
        if(m->captures[i].individual[0] == '\0') continue;
        bool seen = false;
        for(uint32_t j = 0; j < i; j++) {
            if(strcmp(m->captures[j].individual, m->captures[i].individual) == 0) {
                seen = true;
                break;
            }
        }
        if(!seen) distinct++;
    }
    return distinct;
}

static void draw_callback(Canvas* canvas, void* ctx) {
    RadiotchiApp* app = ctx;
    // Draw directly from app->model under the lock. Do NOT copy AppModel onto the
    // GUI thread's (small) stack - it embeds a CaptureEvent + a 32-row capture
    // array (~1.8 KB) and the copy overflowed the render stack (crash).
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    const AppModel* m = &app->model;

    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);

    switch(m->screen) {
    case ScreenHome:
        draw_home(canvas, m);
        if(m->menu_open) draw_menu_panel(canvas, m);
        break;
    case ScreenPetDetail:
        draw_pet_detail(canvas, m);
        break;
    case ScreenNameEdit:
        draw_name_edit(canvas, m);
        break;
    case ScreenEatAnim:
        draw_eat_anim(canvas, m);
        break;
    case ScreenSettings:
        draw_settings(canvas, m);
        break;
    case ScreenFeedScan:
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 2, 11, "Feeding");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 2, 36, "Hopping... searching");
        break;
    case ScreenFeedReadout:
        draw_readout(canvas, m);
        break;
    case ScreenFeedLabel:
        draw_label(canvas, m);
        break;
    case ScreenNoSignal:
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 2, 11, "Nothing to eat");
        canvas_set_font(canvas, FontSecondary);
        {
            char line[40];
            snprintf(line, sizeof(line), "floor %d  best %d", (int)m->floor, (int)m->best);
            canvas_draw_str(canvas, 2, 34, line);
        }
        canvas_draw_str(canvas, 2, 50, "OK:retry  Back:home");
        break;
    case ScreenDexSpecies:
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 2, 11, "Dex");
        draw_scroll_list(canvas, m->dex_count, m->species_sel, m->species_scroll, species_row_text, app);
        break;
    case ScreenDexCaptures: {
        canvas_set_font(canvas, FontPrimary);
        uint32_t devs = count_distinct_individuals(m);
        char title[28];
        if(devs > 0)
            snprintf(title, sizeof(title), "Captures - %lu dev", (unsigned long)devs);
        else
            snprintf(title, sizeof(title), "Captures");
        canvas_draw_str(canvas, 2, 11, title);
        draw_scroll_list(canvas, m->capture_count, m->capture_sel, m->capture_scroll, captures_row_text, app);
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 78, 11, "R:learn"); // Right opens the diff-learning view
        break;
    }
    case ScreenCaptureDetail:
        draw_detail(canvas, m);
        break;
    case ScreenDexDiff:
        draw_diff(canvas, m);
        break;
#ifdef RADIOTCHI_DEBUG
    case ScreenDebugGallery:
        draw_debug_gallery(canvas, m);
        break;
#endif
    case ScreenRegradeDone:
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 2, 11, "Re-grade");
        canvas_set_font(canvas, FontSecondary);
        {
            char line[40];
            if(m->regrade_result < 0) {
                snprintf(line, sizeof(line), "Failed (see log)");
            } else if(m->regrade_result == 0) {
                snprintf(line, sizeof(line), "Up to date - no change");
            } else {
                snprintf(line, sizeof(line), "%d capture(s) re-graded", m->regrade_result);
            }
            canvas_draw_str(canvas, 2, 34, line);
        }
        canvas_draw_str(canvas, 2, 50, "Back: home");
        break;
    }

    furi_mutex_release(app->mutex);
}

static void input_callback(InputEvent* event, void* ctx) {
    RadiotchiApp* app = ctx;
    furi_message_queue_put(app->input_queue, event, FuriWaitForever);
}

// ========================= helpers =========================================

static int16_t clamp16(int v, int lo, int hi) {
    if(v < lo) return (int16_t)lo;
    if(v > hi) return (int16_t)hi;
    return (int16_t)v;
}

static void set_screen(RadiotchiApp* app, Screen s) {
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->model.screen = s;
    if(s == ScreenHome) app->model.menu_open = false; // always rest on the clean pet view
    furi_mutex_release(app->mutex);
    view_port_update(app->view_port);
}

// Idle-animation tick: advance the frame and request a redraw (Home animates).
static void anim_timer_callback(void* ctx) {
    RadiotchiApp* app = ctx;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->model.anim_frame++;
    app->model.now_cache = capture_store_now(); // keep the RTC read off the locked draw path
    furi_mutex_release(app->mutex);
    view_port_update(app->view_port);
}

static void apply_tuning(RadiotchiApp* app) {
    SubGhzCaptureConfig cfg = subghz_capture_source_get_config(app->capture);
    cfg.rssi_threshold_dbm = app->model.threshold;
    cfg.detection_margin_db = app->model.margin;
    subghz_capture_source_set_config(app->capture, cfg);
    CaptureTuning t = {app->model.threshold, app->model.margin};
    capture_store_save_tuning(app->store, &t);
}

// ========================= feed / eat ======================================

static void do_feed(RadiotchiApp* app) {
    set_screen(app, ScreenFeedScan);

    CaptureSource cs = subghz_capture_source_as_source(app->capture);
    RawCapture raw;
    bool got = capture_source_next(&cs, &raw);

    int16_t floor = subghz_capture_source_noise_floor(app->capture);
    int16_t best = subghz_capture_source_last_best_rssi(app->capture);

    if(!got) {
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        app->model.have_sweep = true;
        app->model.floor = floor;
        app->model.best = best;
        app->model.screen = ScreenNoSignal;
        furi_mutex_release(app->mutex);
        view_port_update(app->view_port);
        return;
    }

    // Personal rarity: prior count BEFORE scoring (reuses the same fingerprint
    // analyze_capture computes), and the novelty weighting for the growth EMA.
    char sp[RADIOTCHI_SPECIES_LEN];
    radiotchi_fingerprint_species(raw.frequency_hz, raw.modulation, raw.bit_count, sp, sizeof(sp));
    const SpeciesRecord* rec = species_index_find(app->index, sp);
    app->pending_seen_count = rec ? rec->count : 0;
    RarityView rv = {
        .seen_count = app->pending_seen_count,
        .total_captures = species_index_total(app->index),
    };

    uint32_t ts = capture_store_now();
    CaptureEvent ev = analyze_capture(&raw, &rv, ts);

    // Stage the lossless pulse train + preset for commit at Eat (no rescan until then).
    app->pending_pulses = NULL;
    app->pending_pulse_count = subghz_capture_source_last_pulses(app->capture, &app->pending_pulses);
    app->pending_preset = subghz_capture_source_preset(app->capture);

    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->model.ev = ev;
    app->model.have_sweep = true;
    app->model.floor = floor;
    app->model.best = best;
    app->model.screen = ScreenFeedReadout;
    furi_mutex_release(app->mutex);
    view_port_update(app->view_port);
}

// Play the eat celebration on the main-loop thread: tween the stat/exp bars from
// `before` to the new growth (pet hops), then a level-up / evolve flash. Blocks
// input briefly (intended); queued keys are drained by the caller afterward.
static void play_eat_animation(RadiotchiApp* app, const PetGrowth* before) {
    for(int f = 0; f <= EAT_BARS_FRAMES; f++) {
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        app->model.eat_before = *before;
        app->model.eat_t = (float)f / (float)EAT_BARS_FRAMES;
        app->model.eat_phase = EAT_PHASE_BARS;
        app->model.screen = ScreenEatAnim;
        furi_mutex_release(app->mutex);
        view_port_update(app->view_port);
        furi_delay_ms(EAT_BARS_MS);
    }

    // Flash on any notable event: level-up, evolution, or a delicacy/novel catch.
    if(app->model.eat_event != 0u) {
        for(int f = 0; f < EAT_FLASH_FRAMES; f++) {
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            app->model.eat_phase = EAT_PHASE_FLASH;
            app->model.anim_frame++;
            app->model.screen = ScreenEatAnim;
            furi_mutex_release(app->mutex);
            view_port_update(app->view_port);
            furi_delay_ms(EAT_FLASH_MS);
        }
    }
}

static void do_eat(RadiotchiApp* app) {
    // Commit the staged capture: lossless save + species bump + pet evolution.
    capture_store_save(
        app->store,
        &app->model.ev,
        app->pending_pulses,
        app->pending_pulse_count,
        app->pending_preset);

    species_index_bump(app->index, app->model.ev.species_id, app->model.ev.timestamp);
    species_index_save(app->index);

    // Growth layer: snapshot the pre-meal state, feed (5 stats EMA + EXP/level +
    // 100-type re-derivation), record the meal for hunger/mood, then persist. A neglected
    // pet's meal grants reduced exp (computed from the PRE-meal care state); only the exp
    // term is softened — the reversible stat EMA is untouched.
    PetGrowth before = app->model.growth;
    uint32_t raw_gain = pet_growth_exp_gain(&app->model.ev, app->pending_seen_count);
    uint32_t adj_gain =
        pet_mood_apply_exp_pressure(raw_gain, &app->model.care, app->model.ev.timestamp);
    pet_growth_feed_scaled(
        &app->model.growth, &app->model.ev, app->pending_seen_count, adj_gain,
        raw_gain ? raw_gain : 1u);
    pet_care_feed(&app->model.care, &app->model.ev, app->model.ev.timestamp);
    capture_store_save_growth(app->store, &app->model.growth, &app->model.care, app->model.name);

    // What to celebrate: level-up, evolution (morph changed), and/or a delicacy/novel catch.
    uint8_t evt = 0u;
    if(app->model.growth.level > before.level) evt |= EAT_EVT_LEVEL;
    if(app->model.growth.type_id != before.type_id) evt |= EAT_EVT_EVOLVE;
    if(app->pending_seen_count == 0u || app->model.ev.scores.rarity >= EAT_RARE_SCORE)
        evt |= EAT_EVT_RARE;

    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->model.dex_count = species_index_count(app->index);
    app->model.now_cache = app->model.ev.timestamp; // mood reflects the fresh meal at once
    app->model.eat_event = evt;
    furi_mutex_release(app->mutex);

    FURI_LOG_I(
        TAG,
        "ate %s Lv%u type%u exp%lu",
        app->model.ev.species_id,
        (unsigned)app->model.growth.level,
        (unsigned)app->model.growth.type_id,
        (unsigned long)app->model.growth.total_exp);

    // Celebrate the change, then settle back on the clean Home view.
    play_eat_animation(app, &before);
    set_screen(app, ScreenHome);
    furi_message_queue_reset(app->input_queue); // drop keys mashed during the anim
}

// Re-grade the whole dex: re-run the current decoder over every stored capture so old
// records gain Nourishment / graduate species as decoders are added (A12/A21). Heavy
// file I/O runs unlocked (mirrors do_eat); only the model scalars take the mutex.
static void do_regrade(RadiotchiApp* app) {
    int n = capture_store_regrade(app->store, app->index); // rewrites the log, rebuilds idx

    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->model.regrade_result = n;
    app->model.dex_count = species_index_count(app->index);
    app->model.screen = ScreenRegradeDone;
    furi_mutex_release(app->mutex);
    view_port_update(app->view_port);
    FURI_LOG_I(TAG, "regrade: %d rows changed", n);
}

// Commit the name editor into the live name + persisted growth file.
static void commit_name(RadiotchiApp* app) {
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    strncpy(app->model.name, app->model.name_edit, RADIOTCHI_PET_NAME_CAP - 1);
    app->model.name[RADIOTCHI_PET_NAME_CAP - 1] = '\0';
    // Trim trailing spaces.
    for(int i = (int)strlen(app->model.name) - 1; i >= 0 && app->model.name[i] == ' '; i--)
        app->model.name[i] = '\0';
    furi_mutex_release(app->mutex);
    capture_store_save_growth(app->store, &app->model.growth, &app->model.care, app->model.name);
}

// ========================= dex read ======================================

// Collect the newest captures for a species by streaming the capture log.
static void dex_collect_row(void* ctx, const char* const* fields, int nfields);

typedef struct {
    AppModel* m;
    const char* filter;
} DexCollectCtx;

static void open_captures_for_selected(RadiotchiApp* app);
static void open_diff_for_selected(RadiotchiApp* app);

// ========================= input routing ===================================

// Run the highlighted Home command (ui-spec.md §2).
static void run_menu_command(RadiotchiApp* app) {
    switch(app->model.menu_sel) {
    case MENU_DETAIL:
        set_screen(app, ScreenPetDetail);
        break;
    case MENU_FEED:
        do_feed(app);
        break;
    case MENU_DEX:
        app->model.species_sel = 0;
        app->model.species_scroll = 0;
        set_screen(app, ScreenDexSpecies);
        break;
    case MENU_REGRADE:
        do_regrade(app);
        break;
    case MENU_TUNE:
        set_screen(app, ScreenSettings);
        break;
#ifdef RADIOTCHI_DEBUG
    case MENU_DEBUG:
        app->model.dbg_index = 0;
        app->model.dbg_tired = false;
        set_screen(app, ScreenDebugGallery);
        break;
#endif
    default:
        break;
    }
}

// Enter the inline name editor seeded with the current name.
static void enter_name_edit(RadiotchiApp* app) {
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    memset(app->model.name_edit, 0, sizeof(app->model.name_edit));
    strncpy(app->model.name_edit, app->model.name, RADIOTCHI_PET_NAME_CAP - 1);
    app->model.name_cursor = 0;
    app->model.screen = ScreenNameEdit;
    furi_mutex_release(app->mutex);
    view_port_update(app->view_port);
}

// Cycle the character under the cursor by `dir` (+1/-1) within NAME_CHARSET.
static void name_edit_cycle(RadiotchiApp* app, int dir) {
    static const char* set = NAME_CHARSET;
    int n = (int)strlen(set);
    char cur = app->model.name_edit[app->model.name_cursor];
    if(cur == '\0') cur = ' ';
    int idx = 0;
    for(int i = 0; i < n; i++) {
        if(set[i] == cur) {
            idx = i;
            break;
        }
    }
    idx = (idx + dir + n) % n;
    app->model.name_edit[app->model.name_cursor] = set[idx];
    view_port_update(app->view_port);
}

static void on_key(RadiotchiApp* app, InputKey key, InputType type, bool* running) {
    // Long-press Back on the resting Home view exits the app (ui-spec.md §1).
    if(app->model.screen == ScreenHome && !app->model.menu_open && key == InputKeyBack &&
       type == InputTypeLong) {
        *running = false;
        return;
    }

    bool press = (type == InputTypeShort) || (type == InputTypeRepeat);
    if(!press) return;
    Screen screen = app->model.screen;

    switch(screen) {
    case ScreenHome:
        if(type != InputTypeShort) break;
        if(!app->model.menu_open) {
            // Any key (except Back) raises the command menu; Back rests silently.
            if(key != InputKeyBack) {
                app->model.menu_open = true;
                // Keep the selection within the visible scroll window.
                if(app->model.menu_sel < app->model.menu_scroll)
                    app->model.menu_scroll = app->model.menu_sel;
                else if(app->model.menu_sel >= app->model.menu_scroll + MENU_VISIBLE_ROWS)
                    app->model.menu_scroll = app->model.menu_sel - MENU_VISIBLE_ROWS + 1u;
                view_port_update(app->view_port);
            }
        } else if(key == InputKeyUp) {
            if(app->model.menu_sel > 0) {
                app->model.menu_sel--;
                if(app->model.menu_sel < app->model.menu_scroll)
                    app->model.menu_scroll = app->model.menu_sel;
                view_port_update(app->view_port);
            }
        } else if(key == InputKeyDown) {
            if(app->model.menu_sel + 1u < MENU_COUNT) {
                app->model.menu_sel++;
                if(app->model.menu_sel >= app->model.menu_scroll + MENU_VISIBLE_ROWS)
                    app->model.menu_scroll = app->model.menu_sel - MENU_VISIBLE_ROWS + 1u;
                view_port_update(app->view_port);
            }
        } else if(key == InputKeyOk) {
            run_menu_command(app);
        } else if(key == InputKeyBack) {
            app->model.menu_open = false;
            view_port_update(app->view_port);
        }
        break;

    case ScreenPetDetail:
        if(type != InputTypeShort) break;
        if(key == InputKeyOk)
            enter_name_edit(app); // edit the name
        else if(key == InputKeyBack)
            set_screen(app, ScreenHome);
        break;

    case ScreenNameEdit:
        if(type != InputTypeShort && type != InputTypeRepeat) break;
        if(key == InputKeyUp)
            name_edit_cycle(app, +1);
        else if(key == InputKeyDown)
            name_edit_cycle(app, -1);
        else if(key == InputKeyLeft) {
            if(app->model.name_cursor > 0) app->model.name_cursor--;
            view_port_update(app->view_port);
        } else if(key == InputKeyRight) {
            if(app->model.name_cursor + 2u < RADIOTCHI_PET_NAME_CAP) app->model.name_cursor++;
            view_port_update(app->view_port);
        } else if(key == InputKeyOk && type == InputTypeShort) {
            commit_name(app);
            set_screen(app, ScreenPetDetail);
        } else if(key == InputKeyBack && type == InputTypeShort) {
            set_screen(app, ScreenPetDetail); // cancel
        }
        break;

    case ScreenSettings:
        if(key == InputKeyUp) {
            app->model.threshold = clamp16(app->model.threshold + THRESHOLD_STEP, THRESHOLD_MIN, THRESHOLD_MAX);
            apply_tuning(app);
            view_port_update(app->view_port);
        } else if(key == InputKeyDown) {
            app->model.threshold = clamp16(app->model.threshold - THRESHOLD_STEP, THRESHOLD_MIN, THRESHOLD_MAX);
            apply_tuning(app);
            view_port_update(app->view_port);
        } else if(key == InputKeyRight) {
            app->model.margin = clamp16(app->model.margin + MARGIN_STEP, MARGIN_MIN, MARGIN_MAX);
            apply_tuning(app);
            view_port_update(app->view_port);
        } else if(key == InputKeyLeft) {
            app->model.margin = clamp16(app->model.margin - MARGIN_STEP, MARGIN_MIN, MARGIN_MAX);
            apply_tuning(app);
            view_port_update(app->view_port);
        } else if(key == InputKeyBack && type == InputTypeShort) {
            set_screen(app, ScreenHome);
        }
        break;

    case ScreenFeedReadout:
        if(type != InputTypeShort) break;
        if(key == InputKeyOk) set_screen(app, ScreenFeedLabel);
        else if(key == InputKeyBack)
            set_screen(app, ScreenHome); // discard
        break;

    case ScreenFeedLabel:
        if(type != InputTypeShort) break;
        if(key == InputKeyOk) do_eat(app);
        else if(key == InputKeyBack)
            set_screen(app, ScreenHome); // discard
        break;

    case ScreenNoSignal:
        if(type != InputTypeShort) break;
        if(key == InputKeyOk) do_feed(app);
        else if(key == InputKeyBack)
            set_screen(app, ScreenHome);
        break;

    case ScreenRegradeDone:
        if(type != InputTypeShort) break;
        if(key == InputKeyBack || key == InputKeyOk) set_screen(app, ScreenHome);
        break;

    case ScreenDexSpecies: {
        uint32_t count = app->model.dex_count;
        if(key == InputKeyDown && count > 0) {
            if(app->model.species_sel + 1 < count) app->model.species_sel++;
            if(app->model.species_sel >= app->model.species_scroll + LIST_VISIBLE_ROWS)
                app->model.species_scroll = app->model.species_sel - LIST_VISIBLE_ROWS + 1;
            view_port_update(app->view_port);
        } else if(key == InputKeyUp && count > 0) {
            if(app->model.species_sel > 0) app->model.species_sel--;
            if(app->model.species_sel < app->model.species_scroll)
                app->model.species_scroll = app->model.species_sel;
            view_port_update(app->view_port);
        } else if(key == InputKeyOk && type == InputTypeShort && count > 0) {
            open_captures_for_selected(app);
        } else if(key == InputKeyBack && type == InputTypeShort) {
            set_screen(app, ScreenHome);
        }
        break;
    }

    case ScreenDexCaptures: {
        uint32_t count = app->model.capture_count;
        if(key == InputKeyDown && count > 0) {
            if(app->model.capture_sel + 1 < count) app->model.capture_sel++;
            if(app->model.capture_sel >= app->model.capture_scroll + LIST_VISIBLE_ROWS)
                app->model.capture_scroll = app->model.capture_sel - LIST_VISIBLE_ROWS + 1;
            view_port_update(app->view_port);
        } else if(key == InputKeyUp && count > 0) {
            if(app->model.capture_sel > 0) app->model.capture_sel--;
            if(app->model.capture_sel < app->model.capture_scroll)
                app->model.capture_scroll = app->model.capture_sel;
            view_port_update(app->view_port);
        } else if(key == InputKeyOk && type == InputTypeShort && count > 0) {
            // Detail uses the currently-loaded ev (last captured). For older rows
            // a full re-load from .sub is post-MVP; show the live ev fields.
            set_screen(app, ScreenCaptureDetail);
        } else if(key == InputKeyRight && type == InputTypeShort && count > 0) {
            open_diff_for_selected(app); // align this species' frames byte-by-byte
        } else if(key == InputKeyBack && type == InputTypeShort) {
            set_screen(app, ScreenDexSpecies);
        }
        break;
    }

    case ScreenCaptureDetail:
        if(key == InputKeyBack && type == InputTypeShort) set_screen(app, ScreenDexCaptures);
        break;

    case ScreenDexDiff:
        if(key == InputKeyBack && type == InputTypeShort) set_screen(app, ScreenDexCaptures);
        break;

#ifdef RADIOTCHI_DEBUG
    case ScreenDebugGallery:
        // L/R step one; U/D jump DEBUG_GALLERY_STEP; both wrap. OK toggles eye; Back home.
        if(key == InputKeyRight) {
            app->model.dbg_index = (uint16_t)((app->model.dbg_index + 1u) % DEBUG_GALLERY_COUNT);
            view_port_update(app->view_port);
        } else if(key == InputKeyLeft) {
            app->model.dbg_index =
                (uint16_t)((app->model.dbg_index + DEBUG_GALLERY_COUNT - 1u) % DEBUG_GALLERY_COUNT);
            view_port_update(app->view_port);
        } else if(key == InputKeyDown) {
            app->model.dbg_index =
                (uint16_t)((app->model.dbg_index + DEBUG_GALLERY_STEP) % DEBUG_GALLERY_COUNT);
            view_port_update(app->view_port);
        } else if(key == InputKeyUp) {
            app->model.dbg_index = (uint16_t)(
                (app->model.dbg_index + DEBUG_GALLERY_COUNT - DEBUG_GALLERY_STEP) %
                DEBUG_GALLERY_COUNT);
            view_port_update(app->view_port);
        } else if(key == InputKeyOk && type == InputTypeShort) {
            app->model.dbg_tired = !app->model.dbg_tired;
            view_port_update(app->view_port);
        } else if(key == InputKeyBack && type == InputTypeShort) {
            set_screen(app, ScreenHome);
        }
        break;
#endif

    default:
        break;
    }
}

// ========================= lifecycle =======================================

static RadiotchiApp* radiotchi_app_alloc(void) {
    RadiotchiApp* app = malloc(sizeof(RadiotchiApp));
    memset(app, 0, sizeof(*app));

    frequency_plan_load_japan_defaults(&app->plan);
    app->model.screen = ScreenHome;

    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->input_queue = furi_message_queue_alloc(8, sizeof(InputEvent));

    SubGhzCaptureConfig cfg = subghz_capture_config_default();
    app->capture = subghz_capture_source_alloc(&app->plan, cfg);
    app->store = capture_store_alloc();
    app->index = species_index_alloc(app->store ? capture_store_storage(app->store) : NULL);
    if(app->index) species_index_load(app->index);
    app->model.dex_count = species_index_count(app->index);

    // Seed tuning from persisted config, else defaults.
    CaptureTuning t = {cfg.rssi_threshold_dbm, cfg.detection_margin_db};
    if(app->store) capture_store_load_tuning(app->store, &t);
    app->model.threshold = t.rssi_threshold_dbm;
    app->model.margin = t.detection_margin_db;
    cfg.rssi_threshold_dbm = t.rssi_threshold_dbm;
    cfg.detection_margin_db = t.detection_margin_db;
    if(app->capture) subghz_capture_source_set_config(app->capture, cfg);

    // Load the growth layer + care + name, or start a fresh Unformed pet named "Radiotchi".
    // (load_growth always seeds care to never-fed grace, so a fresh or pre-care pet is safe.)
    if(!app->store ||
       !capture_store_load_growth(
           app->store, &app->model.growth, &app->model.care, app->model.name,
           RADIOTCHI_PET_NAME_CAP)) {
        pet_growth_init(&app->model.growth);
        pet_care_init(&app->model.care);
        strncpy(app->model.name, "Radiotchi", RADIOTCHI_PET_NAME_CAP - 1);
    }
    if(app->model.name[0] == '\0') strncpy(app->model.name, "Radiotchi", RADIOTCHI_PET_NAME_CAP - 1);
    app->model.now_cache = capture_store_now(); // seed mood time before the first draw

    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, draw_callback, app);
    view_port_input_callback_set(app->view_port, input_callback, app);

    // Idle animation timer (Home pet bobs; MVP = idle only).
    app->anim_timer = furi_timer_alloc(anim_timer_callback, FuriTimerTypePeriodic, app);
    furi_timer_start(app->anim_timer, furi_ms_to_ticks(ANIM_PERIOD_MS));

    app->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);
    return app;
}

static void radiotchi_app_free(RadiotchiApp* app) {
    if(app->anim_timer) {
        furi_timer_stop(app->anim_timer);
        furi_timer_free(app->anim_timer);
    }
    gui_remove_view_port(app->gui, app->view_port);
    furi_record_close(RECORD_GUI);
    view_port_free(app->view_port);

    if(app->index) species_index_free(app->index);
    if(app->capture) subghz_capture_source_free(app->capture);
    if(app->store) capture_store_free(app->store);

    furi_message_queue_free(app->input_queue);
    furi_mutex_free(app->mutex);
    free(app);
}

int32_t radiotchi_app(void* p) {
    UNUSED(p);
    RadiotchiApp* app = radiotchi_app_alloc();

    InputEvent event;
    bool running = true;
    while(running) {
        if(furi_message_queue_get(app->input_queue, &event, FuriWaitForever) != FuriStatusOk) {
            continue;
        }
        on_key(app, event.key, event.type, &running);
    }

    radiotchi_app_free(app);
    return 0;
}

// ========================= dex read (impl) ===============================

static void fill_row(DexCaptureRow* r, const char* const* fields, int nfields) {
    // Columns: 1=datetime 2=freq 3=mod 4=rssi 5=bits 6=entropy 7=tier ... 15=individual.
    strncpy(r->datetime, fields[1], sizeof(r->datetime) - 1);
    r->datetime[sizeof(r->datetime) - 1] = '\0';
    r->freq = (uint32_t)strtoul(fields[2], NULL, 10);
    strncpy(r->modulation, fields[3], sizeof(r->modulation) - 1);
    r->modulation[sizeof(r->modulation) - 1] = '\0';
    r->rssi = (int16_t)atoi(fields[4]);
    r->bits = (uint16_t)atoi(fields[5]);
    // entropy is "%.3f"; parse integer + first two fractional digits as centi.
    int ei = 0, ef = 0;
    if(sscanf(fields[6], "%d.%2d", &ei, &ef) >= 1) r->entropy_centi = (int16_t)(ei * 100 + ef);
    r->tier = (uint8_t)atoi(fields[7]);
    r->individual[0] = '\0'; // optional trailing column; absent in pre-individual logs
    if(nfields >= 16) {
        strncpy(r->individual, fields[15], sizeof(r->individual) - 1);
        r->individual[sizeof(r->individual) - 1] = '\0';
    }
}

static void dex_collect_row(void* ctx, const char* const* fields, int nfields) {
    DexCollectCtx* c = ctx;
    AppModel* m = c->m;
    // Keep the newest DEX_CAPTURES_MAX rows (log is chronological; shift on overflow).
    if(m->capture_count < DEX_CAPTURES_MAX) {
        fill_row(&m->captures[m->capture_count++], fields, nfields);
    } else {
        for(uint32_t i = 1; i < DEX_CAPTURES_MAX; i++) m->captures[i - 1] = m->captures[i];
        fill_row(&m->captures[DEX_CAPTURES_MAX - 1], fields, nfields);
    }
}

static void open_captures_for_selected(RadiotchiApp* app) {
    const SpeciesRecord* r = species_index_get(app->index, app->model.species_sel);
    if(r == NULL) return;
    strncpy(app->model.browse_species, r->species_id, sizeof(app->model.browse_species) - 1);
    app->model.browse_species[sizeof(app->model.browse_species) - 1] = '\0';

    app->model.capture_count = 0;
    app->model.capture_sel = 0;
    app->model.capture_scroll = 0;
    DexCollectCtx ctx = {.m = &app->model, .filter = app->model.browse_species};
    capture_store_for_each_row(app->store, app->model.browse_species, dex_collect_row, &ctx);

    set_screen(app, ScreenDexCaptures);
}

// Diff-learning: re-read this species' captures from their `.sub`, decode each to a frame, and
// classify byte positions (id / counter / value). Heavy I/O + decode run UNLOCKED (mirrors
// do_regrade); only the small ByteDiff result is published under the lock.
static void open_diff_for_selected(RadiotchiApp* app) {
    uint8_t payloads[DEX_DIFF_MAX][RADIOTCHI_DIFF_BYTES_MAX];
    uint16_t lens[DEX_DIFF_MAX];
    uint8_t count = capture_store_collect_payloads(
        app->store, app->model.browse_species, payloads, lens, DEX_DIFF_MAX);

    const uint8_t* ptrs[DEX_DIFF_MAX];
    for(uint8_t i = 0; i < count; i++) ptrs[i] = payloads[i];
    ByteDiff diff = radiotchi_byte_diff(ptrs, lens, count);

    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->model.diff = diff;
    app->model.diff_count = count;
    furi_mutex_release(app->mutex);
    set_screen(app, ScreenDexDiff);
}
