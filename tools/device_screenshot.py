#!/usr/bin/env python3
"""Save the running device's screen as a PNG.

The firmware serves GET /screenshot as a top-down RGB565 BMP of the live
framebuffer (see handleScreenshot in services/settings_web.cpp). This converts
it to PNG so display changes can be checked without a camera.

    python tools/device_screenshot.py 10.0.0.133 radar.png [scale]

Standard library only (zlib does the PNG deflate) so it runs anywhere.
"""
from __future__ import annotations

import struct
import sys
import urllib.request
import zlib


def bmp565_to_png(bmp: bytes, out_path: str, scale: int = 1) -> tuple[int, int]:
    if bmp[:2] != b"BM":
        raise ValueError("not a BMP")
    pixel_off = struct.unpack_from("<I", bmp, 10)[0]
    width = struct.unpack_from("<i", bmp, 18)[0]
    height_raw = struct.unpack_from("<i", bmp, 22)[0]
    bpp = struct.unpack_from("<H", bmp, 28)[0]
    if bpp != 16:
        raise ValueError(f"expected 16bpp RGB565, got {bpp}")

    top_down = height_raw < 0
    height = abs(height_raw)
    row_bytes = width * 2

    rows: list[bytes] = []
    for y in range(height):
        src_y = y if top_down else (height - 1 - y)
        px = struct.unpack_from(f"<{width}H", bmp, pixel_off + src_y * row_bytes)
        row = bytearray()
        for v in px:
            r5, g6, b5 = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
            row += bytes(((r5 * 255 + 15) // 31,
                          (g6 * 255 + 31) // 63,
                          (b5 * 255 + 15) // 31)) * scale
        for _ in range(scale):
            rows.append(bytes(row))

    def chunk(tag: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    out_w, out_h = width * scale, height * scale
    raw = b"".join(b"\x00" + r for r in rows)  # filter byte 0 per scanline
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", out_w, out_h, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 6))
           + chunk(b"IEND", b""))
    with open(out_path, "wb") as f:
        f.write(png)
    return out_w, out_h


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    host, out = sys.argv[1], sys.argv[2]
    scale = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    data = urllib.request.urlopen(f"http://{host}/screenshot", timeout=30).read()
    w, h = bmp565_to_png(data, out, scale)
    print(f"{len(data)} bytes -> {out} ({w}x{h})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
