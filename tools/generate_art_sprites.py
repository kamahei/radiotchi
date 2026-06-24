#!/usr/bin/env python3
"""Cut final Radiotchi art sprites from an imagegen-generated sprite sheet.

This script intentionally does not draw character art with primitives.  The character designs
come from tools/imagegen_sources/radiotchi_sprite_sheet_imagegen.png, generated with imagegen
using art/probe_cat.png as the style reference.  This script only performs production cleanup:
grid-cell extraction, hard monochrome thresholding, 64x64 fitting, static-derived idle poses,
and 1-bit PNG writing.
"""

from __future__ import annotations

import argparse
import struct
import zlib
from pathlib import Path


GRID = 64
ADULT_RENDER_SIZE = 56
CHILD_RENDER_SIZE = 46
EGG_RENDER_SIZE = 42
DEFAULT_SOURCE = Path("tools/imagegen_sources/radiotchi_sprite_sheet_imagegen.png")
DEFAULT_PURE_SOURCE = Path("tools/imagegen_sources/radiotchi_pure_thinline_imagegen.png")
DEFAULT_OUT_DIR = Path("art")
DEFAULT_IDLE_DIR = Path("art/idle")

FAMILIES = ("mass", "vigor", "wild", "aura", "mind")
SHAPES = ("pure", "sprout", "crested", "woven", "diffuse")

SPRITES: tuple[tuple[str, int, int], ...] = (
    ("egg", 0, 0),
    ("child_mass", 0, 1),
    ("child_vigor", 0, 2),
    ("child_wild", 0, 3),
    ("child_aura", 0, 4),
    ("child_mind", 0, 5),
    *(
        (f"char_{family}_{shape}", family_index + 1, shape_index)
        for family_index, family in enumerate(FAMILIES)
        for shape_index, shape in enumerate(SHAPES)
    ),
)

PURE_SPRITES: tuple[tuple[str, int], ...] = tuple(
    (f"char_{family}_pure", family_index) for family_index, family in enumerate(FAMILIES)
)

Pixel = tuple[int, int, int, int]


def paeth(a: int, b: int, c: int) -> int:
    p = a + b - c
    pa = abs(p - a)
    pb = abs(p - b)
    pc = abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def png_chunks(data: bytes):
    if not data.startswith(b"\x89PNG\r\n\x1a\n"):
        raise ValueError("not a PNG file")
    pos = 8
    while pos < len(data):
        length = struct.unpack(">I", data[pos : pos + 4])[0]
        kind = data[pos + 4 : pos + 8]
        payload = data[pos + 8 : pos + 8 + length]
        pos += 12 + length
        yield kind, payload
        if kind == b"IEND":
            break


