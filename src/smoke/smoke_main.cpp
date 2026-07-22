// Panel-scan calibration pattern for the Waveshare 3.4C (800×800 DSI).
// Draws direct to the DSI framebuffer (no ScaledCanvas) so a photo of the
// glass tells us exactly which framebuffer region the panel presents:
//   - colored border rects inset 0 / 20 / 40 / 60 px (white/red/yellow/green)
//   - center crosshair at (400,400) + circles r=100..390 step 50
//   - coordinate labels along both axes every 100 px

#ifndef BOARD_HAS_PSRAM
#error "PSRAM must be enabled (DSI framebuffer lives in PSRAM)"
#endif

#include <Arduino.h>
#include <Arduino_GFX_Library.h>

#include <esp_cache.h>

#include "displays_config.h"

namespace {

constexpr int8_t kBacklightPin = 26;

Arduino_ESP32DSIPanel* dsipanel = new Arduino_ESP32DSIPanel(
    display_cfg.hsync_pulse_width, display_cfg.hsync_back_porch,
    display_cfg.hsync_front_porch, display_cfg.vsync_pulse_width,
    display_cfg.vsync_back_porch, display_cfg.vsync_front_porch,
    display_cfg.prefer_speed, display_cfg.lane_bit_rate);

Arduino_DSI_Display* gfx = new Arduino_DSI_Display(
    display_cfg.width, display_cfg.height, dsipanel, display_cfg.rotation,
    display_cfg.auto_flush, display_cfg.lcd_rst, display_cfg.init_cmds,
    display_cfg.init_cmds_size);

constexpr uint16_t kWhite = 0xFFFF;
constexpr uint16_t kRed = 0xF800;
constexpr uint16_t kYellow = 0xFFE0;
constexpr uint16_t kGreen = 0x07E0;
constexpr uint16_t kCyan = 0x07FF;

void borderRect(int16_t inset, uint16_t color) {
  const int16_t size = static_cast<int16_t>(800 - 2 * inset);
  gfx->drawRect(inset, inset, size, size, color);
  gfx->drawRect(inset + 1, inset + 1, size - 2, size - 2, color);
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("[cal] calibration pattern");

  ledcAttach(kBacklightPin, 5000, 8);
  ledcWrite(kBacklightPin, 0);  // active-low: full on

  if (!gfx->begin()) {
    Serial.println("[cal] gfx->begin() FAILED");
  }
  gfx->fillScreen(0x0000);

  const int16_t w = gfx->width();
  const int16_t h = gfx->height();
  const int16_t cx2 = w / 2;
  const int16_t cy2 = h / 2;

  // Resolution-agnostic probe: border rects, centered circles, center square.
  gfx->drawRect(0, 0, w, h, kWhite);
  gfx->drawRect(1, 1, w - 2, h - 2, kWhite);
  gfx->drawRect(20, 20, w - 40, h - 40, kRed);
  gfx->drawRect(21, 21, w - 42, h - 42, kRed);
  for (int r = 100; r <= cx2 - 10; r += 100) {
    gfx->drawCircle(cx2, cy2, r, kGreen);
  }
  gfx->drawCircle(cx2, cy2, cx2 - 10, kCyan);  // near-edge ring
  gfx->fillRect(cx2 - 30, cy2 - 30, 60, 60, kWhite);  // CENTER square
  gfx->drawLine(0, cy2, w - 1, cy2, kYellow);
  gfx->drawLine(cx2, 0, cx2, h - 1, kYellow);

  uint16_t* fb = gfx->getFramebuffer();
  Serial.printf("[cal] after white fillRect: fb[400,400]=0x%04X (expect FFFF)\n",
                fb[400 * 800 + 400]);

  const esp_err_t e1 = esp_cache_msync(fb, 800 * 800 * 2,
                                       ESP_CACHE_MSYNC_FLAG_DIR_C2M);
  Serial.printf("[cal] full msync: %s\n", esp_err_to_name(e1));
  delay(50);

  gfx->fillRect(500, 370, 60, 60, 0xF81F);  // magenta, library path
  Serial.printf("[cal] after magenta: fb[530,400]=0x%04X (expect F81F)\n",
                fb[400 * 800 + 530]);

  // Navy block via DIRECT framebuffer writes (no library involved) at
  // (240..300, 370..430), msync'd row-precisely.
  for (int row = 370; row < 430; ++row) {
    for (int col = 240; col < 300; ++col) {
      fb[row * 800 + col] = 0x000F;  // navy blue
    }
  }
  const esp_err_t e2 = esp_cache_msync(fb + 370 * 800, 60 * 800 * 2,
                                       ESP_CACHE_MSYNC_FLAG_DIR_C2M);
  Serial.printf("[cal] navy direct write msync: %s\n", esp_err_to_name(e2));
  Serial.printf("[cal] fb[270,400]=0x%04X (expect 000F)\n", fb[400 * 800 + 270]);

  Serial.println("[cal] pattern drawn");
}

void loop() {
  delay(1000);
}
