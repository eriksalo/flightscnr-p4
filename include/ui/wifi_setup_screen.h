#pragma once

#include <cstddef>
#include <cstdint>

namespace ui {

/** Outcome of the on-device Wi-Fi picker (scan list + on-screen keyboard). */
enum class WifiPickResult : uint8_t {
  Cancelled,    // Cancel tapped or idle timeout — nothing chosen
  Credentials,  // ssid_out/pass_out filled; caller saves and connects
  WebPortal,    // user asked for the phone-browser fallback portal
};

struct WifiPickOptions {
  /** Banner over the list (e.g. "Could not join <ssid>"). */
  const char* status = nullptr;
  /** Reopen directly on this SSID's password keyboard (connect retry). */
  const char* retry_ssid = nullptr;
  /** Password prefill for the retry keyboard so a typo is a one-key fix. */
  const char* retry_pass = nullptr;
  /** Boot has nothing to cancel back to: Cancel becomes "Retry saved". */
  bool boot_mode = false;
  /** Auto-cancel after this long without a touch (0 = wait forever). */
  unsigned long idle_timeout_ms = 0;
};

/**
 * Blocking modal, touch-only: scan for networks, tap one, type the password
 * on an on-screen keyboard. Polls input itself (call from a context where
 * blocking the loop is OK, like the Wi-Fi setup flows). "Other network…"
 * lets a hidden SSID be typed in.
 */
WifiPickResult wifiSetupScreenPick(const WifiPickOptions& opts, char* ssid_out,
                                   size_t ssid_len, char* pass_out,
                                   size_t pass_len);

}  // namespace ui
