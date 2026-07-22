#pragma once

/** Waveshare ESP32-P4-WIFI6-Touch-LCD-3.4C — 800×800 round IPS, MIPI-DSI 2-lane.
 *
 *  Display init lives in lib/waveshare_displays/displays_config.h (JD9365 table,
 *  DSI timings). Touch is a GT911 (I2C 0x5D) handled by lib/waveshare_displays.
 *  WiFi runs on the onboard ESP32-C6 over SDIO (CMD=19 CLK=18 D0-D3=14-17 RST=54)
 *  — those pins are owned by the ESP-Hosted driver, never touch them here.
 */

#define IIC_SDA 7
#define IIC_SCL 8

#define TOUCH_RST 23
#define TOUCH_INT -1  // not wired on the 3.4C; GT911 driver polls over I2C

#define LCD_RST 27
/** Backlight PWM is ACTIVE-LOW (Waveshare BSP uses output_invert=1):
 *  LEDC duty 0 = full brightness, 255 = off. */
#define LCD_BACKLIGHT 26

// Sold as 3.4C (800×800) but the fitted panel is 720×720 (4C-class): feeding
// it 800×800 video drops an ~80px band through the center of both axes.
// Board runs the SCREEN_4INCH_DSI config (see platformio.ini CURRENT_SCREEN).
#define LCD_WIDTH 720
#define LCD_HEIGHT 720
