#pragma once

#include <cstdint>

namespace ui::radar {

/** Accent for grid, compass rose, range labels, and sweep line. */
enum class RadarAccentColor : uint8_t {
  Red = 0,
  Yellow,
  Orange,
  Green,
  White,
};

constexpr uint8_t kRadarAccentCount = 5;

struct AccentRgb {
  uint8_t grid_r;
  uint8_t grid_g;
  uint8_t grid_b;
  uint8_t sweep_r;
  uint8_t sweep_g;
  uint8_t sweep_b;
  uint8_t trail_r;
  uint8_t trail_g;
  uint8_t trail_b;
  uint8_t label_r;
  uint8_t label_g;
  uint8_t label_b;
};

void accentBootLoad();
RadarAccentColor accentColor();
const char* accentColorName();
/** Current accent as a 0-based index into the color list. */
uint8_t accentColorIndex();
/** Display name for a given accent index (empty string when out of range). */
const char* accentColorNameAt(uint8_t index);
AccentRgb accentPalette();
/** Bright sweep RGB for settings UI highlights. */
void accentHighlightRgb(uint8_t* r, uint8_t* g, uint8_t* b);

void accentStep(int8_t delta);

/** Web-form setter: accepts a 0-based index string ("0".."4"). Persists and
 *  returns true when the value was a valid, in-range index. */
bool accentSaveFromForm(const char* value);

}  // namespace ui::radar
