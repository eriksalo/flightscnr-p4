#pragma once

#include <cstdint>

namespace ui::radar {

// Native 720×720 design: fine instrument-style strokes and dense type, sized
// for the 3.4" glass (the legacy 390-space values rendered ~1.85× larger).
constexpr int kSize = 720;
constexpr int kCenterX = kSize / 2;
constexpr int kCenterY = kSize / 2;

/** Outermost grid ring (inside edge labels). */
constexpr int kGridOuterRadius = 324;

/** N: pixels inset from top edge (TopCenter anchor). */
constexpr int kCardinalNorthOffsetY = 14;
/** S: pixels inset from bottom edge (BottomCenter anchor). */
constexpr int kCardinalSouthOffsetY = 14;
/** Intercardinal (NE/SE/SW/NW) labels sit on the outer grid ring minus this inset. */
constexpr int kCardinalDiagonalInsetPx = 10;

/** Radial inset from each ring for scale labels (px; positive = toward center). */
constexpr int kScaleGapFromOuterRing = 18;
/** Outermost ring only, when distance units are km. */
constexpr int kScaleGapOuterRingKm = 30;
/** Range ring labels: bearing from north, clockwise (between W and SW on display). */
constexpr float kScaleLabelBearingDeg = 245.5f;

/** Target cap height (px) for N/S/E/W. */
constexpr int kCardinalLabelHeightPx = 26;
/** Scale label is this many px shorter than cardinals. */
constexpr int kScaleBelowCardinalPx = 7;

constexpr int kRingCount = 3;

/** Shared grid stroke: hairline (single-pixel) rings and spokes. */
constexpr float kGridStrokeHalfWidth = 0.6f;
/** Dashed grid: dash and gap length along lines / arcs (px). */
constexpr int kGridDashLenPx = 8;
constexpr int kGridDashGapPx = 14;

/** Top-down aircraft icon half-extent (see ui/aircraft_symbol). */
constexpr int kAircraftIconRadiusPx = 20;
/** Gap from icon edge to tag block (px). */
constexpr int kAircraftLabelGapPx = 6;
/** Keep icon inside outer ring by at least this inset (px). */
constexpr int kAircraftInsideRingInsetPx = kAircraftIconRadiusPx + 3;

/** Beyond-ring traffic: compact aircraft icons on screen rim. */
constexpr int kBeyondRingScreenMarginPx = 5;
/** Aircraft tags use displayFontTag() directly (4pt, 10px line) rather than the
 *  dynamic height picker — at this size the picker ladder has no rung to hit.
 *  Every aircraft on the scope is tagged: the old nearest-N declutter meant
 *  labels only ever appeared in the middle of the display, and 4pt type is
 *  roughly a third the width of the 10pt it replaced, so they now fit. */

/** Radar sweep: one full rotation period (ms). */
constexpr unsigned long kSweepPeriodMs = 6000;
/** Target animation frame interval (ms); partial spoke blits are ~15–25ms. */
constexpr unsigned long kSweepFrameMs = 33;
/** Loop gap (ms) after which the sweep pauses instead of catching up to wall clock. */
constexpr unsigned long kSweepGapPauseMs = 80;
/** Max simulated dt (ms) per painted frame (~2.9 deg at kSweepPeriodMs). */
constexpr unsigned long kSweepMaxStepMs = 48;
/** Spoke erase uses incremental union only when angle moved less than this (deg). */
constexpr float kSweepIncrementalMaxDeg = 10.0f;
/** Sweep line from center to this radius (px). */
constexpr int kSweepRadiusPx = kCenterX - kBeyondRingScreenMarginPx;
/** drawWideLine half-width for the sweep spoke (~2 px total). */
constexpr float kSweepLineHalfWidth = 0.9f;
/** Trailing sweep lines (1 = leading spoke only, no trail). */
constexpr int kSweepTrailLines = 1;
constexpr float kSweepTrailSpanDeg = 12.0f;
/** PPI behaviour: aircraft blips update only as the sweep spoke crosses their
 *  bearing, instead of the whole screen jumping on each ADS-B poll. Set false to
 *  go back to screen-wide updates (also the fallback when the sweep line is off
 *  or the offscreen content sprite could not be allocated). */
constexpr bool kSweepPaintsAircraft = true;

/** Chroma key for sweep sprite (must not appear in grid or sweep colors). */
constexpr uint16_t kSweepTransparentColor = 0x0001;

/** RGB565 palette targets (applied in initPalette). */
constexpr uint8_t kBgR = 2;
constexpr uint8_t kBgG = 15;
constexpr uint8_t kBgB = 3;
constexpr uint8_t kGridR = 16;
constexpr uint8_t kGridG = 100;
constexpr uint8_t kGridB = 32;
constexpr uint8_t kSweepR = 48;
constexpr uint8_t kSweepG = 255;
constexpr uint8_t kSweepB = 96;
constexpr uint8_t kSweepTrailR = 12;
constexpr uint8_t kSweepTrailG = 72;
constexpr uint8_t kSweepTrailB = 28;
/** Aircraft body colors by kind (see ui::aircraft_icon::ColorGroup). Amber is the
 *  commercial fleet — the bulk of the traffic — so the scope keeps its familiar
 *  look; the other groups are picked to stay clear of the green grid and of the
 *  orange/red alert pulses. */
constexpr uint8_t kAircraftR = 255;  // commercial jets: amber
constexpr uint8_t kAircraftG = 180;
constexpr uint8_t kAircraftB = 40;
constexpr uint8_t kAircraftPrivateR = 70;  // private / business jets: cyan
constexpr uint8_t kAircraftPrivateG = 225;
constexpr uint8_t kAircraftPrivateB = 255;
constexpr uint8_t kAircraftPropR = 185;  // propeller and turboprop: violet
constexpr uint8_t kAircraftPropG = 140;
constexpr uint8_t kAircraftPropB = 255;
constexpr uint8_t kAircraftOtherR = 200;  // rotor, military, glider, drone, unknown
constexpr uint8_t kAircraftOtherG = 210;
constexpr uint8_t kAircraftOtherB = 220;
constexpr uint8_t kTagTypeR = 255;
constexpr uint8_t kTagTypeG = 200;
constexpr uint8_t kTagTypeB = 0;
/** Altitude tag: ascending or level (cyan). */
constexpr uint8_t kTagAltAscendR = 0;
constexpr uint8_t kTagAltAscendG = 255;
constexpr uint8_t kTagAltAscendB = 255;
/** Altitude tag: descending (magenta). */
constexpr uint8_t kTagAltDescendR = 255;
constexpr uint8_t kTagAltDescendG = 0;
constexpr uint8_t kTagAltDescendB = 255;

extern uint16_t kColorBackground;
extern uint16_t kColorGrid;
extern uint16_t kColorSweep;
extern uint16_t kColorSweepTrail;
extern uint16_t kColorLabel;
extern uint16_t kColorAircraft;         // commercial jets
extern uint16_t kColorAircraftPrivate;  // business jets
extern uint16_t kColorAircraftProp;     // piston / turboprop
extern uint16_t kColorAircraftOther;    // rotor, military, glider, drone, unknown
extern uint16_t kColorTagType;
extern uint16_t kColorTagAltitudeAscend;
extern uint16_t kColorTagAltitudeDescend;
extern uint16_t kColorAlertMilitary;
extern uint16_t kColorAlertEmergency;

}  // namespace ui::radar
