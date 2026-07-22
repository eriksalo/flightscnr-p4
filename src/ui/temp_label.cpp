#include "ui/temp_label.h"

#include <cstdio>

#include "hardware/display.h"
#include "hardware/display_font.h"

namespace ui::temp_label {
namespace {

// Generous gaps so the opaque text cells (drawString paints fg-on-bg) never
// overlap — and thus never clip — the drawn degree ring between them.
constexpr int kDegGap1 = 5;
constexpr int kDegGap2 = 4;

int ringRadius(int temp_h) {
  if (temp_h >= 44) {
    return 8;
  }
  if (temp_h >= 28) {
    return 6;
  }
  return 4;
}

int ringThickness(int r) {
  if (r >= 7) {
    return 3;
  }
  return r >= 5 ? 2 : 1;
}

void drawRing(int cx, int cy, int r, uint16_t fg, uint16_t bg) {
  const int thick = ringThickness(r);
  tft.fillCircle(cx, cy, r, fg);
  if (r > thick) {
    tft.fillCircle(cx, cy, r - thick, bg);
  }
}

int superscriptCy(int top_y, int r) { return top_y + r - 1; }

}  // namespace

int visibleWidth(UiTextStyle style, int value, char unit) {
  char num[8];
  snprintf(num, sizeof(num), "%d", value);
  char us[2] = {unit, '\0'};
  displayFontApply(tft, style);
  const int th = displayFontHeight(tft, style);
  const int r = ringRadius(th);
  return tft.textWidth(num) + kDegGap1 + (2 * r + 1) + kDegGap2 + tft.textWidth(us);
}

int drawAt(int x, int top_y, int value, char unit, UiTextStyle style, uint16_t fg,
           uint16_t bg) {
  char num[8];
  snprintf(num, sizeof(num), "%d", value);
  char us[2] = {unit, '\0'};
  displayFontApply(tft, style);
  const int th = displayFontHeight(tft, style);
  const int r = ringRadius(th);
  const int ringw = 2 * r + 1;
  const int numw = tft.textWidth(num);
  tft.setTextDatum(TextDatum::TopLeft);
  tft.setTextColor(fg, bg);
  // Draw both text cells first, then the ring on top, so the opaque unit/number
  // backgrounds can never paint over the ring's edge.
  tft.drawString(num, x, top_y);
  tft.drawString(us, x + numw + kDegGap1 + ringw + kDegGap2, top_y);
  drawRing(x + numw + kDegGap1 + r, superscriptCy(top_y, r), r, fg, bg);
  return numw + kDegGap1 + ringw + kDegGap2 + tft.textWidth(us);
}

void drawCentered(int cx, int top_y, int value, char unit, UiTextStyle style, uint16_t fg,
                  uint16_t bg) {
  const int w = visibleWidth(style, value, unit);
  drawAt(cx - w / 2, top_y, value, unit, style, fg, bg);
}

}  // namespace ui::temp_label
