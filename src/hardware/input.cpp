#include "hardware/input.h"

#include <Arduino.h>

#include <cmath>

#include "config.h"
#include "hardware/buzzer.h"
#include "hardware/pin_config.h"

// lib/waveshare_displays: GT911 over the shared I2C bus (SDA=7 SCL=8, RST=23).
#include "gt911.h"
#include "i2c.h"

namespace {

portMUX_TYPE s_input_mux = portMUX_INITIALIZER_UNLOCKED;

volatile int16_t s_tap_x = -1;
volatile int16_t s_tap_y = -1;
volatile SwipeGesture s_swipe_pending = SwipeNone;

esp_lcd_touch_handle_t s_touch = nullptr;
bool s_touch_ready = false;
bool s_touch_tracking = false;
int16_t s_touch_start_x = 0;
int16_t s_touch_start_y = 0;
int16_t s_touch_last_x = 0;
int16_t s_touch_last_y = 0;
unsigned long s_touch_last_down_ms = 0;

/** The GT911 reports the finger as present only intermittently during a drag
 *  (status flickers between polls). Treat contact as ended only after this
 *  long with no reported point, so one continuous swipe stays one gesture
 *  instead of fragmenting into per-sample taps. */
constexpr unsigned long kTouchReleaseMs = 100;

// Thresholds in native 720px panel space (GT911 reports panel coords).
constexpr int kSwipeMinPx = 130;
constexpr int kTapMaxPx = 46;

/** GT911 reports native panel coords — same space the UI works in. */
int16_t physToLogical(uint16_t v) {
  if (v >= config::kDisplayWidth) {
    return static_cast<int16_t>(config::kDisplayWidth - 1);
  }
  return static_cast<int16_t>(v);
}

void queueSwipe(SwipeGesture gesture) {
  portENTER_CRITICAL(&s_input_mux);
  s_swipe_pending = gesture;
  portEXIT_CRITICAL(&s_input_mux);
}

void queueTap(int16_t x, int16_t y) {
  portENTER_CRITICAL(&s_input_mux);
  s_tap_x = x;
  s_tap_y = y;
  portEXIT_CRITICAL(&s_input_mux);
}

void finishTouchGesture() {
  const int dx = s_touch_last_x - s_touch_start_x;
  const int dy = s_touch_last_y - s_touch_start_y;
  const int adx = std::abs(dx);
  const int ady = std::abs(dy);

  if (dx <= -kSwipeMinPx && ady * 2 < adx) {
    queueSwipe(SwipeLeft);
  } else if (dx >= kSwipeMinPx && ady * 2 < adx) {
    queueSwipe(SwipeRight);
  } else if (dy >= kSwipeMinPx && adx * 2 < ady) {
    queueSwipe(SwipeDown);
  } else if (dy <= -kSwipeMinPx && adx * 2 < ady) {
    queueSwipe(SwipeUp);
  } else if (adx <= kTapMaxPx && ady <= kTapMaxPx) {
    queueTap(s_touch_last_x, s_touch_last_y);
  }
}

void pollTouchGt911() {
  uint16_t x[ESP_LCD_TOUCH_MAX_POINTS] = {};
  uint16_t y[ESP_LCD_TOUCH_MAX_POINTS] = {};
  uint16_t strength[ESP_LCD_TOUCH_MAX_POINTS] = {};
  uint8_t count = 0;

  esp_lcd_touch_read_data(s_touch);
  const bool down = esp_lcd_touch_get_coordinates(s_touch, x, y, strength, &count,
                                                  ESP_LCD_TOUCH_MAX_POINTS) &&
                    count > 0;

  const unsigned long now = millis();
  if (down) {
    const int16_t lx = physToLogical(x[0]);
    const int16_t ly = physToLogical(y[0]);
    if (!s_touch_tracking) {
      // First contact of a new gesture: anchor the start point.
      s_touch_start_x = lx;
      s_touch_start_y = ly;
      s_touch_tracking = true;
      hardware::buzzerClick();
    }
    s_touch_last_x = lx;
    s_touch_last_y = ly;
    s_touch_last_down_ms = now;
  } else if (s_touch_tracking && now - s_touch_last_down_ms >= kTouchReleaseMs) {
    // Real lift-off: no point reported for kTouchReleaseMs.
    finishTouchGesture();
    s_touch_tracking = false;
  }
}

}  // namespace

void inputInit() {
  DEV_I2C_Port port = DEV_I2C_Init();
  s_touch = touch_gt911_init(port);
  s_touch_ready = s_touch != nullptr;
  Serial.println(s_touch_ready ? "GT911 touch ready" : "GT911 touch init failed");
}

void inputPoll() {
  if (s_touch_ready) {
    pollTouchGt911();
  }
}

// --- Knob / encoder: the 3.4C has no rotary encoder or knob button. ---
// Zoom is driven by tapping the range label (already in the UI); Wi-Fi reset
// via the web portal. These stubs keep the shared UI code paths compiling.

int8_t inputConsumeEncoderDelta() { return 0; }

bool inputConsumeKnobTap() { return false; }

bool inputConsumeKnobPress() { return false; }

bool inputConsumeScreenTap(int16_t* x, int16_t* y) {
  portENTER_CRITICAL(&s_input_mux);
  const bool tap = s_tap_x >= 0 && s_tap_y >= 0;
  if (tap) {
    if (x != nullptr) {
      *x = s_tap_x;
    }
    if (y != nullptr) {
      *y = s_tap_y;
    }
    s_tap_x = -1;
    s_tap_y = -1;
  }
  portEXIT_CRITICAL(&s_input_mux);
  return tap;
}

SwipeGesture inputConsumeSwipe() {
  portENTER_CRITICAL(&s_input_mux);
  const SwipeGesture swipe = s_swipe_pending;
  if (swipe != SwipeNone) {
    s_swipe_pending = SwipeNone;
  }
  portEXIT_CRITICAL(&s_input_mux);
  return swipe;
}

void inputDiscardPendingInteractions() {
  portENTER_CRITICAL(&s_input_mux);
  s_swipe_pending = SwipeNone;
  s_tap_x = -1;
  s_tap_y = -1;
  portEXIT_CRITICAL(&s_input_mux);
}

void inputPollLongPress() {}

bool inputConsumeWifiResetUiCancelled() { return false; }
