#include "hardware/display_font.h"

#include <algorithm>
#include <cstdlib>

#include "fonts/MontserratBold10pt7b.h"
#include "fonts/MontserratBold11pt7b.h"
#include "fonts/MontserratBold12pt7b.h"
#include "fonts/MontserratBold14pt7b.h"
#include "fonts/MontserratBold15pt7b.h"
#include "fonts/MontserratBold17pt7b.h"
#include "fonts/MontserratBold18pt7b.h"
#include "fonts/MontserratBold20pt7b.h"
#include "fonts/MontserratBold22pt7b.h"
#include "fonts/MontserratBold24pt7b.h"
#include "fonts/MontserratBold26pt7b.h"
#include "fonts/MontserratBold4pt7b.h"
#include "fonts/MontserratBold63pt7b.h"
#include "fonts/MontserratBold8pt7b.h"
#include "fonts/MontserratBold9pt7b.h"

namespace {

/** Picker ladder for dynamic sizing (indexes are caller-visible: keep order). */
const GFXfont* kFonts[] = {
    &MontserratBold8pt7b,
    &MontserratBold9pt7b,
    &MontserratBold10pt7b,
    &MontserratBold11pt7b,
    &MontserratBold12pt7b,
    &MontserratBold14pt7b,
    &MontserratBold18pt7b,
};

constexpr size_t kFontCount = sizeof(kFonts) / sizeof(kFonts[0]);

int absDiff(int a, int b) { return std::abs(a - b); }

}  // namespace

// Native 720×720 presets, sized for the 3.4" glass: denser and finer than the
// legacy 390-space design (which blew up smartwatch-scale type ~1.85×).

UiTextStyle displayFontTitle() { return UiTextStyle{&MontserratBold24pt7b}; }

UiTextStyle displayFontBody() { return UiTextStyle{&MontserratBold15pt7b}; }

UiTextStyle displayFontDetail() { return UiTextStyle{&MontserratBold12pt7b}; }

UiTextStyle displayFontCardinal() { return UiTextStyle{&MontserratBold14pt7b}; }

UiTextStyle displayFontScale() { return UiTextStyle{&MontserratBold10pt7b}; }

// Radar aircraft tags: 4pt (10px yAdvance, ~6px cap). Deliberately tiny so every
// aircraft on the scope can carry a label without the tags colliding.
UiTextStyle displayFontTag() { return UiTextStyle{&MontserratBold4pt7b}; }

UiTextStyle displayFontClockTime() { return UiTextStyle{&MontserratBold63pt7b}; }

UiTextStyle displayFontClockAmPm() { return UiTextStyle{&MontserratBold26pt7b}; }

UiTextStyle displayFontClockDate() { return UiTextStyle{&MontserratBold20pt7b}; }

UiTextStyle displayFontPickForHeight(PlaneGfx& gfx, int target_px, size_t lo_index,
                                     size_t hi_index) {
  if (kFontCount == 0) {
    return displayFontBody();
  }
  lo_index = std::min(lo_index, kFontCount - 1);
  hi_index = std::min(hi_index, kFontCount - 1);
  if (hi_index < lo_index) {
    std::swap(lo_index, hi_index);
  }

  size_t best = lo_index;
  int best_diff = absDiff(displayFontHeight(gfx, UiTextStyle{kFonts[best]}), target_px);
  for (size_t i = lo_index; i <= hi_index; ++i) {
    const int diff =
        absDiff(displayFontHeight(gfx, UiTextStyle{kFonts[i]}), target_px);
    if (diff < best_diff) {
      best_diff = diff;
      best = i;
    }
  }
  return UiTextStyle{kFonts[best]};
}

void displayFontApply(PlaneGfx& gfx, UiTextStyle style) {
  gfx.setTextWrap(false);
  gfx.setTextSize(1);
  gfx.setFont(style.font);
}

int displayFontHeight(PlaneGfx& gfx, UiTextStyle style) {
  displayFontApply(gfx, style);
  return gfx.fontHeight();
}

int displayFontWidth(PlaneGfx& gfx, UiTextStyle style, const char* text) {
  displayFontApply(gfx, style);
  return gfx.textWidth(text);
}
