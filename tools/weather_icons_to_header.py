#!/usr/bin/env python3
"""Convert weather icon PNGs to compressed GIF bytes in flash (weather-code lookup).

Source icons come from the OFFICIAL Tomorrow.io icon set:
  https://github.com/tomorrow-io-api/tomorrow-weather-codes  (V2_icons/large/png)

The V2 "large" PNGs ship in two resolutions: a 72px base and a 144px "@2x"
variant. We use the @2x art as the source (real detail, properly antialiased)
and downscale it to MAX_W x MAX_H with Lanczos, which looks far crisper on the
466x466 AMOLED than the old upscaled 64px LED-matrix pixel art.

Files are named "<code>_<description>_large@2x.png" (e.g. 10000_clear_large@2x.png),
where <code> is the 5-digit Tomorrow.io full-day weather code (weatherCode*10,
+1 at night) that the device already requests — so they drop straight into the
existing lookup.

Icons are flattened onto the on-device background colour before quant/save: GIF
has only 1-bit transparency, so blending the antialiased edges onto the known
background here (rather than keying a single colour at draw time) avoids the
fringing/halo that 1-bit alpha would otherwise leave around each icon.

Usage:
  python tools/weather_icons_to_header.py [--download] [SRC_DIR] [OUT_HEADER]

  --download   Fetch + extract the official repo zip into the cache first.
  SRC_DIR      Folder of PNGs to convert (defaults to the cached V2 large png).
"""

from __future__ import annotations

import io
import re
import sys
import urllib.request
import zipfile
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    print("Pillow required: pip install Pillow")
    sys.exit(1)

# On-screen icon size (downscaled from the 144px @2x source).
MAX_W = 96
MAX_H = 96

# Sunrise/sunset glyphs are smaller; native @2x source is 48px.
SUN_W = 40
SUN_H = 40

# Device background the icons are composited onto (radar_theme kBg*: dark green).
BG_R, BG_G, BG_B = 2, 15, 3

# Official Tomorrow.io icon set (codeload zip avoids the GitHub API rate limit).
REPO = "tomorrow-io-api/tomorrow-weather-codes"
REPO_BRANCH = "master"
ZIP_URL = f"https://codeload.github.com/{REPO}/zip/refs/heads/{REPO_BRANCH}"
# Relative path to the large PNGs inside the extracted archive.
V2_LARGE_PNG = Path("V2_icons") / "large" / "png"
# Sunrise/sunset glyphs live in their own folder (we use the "dark" variants).
V2_SUN_PNG = Path("V2_icons") / "small" / "sunset-sunrise" / "png"
SUN_FILES = {"sunrise": "sunrise-dark@2x.png", "sunset": "sunset-dark@2x.png"}

ROOT = Path(__file__).resolve().parent.parent
CACHE = ROOT / "tools" / ".cache" / "tomorrow-weather-codes"
DEFAULT_OUT = ROOT / "include" / "data" / "weather_icons_lookup.h"

# "10000_clear_large@2x.png" -> code 10000 (only the @2x large variants).
NAME_RE = re.compile(r"^(\d+)_.*_large@2x\.png$", re.IGNORECASE)


def http_get(url: str) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": "flightscnr-icons"})
    with urllib.request.urlopen(req, timeout=90) as resp:
        return resp.read()


def download_repo() -> Path | None:
    """Fetch + extract the official repo zip; return the V2 large png folder."""
    CACHE.mkdir(parents=True, exist_ok=True)
    print(f"Downloading {ZIP_URL} ...")
    try:
        blob = http_get(ZIP_URL)
    except Exception as exc:  # noqa: BLE001
        print(f"  download failed: {exc}")
        return None
    with zipfile.ZipFile(io.BytesIO(blob)) as zf:
        zf.extractall(CACHE)
    # The archive extracts to "<repo>-<branch>/...".
    for child in CACHE.iterdir():
        candidate = child / V2_LARGE_PNG
        if candidate.is_dir():
            print(f"Extracted icons to {candidate}")
            return candidate
    print("Could not find V2_icons/large/png in the archive.")
    return None


def find_src_dir() -> Path | None:
    """Locate a cached V2 large png folder without re-downloading."""
    if not CACHE.is_dir():
        return None
    for child in CACHE.iterdir():
        candidate = child / V2_LARGE_PNG
        if candidate.is_dir():
            return candidate
    return None


def parse_code(png: Path) -> int | None:
    m = NAME_RE.match(png.name)
    if not m:
        return None
    code = int(m.group(1))
    # The device only requests 5-digit full-day codes (weatherCode*10, +1 at
    # night), so 4-digit base-code icons would be dead weight in flash. Codes run
    # up to 80031 (thunderstorm), which is why the lookup stores them as uint32_t.
    if code < 10000 or code > 99999:
        return None
    return code


def to_gif_on_bg(png: Path, w: int = MAX_W, h: int = MAX_H) -> bytes:
    """Flatten the icon onto the device background and emit an optimized GIF."""
    image = Image.open(png).convert("RGBA")
    if image.size != (w, h):
        image = image.resize((w, h), Image.Resampling.LANCZOS)
    bg = Image.new("RGBA", image.size, (BG_R, BG_G, BG_B, 255))
    flat = Image.alpha_composite(bg, image).convert("RGB")
    byte_io = io.BytesIO()
    flat.save(byte_io, optimize=True, format="GIF")
    return byte_io.getvalue()


