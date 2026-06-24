# UI Spec

The Flipper Zero has a **128×64 monochrome display** and a small input set (Up/Down/Left/Right
+ OK/center + Back). All UI is drawn through the firmware's **GUI / ViewPort** subsystem. This
constrains the design: one screen does one job, text is terse, and visuals are 1-bit. UI lives
entirely in the **Game Shell** (see [architecture.md](architecture.md)); it reads state and
`CaptureEvent` records and never performs RF work.

## Input Model

The interaction is **Tamagotchi-style**: Home shows only the living pet; pressing a button
raises a command menu over it. Buttons are contextual per screen.

| Button | Home (pet view) | Command menu | Lists / detail |
|---|---|---|---|
| OK (center) | Open the **command menu** | Run the highlighted command | Confirm / drill in |
| Up / Down | Open menu | Move the highlight (list scrolls) | Scroll |
| Left / Right | Open menu | (unused) | Page / navigate |
| Back | (long-press exits the app) | Close menu → clean pet view | Return to previous screen |

## Screens

### 1. Home / Pet View (default)

The resting screen is **just the pet** — no status text, no chrome — so it reads as a living
companion.

- Shows the **pet** for its current life stage (egg → lineage-tinted child → the 100-type
  morph; [pet-growth-spec.md](pet-growth-spec.md) §4, D18/D21). The adult morph is **composed
  from layered 1-bit parts** (lineage base + body accent + head + eyes) keyed off `type_id`.
  *(`pet_sprites.c` composes the real compiled-in 1-bit parts via `canvas_draw_icon`.)*
- The pet **animates autonomously**. **MVP: an Idle animation only** (e.g. a gentle 2-frame
  bob/blink); richer autonomous behaviors (wander, react, sleep) come later.
- **Any button raises the Command Menu** (§2); Back at rest does nothing (long-press exits).

### 2. Command Menu (overlay on Home)

A **vertical panel down the right side** of the wide screen; the pet shifts left and stays
visible. Highlight moves with **Up/Down** (the list **scrolls** when commands exceed the
visible rows), **OK runs**, **Back dismisses** to the clean pet view. Commands:

| Command | Goes to | Purpose |
|---|---|---|
| **Detail** | Pet Detail (§3) | Inspect the pet: name, level/exp, type, parameters |
| **Feed** (primary) | Feed Flow (§4) | Capture the strongest signal and feed it |
| **Dex** | Dex Browser (§5) | Browse collected species / captures |
| **Re-grade** | result toast | Re-run the decoder over the whole capture log so old captures gain Nourishment / graduate species (D25); shows how many changed |
| **Tune** | Settings (§6) | Band preset and pet name |

### 3. Pet Detail

The pet's "stat card", reached from the Detail command. Read-only except the name. Shows:

- **Name** — user-given, editable here (text-input; default e.g. `Radiotchi`). See
  [data-model.md](data-model.md) `name`.
- **Level & EXP** — current level and progress toward the next (from `total_exp`,
  [pet-growth-spec.md](pet-growth-spec.md) §3).
- **Type** — the evolution/character type name (e.g. `Wild-Aura Pure`) and `type_id`.
- **Parameters** — the five stats as labeled bars: `MASS · VIGOR · WILD · AURA · MIND`
  ([pet-growth-spec.md](pet-growth-spec.md) §1).

### 4. Feed Flow (transient sequence)

A short sequence triggered by the **Feed** command (§2); no free navigation mid-capture.

1. **Scanning** — a brief "sweeping…" indicator while the Sub-GHz sweep runs over the band
   preset and the **strongest** signal is captured.
2. **"What it was"** readout — the raw, honest facts of the catch:
   - frequency (e.g. `433.92 MHz`)
   - modulation guess (e.g. `OOK`, or `?` if unknown)
   - RSSI (e.g. `-62 dBm`)
   - protocol/device if identified, else `Unknown`
3. **Nutrition Label** — the 5 axes as compact bars/values:
   `Calories · Freshness · Additives · Rarity · Nourishment`.
   In the MVP, **Nourishment** may be shown as `—` (not yet computed); the other four are
   always present (graceful degradation, see [product-spec.md](product-spec.md)).
4. **Eat** — confirm; the pet reacts, stats/evolution nudge, the capture is appended to the
   log and the species count bumps. Returns to Home.

### 5. Dex Browser

- **Species list:** name (or provisional fingerprint label), count, first/last-seen hint;
  scroll with Up/Down. Rare species visually distinguished.
- **Species detail → capture list:** individual captures with timestamp; rows also show the
  privacy-safe device tag (`id-XXXX`, D27) when a stable code was decoded, so a **recurring
  device** is scannable down the list. The header shows the **distinct-device count** ("Captures
  - N dev") for that species. Selecting a row shows its stored facts and scores.
- **Capture detail:** the longitudinal/learning view — decode tier, entropy, the device tag
  (where decoded), and (future) a **diff hint** against prior captures of the same device
  (static vs. incrementing vs. world-varying bytes — needs multi-field decoders).
- **Privacy:** persistent identifiers (TPMS/key-fob IDs) are never surfaced raw — the device tag
  is a one-way hash (you cannot recover the id), and species stay family-level, per
  [data-model.md](data-model.md) / decision-log D26–D27.

### 6. Settings (Tune)

- **Band preset** — the Sub-GHz frequency plan used by the sweep (the primary MVP setting; see
  [decision-log.md](decision-log.md) D14).
- **Pet name** — edit the user-given name (also reachable from Pet Detail §3).
- Keep it lean; nothing here beyond the above for v1.

## Sprite & Animation Sizing

- **Base cell is 64×64** (the pet at rest, idle animation). It need not fill the 128-wide
  screen — the resting pet sits in a 64×64 cell, leaving room for the command-menu overlay.
- **Full-frame animations may use the whole 128×64** (special/autonomous moments later — e.g.
  an evolution flourish or a big reaction). MVP ships **Idle only**.
- The adult pet is **one monolithic image-generated 1-bit creature** per `family×shape` (25),
  drawn into a 64×64 cell (D22, superseding the earlier layered-parts compose D21); the egg and
  the lineage-tinted child are shared pre-morph sprites. Mood is baked into the art; the 2nd-stat
  partner is shown as text on Pet Detail. Sprites are drawn **by reference**
  (`canvas_draw_icon`) — never copy a large sprite buffer onto the draw-thread stack.

## State & Rendering Notes

- Rendering is **pull-based** from Game Shell state; the ViewPort draw callback must be cheap
  and allocation-free on the hot path.
- **Do not copy large sprite/animation buffers onto the draw-thread stack** — the GUI thread's
  stack is tiny; draw assets by reference (icon/bitmap APIs), not by value.
- All long work (sweep/capture) happens off the draw thread; the Feed Flow shows a progress
  state rather than blocking the UI.
- Because the screen is 1-bit and tiny, prefer glyphs + short labels over prose; the detailed
  "teaching" text belongs in the dex detail view, not the transient feed flow.

## Out of Scope (UI, v1)

- Battle/versus screens (future; see [architecture.md](architecture.md) §7).
- Autonomous behaviors beyond the Idle animation (wander/react/sleep are post-MVP).
- Settings beyond the band-preset selector and pet name (keep the MVP lean).