def read_png_rgba(path: Path) -> tuple[int, int, list[Pixel]]:
    ihdr = None
    idat = bytearray()
    for kind, payload in png_chunks(path.read_bytes()):
        if kind == b"IHDR":
            ihdr = payload
        elif kind == b"IDAT":
            idat.extend(payload)
    if ihdr is None:
        raise ValueError(f"{path}: missing IHDR")

    width, height, bit_depth, color_type, compression, filter_method, interlace = struct.unpack(
        ">IIBBBBB", ihdr
    )
    if compression != 0 or filter_method != 0 or interlace != 0:
        raise ValueError(f"{path}: unsupported PNG options")
    if bit_depth != 8 or color_type not in (0, 2, 6):
        raise ValueError(f"{path}: expected 8-bit grayscale/RGB/RGBA PNG, got {bit_depth}/{color_type}")

    channels = {0: 1, 2: 3, 6: 4}[color_type]
    bpp = channels
    stride = width * channels
    raw = zlib.decompress(bytes(idat))
    rows: list[bytes] = []
    pos = 0
    prev = bytearray(stride)
    for _ in range(height):
        filter_type = raw[pos]
        pos += 1
        row = bytearray(raw[pos : pos + stride])
        pos += stride
        for i, value in enumerate(row):
            left = row[i - bpp] if i >= bpp else 0
            up = prev[i]
            up_left = prev[i - bpp] if i >= bpp else 0
            if filter_type == 0:
                decoded = value
            elif filter_type == 1:
                decoded = value + left
            elif filter_type == 2:
                decoded = value + up
            elif filter_type == 3:
                decoded = value + ((left + up) // 2)
            elif filter_type == 4:
                decoded = value + paeth(left, up, up_left)
            else:
                raise ValueError(f"{path}: unsupported PNG filter {filter_type}")
            row[i] = decoded & 0xFF
        rows.append(bytes(row))
        prev = row

    pixels: list[Pixel] = []
    for row in rows:
        for x in range(width):
            base = x * channels
            if color_type == 0:
                v = row[base]
                pixels.append((v, v, v, 255))
            elif color_type == 2:
                pixels.append((row[base], row[base + 1], row[base + 2], 255))
            else:
                pixels.append((row[base], row[base + 1], row[base + 2], row[base + 3]))
    return width, height, pixels


def luma(px: Pixel) -> int:
    r, g, b, a = px
    if a == 0:
        return 255
    return (299 * r + 587 * g + 114 * b) // 1000


def group_runs(indices: list[int]) -> list[int]:
    if not indices:
        return []
    groups: list[list[int]] = [[indices[0]]]
    for index in indices[1:]:
        if index <= groups[-1][-1] + 1:
            groups[-1].append(index)
        else:
            groups.append([index])
    return [sum(group) // len(group) for group in groups]


def line_centers(
    width: int,
    height: int,
    pixels: list[Pixel],
    axis: str,
    threshold: int,
    min_fraction: float,
) -> list[int]:
    candidates = []
    if axis == "vertical":
        for x in range(width):
            non_white = sum(1 for y in range(height) if luma(pixels[y * width + x]) < threshold)
            if non_white > height * min_fraction:
                candidates.append(x)
    elif axis == "horizontal":
        for y in range(height):
            row_offset = y * width
            non_white = sum(1 for x in range(width) if luma(pixels[row_offset + x]) < threshold)
            if non_white > width * min_fraction:
                candidates.append(y)
    else:
        raise ValueError(f"unknown axis {axis!r}")
    return group_runs(candidates)


def detect_grid_lines(width: int, height: int, pixels: list[Pixel], threshold: int = 245) -> tuple[list[int], list[int]]:
    vertical = line_centers(width, height, pixels, "vertical", threshold, 0.72)
    horizontal = line_centers(width, height, pixels, "horizontal", threshold, 0.72)

    if len(vertical) != 7:
        vertical = [round(i * (width - 1) / 6) for i in range(7)]
    if len(horizontal) != 7:
        horizontal = [round(i * (height - 1) / 6) for i in range(7)]
    return vertical, horizontal


def detect_pure_cells(width: int, height: int, pixels: list[Pixel], columns: int = 5) -> list[tuple[int, int, int, int]]:
    vertical = line_centers(width, height, pixels, "vertical", 245, 0.35)
    horizontal = line_centers(width, height, pixels, "horizontal", 245, 0.35)

    if len(vertical) >= columns * 2 and len(horizontal) >= 2:
        left_right = [(vertical[i * 2], vertical[i * 2 + 1]) for i in range(columns)]
        top = horizontal[0]
        bottom = horizontal[-1]
        return [(left + 2, top + 2, right - 2, bottom - 2) for left, right in left_right]

    if len(vertical) >= columns + 1 and len(horizontal) >= 2:
        top = horizontal[0]
        bottom = horizontal[-1]
        return [
            (vertical[col] + 2, top + 2, vertical[col + 1] - 2, bottom - 2)
            for col in range(columns)
        ]

    cell_w = width / columns
    top = round(height * 0.16)
    bottom = round(height * 0.84)
    return [
        (round(col * cell_w) + 2, top, round((col + 1) * cell_w) - 2, bottom)
        for col in range(columns)
    ]


def dark_mask(
    width: int,
    pixels: list[Pixel],
    left: int,
    top: int,
    right: int,
    bottom: int,
    threshold: int,
) -> set[tuple[int, int]]:
    dark: set[tuple[int, int]] = set()
    for y in range(top, bottom):
        for x in range(left, right):
            if luma(pixels[y * width + x]) < threshold:
                dark.add((x - left, y - top))
    return dark


def bbox(points: set[tuple[int, int]]) -> tuple[int, int, int, int]:
    xs = [x for x, _ in points]
    ys = [y for _, y in points]
    return min(xs), min(ys), max(xs) + 1, max(ys) + 1


def connected_components(points: set[tuple[int, int]]) -> list[set[tuple[int, int]]]:
    remaining = set(points)
    components: list[set[tuple[int, int]]] = []
    while remaining:
        start = remaining.pop()
        component = {start}
        stack = [start]
        while stack:
            x, y = stack.pop()
            for nx, ny in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)):
                point = (nx, ny)
                if point in remaining:
                    remaining.remove(point)
                    component.add(point)
                    stack.append(point)
        components.append(component)
    return components


def cleanup_stray_components(mask: set[tuple[int, int]], margin: int = 6) -> set[tuple[int, int]]:
    if not mask:
        return set()

    components = connected_components(mask)
    largest = max(components, key=len)
    lx0, ly0, lx1, ly1 = bbox(largest)
    cleaned: set[tuple[int, int]] = set()
    for component in components:
        cx0, cy0, cx1, cy1 = bbox(component)
        center_x = (cx0 + cx1) // 2
        center_y = (cy0 + cy1) // 2
        inside_main_bounds = (
            lx0 - margin <= center_x <= lx1 + margin
            and ly0 - margin <= center_y <= ly1 + margin
        )
        if inside_main_bounds or len(component) >= 24:
            cleaned.update(component)
    return cleaned


