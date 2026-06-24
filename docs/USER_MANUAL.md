# Radiotchi — User Manual

A complete guide to raising your radio creature. For a one‑page intro see the [README](../README.md).

> **RX‑only.** Radiotchi only ever *listens* on Sub‑GHz. It never transmits, replays, or jams.

---

## 1. Install

1. Download `radiotchi.fap` from the project’s **Releases** page.
2. Copy it onto your Flipper: `SD Card / apps / Sub‑GHz / radiotchi.fap` (use [qFlipper](https://flipperzero.one/update) or just drag‑and‑drop the file).
3. Launch it on the Flipper: **Apps → Sub‑GHz → Radiotchi**.

Works on Flipper Zero **OFW** and OFW‑compatible firmwares.

## 2. Your pet, at a glance

When you first launch, your pet is an **egg**. It hatches and grows as you feed it real radio signals. The home screen is just the creature, idling.

<img src="images/home.png" width="320" alt="Home screen">

| Control | On the home screen | In the menu | In lists / detail |
|---|---|---|---|
| **OK** | open the command menu | run the highlighted command | confirm / open |
| **Up / Down** | open the menu | move the highlight | scroll |
| **Left / Right** | open the menu | — | — |
| **Back** | (hold to exit the app) | close the menu | go back |

Press any key to open the **command menu**: **Detail · Feed · Dex · Re‑grade · Tune**.

## 3. Feeding (the core loop)

Choose **Feed**. Your Flipper hops across a set of Japan‑relevant Sub‑GHz bands, samples signal strength, and **locks onto the single strongest signal** it hears, then captures it.

1. **Scanning** — “Hopping… searching”. Stand near a source of RF (a remote, a sensor, ambient traffic).
2. **What it was** — the capture’s real facts: frequency, modulation, RSSI, and the decoded protocol (or `Unknown`). Press **OK** to continue.
3. **Nutrition label** — the five‑axis score (below). Press **OK** to **Eat**, or **Back** to drop the catch.
4. **Eat** — your pet eats, its stats shift, and it may **level up** or **evolve** (a little celebration plays).

If nothing stands out from the noise, you’ll get **“Nothing to eat”** — move somewhere with more RF, or lower the detection threshold in **Tune**.

## 4. Reading the nutrition label

<img src="images/label.png" width="320" alt="Nutrition label">

Every catch is scored 0–100 on five axes:

| Axis | Means | High value =… |
|---|---|---|
| **Cal** — Calories | data volume / burst length | a big, busy signal |
| **Fre** — Freshness | signal strength (RSSI) | the source was close / strong |
| **Add** — Additives | entropy of the payload | looks encrypted / whitened (junk) |
| **Rar** — Rarity | how unusual it is **for you** | you’ve rarely seen this kind before |
| **Nou** — Nourishment | how deeply it could be decoded | a recognised protocol with real values |

The label is what shapes your pet. **Junk** (high Additives, low everything else) keeps a scrappy creature; **delicacies** (rare, structured, decodable, strong) grow something richer.

## 5. Growth & the 100‑type morph

<img src="images/growth.png" width="560" alt="Egg, child, adult">

- Your pet earns **EXP** every meal and **levels up**. Level is a *maturity clock*, never power — collection is the goal, not grinding.
- Five hidden **stats** are long‑memory averages of your diet (one per nutrition axis).
- At level checkpoints the pet re‑reads its stats and **morphs** into one of **100 character types** — a family (your dominant stat → an animal motif) × a shape (how peaked your profile is). The 2nd‑strongest stat is shown as text on the **Detail** screen.
- It’s **reversible**: change what you feed it, and a later checkpoint re‑shapes the creature. The 100 types are a **morph‑dex** to discover.

Life stages: **egg** (Lv 1–4) → a lineage‑tinted **child** (Lv 5–9) → the full **adult morph** (Lv 10+).

**Detail** shows the name (editable), level/EXP, the type name (e.g. *Wild‑Aura Pure*), and all five stats.

## 6. The dex (your RF field guide)

Choose **Dex** to browse what you’ve collected.

- **Species list** — each kind of signal you’ve caught, with a count. Unknown signals are bucketed into a provisional fingerprint species; once a decoder recognises them they **graduate** to a named protocol.
- **Captures** — the individual catches for a species, newest first. When a stable device code was decoded, the row shows a **device tag** (`id‑XXXX`), and the header shows the **distinct‑device count** (“Captures – N dev”) — so you can see how many different devices of this kind you’ve met.
- **Capture detail** — the stored facts and scores for one catch.

The device tag is a **one‑way hash** — it lets you recognise the *same* device over time without ever storing or showing its raw serial (see Privacy).

## 7. Re‑grade

Decoding can improve over time (a new decoder is added, or you re‑read a saved `.sub`). **Re‑grade** re‑analyses your *entire* capture history so old catches gain meaning retroactively — their Nourishment can rise and unknown signals can graduate to named protocols. It only ever raises a tier, never lowers, so running it again when nothing’s new safely reports **“Up to date – no change.”**

It rewrites the analysis log and rebuilds the species index **atomically** and **never touches your saved `.sub` recordings**. A re‑grade over a large history can take several seconds — the screen may look paused; it isn’t.

## 8. Settings (Tune)

**Tune** adjusts detection:

- **Threshold** — the absolute RSSI gate (Up/Down).
- **Margin** — how far a signal must stand out above the ambient noise floor (Left/Right).

Raise them in a noisy environment so only strong signals are caught; lower them to catch fainter ones. Settings persist across reboots.

## 9. Where your data lives

On the SD card under `/ext/apps_data/radiotchi/`:

- `captures/*.sub` — the lossless raw recordings (re‑decodable later).
- `capture_log.csv` — the analysis rows.
- `species.csv` — the species index (counts, first/last seen).
- `growth.txt` — your pet (stats, level, type, name).

Everything survives a reboot, and stays **on your device**.

## 10. Privacy & ethics

- **RX‑only**, always — Radiotchi never transmits.
- Some signals (TPMS, key fobs) carry a **persistent identifier**. Radiotchi **never surfaces it raw**: species are family‑level (e.g. the protocol name), and the per‑device tag is a one‑way hash. The dex can show that you’ve seen a device again, but cannot be used to identify or track a specific vehicle or person, and nothing leaves the device.

## 11. FAQ / troubleshooting

**“Nothing to eat” every time.** You’re below the detection gate. Move nearer a signal source, or lower the Threshold/Margin in **Tune**.

**A catch shows `Unknown` / `ook‑fixed‑…` and no device tag.** The firmware couldn’t decode that protocol, so Radiotchi labels it honestly at the family level and doesn’t invent a per‑device id. It still counts as a real catch and feeds your pet.

**My remote doesn’t produce a device tag.** Only protocols the firmware can decode (or clean fixed‑codes) yield a stable per‑device tag. If even the stock Flipper Sub‑GHz **Read** can’t decode your remote, Radiotchi can’t either — it’s an unsupported/non‑standard protocol.

**Can it open my car / clone a remote?** No. Radiotchi is **receive‑only** and has no transmit path of any kind. It’s a learning toy, not a security tool.
