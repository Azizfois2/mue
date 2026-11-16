#!/usr/bin/env python3
"""Build src/mue.ico from the mue app icon's pixel art.

The About button's chip (icon_about() in src/mue_icons.c) is the app icon, so
it must not be redrawn here: this script parses that function's 16x16 art and
the pix_art() palette straight out of the C source, then writes a multi-size
Windows icon.  Nearest-neighbour scaling keeps the pixel art crisp at every
size.

Usage:
    python re/make_icon.py
"""
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src", "mue_icons.c")
OUT = os.path.join(ROOT, "src", "mue.ico")

# Sizes Windows asks for, smallest first.  Explorer, the taskbar and the
# title bar each pick a different one.
SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]


def read_palette(text):
    """The pix_art() switch: case 'X': r = ..; g = ..; b = ..;"""
    pal = {}
    for m in re.finditer(
        r"case '(.)':\s*r\s*=\s*(\d+);\s*g\s*=\s*(\d+);\s*b\s*=\s*(\d+);",
        text,
    ):
        pal[m.group(1)] = (int(m.group(2)), int(m.group(3)), int(m.group(4)))
    if not pal:
        sys.exit("no palette found in %s" % SRC)
    return pal


def read_art(text, func):
    """Rows of the `static const char *const art[16]` inside `func`."""
    i = text.index(func)
    j = text.index("return pix_art(art);", i)
    rows = re.findall(r'"([^"]{16})"', text[i:j])
    if len(rows) != 16:
        sys.exit("%s: expected 16 rows, found %d" % (func, len(rows)))
    return rows


def render(rows, pal, size):
    """RGBA bytes, row-major top-down, nearest-neighbour from 16x16."""
    px = bytearray(size * size * 4)
    for y in range(size):
        sy = y * 16 // size
        for x in range(size):
            sx = x * 16 // size
            c = rows[sy][sx]
            o = (y * size + x) * 4
            if c == '.':
                # The icon's field: transparent, so the chip stands alone.
                px[o:o + 4] = b"\x00\x00\x00\x00"
            else:
                r, g, b = pal.get(c, (240, 240, 240))
                px[o] = r
                px[o + 1] = g
                px[o + 2] = b
                px[o + 3] = 255
    return bytes(px)


def dib(px, size):
    """A 32bpp ICO bitmap: BITMAPINFOHEADER, BGRA rows bottom-up, AND mask."""
    hdr = struct.pack(
        "<IiiHHIIiiII",
        40, size, size * 2, 1, 32, 0, size * size * 4, 0, 0, 0, 0,
    )
    body = bytearray()
    for y in range(size - 1, -1, -1):
        for x in range(size):
            o = (y * size + x) * 4
            r, g, b, a = px[o], px[o + 1], px[o + 2], px[o + 3]
            body += bytes((b, g, r, a))
    mask_stride = ((size + 31) // 32) * 4
    body += b"\x00" * (mask_stride * size)
    return hdr + bytes(body)


def main():
    text = open(SRC, encoding="utf-8").read()
    pal = read_palette(text)
    rows = read_art(text, "icon_about")
    entries = []
    for size in SIZES:
        px = render(rows, pal, size)
        entries.append((size, dib(px, size)))
    off = 6 + 16 * len(entries)
    out = bytearray(struct.pack("<HHH", 0, 1, len(entries)))
    for size, data in entries:
        out += struct.pack(
            "<BBBBHHII",
            0 if size >= 256 else size,
            0 if size >= 256 else size,
            0, 0, 1, 32, len(data), off,
        )
        off += len(data)
    for _, data in entries:
        out += data
    with open(OUT, "wb") as f:
        f.write(out)
    print("wrote %s (%d bytes, sizes %s)"
          % (OUT, len(out), ",".join(str(s) for s in SIZES)))


if __name__ == "__main__":
    main()
