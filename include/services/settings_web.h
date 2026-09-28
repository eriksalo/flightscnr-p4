#pragma once

#include <WString.h>

/** Start HTTP settings UI on port 80 (STA connected). No-op if already running. */
void settingsWebStart();

/** Stop HTTP server (e.g. before captive portal). */
void settingsWebStop();

/** Call from loop while on home Wi‑Fi. */
void settingsWebPoll();

bool settingsWebActive();

/** Debug screen change queued via GET /nav?s=<screen>; empty when none pending. */
String settingsWebTakeNavRequest();
