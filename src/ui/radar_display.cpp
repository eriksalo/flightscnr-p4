#include "ui/radar_display.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "config.h"
#include "data/towns_lookup.h"
#include "hardware/display.h"
#include "hardware/display_font.h"
#include "hardware/panel.h"
#include "ui/display_prefs.h"
#include "services/adsb_client.h"
#include "services/aircraft_alert.h"
#include "services/aircraft_type_lookup.h"
#include "geo/flat_earth.h"
#include "services/map_center.h"
#include "ui/aircraft_icon.h"
#include "ui/aircraft_symbol.h"
#include "ui/map_underlay.h"
#include "ui/radar_accent.h"
#include "ui/radar_scale.h"
#include "ui/radar_theme.h"

#include "hardware/gfx_log.h"

namespace ui {
namespace radar {

uint16_t kColorBackground = 0x0000;
uint16_t kColorGrid = 0x0320;
uint16_t kColorSweep = 0x0320;
uint16_t kColorSweepTrail = 0x0320;
uint16_t kColorLabel = 0xFFFF;
uint16_t kColorAircraft = 0x001F;
uint16_t kColorAircraftPrivate = 0x001F;
uint16_t kColorAircraftProp = 0x001F;
uint16_t kColorAircraftOther = 0xFFFF;
uint16_t kColorMapDot = 0x39C7;
uint16_t kColorMapLabel = 0x4A69;
uint16_t kColorTagType = 0x5DFF;
uint16_t kColorTagAltitudeAscend = 0x07FF;
uint16_t kColorTagAltitudeDescend = 0xF81F;
uint16_t kColorAlertMilitary = 0xFBE0;    // orange
uint16_t kColorAlertEmergency = 0xF800;   // red

}  // namespace radar

namespace {

bool s_label_metrics_ready = false;
UiTextStyle s_cardinal_style = displayFontCardinal();
UiTextStyle s_scale_style = displayFontScale();
UiTextStyle s_tag_style = displayFontTag();

int s_scale_label_max_w = 0;
int s_scale_label_h = 0;

PlaneGfx* s_draw = &tft;
PlaneGfxSprite s_bg(&tft);
PlaneGfxSprite s_content(&tft);
bool s_bg_ready = false;
bool s_content_ready = false;

struct IntRect {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;

  IntRect() = default;
  IntRect(int x_in, int y_in, int w_in, int h_in) : x(x_in), y(y_in), w(w_in), h(h_in) {}
};

bool s_aircraft_dirty = false;
IntRect s_aircraft_dirty_rect{};
bool s_content_base_valid = false;
bool s_sweep_track_valid = false;
IntRect s_prev_sweep_dirty{};
float s_display_sweep_deg = 0.0f;
float s_last_painted_sweep_deg = 0.0f;
unsigned long s_last_sweep_paint_ms = 0;
bool s_display_sweep_init = false;

/** What the beam still owes a target; planned once per poll by planReveal(). */
enum : uint8_t {
  kRevealIdle = 0,  /** nothing pending */
  kRevealUpdate,    /** live entry replaces shown[reveal_slot] */
  kRevealAdd,       /** live entry is new to the scope */
  kRevealRetire,    /** shown entry left the feed */
};

struct CachedAircraftMarker {
  services::adsb::Aircraft plane{};
  int x = 0;
  int y = 0;
  bool beyond_dot = false;
  /** Screen extent (symbol + tag), filled on first use by markerBoundsOf().
   *  Measuring a tag block costs three textWidth() passes and the sweep asks for
   *  these every frame, so they are computed once per marker snapshot. int16 (and
   *  bounds_w < 0 as the "not measured yet" marker) keeps the two 64-slot arrays
   *  off the internal DRAM that TLS handshakes need. */
  mutable int16_t bounds_x = 0;
  mutable int16_t bounds_y = 0;
  mutable int16_t bounds_w = -1;
  mutable int16_t bounds_h = 0;
  /** Shown entries only: cleared when the beam retires the blip. Retired slots
   *  are compacted at the next poll so reveal_slot stays valid for a cycle. */
  bool alive = true;
  uint8_t reveal_state = kRevealIdle;
  /** Live entries with kRevealUpdate: shown slot this target replaces. */
  int8_t reveal_slot = -1;
};

/** Aircraft state currently painted into the content sprite / panel. Live ADS-B
 *  data is copied into this snapshot one target at a time, as the sweep spoke
 *  crosses that target's bearing (see revealAircraftUnderBeam) — so a blip only
 *  appears, moves, or vanishes when the beam paints it, like a real PPI scope. */
CachedAircraftMarker s_shown_markers[services::adsb::kMaxAircraft];
size_t s_shown_marker_count = 0;
/** Scratch for ADS-B refresh — must not live on loopTask stack (~8 KB). */
CachedAircraftMarker s_current_aircraft_markers[services::adsb::kMaxAircraft];
bool s_marker_prev_used[services::adsb::kMaxAircraft] = {};

/** Valid entries in s_current_aircraft_markers while a reveal plan is running. */
size_t s_live_marker_count = 0;
/** Live ADS-B data may differ from s_shown_markers; beam reveal still has work. */
bool s_reveal_pending = false;
/** Adopt live aircraft state on the next content rebuild (no-reveal fallbacks). */
bool s_aircraft_sync_pending = false;

bool rectEmpty(const IntRect& r);
bool rectsOverlap(const IntRect& a, const IntRect& b);
IntRect clampRectToScreen(IntRect r);
IntRect unionRect(const IntRect& a, const IntRect& b);
IntRect markerBounds(const CachedAircraftMarker& marker);

/** markerBounds() with the result memoized on the marker. Bounds are always
 *  clamped to the screen, so int16 storage is lossless. */
IntRect markerBoundsOf(const CachedAircraftMarker& marker) {
  if (marker.bounds_w < 0) {
    const IntRect b = markerBounds(marker);
    marker.bounds_x = static_cast<int16_t>(b.x);
    marker.bounds_y = static_cast<int16_t>(b.y);
    marker.bounds_w = static_cast<int16_t>(std::max(0, b.w));
    marker.bounds_h = static_cast<int16_t>(b.h);
  }
  return IntRect{marker.bounds_x, marker.bounds_y, marker.bounds_w, marker.bounds_h};
}

class DrawScope {
 public:
  explicit DrawScope(PlaneGfx& gfx) : prev_(s_draw) { s_draw = &gfx; }
  ~DrawScope() { s_draw = prev_; }

