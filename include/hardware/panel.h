#pragma once

#include <cstdint>

namespace hardware {

/** Display + touch hardware variant. The Waveshare 3.4C has exactly one
 *  panel (JD9365 DSI + GT911), so this exists only to keep the call sites
 *  from the multi-panel T-Encoder Pro codebase compiling. */
enum class PanelType : uint8_t {
  /** Waveshare ESP32-P4-WIFI6-Touch-LCD-3.4C — JD9365 800×800 DSI + GT911. */
  WaveshareJd9365 = 3,
};

/** Resolve panel type before displayInit() / inputInit(). Fixed on this board. */
void panelBootResolve();

PanelType panelType();
const char* panelTypeName();
/** CO5300 AMOLED quirk paths (T-Encoder Pro only) — always false here. */
bool panelUsesCo5300();

}  // namespace hardware
