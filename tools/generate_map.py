#!/usr/bin/env python3
"""Generate the radar's shaded-relief map underlay from open data.

Produces include/data/map_lookup.h: a small terrain raster (elevation band,
hillshade, water fraction — bilinearly sampled on device so it stays smooth at
every zoom) plus simplified vector polylines for major roads and rivers. The
extract covers the widest radar range (30 mi) with margin; like the towns
underlay, a radar center far from the extract simply draws nothing.

    python tools/generate_map.py --center 40.13,-105.18
    python tools/generate_map.py                # anchor from towns_lookup.h

The anchor is rounded to 0.1 deg (~11 km) before it is written to the header,
matching the privacy blur of the committed towns data.

Sources:
  Elevation: AWS Terrain Tiles (terrarium), https://registry.opendata.aws/terrain-tiles/
  Roads / water: OpenStreetMap via Overpass API (ODbL), https://overpass-api.de/
Tiles are cached in tools/map_tiles/; the Overpass response in tools/map_osm.json.
"""
from __future__ import annotations

import argparse
import json
import math
import re
import sys
import urllib.request
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "include" / "data" / "map_lookup.h"
TOWNS_HEADER = ROOT / "include" / "data" / "towns_lookup.h"
TILE_CACHE = Path(__file__).resolve().parent / "map_tiles"
OSM_CACHE_DIR = Path(__file__).resolve().parent / "map_osm"

USER_AGENT = "round-radar-native/1.0 (map underlay generator)"

# Must match geo::kKmPerDegreeLatitude on the device so the map registers with
# the aircraft and town layers.
KM_PER_DEG = 111.0

TERRARIUM_URL = "https://s3.amazonaws.com/elevation-tiles-prod/terrarium/{z}/{x}/{y}.png"
TERRARIUM_ZOOM = 10
OVERPASS_URLS = [
    "https://overpass-api.de/api/interpreter",
    "https://overpass.kumi.systems/api/interpreter",
]

# Grid: kCells x kCells cells of kCellKm, centered on the anchor. 208 * 0.5 km
# gives a 52 km half-extent — the 30 mi outer ring needs 48.3 km.
CELLS = 208
CELL_KM = 0.5
HALF_KM = CELLS * CELL_KM / 2.0
SUPERSAMPLE = 4  # water rasterization oversampling per cell

# Hillshade sun: standard cartographic NW light.
SUN_AZIMUTH_DEG = 315.0
SUN_ALTITUDE_DEG = 45.0
SLOPE_EXAGGERATION = 2.0

SIMPLIFY_TOL_KM = 0.06
VERT_QUANT = 32  # int16 vertex unit = 1/32 km (~31 m)

KIND_MOTORWAY = 0
KIND_PRIMARY = 1
KIND_RIVER = 2
KIND_SECONDARY = 3
KIND_STREAM = 4
KIND_RUNWAY = 5
ALL_KINDS = (KIND_MOTORWAY, KIND_PRIMARY, KIND_RIVER, KIND_SECONDARY, KIND_STREAM,
             KIND_RUNWAY)

# Secondary roads and creeks only draw at close ranges, so their extract can be
# much smaller than the full grid (which must feed the 30 mi view).
DETAIL_HALF_KM = 20.0


