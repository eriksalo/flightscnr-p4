#pragma once

namespace geo {

/** Approximate km per degree of latitude (WGS84 flat-earth). */
constexpr float kKmPerDegreeLatitude = 111.0f;

/** East/north offset in km from an anchor; optional great-circle distance. */
void localOffsetKm(double anchor_lat, double anchor_lon, float target_lat,
                   float target_lon, float* east_km, float* north_km,
                   float* distance_km);

/** Compass bearing 0–359° from east/north offsets (0° = north). */
int bearingFromOffset(float east_km, float north_km);

}  // namespace geo
