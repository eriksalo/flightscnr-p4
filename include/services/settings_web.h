#pragma once

/** Start HTTP settings UI on port 80 (STA connected). No-op if already running. */
void settingsWebStart();

/** Stop HTTP server (e.g. before captive portal). */
void settingsWebStop();

/** Call from loop while on home Wi‑Fi. */
void settingsWebPoll();

bool settingsWebActive();
