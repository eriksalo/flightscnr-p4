#include "hardware/scaled_canvas.h"

#include <esp_cache.h>

#include <algorithm>
#include <cmath>
#include <cstring>

void ScaledCanvas::writeFastVLine(int16_t x, int16_t y, int16_t h,
                                  uint16_t color) {
  writeFillRectPreclipped(x, y, 1, h, color);
}

void ScaledCanvas::writeFastHLine(int16_t x, int16_t y, int16_t w,
                                  uint16_t color) {
  writeFillRectPreclipped(x, y, w, 1, color);
}

void ScaledCanvas::writeLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                             uint16_t color) {
  // Logical 1px stroke ≈ 24/13 physical px: rasterize at physical resolution
  // with a matching width so lines keep their visual weight but gain detail.
  physWideLine(map(x0), map(y0), map(x1), map(y1),
               0.5f * static_cast<float>(kScaleNum) / kScaleDen, color);
}

void ScaledCanvas::draw16bitRGBBitmap(int16_t x, int16_t y, uint16_t* bitmap,
                                      int16_t w, int16_t h) {
  blitScaled(x, y, bitmap, w, h, kNoTransparent);
}

void ScaledCanvas::draw16bitRGBBitmapWithTranColor(int16_t x, int16_t y,
                                                   uint16_t* bitmap,
                                                   uint16_t transparent_color,
                                                   int16_t w, int16_t h) {
  blitScaled(x, y, bitmap, w, h, transparent_color);
}

