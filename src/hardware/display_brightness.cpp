#include "hardware/display_brightness.h"

#include <Arduino.h>
#include <Preferences.h>
#include <cstdlib>

#include "hardware/display.h"
#include "hardware/pin_config.h"

namespace hardware {

namespace {

constexpr char kStoreNs[] = "flightscnr";
constexpr char kBrightPctKey[] = "bright_pct";

constexpr uint8_t kLevels[] = {20, 40, 60, 80, 100};
constexpr size_t kLevelCount = sizeof(kLevels) / sizeof(kLevels[0]);

uint8_t s_percent = 100;

size_t levelIndexFor(uint8_t pct) {
  for (size_t i = 0; i < kLevelCount; ++i) {
    if (kLevels[i] == pct) {
      return i;
    }
  }
  return kLevelCount - 1;
}

/** Backlight LEDC duty for a brightness percent. Active-low: 0 = full on. */
void applyBacklightPercent(uint8_t pct) {
  const uint32_t duty = 255u - ((static_cast<uint32_t>(pct) * 255u + 50u) / 100u);
  ledcWrite(LCD_BACKLIGHT, duty);
}

}  // namespace

void displayBrightnessBootLoad() {
  Preferences prefs;
  if (!prefs.begin(kStoreNs, true)) {
    return;
  }
  const uint8_t stored = prefs.getUChar(kBrightPctKey, 100);
  prefs.end();
  s_percent = kLevels[levelIndexFor(stored)];
}

uint8_t displayBrightnessPercent() { return s_percent; }

void displayApplyBrightness() { applyBacklightPercent(s_percent); }

void displayBrightnessStep(int8_t delta) {
  if (delta == 0) {
    return;
  }

  size_t idx = levelIndexFor(s_percent);
  if (delta > 0) {
    idx = (idx + 1) % kLevelCount;
  } else {
    idx = (idx == 0) ? kLevelCount - 1 : idx - 1;
  }
  s_percent = kLevels[idx];

  Preferences prefs;
  if (prefs.begin(kStoreNs, false)) {
    prefs.putUChar(kBrightPctKey, s_percent);
    prefs.end();
  }

  displayApplyBrightness();
  Serial.printf("Brightness: %u%%\n", static_cast<unsigned>(s_percent));
}

void displayBrightnessSaveFromForm(const char* percent_str) {
  if (percent_str == nullptr || percent_str[0] == '\0') {
    return;
  }
  char* end = nullptr;
  const long v = strtol(percent_str, &end, 10);
  if (end == percent_str || (end != nullptr && *end != '\0')) {
    return;
  }
  if (v < 0 || v > 100) {
    return;
  }
  s_percent = kLevels[levelIndexFor(static_cast<uint8_t>(v))];

  Preferences prefs;
  if (prefs.begin(kStoreNs, false)) {
    prefs.putUChar(kBrightPctKey, s_percent);
    prefs.end();
  }
  Serial.printf("Brightness: %u%%\n", static_cast<unsigned>(s_percent));
}

void displayBrightnessOverride(uint8_t percent) { applyBacklightPercent(percent); }

void displayBrightnessRestore() { displayApplyBrightness(); }

}  // namespace hardware
