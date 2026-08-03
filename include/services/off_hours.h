#pragma once

#include <cstdint>

namespace services::offhours {

enum class Mode : uint8_t {
  Dim = 0,
  DisplayOff = 1,
  SleepMessage = 2,
};

void bootLoad();

/** True when the schedule is enabled AND current local time is outside the
 *  awake window (or on a non-awake day). Always false until NTP has synced. */
bool active();

/** Current configured mode (only meaningful when active). */
Mode mode();

bool enabled();
/** Awake window: the device runs normally inside [start, end). */
uint16_t awakeStartMinute();
uint16_t awakeEndMinute();
/** Awake days bitmask, bit 0 = Sunday ... bit 6 = Saturday. */
uint8_t awakeDayMask();

void saveFromForm(const char* enable_checkbox, const char* mode_str,
                  const char* start_str, const char* end_str, uint8_t day_mask);

}  // namespace services::offhours
