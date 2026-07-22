#include "hardware/plane_gfx.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <esp_cache.h>
#include <esp_heap_caps.h>

#include "hardware/gfx_log.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static SemaphoreHandle_t s_panel_mutex = nullptr;

namespace {

class GfxWriteLineAccess : public Arduino_GFX {
 public:
  static void writeLineOn(Arduino_GFX* gfx, int16_t x0, int16_t y0, int16_t x1,
                          int16_t y1, uint16_t color) {
    static_cast<GfxWriteLineAccess*>(gfx)->writeLine(x0, y0, x1, y1, color);
  }
};

class SpriteCanvas : public Arduino_GFX {
 public:
  SpriteCanvas(uint16_t* buffer, int16_t w, int16_t h)
      : Arduino_GFX(w, h), buffer_(buffer), width_(w), height_(h) {}

  bool begin(int32_t speed = GFX_NOT_DEFINED) override {
    (void)speed;
    return true;
  }

  void writePixelPreclipped(int16_t x, int16_t y, uint16_t color) override {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) {
      return;
    }
    buffer_[static_cast<size_t>(y) * static_cast<size_t>(width_) +
            static_cast<size_t>(x)] = color;
  }

  void writeFillRectPreclipped(int16_t x, int16_t y, int16_t w, int16_t h,
                               uint16_t color) override {
    // Clip here despite the contract — raw Bresenham output reaches this via
    // Arduino_GFX::writeLine, which never clips.
    if (x < 0) {
      w = static_cast<int16_t>(w + x);
      x = 0;
    }
    if (y < 0) {
      h = static_cast<int16_t>(h + y);
      y = 0;
    }
    if (x + w > width_) {
      w = static_cast<int16_t>(width_ - x);
    }
    if (y + h > height_) {
      h = static_cast<int16_t>(height_ - y);
    }
    if (w <= 0 || h <= 0) {
      return;
    }
    for (int16_t row = y; row < y + h; ++row) {
      uint16_t* d = buffer_ + static_cast<size_t>(row) * static_cast<size_t>(width_) +
                    static_cast<size_t>(x);
      std::fill(d, d + w, color);
    }
  }

 private:
  uint16_t* buffer_;
  int16_t width_;
  int16_t height_;
};

uint16_t* s_blit_scratch = nullptr;
size_t s_blit_scratch_pixels = 0;

bool ensureBlitScratch(size_t pixels) {
  if (pixels == 0) {
    return false;
  }
  if (s_blit_scratch != nullptr && s_blit_scratch_pixels >= pixels) {
    return true;
  }
  if (s_blit_scratch != nullptr) {
    heap_caps_free(s_blit_scratch);
    s_blit_scratch = nullptr;
    s_blit_scratch_pixels = 0;
  }
  const size_t bytes = pixels * sizeof(uint16_t);
  s_blit_scratch = static_cast<uint16_t*>(
      heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (s_blit_scratch == nullptr) {
    s_blit_scratch = static_cast<uint16_t*>(heap_caps_malloc(bytes, MALLOC_CAP_8BIT));
  }
  if (s_blit_scratch == nullptr) {
    return false;
  }
  s_blit_scratch_pixels = pixels;
  return true;
}

/** Blit non-transparent runs of each row (DSI framebuffer: one draw per run). */
void blitRegionWithTransparentKey(int16_t x, int16_t y, int16_t w, int16_t h,
                                  const uint16_t* src, int16_t src_stride,
                                  uint16_t transparent_key, PlaneGfx* gfx) {
  if (gfx == nullptr || gfx->raw() == nullptr || src == nullptr || w <= 0 || h <= 0 ||
      src_stride <= 0) {
    return;
  }

  Arduino_GFX* panel = gfx->raw();
  const int16_t screen_w = static_cast<int16_t>(panel->width());
  const int16_t screen_h = static_cast<int16_t>(panel->height());

  for (int16_t row = 0; row < h; ++row) {
    const int16_t dst_y = static_cast<int16_t>(y + row);
    if (dst_y < 0 || dst_y >= screen_h) {
      continue;
    }
    const uint16_t* row_src =
        src + static_cast<size_t>(row) * static_cast<size_t>(src_stride);
    int16_t col = 0;
    while (col < w) {
      while (col < w && row_src[col] == transparent_key) {
        ++col;
      }
      const int16_t run_start = col;
      while (col < w && row_src[col] != transparent_key) {
        ++col;
      }
      const int16_t run_len = static_cast<int16_t>(col - run_start);
      if (run_len <= 0) {
        continue;
      }
      int16_t dst_x = static_cast<int16_t>(x + run_start);
      int16_t src_skip = 0;
      int16_t len = run_len;
      if (dst_x < 0) {
        src_skip = static_cast<int16_t>(-dst_x);
        len = static_cast<int16_t>(len - src_skip);
        dst_x = 0;
      }
      if (dst_x >= screen_w || len <= 0) {
        continue;
      }
      if (dst_x + len > screen_w) {
        len = static_cast<int16_t>(screen_w - dst_x);
      }
      panel->draw16bitRGBBitmap(dst_x, dst_y,
                                const_cast<uint16_t*>(row_src + run_start + src_skip),
                                len, 1);
    }
  }
}

}  // namespace

