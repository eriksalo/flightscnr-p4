#include "ui/map_underlay.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "data/map_lookup.h"
#include "geo/flat_earth.h"
#include "hardware/plane_gfx.h"
#include "services/map_center.h"
#include "ui/radar_scale.h"
#include "ui/radar_theme.h"

namespace ui::map_underlay {

namespace {

/** Hypsometric ramp for the terrain fill, kept dim so the scope instrument
 *  stays on top: dark plains rising through olive foothills and gray montane
 *  slopes to pale alpine rock. Elevations in meters; colors lerp between
 *  stops and clamp beyond the ends. */
struct HypsoStop {
  float elev_m;
  uint8_t r, g, b;
};

constexpr HypsoStop kHypsoStops[] = {
    {1200.0f, 5, 17, 8},      // plains floor, just above the scope background
    {1650.0f, 11, 24, 11},
    {1950.0f, 20, 30, 15},    // foothills olive
    {2450.0f, 31, 35, 21},    // montane umber
    {3050.0f, 42, 46, 40},    // subalpine gray
    {3600.0f, 55, 59, 60},
    {4050.0f, 74, 78, 84},    // alpine rock
    {4450.0f, 95, 100, 108},  // snowcap hint
};
constexpr int kHypsoStopCount = sizeof(kHypsoStops) / sizeof(kHypsoStops[0]);

constexpr uint8_t kWaterR = 13, kWaterG = 39, kWaterB = 55;

/** Vector stroke colors / half-widths (drawWideLine units). */
constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
constexpr uint16_t kColorRiver = rgb565(18, 50, 70);
constexpr uint16_t kColorStream = rgb565(14, 42, 58);
constexpr uint16_t kColorMotorway = rgb565(56, 60, 66);
constexpr uint16_t kColorPrimary = rgb565(37, 41, 46);
constexpr uint16_t kColorSecondary = rgb565(27, 31, 36);
constexpr uint16_t kColorRunway = rgb565(104, 110, 120);
constexpr float kRiverHalfWidth = 0.9f;
constexpr float kStreamHalfWidth = 0.6f;
constexpr float kMotorwayHalfWidth = 1.1f;
constexpr float kPrimaryHalfWidth = 0.6f;
constexpr float kSecondaryHalfWidth = 0.5f;
constexpr float kRunwayHalfWidth = 1.7f;

/** Detail layers only appear zoomed in: at metro-wide ranges the arterial
 *  street grid and every creek read as noise (and cost thousands of paths per
 *  background rebuild). Motorways, trunks, rivers and runways carry the
 *  orientation at every range. */
constexpr float kPrimaryMaxRangeKm = 20.0f;   // 10 mi and closer
constexpr float kDetailMaxRangeKm = 13.5f;    // 8 mi and closer

/** Terrain fills the scope out to the outer grid ring, fading to the plain
 *  background over the last pixels so the dashed ring sits on quiet ground.
 *  Vectors clip a little short of the fade so they never poke into the rim. */
constexpr int kTerrainRadiusPx = radar::kGridOuterRadius;
constexpr int kFadeStartPx = kTerrainRadiusPx - 26;
constexpr float kVectorClipRadiusPx = static_cast<float>(kFadeStartPx + 8);

/** 4x4 Bayer matrix: the dark ramp spans few RGB565 steps, so smooth terrain
 *  would band into contour rings without ordered dither. */
constexpr int8_t kBayer4[4][4] = {
    {0, 8, 2, 10},
    {12, 4, 14, 6},
    {3, 11, 1, 9},
    {15, 7, 13, 5},
};

struct Rgb {
  uint8_t r, g, b;
};

Rgb s_hypso_lut[256];
bool s_lut_ready = false;

uint16_t s_row[radar::kSize];

void buildHypsoLut() {
  if (s_lut_ready) {
    return;
  }
  for (int q = 0; q < 256; ++q) {
    const float elev = data::map::kElevMinM + static_cast<float>(q) * data::map::kElevStepM;
    const HypsoStop* lo = &kHypsoStops[0];
    const HypsoStop* hi = &kHypsoStops[kHypsoStopCount - 1];
    for (int i = 0; i < kHypsoStopCount - 1; ++i) {
      if (elev >= kHypsoStops[i].elev_m && elev <= kHypsoStops[i + 1].elev_m) {
        lo = &kHypsoStops[i];
        hi = &kHypsoStops[i + 1];
        break;
      }
    }
    float t = 0.0f;
    if (elev <= lo->elev_m) {
      t = 0.0f;
    } else if (elev >= hi->elev_m) {
      t = 1.0f;
    } else {
      t = (elev - lo->elev_m) / (hi->elev_m - lo->elev_m);
    }
    s_hypso_lut[q].r = static_cast<uint8_t>(lroundf(lo->r + (hi->r - lo->r) * t));
    s_hypso_lut[q].g = static_cast<uint8_t>(lroundf(lo->g + (hi->g - lo->g) * t));
    s_hypso_lut[q].b = static_cast<uint8_t>(lroundf(lo->b + (hi->b - lo->b) * t));
  }
  s_lut_ready = true;
}

/** Radar center in km east/north of the map anchor. */
void centerOffsetKm(float* east, float* north) {
  geo::localOffsetKm(data::map::kAnchorLat, data::map::kAnchorLon,
                     static_cast<float>(services::map_center::latitude()),
                     static_cast<float>(services::map_center::longitude()), east, north,
                     nullptr);
}

/** One bilinear tap on a kCells x kCells byte grid at 16.16 fixed-point cell
 *  coordinates (cell-center space). Returns -1 outside the grid. */
int sampleGrid(const uint8_t* grid, int32_t u_fp, int32_t v_fp) {
  const int32_t ix = u_fp >> 16;
  const int32_t iy = v_fp >> 16;
  if (ix < 0 || iy < 0 || ix >= data::map::kCells - 1 || iy >= data::map::kCells - 1) {
    return -1;
  }
  const uint32_t fx = (static_cast<uint32_t>(u_fp) >> 8) & 0xFF;
  const uint32_t fy = (static_cast<uint32_t>(v_fp) >> 8) & 0xFF;
  const uint8_t* p = grid + iy * data::map::kCells + ix;
  const uint32_t v00 = p[0];
  const uint32_t v10 = p[1];
  const uint32_t v01 = p[data::map::kCells];
  const uint32_t v11 = p[data::map::kCells + 1];
  const uint32_t top = v00 * (256 - fx) + v10 * fx;
  const uint32_t bot = v01 * (256 - fx) + v11 * fx;
  return static_cast<int>((top * (256 - fy) + bot * fy) >> 16);
}

void drawTerrain(PlaneGfx& gfx) {
  buildHypsoLut();

  const float outer_km = radar::scaleActive().label_km;
  if (outer_km <= 0.0f) {
    return;
  }
  const float px_per_km = static_cast<float>(radar::kGridOuterRadius) / outer_km;
  const float km_per_px = 1.0f / px_per_km;

  const float facing = static_cast<float>(radar::facingDeg()) * 3.14159265f / 180.0f;
  const float fc = cosf(facing);
  const float fs = sinf(facing);

  float ce = 0.0f;
  float cn = 0.0f;
  centerOffsetKm(&ce, &cn);

  // Screen px -> geographic km (inverse of rotateEastNorthForFacing), then to
  // grid cell-center coordinates. Walking +1 px along a row advances the grid
  // position by a constant step, tracked in 16.16 fixed point.
  const float cell = data::map::kCellKm;
  const float du_px = km_per_px * fc / cell;
  const float dv_px = km_per_px * fs / cell;
  const int32_t du_fp = static_cast<int32_t>(lroundf(du_px * 65536.0f));
  const int32_t dv_fp = static_cast<int32_t>(lroundf(dv_px * 65536.0f));

  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int radius = kTerrainRadiusPx;
  const int fade_start_sq = kFadeStartPx * kFadeStartPx;
  const float inv_fade_len = 1.0f / static_cast<float>(radius - kFadeStartPx);
  const int bg_r = radar::kBgR;
  const int bg_g = radar::kBgG;
  const int bg_b = radar::kBgB;

  for (int y = cy - radius; y <= cy + radius; ++y) {
    const int dy = y - cy;
    const int span_sq = radius * radius - dy * dy;
    if (span_sq <= 0) {
      continue;
    }
    const int half_w = static_cast<int>(sqrtf(static_cast<float>(span_sq)));
    const int x0 = cx - half_w;
    const int x1 = cx + half_w;

    // Grid position of the row's first pixel (float once per row, then step).
    const float es = static_cast<float>(x0 - cx) * km_per_px;
    const float ns = static_cast<float>(cy - y) * km_per_px;
    const float east = es * fc + ns * fs + ce;
    const float north = -es * fs + ns * fc + cn;
    const float u0 = (east + data::map::kHalfKm) / cell - 0.5f;
    const float v0 = (data::map::kHalfKm - north) / cell - 0.5f;
    int32_t u_fp = static_cast<int32_t>(lroundf(u0 * 65536.0f));
    int32_t v_fp = static_cast<int32_t>(lroundf(v0 * 65536.0f));

    const int8_t* bayer_row = kBayer4[y & 3];

    for (int x = x0; x <= x1; ++x, u_fp += du_fp, v_fp += dv_fp) {
      const int elev = sampleGrid(data::map::kElev, u_fp, v_fp);
      int r;
      int g;
      int b;
      if (elev < 0) {
        r = bg_r;
        g = bg_g;
        b = bg_b;
      } else {
        const Rgb base = s_hypso_lut[elev];
        const int shade = sampleGrid(data::map::kShade, u_fp, v_fp);
        r = (base.r * shade) >> 7;
        g = (base.g * shade) >> 7;
        b = (base.b * shade) >> 7;
        const int water = sampleGrid(data::map::kWater, u_fp, v_fp);
        if (water > 0) {
          r += ((kWaterR - r) * water) >> 8;
          g += ((kWaterG - g) * water) >> 8;
          b += ((kWaterB - b) * water) >> 8;
        }
      }

      const int dx = x - cx;
      const int d_sq = dx * dx + dy * dy;
      if (d_sq > fade_start_sq) {
        const float t = std::min(
            1.0f, (sqrtf(static_cast<float>(d_sq)) - kFadeStartPx) * inv_fade_len);
        r += static_cast<int>((bg_r - r) * t);
        g += static_cast<int>((bg_g - g) * t);
        b += static_cast<int>((bg_b - b) * t);
      }

      const int dith = bayer_row[x & 3] - 8;
      r = std::clamp(r + (dith >> 1), 0, 255);
      g = std::clamp(g + (dith >> 2), 0, 255);
      b = std::clamp(b + (dith >> 1), 0, 255);
      s_row[x] = static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    }

    gfx.draw16bitRGBBitmap(static_cast<int16_t>(x0), static_cast<int16_t>(y),
                           &s_row[x0], static_cast<int16_t>(x1 - x0 + 1), 1);
  }
}

/** Clip segment p0-p1 to the disc |p - c| <= r and draw what remains. */
void drawSegmentClipped(PlaneGfx& gfx, float x0, float y0, float x1, float y1,
                        float half_width, uint16_t color) {
  const float cx = static_cast<float>(radar::kCenterX);
  const float cy = static_cast<float>(radar::kCenterY);
  const float r = kVectorClipRadiusPx;
  const float r_sq = r * r;

  const float ax = x0 - cx;
  const float ay = y0 - cy;
  const float dx = x1 - x0;
  const float dy = y1 - y0;

  const bool in0 = ax * ax + ay * ay <= r_sq;
  const bool in1 = (x1 - cx) * (x1 - cx) + (y1 - cy) * (y1 - cy) <= r_sq;

  float t0 = 0.0f;
  float t1 = 1.0f;
  if (!in0 || !in1) {
    const float a = dx * dx + dy * dy;
    if (a < 1e-6f) {
      return;
    }
    const float b = 2.0f * (ax * dx + ay * dy);
    const float c = ax * ax + ay * ay - r_sq;
    const float disc = b * b - 4.0f * a * c;
    if (disc <= 0.0f) {
      return;
    }
    const float sq = sqrtf(disc);
    t0 = std::max(0.0f, (-b - sq) / (2.0f * a));
    t1 = std::min(1.0f, (-b + sq) / (2.0f * a));
    if (t0 >= t1) {
      return;
    }
  }

  gfx.drawWideLine(static_cast<int16_t>(lroundf(x0 + dx * t0)),
                   static_cast<int16_t>(lroundf(y0 + dy * t0)),
                   static_cast<int16_t>(lroundf(x0 + dx * t1)),
                   static_cast<int16_t>(lroundf(y0 + dy * t1)), half_width, color);
}

void drawPaths(PlaneGfx& gfx) {
  const float outer_km = radar::scaleActive().label_km;
  if (outer_km <= 0.0f) {
    return;
  }
  const float px_per_km = static_cast<float>(radar::kGridOuterRadius) / outer_km;

  const float facing = static_cast<float>(radar::facingDeg()) * 3.14159265f / 180.0f;
  const float fc = cosf(facing);
  const float fs = sinf(facing);

  float ce = 0.0f;
  float cn = 0.0f;
  centerOffsetKm(&ce, &cn);

  const float cx = static_cast<float>(radar::kCenterX);
  const float cy = static_cast<float>(radar::kCenterY);
  constexpr float kInvQuant = 1.0f / 32.0f;
  // Quick-reject box in km around the visible disc.
  const float cull_km = kVectorClipRadiusPx / px_per_km + 1.0f;

  // Water under roads, minor under major at crossings; runways on top.
  struct KindStyle {
    uint8_t kind;
    float half_width;
    uint16_t color;
    float max_range_km;  // 0 = every range
  };
  constexpr KindStyle kKindOrder[] = {
      {data::map::kPathStream, kStreamHalfWidth, kColorStream, kDetailMaxRangeKm},
      {data::map::kPathRiver, kRiverHalfWidth, kColorRiver, 0.0f},
      {data::map::kPathSecondary, kSecondaryHalfWidth, kColorSecondary,
       kDetailMaxRangeKm},
      {data::map::kPathPrimary, kPrimaryHalfWidth, kColorPrimary, kPrimaryMaxRangeKm},
      {data::map::kPathMotorway, kMotorwayHalfWidth, kColorMotorway, 0.0f},
      {data::map::kPathRunway, kRunwayHalfWidth, kColorRunway, 0.0f},
  };
  for (const KindStyle& style : kKindOrder) {
    if (style.max_range_km > 0.0f && outer_km > style.max_range_km) {
      continue;
    }
    const uint8_t kind = style.kind;
    const float half_width = style.half_width;
    const uint16_t color = style.color;

    for (size_t p = 0; p < data::map::kPathCount; ++p) {
      const data::map::Path& path = data::map::kPaths[p];
      if (path.kind != kind) {
        continue;
      }
      bool have_prev = false;
      float px = 0.0f;
      float py = 0.0f;
      float pe = 0.0f;
      float pn = 0.0f;
      for (uint16_t i = 0; i < path.count; ++i) {
        const data::map::PathVert& v = data::map::kPathVerts[path.first + i];
        const float e = static_cast<float>(v.east) * kInvQuant - ce;
        const float n = static_cast<float>(v.north) * kInvQuant - cn;
        const float x = cx + (e * fc - n * fs) * px_per_km;
        const float y = cy - (e * fs + n * fc) * px_per_km;
        if (have_prev) {
          // Reject only when both ends sit beyond the same side of the view
          // box — a long simplified segment may cross the scope even though
          // neither endpoint is anywhere near it.
          const bool same_side = (e > cull_km && pe > cull_km) ||
                                 (e < -cull_km && pe < -cull_km) ||
                                 (n > cull_km && pn > cull_km) ||
                                 (n < -cull_km && pn < -cull_km);
          if (!same_side) {
            drawSegmentClipped(gfx, px, py, x, y, half_width, color);
          }
        }
        px = x;
        py = y;
        pe = e;
        pn = n;
        have_prev = true;
      }
    }
  }
}

}  // namespace

bool available() {
  if (!data::map::kHasData) {
    return false;
  }
  float ce = 0.0f;
  float cn = 0.0f;
  centerOffsetKm(&ce, &cn);
  return fabsf(ce) < data::map::kHalfKm && fabsf(cn) < data::map::kHalfKm;
}

void draw(PlaneGfx& gfx) {
  if (!available()) {
    return;
  }
  drawTerrain(gfx);
  drawPaths(gfx);
}

}  // namespace ui::map_underlay
