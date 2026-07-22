#pragma once

namespace services::map_center {

/** Load stored coordinates or factory defaults. Call once before WiFi setup. */
void bootLoad();

double latitude();
double longitude();

/** Parse portal strings, validate, persist. */
bool applyPortalCoordinates(const char* lat_str, const char* lon_str);

/** Parse "latitude, longitude" (e.g. 37.636422, -122.365968), validate, persist. */
bool applyRadarCenterFromForm(const char* center_str);

}  // namespace services::map_center
