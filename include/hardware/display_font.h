#pragma once

#include "hardware/plane_gfx.h"

/** Montserrat Bold GFXfont preset for UI text. */
struct UiTextStyle {
  const GFXfont* font;
};

UiTextStyle displayFontTitle();
UiTextStyle displayFontBody();
UiTextStyle displayFontDetail();
UiTextStyle displayFontCardinal();
UiTextStyle displayFontScale();
UiTextStyle displayFontTag();
/** Native large sizes for clock (avoid textSize scaling — looks pixelated). */
UiTextStyle displayFontClockTime();
UiTextStyle displayFontClockAmPm();
UiTextStyle displayFontClockDate();

/** Pick the font whose cap height is closest to target_px (inclusive index range). */
UiTextStyle displayFontPickForHeight(PlaneGfx& gfx, int target_px, size_t lo_index,
                                     size_t hi_index);

void displayFontApply(PlaneGfx& gfx, UiTextStyle style);

/** Physical-resolution (× 24/13) variant of a logical font, for full-res text
 *  on the 720×720 panel; nullptr when no physical variant exists. */
const GFXfont* displayFontPhysical(const GFXfont* logical);

int displayFontHeight(PlaneGfx& gfx, UiTextStyle style);
int displayFontWidth(PlaneGfx& gfx, UiTextStyle style, const char* text);
