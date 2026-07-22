#pragma once

#include "hardware/plane_gfx.h"

extern PlaneGfx tft;

void displayInit();
void planeGfxPanelLockInit();

/** Put the panel into sleep mode (display off, low power). */
void displaySleep();
/** Wake the panel from sleep mode. */
void displayWake();

/** Raw DSI framebuffer (RGB565, physical resolution) — for /screenshot. */
uint16_t* displayFramebuffer();
int displayFbWidth();
int displayFbHeight();
