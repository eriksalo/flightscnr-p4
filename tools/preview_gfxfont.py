#!/usr/bin/env python3
"""Preview a generated GFXfont header as ASCII art, before trusting it on glass.

Parses the exact bitmap/glyph tables the firmware will draw, so what you see is
the real on-device pixels rather than a re-rasterization. Use it after
ttf_to_gfxfont.py to check a small size is actually legible — 3pt Montserrat
Bold, for example, collapses 'A' and the digits into each other at 5px.

    python tools/preview_gfxfont.py include/fonts/MontserratBold4pt7b.h "N735RB 12,500ft"
"""
import re
import sys


def parse(path):
    src = open(path, encoding='utf-8').read()
    bm = re.search(r'Bitmaps\[\]\s*PROGMEM\s*=\s*\{(.*?)\};', src, re.S).group(1)
    bitmaps = [int(x, 16) for x in re.findall(r'0x([0-9A-Fa-f]{2})', bm)]
    gl = re.search(r'Glyphs\[\]\s*PROGMEM\s*=\s*\{(.*?)\};', src, re.S).group(1)
    glyphs = []
    for m in re.finditer(r'\{\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*\}', gl):
        off, w, h, xadv, dx, dy = (int(g) for g in m.groups())
        glyphs.append(dict(off=off, w=w, h=h, xadv=xadv, dx=dx, dy=dy))
    tail = re.search(r'0x20,\s*0x7E,\s*(\d+)', src)
    yadv = int(tail.group(1))
    return bitmaps, glyphs, yadv


def render(path, text):
    bitmaps, glyphs, yadv = parse(path)
    # canvas tall enough for ascender+descender
    top, height = -yadv, yadv + 4
    width = sum(glyphs[ord(c) - 0x20]['xadv'] for c in text) + 4
    grid = [[' '] * width for _ in range(height)]
    pen = 1
    for c in text:
        g = glyphs[ord(c) - 0x20]
        bit = g['off'] * 8
        for row in range(g['h']):
            for col in range(g['w']):
                byte = bitmaps[bit >> 3] if (bit >> 3) < len(bitmaps) else 0
                on = (byte >> (7 - (bit & 7))) & 1
                bit += 1
                if on:
                    y = row + g['dy'] - top
                    x = pen + col + g['dx']
                    if 0 <= y < height and 0 <= x < width:
                        grid[y][x] = '#'
        pen += g['xadv']
    lines = [''.join(r) for r in grid]
    while lines and not lines[0].strip():
        lines.pop(0)
    while lines and not lines[-1].strip():
        lines.pop()
    print(f"--- {path.split('/')[-1]}  yAdvance={yadv}px  rendered_h={len(lines)}px  w={pen}px")
    for l in lines:
        print('|' + l.rstrip())


if __name__ == '__main__':
    sample = sys.argv[2] if len(sys.argv) > 2 else 'UAL1234'
    render(sys.argv[1], sample)
