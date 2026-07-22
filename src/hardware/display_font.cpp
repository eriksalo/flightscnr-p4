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
#include "fonts/MontserratBold33pt7b.h"
#include "fonts/MontserratBold36pt7b.h"
#include "fonts/MontserratBold44pt7b.h"
#include "fonts/MontserratBold63pt7b.h"
#include "fonts/MontserratBold8pt7b.h"
#include "fonts/MontserratBold9pt7b.h"

namespace {

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

/** Logical font → same face at point size × 24/13 (rounded), rendered at the
 *  panel's physical resolution by PlaneGfx. Generated with
 *  tools/ttf_to_gfxfont.py like the logical set. */
struct PhysFontMap {
  const GFXfont* logical;
  const GFXfont* physical;
};

const PhysFontMap kPhysFonts[] = {
    {&MontserratBold8pt7b, &MontserratBold15pt7b},
    {&MontserratBold9pt7b, &MontserratBold17pt7b},
    {&MontserratBold10pt7b, &MontserratBold18pt7b},
    {&MontserratBold11pt7b, &MontserratBold20pt7b},
    {&MontserratBold12pt7b, &MontserratBold22pt7b},
    {&MontserratBold14pt7b, &MontserratBold26pt7b},
    {&MontserratBold18pt7b, &MontserratBold33pt7b},
    {&MontserratBold24pt7b, &MontserratBold44pt7b},
    // 36 × 24/13 ≈ 66pt, but a full-ASCII 66pt bitmap overflows GFXglyph's
    // uint16_t bitmapOffset; 63pt is the largest size that fits.
    {&MontserratBold36pt7b, &MontserratBold63pt7b},
};

int absDiff(int a, int b) { return std::abs(a - b); }

}  // namespace

UiTextStyle displayFontTitle() { return UiTextStyle{&MontserratBold18pt7b}; }

UiTextStyle displayFontBody() { return UiTextStyle{&MontserratBold12pt7b}; }

UiTextStyle displayFontDetail() { return UiTextStyle{&MontserratBold10pt7b}; }

UiTextStyle displayFontCardinal() { return UiTextStyle{&MontserratBold12pt7b}; }

UiTextStyle displayFontScale() { return UiTextStyle{&MontserratBold9pt7b}; }

UiTextStyle displayFontTag() { return UiTextStyle{&MontserratBold11pt7b}; }

UiTextStyle displayFontClockTime() { return UiTextStyle{&MontserratBold36pt7b}; }

UiTextStyle displayFontClockAmPm() { return UiTextStyle{&MontserratBold24pt7b}; }

UiTextStyle displayFontClockDate() { return UiTextStyle{&MontserratBold18pt7b}; }

const GFXfont* displayFontPhysical(const GFXfont* logical) {
  if (logical == nullptr) {
    return nullptr;
  }
  for (const PhysFontMap& m : kPhysFonts) {
    if (m.logical == logical) {
      return m.physical;
    }
  }
  return nullptr;
}

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