void planeGfxPanelLockInit() {
  if (s_panel_mutex == nullptr) {
    s_panel_mutex = xSemaphoreCreateMutex();
  }
}

void PlaneGfx::fillScreen(uint16_t color) {
  if (gfx_ == nullptr) {
    return;
  }
  if (hardware_panel_) {
    gfx_->fillScreen(color);
    return;
  }
  const bool opened_here = write_depth_ == 0;
  if (opened_here) {
    startWrite();
  }
  gfx_->fillScreen(color);
  if (opened_here) {
    endWrite();
  }
}

void PlaneGfx::fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  if (gfx_ == nullptr) {
    return;
  }
  if (hardware_panel_) {
    gfx_->fillRect(x, y, w, h, color);
    return;
  }
  const bool opened_here = write_depth_ == 0;
  if (opened_here) {
    startWrite();
  }
  gfx_->fillRect(x, y, w, h, color);
  if (opened_here) {
    endWrite();
  }
}

void PlaneGfx::fillCircle(int16_t x, int16_t y, int16_t r, uint16_t color) {
  if (gfx_ != nullptr) {
    gfx_->fillCircle(x, y, r, color);
  }
}

void PlaneGfx::drawCircle(int16_t x, int16_t y, int16_t r, uint16_t color) {
  if (gfx_ != nullptr) {
    gfx_->drawCircle(x, y, r, color);
  }
}

void PlaneGfx::fillTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                            int16_t x2, int16_t y2, uint16_t color) {
  if (gfx_ != nullptr) {
    gfx_->fillTriangle(x0, y0, x1, y1, x2, y2, color);
  }
}

