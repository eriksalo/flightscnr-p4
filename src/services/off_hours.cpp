#include "services/off_hours.h"

#include <Arduino.h>
#include <Preferences.h>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "config.h"

namespace services::offhours {
namespace {

constexpr char kStoreNs[] = "flightscnr";
// New key family: semantics flipped from "night window" to "awake window";
// the old night_* keys are orphaned so stale values can't invert the schedule.
constexpr char kEnabledKey[] = "sch_en";
constexpr char kModeKey[] = "sch_mode";
constexpr char kStartKey[] = "sch_start";
constexpr char kEndKey[] = "sch_end";
constexpr char kDaysKey[] = "sch_days";

bool s_enabled = true;
Mode s_mode = Mode::SleepMessage;
uint16_t s_awake_start_min = config::kAwakeDefaultStartMin;
uint16_t s_awake_end_min = config::kAwakeDefaultEndMin;
uint8_t s_day_mask = config::kAwakeDefaultDayMask;

uint16_t parseTimeStr(const char* str, uint16_t fallback) {
  if (str == nullptr || str[0] == '\0') {
    return fallback;
  }
  const char* colon = strchr(str, ':');
  if (colon == nullptr) {
    return fallback;
  }
  int h = atoi(str);
  int m = atoi(colon + 1);
  if (h < 0 || h > 23 || m < 0 || m > 59) {
    return fallback;
  }
  return static_cast<uint16_t>(h * 60 + m);
}

Mode parseMode(uint8_t raw) {
  switch (raw) {
    case 0:
      return Mode::Dim;
    case 1:
      return Mode::DisplayOff;
    default:
      return Mode::SleepMessage;
  }
}

void persist() {
  Preferences prefs;
  if (prefs.begin(kStoreNs, false)) {
    prefs.putBool(kEnabledKey, s_enabled);
    prefs.putUChar(kModeKey, static_cast<uint8_t>(s_mode));
    prefs.putUShort(kStartKey, s_awake_start_min);
    prefs.putUShort(kEndKey, s_awake_end_min);
    prefs.putUChar(kDaysKey, s_day_mask);
    prefs.end();
  }
}

}  // namespace

void bootLoad() {
  Preferences prefs;
  if (!prefs.begin(kStoreNs, true)) {
    return;
  }
  s_enabled = prefs.getBool(kEnabledKey, true);
  s_mode = parseMode(prefs.getUChar(kModeKey, static_cast<uint8_t>(Mode::SleepMessage)));
  s_awake_start_min = prefs.getUShort(kStartKey, config::kAwakeDefaultStartMin);
  s_awake_end_min = prefs.getUShort(kEndKey, config::kAwakeDefaultEndMin);
  s_day_mask = prefs.getUChar(kDaysKey, config::kAwakeDefaultDayMask);
  prefs.end();
}

bool active() {
  if (!s_enabled) {
    return false;
  }
  struct tm local {};
  const time_t now = time(nullptr);
  if (localtime_r(&now, &local) == nullptr) {
    return false;
  }
  // Stay awake until NTP has synced — a 1970 clock would sleep the wrong hours.
  if (local.tm_year + 1900 < 2020) {
    return false;
  }
  if ((s_day_mask & (1u << local.tm_wday)) == 0) {
    return true;
  }
  if (s_awake_start_min == s_awake_end_min) {
    return false;  // degenerate window: treat as always awake
  }
  const uint16_t now_min = static_cast<uint16_t>(local.tm_hour * 60 + local.tm_min);
  bool awake;
  if (s_awake_start_min < s_awake_end_min) {
    awake = now_min >= s_awake_start_min && now_min < s_awake_end_min;
  } else {
    // Overnight awake window (e.g. 22:00 - 06:00)
    awake = now_min >= s_awake_start_min || now_min < s_awake_end_min;
  }
  return !awake;
}

Mode mode() { return s_mode; }
bool enabled() { return s_enabled; }
uint16_t awakeStartMinute() { return s_awake_start_min; }
uint16_t awakeEndMinute() { return s_awake_end_min; }
uint8_t awakeDayMask() { return s_day_mask; }

void saveFromForm(const char* enable_checkbox, const char* mode_str,
                  const char* start_str, const char* end_str, uint8_t day_mask) {
  s_enabled = (enable_checkbox != nullptr && enable_checkbox[0] == 'T');
  if (mode_str != nullptr && mode_str[0] != '\0') {
    s_mode = parseMode(static_cast<uint8_t>(atoi(mode_str)));
  }
  s_awake_start_min = parseTimeStr(start_str, s_awake_start_min);
  s_awake_end_min = parseTimeStr(end_str, s_awake_end_min);
  s_day_mask = day_mask;
  persist();
  Serial.printf(
      "[offhours] saved en=%d mode=%d awake=%02u:%02u-%02u:%02u days=0x%02x\n",
      s_enabled ? 1 : 0, static_cast<int>(s_mode), s_awake_start_min / 60,
      s_awake_start_min % 60, s_awake_end_min / 60, s_awake_end_min % 60, s_day_mask);
}

}  // namespace services::offhours
