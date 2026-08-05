#!/usr/bin/env python3
"""Repack the PicoType font blob into the fixed-stride tables menu.cpp needs.

Source data
-----------
The glyphs come from Cubic 11 (俐方體十一號), an 11x11 pixel font, rasterised at
size 12 by https://github.com/pondahai/ime-charset-font-bitmap and shipped as
``picotype_data_optimized.h`` in
https://github.com/pondahai/pico_keyboard_ime_terminal_usb_host

That header stores one *byte* per pixel (values are only ever 0x00 or 0xFF --
the ">128" test in its renderer never sees anything else), which costs 1.24 MB.
Repacking to one *bit* per pixel is lossless and gets it under 200 KB, which is
what makes it fit in this project's 512 KB program region alongside InfoNES.

Output format
-------------
Two tables, both fixed-stride so that the scanline rasteriser can address a row
with plain arithmetic. menu.cpp redraws every cell of every scanline of every
frame, so a per-cell binary search would be far too slow -- codepoints are
resolved to a glyph index once, when the text changes, and the inner loop only
ever does ``base + index * ROWS + row``.

  cjk_bitmap[]   uint16 per row, CJK_GLYPH_ROWS rows per glyph. Bit 0 is the
                 leftmost pixel, matching the ``slice & 1; slice >>= 1`` order
                 the existing 8x8 path already uses.
  cjk_index[]    sorted uint16 codepoints, parallel to cjk_bitmap. Binary
                 searched by putText().
  ascii_bitmap[] uint8 per row, ASCII_GLYPH_ROWS rows per glyph, indexed
                 directly by (codepoint - 32).

Each glyph's y_offset is baked in at pack time, so every stored row maps
straight onto a cell row and the renderer needs no per-glyph metrics at all.

Usage
-----
    python3 make_cjk_font.py <path to picotype_data_optimized.h> \\
                             [-o ../infones/font_cjk.h]
"""

import argparse
import pathlib
import re
import struct
import sys

# Layout of the source header's FontMapRecord_Opt (packed, 14 bytes).
REC = struct.Struct("<IIBBBbbB")
MAP_SYMBOL = "font_map_raw_opt"
BMP_SYMBOL = "font_bitmap_data_opt"

# The source font is Traditional Chinese and does not cover every codepoint the
# charset asked for. Characters it lacks were rasterised as a 2x2 dot in a 13x5
# box -- a notdef placeholder, not a glyph. Dropping them saves ~80 KB and lets
# the renderer draw a proper missing-character box instead.
#
# Matched on the exact signature rather than on ink count: ' and ; are real
# glyphs that also happen to have exactly four lit pixels.
PLACEHOLDER = dict(ink=4, bbox=(2, 2), origin=(5, 0), size=(13, 5))

# Cell geometry. A cell is 8 px wide and CELL_HEIGHT tall; a wide glyph spans
# two cells.
#
# Rows are stored tight against row 0 -- the vertical centring nudge is applied
# by the renderer as a constant, not baked in here. Baking it in would cost an
# extra stored row for every glyph (~15 KB) to hold padding that is always zero.
CELL_HEIGHT = 16
GLYPH_TOP = 0
CELL_WIDTH = 8

# What the renderer should add to a stored row index to centre the text in the
# cell. Emitted as a #define; free at draw time.
GLYPH_YSHIFT = 2

# Cubic 11 draws M, W and w 10 px wide (9 inked columns), which cannot survive
# being clipped to an 8 px cell -- the right-hand stem is what gets lost. These
# are hand-drawn 8 px replacements in the same style. Rows are top-to-bottom and
# are placed using the original glyph's y_offset.
NARROW_OVERRIDES = {
    ord("M"): [
        "#......#",
        "##....##",
        "#.#..#.#",
        "#..##..#",
        "#......#",
        "#......#",
        "#......#",
        "#......#",
        "#......#",
    ],
    ord("W"): [
        "#.....#.",
        "#.....#.",
        "#.....#.",
        "#..#..#.",
        "#..#..#.",
        "#.#.#.#.",
        "#.#.#.#.",
        ".#...#..",
        ".#...#..",
    ],
    ord("w"): [
        "#.....#.",
        "#.....#.",
        "#..#..#.",
        "#.#.#.#.",
        "#.#.#.#.",
        ".#...#..",
    ],
}


