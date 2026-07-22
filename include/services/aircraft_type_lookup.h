#pragma once

#include <cstddef>

namespace services::aircraft_type {

/** Resolve ICAO type designator (e.g. B738) to full description from flash table. */
bool lookupDescription(const char* icao_type, char* out, size_t out_len);

/** Radar tag: short name if <= 7 chars, otherwise ICAO code (e.g. E75L). */
void formatRadarTagLabel(const char* icao_type, char* out, size_t out_len);

}  // namespace services::aircraft_type