def emit_u8_array(name: str, data: bytes) -> str:
    lines: list[str] = []
    for i in range(0, len(data), 16):
        chunk = data[i : i + 16]
        parts = ", ".join(f"0x{b:02x}" for b in chunk)
        lines.append(f"  {parts}")
    body = ",\n".join(lines)
    return f"constexpr uint8_t {name}[] PROGMEM = {{\n{body}\n}};\n"


def main() -> int:
    args = [a for a in sys.argv[1:] if a != "--download"]
    want_download = "--download" in sys.argv[1:]

    if len(args) > 0:
        src_dir: Path | None = Path(args[0])
    else:
        src_dir = find_src_dir()
        if src_dir is None or want_download:
            src_dir = download_repo()
    out_path = Path(args[1]) if len(args) > 1 else DEFAULT_OUT

    if src_dir is None or not src_dir.is_dir():
        print(f"Skip weather icons: source dir not found ({src_dir})")
        return 0

    pngs = sorted(src_dir.glob("*.png"))
    entries: list[tuple[int, str]] = []
    blob_sources: list[str] = []
    blob_meta: dict[int, tuple[int, int, int]] = {}
    total_bytes = 0

    for png in pngs:
        code = parse_code(png)
        if code is None:
            continue
        gif = to_gif_on_bg(png)
        total_bytes += len(gif)
        sym = f"wx_icon_{code}"
        blob_sources.append(emit_u8_array(f"{sym}_gif", gif))
        blob_meta[code] = (MAX_W, MAX_H, len(gif))
        entries.append((code, sym))

    entries.sort(key=lambda e: e[0])

    # Sunrise/sunset glyphs (named blobs, not part of the weather-code lookup).
    sun_dir = src_dir.parent.parent / "small" / "sunset-sunrise" / "png"
    sun_blobs: list[tuple[str, bytes]] = []  # (symbol, gif)
    for sym, fname in SUN_FILES.items():
        path = sun_dir / fname
        if not path.is_file():
            print(f"  sun icon missing: {path}")
            continue
        gif = to_gif_on_bg(path, SUN_W, SUN_H)
        total_bytes += len(gif)
        sun_blobs.append((sym, gif))

    out_path.parent.mkdir(parents=True, exist_ok=True)
    with out_path.open("w", encoding="utf-8") as out:
        out.write("// Auto-generated by tools/weather_icons_to_header.py — do not edit.\n")
        out.write("// Icons (c) Tomorrow.io, CC BY 4.0 — Powered by Tomorrow.io.\n")
        out.write("#pragma once\n\n")
        out.write("#include <cstddef>\n#include <cstdint>\n\n")
        out.write("namespace data::weather_icons {\n\n")
        out.write(f"constexpr int kMaxIconW = {MAX_W};\n")
        out.write(f"constexpr int kMaxIconH = {MAX_H};\n\n")
        out.write("constexpr uint16_t kTransparentRgb565 = 0xF81F;\n\n")
        out.write("struct IconBlob {\n  uint16_t w;\n  uint16_t h;\n")
        out.write("  uint32_t size;\n  const uint8_t* gif;\n};\n\n")
        out.write("struct Entry {\n  uint32_t code;\n  const IconBlob* icon;\n};\n\n")

        if not entries:
            out.write("constexpr size_t kCount = 0;\n")
            out.write("constexpr Entry kEntries[] = {{0, nullptr}};\n\n")
            out.write("}  // namespace data::weather_icons\n")
            print(f"Wrote {out_path}: 0 icons (no PNGs found in {src_dir})")
            return 0

        for src in blob_sources:
            out.write(src)
            out.write("\n")
        for code, (w, h, size) in sorted(blob_meta.items()):
            out.write(
                f"constexpr IconBlob kBlob_{code} = "
                f"{{ {w}, {h}, {size}, wx_icon_{code}_gif }};\n"
            )
        out.write("\n")
        out.write(f"constexpr size_t kCount = {len(entries)};\n\n")
        out.write("constexpr Entry kEntries[] PROGMEM = {\n")
        for code, _sym in entries:
            out.write(f"  {{{code}, &kBlob_{code}}},\n")
        out.write("};\n\n")

        sun_syms = {sym for sym, _ in sun_blobs}
        have_sun = {"sunrise", "sunset"}.issubset(sun_syms)
        out.write(f"constexpr bool kHasSunIcons = {'true' if have_sun else 'false'};\n\n")
        if have_sun:
            for sym, gif in sun_blobs:
                out.write(emit_u8_array(f"wx_{sym}_gif", gif))
                out.write("\n")
            for sym, gif in sun_blobs:
                out.write(
                    f"constexpr IconBlob kBlob_{sym} = "
                    f"{{ {SUN_W}, {SUN_H}, {len(gif)}, wx_{sym}_gif }};\n"
                )
            out.write("\n")
        out.write("}  // namespace data::weather_icons\n")

    print(f"Wrote {out_path}: {len(entries)} icons ({total_bytes / 1024:.0f} KiB)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
