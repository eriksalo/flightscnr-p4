#pragma once

#include <cstdint>

class PlaneGfx;

namespace services::weather_icon {

/** True when a bitmap icon exists for this full-day weather code. */
bool hasIcon(int code);

/** Decode and draw the icon centered horizontally at center_x, top at y.
 *  Transparent pixels are filled with bg. Returns false when no icon exists. */
bool drawIcon(PlaneGfx& tft, int code, int16_t center_x, int16_t y, uint16_t bg);

/** Like drawIcon but nearest-neighbor scaled to target_size (square). Used where
 *  the native icon is too large for the layout (e.g. the clock screen). */
bool drawIconScaled(PlaneGfx& tft, int code, int16_t center_x, int16_t y, uint16_t bg,
                    int target_size);

/** Width/height of the icon for layout, or 0 when none. */
int iconWidth(int code);
int iconHeight(int code);

/** True when the dedicated sunrise/sunset glyphs are bundled in flash. */
bool hasSunIcons();

/** Native size of the sunrise/sunset glyph (square), or 0 when unavailable. */
int sunIconSize();

/** Draw the sunrise (sunset=false) or sunset glyph centered at center_x, top y.
 *  Transparent pixels are filled with bg. Returns false when unavailable. */
bool drawSunIcon(PlaneGfx& tft, bool sunset, int16_t center_x, int16_t y, uint16_t bg);

/** Free the decode frame buffer (call when leaving weather screens). */
void releaseBuffer();

}  // namespace services::weather_icon