def read_blob(src, symbol, size):
    """Pull one ``const uint8_t sym[size] PROGMEM = {...}`` array out of the header.

    Located with str.find rather than a regex: a non-greedy ``(.*?)`` across a
    9 MB file degrades to near-quadratic and takes minutes.
    """
    marker = "%s[%d] PROGMEM = {" % (symbol, size)
    try:
        start = src.index(marker)
    except ValueError:
        sys.exit("error: %s[%d] not found -- is this the right header?" % (symbol, size))
    start = src.index("{", start) + 1
    end = src.index("};", start)
    blob = bytes(int(h, 16) for h in re.findall(r"0x([0-9a-fA-F]{2})", src[start:end]))
    if len(blob) != size:
        sys.exit("error: %s decoded to %d bytes, expected %d" % (symbol, len(blob), size))
    return blob


def array_size(src, symbol):
    m = re.search(re.escape(symbol) + r"\[(\d+)\] PROGMEM", src)
    if not m:
        sys.exit("error: could not find declaration of %s" % symbol)
    return int(m.group(1))


def is_placeholder(bitmap, rec):
    offset, width, height = rec[1], rec[2], rec[3]
    if (width, height) != PLACEHOLDER["size"]:
        return False
    lit = [(i % width, i // width) for i in range(width * height) if bitmap[offset + i]]
    if len(lit) != PLACEHOLDER["ink"]:
        return False
    xs = [x for x, _ in lit]
    ys = [y for _, y in lit]
    bbox = (max(xs) - min(xs) + 1, max(ys) - min(ys) + 1)
    return bbox == PLACEHOLDER["bbox"] and (min(xs), min(ys)) == PLACEHOLDER["origin"]


def glyph_rows(bitmap, rec, cell_width):
    """Return the glyph as a list of (row_in_cell, bitmask), bit 0 = leftmost."""
    offset, width, height, _, x_offset, y_offset = rec[1], rec[2], rec[3], rec[4], rec[5], rec[6]
    rows = []
    for y in range(height):
        mask = 0
        for x in range(width):
            if bitmap[offset + y * width + x]:
                px = x + x_offset
                if 0 <= px < cell_width:
                    mask |= 1 << px
        rows.append((y + y_offset + GLYPH_TOP, mask))
    return rows


def override_rows(art, y_offset):
    return [
        (y + y_offset + GLYPH_TOP, sum(1 << x for x, c in enumerate(line) if c == "#"))
        for y, line in enumerate(art)
    ]


def emit_array(out, decl, values, per_line, fmt):
    out.append(decl + " = {")
    for i in range(0, len(values), per_line):
        out.append("    " + "".join(fmt % v + "," for v in values[i : i + per_line]))
    out.append("};")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("source", type=pathlib.Path, help="picotype_data_optimized.h")
    ap.add_argument(
        "-o",
        "--output",
        type=pathlib.Path,
        default=pathlib.Path(__file__).resolve().parent.parent / "infones" / "font_cjk.h",
    )
    args = ap.parse_args()

    src = args.source.read_text(errors="ignore")
    map_raw = read_blob(src, MAP_SYMBOL, array_size(src, MAP_SYMBOL))
    bitmap = read_blob(src, BMP_SYMBOL, array_size(src, BMP_SYMBOL))
    records = [REC.unpack_from(map_raw, i * REC.size) for i in range(len(map_raw) // REC.size)]

    wide = {}
    ascii_glyphs = {}
    dropped = 0
    for rec in records:
        codepoint = rec[0]
        if is_placeholder(bitmap, rec):
            dropped += 1
            continue
        if codepoint > 0xFFFF:
            dropped += 1  # index is uint16; the source has none of these that matter
            continue
        if 0x20 <= codepoint < 0x7F:
            if codepoint in NARROW_OVERRIDES:
                ascii_glyphs[codepoint] = override_rows(NARROW_OVERRIDES[codepoint], rec[6])
            else:
                ascii_glyphs[codepoint] = glyph_rows(bitmap, rec, CELL_WIDTH)
        else:
            wide[codepoint] = glyph_rows(bitmap, rec, CELL_WIDTH * 2)

    if not wide:
        sys.exit("error: no wide glyphs survived -- check the source header")

    cjk_rows = max(row for g in wide.values() for row, _ in g) + 1
    ascii_rows = max(row for g in ascii_glyphs.values() for row, _ in g) + 1
    tallest = max(cjk_rows, ascii_rows) + GLYPH_YSHIFT
    if tallest > CELL_HEIGHT:
        sys.exit(
            "error: %d rows + %d shift overflows a %d row cell"
            % (max(cjk_rows, ascii_rows), GLYPH_YSHIFT, CELL_HEIGHT)
        )

    codepoints = sorted(wide)
    cjk_data = []
    for codepoint in codepoints:
        packed = [0] * cjk_rows
        for row, mask in wide[codepoint]:
            packed[row] = mask
        cjk_data.extend(packed)

    ascii_data = []
    for codepoint in range(0x20, 0x7F):
        packed = [0] * ascii_rows
        for row, mask in ascii_glyphs.get(codepoint, []):
            packed[row] = mask
        ascii_data.extend(packed)

    out = [
        "// Generated by software/tools/make_cjk_font.py -- DO NOT EDIT.",
        "//",
        "// Glyphs are Cubic 11 (俐方體十一號), an 11x11 pixel font, rasterised at size 12.",
        "// Pipeline: https://github.com/pondahai/ime-charset-font-bitmap",
        "// Repacked losslessly from 1 byte/pixel to 1 bit/pixel; see the script for why.",
        "//",
        "// Bit 0 of every row is the leftmost pixel. Each glyph's y_offset is already",
        "// baked into its row positions, so a stored row index is a cell row index.",
        "",
        "#pragma once",
        "#include <stdint.h>",
        "",
        "#define CJK_CELL_HEIGHT   %d" % CELL_HEIGHT,
        "#define CJK_CELL_WIDTH    %d" % CELL_WIDTH,
        "#define CJK_GLYPH_ROWS    %d" % cjk_rows,
        "#define CJK_GLYPH_COUNT   %d" % len(codepoints),
        "#define CJK_GLYPH_YSHIFT  %d" % GLYPH_YSHIFT,
        "#define ASCII_GLYPH_ROWS  %d" % ascii_rows,
        "#define ASCII_FIRST       0x20",
        "#define ASCII_COUNT       95",
        "",
    ]
    emit_array(
        out,
        "static const uint16_t cjk_index[CJK_GLYPH_COUNT]",
        codepoints,
        12,
        "0x%04x",
    )
    out.append("")
    emit_array(
        out,
        "static const uint16_t cjk_bitmap[CJK_GLYPH_COUNT * CJK_GLYPH_ROWS]",
        cjk_data,
        12,
        "0x%04x",
    )
    out.append("")
    emit_array(
        out,
        "static const uint8_t ascii_bitmap[ASCII_COUNT * ASCII_GLYPH_ROWS]",
        ascii_data,
        16,
        "0x%02x",
    )
    out.append("")
    args.output.write_text("\n".join(out))

    cjk_bytes = len(cjk_data) * 2
    ascii_bytes = len(ascii_data)
    index_bytes = len(codepoints) * 2
    print("dropped %d placeholder/out-of-range glyphs" % dropped)
    print("wide glyphs : %5d x %2d rows x 2 B = %6.1f KB" % (len(codepoints), cjk_rows, cjk_bytes / 1024))
    print("index       : %5d x  2 B           = %6.1f KB" % (len(codepoints), index_bytes / 1024))
    print("ascii       : %5d x %2d rows x 1 B = %6.1f KB" % (95, ascii_rows, ascii_bytes / 1024))
    print("total                               = %6.1f KB" % ((cjk_bytes + index_bytes + ascii_bytes) / 1024))
    print("wrote %s" % args.output)


if __name__ == "__main__":
    main()