def pad_bbox(
    bounds: tuple[int, int, int, int],
    max_w: int,
    max_h: int,
) -> tuple[int, int, int, int]:
    bx0, by0, bx1, by1 = bounds
    pad = max(4, int(max(bx1 - bx0, by1 - by0) * 0.04))
    return (
        max(0, bx0 - pad),
        max(0, by0 - pad),
        min(max_w, bx1 + pad),
        min(max_h, by1 + pad),
    )


def render_64_from_bbox(
    mask: set[tuple[int, int]],
    source_bbox: tuple[int, int, int, int],
    target_size: int,
) -> set[tuple[int, int]]:
    bx0, by0, bx1, by1 = source_bbox
    src_w = bx1 - bx0
    src_h = by1 - by0
    if src_w <= 0 or src_h <= 0:
        return set()

    scale = min(target_size / src_w, target_size / src_h)
    dst_w = max(1, round(src_w * scale))
    dst_h = max(1, round(src_h * scale))
    ox = (GRID - dst_w) // 2
    oy = (GRID - dst_h) // 2

    out: set[tuple[int, int]] = set()
    for dy in range(dst_h):
        sy = by0 + min(src_h - 1, int((dy + 0.5) / scale))
        for dx in range(dst_w):
            sx = bx0 + min(src_w - 1, int((dx + 0.5) / scale))
            hit = (sx, sy) in mask
            if not hit:
                hit = any(
                    (sx + nx, sy + ny) in mask
                    for ny in (-1, 0, 1)
                    for nx in (-1, 0, 1)
                )
            if hit:
                out.add((ox + dx, oy + dy))
    return out


def render_64(
    mask: set[tuple[int, int]],
    cell_w: int,
    cell_h: int,
    target_size: int = ADULT_RENDER_SIZE,
) -> set[tuple[int, int]]:
    if not mask:
        return set()

    return render_64_from_bbox(mask, pad_bbox(bbox(mask), cell_w, cell_h), target_size)


def png_chunk(kind: bytes, payload: bytes) -> bytes:
    crc = zlib.crc32(kind)
    crc = zlib.crc32(payload, crc)
    return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", crc & 0xFFFFFFFF)


def write_png_1bit(path: Path, pixels: set[tuple[int, int]]) -> None:
    raw = bytearray()
    for y in range(GRID):
        raw.append(0)
        byte = 0
        bit_count = 0
        for x in range(GRID):
            bit = 0 if (x, y) in pixels else 1
            byte = (byte << 1) | bit
            bit_count += 1
            if bit_count == 8:
                raw.append(byte)
                byte = 0
                bit_count = 0
        if bit_count:
            raw.append(byte << (8 - bit_count))

    ihdr = struct.pack(">IIBBBBB", GRID, GRID, 1, 0, 0, 0, 0)
    data = b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", ihdr)
    data += png_chunk(b"IDAT", zlib.compress(bytes(raw), level=9))
    data += png_chunk(b"IEND", b"")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def relative_rect(
    pixels: set[tuple[int, int]],
    left: float,
    top: float,
    right: float,
    bottom: float,
) -> tuple[int, int, int, int]:
    x0, y0, x1, y1 = bbox(pixels)
    width = max(1, x1 - x0)
    height = max(1, y1 - y0)
    return (
        x0 + round(width * left),
        y0 + round(height * top),
        x0 + round(width * right),
        y0 + round(height * bottom),
    )


def points_in_rect(
    pixels: set[tuple[int, int]],
    rect: tuple[int, int, int, int],
) -> set[tuple[int, int]]:
    left, top, right, bottom = rect
    return {(x, y) for x, y in pixels if left <= x < right and top <= y < bottom}


def rock_pixels(pixels: set[tuple[int, int]], top_dx: int) -> set[tuple[int, int]]:
    if not pixels:
        return set()

    _, y0, _, y1 = bbox(pixels)
    height = max(1, y1 - y0 - 1)
    out: set[tuple[int, int]] = set()
    for x, y in pixels:
        vertical_weight = (y1 - 1 - y) / height
        dx = round(top_dx * vertical_weight)
        nx = x + dx
        if 0 <= nx < GRID:
            out.add((nx, y))
    return out


def idle_amplitude(name: str, family: str) -> int:
    if name == "egg" or name.startswith("child_"):
        return 2
    if family in {"vigor", "aura"}:
        return 3
    return 2


