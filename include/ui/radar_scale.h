#pragma once

#include <cstddef>
#include <cstdint>

namespace ui::radar {

/** One zoom band: outer ring = label_km; coverage uses 4/3 × label km for rim fetch. */
struct ScaleBand {
  float label_km;
  float coverage_km;
};

constexpr float kLabelToCoverageKm = 4.0f / 3.0f;
constexpr float kStatuteMileKm = 1.609344f;
constexpr float kNauticalMileKm = 1.852f;
constexpr float kKnotsToKmh = kNauticalMileKm;
constexpr float kKnotsToMph = 1.15077945f;
constexpr float kFeetToMeters = 0.3048f;

/** Allowed outer-ring ranges (statute miles). */
constexpr uint8_t kRangeMileOptions[] = {2, 3, 5, 6, 8, 10, 20, 30};
constexpr size_t kRangeMileOptionCount =
    sizeof(kRangeMileOptions) / sizeof(kRangeMileOptions[0]);

/** Radar swipe zoom ladder: four steps from close-in to wide, each roughly
 *  double the last. Every value is also in kRangeMileOptions, so the gesture and
 *  the settings pages / web form never disagree; those still expose the full list
 *  for fine tuning. With three rings the labels land on 2/3/5, 3/7/10, 7/13/20
 *  and 10/20/30 miles. */
constexpr uint8_t kRangeSwipeStepsMi[] = {5, 10, 20, 30};
constexpr size_t kRangeSwipeStepCount =
    sizeof(kRangeSwipeStepsMi) / sizeof(kRangeSwipeStepsMi[0]);

/** @deprecated Use kRangeMileOptionCount. Kept for label-metrics sizing loops. */
constexpr size_t kScaleBandCount = kRangeMileOptionCount;

enum class DistanceUnit : uint8_t { Km = 0, StatuteMile = 1, NauticalMile = 2 };

void scaleBootLoad();
void scaleIncrease();
void scaleDecrease();
void scaleStep(int8_t delta);
void scaleSelect(uint8_t option_index);
bool scaleSetMiles(uint8_t miles);

/** Move one step along kRangeSwipeStepsMi: delta < 0 zooms in (closer),
 *  delta > 0 zooms out (wider). Clamps at both ends and returns false when
 *  nothing changed, so the caller can skip a full radar repaint. A range set
 *  off-ladder from the web form moves to the next ladder step in that
 *  direction rather than snapping backwards. */
bool scaleSwipeStep(int8_t delta);
bool scaleSaveMilesFromForm(const char* miles_str);
const ScaleBand& scaleActive();
uint8_t scaleActiveIndex();
uint8_t scaleActiveMiles();

/** ADS-B query radius (km) scaled to screen edge for rim targets. */
float adsbQueryRadiusKm();

DistanceUnit distanceUnit();
const char* distanceUnitLabel();
void cycleDistanceUnits();
void saveDistanceUnitsFromForm(const char* unit_value, const char* legacy_miles_checkbox);

bool showCompassRose();
void toggleCompassRose();
void saveCompassRoseFromForm(const char* checkbox_value);

/** Geographic direction at the top of the radar (0=N, 90=E, 180=S, 270=W). */
uint16_t facingDeg();
/** Update RAM facing (snapped to 5°); does not write NVS. */
void applyFacingDeg(uint16_t deg);
/** Step facing by ±5° in RAM (wraps 0–355). */
void facingStep(int8_t delta);
/** Persist current facingDeg() to NVS. */
void persistFacingDeg();
/** Apply + persist (web / direct set). */
void setFacingDeg(uint16_t deg);
void saveFacingDegFromForm(const char* degrees_str);
/** Label for settings UI: "N"/"E"/"S"/"W" or "180°". */
void facingLabel(char* out, size_t out_len);

void formatScaleTag(char* buf, size_t len, float label_km, DistanceUnit unit);
void formatActiveScaleTag(char* buf, size_t len);
void formatAltitudeDisplay(const char* alt_ft_tag, char* out, size_t out_len);
void formatSpeedLabel(char* out, size_t out_len, float gs_knots);

}  // namespace ui::radar
