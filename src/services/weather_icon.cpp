#include "services/weather_icon.h"

#include <AnimatedGIF.h>
#include <esp_heap_caps.h>
#include <new>
#include <Arduino.h>

#include "data/weather_icons_lookup.h"
#include "hardware/plane_gfx.h"

namespace services::weather_icon {

namespace {

// PSRAM-resident: the GIFIMAGE state is tens of KB and internal RAM is scarce
// on the P4 (ESP-Hosted SDIO needs a big internal DMA pool).
AnimatedGIF& gifInstance() {
  static AnimatedGIF* inst = new (heap_caps_malloc(sizeof(AnimatedGIF), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) AnimatedGIF();
  return *inst;
}
#define s_gif gifInstance()
uint16_t* s_frame = nullptr;
int s_frame_w = 0;
int s_frame_h = 0;
bool s_gif_ready = false;

const data::weather_icons::IconBlob* findIconBlob(int code) {
  if (code <= 0 || data::weather_icons::kCount == 0) {
    return nullptr;
  }
  const uint32_t key = static_cast<uint32_t>(code);
  size_t lo = 0;
  size_t hi = data::weather_icons::kCount;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    const data::weather_icons::Entry* entry = &data::weather_icons::kEntries[mid];
    const uint32_t c = static_cast<uint32_t>(pgm_read_dword(&entry->code));
    if (c == key) {
      return reinterpret_cast<const data::weather_icons::IconBlob*>(
          pgm_read_ptr(&entry->icon));
    }
    if (key < c) {
      hi = mid;
    } else {
      lo = mid + 1;
    }
  }
  return nullptr;
}

bool ensureFrameBuffer(int w, int h) {
  if (w <= 0 || h <= 0 || w > data::weather_icons::kMaxIconW ||
      h > data::weather_icons::kMaxIconH) {
    return false;
  }
  if (s_frame != nullptr && s_frame_w == w && s_frame_h == h) {
    return true;
  }
  if (s_frame != nullptr) {
    free(s_frame);
    s_frame = nullptr;
  }
  const size_t pixels = static_cast<size_t>(w) * static_cast<size_t>(h);
  s_frame = static_cast<uint16_t*>(ps_malloc(pixels * sizeof(uint16_t)));
  if (s_frame == nullptr) {
    s_frame = static_cast<uint16_t*>(malloc(pixels * sizeof(uint16_t)));
  }
  if (s_frame == nullptr) {
    return false;
  }
  s_frame_w = w;
  s_frame_h = h;
  return true;
}

void GIFDraw(GIFDRAW* p_draw) {
  if (s_frame == nullptr || p_draw == nullptr) {
    return;
  }
  const int y = p_draw->y;
  if (y < 0 || y >= s_frame_h) {
    return;
  }
  uint8_t* pixels = p_draw->pPixels;
  uint16_t* palette = p_draw->pPalette;
  uint16_t* row = s_frame + y * s_frame_w;
  const int width = p_draw->iWidth;
  if (p_draw->ucHasTransparency) {
    const uint8_t transparent = p_draw->ucTransparent;
    for (int i = 0; i < width; ++i) {
      row[i] = (pixels[i] == transparent) ? data::weather_icons::kTransparentRgb565
                                          : palette[pixels[i]];
    }
  } else {
    for (int i = 0; i < width; ++i) {
      row[i] = palette[pixels[i]];
    }
  }
}

bool decodeGifToFrame(const data::weather_icons::IconBlob* blob) {
  if (blob == nullptr) {
    return false;
  }
  const uint16_t w = pgm_read_word(&blob->w);
  const uint16_t h = pgm_read_word(&blob->h);
  const uint32_t size = pgm_read_dword(&blob->size);
  const uint8_t* gif = reinterpret_cast<const uint8_t*>(pgm_read_ptr(&blob->gif));
  if (gif == nullptr || size == 0 || w == 0 || h == 0) {
    return false;
  }
  if (!ensureFrameBuffer(w, h)) {
    return false;
  }
  for (size_t i = 0; i < static_cast<size_t>(w) * static_cast<size_t>(h); ++i) {
    s_frame[i] = data::weather_icons::kTransparentRgb565;
  }
  if (!s_gif_ready) {
    s_gif.begin(LITTLE_ENDIAN_PIXELS);
    s_gif_ready = true;
  }
  if (!s_gif.open(const_cast<uint8_t*>(gif), static_cast<int>(size), GIFDraw)) {
    return false;
  }
  s_gif.playFrame(true, nullptr);
  s_gif.close();
  return true;
}

}  // namespace

bool hasIcon(int code) { return findIconBlob(code) != nullptr; }

int iconWidth(int code) {
  const data::weather_icons::IconBlob* blob = findIconBlob(code);
  return blob == nullptr ? 0 : static_cast<int>(pgm_read_word(&blob->w));
}

int iconHeight(int code) {
  const data::weather_icons::IconBlob* blob = findIconBlob(code);
  return blob == nullptr ? 0 : static_cast<int>(pgm_read_word(&blob->h));
}

bool drawBlob(PlaneGfx& tft, const data::weather_icons::IconBlob* blob, int16_t center_x,
              int16_t y, uint16_t bg) {
  if (blob == nullptr || !decodeGifToFrame(blob)) {
    return false;
  }
  const int w = s_frame_w;
  const int h = s_frame_h;
  const size_t pixels = static_cast<size_t>(w) * static_cast<size_t>(h);
  const uint16_t key = data::weather_icons::kTransparentRgb565;
  for (size_t i = 0; i < pixels; ++i) {
    if (s_frame[i] == key) {
      s_frame[i] = bg;
    }
  }
  const int16_t x = static_cast<int16_t>(center_x - w / 2);
  tft.draw16bitRGBBitmap(x, y, s_frame, static_cast<int16_t>(w), static_cast<int16_t>(h));
  return true;
}

bool drawIcon(PlaneGfx& tft, int code, int16_t center_x, int16_t y, uint16_t bg) {
  return drawBlob(tft, findIconBlob(code), center_x, y, bg);
}

bool drawIconScaled(PlaneGfx& tft, int code, int16_t center_x, int16_t y, uint16_t bg,
                    int target_size) {
  const data::weather_icons::IconBlob* blob = findIconBlob(code);
  if (blob == nullptr || !decodeGifToFrame(blob)) {
    return false;
  }
  const int w = s_frame_w;
  const int h = s_frame_h;
  if (target_size <= 0 || target_size >= w) {
    return drawBlob(tft, blob, center_x, y, bg);
  }
  const int tw = target_size;
  const int th = (h * target_size) / w;
  const uint16_t key = data::weather_icons::kTransparentRgb565;
  const int16_t x = static_cast<int16_t>(center_x - tw / 2);
  // Nearest-neighbor downscale, one row at a time to keep RAM tiny.
  uint16_t row[data::weather_icons::kMaxIconW];
  for (int ty = 0; ty < th; ++ty) {
    const int sy = (ty * h) / th;
    const uint16_t* src = s_frame + sy * w;
    for (int tx = 0; tx < tw; ++tx) {
      const uint16_t px = src[(tx * w) / tw];
      row[tx] = (px == key) ? bg : px;
    }
    tft.draw16bitRGBBitmap(x, static_cast<int16_t>(y + ty), row, static_cast<int16_t>(tw), 1);
  }
  return true;
}

bool hasSunIcons() { return data::weather_icons::kHasSunIcons; }

int sunIconSize() {
  return data::weather_icons::kHasSunIcons
             ? static_cast<int>(pgm_read_word(&data::weather_icons::kBlob_sunrise.w))
             : 0;
}

bool drawSunIcon(PlaneGfx& tft, bool sunset, int16_t center_x, int16_t y, uint16_t bg) {
  if (!data::weather_icons::kHasSunIcons) {
    return false;
  }
  const data::weather_icons::IconBlob* blob =
      sunset ? &data::weather_icons::kBlob_sunset : &data::weather_icons::kBlob_sunrise;
  return drawBlob(tft, blob, center_x, y, bg);
}

void releaseBuffer() {
  if (s_frame != nullptr) {
    free(s_frame);
    s_frame = nullptr;
  }
  s_frame_w = 0;
  s_frame_h = 0;
}

}  // namespace services::weather_icon
