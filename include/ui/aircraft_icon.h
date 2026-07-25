#pragma once

#include <cstdint>

class PlaneGfx;

namespace services::adsb {
struct Aircraft;
}

namespace ui::aircraft_icon {

/** Resolve Pi-style icon category id for an aircraft (ICAO type + heuristics). */
/** Coarse kind used to color the body. Commercial covers the airline and cargo
 *  fleet, Private is business jets, Propeller covers piston and turboprop, and
 *  Other collects rotorcraft, military, gliders, drones and unknowns (military
 *  traffic is still pulsed by the alert layer when that is enabled). */
enum class ColorGroup : uint8_t {
  CommercialJet = 0,
  PrivateJet,
  Propeller,
  Other,
};

/** Map an icon category (see resolveCategory) to its body-color group. */
ColorGroup colorGroup(uint8_t category);

uint8_t resolveCategory(const services::adsb::Aircraft& aircraft);

/** True when a flash silhouette exists for this category id. */
bool hasIcon(uint8_t category);

/** Draw tinted, heading-rotated silhouette. Returns false → caller should vector-fallback. */
bool draw(PlaneGfx& gfx, int cx, int cy, float heading_deg, uint16_t color, uint8_t category,
          int base_side_px);

}  // namespace ui::aircraft_icon