 private:
  PlaneGfx* prev_;
};

UiTextStyle pickTextSizeForHeight(int target_px, size_t lo, size_t hi) {
  return displayFontPickForHeight(tft, target_px, lo, hi);
}

void initLabelMetrics() {
  if (s_label_metrics_ready) {
    return;
  }

  s_cardinal_style = pickTextSizeForHeight(radar::kCardinalLabelHeightPx, 2, 5);
  const int cardinal_h = displayFontHeight(tft, s_cardinal_style);
  s_scale_style =
      pickTextSizeForHeight(cardinal_h - radar::kScaleBelowCardinalPx, 0, 3);
  s_tag_style = displayFontTag();  // explicit 4pt; below the picker ladder

  displayFontApply(tft, s_scale_style);
  s_scale_label_h = tft.fontHeight();
  s_scale_label_max_w = 0;
  char label[12];
  for (size_t i = 0; i < radar::kRangeMileOptionCount; ++i) {
    const float label_km =
        static_cast<float>(radar::kRangeMileOptions[i]) * radar::kStatuteMileKm;
    for (int ring = 1; ring <= radar::kRingCount; ++ring) {
      const float ring_km = label_km * static_cast<float>(ring) /
                            static_cast<float>(radar::kRingCount);
      for (int unit = 0; unit <= static_cast<int>(radar::DistanceUnit::NauticalMile);
           ++unit) {
        radar::formatScaleTag(label, sizeof(label), ring_km,
                              static_cast<radar::DistanceUnit>(unit));
        const int w = tft.textWidth(label);
        if (w > s_scale_label_max_w) {
          s_scale_label_max_w = w;
        }
      }
    }
  }

  s_label_metrics_ready = true;
}

void initPalette() {
  const radar::AccentRgb accent = radar::accentPalette();
  radar::kColorBackground = tft.color565(radar::kBgR, radar::kBgG, radar::kBgB);
  radar::kColorGrid = tft.color565(accent.grid_r, accent.grid_g, accent.grid_b);
  radar::kColorSweep = tft.color565(accent.sweep_r, accent.sweep_g, accent.sweep_b);
  radar::kColorSweepTrail =
      tft.color565(accent.trail_r, accent.trail_g, accent.trail_b);
  radar::kColorLabel = tft.color565(accent.label_r, accent.label_g, accent.label_b);
  radar::kColorAircraft =
      tft.color565(radar::kAircraftR, radar::kAircraftG, radar::kAircraftB);
  radar::kColorAircraftPrivate = tft.color565(
      radar::kAircraftPrivateR, radar::kAircraftPrivateG, radar::kAircraftPrivateB);
  radar::kColorAircraftProp =
      tft.color565(radar::kAircraftPropR, radar::kAircraftPropG, radar::kAircraftPropB);
  radar::kColorAircraftOther = tft.color565(
      radar::kAircraftOtherR, radar::kAircraftOtherG, radar::kAircraftOtherB);
  radar::kColorMapDot = tft.color565(radar::kMapDotR, radar::kMapDotG, radar::kMapDotB);
  radar::kColorMapLabel =
      tft.color565(radar::kMapLabelR, radar::kMapLabelG, radar::kMapLabelB);
  radar::kColorTagType =
      tft.color565(radar::kTagTypeR, radar::kTagTypeG, radar::kTagTypeB);
  radar::kColorTagAltitudeAscend =
      tft.color565(radar::kTagAltAscendR, radar::kTagAltAscendG, radar::kTagAltAscendB);
  radar::kColorTagAltitudeDescend = tft.color565(radar::kTagAltDescendR,
                                                 radar::kTagAltDescendG,
                                                 radar::kTagAltDescendB);
}

/** Body color by aircraft kind: commercial amber, business-jet cyan, propeller
 *  violet, everything else slate. Alert traffic overrides this while pulsing. */
uint16_t aircraftBodyColor(const services::adsb::Aircraft& plane) {
  switch (aircraft_icon::colorGroup(aircraft_icon::resolveCategory(plane))) {
    case aircraft_icon::ColorGroup::CommercialJet:
      return radar::kColorAircraft;
    case aircraft_icon::ColorGroup::PrivateJet:
      return radar::kColorAircraftPrivate;
    case aircraft_icon::ColorGroup::Propeller:
      return radar::kColorAircraftProp;
    default:
      return radar::kColorAircraftOther;
  }
}

/** Treat small negative rates as level (cyan). */
constexpr int16_t kDescendRateThresholdFpm = -64;

uint16_t altitudeTagColor(const services::adsb::Aircraft& plane) {
  if (plane.vert_rate_fpm != services::adsb::kVertRateUnknown &&
      plane.vert_rate_fpm < kDescendRateThresholdFpm) {
    return radar::kColorTagAltitudeDescend;
  }
  return radar::kColorTagAltitudeAscend;
}

void localOffsetFromCenter(float lat, float lon, float* dx_km, float* dy_km,
                           float* dist_km) {
  geo::localOffsetKm(services::map_center::latitude(), services::map_center::longitude(),
                     lat, lon, dx_km, dy_km, dist_km);
}

/** Rotate east/north so geographic facingDeg() maps to screen-up. */
void rotateEastNorthForFacing(float east_km, float north_km, float* out_east,
                              float* out_north) {
  const float f = static_cast<float>(radar::facingDeg()) * 3.14159265f / 180.0f;
  const float c = cosf(f);
  const float s = sinf(f);
  *out_east = east_km * c - north_km * s;
  *out_north = east_km * s + north_km * c;
}

/** Track/nose angle for drawing after facing rotation. */
float displayTrackDeg(float track_deg) {
  float t = track_deg - static_cast<float>(radar::facingDeg());
  while (t < 0.0f) {
    t += 360.0f;
  }
  while (t >= 360.0f) {
    t -= 360.0f;
  }
  return t;
}

float innerRingMaxKm() {
  const float outer_km = radar::scaleActive().label_km;
  return outer_km * (static_cast<float>(radar::kGridOuterRadius -
                                       radar::kAircraftInsideRingInsetPx) /
                     static_cast<float>(radar::kGridOuterRadius));
}

void latLonToScreen(float lat, float lon, int* out_x, int* out_y) {
  const float outer_km = radar::scaleActive().label_km;
  const float px_per_km = static_cast<float>(radar::kGridOuterRadius) / outer_km;

  float dx_km = 0.0f;
  float dy_km = 0.0f;
  float dist_km = 0.0f;
  localOffsetFromCenter(lat, lon, &dx_km, &dy_km, &dist_km);
  rotateEastNorthForFacing(dx_km, dy_km, &dx_km, &dy_km);

  *out_x = radar::kCenterX + static_cast<int>(lroundf(dx_km * px_per_km));
  *out_y = radar::kCenterY - static_cast<int>(lroundf(dy_km * px_per_km));
}

bool isInsideOuterRingKm(float dist_km) { return dist_km <= innerRingMaxKm(); }

int distSqFromCenter(int x, int y) {
  const int dx = x - radar::kCenterX;
  const int dy = y - radar::kCenterY;
  return dx * dx + dy * dy;
}

bool beyondRingEdgeDotFromLatLon(float lat, float lon, int* out_x, int* out_y) {
  float dx_km = 0.0f;
  float dy_km = 0.0f;
  float dist_km = 0.0f;
  localOffsetFromCenter(lat, lon, &dx_km, &dy_km, &dist_km);
  if (dist_km < 0.01f || isInsideOuterRingKm(dist_km)) {
    return false;
  }
  rotateEastNorthForFacing(dx_km, dy_km, &dx_km, &dy_km);

  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int rim_r = radar::kCenterX - radar::kBeyondRingScreenMarginPx;
  const float angle_rad = atan2f(dx_km, dy_km);

  *out_x = cx + static_cast<int>(lroundf(sinf(angle_rad) * rim_r));
  *out_y = cy - static_cast<int>(lroundf(cosf(angle_rad) * rim_r));
  return true;
}

void drawBeyondRingMarker(int x, int y, float heading_deg,
                          const services::adsb::Aircraft* aircraft) {
  const uint16_t color =
      aircraft != nullptr ? aircraftBodyColor(*aircraft) : radar::kColorAircraftOther;
  aircraft_symbol::drawCompact(*s_draw, x, y, heading_deg, color, aircraft);
}

void applyTagStyle() { displayFontApply(*s_draw, s_tag_style); }

int aircraftSymbolHalfPx() {
  return std::max(aircraft_symbol::radiusPx(), radar::kAircraftIconRadiusPx);
}

int measureTagBlockWidth(const services::adsb::Aircraft& plane) {
  applyTagStyle();
  int max_w = 0;
  if (plane.callsign[0] != '\0') {
    max_w = std::max(max_w, s_draw->textWidth(plane.callsign));
  }
  if (plane.type[0] != '\0') {
    char type_label[32];
    services::aircraft_type::formatRadarTagLabel(plane.type, type_label, sizeof(type_label));
    max_w = std::max(max_w, s_draw->textWidth(type_label));
  }
  if (plane.alt[0] != '\0') {
    char alt_display[20];
    radar::formatAltitudeDisplay(plane.alt, alt_display, sizeof(alt_display));
    max_w = std::max(max_w, s_draw->textWidth(alt_display));
  }
  return max_w;
}

void drawAircraftTag(int x, int y, const services::adsb::Aircraft& plane) {
  initLabelMetrics();
  applyTagStyle();

  const int line_h = s_draw->fontHeight();
  const int block_w = measureTagBlockWidth(plane);
  const int block_h = line_h * 3;
  int ly = y - block_h / 2;

  const int symbol_half = aircraftSymbolHalfPx();
  const bool tag_on_right = x < radar::kCenterX;
  int anchor_x = 0;
  if (tag_on_right) {
    anchor_x = x + symbol_half + radar::kAircraftLabelGapPx;
    anchor_x = std::min(anchor_x, radar::kSize - block_w - 1);
    s_draw->setTextDatum(TextDatum::TopLeft);
  } else {
    anchor_x = x - symbol_half - radar::kAircraftLabelGapPx;
    anchor_x = std::max(anchor_x, block_w + 1);
    s_draw->setTextDatum(TextDatum::TopRight);
  }
  ly = std::max(1, std::min(ly, radar::kSize - block_h - 1));

  if (plane.callsign[0] != '\0') {
    s_draw->setTextColor(radar::kColorLabel, radar::kColorBackground);
    s_draw->drawString(plane.callsign, anchor_x, ly);
  }
  ly += line_h;

  if (plane.type[0] != '\0') {
    char type_label[32];
    services::aircraft_type::formatRadarTagLabel(plane.type, type_label, sizeof(type_label));
    s_draw->setTextColor(radar::kColorTagType, radar::kColorBackground);
    s_draw->drawString(type_label, anchor_x, ly);
  }
  ly += line_h;

  if (plane.alt[0] != '\0') {
    char alt_display[20];
    radar::formatAltitudeDisplay(plane.alt, alt_display, sizeof(alt_display));
    s_draw->setTextColor(altitudeTagColor(plane), radar::kColorBackground);
    s_draw->drawString(alt_display, anchor_x, ly);
  }
}

struct AircraftDrawItem {
  size_t index = 0;
  int x = 0;
  int y = 0;
  int dist_sq = 0;
};

struct BeyondDotDrawItem {
  size_t index = 0;
  int x = 0;
  int y = 0;
  int dist_sq = 0;
};

void sortDrawItemsFarFirst(AircraftDrawItem* items, size_t count) {
  for (size_t i = 1; i < count; ++i) {
    const AircraftDrawItem key = items[i];
    size_t j = i;
    while (j > 0 && items[j - 1].dist_sq < key.dist_sq) {
      items[j] = items[j - 1];
      --j;
    }
    items[j] = key;
  }
}

void sortBeyondDotsFarFirst(BeyondDotDrawItem* items, size_t count) {
  for (size_t i = 1; i < count; ++i) {
    const BeyondDotDrawItem key = items[i];
    size_t j = i;
    while (j > 0 && items[j - 1].dist_sq < key.dist_sq) {
      items[j] = items[j - 1];
      --j;
    }
    items[j] = key;
  }
}

bool isAircraftInRange(const services::adsb::Aircraft& ac) {
  float dx_km = 0.0f;
  float dy_km = 0.0f;
  float dist_km = 0.0f;
  localOffsetFromCenter(ac.lat, ac.lon, &dx_km, &dy_km, &dist_km);
  return isInsideOuterRingKm(dist_km);
}

size_t inRangeAircraftCount() {
  const size_t n = services::adsb::aircraftCount();
  const services::adsb::Aircraft* planes = services::adsb::aircraftList();
  size_t in_range = 0;
  for (size_t i = 0; i < n; ++i) {
    if (isAircraftInRange(planes[i])) {
      ++in_range;
    }
  }
  return in_range;
}

/** Aircraft drawn as full planes inside the outer ring (hide filter applied).
 *  Beyond-ring edge blips are excluded — idle clock only returns to radar for
 *  in-scope traffic, not off-screen dots. */
size_t visibleAircraftCount() {
  const size_t n = services::adsb::aircraftCount();
  const services::adsb::Aircraft* planes = services::adsb::aircraftList();
  const bool hide_others = services::alert::hideNonAlertedEnabled();
  size_t visible = 0;
  for (size_t i = 0; i < n; ++i) {
    if (hide_others && !services::alert::isHighlighted(planes[i])) {
      continue;
    }
    if (isAircraftInRange(planes[i])) {
      ++visible;
    }
  }
  return visible;
}

/** Paint the shown-marker snapshot. With clip != nullptr only markers touching
 *  that rect are drawn (region repaint); a marker's own extent may exceed the
 *  rect, which is harmless because unchanged markers redraw identically.
 *  Every marker is collected before clipping so far-first z-order (and tag
 *  declutter) come out the same as in a full-screen redraw. */
void drawShownMarkers(const IntRect* clip) {
  initLabelMetrics();

  AircraftDrawItem items[services::adsb::kMaxAircraft];
  BeyondDotDrawItem dots[services::adsb::kMaxAircraft];
  size_t draw_count = 0;
  size_t dot_count = 0;

  const bool hide_others = services::alert::hideNonAlertedEnabled();
  for (size_t i = 0; i < s_shown_marker_count; ++i) {
    const CachedAircraftMarker& marker = s_shown_markers[i];
    if (!marker.alive) {
      continue;
    }
    if (hide_others && !services::alert::isHighlighted(marker.plane)) {
      continue;
    }

    if (marker.beyond_dot) {
      dots[dot_count].index = i;
      dots[dot_count].x = marker.x;
      dots[dot_count].y = marker.y;
      dots[dot_count].dist_sq = distSqFromCenter(marker.x, marker.y);
      ++dot_count;
      continue;
    }

    items[draw_count].index = i;
    items[draw_count].x = marker.x;
    items[draw_count].y = marker.y;
    items[draw_count].dist_sq = distSqFromCenter(marker.x, marker.y);
    ++draw_count;
  }

  const auto clipped_out = [clip](size_t marker_index) {
    return clip != nullptr &&
           !rectsOverlap(*clip, markerBoundsOf(s_shown_markers[marker_index]));
  };

  sortBeyondDotsFarFirst(dots, dot_count);
  for (size_t d = 0; d < dot_count; ++d) {
    if (clipped_out(dots[d].index)) {
      continue;
    }
    const services::adsb::Aircraft& plane = s_shown_markers[dots[d].index].plane;
    drawBeyondRingMarker(dots[d].x, dots[d].y, displayTrackDeg(plane.track_deg), &plane);
  }

  const bool pulse_on = services::alert::pulsePhase();
  sortDrawItemsFarFirst(items, draw_count);
  for (size_t d = 0; d < draw_count; ++d) {
    if (clipped_out(items[d].index)) {
      continue;
    }
    const services::adsb::Aircraft& plane = s_shown_markers[items[d].index].plane;
    uint16_t color = aircraftBodyColor(plane);
    if (services::alert::isHighlighted(plane)) {
      if (pulse_on) {
        color = plane.isEmergencySquawk() ? radar::kColorAlertEmergency
                                          : radar::kColorAlertMilitary;
      } else {
        color = radar::kColorGrid;
      }
    }
    aircraft_symbol::draw(*s_draw, items[d].x, items[d].y, displayTrackDeg(plane.track_deg),
                          color, &plane);
  }
  for (size_t d = 0; d < draw_count; ++d) {
    const services::adsb::Aircraft& plane = s_shown_markers[items[d].index].plane;
    if (clipped_out(items[d].index)) {
      continue;
    }
    drawAircraftTag(items[d].x, items[d].y, plane);
  }
}

void drawAircraft() { drawShownMarkers(nullptr); }

void drawCardinalLabel(const char* text, int x, int y, TextDatum datum) {
  displayFontApply(*s_draw, s_cardinal_style);
  s_draw->setTextDatum(datum);
  s_draw->setTextColor(radar::kColorGrid, radar::kColorBackground);
  s_draw->drawString(text, x, y);
}

void drawScaleLabelWithBackground(const char* text, int x, int y) {
  displayFontApply(*s_draw, s_scale_style);
  // Bottom-right anchor: text grows up/left into the W–SW wedge (not toward S).
  s_draw->setTextDatum(TextDatum::BottomRight);

  const int tw = s_draw->textWidth(text);
  const int th = s_draw->fontHeight();
  constexpr int kPadX = 3;
  constexpr int kPadY = 2;

  const int left = x - tw - kPadX;
  const int top = y - th - kPadY;

  s_draw->fillRect(left, top, tw + kPadX * 2, th + kPadY * 2, radar::kColorBackground);
  s_draw->setTextColor(radar::kColorGrid, radar::kColorBackground);
  s_draw->drawString(text, x, y);
}

void pointOnRadarArc(int cx, int cy, int r, float arc_len_px, int* x, int* y) {
  const float angle = arc_len_px / static_cast<float>(r);
  *x = cx + static_cast<int>(lroundf(sinf(angle) * static_cast<float>(r)));
  *y = cy - static_cast<int>(lroundf(cosf(angle) * static_cast<float>(r)));
}

void drawArcDash(int cx, int cy, int r, float arc_start_px, float arc_end_px, float half_width,
                 uint16_t color) {
  const float dash_len = arc_end_px - arc_start_px;
  if (dash_len < 0.5f) {
    return;
  }
  const int steps = std::max(2, static_cast<int>(lroundf(dash_len / 3.0f)));
  int px = 0;
  int py = 0;
  pointOnRadarArc(cx, cy, r, arc_start_px, &px, &py);
  for (int i = 1; i <= steps; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(steps);
    const float s = arc_start_px + dash_len * t;
    int nx = 0;
    int ny = 0;
    pointOnRadarArc(cx, cy, r, s, &nx, &ny);
    s_draw->drawWideLine(px, py, nx, ny, half_width, color);
    px = nx;
    py = ny;
  }
}

void drawDashedWideLine(int x0, int y0, int x1, int y1, float half_width, uint16_t color) {
  const float dx = static_cast<float>(x1 - x0);
  const float dy = static_cast<float>(y1 - y0);
  const float len = sqrtf(dx * dx + dy * dy);
  if (len < 1.0f) {
    return;
  }

  const float ux = dx / len;
  const float uy = dy / len;
  const float dash = static_cast<float>(radar::kGridDashLenPx);
  const float gap = static_cast<float>(radar::kGridDashGapPx);
  const float period = dash + gap;

  for (float pos = 0.0f; pos + dash <= len; pos += period) {
    const float end = pos + dash;
    const int sx = x0 + static_cast<int>(lroundf(ux * pos));
    const int sy = y0 + static_cast<int>(lroundf(uy * pos));
    const int ex = x0 + static_cast<int>(lroundf(ux * end));
    const int ey = y0 + static_cast<int>(lroundf(uy * end));
    s_draw->drawWideLine(sx, sy, ex, ey, half_width, color);
  }
}

void drawDashedCircle(int cx, int cy, int r, float half_width, uint16_t color) {
  if (r <= 0) {
    return;
  }

  const float circumference = 2.0f * 3.14159265f * static_cast<float>(r);
  const float dash = static_cast<float>(radar::kGridDashLenPx);
  const float gap = static_cast<float>(radar::kGridDashGapPx);
  const float period = dash + gap;

  for (float arc_pos = 0.0f; arc_pos + dash <= circumference; arc_pos += period) {
    drawArcDash(cx, cy, r, arc_pos, arc_pos + dash, half_width, color);
  }
}

void drawGridRing(int cx, int cy, int r, uint16_t color) {
  if (r <= 0) {
    return;
  }
  const float hw = radar::kGridStrokeHalfWidth;
  const int thickness =
      std::max(1, static_cast<int>(radar::kGridStrokeHalfWidth * 2.0f));
  for (int i = 0; i < thickness && r - i > 0; ++i) {
    drawDashedCircle(cx, cy, r - i, hw, color);
  }
}

void drawRings(int cx, int cy, int outer_radius) {
  for (int i = 1; i <= radar::kRingCount; ++i) {
    const int r = (outer_radius * i) / radar::kRingCount;
    drawGridRing(cx, cy, r, radar::kColorGrid);
  }
}

void drawGridSpokes(int cx, int cy, int radius, uint16_t color) {
  const float hw = radar::kGridStrokeHalfWidth;
  const float facing = static_cast<float>(radar::facingDeg());
  auto spoke = [&](float angle_deg_from_up) {
    const float rad = angle_deg_from_up * 3.14159265f / 180.0f;
    const int dx = static_cast<int>(lroundf(sinf(rad) * static_cast<float>(radius)));
    const int dy = static_cast<int>(lroundf(-cosf(rad) * static_cast<float>(radius)));
    drawDashedWideLine(cx - dx, cy - dy, cx + dx, cy + dy, hw, color);
  };
  // Geographic N-S / E-W / diagonals, rotated so facing is at screen-up.
  spoke(-facing);
  spoke(-facing + 90.0f);
  spoke(-facing + 45.0f);
  spoke(-facing + 135.0f);
}

void drawIntercardinalLabel(const char* text, int cx, int cy, int radius,
                            float angle_deg_from_north) {
  const float rad = angle_deg_from_north * 3.14159265f / 180.0f;
  const int r = radius - radar::kCardinalDiagonalInsetPx;
  const int x = cx + static_cast<int>(lroundf(sinf(rad) * static_cast<float>(r)));
  const int y = cy - static_cast<int>(lroundf(cosf(rad) * static_cast<float>(r)));
  drawCardinalLabel(text, x, y, TextDatum::MiddleCenter);
}

bool s_force_cardinals = false;

void drawCardinalLabels() {
  if (!s_force_cardinals && !radar::showCompassRose()) {
    return;
  }
  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int rim_r = radar::kGridOuterRadius;
  const float facing = static_cast<float>(radar::facingDeg());

  // Geographic bearings → screen angle from up = bearing - facing.
  drawIntercardinalLabel("N", cx, cy, rim_r, 0.0f - facing);
  drawIntercardinalLabel("NE", cx, cy, rim_r, 45.0f - facing);
  drawIntercardinalLabel("E", cx, cy, rim_r, 90.0f - facing);
  drawIntercardinalLabel("SE", cx, cy, rim_r, 135.0f - facing);
  drawIntercardinalLabel("S", cx, cy, rim_r, 180.0f - facing);
  drawIntercardinalLabel("SW", cx, cy, rim_r, 225.0f - facing);
  drawIntercardinalLabel("W", cx, cy, rim_r, 270.0f - facing);
  drawIntercardinalLabel("NW", cx, cy, rim_r, 315.0f - facing);
}

/** Anchor scale text on the ring along kScaleLabelBearingDeg (between W and SW). */
void scaleLabelAnchorOnRing(int cx, int cy, int ring_radius, int gap_px, int* x,
                            int* y) {
  const float bearing =
      radar::kScaleLabelBearingDeg - static_cast<float>(radar::facingDeg());
  const float rad = bearing * 3.14159265f / 180.0f;
  const float r = static_cast<float>(ring_radius - gap_px);
  *x = cx + static_cast<int>(lroundf(sinf(rad) * r));
  *y = cy - static_cast<int>(lroundf(cosf(rad) * r));
}

void drawRingScaleLabels(int cx, int cy, int outer_radius) {
  const float label_km = radar::scaleActive().label_km;
  const radar::DistanceUnit unit = radar::distanceUnit();

  for (int ring = radar::kRingCount; ring >= 1; --ring) {
    const int ring_radius = (outer_radius * ring) / radar::kRingCount;
    const float ring_km =
        label_km * static_cast<float>(ring) / static_cast<float>(radar::kRingCount);

    char scale_label[12];
    radar::formatScaleTag(scale_label, sizeof(scale_label), ring_km, unit);

    int gap_px = radar::kScaleGapFromOuterRing;
    if (ring == radar::kRingCount && unit == radar::DistanceUnit::Km) {
      gap_px = radar::kScaleGapOuterRingKm;
    }

    int ax = 0;
    int ay = 0;
    scaleLabelAnchorOnRing(cx, cy, ring_radius, gap_px, &ax, &ay);
    drawScaleLabelWithBackground(scale_label, ax, ay);
  }
}

/** Soft town underlay: a dim dot and name for populated places inside the scope,
 *  drawn before the rings so the instrument stays on top. Places arrive sorted by
 *  population, and a label is skipped when its box would collide with one already
 *  placed, so the big towns win the space and the small ones drop out quietly. */
void drawTownUnderlay() {
  if (data::towns::kCount == 0) {
    return;
  }
  initLabelMetrics();

  const float outer_km = radar::scaleActive().label_km;
  if (outer_km <= 0.0f) {
    return;
  }
  const float px_per_km = static_cast<float>(radar::kGridOuterRadius) / outer_km;
  // Keep dots off the very rim where the range labels and edge blips live.
  const int max_r = radar::kGridOuterRadius - radar::kTownDotRadiusPx - 2;

  displayFontApply(*s_draw, s_tag_style);
  s_draw->setTextDatum(TextDatum::TopLeft);
  const int line_h = s_draw->fontHeight();
  // Over the terrain underlay the labels must not fill their glyph cells with
  // the flat background color; on a plain scope the fill is equivalent anyway.
  const bool over_map = map_underlay::available();

  IntRect placed[radar::kMaxTownLabels];
  int placed_count = 0;

  for (size_t i = 0; i < data::towns::kCount && placed_count < radar::kMaxTownLabels; ++i) {
    const data::towns::Town& town = data::towns::kTowns[i];

    float dx_km = 0.0f;
    float dy_km = 0.0f;
    float dist_km = 0.0f;
    localOffsetFromCenter(town.lat, town.lon, &dx_km, &dy_km, &dist_km);
    if (dist_km > outer_km) {
      continue;  // outside the scope at this range
    }
    rotateEastNorthForFacing(dx_km, dy_km, &dx_km, &dy_km);

    const int x = radar::kCenterX + static_cast<int>(lroundf(dx_km * px_per_km));
    const int y = radar::kCenterY - static_cast<int>(lroundf(dy_km * px_per_km));
    if (distSqFromCenter(x, y) > max_r * max_r) {
      continue;
    }

    const int text_w = s_draw->textWidth(town.name);
    IntRect label(x + radar::kTownDotRadiusPx + radar::kTownLabelGapPx, y - line_h / 2,
                  text_w, line_h);
    // Flip to the left of the dot when the label would run off the disc.
    if (label.x + label.w > radar::kSize - 2) {
      label.x = x - radar::kTownDotRadiusPx - radar::kTownLabelGapPx - text_w;
      s_draw->setTextDatum(TextDatum::TopLeft);
    }
    if (label.x < 2 || label.y < 2 || label.y + label.h > radar::kSize - 2) {
      continue;
    }

    bool collides = false;
    for (int p = 0; p < placed_count; ++p) {
      if (rectsOverlap(placed[p], label)) {
        collides = true;
        break;
      }
    }
    if (collides) {
      continue;
    }

    s_draw->fillCircle(static_cast<int16_t>(x), static_cast<int16_t>(y),
                       static_cast<int16_t>(radar::kTownDotRadiusPx), radar::kColorMapDot);
    if (over_map) {
      s_draw->setTextColor(radar::kColorMapLabel);
    } else {
      s_draw->setTextColor(radar::kColorMapLabel, radar::kColorBackground);
    }
    s_draw->drawString(town.name, static_cast<int16_t>(label.x),
                       static_cast<int16_t>(label.y));

    // Reserve the dot as well, so another town's label cannot sit on it.
    placed[placed_count++] = unionRect(
        label, IntRect(x - radar::kTownDotRadiusPx, y - radar::kTownDotRadiusPx,
                       radar::kTownDotRadiusPx * 2 + 1, radar::kTownDotRadiusPx * 2 + 1));
  }

  s_draw->setTextDatum(TextDatum::TopLeft);
}

template <typename GfxRef>
void drawStaticGrid(GfxRef& gfx) {
  initLabelMetrics();
  const DrawScope scope(gfx);
  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int grid_r = radar::kGridOuterRadius;

  gfx.fillScreen(radar::kColorBackground);
  if (displayPrefsMapUnderlayEnabled()) {
    map_underlay::draw(*s_draw);  // shaded relief, water and roads first
    drawTownUnderlay();  // beneath the rings: the map is context, not instrument
  }
  drawRings(cx, cy, grid_r);
  drawGridSpokes(cx, cy, grid_r, radar::kColorGrid);
  drawCardinalLabels();
  drawRingScaleLabels(cx, cy, grid_r);
  gfx.setTextDatum(TextDatum::TopLeft);
}

bool rebuildBackgroundSprite() {
  if (!s_bg_ready) {
    if (!s_bg.createSprite(radar::kSize, radar::kSize)) {
      Serial.println("radar: background sprite alloc failed");
      return false;
    }
    s_bg_ready = true;
  }

  drawStaticGrid(s_bg.gfx());
  s_content_base_valid = false;
  s_sweep_track_valid = false;
  return true;
}

bool ensureContentSprite() {
  if (s_content_ready) {
    return true;
  }
  if (!s_content.createSprite(radar::kSize, radar::kSize)) {
    Serial.println("radar: content sprite alloc failed");
    return false;
  }
  s_content_ready = true;
  return true;
}

void drawSweepAtOn(PlaneGfx& gfx, float lead_deg);
float currentSweepAngleDeg();
void resetDisplaySweepAngle(unsigned long now);
float advanceDisplaySweepAngle(unsigned long now);
float sweepAngleDeltaDeg(float from_deg, float to_deg);
void recoverSweepAfterGap(unsigned long gap_ms);
bool rebuildContentBase();
void blitRegionToPanel(const IntRect& rect);
void updateSweepOnPanel(float lead_deg);

void sweepSpokeEndpoint(float angle_deg, int* ex, int* ey) {
  constexpr float kDegToRad = 0.01745329252f;
  const float rad = angle_deg * kDegToRad;
  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int r = radar::kSweepRadiusPx;
  *ex = cx + static_cast<int>(lroundf(sinf(rad) * static_cast<float>(r)));
  *ey = cy - static_cast<int>(lroundf(cosf(rad) * static_cast<float>(r)));
}

void syncShownMarkersToLive();

bool rectEmpty(const IntRect& r);
IntRect markerBounds(const CachedAircraftMarker& marker);

bool rectEmpty(const IntRect& r) { return r.w <= 0 || r.h <= 0; }

bool rectsOverlap(const IntRect& a, const IntRect& b) {
  if (rectEmpty(a) || rectEmpty(b)) {
    return false;
  }
  return a.x < b.x + b.w && a.x + a.w > b.x && a.y < b.y + b.h && a.y + a.h > b.y;
}

IntRect rectFromPoints(int x0, int y0, int x1, int y1, int margin) {
  IntRect r;
  r.x = std::min(x0, x1) - margin;
  r.y = std::min(y0, y1) - margin;
  const int x2 = std::max(x0, x1) + margin;
  const int y2 = std::max(y0, y1) + margin;
  r.w = x2 - r.x + 1;
  r.h = y2 - r.y + 1;
  return r;
}

IntRect clampRectToScreen(IntRect r) {
  if (r.x < 0) {
    r.w += r.x;
    r.x = 0;
  }
  if (r.y < 0) {
    r.h += r.y;
    r.y = 0;
  }
  if (r.x + r.w > radar::kSize) {
    r.w = radar::kSize - r.x;
  }
  if (r.y + r.h > radar::kSize) {
    r.h = radar::kSize - r.y;
  }
  return r;
}

IntRect unionRect(const IntRect& a, const IntRect& b) {
  if (rectEmpty(a)) {
    return b;
  }
  if (rectEmpty(b)) {
    return a;
  }
  const int x0 = std::min(a.x, b.x);
  const int y0 = std::min(a.y, b.y);
  const int x1 = std::max(a.x + a.w, b.x + b.w);
  const int y1 = std::max(a.y + a.h, b.y + b.h);
  return IntRect{x0, y0, x1 - x0, y1 - y0};
}

int sweepEraseMargin() {
  constexpr int kBase = static_cast<int>(radar::kSweepLineHalfWidth * 2.0f + 4.0f);
  return kBase + (hardware::panelUsesCo5300() ? 2 : 0);
}

IntRect spokeBounds(float angle_deg, int margin) {
  int ex = 0;
  int ey = 0;
  sweepSpokeEndpoint(angle_deg, &ex, &ey);
  return clampRectToScreen(
      rectFromPoints(radar::kCenterX, radar::kCenterY, ex, ey, margin));
}

IntRect unionSpokeBounds(const float* angles, int count, int margin) {
  IntRect bounds{};
  for (int i = 0; i < count; ++i) {
    bounds = unionRect(bounds, spokeBounds(angles[i], margin));
  }
  return bounds;
}

void copyBgRegionToContent(const IntRect& rect) {
  if (rectEmpty(rect)) {
    return;
  }
  const IntRect clipped = clampRectToScreen(rect);
  if (rectEmpty(clipped)) {
    return;
  }

  s_content.copyRegionFrom(s_bg, static_cast<int16_t>(clipped.x),
                           static_cast<int16_t>(clipped.y),
                           static_cast<int16_t>(clipped.w),
                           static_cast<int16_t>(clipped.h));
}

int collectSweepAngles(float lead_deg, float* angles, int max_angles) {
  if (max_angles <= 0) {
    return 0;
  }
  if (radar::kSweepTrailLines <= 1) {
    angles[0] = lead_deg;
    return 1;
  }

  int count = 0;
  for (int i = radar::kSweepTrailLines - 1; i >= 1; --i) {
    if (count >= max_angles - 1) {
      break;
    }
    const float t = static_cast<float>(i) / static_cast<float>(radar::kSweepTrailLines - 1);
    angles[count++] = lead_deg - t * radar::kSweepTrailSpanDeg;
  }
  if (count < max_angles) {
    angles[count++] = lead_deg;
  }
  return count;
}

void drawAircraftInRect(const IntRect& dirty) {
  if (rectEmpty(dirty)) {
    return;
  }
  const IntRect clip = clampRectToScreen(dirty);
  drawShownMarkers(&clip);
}

bool rebuildContentBase() {
  if (!s_bg_ready || !ensureContentSprite()) {
    return false;
  }

  s_content.copyRegionFrom(s_bg, 0, 0, s_bg.width(), s_bg.height());

  {
    const DrawScope scope(s_content.gfx());
    drawAircraft();
  }

  s_content_base_valid = true;
  s_sweep_track_valid = false;
  return true;
}

void blitRegionToPanel(const IntRect& rect) {
  if (rectEmpty(rect) || !s_content_ready) {
    return;
  }
  const IntRect clipped = clampRectToScreen(rect);
  if (rectEmpty(clipped)) {
    return;
  }

  s_content.pushRegion(static_cast<int16_t>(clipped.x),
                       static_cast<int16_t>(clipped.y),
                       static_cast<int16_t>(clipped.w),
                       static_cast<int16_t>(clipped.h));
}

void updateSweepOnPanel(float lead_deg) {
  float angles[8] = {};
  const int count = collectSweepAngles(lead_deg, angles, 8);
  const int margin = sweepEraseMargin();
  const IntRect new_dirty = unionSpokeBounds(angles, count, margin);

  IntRect erase_dirty = new_dirty;
  const float angle_step = sweepAngleDeltaDeg(s_last_painted_sweep_deg, lead_deg);
  if (s_sweep_track_valid) {
    if (angle_step <= radar::kSweepIncrementalMaxDeg) {
      erase_dirty = unionRect(s_prev_sweep_dirty, new_dirty);
    } else if (!rectEmpty(s_prev_sweep_dirty)) {
      blitRegionToPanel(s_prev_sweep_dirty);
      tft.setTextDatum(TextDatum::TopLeft);
    }
  }

  const unsigned long blit_start = millis();
  if (!rectEmpty(erase_dirty)) {
    blitRegionToPanel(erase_dirty);
  }
  const unsigned long blit_ms = millis() - blit_start;

  const unsigned long draw_start = millis();
  {
    const DrawScope scope(tft);
    drawSweepAtOn(tft, lead_deg);
  }
  tft.setTextDatum(TextDatum::TopLeft);
  const unsigned long draw_ms = millis() - draw_start;

  s_prev_sweep_dirty = new_dirty;
  s_sweep_track_valid = true;
  s_last_painted_sweep_deg = lead_deg;

  if (config::kRadarSweepTraceDebug && (blit_ms >= 8 || draw_ms >= 8)) {
    Serial.printf("[sweep] panel ang=%.1f erase=%dx%d blit_ms=%lu draw_ms=%lu\n", lead_deg,
                  erase_dirty.w, erase_dirty.h, blit_ms, draw_ms);
  }
}

void drawSweepSpokeOn(PlaneGfx& gfx, float angle_deg, uint16_t color) {
  int ex = 0;
  int ey = 0;
  sweepSpokeEndpoint(angle_deg, &ex, &ey);
  gfx.drawWideLine(radar::kCenterX, radar::kCenterY, ex, ey, radar::kSweepLineHalfWidth,
                   color);
}

void drawSweepAtOn(PlaneGfx& gfx, float lead_deg) {
  if (radar::kSweepTrailLines <= 1) {
    drawSweepSpokeOn(gfx, lead_deg, radar::kColorSweep);
    return;
  }

  for (int i = radar::kSweepTrailLines - 1; i >= 1; --i) {
    const float t = static_cast<float>(i) / static_cast<float>(radar::kSweepTrailLines - 1);
    const float angle = lead_deg - t * radar::kSweepTrailSpanDeg;
    drawSweepSpokeOn(gfx, angle, radar::kColorSweepTrail);
  }
  drawSweepSpokeOn(gfx, lead_deg, radar::kColorSweep);
}

float currentSweepAngleDeg() {
  const unsigned long t = millis() % radar::kSweepPeriodMs;
  return 360.0f * static_cast<float>(t) / static_cast<float>(radar::kSweepPeriodMs);
}

void resetDisplaySweepAngle(unsigned long now) {
  s_display_sweep_deg = currentSweepAngleDeg();
  s_last_painted_sweep_deg = s_display_sweep_deg;
  s_last_sweep_paint_ms = now;
  s_display_sweep_init = true;
}

float sweepAngleDeltaDeg(float from_deg, float to_deg) {
  float delta = std::fabs(to_deg - from_deg);
  if (delta > 180.0f) {
    delta = 360.0f - delta;
  }
  return delta;
}

float advanceDisplaySweepAngle(unsigned long now) {
  if (!s_display_sweep_init) {
    resetDisplaySweepAngle(now);
    return s_display_sweep_deg;
  }

  unsigned long dt = now - s_last_sweep_paint_ms;
  s_last_sweep_paint_ms = now;
  if (dt == 0) {
    return s_display_sweep_deg;
  }

  if (dt > radar::kSweepGapPauseMs) {
    dt = radar::kSweepFrameMs;
  } else if (dt > radar::kSweepMaxStepMs) {
    dt = radar::kSweepMaxStepMs;
  }

  const float rate = 360.0f / static_cast<float>(radar::kSweepPeriodMs);
  s_display_sweep_deg += rate * static_cast<float>(dt);
  if (s_display_sweep_deg >= 360.0f) {
    s_display_sweep_deg = std::fmod(s_display_sweep_deg, 360.0f);
  }
  return s_display_sweep_deg;
}

void recoverSweepAfterGap(unsigned long gap_ms) {
  if (gap_ms <= radar::kSweepGapPauseMs) {
    return;
  }
  if (s_sweep_track_valid && !rectEmpty(s_prev_sweep_dirty)) {
    blitRegionToPanel(s_prev_sweep_dirty);
    tft.setTextDatum(TextDatum::TopLeft);
    s_sweep_track_valid = false;
    if (config::kRadarSweepTraceDebug) {
      Serial.printf("[sweep] gap_recover gap=%lums\n", gap_ms);
    }
  }
}

IntRect aircraftMarkerBounds(int x, int y, const services::adsb::Aircraft& plane) {
  applyTagStyle();
  const int block_w = measureTagBlockWidth(plane);
  const int block_h = s_draw->fontHeight() * 3;
  const int symbol_half = aircraftSymbolHalfPx();
  constexpr int kPad = 6;

  int min_x = x - symbol_half - kPad;
  int max_x = x + symbol_half + kPad;
  int min_y = y - symbol_half - kPad;
  int max_y = y + symbol_half + kPad;

  // The tag side MUST match drawAircraftTag(): the label is drawn to the right
  // of the symbol when it sits left of center, otherwise to the left. Reserving
  // block_w on the wrong side leaves the label outside this dirty rect, so its
  // old text ghosts on the panel until the rotating sweep happens to repaint
  // that region from the content sprite.
  const bool tag_on_right = x < radar::kCenterX;
  if (tag_on_right) {
    max_x = std::max(max_x, x + symbol_half + radar::kAircraftLabelGapPx + block_w + kPad);
  } else {
    min_x = std::min(min_x, x - symbol_half - radar::kAircraftLabelGapPx - block_w - kPad);
  }
  min_y = std::min(min_y, y - block_h / 2 - kPad);
  max_y = std::max(max_y, y + block_h / 2 + kPad);

  return clampRectToScreen(
      IntRect(min_x, min_y, max_x - min_x + 1, max_y - min_y + 1));
}

size_t collectAircraftMarkers(CachedAircraftMarker* markers, size_t max_markers) {
  if (markers == nullptr || max_markers == 0) {
    return 0;
  }

  const size_t n = services::adsb::aircraftCount();
  const services::adsb::Aircraft* planes = services::adsb::aircraftList();
  size_t count = 0;

  for (size_t i = 0; i < n && count < max_markers; ++i) {
    float dx_km = 0.0f;
    float dy_km = 0.0f;
    float dist_km = 0.0f;
    localOffsetFromCenter(planes[i].lat, planes[i].lon, &dx_km, &dy_km, &dist_km);

    CachedAircraftMarker& marker = markers[count];
    marker.plane = planes[i];
    // These arrays are reused every poll: drop the previous occupant's measured
    // extent and reveal bookkeeping, or this target inherits the wrong rect.
    marker.bounds_w = -1;
    marker.alive = true;
    marker.reveal_state = kRevealIdle;
    marker.reveal_slot = -1;

    if (isInsideOuterRingKm(dist_km)) {
      latLonToScreen(planes[i].lat, planes[i].lon, &marker.x, &marker.y);
      marker.beyond_dot = false;
      ++count;
      continue;
    }

    if (beyondRingEdgeDotFromLatLon(planes[i].lat, planes[i].lon, &marker.x, &marker.y)) {
      marker.beyond_dot = true;
      ++count;
    }
  }
  return count;
}

IntRect markerBounds(const CachedAircraftMarker& marker) {
  if (marker.beyond_dot) {
    const int dot_r = aircraft_symbol::radiusPx() / 2 + 4;
    return clampRectToScreen(IntRect(marker.x - dot_r, marker.y - dot_r, dot_r * 2 + 1,
                                     dot_r * 2 + 1));
  }
  return aircraftMarkerBounds(marker.x, marker.y, marker.plane);
}

bool aircraftIdentityMatch(const services::adsb::Aircraft& a,
                           const services::adsb::Aircraft& b) {
  if (a.callsign[0] != '\0' && b.callsign[0] != '\0') {
    return strcmp(a.callsign, b.callsign) == 0;
  }
  constexpr float kPosEps = 0.001f;
  return fabsf(a.lat - b.lat) < kPosEps && fabsf(a.lon - b.lon) < kPosEps;
}

bool markerVisualChanged(const CachedAircraftMarker& prev,
                         const CachedAircraftMarker& curr) {
  if (prev.beyond_dot != curr.beyond_dot) {
    return true;
  }
  if (prev.x != curr.x || prev.y != curr.y) {
    return true;
  }
  if (fabsf(prev.plane.track_deg - curr.plane.track_deg) > 0.5f) {
    return true;
  }
  if (strcmp(prev.plane.callsign, curr.plane.callsign) != 0) {
    return true;
  }
  if (strcmp(prev.plane.type, curr.plane.type) != 0) {
    return true;
  }
  if (strcmp(prev.plane.alt, curr.plane.alt) != 0) {
    return true;
  }
  if (prev.plane.vert_rate_fpm != curr.plane.vert_rate_fpm) {
    return true;
  }
  return false;
}

IntRect unionChangedMarkerBounds(const CachedAircraftMarker* current, size_t curr_count) {
  memset(s_marker_prev_used, 0, sizeof(s_marker_prev_used));
  IntRect dirty{};

  for (size_t c = 0; c < curr_count; ++c) {
    int prev_i = -1;
    for (size_t p = 0; p < s_shown_marker_count; ++p) {
      if (s_marker_prev_used[p]) {
        continue;
      }
      if (aircraftIdentityMatch(s_shown_markers[p].plane, current[c].plane)) {
        prev_i = static_cast<int>(p);
        break;
      }
    }

    if (prev_i >= 0) {
      s_marker_prev_used[static_cast<size_t>(prev_i)] = true;
      if (markerVisualChanged(s_shown_markers[static_cast<size_t>(prev_i)],
                              current[c])) {
        dirty = unionRect(dirty,
                          markerBoundsOf(s_shown_markers[static_cast<size_t>(prev_i)]));
        dirty = unionRect(dirty, markerBoundsOf(current[c]));
      }
    } else {
      dirty = unionRect(dirty, markerBoundsOf(current[c]));
    }
  }

  for (size_t p = 0; p < s_shown_marker_count; ++p) {
    if (!s_marker_prev_used[p]) {
      dirty = unionRect(dirty, markerBoundsOf(s_shown_markers[p]));
    }
  }

  return dirty;
}

/** Adopt the whole live picture as shown (screen-wide update, no beam reveal). */
void syncShownMarkersToLive() {
  s_shown_marker_count =
      collectAircraftMarkers(s_shown_markers, services::adsb::kMaxAircraft);
  s_reveal_pending = false;
  s_aircraft_sync_pending = false;
  s_live_marker_count = 0;
}

/** Screen bearing of a marker in sweep coordinates: 0 = up, clockwise. */
float markerBearingDeg(const CachedAircraftMarker& marker) {
  const float dx = static_cast<float>(marker.x - radar::kCenterX);
  const float dy = static_cast<float>(radar::kCenterY - marker.y);
  if (fabsf(dx) < 0.01f && fabsf(dy) < 0.01f) {
    return 0.0f;
  }
  float deg = atan2f(dx, dy) * 180.0f / 3.14159265f;
  if (deg < 0.0f) {
    deg += 360.0f;
  }
  return deg;
}

float normalizeSweepDeg(float deg) {
  deg = std::fmod(deg, 360.0f);
  if (deg < 0.0f) {
    deg += 360.0f;
  }
  return deg;
}

/** True when the spoke moved past target_deg going from from_deg to to_deg. */
bool beamCrossed(float from_deg, float to_deg, float target_deg) {
  const float span = normalizeSweepDeg(to_deg - from_deg);
  if (span <= 0.0f) {
    return false;
  }
  const float rel = normalizeSweepDeg(target_deg - from_deg);
  return rel > 0.0f && rel <= span;
}

/** Repaint one region of the content sprite from the shown snapshot and push it. */
void repaintRegion(const IntRect& rect) {
  const IntRect clipped = clampRectToScreen(rect);
  if (rectEmpty(clipped) || !s_bg_ready || !s_content_ready) {
    return;
  }
  copyBgRegionToContent(clipped);
  {
    const DrawScope scope(s_content.gfx());
    drawAircraftInRect(clipped);
  }
  blitRegionToPanel(clipped);
  tft.setTextDatum(TextDatum::TopLeft);
}

/** Drop blips the beam has retired. Called before planning a new poll so the
 *  reveal_slot indices handed out below stay valid for the whole cycle. */
void compactShownMarkers() {
  size_t write_i = 0;
  for (size_t i = 0; i < s_shown_marker_count; ++i) {
    if (!s_shown_markers[i].alive) {
      continue;
    }
    if (write_i != i) {
      s_shown_markers[write_i] = s_shown_markers[i];
    }
    ++write_i;
  }
  s_shown_marker_count = write_i;
}

/** Match the new ADS-B picture against what is on screen and record what the
 *  beam owes each target. Run once per poll: the identity matching is O(n^2)
 *  strcmp and marker projection is trig-heavy, neither of which belongs in a
 *  33 ms sweep frame. */
void planReveal() {
  compactShownMarkers();

  s_live_marker_count =
      collectAircraftMarkers(s_current_aircraft_markers, services::adsb::kMaxAircraft);

  for (size_t p = 0; p < s_shown_marker_count; ++p) {
    s_shown_markers[p].reveal_state = kRevealIdle;
  }

  memset(s_marker_prev_used, 0, sizeof(s_marker_prev_used));  // indexed by shown slot
  bool pending = false;

  for (size_t c = 0; c < s_live_marker_count; ++c) {
    CachedAircraftMarker& live = s_current_aircraft_markers[c];
    int shown_i = -1;
    for (size_t p = 0; p < s_shown_marker_count; ++p) {
      if (s_marker_prev_used[p]) {
        continue;
      }
      if (aircraftIdentityMatch(s_shown_markers[p].plane, live.plane)) {
        shown_i = static_cast<int>(p);
        break;
      }
    }

    if (shown_i < 0) {
      live.reveal_state = kRevealAdd;
      pending = true;
      continue;
    }

    s_marker_prev_used[static_cast<size_t>(shown_i)] = true;
    if (markerVisualChanged(s_shown_markers[static_cast<size_t>(shown_i)], live)) {
      live.reveal_state = kRevealUpdate;
      live.reveal_slot = static_cast<int8_t>(shown_i);
      pending = true;
    }
  }

  for (size_t p = 0; p < s_shown_marker_count; ++p) {
    if (!s_marker_prev_used[p]) {
      s_shown_markers[p].reveal_state = kRevealRetire;
      pending = true;
    }
  }

  s_reveal_pending = pending;
}

/** Apply the planned work for every target the spoke just crossed, repainting
 *  only those targets' regions. Blips therefore appear, step, and drop out under
 *  the beam instead of the whole scope updating at once. Per frame this costs one
 *  bearing comparison per pending target — no matching, no projection. */
void revealAircraftUnderBeam(float from_deg, float to_deg) {
  if (!s_reveal_pending || !s_bg_ready || !s_content_ready || !s_content_base_valid) {
    return;
  }
  if (normalizeSweepDeg(to_deg - from_deg) <= 0.0f) {
    return;
  }
  initLabelMetrics();

  // Regions to repaint this frame; kept separate (not unioned) so a wide-apart
  // pair does not turn into a quadrant-sized blit.
  constexpr int kMaxRevealRects = 8;
  IntRect rects[kMaxRevealRects];
  int rect_count = 0;
  auto add_rect = [&](const IntRect& r) {
    if (rectEmpty(r)) {
      return;
    }
    if (rect_count < kMaxRevealRects) {
      rects[rect_count++] = r;
    } else {
      rects[kMaxRevealRects - 1] = unionRect(rects[kMaxRevealRects - 1], r);
    }
  };

  bool pending = false;

  for (size_t c = 0; c < s_live_marker_count; ++c) {
    CachedAircraftMarker& live = s_current_aircraft_markers[c];
    if (live.reveal_state == kRevealIdle) {
      continue;
    }

    if (live.reveal_state == kRevealUpdate) {
      const size_t slot = static_cast<size_t>(live.reveal_slot);
      if (live.reveal_slot < 0 || slot >= s_shown_marker_count) {
        live.reveal_state = kRevealIdle;  // snapshot was resynced under us
        continue;
      }
      CachedAircraftMarker& shown = s_shown_markers[slot];
      // Either bearing counts: a target that moved counter-sweep is still painted
      // (and its old blip erased) on the pass that reaches it first.
      if (!beamCrossed(from_deg, to_deg, markerBearingDeg(live)) &&
          !beamCrossed(from_deg, to_deg, markerBearingDeg(shown))) {
        pending = true;
        continue;
      }
      add_rect(unionRect(markerBoundsOf(shown), markerBoundsOf(live)));
      shown = live;
      shown.alive = true;
      shown.reveal_state = kRevealIdle;
      shown.reveal_slot = -1;
      live.reveal_state = kRevealIdle;
      continue;
    }

    // kRevealAdd: a target new to the feed paints in as the beam reaches it.
    if (!beamCrossed(from_deg, to_deg, markerBearingDeg(live))) {
      pending = true;
      continue;
    }
    if (s_shown_marker_count >= services::adsb::kMaxAircraft) {
      pending = true;
      continue;
    }
    CachedAircraftMarker& dst = s_shown_markers[s_shown_marker_count++];
    dst = live;
    dst.alive = true;
    dst.reveal_state = kRevealIdle;
    dst.reveal_slot = -1;
    add_rect(markerBoundsOf(dst));
    live.reveal_state = kRevealIdle;
  }

  for (size_t p = 0; p < s_shown_marker_count; ++p) {
    CachedAircraftMarker& shown = s_shown_markers[p];
    if (shown.reveal_state != kRevealRetire) {
      continue;
    }
    // Dropped from the feed: the blip goes dark when the beam next sweeps it.
    if (!beamCrossed(from_deg, to_deg, markerBearingDeg(shown))) {
      pending = true;
      continue;
    }
    add_rect(markerBoundsOf(shown));
    shown.alive = false;
    shown.reveal_state = kRevealIdle;
  }

  s_reveal_pending = pending;

  for (int i = 0; i < rect_count; ++i) {
    repaintRegion(rects[i]);
  }

  if (config::kRadarSweepTraceDebug && rect_count > 0) {
    Serial.printf("[sweep] reveal ang=%.1f->%.1f rects=%d shown=%u pending=%d\n", from_deg,
                  to_deg, rect_count, static_cast<unsigned>(s_shown_marker_count),
                  pending ? 1 : 0);
  }
}

}  // namespace

static void blitStatic() {
  initPalette();
  // Entering the radar shows the current picture at once; per-target beam reveal
  // only governs updates from here on.
  syncShownMarkersToLive();

  if (!s_bg_ready) {
    const DrawScope scope(tft);
    drawStaticGrid(tft);
    drawAircraft();
    tft.setTextDatum(TextDatum::TopLeft);
    return;
  }

  if (!rebuildContentBase()) {
    s_bg.pushSprite(0, 0);
    drawAircraft();
    tft.setTextDatum(TextDatum::TopLeft);
    return;
  }

  s_content.pushSprite(0, 0);
  tft.setTextDatum(TextDatum::TopLeft);
  if (displayPrefsSweepLineEnabled()) {
    resetDisplaySweepAngle(millis());
    updateSweepOnPanel(s_display_sweep_deg);
  }
}

void radarDisplayRefreshSweep() {
  initPalette();

  if (!s_bg_ready) {
    rebuildBackgroundSprite();
  }

  const unsigned long now = millis();
  const bool draw_sweep = displayPrefsSweepLineEnabled();

  if (!s_content_ready) {
    if (!ensureContentSprite()) {
      if (config::kRadarResumeDebug) {
        static unsigned long s_last_no_sprite_log_ms = 0;
        const unsigned long now_ms = millis();
        if (now_ms - s_last_no_sprite_log_ms >= 2000UL) {
          Serial.printf("[radar] sweep_no_sprite bg=%d content=%d base=%d heap=%u max_blk=%u\n",
                        s_bg_ready ? 1 : 0, s_content_ready ? 1 : 0,
                        s_content_base_valid ? 1 : 0, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
          s_last_no_sprite_log_ms = now_ms;
        }
      }
      const DrawScope scope(tft);
      if (draw_sweep) {
        unsigned long paint_gap = 0;
        if (s_display_sweep_init && s_last_sweep_paint_ms > 0) {
          paint_gap = now - s_last_sweep_paint_ms;
        }
        recoverSweepAfterGap(paint_gap);
        const float angle = advanceDisplaySweepAngle(now);
        drawSweepAtOn(tft, angle);
      }
      // No offscreen buffer: nothing to reveal region-by-region, so fall back to
      // screen-wide updates of the whole aircraft layer.
      if (s_aircraft_dirty || s_aircraft_sync_pending || s_reveal_pending) {
        syncShownMarkersToLive();
        s_aircraft_dirty = false;
      }
      drawAircraft();
      tft.setTextDatum(TextDatum::TopLeft);
      return;
    }
  }

  {
    static bool s_last_pulse = false;
    const bool cur_pulse = services::alert::pulsePhase();
    if (cur_pulse != s_last_pulse) {
      s_last_pulse = cur_pulse;
      const size_t ac_n = services::adsb::aircraftCount();
      const services::adsb::Aircraft* ac_list = services::adsb::aircraftList();
      for (size_t i = 0; i < ac_n; ++i) {
        if (services::alert::isHighlighted(ac_list[i])) {
          s_aircraft_dirty = true;
          s_aircraft_dirty_rect = IntRect{0, 0, radar::kSize, radar::kSize};
          break;
        }
      }
    }
  }

  if (s_aircraft_dirty || !s_content_base_valid) {
    // A screen-wide rebuild draws the shown snapshot; only the fallback paths
    // (sweep line off, or reveal unavailable) adopt live state here.
    if (s_aircraft_sync_pending) {
      syncShownMarkersToLive();
    }
    if (!rebuildContentBase()) {
      if (config::kRadarResumeDebug) {
        static unsigned long s_last_rebuild_fail_ms = 0;
        const unsigned long now_ms = millis();
        if (now_ms - s_last_rebuild_fail_ms >= 2000UL) {
          Serial.printf("[radar] sweep_rebuild_fail bg=%d content=%d base=%d dirty=%d heap=%u "
                        "max_blk=%u\n",
                        s_bg_ready ? 1 : 0, s_content_ready ? 1 : 0,
                        s_content_base_valid ? 1 : 0, s_aircraft_dirty ? 1 : 0,
                        ESP.getFreeHeap(), ESP.getMaxAllocHeap());
          s_last_rebuild_fail_ms = now_ms;
        }
      }
      return;
    }

    const unsigned long patch_start = millis();
    if (s_sweep_track_valid && !rectEmpty(s_prev_sweep_dirty)) {
      blitRegionToPanel(s_prev_sweep_dirty);
      tft.setTextDatum(TextDatum::TopLeft);
    }

    IntRect patch = s_aircraft_dirty_rect;
    if (rectEmpty(patch)) {
      patch = IntRect{0, 0, radar::kSize, radar::kSize};
    }
    patch = clampRectToScreen(patch);
    constexpr int kScreenPixels = radar::kSize * radar::kSize;
    const int patch_pixels = patch.w * patch.h;
    if (patch_pixels >= kScreenPixels / 3) {
      s_content.pushSprite(0, 0);
      tft.setTextDatum(TextDatum::TopLeft);
      if (config::kRadarSweepTraceDebug) {
        Serial.printf("[sweep] aircraft blit full (%lums)\n", millis() - patch_start);
      }
    } else {
      blitRegionToPanel(patch);
      tft.setTextDatum(TextDatum::TopLeft);
      if (config::kRadarSweepTraceDebug) {
        Serial.printf("[sweep] aircraft blit %dx%d @ (%d,%d) (%lums)\n", patch.w, patch.h,
                      patch.x, patch.y, millis() - patch_start);
      }
    }
    s_sweep_track_valid = false;
    s_aircraft_dirty = false;
    s_aircraft_dirty_rect = {};
  }

  if (draw_sweep) {
    unsigned long paint_gap = 0;
    if (s_display_sweep_init && s_last_sweep_paint_ms > 0) {
      paint_gap = now - s_last_sweep_paint_ms;
    }
    recoverSweepAfterGap(paint_gap);
    const bool had_angle = s_display_sweep_init;
    const float prev_angle = s_display_sweep_deg;
    const float angle = advanceDisplaySweepAngle(now);
    if (had_angle) {
      // Paint targets the spoke just passed before the spoke itself, so the line
      // stays on top of anything it reveals this frame.
      revealAircraftUnderBeam(prev_angle, angle);
    }
    updateSweepOnPanel(angle);
  } else if (s_sweep_track_valid) {
    blitRegionToPanel(s_prev_sweep_dirty);
    tft.setTextDatum(TextDatum::TopLeft);
    s_sweep_track_valid = false;
  }
}

void radarDisplayDraw() {
  initPalette();
  initLabelMetrics();
  s_aircraft_dirty = false;
  s_aircraft_dirty_rect = {};
  s_content_base_valid = false;
  s_sweep_track_valid = false;
  s_display_sweep_init = false;
  s_shown_marker_count = 0;

  if (rebuildBackgroundSprite() && ensureContentSprite()) {
    blitStatic();
    return;
  }

  syncShownMarkersToLive();
  const DrawScope scope(tft);
  drawStaticGrid(tft);
  if (displayPrefsSweepLineEnabled()) {
    resetDisplaySweepAngle(millis());
    drawSweepAtOn(tft, s_display_sweep_deg);
  }
  drawAircraft();
  tft.setTextDatum(TextDatum::TopLeft);
}

void radarDisplayRefreshAircraft() {
  initPalette();

  if (!s_bg_ready) {
    rebuildBackgroundSprite();
  }

  // PPI behaviour: hand the new ADS-B picture to the sweep instead of pushing it
  // now. radarDisplayRefreshSweep() copies each target into the shown snapshot as
  // the spoke crosses its bearing, so blips update under the beam.
  if (radar::kSweepPaintsAircraft && displayPrefsSweepLineEnabled() && s_bg_ready &&
      ensureContentSprite()) {
    planReveal();
    if (config::kRadarSweepTraceDebug) {
      Serial.println("[sweep] aircraft_refresh queued for beam reveal");
    }
    return;
  }

  const size_t curr_count = collectAircraftMarkers(s_current_aircraft_markers,
                                                    services::adsb::kMaxAircraft);
  const IntRect dirty =
      unionChangedMarkerBounds(s_current_aircraft_markers, curr_count);
  if (rectEmpty(dirty)) {
    return;
  }

  hardware::gfxLogf("[radar] adsb dirty %dx%d @ (%d,%d)", dirty.w, dirty.h, dirty.x, dirty.y);
  if (config::kSerialTraceDebug) {
    Serial.printf("[radar] adsb dirty %dx%d @ (%d,%d)\n", dirty.w, dirty.h, dirty.x, dirty.y);
  }

  // Sweep line off (or no offscreen buffer): there is no beam to paint the change
  // in, so apply it screen-wide right away and keep blips on the ADS-B poll (~2s).
  if (!ensureContentSprite()) {
    s_aircraft_dirty = true;
    s_aircraft_dirty_rect = dirty;
    s_aircraft_sync_pending = true;
    if (config::kRadarResumeDebug) {
      static unsigned long s_last_defer_ms = 0;
      const unsigned long now_ms = millis();
      if (now_ms - s_last_defer_ms >= 2000UL) {
        Serial.printf("[radar] ac_refresh_defer bg=%d content=%d base=%d heap=%u max_blk=%u\n",
                      s_bg_ready ? 1 : 0, s_content_ready ? 1 : 0,
                      s_content_base_valid ? 1 : 0, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
        s_last_defer_ms = now_ms;
      }
    }
    if (config::kRadarSweepTraceDebug) {
      Serial.println("[sweep] aircraft_refresh deferred (no content sprite)");
    }
    return;
  }

  syncShownMarkersToLive();
  if (!rebuildContentBase()) {
    s_aircraft_dirty = true;
    s_aircraft_dirty_rect = dirty;
    if (config::kRadarSweepTraceDebug) {
      Serial.println("[sweep] aircraft_refresh deferred (content rebuild failed)");
    }
    return;
  }

  PanelSession panel(tft);
  // Clear the prior sweep spoke from the panel so it isn't orphaned by our blit;
  // the sweep redraws on the next animation frame.
  if (s_sweep_track_valid && !rectEmpty(s_prev_sweep_dirty)) {
    blitRegionToPanel(s_prev_sweep_dirty);
  }

  IntRect patch = clampRectToScreen(dirty);
  constexpr int kScreenPixels = radar::kSize * radar::kSize;
  if (patch.w * patch.h >= kScreenPixels / 3) {
    s_content.pushSprite(0, 0);
  } else {
    blitRegionToPanel(patch);
  }
  tft.setTextDatum(TextDatum::TopLeft);

  s_sweep_track_valid = false;
  s_aircraft_dirty = false;
  s_aircraft_dirty_rect = {};
}

size_t radarDisplayInRangeAircraftCount() { return inRangeAircraftCount(); }

size_t radarDisplayVisibleAircraftCount() { return visibleAircraftCount(); }

bool radarDisplayIsInRange(const services::adsb::Aircraft& ac) { return isAircraftInRange(ac); }

void radarDisplayInvalidateAircraft() {
  s_aircraft_dirty = true;
  s_aircraft_dirty_rect = IntRect{0, 0, radar::kSize, radar::kSize};
  s_content_base_valid = false;
}

void radarDisplayReleasePressureSprites() {
  if (s_content_ready) {
    s_content.deleteSprite();
    s_content_ready = false;
    s_content_base_valid = false;
    s_sweep_track_valid = false;
  }
  if (s_bg_ready) {
    s_bg.deleteSprite();
    s_bg_ready = false;
    s_content_base_valid = false;
    s_sweep_track_valid = false;
  }
}

void drawOrientationHints(PlaneGfx& gfx) {
  char facing_buf[16];
  radar::facingLabel(facing_buf, sizeof(facing_buf));
  char line[40];
  snprintf(line, sizeof(line), "Facing %s", facing_buf);

  // Body size matches settings rows; draw into a sprite/offscreen buffer so
  // CO5300 2x2 pixelAlign does not chunk the glyphs.
  displayFontApply(gfx, displayFontBody());
  gfx.setTextDatum(TextDatum::TopCenter);
  gfx.setTextColor(radar::kColorLabel, radar::kColorBackground);
  gfx.drawString(line, radar::kCenterX, 28);
  gfx.drawString("Tap to save", radar::kCenterX, radar::kSize - 48);
  gfx.setTextDatum(TextDatum::TopLeft);
}

void radarDisplayDrawOrientationPreview() {
  initPalette();
  initLabelMetrics();
  s_force_cardinals = true;

  // Rebuild offscreen and push once — avoids the live fillScreen blank between
  // each 5° dial step that direct-to-panel redraw caused.
  if (rebuildBackgroundSprite()) {
    drawOrientationHints(s_bg.gfx());
    s_bg.pushSprite(0, 0);
  } else if (tft.beginOffscreen()) {
    {
      const DrawScope scope(tft);
      drawStaticGrid(tft);
      drawOrientationHints(tft);
    }
    tft.endOffscreen();
  } else {
    const DrawScope scope(tft);
    drawStaticGrid(tft);
    drawOrientationHints(tft);
  }

  s_force_cardinals = false;
}

bool radarDisplayDebugBgReady() { return s_bg_ready; }

bool radarDisplayDebugContentReady() { return s_content_ready; }

bool radarDisplayDebugContentBaseValid() { return s_content_base_valid; }

}  // namespace ui