void PlaneGfx::fbSyncRows(int32_t y0, int32_t h) {
  if (fb_ == nullptr || h <= 0) {
    return;
  }
  const int32_t a = std::max<int32_t>(0, y0);
  const int32_t b = std::min<int32_t>(fb_h_, y0 + h);
  if (b <= a) {
    return;
  }
  uint16_t* start = fb_ + static_cast<size_t>(a) * static_cast<size_t>(fb_w_);
  const size_t bytes =
      static_cast<size_t>(b - a) * static_cast<size_t>(fb_w_) * sizeof(uint16_t);
  esp_cache_msync(start, bytes,
                  ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

void PlaneGfx::fbLineNoSync(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                            uint16_t color, int32_t* ymin, int32_t* ymax) {
  const bool steep = std::abs(y1 - y0) > std::abs(x1 - x0);
  if (steep) {
    std::swap(x0, y0);
    std::swap(x1, y1);
  }
  if (x0 > x1) {
    std::swap(x0, x1);
    std::swap(y0, y1);
  }
  const int32_t dx = x1 - x0;
  const int32_t dy = std::abs(y1 - y0);
  const int32_t ystep = (y0 < y1) ? 1 : -1;
  int32_t err = dx / 2;
  int32_t y = y0;
  for (int32_t x = x0; x <= x1; ++x) {
    const int32_t px = steep ? y : x;
    const int32_t py = steep ? x : y;
    if (px >= 0 && py >= 0 && px < fb_w_ && py < fb_h_) {
      fb_[static_cast<size_t>(py) * static_cast<size_t>(fb_w_) +
          static_cast<size_t>(px)] = color;
      if (py < *ymin) {
        *ymin = py;
      }
      if (py > *ymax) {
        *ymax = py;
      }
    }
    err -= dy;
    if (err < 0) {
      y += ystep;
      err += dx;
    }
  }
}

void PlaneGfx::drawLineInternal(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                                uint16_t color) {
  if (hardware_panel_ && fb_ != nullptr) {
    int32_t ymin = fb_h_;
    int32_t ymax = -1;
    fbLineNoSync(x0, y0, x1, y1, color, &ymin, &ymax);
    if (ymax >= ymin) {
      fbSyncRows(ymin, ymax - ymin + 1);
    }
    return;
  }
  GfxWriteLineAccess::writeLineOn(gfx_, x0, y0, x1, y1, color);
}

void PlaneGfx::drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                        uint16_t color) {
  if (gfx_ == nullptr) {
    return;
  }
  const bool opened_here = write_depth_ == 0;
  if (opened_here) {
    startWrite();
  }
  drawLineInternal(x0, y0, x1, y1, color);
  if (opened_here) {
    endWrite();
  }
}

void PlaneGfx::drawWideLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                            float half_width, uint16_t color) {
  if (gfx_ == nullptr) {
    return;
  }
  const bool opened_here = write_depth_ == 0;
  if (opened_here) {
    startWrite();
  }
  const int steps = std::max(1, static_cast<int>(half_width * 2.0f + 0.5f));
  const float offset = -half_width;
  if (hardware_panel_ && fb_ != nullptr) {
    // All offset strokes into the framebuffer, then one cache sync.
    int32_t ymin = fb_h_;
    int32_t ymax = -1;
    for (int i = 0; i < steps; ++i) {
      const float t = offset + static_cast<float>(i);
      const int ox = static_cast<int>(std::lround(t));
      const int oy = static_cast<int>(std::lround(-t));
      fbLineNoSync(x0 + ox, y0 + oy, x1 + ox, y1 + oy, color, &ymin, &ymax);
    }
    if (ymax >= ymin) {
      fbSyncRows(ymin, ymax - ymin + 1);
    }
  } else {
    for (int i = 0; i < steps; ++i) {
      const float t = offset + static_cast<float>(i);
      const int ox = static_cast<int>(std::lround(t));
      const int oy = static_cast<int>(std::lround(-t));
      GfxWriteLineAccess::writeLineOn(gfx_, x0 + ox, y0 + oy, x1 + ox, y1 + oy,
                                      color);
    }
  }
  if (opened_here) {
    endWrite();
  }
}

