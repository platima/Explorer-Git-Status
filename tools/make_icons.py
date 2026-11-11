#!/usr/bin/env python3
"""Generates the overlay icons in ../icons using only the standard library.

Overlay icons are drawn over the full folder icon, so each image is mostly
transparent with a small coloured badge in the bottom-left corner (the same
place OneDrive draws its sync status).

Usage: python tools/make_icons.py
"""

import math
import os
import struct

SIZES = (16, 24, 32, 48, 64)
SUPERSAMPLE = 4

# (file name, RGB colour, glyph)
ICONS = (
    ("clean.ico", (0x2E, 0xA0, 0x43), "check"),
    ("modified.ico", (0xD7, 0x3A, 0x49), "dot"),
    ("aheadbehind.ico", (0x21, 0x6F, 0xD8), "arrows"),
)


def seg_dist(px, py, ax, ay, bx, by):
    dx, dy = bx - ax, by - ay
    t = ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy)
    t = max(0.0, min(1.0, t))
    return math.hypot(px - (ax + t * dx), py - (ay + t * dy))


def glyph_hit(glyph, x, y, stroke):
    """x, y are in badge-local coordinates, -1..1."""
    if glyph == "check":
        return (seg_dist(x, y, -0.45, 0.0, -0.12, 0.35) < stroke or
                seg_dist(x, y, -0.12, 0.35, 0.45, -0.3) < stroke)
    if glyph == "dot":
        return math.hypot(x, y) < 0.32
    if glyph == "arrows":
        # Up arrow on the left, down arrow on the right.
        up = (seg_dist(x, y, -0.25, 0.45, -0.25, -0.4) < stroke or
              seg_dist(x, y, -0.25, -0.45, -0.5, -0.15) < stroke or
              seg_dist(x, y, -0.25, -0.45, 0.0, -0.15) < stroke)
        down = (seg_dist(x, y, 0.25, -0.45, 0.25, 0.4) < stroke or
                seg_dist(x, y, 0.25, 0.45, 0.0, 0.15) < stroke or
                seg_dist(x, y, 0.25, 0.45, 0.5, 0.15) < stroke)
        return up or down
    raise ValueError(glyph)


def render(size, colour, glyph):
    """Returns rows of (r, g, b, a) top-down."""
    badge = max(9.0, size * 0.5)          # badge diameter in pixels
    radius = badge / 2.0
    cx, cy = radius, size - radius         # bottom-left corner
    border = max(1.0, size / 24.0)         # white ring so it shows on any icon
    stroke = 0.16 if size >= 32 else 0.2

    rows = []
    n = SUPERSAMPLE
    for py in range(size):
        row = []
        for px in range(size):
            r = g = b = a = 0.0
            for sy in range(n):
                for sx in range(n):
                    x = px + (sx + 0.5) / n
                    y = py + (sy + 0.5) / n
                    d = math.hypot(x - cx, y - cy)
                    if d > radius:
                        continue
                    if d > radius - border:
                        c = (255, 255, 255)
                    else:
                        inner = radius - border
                        lx, ly = (x - cx) / inner, (y - cy) / inner
                        c = (255, 255, 255) if glyph_hit(glyph, lx, ly, stroke) else colour
                    r += c[0]
                    g += c[1]
                    b += c[2]
                    a += 1
            samples = n * n
            if a:
                row.append((int(r / a), int(g / a), int(b / a), int(255 * a / samples)))
            else:
                row.append((0, 0, 0, 0))
        rows.append(row)
    return rows


def bmp_entry(size, rows):
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    pixels = bytearray()
    for row in reversed(rows):
        for r, g, b, a in row:
            pixels += bytes((b, g, r, a))
    mask_stride = ((size + 31) // 32) * 4
    mask = bytearray()
    for row in reversed(rows):
        line = bytearray(mask_stride)
        for x, (_, _, _, a) in enumerate(row):
            if a == 0:
                line[x // 8] |= 0x80 >> (x % 8)
        mask += line
    return header + bytes(pixels) + bytes(mask)


def write_ico(path, colour, glyph):
    images = [(s, bmp_entry(s, render(s, colour, glyph))) for s in SIZES]
    out = bytearray(struct.pack("<HHH", 0, 1, len(images)))
    offset = 6 + 16 * len(images)
    for size, data in images:
        out += struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    for _, data in images:
        out += data
    with open(path, "wb") as f:
        f.write(out)


def main():
    out_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "icons")
    os.makedirs(out_dir, exist_ok=True)
    for name, colour, glyph in ICONS:
        write_ico(os.path.join(out_dir, name), colour, glyph)
        print("wrote", os.path.normpath(os.path.join(out_dir, name)))


if __name__ == "__main__":
    main()