def fetch(url: str, data: bytes | None = None, timeout: int = 180) -> bytes:
    req = urllib.request.Request(url, data=data, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return resp.read()


def anchor_from_towns() -> tuple[float, float] | None:
    if not TOWNS_HEADER.exists():
        return None
    m = re.search(r"within \d+ km of (-?[\d.]+),(-?[\d.]+)", TOWNS_HEADER.read_text(encoding="utf-8"))
    if not m:
        return None
    return float(m.group(1)), float(m.group(2))


def parse_center(arg: str | None) -> tuple[float, float]:
    if arg:
        lat_s, lon_s = arg.split(",")
        return float(lat_s), float(lon_s)
    towns = anchor_from_towns()
    if towns:
        print(f"anchor from towns_lookup.h: {towns[0]:.1f},{towns[1]:.1f}")
        return towns
    sys.exit("no --center given and no committed towns anchor found")


# --- Elevation ---------------------------------------------------------------

def mercator_px(lat: float, lon: float, zoom: int) -> tuple[float, float]:
    n = 256 * (1 << zoom)
    x = (lon + 180.0) / 360.0 * n
    lat_r = math.radians(lat)
    y = (1.0 - math.log(math.tan(lat_r) + 1.0 / math.cos(lat_r)) / math.pi) / 2.0 * n
    return x, y


class TileMosaic:
    """Bilinear elevation sampler over a lazily fetched terrarium tile set."""

    def __init__(self, zoom: int):
        self.zoom = zoom
        self.tiles: dict[tuple[int, int], object] = {}
        TILE_CACHE.mkdir(exist_ok=True)

    def _tile(self, tx: int, ty: int):
        key = (tx, ty)
        if key not in self.tiles:
            cache = TILE_CACHE / f"{self.zoom}_{tx}_{ty}.png"
            if not cache.exists():
                print(f"  fetching tile {self.zoom}/{tx}/{ty}")
                cache.write_bytes(fetch(TERRARIUM_URL.format(z=self.zoom, x=tx, y=ty), timeout=60))
            self.tiles[key] = Image.open(cache).convert("RGB").load()
        return self.tiles[key]

    def _elev_at_px(self, px: int, py: int) -> float:
        tx, ox = divmod(px, 256)
        ty, oy = divmod(py, 256)
        r, g, b = self._tile(tx, ty)[ox, oy]
        return (r * 256.0 + g + b / 256.0) - 32768.0

    def elevation(self, lat: float, lon: float) -> float:
        x, y = mercator_px(lat, lon, self.zoom)
        x0, y0 = int(math.floor(x - 0.5)), int(math.floor(y - 0.5))
        fx, fy = (x - 0.5) - x0, (y - 0.5) - y0
        e00 = self._elev_at_px(x0, y0)
        e10 = self._elev_at_px(x0 + 1, y0)
        e01 = self._elev_at_px(x0, y0 + 1)
        e11 = self._elev_at_px(x0 + 1, y0 + 1)
        return (e00 * (1 - fx) * (1 - fy) + e10 * fx * (1 - fy)
                + e01 * (1 - fx) * fy + e11 * fx * fy)


def cell_center_km(gx: int, gy: int) -> tuple[float, float]:
    """East/north km of a cell center; row 0 is the NORTH edge."""
    east = -HALF_KM + (gx + 0.5) * CELL_KM
    north = HALF_KM - (gy + 0.5) * CELL_KM
    return east, north


def build_elevation(anchor: tuple[float, float]) -> list[list[float]]:
    lat0, lon0 = anchor
    kx = KM_PER_DEG * math.cos(math.radians(lat0))
    mosaic = TileMosaic(TERRARIUM_ZOOM)
    grid = [[0.0] * CELLS for _ in range(CELLS)]
    for gy in range(CELLS):
        for gx in range(CELLS):
            east, north = cell_center_km(gx, gy)
            grid[gy][gx] = mosaic.elevation(lat0 + north / KM_PER_DEG, lon0 + east / kx)
        if gy % 32 == 0:
            print(f"  elevation rows {gy}/{CELLS}")
    return grid


def build_hillshade(elev: list[list[float]]) -> list[list[int]]:
    """Horn hillshade, byte-encoded so 128 is flat ground (device multiplies by
    shade/128)."""
    zen = math.radians(90.0 - SUN_ALTITUDE_DEG)
    az = math.radians(SUN_AZIMUTH_DEG)
    cell_m = CELL_KM * 1000.0
    flat = math.cos(zen)
    shade = [[128] * CELLS for _ in range(CELLS)]
    for gy in range(1, CELLS - 1):
        for gx in range(1, CELLS - 1):
            e = elev
            dzdx = ((e[gy - 1][gx + 1] + 2 * e[gy][gx + 1] + e[gy + 1][gx + 1])
                    - (e[gy - 1][gx - 1] + 2 * e[gy][gx - 1] + e[gy + 1][gx - 1])) / (8 * cell_m)
            # Row index grows southward, so +gy is -north.
            dzdy = ((e[gy + 1][gx - 1] + 2 * e[gy + 1][gx] + e[gy + 1][gx + 1])
                    - (e[gy - 1][gx - 1] + 2 * e[gy - 1][gx] + e[gy - 1][gx + 1])) / (8 * cell_m)
            dzdx *= SLOPE_EXAGGERATION
            dzdy *= SLOPE_EXAGGERATION
            slope = math.atan(math.hypot(dzdx, dzdy))
            aspect = math.atan2(dzdy, -dzdx)
            hs = (math.cos(zen) * math.cos(slope)
                  + math.sin(zen) * math.sin(slope) * math.cos(az - aspect))
            hs = max(0.0, hs)
            shade[gy][gx] = max(30, min(230, round(128.0 * hs / flat)))
    # Edge rows copy their neighbours so the border does not ring.
    for gx in range(CELLS):
        shade[0][gx] = shade[1][gx]
        shade[CELLS - 1][gx] = shade[CELLS - 2][gx]
    for gy in range(CELLS):
        shade[gy][0] = shade[gy][1]
        shade[gy][CELLS - 1] = shade[gy][CELLS - 2]
    return shade


# --- OpenStreetMap -----------------------------------------------------------

def overpass_query(anchor: tuple[float, float]) -> dict:
    """Fetch roads, rivers and water bodies as separate cached queries — the
    combined request 504s on the public endpoints over a metro this size.
    Unnamed ponds are skipped: they are OSM noise at radar scale."""
    lat0, lon0 = anchor
    kx = KM_PER_DEG * math.cos(math.radians(lat0))

    def bbox_for(half_km: float) -> str:
        dlat = (half_km + 2.0) / KM_PER_DEG
        dlon = (half_km + 2.0) / kx
        return f"{lat0 - dlat},{lon0 - dlon},{lat0 + dlat},{lon0 + dlon}"

    bbox = bbox_for(HALF_KM)
    inner = bbox_for(DETAIL_HALF_KM)
    parts = {
        "roads": f'way["highway"~"^(motorway|trunk|primary)$"]({bbox});',
        "rivers": f'way["waterway"="river"]({bbox});',
        "water": (f'way["natural"="water"]["name"]({bbox});'
                  f'relation["natural"="water"]["name"]({bbox});'
                  f'way["landuse"="reservoir"]["name"]({bbox});'),
        # Close-zoom detail from the inner extract only.
        "roads_secondary": f'way["highway"="secondary"]({inner});',
        "streams": f'way["waterway"="stream"]["name"]({inner});',
        # Runways across the whole extract — this is an aircraft radar.
        "runways": f'way["aeroway"="runway"]({bbox});',
        # Unnamed ponds (gravel pits, small reservoirs): the perimeter filter
        # drops farm-pond noise server-side.
        "ponds": f'way["natural"="water"][!"name"]({bbox})(if:length()>800);',
    }
    OSM_CACHE_DIR.mkdir(exist_ok=True)
    elements: list[dict] = []
    for name, body in parts.items():
        cache = OSM_CACHE_DIR / f"{name}.json"
        if cache.exists():
            print(f"  using cached {cache.name}")
            elements.extend(json.loads(cache.read_text(encoding="utf-8"))["elements"])
            continue
        query = f"[out:json][timeout:300];({body});out geom;"
        last_err: Exception | None = None
        raw = None
        for url in OVERPASS_URLS:
            try:
                print(f"  querying {name} via {url} ...")
                raw = fetch(url, data=query.encode("utf-8"), timeout=320)
                break
            except Exception as err:  # noqa: BLE001 - try the mirror
                print(f"    failed: {err}")
                last_err = err
        if raw is None:
            sys.exit(f"all Overpass endpoints failed for {name}: {last_err}")
        cache.write_bytes(raw)
        elements.extend(json.loads(raw)["elements"])
    return {"elements": elements}


def to_km(anchor: tuple[float, float], lat: float, lon: float) -> tuple[float, float]:
    lat0, lon0 = anchor
    kx = KM_PER_DEG * math.cos(math.radians(lat0))
    return (lon - lon0) * kx, (lat - lat0) * KM_PER_DEG


def way_points_km(anchor: tuple[float, float], geometry: list[dict]) -> list[tuple[float, float]]:
    return [to_km(anchor, p["lat"], p["lon"]) for p in geometry]


def stitch_rings(members: list[list[tuple[float, float]]]) -> list[list[tuple[float, float]]]:
    """Join relation member ways end-to-end into closed rings (best effort)."""
    unused = [list(m) for m in members if len(m) >= 2]
    rings: list[list[tuple[float, float]]] = []
    while unused:
        ring = unused.pop()
        progress = True
        while progress and ring[0] != ring[-1]:
            progress = False
            for i, seg in enumerate(unused):
                if seg[0] == ring[-1]:
                    ring.extend(seg[1:])
                elif seg[-1] == ring[-1]:
                    ring.extend(reversed(seg[:-1]))
                elif seg[-1] == ring[0]:
                    ring[:0] = seg[:-1]
                elif seg[0] == ring[0]:
                    ring[:0] = list(reversed(seg[1:]))
                else:
                    continue
                unused.pop(i)
                progress = True
                break
        if ring[0] == ring[-1] and len(ring) >= 4:
            rings.append(ring)
    return rings


def stitch_paths(paths: list[list[tuple[float, float]]]) -> list[list[tuple[float, float]]]:
    """Join open polylines whose ends meet at a degree-2 point. OSM splits ways
    at every junction; without stitching the average road path is ~2 vertices
    and per-path overhead dominates the flash cost, while Douglas-Peucker never
    sees a run long enough to simplify."""

    def key(p: tuple[float, float]) -> tuple[int, int]:
        return round(p[0] * 1000), round(p[1] * 1000)

    ends: dict[tuple[int, int], list[tuple[int, int]]] = {}
    for i, pts in enumerate(paths):
        ends.setdefault(key(pts[0]), []).append((i, 0))
        ends.setdefault(key(pts[-1]), []).append((i, 1))

    used = [False] * len(paths)
    out: list[list[tuple[float, float]]] = []
    for i, pts in enumerate(paths):
        if used[i]:
            continue
        used[i] = True
        chain = list(pts)
        for direction in (1, 0):  # extend past the tail, then past the head
            while True:
                endpoint = chain[-1] if direction else chain[0]
                links = [(j, e) for j, e in ends.get(key(endpoint), []) if not used[j]]
                if len(links) != 1 or len(ends.get(key(endpoint), [])) != 2:
                    break
                j, end = links[0]
                used[j] = True
                seg = paths[j] if end == 0 else list(reversed(paths[j]))
                if direction:
                    chain.extend(seg[1:])
                else:
                    chain[:0] = list(reversed(seg))[:-1]
        out.append(chain)
    return out


def douglas_peucker(pts: list[tuple[float, float]], tol: float) -> list[tuple[float, float]]:
    if len(pts) < 3:
        return list(pts)
    keep = [False] * len(pts)
    keep[0] = keep[-1] = True
    stack = [(0, len(pts) - 1)]
    while stack:
        a, b = stack.pop()
        ax, ay = pts[a]
        bx, by = pts[b]
        dx, dy = bx - ax, by - ay
        seg_len_sq = dx * dx + dy * dy
        worst, worst_d = -1, tol
        for i in range(a + 1, b):
            px, py = pts[i]
            if seg_len_sq == 0:
                d = math.hypot(px - ax, py - ay)
            else:
                t = max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / seg_len_sq))
                d = math.hypot(px - (ax + t * dx), py - (ay + t * dy))
            if d > worst_d:
                worst, worst_d = i, d
        if worst >= 0:
            keep[worst] = True
            stack.append((a, worst))
            stack.append((worst, b))
    return [p for p, k in zip(pts, keep) if k]


