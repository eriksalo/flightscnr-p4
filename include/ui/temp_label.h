#pragma once

#include <cstdint>

#include "hardware/display_font.h"

namespace ui::temp_label {

/** Visible width of "<value>°<unit>" with a drawn degree ring. */
int visibleWidth(UiTextStyle style, int value, char unit);

/** Draw "<value>°<unit>" with top-left at (x, top_y); returns width drawn. */
int drawAt(int x, int top_y, int value, char unit, UiTextStyle style, uint16_t fg,
           uint16_t bg);

/** Draw "<value>°<unit>" centered at cx (top at top_y). */
void drawCentered(int cx, int top_y, int value, char unit, UiTextStyle style, uint16_t fg,
                  uint16_t bg);

}  // namespace ui::temp_label