void ScaledCanvas::physSyncRect(int32_t py, int32_t h) {
  if (!sync_ || fb_ == nullptr || h <= 0) {
    return;
  }
  const int32_t y0 = std::max<int32_t>(0, py);
  const int32_t y1 = std::min<int32_t>(fb_h_, py + h);
  if (y1 <= y0) {
    return;
  }
  uint16_t* start = fb_ + static_cast<size_t>(y0) * static_cast<size_t>(fb_w_);
  const size_t bytes =
      static_cast<size_t>(y1 - y0) * static_cast<size_t>(fb_w_) * sizeof(uint16_t);
  esp_cache_msync(start, bytes,
                  ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

void ScaledCanvas::blitPhysical(int16_t px, int16_t py, const uint16_t* src,
                                int16_t w, int16_t h, int16_t src_stride) {
  if (fb_ == nullptr || src == nullptr || w <= 0 || h <= 0 || src_stride <= 0) {
    return;
  }
  if (px < 0) {
    src += static_cast<size_t>(-px);
    w = static_cast<int16_t>(w + px);
    px = 0;
  }
  if (py < 0) {
    src += static_cast<size_t>(-py) * static_cast<size_t>(src_stride);
    h = static_cast<int16_t>(h + py);
    py = 0;
  }
  if (px + w > fb_w_) {
    w = static_cast<int16_t>(fb_w_ - px);
  }
  if (py + h > fb_h_) {
    h = static_cast<int16_t>(fb_h_ - py);
  }
  if (w <= 0 || h <= 0) {
    return;
  }

  for (int16_t row = 0; row < h; ++row) {
    memcpy(fb_ + (static_cast<size_t>(py) + row) * static_cast<size_t>(fb_w_) +
               static_cast<size_t>(px),
           src + static_cast<size_t>(row) * static_cast<size_t>(src_stride),
           static_cast<size_t>(w) * sizeof(uint16_t));
  }
  physSyncRect(py, h);
}

void ScaledCanvas::blitPhysicalTransparent(int16_t px, int16_t py,
                                           const uint16_t* src, int16_t w,
                                           int16_t h, int16_t src_stride,
                                           uint16_t transparent_color) {
  if (fb_ == nullptr || src == nullptr || w <= 0 || h <= 0 || src_stride <= 0) {
    return;
  }
  if (px < 0) {
    src += static_cast<size_t>(-px);
    w = static_cast<int16_t>(w + px);
    px = 0;
  }
  if (py < 0) {
    src += static_cast<size_t>(-py) * static_cast<size_t>(src_stride);
    h = static_cast<int16_t>(h + py);
    py = 0;
  }
  if (px + w > fb_w_) {
    w = static_cast<int16_t>(fb_w_ - px);
  }
  if (py + h > fb_h_) {
    h = static_cast<int16_t>(fb_h_ - py);
  }
  if (w <= 0 || h <= 0) {
    return;
  }

  for (int16_t row = 0; row < h; ++row) {
    const uint16_t* s =
        src + static_cast<size_t>(row) * static_cast<size_t>(src_stride);
    uint16_t* d = fb_ +
                  (static_cast<size_t>(py) + row) * static_cast<size_t>(fb_w_) +
                  static_cast<size_t>(px);
    for (int16_t col = 0; col < w; ++col) {
      if (s[col] != transparent_color) {
        d[col] = s[col];
      }
    }
  }
  physSyncRect(py, h);
}

void ScaledCanvas::physLineNoSync(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                                  uint16_t color, int32_t* ymin, int32_t* ymax) {
  // Bresenham straight into the framebuffer (bounds-checked per pixel).
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
    const int32_t plot_x = steep ? y : x;
    const int32_t plot_y = steep ? x : y;
    physFillPixel(plot_x, plot_y, color);
    if (plot_y < *ymin) {
      *ymin = plot_y;
    }
    if (plot_y > *ymax) {
      *ymax = plot_y;
    }
    err -= dy;
    if (err < 0) {
      y += ystep;
      err += dx;
    }
  }
}

void ScaledCanvas::physWideLine(int32_t px0, int32_t py0, int32_t px1,
                                int32_t py1, float half_width, uint16_t color) {
  if (fb_ == nullptr) {
    return;
  }
  // Same offset-stack widening the app used in logical space (PlaneGfx::
  // drawWideLine), so stroke shape and endpoints match the previous look.
  const int steps = std::max(1, static_cast<int>(half_width * 2.0f + 0.5f));
  const float offset = -half_width;
  int32_t ymin = fb_h_;
  int32_t ymax = -1;
  for (int i = 0; i < steps; ++i) {
    const float t = offset + static_cast<float>(i);
    const int32_t ox = static_cast<int32_t>(std::lround(t));
    const int32_t oy = static_cast<int32_t>(std::lround(-t));
    physLineNoSync(px0 + ox, py0 + oy, px1 + ox, py1 + oy, color, &ymin, &ymax);
  }
  if (ymax >= ymin) {
    physSyncRect(ymin, ymax - ymin + 1);
  }
}

void ScaledCanvas::blitScaled(int16_t x, int16_t y, const uint16_t* bitmap,
                              int16_t w, int16_t h,
                              uint32_t transparent_color) {
  if (bitmap == nullptr || w <= 0 || h <= 0 || fb_ == nullptr) {
    return;
  }

  // Clip in logical space (src_stride stays the caller's full bitmap width).
  const int32_t src_stride = w;
  int16_t src_x = 0;
  int16_t src_y = 0;
  if (x < 0) {
    src_x = static_cast<int16_t>(-x);
    w = static_cast<int16_t>(w - src_x);
    x = 0;
  }
  if (y < 0) {
    src_y = static_cast<int16_t>(-y);
    h = static_cast<int16_t>(h - src_y);
    y = 0;
  }
  if (x + w > logical_w_) {
    w = static_cast<int16_t>(logical_w_ - x);
  }
  if (y + h > logical_h_) {
    h = static_cast<int16_t>(logical_h_ - y);
  }
  if (w <= 0 || h <= 0) {
    return;
  }

  for (int16_t row = 0; row < h; ++row) {
    const uint16_t* src =
        bitmap + static_cast<int32_t>(src_y + row) * src_stride + src_x;
    const int32_t py0 = map(y + row);
    const int32_t py1 = map(y + row + 1);
    uint16_t* dst_row0 = fb_ + py0 * fb_w_;
    for (int16_t col = 0; col < w; ++col) {
      const uint16_t v = src[col];
      if (static_cast<uint32_t>(v) == transparent_color) {
        continue;
      }
      const int32_t px0 = map(x + col);
      const int32_t px1 = map(x + col + 1);
      for (int32_t py = py0; py < py1; ++py) {
        uint16_t* d = dst_row0 + (py - py0) * fb_w_ + px0;
        for (int32_t px = px0; px < px1; ++px) {
          *d++ = v;
        }
      }
    }
  }

  physSyncRect(map(y), map(y + h) - map(y));
}
