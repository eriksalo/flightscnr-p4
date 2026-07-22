#pragma once

#include <cstdint>

namespace ui {

void clockSettingsScreenDraw();
void clockSettingsHandleKnob(int8_t delta);
void clockSettingsCycleFocus();
void clockSettingsResetFocus();

}  // namespace ui
