#include "hardware/panel.h"

#include <Arduino.h>

namespace hardware {

void panelBootResolve() {
  Serial.printf("Panel: %s (fixed)\n", panelTypeName());
}

PanelType panelType() { return PanelType::WaveshareJd9365; }

const char* panelTypeName() { return "Waveshare 3.4C / JD9365 / GT911"; }

bool panelUsesCo5300() { return false; }

}  // namespace hardware