def clip_to_extent(pts: list[tuple[float, float]], margin: float) -> list[list[tuple[float, float]]]:
    """Split a polyline into runs whose points are inside the square extent."""
    limit = HALF_KM + margin
    runs: list[list[tuple[float, float]]] = []
    cur: list[tuple[float, float]] = []
    for p in pts:
        if abs(p[0]) <= limit and abs(p[1]) <= limit:
            cur.append(p)
        elif cur:
            if len(cur) >= 2:
                runs.append(cur)
            cur = []
    if len(cur) >= 2:
        runs.append(cur)
    return runs


def build_water_raster(polys: list[tuple[list[tuple[float, float]], bool]]) -> list[list[int]]:
    """Water fraction per cell from supersampled polygon fill. polys entries are
    (ring, is_inner); inners (islands) punch holes after outers fill."""
    size = CELLS * SUPERSAMPLE
    img = Image.new("L", (size, size), 0)
    draw = ImageDraw.Draw(img)

    def to_px(ring: list[tuple[float, float]]) -> list[tuple[float, float]]:
        s = SUPERSAMPLE / CELL_KM
        return [((e + HALF_KM) * s, (HALF_KM - n) * s) for e, n in ring]

    for ring, is_inner in polys:
        if not is_inner and len(ring) >= 3:
            draw.polygon(to_px(ring), fill=255)
    for ring, is_inner in polys:
        if is_inner and len(ring) >= 3:
            draw.polygon(to_px(ring), fill=0)

    small = img.resize((CELLS, CELLS), Image.BOX)
    px = small.load()
    return [[px[gx, gy] for gx in range(CELLS)] for gy in range(CELLS)]


