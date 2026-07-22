#pragma once

#include <Arduino_GFX_Library.h>
#include <cstdint>

class ScaledCanvas;

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

/** Drawing helpers and text layout on top of Arduino_GFX (DSI framebuffer).
 *
 *  All coordinates are logical (390×390 design space). When attached to a
 *  ScaledCanvas, shapes/lines/text are rasterized at the physical resolution
 *  behind it (720×720 panel or physical-res sprite) for full-detail output;
 *  metrics keep returning logical units so layout code is unaffected.
 */
class PlaneGfx {
 public:
  PlaneGfx() = default;

  void attach(Arduino_GFX* gfx, bool hardware_panel = false,
              ScaledCanvas* scaled = nullptr) {
    gfx_ = gfx;
    hardware_panel_ = hardware_panel;
    scaled_ = scaled;
    font_ = nullptr;
    phys_font_ = nullptr;
  }
  Arduino_GFX* raw() const { return gfx_; }
  ScaledCanvas* scaledCanvas() const { return scaled_; }

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
  /** True when text can render at physical resolution (scaled canvas attached,
   *  a physical variant exists for the current font, textSize is 1). */
  bool physTextActive() const;
  /** Text bounds at physical resolution (physical font on the physical target). */
  void physTextBounds(const char* text, int16_t* x1, int16_t* y1, uint16_t* w,
                      uint16_t* h) const;

  Arduino_GFX* gfx_ = nullptr;
  ScaledCanvas* scaled_ = nullptr;
  bool hardware_panel_ = false;
  TextDatum datum_ = TextDatum::TopLeft;
  uint8_t write_depth_ = 0;
  const GFXfont* font_ = nullptr;
  const GFXfont* phys_font_ = nullptr;
  uint8_t text_size_ = 1;
  uint16_t text_fg_ = 0xFFFF;
  uint16_t text_bg_ = 0x0000;
  bool text_bg_set_ = false;

  /** Single flush to the hardware panel (contiguous src, stride == w). */
  void panelFlushBitmap(int16_t x, int16_t y, int16_t w, int16_t h,
                        const uint16_t* src);
  void drawLineInternal(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                        uint16_t color);
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

/** Off-screen buffer for radar / detail composition (PSRAM when available).
 *
 *  createSprite takes LOGICAL dimensions; the backing buffer is allocated at
 *  physical resolution (× 24/13) and all drawing through gfx() rasterizes at
 *  that resolution. push/copy helpers also take logical coordinates. */
class PlaneGfxSprite {
 public:
  explicit PlaneGfxSprite(PlaneGfx* parent);
  ~PlaneGfxSprite();

  bool createSprite(int16_t w, int16_t h);
  void deleteSprite();
  bool ready() const { return buffer_ != nullptr; }
  int16_t width() const { return width_; }
  int16_t height() const { return height_; }
  int16_t physWidth() const { return phys_width_; }
  int16_t physHeight() const { return phys_height_; }
  const uint16_t* buffer() const { return buffer_; }
  uint16_t* bufferMut() { return buffer_; }

  PlaneGfx& gfx() { return canvas_; }
  void pushSprite(int16_t x, int16_t y);
  /** Blit sprite skipping pixels equal to transparent_color (non-blocking overlay). */
  void pushSprite(int16_t x, int16_t y, uint16_t transparent_color);
  /** Blit a logical-space region of this sprite to the panel at its own position. */
  void pushRegion(int16_t x, int16_t y, int16_t w, int16_t h);
  /** Copy a logical-space region from another same-size sprite into this one. */
  void copyRegionFrom(const PlaneGfxSprite& src, int16_t x, int16_t y, int16_t w,
                      int16_t h);

 private:
  PlaneGfx* parent_ = nullptr;
  PlaneGfx canvas_;
  uint16_t* buffer_ = nullptr;
  Arduino_GFX* phys_canvas_ = nullptr;
  ScaledCanvas* scaled_ = nullptr;
  int16_t width_ = 0;
  int16_t height_ = 0;
  int16_t phys_width_ = 0;
  int16_t phys_height_ = 0;
};
