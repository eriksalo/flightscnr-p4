#include "ui/wifi_setup_screen.h"

#include <Arduino.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "config.h"
#include "hardware/display.h"
#include "hardware/display_font.h"
#include "hardware/input.h"

// On-device Wi-Fi setup for the touch-only 3.4C: every rectangle is laid out
// to stay inside the 720 px circle (radius 360 around 360,360) — corners of
// the square canvas do not exist on the glass.

namespace ui {
namespace {

constexpr int kCx = config::kDisplayWidth / 2;

constexpr uint16_t kBg = config::kColorBlack;
constexpr uint16_t kText = config::kTextOnBlack;

uint16_t colAccent() { return tft.color565(26, 156, 60); }
uint16_t colKey() { return tft.color565(40, 44, 50); }
uint16_t colKeySpecial() { return tft.color565(62, 68, 76); }
uint16_t colField() { return tft.color565(22, 24, 28); }
uint16_t colDim() { return tft.color565(140, 146, 152); }
uint16_t colWarn() { return tft.color565(255, 176, 64); }

struct Rect {
  int16_t x = 0;
  int16_t y = 0;
  int16_t w = 0;
  int16_t h = 0;
  bool contains(int16_t px, int16_t py) const {
    // 6 px slop: the GT911 release point wanders a little on quick taps.
    return w > 0 && px >= x - 6 && px < x + w + 6 && py >= y - 6 && py < y + h + 6;
  }
};

void fillRectR(const Rect& r, uint16_t color) { tft.fillRect(r.x, r.y, r.w, r.h, color); }

void drawCenteredIn(const Rect& r, const char* text) {
  tft.setTextDatum(TextDatum::MiddleCenter);
  tft.drawString(text, r.x + r.w / 2, r.y + r.h / 2);
}

/** Truncate text with a trailing ellipsis until it fits max_px (current font). */
void fitText(const char* in, char* out, size_t out_len, int max_px) {
  strncpy(out, in, out_len - 1);
  out[out_len - 1] = '\0';
  if (tft.textWidth(out) <= max_px) {
    return;
  }
  const size_t len = strlen(out);
  for (size_t n = len; n > 0; --n) {
    char probe[64];
    snprintf(probe, sizeof(probe), "%.*s…", static_cast<int>(n), in);
    if (tft.textWidth(probe) <= max_px) {
      strncpy(out, probe, out_len - 1);
      out[out_len - 1] = '\0';
      return;
    }
  }
  strncpy(out, "…", out_len - 1);
  out[out_len - 1] = '\0';
}

// --- scan results -----------------------------------------------------------

struct ScanNet {
  char ssid[33];
  int16_t rssi;
  bool secured;
};

constexpr uint8_t kMaxScan = 24;
ScanNet s_scan[kMaxScan];
uint8_t s_scan_n = 0;

void drawScanningScreen() {
  tft.fillScreen(kBg);
  tft.setTextColor(kText, kBg);
  tft.setTextDatum(TextDatum::MiddleCenter);
  displayFontApply(tft, displayFontTitle());
  tft.drawString("Scanning…", kCx, kCx);
}

void scanNetworks() {
  drawScanningScreen();
  s_scan_n = 0;
  if (WiFi.getMode() == WIFI_MODE_NULL) {
    WiFi.mode(WIFI_STA);
    delay(50);
  }
  const int n = WiFi.scanNetworks(/*async=*/false, /*hidden=*/false);
  for (int i = 0; i < n; ++i) {
    const String ssid = WiFi.SSID(i);
    if (ssid.length() == 0 || ssid.length() > 32) {
      continue;
    }
    const int16_t rssi = static_cast<int16_t>(WiFi.RSSI(i));
    const bool secured = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    int dup = -1;
    for (uint8_t k = 0; k < s_scan_n; ++k) {
      if (strcmp(s_scan[k].ssid, ssid.c_str()) == 0) {
        dup = k;
        break;
      }
    }
    if (dup >= 0) {
      if (rssi > s_scan[dup].rssi) {
        s_scan[dup].rssi = rssi;  // several APs, one network: keep strongest
      }
      continue;
    }
    if (s_scan_n >= kMaxScan) {
      continue;
    }
    strncpy(s_scan[s_scan_n].ssid, ssid.c_str(), sizeof(s_scan[0].ssid) - 1);
    s_scan[s_scan_n].ssid[sizeof(s_scan[0].ssid) - 1] = '\0';
    s_scan[s_scan_n].rssi = rssi;
    s_scan[s_scan_n].secured = secured;
    ++s_scan_n;
  }
  WiFi.scanDelete();
  std::sort(s_scan, s_scan + s_scan_n,
            [](const ScanNet& a, const ScanNet& b) { return a.rssi > b.rssi; });
  Serial.printf("[wifi] touch setup scan: %u networks\n",
                static_cast<unsigned>(s_scan_n));
}

// --- network list screen ------------------------------------------------------

constexpr uint8_t kRowsPerPage = 5;
constexpr int kRowW = 520;
constexpr int kRowH = 64;
constexpr int kRowGap = 10;
constexpr int kRowX = (config::kDisplayWidth - kRowW) / 2;
constexpr int kRowY0 = 148;

Rect s_r_row[kRowsPerPage];
Rect s_r_page_up;
Rect s_r_rescan;
Rect s_r_page_down;
Rect s_r_web;
Rect s_r_cancel;

uint8_t listEntryCount() { return static_cast<uint8_t>(s_scan_n + 1); }  // +Other…

uint8_t listPageCount() {
  return static_cast<uint8_t>((listEntryCount() + kRowsPerPage - 1) / kRowsPerPage);
}

void drawSignalBars(int right_x, int mid_y, int16_t rssi) {
  const int lit = rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : 1;
  const uint16_t dim = tft.color565(70, 76, 84);
  for (int b = 0; b < 4; ++b) {
    const int h = 12 + b * 8;
    const int x = right_x - (4 - b) * 14;
    tft.fillRect(x, mid_y + 18 - h, 10, h, b < lit ? colAccent() : dim);
  }
}

void drawLockGlyph(int cx, int mid_y) {
  tft.drawCircle(cx, mid_y - 6, 6, colDim());
  tft.fillRect(cx - 9, mid_y - 4, 18, 16, colDim());
}

void drawTriangleArrow(const Rect& r, bool up, uint16_t color) {
  const int cx = r.x + r.w / 2;
  const int cy = r.y + r.h / 2;
  if (up) {
    tft.fillTriangle(cx, cy - 10, cx - 14, cy + 8, cx + 14, cy + 8, color);
  } else {
    tft.fillTriangle(cx, cy + 10, cx - 14, cy - 8, cx + 14, cy - 8, color);
  }
}

void drawListScreen(uint8_t page, const char* status, bool boot_mode) {
  tft.fillScreen(kBg);
  tft.setTextColor(kText, kBg);
  tft.setTextDatum(TextDatum::MiddleCenter);
  displayFontApply(tft, displayFontTitle());
  tft.drawString("Wi-Fi setup", kCx, 78);

  displayFontApply(tft, displayFontDetail());
  if (status != nullptr && status[0] != '\0') {
    char fitted[96];
    fitText(status, fitted, sizeof(fitted), 560);
    tft.setTextColor(colWarn(), kBg);
    tft.drawString(fitted, kCx, 120);
  } else {
    tft.setTextColor(colDim(), kBg);
    tft.drawString("Tap a network to join", kCx, 120);
  }

  const uint8_t pages = listPageCount();
  const uint8_t entries = listEntryCount();
  const String connected_ssid = WiFi.status() == WL_CONNECTED ? WiFi.SSID() : String();
  const char* connected = connected_ssid.c_str();

  for (uint8_t slot = 0; slot < kRowsPerPage; ++slot) {
    s_r_row[slot] = Rect{};
    const uint16_t entry = static_cast<uint16_t>(page) * kRowsPerPage + slot;
    if (entry >= entries) {
      continue;
    }
    const Rect r{static_cast<int16_t>(kRowX),
                 static_cast<int16_t>(kRowY0 + slot * (kRowH + kRowGap)),
                 static_cast<int16_t>(kRowW), static_cast<int16_t>(kRowH)};
    s_r_row[slot] = r;
    fillRectR(r, colKey());

    displayFontApply(tft, displayFontBody());
    tft.setTextDatum(TextDatum::MiddleLeft);
    const int mid_y = r.y + r.h / 2;
    if (entry < s_scan_n) {
      const ScanNet& net = s_scan[entry];
      const bool is_connected = connected[0] != '\0' && strcmp(net.ssid, connected) == 0;
      char fitted[48];
      fitText(net.ssid, fitted, sizeof(fitted), kRowW - 130);
      tft.setTextColor(is_connected ? colAccent() : kText, colKey());
      tft.drawString(fitted, r.x + 20, mid_y);
      drawSignalBars(r.x + r.w - (net.secured ? 44 : 16), mid_y, net.rssi);
      if (net.secured) {
        drawLockGlyph(r.x + r.w - 26, mid_y);
      }
    } else {
      tft.setTextColor(colDim(), colKey());
      tft.drawString("Other network…", r.x + 20, mid_y);
    }
  }

  // Pager + rescan row.
  const int pager_y = 524;
  s_r_page_up = Rect{142, pager_y, 110, 56};
  s_r_rescan = Rect{260, pager_y, 200, 56};
  s_r_page_down = Rect{468, pager_y, 110, 56};

  fillRectR(s_r_rescan, colKeySpecial());
  displayFontApply(tft, displayFontBody());
  tft.setTextColor(kText, colKeySpecial());
  drawCenteredIn(s_r_rescan, "Rescan");

  const bool can_up = page > 0;
  const bool can_down = page + 1 < pages;
  fillRectR(s_r_page_up, colKeySpecial());
  drawTriangleArrow(s_r_page_up, true, can_up ? kText : colDim());
  fillRectR(s_r_page_down, colKeySpecial());
  drawTriangleArrow(s_r_page_down, false, can_down ? kText : colDim());

  // Bottom row: web fallback + cancel.
  s_r_web = Rect{164, 592, 190, 56};
  s_r_cancel = Rect{366, 592, 190, 56};
  fillRectR(s_r_web, colKeySpecial());
  displayFontApply(tft, displayFontBody());
  tft.setTextColor(kText, colKeySpecial());
  drawCenteredIn(s_r_web, "Use phone");
  fillRectR(s_r_cancel, colKeySpecial());
  drawCenteredIn(s_r_cancel, boot_mode ? "Retry saved" : "Cancel");
}

// --- on-screen keyboard ---------------------------------------------------------

// Rows are 10 / 9 / 7 characters wide; row 3 is the fixed bottom row
// (mode, space, period, OK). All rows fit the circle down to y=558.
struct KbPage {
  const char* r0;
  const char* r1;
  const char* r2;
};

constexpr KbPage kKbPages[] = {
    {"qwertyuiop", "asdfghjkl", "zxcvbnm"},
    {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"},
    {"1234567890", "@#$%&*()-", "_+=/:;!"},
    {"1234567890", "\"'<>?[]{}", "\\^`~,.|"},
};

enum KeyAction : uint8_t {
  KeyChar = 0,
  KeyShift,
  KeyBackspace,
  KeyMode,
  KeySpace,
  KeyOk,
  KeyClose,
};

struct KeySpec {
  Rect r;
  char ch;
  uint8_t action;
};

constexpr int kKeyW = 54;
constexpr int kKeyGap = 4;
constexpr int kKeyH = 72;
constexpr int kKeyPitchY = 80;
constexpr int kKbTopY = 246;

constexpr Rect kFieldRect{90, 124, 460, 56};
constexpr int kCloseCx = 596;
constexpr int kCloseCy = 152;
constexpr int kCloseR = 30;

KeySpec s_keys[44];
uint8_t s_key_n = 0;

void addKey(int x, int y, int w, char ch, uint8_t action) {
  if (s_key_n >= sizeof(s_keys) / sizeof(s_keys[0])) {
    return;
  }
  s_keys[s_key_n].r = Rect{static_cast<int16_t>(x), static_cast<int16_t>(y),
                           static_cast<int16_t>(w), static_cast<int16_t>(kKeyH)};
  s_keys[s_key_n].ch = ch;
  s_keys[s_key_n].action = action;
  ++s_key_n;
}

void addCharRow(const char* chars, int y) {
  const int n = static_cast<int>(strlen(chars));
  const int row_w = n * kKeyW + (n - 1) * kKeyGap;
  int x = (config::kDisplayWidth - row_w) / 2;
  for (int i = 0; i < n; ++i) {
    addKey(x, y, kKeyW, chars[i], KeyChar);
    x += kKeyW + kKeyGap;
  }
}

void buildKeys(uint8_t page) {
  s_key_n = 0;
  const KbPage& p = kKbPages[page];
  addCharRow(p.r0, kKbTopY);
  addCharRow(p.r1, kKbTopY + kKeyPitchY);

  // Row 2: shift/page-toggle + letters + backspace.
  {
    const int y = kKbTopY + 2 * kKeyPitchY;
    const int n = static_cast<int>(strlen(p.r2));
    const int row_w = 78 + kKeyGap + n * kKeyW + (n - 1) * kKeyGap + kKeyGap + 78;
    int x = (config::kDisplayWidth - row_w) / 2;
    addKey(x, y, 78, '\0', KeyShift);
    x += 78 + kKeyGap;
    for (int i = 0; i < n; ++i) {
      addKey(x, y, kKeyW, p.r2[i], KeyChar);
      x += kKeyW + kKeyGap;
    }
    addKey(x, y, 78, '\0', KeyBackspace);
  }

  // Row 3: mode, space, period, OK.
  {
    const int y = kKbTopY + 3 * kKeyPitchY;
    const int row_w = 96 + kKeyGap + 268 + kKeyGap + kKeyW + kKeyGap + 116;
    int x = (config::kDisplayWidth - row_w) / 2;
    addKey(x, y, 96, '\0', KeyMode);
    x += 96 + kKeyGap;
    addKey(x, y, 268, ' ', KeySpace);
    x += 268 + kKeyGap;
    addKey(x, y, kKeyW, '.', KeyChar);
    x += kKeyW + kKeyGap;
    addKey(x, y, 116, '\0', KeyOk);
  }
}

void drawShiftGlyph(const Rect& r, bool active) {
  const int cx = r.x + r.w / 2;
  const int cy = r.y + r.h / 2;
  const uint16_t color = active ? colAccent() : kText;
  tft.fillTriangle(cx, cy - 14, cx - 13, cy + 2, cx + 13, cy + 2, color);
  tft.fillRect(cx - 5, cy + 2, 10, 12, color);
}

void drawBackspaceGlyph(const Rect& r) {
  const int cx = r.x + r.w / 2;
  const int cy = r.y + r.h / 2;
  tft.fillTriangle(cx - 16, cy, cx - 2, cy - 11, cx - 2, cy + 11, kText);
  tft.fillRect(cx - 2, cy - 11, 18, 23, kText);
  // The x inside the backspace body, in key color for contrast.
  tft.drawLine(cx + 2, cy - 5, cx + 12, cy + 5, colKeySpecial());
  tft.drawLine(cx + 2, cy + 5, cx + 12, cy - 5, colKeySpecial());
}

void drawKeyboardField(const char* buf) {
  fillRectR(kFieldRect, colField());
  displayFontApply(tft, displayFontBody());
  tft.setTextColor(kText, colField());
  tft.setTextDatum(TextDatum::MiddleLeft);

  // Show the tail when the text is wider than the field (typing stays visible).
  const int max_px = kFieldRect.w - 32;
  const char* shown = buf;
  while (shown[0] != '\0' && tft.textWidth(shown) > max_px) {
    ++shown;
  }
  const int text_x = kFieldRect.x + 16;
  const int mid_y = kFieldRect.y + kFieldRect.h / 2;
  tft.drawString(shown, text_x, mid_y);
  const int cursor_x = text_x + (shown[0] != '\0' ? tft.textWidth(shown) + 3 : 0);
  tft.fillRect(cursor_x, mid_y - 18, 3, 36, colAccent());
}

void drawKeyboardScreen(const char* prompt, const char* subtitle, bool subtitle_warn,
                        uint8_t page, bool shift_pending, const char* buf) {
  tft.fillScreen(kBg);
  tft.setTextColor(kText, kBg);
  tft.setTextDatum(TextDatum::MiddleCenter);
  displayFontApply(tft, displayFontBody());
  tft.drawString(prompt, kCx, 56);
  if (subtitle != nullptr && subtitle[0] != '\0') {
    char fitted[96];
    displayFontApply(tft, displayFontDetail());
    fitText(subtitle, fitted, sizeof(fitted), 430);
    tft.setTextColor(subtitle_warn ? colWarn() : colDim(), kBg);
    tft.drawString(fitted, kCx, 94);
  }

  // Close (✕) button, top right inside the circle.
  tft.fillCircle(kCloseCx, kCloseCy, kCloseR, colKeySpecial());
  tft.drawLine(kCloseCx - 10, kCloseCy - 10, kCloseCx + 10, kCloseCy + 10, kText);
  tft.drawLine(kCloseCx - 10, kCloseCy + 10, kCloseCx + 10, kCloseCy - 10, kText);

  drawKeyboardField(buf);

  buildKeys(page);
  for (uint8_t i = 0; i < s_key_n; ++i) {
    const KeySpec& k = s_keys[i];
    const bool special = k.action != KeyChar && k.action != KeySpace;
    fillRectR(k.r, k.action == KeyOk ? colAccent()
                                     : (special ? colKeySpecial() : colKey()));
    switch (k.action) {
      case KeyChar: {
        char label[2] = {k.ch, '\0'};
        displayFontApply(tft, displayFontBody());
        tft.setTextColor(kText, colKey());
        drawCenteredIn(k.r, label);
        break;
      }
      case KeyShift:
        if (page >= 2) {
          displayFontApply(tft, displayFontDetail());
          tft.setTextColor(kText, colKeySpecial());
          drawCenteredIn(k.r, page == 2 ? "1/2" : "2/2");
        } else {
          drawShiftGlyph(k.r, shift_pending);
        }
        break;
      case KeyBackspace:
        drawBackspaceGlyph(k.r);
        break;
      case KeyMode:
        displayFontApply(tft, displayFontDetail());
        tft.setTextColor(kText, colKeySpecial());
        drawCenteredIn(k.r, page >= 2 ? "abc" : "?123");
        break;
      case KeySpace:
        displayFontApply(tft, displayFontDetail());
        tft.setTextColor(colDim(), colKey());
        drawCenteredIn(k.r, "space");
        break;
      case KeyOk:
        displayFontApply(tft, displayFontBody());
        tft.setTextColor(kText, colAccent());
        drawCenteredIn(k.r, "OK");
        break;
      default:
        break;
    }
  }
}

enum class KbResult : uint8_t { Ok, Cancelled, Timeout };

/** Blocking on-screen keyboard editing buf in place (ASCII only). */
KbResult keyboardModal(const char* prompt, const char* subtitle, bool subtitle_warn,
                       char* buf, size_t buf_cap, unsigned long idle_timeout_ms) {
  uint8_t page = 0;
  bool shift_pending = false;
  drawKeyboardScreen(prompt, subtitle, subtitle_warn, page, shift_pending, buf);

  unsigned long last_activity = millis();
  for (;;) {
    inputPoll();
    (void)inputConsumeLongPress();

    const SwipeGesture swipe = inputConsumeSwipe();
    if (swipe == SwipeRight) {
      return KbResult::Cancelled;
    }

    int16_t tx = 0;
    int16_t ty = 0;
    if (inputConsumeScreenTap(&tx, &ty)) {
      last_activity = millis();

      const int dx = tx - kCloseCx;
      const int dy = ty - kCloseCy;
      if (dx * dx + dy * dy <= (kCloseR + 8) * (kCloseR + 8)) {
        return KbResult::Cancelled;
      }

      // The 6 px hit slop makes neighbours overlap in the 4 px key gaps:
      // pick the key whose center is closest instead of the first match.
      int best = -1;
      int32_t best_d2 = INT32_MAX;
      for (uint8_t i = 0; i < s_key_n; ++i) {
        if (!s_keys[i].r.contains(tx, ty)) {
          continue;
        }
        const int32_t dcx = tx - (s_keys[i].r.x + s_keys[i].r.w / 2);
        const int32_t dcy = ty - (s_keys[i].r.y + s_keys[i].r.h / 2);
        const int32_t d2 = dcx * dcx + dcy * dcy;
        if (d2 < best_d2) {
          best_d2 = d2;
          best = i;
        }
      }
      if (best >= 0) {
        const KeySpec& k = s_keys[best];
        const size_t len = strlen(buf);
        switch (k.action) {
          case KeyChar:
          case KeySpace:
            if (len + 1 < buf_cap) {
              buf[len] = k.ch;
              buf[len + 1] = '\0';
              drawKeyboardField(buf);
              if (shift_pending && page == 1) {
                // One-shot shift, like a phone keyboard.
                shift_pending = false;
                page = 0;
                drawKeyboardScreen(prompt, subtitle, subtitle_warn, page,
                                   shift_pending, buf);
              }
            }
            break;
          case KeyBackspace:
            if (len > 0) {
              buf[len - 1] = '\0';
              drawKeyboardField(buf);
            }
            break;
          case KeyShift:
            if (page >= 2) {
              page = page == 2 ? 3 : 2;
            } else {
              shift_pending = page == 0;
              page = page == 0 ? 1 : 0;
            }
            drawKeyboardScreen(prompt, subtitle, subtitle_warn, page, shift_pending,
                               buf);
            break;
          case KeyMode:
            page = page >= 2 ? 0 : 2;
            shift_pending = false;
            drawKeyboardScreen(prompt, subtitle, subtitle_warn, page, shift_pending,
                               buf);
            break;
          case KeyOk:
            return KbResult::Ok;
          default:
            break;
        }
      }
    }

    if (idle_timeout_ms != 0 && millis() - last_activity >= idle_timeout_ms) {
      return KbResult::Timeout;
    }
    delay(5);
  }
}

}  // namespace

WifiPickResult wifiSetupScreenPick(const WifiPickOptions& opts, char* ssid_out,
                                   size_t ssid_len, char* pass_out, size_t pass_len) {
  if (ssid_out == nullptr || pass_out == nullptr || ssid_len == 0 || pass_len == 0) {
    return WifiPickResult::Cancelled;
  }
  inputDiscardPendingInteractions();

  char status[96] = "";
  if (opts.status != nullptr) {
    strncpy(status, opts.status, sizeof(status) - 1);
    status[sizeof(status) - 1] = '\0';
  }

  // Callers may pass the out buffers themselves as the retry values, so stash
  // the retry credentials before clearing the outputs.
  char retry_ssid[33] = "";
  char retry_pass[65] = "";
  if (opts.retry_ssid != nullptr) {
    strncpy(retry_ssid, opts.retry_ssid, sizeof(retry_ssid) - 1);
    retry_ssid[sizeof(retry_ssid) - 1] = '\0';
  }
  if (opts.retry_pass != nullptr) {
    strncpy(retry_pass, opts.retry_pass, sizeof(retry_pass) - 1);
    retry_pass[sizeof(retry_pass) - 1] = '\0';
  }
  ssid_out[0] = '\0';
  pass_out[0] = '\0';

  // Connect retry: jump straight back onto the password keyboard.
  if (retry_ssid[0] != '\0') {
    strncpy(ssid_out, retry_ssid, ssid_len - 1);
    ssid_out[ssid_len - 1] = '\0';
    strncpy(pass_out, retry_pass, pass_len - 1);
    pass_out[pass_len - 1] = '\0';
    char subtitle[64];
    snprintf(subtitle, sizeof(subtitle), "for %s", ssid_out);
    const KbResult kb = keyboardModal("Password", status[0] != '\0' ? status : subtitle,
                                      status[0] != '\0', pass_out, pass_len,
                                      opts.idle_timeout_ms);
    if (kb == KbResult::Ok) {
      return WifiPickResult::Credentials;
    }
    if (kb == KbResult::Timeout) {
      return WifiPickResult::Cancelled;
    }
    status[0] = '\0';  // cancelled out of retry: fall through to the list
  }

  scanNetworks();
  uint8_t page = 0;
  bool need_redraw = true;
  unsigned long last_activity = millis();

  for (;;) {
    if (need_redraw) {
      drawListScreen(page, status[0] != '\0' ? status : nullptr, opts.boot_mode);
      need_redraw = false;
    }

    inputPoll();
    (void)inputConsumeLongPress();

    const SwipeGesture swipe = inputConsumeSwipe();
    if (swipe == SwipeUp && page + 1 < listPageCount()) {
      ++page;
      need_redraw = true;
      last_activity = millis();
    } else if (swipe == SwipeDown && page > 0) {
      --page;
      need_redraw = true;
      last_activity = millis();
    }

    int16_t tx = 0;
    int16_t ty = 0;
    if (inputConsumeScreenTap(&tx, &ty)) {
      last_activity = millis();

      if (s_r_cancel.contains(tx, ty)) {
        return WifiPickResult::Cancelled;
      }
      if (s_r_web.contains(tx, ty)) {
        return WifiPickResult::WebPortal;
      }
      if (s_r_rescan.contains(tx, ty)) {
        status[0] = '\0';
        scanNetworks();
        page = 0;
        need_redraw = true;
        continue;
      }
      if (s_r_page_up.contains(tx, ty) && page > 0) {
        --page;
        need_redraw = true;
        continue;
      }
      if (s_r_page_down.contains(tx, ty) && page + 1 < listPageCount()) {
        ++page;
        need_redraw = true;
        continue;
      }

      for (uint8_t slot = 0; slot < kRowsPerPage; ++slot) {
        if (!s_r_row[slot].contains(tx, ty)) {
          continue;
        }
        const uint16_t entry = static_cast<uint16_t>(page) * kRowsPerPage + slot;
        if (entry >= listEntryCount()) {
          break;
        }

        if (entry >= s_scan_n) {
          // "Other network…": type a hidden/unlisted SSID, then its password.
          ssid_out[0] = '\0';
          const KbResult kb_ssid =
              keyboardModal("Network name", "Hidden networks welcome", false,
                            ssid_out, ssid_len, opts.idle_timeout_ms);
          if (kb_ssid == KbResult::Timeout) {
            return WifiPickResult::Cancelled;
          }
          if (kb_ssid != KbResult::Ok || ssid_out[0] == '\0') {
            need_redraw = true;
            break;
          }
        } else {
          strncpy(ssid_out, s_scan[entry].ssid, ssid_len - 1);
          ssid_out[ssid_len - 1] = '\0';
          if (!s_scan[entry].secured) {
            pass_out[0] = '\0';  // open network: no keyboard needed
            return WifiPickResult::Credentials;
          }
        }

        char prompt[64];
        snprintf(prompt, sizeof(prompt), "for %s", ssid_out);
        pass_out[0] = '\0';
        const KbResult kb =
            keyboardModal("Password", prompt, false, pass_out, pass_len,
                          opts.idle_timeout_ms);
        if (kb == KbResult::Ok) {
          return WifiPickResult::Credentials;
        }
        if (kb == KbResult::Timeout) {
          return WifiPickResult::Cancelled;
        }
        need_redraw = true;  // cancelled: back to the list
        break;
      }
    }

    if (opts.idle_timeout_ms != 0 && millis() - last_activity >= opts.idle_timeout_ms) {
      Serial.println("[wifi] touch setup idle timeout");
      return WifiPickResult::Cancelled;
    }
    delay(5);
  }
}

}  // namespace ui
