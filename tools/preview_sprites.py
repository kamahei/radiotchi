#!/usr/bin/env python3
"""Build a PC viewer (HTML gallery) of the Radiotchi pet characters.

Each creature is now ONE monolithic 64x64 sprite (no layered compose) with a 4-frame
idle sway in icons/ (<name>_0.png .. <name>_3.png; see tools/convert_art.py). This
viewer scales each frame up and writes an animated GIF per character that loops the
sway at the device's idle rate, then lays them out as a family x shape grid plus the
egg/children strip in preview/index.html.

Run with the Pillow-enabled interpreter (same one convert_art.py uses), e.g. ufbt's
bundled python:
    .../python.exe tools/convert_art.py        # (re)build icons/ from art/
    .../python.exe tools/preview_sprites.py     # build preview/index.html
    # then open preview/index.html in a browser
"""

from __future__ import annotations

from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
ICONS = ROOT / "icons"
OUT = ROOT / "preview"

GRID = 64
SCALE = 5  # on-page pixel size
FRAME_MS = 400  # mirror ANIM_PERIOD_MS in radiotchi_app.c

FAMILIES = ("mass", "vigor", "wild", "aura", "mind")
SHAPES = ("pure", "sprout", "crested", "woven", "diffuse")
FAM_LABEL = {"mass": "Mass", "vigor": "Vigor", "wild": "Wild", "aura": "Aura", "mind": "Mind"}


def upscale(im: Image.Image) -> Image.Image:
    return im.convert("L").resize((GRID * SCALE, GRID * SCALE), Image.NEAREST)


def emit_gif(stem: str) -> str:
    """Write preview/<stem>.gif looping the 4-frame idle sway; return the filename."""
    frames = [upscale(Image.open(ICONS / f"{stem}_{f}.png")) for f in (0, 1, 2, 3)]
    out = OUT / f"{stem}.gif"
    frames[0].save(
        out,
        save_all=True,
        append_images=frames[1:],
        duration=FRAME_MS,
        loop=0,
        disposal=2,
    )
    return out.name


def main() -> None:
    if not ICONS.exists():
        raise SystemExit("icons/ not found - run: python tools/convert_art.py")
    OUT.mkdir(parents=True, exist_ok=True)

    # 25 adults: rows = family, cols = shape.
    rows = []
    for fam in FAMILIES:
        tds = [f'<th class="rowh">{FAM_LABEL[fam]}</th>']
        for shp in SHAPES:
            src = emit_gif(f"char_{fam}_{shp}")
            tds.append(f'<td><img src="{src}"><div class="cap">{fam}/{shp}</div></td>')
        rows.append("<tr>" + "".join(tds) + "</tr>")
    header = "<tr><th></th>" + "".join(f"<th>{s}</th>" for s in SHAPES) + "</tr>"
    adult_table = f'<table class="grid">{header}{"".join(rows)}</table>'

    # egg + children
    pre = [f'<td><img src="{emit_gif("egg")}"><div class="cap">egg</div></td>']
    for fam in FAMILIES:
        pre.append(f'<td><img src="{emit_gif(f"child_{fam}")}"><div class="cap">child {fam}</div></td>')
    pre_table = f'<table class="strip"><tr>{"".join(pre)}</tr></table>'

    html = f"""<!doctype html><html><head><meta charset="utf-8">
<title>Radiotchi characters</title>
<style>
 body{{background:#cfcfcf;color:#222;font:13px/1.4 system-ui,sans-serif;margin:24px}}
 h1{{font-size:18px}} h2{{font-size:14px;margin:24px 0 8px}}
 img{{image-rendering:pixelated;background:#fff;border:1px solid #aaa;display:block}}
 table{{border-collapse:separate;border-spacing:10px}}
 td{{text-align:center;vertical-align:top}}
 th{{font-weight:600;color:#444}} th.rowh{{text-align:right;padding-right:4px}}
 .cap{{margin-top:4px;color:#555;font-size:11px}}
</style></head><body>
<h1>Radiotchi pet characters</h1>
<p>Each creature is a single 64x64 1-bit sprite as the device draws it, animated through
its 4-frame idle sway (rest, pose A, rest, pose B). The 25 adult characters are keyed by
<b>family × shape</b>; the 2nd-stat partner that fans these into the full 100 types is
shown as text on the Pet Detail screen, not on the silhouette. Regenerate art into
<code>icons/</code> with <code>python tools/convert_art.py</code> then rerun this script.</p>
<h2>Adults — 25 characters (family rows × shape columns)</h2>
{adult_table}
<h2>Egg &amp; children</h2>
{pre_table}
</body></html>"""
    (OUT / "index.html").write_text(html, encoding="utf-8")
    n = len(list(OUT.glob("*.gif")))
    print(f"wrote {OUT/'index.html'} + {n} GIFs (scale {SCALE}x). Open it in a browser.")


if __name__ == "__main__":
    main()
