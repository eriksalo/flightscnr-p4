#pragma once

#include <Arduino_GFX_Library.h>

/** Presents a logical design space (the app's original 390×390 T-Encoder Pro
 *  layout) over a physical-resolution RGB565 framebuffer, scaled by exactly
 *  24/13 (390 × 24/13 = 720, edge to edge on the DSI panel).
 *
 *  Two instances exist per surface kind:
 *   - the panel canvas: physical target = Arduino_DSI_Display, fb = the DSI
 *     scanout framebuffer (PSRAM, needs esp_cache_msync after direct writes);
 *   - sprite canvases: physical target = an in-memory SpriteCanvas over a
 *     PSRAM buffer (no cache sync — nothing scans it out).
 *
 *  Full-resolution rendering: shapes, lines, and text are rasterized at
 *  PHYSICAL resolution. PlaneGfx maps logical coordinates and draws on
 *  physicalTarget() (or via the phys* helpers below, which write straight into
 *  the framebuffer and cache-sync once per call). The logical write* overrides
 *  remain for base-class fallbacks; axis-aligned rect fills scale losslessly
 *  through them, and writeLine is intercepted so even fallback rasterization
 *  of diagonals lands at physical resolution.
 *
 *  NOTE: despite the "Preclipped" contract, write ops clip against the logical
 *  bounds. PlaneGfx::drawLineInternal feeds raw Bresenham output straight to
 *  writePixelPreclipped (via Arduino_GFX::writeLine, which never clips); the
 *  old QSPI panel dropped out-of-range writes in hardware, but here they would
 *  land at wild framebuffer offsets and paint garbage.
 */
class ScaledCanvas : public Arduino_GFX {
 public:
  static constexpr int16_t kLogicalSize = 390;
  static constexpr int32_t kScaleNum = 24;
  static constexpr int32_t kScaleDen = 13;  // 390 * 24 / 13 = 720 exactly

  ScaledCanvas(Arduino_GFX* target, uint16_t* fb, int16_t fb_w, int16_t fb_h,
               bool sync_cache, int16_t logical_w = kLogicalSize,
               int16_t logical_h = kLogicalSize)
      : Arduino_GFX(logical_w, logical_h),
        target_(target),
        fb_(fb),
        fb_w_(fb_w),
        fb_h_(fb_h),
        sync_(sync_cache),
        logical_w_(logical_w),
        logical_h_(logical_h) {}

  bool begin(int32_t speed = GFX_NOT_DEFINED) override {
    (void)speed;
    return target_ != nullptr && fb_ != nullptr;
  }

  void startWrite() override { target_->startWrite(); }
  void endWrite() override { target_->endWrite(); }

  void writePixelPreclipped(int16_t x, int16_t y, uint16_t color) override {
    if (x < 0 || y < 0 || x >= logical_w_ || y >= logical_h_) {
      return;
    }
    const int16_t px = map(x);
    const int16_t py = map(y);
    target_->writeFillRectPreclipped(px, py, map(x + 1) - px, map(y + 1) - py,
                                     color);
  }

  void writeFillRectPreclipped(int16_t x, int16_t y, int16_t w, int16_t h,
                               uint16_t color) override {
    if (!clipRect(&x, &y, &w, &h)) {
      return;
    }
    const int16_t px = map(x);
    const int16_t py = map(y);
    target_->writeFillRectPreclipped(px, py, map(x + w) - px, map(y + h) - py,
                                     color);
  }

