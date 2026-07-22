#include "hardware/display.h"

#include <Arduino.h>

#include "hardware/display_brightness.h"
#include "hardware/panel.h"
#include "hardware/pin_config.h"
#include "hardware/scaled_canvas.h"

#include "displays_config.h"  // lib/waveshare_displays: JD9365 init table + DSI timings

namespace {

Arduino_ESP32DSIPanel* s_dsi_panel = nullptr;
Arduino_DSI_Display* s_panel = nullptr;
ScaledCanvas* s_scaled = nullptr;

}  // namespace

PlaneGfx tft;

void displayInit() {
  // Backlight off (active-low PWM) until the first frame is composed.
  ledcAttach(LCD_BACKLIGHT, 5000, 8);
  ledcWrite(LCD_BACKLIGHT, 255);

  s_dsi_panel = new Arduino_ESP32DSIPanel(
      display_cfg.hsync_pulse_width, display_cfg.hsync_back_porch,
      display_cfg.hsync_front_porch, display_cfg.vsync_pulse_width,
      display_cfg.vsync_back_porch, display_cfg.vsync_front_porch,
      display_cfg.prefer_speed, display_cfg.lane_bit_rate);
  s_panel = new Arduino_DSI_Display(
      display_cfg.width, display_cfg.height, s_dsi_panel, display_cfg.rotation,
      display_cfg.auto_flush, display_cfg.lcd_rst, display_cfg.init_cmds,
      display_cfg.init_cmds_size);

  if (!s_panel->begin()) {
    Serial.println("Display init failed");
  } else {
    Serial.printf("Display: %s %dx%d DSI\n", display_cfg.name, s_panel->width(),
                  s_panel->height());
  }

  // Clear the physical framebuffer once — the 10px border ring around the
  // scaled logical area is never painted by app code.
  s_panel->fillScreen(RGB565_BLACK);

  // The app lays out in its original 390x390 logical space; shapes and text
  // rasterize at the panel's native resolution (see hardware/scaled_canvas.h).
  s_scaled = new ScaledCanvas(s_panel, s_panel->getFramebuffer(),
                              static_cast<int16_t>(s_panel->width()),
                              static_cast<int16_t>(s_panel->height()),
                              /*sync_cache=*/true);
  s_scaled->begin();

  tft.attach(s_scaled, true, s_scaled);
  planeGfxPanelLockInit();
  tft.fillScreen(RGB565_BLACK);

  hardware::displayBrightnessBootLoad();
  hardware::displayApplyBrightness();

  tft.setTextWrap(false);
}

uint16_t* displayFramebuffer() {
  return s_panel != nullptr ? s_panel->getFramebuffer() : nullptr;
}

int displayFbWidth() { return s_panel != nullptr ? s_panel->width() : 0; }

int displayFbHeight() { return s_panel != nullptr ? s_panel->height() : 0; }

void displaySleep() {
  // DSI scanout keeps running; killing the backlight is the real power move.
  ledcWrite(LCD_BACKLIGHT, 255);
}

void displayWake() {
  hardware::displayApplyBrightness();
}
