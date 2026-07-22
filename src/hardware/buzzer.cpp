// Waveshare 3.4C: no piezo buzzer. The T-Encoder Pro's buzzer pin (GPIO 17)
// is SDIO D3 to the ESP32-C6 on this board — it must never be driven.
// Settings (enable + tone) are still persisted so the web UI round-trips;
// the board's ES8311 codec + speaker could take over click/alert later.
#include "hardware/buzzer.h"

#include <Arduino.h>
#include <Preferences.h>

namespace hardware {

namespace {

constexpr char kStoreNs[] = "flightscnr";
constexpr char kEnabledKey[] = "beep_en";
constexpr char kToneKey[] = "beep_vol";

constexpr char kToneLetters[] = {'A', 'B', 'C', 'D', 'E'};
constexpr size_t kToneLevelCount = sizeof(kToneLetters) / sizeof(kToneLetters[0]);
constexpr uint8_t kDefaultToneIndex = 0;  // A

bool s_enabled = true;
uint8_t s_tone_index = kDefaultToneIndex;

void persist() {
  Preferences prefs;
  if (prefs.begin(kStoreNs, false)) {
    prefs.putBool(kEnabledKey, s_enabled);
    prefs.putUChar(kToneKey, s_tone_index);
    prefs.end();
  }
}

}  // namespace

void buzzerInit() {}

void buzzerBootLoad() {
  Preferences prefs;
  if (!prefs.begin(kStoreNs, true)) {
    return;
  }
  s_enabled = prefs.getBool(kEnabledKey, true);
  const uint8_t stored = prefs.getUChar(kToneKey, kDefaultToneIndex);
  prefs.end();
  s_tone_index = (stored < kToneLevelCount) ? stored : kDefaultToneIndex;
}

bool buzzerEnabled() { return s_enabled; }

char buzzerToneLetter() { return kToneLetters[s_tone_index]; }

void buzzerSetEnabled(bool enabled) {
  s_enabled = enabled;
  persist();
}

void buzzerToneStep(int8_t delta) {
  if (delta == 0) {
    return;
  }
  if (delta > 0) {
    s_tone_index = (s_tone_index + 1) % kToneLevelCount;
  } else {
    s_tone_index = (s_tone_index == 0) ? kToneLevelCount - 1 : s_tone_index - 1;
  }
  persist();
}

void buzzerClick() {}

void buzzerAlert() {}

void buzzerPoll() {}

void saveBeepEnabledFromForm(const char* checkbox_value) {
  s_enabled = (checkbox_value != nullptr);
  persist();
}

void saveBeepToneFromForm(const char* value) {
  if (value == nullptr || value[0] == '\0') {
    return;
  }
  const char c = value[0];
  for (size_t i = 0; i < kToneLevelCount; ++i) {
    if (kToneLetters[i] == c || kToneLetters[i] == (c - 32)) {
      s_tone_index = static_cast<uint8_t>(i);
      persist();
      return;
    }
  }
}

}  // namespace hardware
