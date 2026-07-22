#pragma once

#include <Arduino_GFX_Library.h>
#include <cstdint>

enum class TextDatum : uint8_t {
  TopLeft,
  TopCenter,
  TopRight,
  MiddleLeft,
  MiddleCenter,
  MiddleRight,
  BottomLeft,
  BottomCenter,
  BottomRight,
};

/** Drawing helpers and text layout on top of Arduino_GFX.
 *
 *  All coordinates are native panel pixels (720×720 DSI). For the hardware
 *  panel, line strokes are written straight into the DSI framebuffer with a
 *  single cache sync per stroke (fast enough for the per-frame radar sweep);
 *  everything else delegates to the display's self-syncing draw methods. */
class PlaneGfx {
 public:
  PlaneGfx() = default;

  /** fb/fb_w/fb_h: the DSI framebuffer for the direct line path (hardware
   *  panel only — sprites pass nullptr and use the canvas rasterizer). */
  void attach(Arduino_GFX* gfx, bool hardware_panel = false,
              uint16_t* fb = nullptr, int16_t fb_w = 0, int16_t fb_h = 0) {
    gfx_ = gfx;
    hardware_panel_ = hardware_panel;
    fb_ = fb;
    fb_w_ = fb_w;
    fb_h_ = fb_h;
  }
  Arduino_GFX* raw() const { return gfx_; }

  void fillScreen(uint16_t color);
  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
  void fillCircle(int16_t x, int16_t y, int16_t r, uint16_t color);
  void drawCircle(int16_t x, int16_t y, int16_t r, uint16_t color);
  void fillTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t x2,
                    int16_t y2, uint16_t color);
  void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color);
  void drawWideLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, float half_width,
                    uint16_t color);

  uint16_t color565(uint8_t r, uint8_t g, uint8_t b) const;

  void setTextColor(uint16_t fg);
  void setTextColor(uint16_t fg, uint16_t bg);
  void setTextSize(uint8_t size);
  void setFont(const GFXfont* font);
  void setTextDatum(TextDatum datum);
  void setTextWrap(bool wrap);

  int textWidth(const char* text) const;
  int fontHeight() const;
  void drawString(const char* text, int16_t x, int16_t y);

  void startWrite();
  void endWrite();

  /** Legacy CO5300 (pixelAlign2) offscreen compose path. The DSI panel has no
   *  2x2 write quantization, so this is a permanent no-op returning false —
   *  callers fall back to direct drawing, which is already crisp. */
  bool beginOffscreen();
  void endOffscreen();

  void draw16bitRGBBitmap(int16_t x, int16_t y, const uint16_t* bitmap, int16_t w,
                          int16_t h);
  void draw16bitRGBBitmap(int16_t x, int16_t y, const uint16_t* bitmap,
                          uint16_t transparent_color, int16_t w, int16_t h);
  /** Copies a screen region to the hardware panel. Handles src_stride != w. */
  void blitRegionFromBuffer(int16_t x, int16_t y, int16_t w, int16_t h,
                            const uint16_t* src, int16_t src_stride);

 private:
  Arduino_GFX* gfx_ = nullptr;
  bool hardware_panel_ = false;
  uint16_t* fb_ = nullptr;
  int16_t fb_w_ = 0;
  int16_t fb_h_ = 0;
  TextDatum datum_ = TextDatum::TopLeft;
  uint8_t write_depth_ = 0;

  /** Single flush to the hardware panel (contiguous src, stride == w). */
  void panelFlushBitmap(int16_t x, int16_t y, int16_t w, int16_t h,
                        const uint16_t* src);
  void drawLineInternal(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                        uint16_t color);
  /** Bresenham straight into fb_, no cache sync; tracks touched row range. */
  void fbLineNoSync(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                    uint16_t color, int32_t* ymin, int32_t* ymax);
  void fbSyncRows(int32_t y0, int32_t h);
  void mapDatum(const char* text, int16_t x, int16_t y, int16_t* out_x,
                int16_t* out_y) const;
};

/** RAII panel session for one composited frame (serializes panel access). */
class PanelSession {
 public:
  explicit PanelSession(PlaneGfx& gfx) : gfx_(&gfx) { gfx_->startWrite(); }
  ~PanelSession() { gfx_->endWrite(); }

  PanelSession(const PanelSession&) = delete;
  PanelSession& operator=(const PanelSession&) = delete;

 private:
  PlaneGfx* gfx_;
};

/** Off-screen compose buffer at native resolution (PSRAM when available). */
class PlaneGfxSprite {
 public:
  explicit PlaneGfxSprite(PlaneGfx* parent);
  ~PlaneGfxSprite();

  bool createSprite(int16_t w, int16_t h);
  void deleteSprite();
  bool ready() const { return buffer_ != nullptr; }
  int16_t width() const { return width_; }
  int16_t height() const { return height_; }
  const uint16_t* buffer() const { return buffer_; }
  uint16_t* bufferMut() { return buffer_; }

  PlaneGfx& gfx() { return canvas_; }
  void pushSprite(int16_t x, int16_t y);
  /** Blit sprite skipping pixels equal to transparent_color (non-blocking overlay). */
  void pushSprite(int16_t x, int16_t y, uint16_t transparent_color);
  /** Blit a region of this sprite to the panel at its own position. */
  void pushRegion(int16_t x, int16_t y, int16_t w, int16_t h);
  /** Copy a region from another same-size sprite into this one. */
  void copyRegionFrom(const PlaneGfxSprite& src, int16_t x, int16_t y, int16_t w,
                      int16_t h);

 private:
  PlaneGfx* parent_ = nullptr;
  PlaneGfx canvas_;
  uint16_t* buffer_ = nullptr;
  int16_t width_ = 0;
  int16_t height_ = 0;
};
