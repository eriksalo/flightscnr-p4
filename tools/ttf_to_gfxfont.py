#!/usr/bin/env python3
"""Convert a TrueType font to Adafruit GFXfont header (7-bit ASCII)."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

try:
    import freetype
except ImportError:
    print("Install freetype-py: pip install freetype-py", file=sys.stderr)
    raise

DPI = 141  # Adafruit fontconvert default


def sanitize_name(path: Path, size: int, last: int) -> str:
    stem = re.sub(r"[^0-9A-Za-z]", "", path.stem)
    bits = 8 if last > 127 else 7
    return f"{stem}{size}pt{bits}b"


def enbit(value: int, state: dict) -> None:
    if value:
        state["sum"] |= state["bit"]
    state["bit"] >>= 1
    if state["bit"] == 0:
        if not state["first"]:
            if state["row"] >= 11:
                state["out"].append(",\n  ")
                state["row"] = 0
            else:
                state["out"].append(", ")
        state["out"].append(f"0x{state['sum']:02X}")
        state["sum"] = 0
        state["bit"] = 0x80
        state["first"] = False
        state["row"] += 1


def flush_bits(state: dict) -> None:
    if state["bit"] != 0x80:
        enbit(0, state)
        while state["bit"] != 0x80:
            enbit(0, state)


def convert(ttf: Path, size: int, first: int = ord(" "), last: int = ord("~")) -> str:
    face = freetype.Face(str(ttf))
    face.set_char_size(size * 64, 0, DPI, 0)

    font_name = sanitize_name(ttf, size, last)
    glyphs: list[dict] = []
    bitmap_offset = 0
    bit_state = {"out": [], "sum": 0, "bit": 0x80, "row": 0, "first": True}

    for code in range(first, last + 1):
        face.load_char(code, freetype.FT_LOAD_TARGET_MONO)
        face.glyph.render(freetype.FT_RENDER_MODE_MONO)
        bitmap = face.glyph.bitmap
        left = face.glyph.bitmap_left
        top = face.glyph.bitmap_top
        advance = face.glyph.advance.x >> 6

        glyphs.append(
            {
                "bitmapOffset": bitmap_offset,
                "width": bitmap.width,
                "height": bitmap.rows,
                "xAdvance": advance,
                "xOffset": left,
                "yOffset": 1 - top,
            }
        )

        pitch = bitmap.pitch
        buf = bitmap.buffer
        for y in range(bitmap.rows):
            for x in range(bitmap.width):
                byte = x // 8
                bit = 0x80 >> (x & 7)
                idx = y * pitch + byte
                val = buf[idx] if isinstance(buf[idx], int) else ord(buf[idx])
                enbit(1 if val & bit else 0, bit_state)

        n = (bitmap.width * bitmap.rows) & 7
        if n:
            for _ in range(8 - n):
                enbit(0, bit_state)

        bitmap_offset += (bitmap.width * bitmap.rows + 7) // 8

    flush_bits(bit_state)
    bitmap_lines = "".join(bit_state["out"])

    y_advance = face.size.height >> 6
    if y_advance == 0 and glyphs:
        y_advance = glyphs[0]["height"]

    lines = [
        "#pragma once",
        "",
        "const uint8_t "
        + font_name
        + "Bitmaps[] PROGMEM = {\n  "
        + bitmap_lines
        + " };",
        "",
        f"const GFXglyph {font_name}Glyphs[] PROGMEM = {{",
    ]

    for i, g in enumerate(glyphs):
        code = first + i
        comment = ""
        if code <= last:
            suffix = f"   // 0x{code:02X}"
            if 32 <= code <= 126:
                suffix += f" '{chr(code)}'"
            comment = suffix if code < last else ""
        comma = "," if code < last else ""
        lines.append(
            f"  {{ {g['bitmapOffset']:5d}, {g['width']:3d}, {g['height']:3d}, "
            f"{g['xAdvance']:3d}, {g['xOffset']:4d}, {g['yOffset']:4d} }}"
            f"{comma}{comment}"
        )

    lines.append(f" }}; // 0x{last:02X}")
    if 32 <= last <= 126:
        lines[-1] += f" '{chr(last)}'"
    lines += [
        "",
        f"const GFXfont {font_name} PROGMEM = {{",
        f"  (uint8_t  *){font_name}Bitmaps,",
        f"  (GFXglyph *){font_name}Glyphs,",
        f"  0x{first:02X}, 0x{last:02X}, {y_advance} }};",
        "",
        f"// Approx. {bitmap_offset + (last - first + 1) * 7 + 7} bytes",
        "",
    ]
    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser(description="TTF to GFXfont header")
    parser.add_argument("ttf", type=Path)
    parser.add_argument("size", type=int, help="Font size in points")
    parser.add_argument("-o", "--output", type=Path, required=True)
    args = parser.parse_args()

    header = convert(args.ttf, args.size)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(header, encoding="utf-8")
    print(f"Wrote {args.output} ({args.output.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