  void writeFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) override;
  void writeFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) override;

  /** Diagonals from base-class rasterizers land at physical resolution too. */
  void writeLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                 uint16_t color) override;

  void draw16bitRGBBitmap(int16_t x, int16_t y, uint16_t* bitmap, int16_t w,
                          int16_t h) override;
  void draw16bitRGBBitmapWithTranColor(int16_t x, int16_t y, uint16_t* bitmap,
                                       uint16_t transparent_color, int16_t w,
                                       int16_t h) override;

  // --- Physical-resolution access (coordinates in framebuffer pixels) ---

  Arduino_GFX* physicalTarget() const { return target_; }
  uint16_t* physFb() const { return fb_; }
  int16_t physWidth() const { return fb_w_; }
  int16_t physHeight() const { return fb_h_; }
  bool syncsCache() const { return sync_; }

  /** 1:1 copy of a physical-resolution bitmap region into the framebuffer.
   *  Clips against fb bounds; one cache sync over the touched rows. */
  void blitPhysical(int16_t px, int16_t py, const uint16_t* src, int16_t w,
                    int16_t h, int16_t src_stride);
  /** Same, skipping pixels equal to transparent_color. */
  void blitPhysicalTransparent(int16_t px, int16_t py, const uint16_t* src,
                               int16_t w, int16_t h, int16_t src_stride,
                               uint16_t transparent_color);

  /** Wide line at physical resolution, written straight into the framebuffer
   *  (one cache sync per call — cheap enough for the per-frame radar sweep). */
  void physWideLine(int32_t px0, int32_t py0, int32_t px1, int32_t py1,
                    float half_width, uint16_t color);

  /** Single physical pixel, no cache sync — pair with physSyncRect. */
  void physFillPixel(int32_t px, int32_t py, uint16_t color) {
    if (fb_ == nullptr || px < 0 || py < 0 || px >= fb_w_ || py >= fb_h_) {
      return;
    }
    fb_[static_cast<size_t>(py) * static_cast<size_t>(fb_w_) +
        static_cast<size_t>(px)] = color;
  }

  /** Cache-sync the framebuffer rows covering [py, py+h) (no-op for sprites). */
  void physSyncRect(int32_t py, int32_t h);

  /** Logical → physical (exact; map(390) == 720). */
  static int16_t map(int32_t v) {
    return static_cast<int16_t>(v * kScaleNum / kScaleDen);
  }
  /** Logical length → physical length, round-to-nearest (radii, stroke widths). */
  static int16_t mapLen(int32_t v) {
    return static_cast<int16_t>((v * kScaleNum + kScaleDen / 2) / kScaleDen);
  }
  static float mapF(float v) {
    return v * static_cast<float>(kScaleNum) / static_cast<float>(kScaleDen);
  }
  /** Physical → logical (touch input). */
  static int16_t unmap(int32_t v) {
    int32_t mapped = v * kScaleDen / kScaleNum;
    if (mapped < 0) {
      mapped = 0;
    }
    if (mapped >= kLogicalSize) {
      mapped = kLogicalSize - 1;
    }
    return static_cast<int16_t>(mapped);
  }
  /** Physical length → logical length, rounded up (text metrics: layout must
   *  reserve at least the true physical footprint). */
  static int16_t unmapLenCeil(int32_t v) {
    return static_cast<int16_t>((v * kScaleDen + kScaleNum - 1) / kScaleNum);
  }

 private:
  /** Clamp a logical-space rect to bounds; false when nothing remains. */
  bool clipRect(int16_t* x, int16_t* y, int16_t* w, int16_t* h) const {
    if (*x < 0) {
      *w = static_cast<int16_t>(*w + *x);
      *x = 0;
    }
    if (*y < 0) {
      *h = static_cast<int16_t>(*h + *y);
      *y = 0;
    }
    if (*x + *w > logical_w_) {
      *w = static_cast<int16_t>(logical_w_ - *x);
    }
    if (*y + *h > logical_h_) {
      *h = static_cast<int16_t>(logical_h_ - *y);
    }
    return *w > 0 && *h > 0;
  }

  /** Scaled blit into the framebuffer; pass kNoTransparent to copy all. */
  void blitScaled(int16_t x, int16_t y, const uint16_t* bitmap, int16_t w,
                  int16_t h, uint32_t transparent_color);

  /** Bresenham into the framebuffer, no cache sync; tracks touched row range. */
  void physLineNoSync(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                      uint16_t color, int32_t* ymin, int32_t* ymax);

  static constexpr uint32_t kNoTransparent = 0x10000;

  Arduino_GFX* target_;
  uint16_t* fb_;
  int16_t fb_w_;
  int16_t fb_h_;
  bool sync_;
  int16_t logical_w_;
  int16_t logical_h_;
};
