#include "ui/sleep_screen.h"

#include <Arduino.h>
#include <cmath>

#include "config.h"
#include "hardware/display.h"
#include "hardware/display_font.h"

namespace ui {
namespace {

constexpr int kLineGap = 10;
/** Relocate the message on this cadence so nothing burns into the panel. */
constexpr unsigned long kMoveIntervalMs = 15000;
/** Drift orbit radius around panel center; keeps both lines inside the circle. */
constexpr int kDriftRadiusPx = 80;
constexpr int kDriftSteps = 12;

const int kCenterX = config::kDisplayWidth / 2;
const int kCenterY = config::kDisplayHeight / 2;

/** Dim gray-green so the sleeping panel stays unobtrusive at night. */
constexpr uint16_t kMsgColor = 0x4C6A;

int s_drift_step = 0;
unsigned long s_last_move_ms = 0;

void drawMessage() {
  const float angle =
      2.0f * static_cast<float>(M_PI) * static_cast<float>(s_drift_step) / kDriftSteps;
  const int cx = kCenterX + static_cast<int>(lroundf(sinf(angle) * kDriftRadiusPx));
  const int cy = kCenterY - static_cast<int>(lroundf(cosf(angle) * kDriftRadiusPx));

  tft.beginOffscreen();
  tft.fillScreen(config::kColorBlack);
  tft.setTextDatum(TextDatum::MiddleCenter);
  tft.setTextColor(kMsgColor, config::kColorBlack);

  const UiTextStyle title = displayFontTitle();
  const UiTextStyle body = displayFontBody();
  const int title_h = displayFontHeight(tft, title);
  const int body_h = displayFontHeight(tft, body);
  const int total_h = title_h + kLineGap + body_h;

  int y = cy - total_h / 2;
  displayFontApply(tft, title);
  tft.drawString("Sleeping", cx, y + title_h / 2);
  y += title_h + kLineGap;
  displayFontApply(tft, body);
  tft.drawString("Touch screen to wake", cx, y + body_h / 2);
  tft.endOffscreen();
}

}  // namespace

void sleepScreenEnter() {
  s_drift_step = 0;
  s_last_move_ms = millis();
  drawMessage();
}

void sleepScreenTick() {
  const unsigned long now = millis();
  if (now - s_last_move_ms < kMoveIntervalMs) {
    return;
  }
  s_last_move_ms = now;
  s_drift_step = (s_drift_step + 1) % kDriftSteps;
  drawMessage();
}

}  // namespace ui
