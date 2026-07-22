#include "geo/flat_earth.h"

#include <cmath>

namespace geo {

void localOffsetKm(double anchor_lat, double anchor_lon, float target_lat,
                   float target_lon, float* east_km, float* north_km,
                   float* distance_km) {
  const float cos_lat =
      cosf(static_cast<float>(anchor_lat * (3.14159265 / 180.0)));
  const float dx = static_cast<float>(target_lon - anchor_lon) * kKmPerDegreeLatitude *
                 cos_lat;
  const float dy = static_cast<float>(target_lat - anchor_lat) * kKmPerDegreeLatitude;
  if (east_km != nullptr) {
    *east_km = dx;
  }
  if (north_km != nullptr) {
    *north_km = dy;
  }
  if (distance_km != nullptr) {
    *distance_km = sqrtf(dx * dx + dy * dy);
  }
}

int bearingFromOffset(float east_km, float north_km) {
  float deg = atan2f(east_km, north_km) * (180.0f / 3.14159265f);
  if (deg < 0.0f) {
    deg += 360.0f;
  }
  return static_cast<int>(lroundf(deg)) % 360;
}

}  // namespace geo