uint16_t PlaneGfx::color565(uint8_t r, uint8_t g, uint8_t b) const {
  if (gfx_ != nullptr) {
    return gfx_->color565(r, g, b);
  }
  return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

void PlaneGfx::setTextColor(uint16_t fg) {
  if (gfx_ != nullptr) {
    gfx_->setTextColor(fg);
  }
}

void PlaneGfx::setTextColor(uint16_t fg, uint16_t bg) {
  if (gfx_ != nullptr) {
    gfx_->setTextColor(fg, bg);
  }
}

void PlaneGfx::setTextSize(uint8_t size) {
  if (gfx_ != nullptr) {
    gfx_->setTextSize(size);
  }
}

void PlaneGfx::setFont(const GFXfont* font) {
  if (gfx_ != nullptr) {
    gfx_->setFont(font);
  }
}

void PlaneGfx::setTextDatum(TextDatum datum) { datum_ = datum; }

void PlaneGfx::setTextWrap(bool wrap) {
  if (gfx_ != nullptr) {
    gfx_->setTextWrap(wrap);
  }
}

int PlaneGfx::textWidth(const char* text) const {
  if (gfx_ == nullptr || text == nullptr) {
    return 0;
  }
  int16_t x1 = 0;
  int16_t y1 = 0;
  uint16_t w = 0;
  uint16_t h = 0;
  gfx_->getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  return static_cast<int>(w);
}

int PlaneGfx::fontHeight() const {
  if (gfx_ == nullptr) {
    return 8;
  }
  int16_t x1 = 0;
  int16_t y1 = 0;
  uint16_t w = 0;
  uint16_t h = 0;
  gfx_->getTextBounds("Ag", 0, 0, &x1, &y1, &w, &h);
  return static_cast<int>(h);
}

void PlaneGfx::mapDatum(const char* text, int16_t x, int16_t y, int16_t* out_x,
                        int16_t* out_y) const {
  if (gfx_ == nullptr || text == nullptr) {
    *out_x = x;
    *out_y = y;
    return;
  }

  int16_t x1 = 0;
  int16_t y1 = 0;
  uint16_t w = 0;
  uint16_t h = 0;
  gfx_->getTextBounds(text, 0, 0, &x1, &y1, &w, &h);

  switch (datum_) {
    case TextDatum::TopLeft:
      *out_x = x - x1;
      *out_y = y - y1;
      break;
    case TextDatum::TopCenter:
      *out_x = x - (x1 + static_cast<int16_t>(w) / 2);
      *out_y = y - y1;
      break;
    case TextDatum::TopRight:
      *out_x = x - (x1 + static_cast<int16_t>(w));
      *out_y = y - y1;
      break;
    case TextDatum::MiddleLeft:
      *out_x = x - x1;
      *out_y = y - (y1 + static_cast<int16_t>(h) / 2);
      break;
    case TextDatum::MiddleCenter:
      *out_x = x - (x1 + static_cast<int16_t>(w) / 2);
      *out_y = y - (y1 + static_cast<int16_t>(h) / 2);
      break;
    case TextDatum::MiddleRight:
      *out_x = x - (x1 + static_cast<int16_t>(w));
      *out_y = y - (y1 + static_cast<int16_t>(h) / 2);
      break;
    case TextDatum::BottomLeft:
      *out_x = x - x1;
      *out_y = y - (y1 + static_cast<int16_t>(h));
      break;
    case TextDatum::BottomCenter:
      *out_x = x - (x1 + static_cast<int16_t>(w) / 2);
      *out_y = y - (y1 + static_cast<int16_t>(h));
      break;
    case TextDatum::BottomRight:
      *out_x = x - (x1 + static_cast<int16_t>(w));
      *out_y = y - (y1 + static_cast<int16_t>(h));
      break;
  }
}

void PlaneGfx::drawString(const char* text, int16_t x, int16_t y) {
  if (gfx_ == nullptr || text == nullptr) {
    return;
  }
  int16_t draw_x = x;
  int16_t draw_y = y;
  mapDatum(text, x, y, &draw_x, &draw_y);
  gfx_->setCursor(draw_x, draw_y);
  if (hardware_panel_) {
    gfx_->print(text);
    return;
  }
  const bool opened_here = write_depth_ == 0;
  if (opened_here) {
    startWrite();
  }
  gfx_->print(text);
  if (opened_here) {
    endWrite();
  }
}

void PlaneGfx::startWrite() {
  if (gfx_ == nullptr) {
    return;
  }
  if (write_depth_++ == 0) {
    if (hardware_panel_ && s_panel_mutex != nullptr) {
      xSemaphoreTake(s_panel_mutex, portMAX_DELAY);
    }
    gfx_->startWrite();
  }
}

void PlaneGfx::endWrite() {
  if (gfx_ == nullptr || write_depth_ == 0) {
    return;
  }
  if (--write_depth_ == 0) {
    gfx_->endWrite();
    if (hardware_panel_ && s_panel_mutex != nullptr) {
      xSemaphoreGive(s_panel_mutex);
    }
  }
}

bool PlaneGfx::beginOffscreen() {
  // DSI framebuffer writes are never 2x2-quantized — direct drawing is crisp.
  return false;
}

void PlaneGfx::endOffscreen() {}

void PlaneGfx::panelFlushBitmap(int16_t x, int16_t y, int16_t w, int16_t h,
                                const uint16_t* src) {
  if (gfx_ == nullptr || src == nullptr || w <= 0 || h <= 0) {
    return;
  }

  hardware::gfxLogf("[gfx] flush %dx%d @ (%d,%d) begin", w, h, x, y);
  // DSI display and sprite canvas both take the full bitmap in one call.
  gfx_->draw16bitRGBBitmap(x, y, const_cast<uint16_t*>(src), w, h);
  hardware::gfxLog("[gfx] flush ok");
}

void PlaneGfx::draw16bitRGBBitmap(int16_t x, int16_t y, const uint16_t* bitmap,
                                  int16_t w, int16_t h) {
  if (gfx_ == nullptr || bitmap == nullptr) {
    return;
  }
  if (hardware_panel_) {
    blitRegionFromBuffer(x, y, w, h, bitmap, w);
    return;
  }
  gfx_->draw16bitRGBBitmap(x, y, const_cast<uint16_t*>(bitmap), w, h);
}

void PlaneGfx::draw16bitRGBBitmap(int16_t x, int16_t y, const uint16_t* bitmap,
                                  uint16_t transparent_color, int16_t w,
                                  int16_t h) {
  if (gfx_ == nullptr || bitmap == nullptr) {
    return;
  }
  if (!hardware_panel_) {
    gfx_->draw16bitRGBBitmapWithTranColor(x, y, const_cast<uint16_t*>(bitmap),
                                          transparent_color, w, h);
    return;
  }
  const bool opened_here = write_depth_ == 0;
  if (opened_here) {
    startWrite();
  }
  blitRegionWithTransparentKey(x, y, w, h, bitmap, w, transparent_color, this);
  if (opened_here) {
    endWrite();
  }
}

void PlaneGfx::blitRegionFromBuffer(int16_t x, int16_t y, int16_t w, int16_t h,
                                    const uint16_t* src, int16_t src_stride) {
  if (gfx_ == nullptr || src == nullptr || w <= 0 || h <= 0 || src_stride <= 0) {
    return;
  }

  const int16_t screen_w = static_cast<int16_t>(gfx_->width());
  const int16_t screen_h = static_cast<int16_t>(gfx_->height());
  if (x >= screen_w || y >= screen_h) {
    return;
  }

  if (x < 0) {
    src += static_cast<size_t>(-x);
    w += x;
    x = 0;
  }
  if (y < 0) {
    src += static_cast<size_t>(-y) * static_cast<size_t>(src_stride);
    h += y;
    y = 0;
  }
  if (x + w > screen_w) {
    w = screen_w - x;
  }
  if (y + h > screen_h) {
    h = screen_h - y;
  }
  if (w <= 0 || h <= 0) {
    return;
  }

  const bool opened_here = write_depth_ == 0;
  if (opened_here) {
    startWrite();
  }
  if (src_stride == w) {
    panelFlushBitmap(x, y, w, h, src);
  } else {
    // Pack the strided region into contiguous scratch: one draw call, one
    // cache writeback, instead of h separate row blits.
    const size_t pixels = static_cast<size_t>(w) * static_cast<size_t>(h);
    if (ensureBlitScratch(pixels)) {
      for (int16_t row = 0; row < h; ++row) {
        memcpy(s_blit_scratch + static_cast<size_t>(row) * static_cast<size_t>(w),
               src + static_cast<size_t>(row) * static_cast<size_t>(src_stride),
               static_cast<size_t>(w) * sizeof(uint16_t));
      }
      panelFlushBitmap(x, y, w, h, s_blit_scratch);
    } else {
      for (int16_t row = 0; row < h; ++row) {
        const uint16_t* row_ptr =
            src + static_cast<size_t>(row) * static_cast<size_t>(src_stride);
        gfx_->draw16bitRGBBitmap(x, static_cast<int16_t>(y + row),
                                 const_cast<uint16_t*>(row_ptr), w, 1);
      }
    }
  }
  if (opened_here) {
    endWrite();
  }
}

PlaneGfxSprite::PlaneGfxSprite(PlaneGfx* parent) : parent_(parent) {}

PlaneGfxSprite::~PlaneGfxSprite() { deleteSprite(); }

bool PlaneGfxSprite::createSprite(int16_t w, int16_t h) {
  deleteSprite();
  const size_t bytes = static_cast<size_t>(w) * static_cast<size_t>(h) * sizeof(uint16_t);
  buffer_ = static_cast<uint16_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (buffer_ == nullptr) {
    buffer_ = static_cast<uint16_t*>(heap_caps_malloc(bytes, MALLOC_CAP_8BIT));
  }
  if (buffer_ == nullptr) {
    return false;
  }
  width_ = w;
  height_ = h;
  canvas_.attach(new SpriteCanvas(buffer_, w, h));
  canvas_.setTextWrap(false);
  return true;
}

void PlaneGfxSprite::deleteSprite() {
  if (canvas_.raw() != nullptr) {
    delete canvas_.raw();
    canvas_.attach(nullptr);
  }
  if (buffer_ != nullptr) {
    heap_caps_free(buffer_);
    buffer_ = nullptr;
  }
  width_ = 0;
  height_ = 0;
}

void PlaneGfxSprite::pushSprite(int16_t x, int16_t y) {
  if (parent_ == nullptr || buffer_ == nullptr || parent_->raw() == nullptr) {
    return;
  }
  parent_->draw16bitRGBBitmap(x, y, buffer_, width_, height_);
}

void PlaneGfxSprite::pushSprite(int16_t x, int16_t y, uint16_t transparent_color) {
  if (parent_ == nullptr || buffer_ == nullptr || parent_->raw() == nullptr) {
    return;
  }
  parent_->draw16bitRGBBitmap(x, y, buffer_, transparent_color, width_, height_);
}

void PlaneGfxSprite::pushRegion(int16_t x, int16_t y, int16_t w, int16_t h) {
  if (parent_ == nullptr || buffer_ == nullptr || w <= 0 || h <= 0) {
    return;
  }
  if (x < 0) {
    w = static_cast<int16_t>(w + x);
    x = 0;
  }
  if (y < 0) {
    h = static_cast<int16_t>(h + y);
    y = 0;
  }
  if (x + w > width_) {
    w = static_cast<int16_t>(width_ - x);
  }
  if (y + h > height_) {
    h = static_cast<int16_t>(height_ - y);
  }
  if (w <= 0 || h <= 0) {
    return;
  }
  parent_->blitRegionFromBuffer(
      x, y, w, h,
      buffer_ + static_cast<size_t>(y) * static_cast<size_t>(width_) +
          static_cast<size_t>(x),
      width_);
}

void PlaneGfxSprite::copyRegionFrom(const PlaneGfxSprite& src, int16_t x,
                                    int16_t y, int16_t w, int16_t h) {
  if (buffer_ == nullptr || src.buffer_ == nullptr || src.width_ != width_ ||
      src.height_ != height_ || w <= 0 || h <= 0) {
    return;
  }
  if (x < 0) {
    w = static_cast<int16_t>(w + x);
    x = 0;
  }
  if (y < 0) {
    h = static_cast<int16_t>(h + y);
    y = 0;
  }
  if (x + w > width_) {
    w = static_cast<int16_t>(width_ - x);
  }
  if (y + h > height_) {
    h = static_cast<int16_t>(height_ - y);
  }
  if (w <= 0 || h <= 0) {
    return;
  }
  const size_t row_bytes = static_cast<size_t>(w) * sizeof(uint16_t);
  for (int16_t row = y; row < y + h; ++row) {
    memcpy(buffer_ + static_cast<size_t>(row) * static_cast<size_t>(width_) +
               static_cast<size_t>(x),
           src.buffer_ + static_cast<size_t>(row) * static_cast<size_t>(width_) +
               static_cast<size_t>(x),
           row_bytes);
  }
}