def blink_region(
    pixels: set[tuple[int, int]],
    rel_rect: tuple[float, float, float, float],
) -> set[tuple[int, int]]:
    region = points_in_rect(pixels, relative_rect(pixels, *rel_rect))
    if not region:
        return set(pixels)

    candidates: list[set[tuple[int, int]]] = []
    for component in connected_components(region):
        cx0, cy0, cx1, cy1 = bbox(component)
        comp_w = cx1 - cx0
        comp_h = cy1 - cy0
        if 1 <= len(component) <= 40 and comp_w <= 8 and comp_h <= 8:
            candidates.append(component)

    if not candidates:
        return set(pixels)

    out = set(pixels)
    for component in candidates:
        cx0, cy0, cx1, cy1 = bbox(component)
        center_x = (cx0 + cx1) // 2
        center_y = (cy0 + cy1) // 2
        half_width = max(1, min(3, (cx1 - cx0 + 1) // 2))
        out.difference_update(component)
        for x in range(center_x - half_width, center_x + half_width + 1):
            if 0 <= x < GRID and 0 <= center_y < GRID:
                out.add((x, center_y))
    return out


def shape_family(name: str) -> str:
    if name == "egg":
        return "egg"
    if name.startswith("child_"):
        return name[len("child_") :]
    if name.startswith("char_"):
        return name.split("_", 2)[1]
    return ""


def idle_pose(name: str, pixels: set[tuple[int, int]], frame: int) -> set[tuple[int, int]]:
    if frame in (0, 2):
        return set(pixels)

    family = shape_family(name)
    direction = -1 if frame == 1 else 1
    posed = rock_pixels(pixels, direction * idle_amplitude(name, family))

    if family == "mass":
        if frame == 1:
            return blink_region(posed, (0.02, 0.12, 0.42, 0.48))
        return posed
    if family == "wild":
        if frame == 3:
            return blink_region(posed, (0.18, 0.24, 0.68, 0.52))
        return posed
    if family == "mind":
        if frame == 1:
            return blink_region(posed, (0.18, 0.16, 0.82, 0.52))
        return posed
    return posed


def extract_cell(
    width: int,
    pixels: list[Pixel],
    bounds: tuple[int, int, int, int],
    threshold: int,
    target_size: int = ADULT_RENDER_SIZE,
) -> set[tuple[int, int]]:
    left, top, right, bottom = bounds
    mask = dark_mask(width, pixels, left, top, right, bottom, threshold)
    return render_64(mask, right - left, bottom - top, target_size)


def render_size_for(name: str) -> int:
    if name == "egg":
        return EGG_RENDER_SIZE
    if name.startswith("child_"):
        return CHILD_RENDER_SIZE
    return ADULT_RENDER_SIZE


def write_idle_frames(
    idle_dir: Path,
    static_sprites: dict[str, set[tuple[int, int]]],
) -> None:
    for name, pixels in static_sprites.items():
        for frame in range(4):
            final = idle_pose(name, pixels, frame)
            write_png_1bit(idle_dir / f"{name}_idle_{frame}.png", final)


def generate(
    source: Path,
    out_dir: Path,
    threshold: int,
    pure_source: Path,
    idle_dir: Path,
) -> None:
    width, height, pixels = read_png_rgba(source)
    vertical, horizontal = detect_grid_lines(width, height, pixels)

    static_sprites: dict[str, set[tuple[int, int]]] = {}
    for name, row, col in SPRITES:
        left = vertical[col] + 2
        right = vertical[col + 1] - 2
        top = horizontal[row] + 2
        bottom = horizontal[row + 1] - 2
        final = extract_cell(width, pixels, (left, top, right, bottom), threshold, render_size_for(name))
        static_sprites[name] = final
        write_png_1bit(out_dir / f"{name}.png", final)

    if pure_source.exists():
        pure_width, pure_height, pure_pixels = read_png_rgba(pure_source)
        pure_cells = detect_pure_cells(pure_width, pure_height, pure_pixels, len(PURE_SPRITES))
        for name, col in PURE_SPRITES:
            final = extract_cell(pure_width, pure_pixels, pure_cells[col], threshold)
            static_sprites[name] = final
            write_png_1bit(out_dir / f"{name}.png", final)

    write_idle_frames(idle_dir, static_sprites)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=DEFAULT_SOURCE)
    parser.add_argument("--pure-source", type=Path, default=DEFAULT_PURE_SOURCE)
    parser.add_argument("--out-dir", type=Path, default=DEFAULT_OUT_DIR)
    parser.add_argument("--idle-dir", type=Path, default=DEFAULT_IDLE_DIR)
    parser.add_argument("--threshold", type=int, default=150)
    args = parser.parse_args()

    generate(args.source, args.out_dir, args.threshold, args.pure_source, args.idle_dir)


if __name__ == "__main__":
    main()