# --- Emission ----------------------------------------------------------------

def quantize_path(pts: list[tuple[float, float]]) -> list[tuple[int, int]]:
    out: list[tuple[int, int]] = []
    for e, n in pts:
        q = (round(e * VERT_QUANT), round(n * VERT_QUANT))
        if abs(q[0]) > 32767 or abs(q[1]) > 32767:
            continue
        if not out or q != out[-1]:
            out.append(q)
    return out


def fmt_bytes(rows: list[list[int]]) -> str:
    flat = [v for row in rows for v in row]
    lines = []
    for i in range(0, len(flat), 26):
        lines.append("    " + ",".join(str(v) for v in flat[i:i + 26]) + ",")
    return "\n".join(lines)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--center", help="lat,lon radar center (default: towns_lookup.h anchor)")
    args = ap.parse_args()

    lat, lon = parse_center(args.center)
    anchor = (round(lat * 10) / 10, round(lon * 10) / 10)  # same blur as towns
    print(f"anchor {anchor[0]:.1f},{anchor[1]:.1f}  extent ±{HALF_KM:.0f} km  grid {CELLS}x{CELLS}")

    print("elevation ...")
    elev = build_elevation(anchor)
    emin = min(min(r) for r in elev)
    emax = max(max(r) for r in elev)
    estep = max((emax - emin) / 255.0, 1.0)
    elev_q = [[max(0, min(255, round((v - emin) / estep))) for v in row] for row in elev]
    print(f"  range {emin:.0f}..{emax:.0f} m, step {estep:.1f} m")

    print("hillshade ...")
    shade = build_hillshade(elev)

    print("openstreetmap ...")
    osm = overpass_query(anchor)

    water_polys: list[tuple[list[tuple[float, float]], bool]] = []
    paths: list[tuple[int, list[tuple[float, float]]]] = []

    for el in osm.get("elements", []):
        tags = el.get("tags", {})
        if el["type"] == "way" and "geometry" in el:
            pts = way_points_km(anchor, el["geometry"])
            hw = tags.get("highway", "")
            if hw in ("motorway", "trunk"):
                paths.append((KIND_MOTORWAY, pts))
            elif hw == "primary":
                paths.append((KIND_PRIMARY, pts))
            elif hw == "secondary":
                paths.append((KIND_SECONDARY, pts))
            elif tags.get("waterway") == "river":
                paths.append((KIND_RIVER, pts))
            elif tags.get("waterway") == "stream":
                paths.append((KIND_STREAM, pts))
            elif tags.get("aeroway") == "runway" and tags.get("area") != "yes":
                paths.append((KIND_RUNWAY, pts))
            elif tags.get("natural") == "water" or tags.get("landuse") == "reservoir":
                water_polys.append((pts, False))
        elif el["type"] == "relation" and tags.get("natural") == "water":
            outers = [way_points_km(anchor, m["geometry"]) for m in el.get("members", [])
                      if m.get("role") == "outer" and "geometry" in m]
            inners = [way_points_km(anchor, m["geometry"]) for m in el.get("members", [])
                      if m.get("role") == "inner" and "geometry" in m]
            for ring in stitch_rings(outers):
                water_polys.append((ring, False))
            for ring in stitch_rings(inners):
                water_polys.append((ring, True))

    print(f"  {len(paths)} raw paths, {len(water_polys)} water rings")

    print("water raster ...")
    water = build_water_raster(water_polys)

    out_paths: list[tuple[int, list[tuple[int, int]]]] = []
    total_verts = 0
    for kind in ALL_KINDS:
        raw = [pts for k, pts in paths if k == kind and len(pts) >= 2]
        for chain in stitch_paths(raw):
            for run in clip_to_extent(chain, margin=1.0):
                q = quantize_path(douglas_peucker(run, SIMPLIFY_TOL_KM))
                if len(q) >= 2:
                    out_paths.append((kind, q))
                    total_verts += len(q)
    out_paths.sort(key=lambda p: p[0])
    if total_verts > 65535:
        sys.exit(f"vertex count {total_verts} overflows the uint16 path index")
    print(f"  {len(out_paths)} stitched paths, {total_verts} vertices")

    counts = {k: sum(1 for kk, _ in out_paths if kk == k) for k in ALL_KINDS}
    grid_bytes = CELLS * CELLS * 3
    vec_bytes = total_verts * 4 + len(out_paths) * 6
    print(f"  flash: grids ~{grid_bytes // 1024} KB, vectors ~{vec_bytes // 1024} KB")

    vert_lines = []
    path_lines = []
    first = 0
    for kind, q in out_paths:
        path_lines.append(f"    {{{first}, {len(q)}, {kind}}},")
        coords = ",".join(f"{{{x},{y}}}" for x, y in q)
        vert_lines.append(f"    {coords},")
        first += len(q)

    header = f"""#pragma once
// Auto-generated by tools/generate_map.py — do not edit.
// Shaded-relief underlay: {CELLS}x{CELLS} x {CELL_KM} km grid centered on
// {anchor[0]:.1f},{anchor[1]:.1f} (elevation, hillshade, water fraction), plus
// {counts[0]} motorway, {counts[1]} primary, {counts[3]} secondary,
// {counts[2]} river, {counts[4]} stream and {counts[5]} runway paths.
// Elevation: AWS Terrain Tiles. Roads/water: OpenStreetMap (ODbL).
// Regenerate for another region: python tools/generate_map.py --center LAT,LON

#include <cstddef>
#include <cstdint>

namespace data::map {{

constexpr bool kHasData = true;
constexpr float kAnchorLat = {anchor[0]:.4f}f;
constexpr float kAnchorLon = {anchor[1]:.4f}f;
constexpr int kCells = {CELLS};
constexpr float kCellKm = {CELL_KM}f;
constexpr float kHalfKm = {HALF_KM}f;
constexpr float kElevMinM = {emin:.1f}f;
constexpr float kElevStepM = {estep:.3f}f;

// Row 0 is the north edge; index = gy * kCells + gx.
constexpr uint8_t kElev[kCells * kCells] = {{
{fmt_bytes(elev_q)}
}};

// Hillshade multiplier byte: 128 = flat ground, device scales color by v/128.
constexpr uint8_t kShade[kCells * kCells] = {{
{fmt_bytes(shade)}
}};

// Water coverage fraction 0..255 per cell.
constexpr uint8_t kWater[kCells * kCells] = {{
{fmt_bytes(water)}
}};

// Vector overlays. Vertices are km east/north of the anchor, in 1/{VERT_QUANT} km.
enum : uint8_t {{
  kPathMotorway = 0,
  kPathPrimary = 1,
  kPathRiver = 2,
  kPathSecondary = 3,
  kPathStream = 4,
  kPathRunway = 5,
}};

struct PathVert {{
  int16_t east;
  int16_t north;
}};

struct Path {{
  uint16_t first;
  uint16_t count;
  uint8_t kind;
}};

constexpr size_t kPathVertCount = {total_verts};
constexpr PathVert kPathVerts[kPathVertCount] = {{
{chr(10).join(vert_lines)}
}};

constexpr size_t kPathCount = {len(out_paths)};
constexpr Path kPaths[kPathCount] = {{
{chr(10).join(path_lines)}
}};

}}  // namespace data::map
"""
    OUT.write_text(header, encoding="utf-8", newline="\n")
    print(f"wrote {OUT} ({OUT.stat().st_size // 1024} KB)")


if __name__ == "__main__":
    main()
