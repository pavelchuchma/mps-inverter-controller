#pragma once

#include <Arduino.h>

// Logging helpers. Each call prints "[timestamp] [LEVEL] message" to Serial
// and appends it to /app.log on LittleFS. Timestamp is local wall-clock time
// once NTP is synced; otherwise boot-relative HH:MM:SS.sss.
void printWarning(const char* fmt, ...);
void printInfo(const char* fmt, ...);

// Log a WARN header line followed by an arbitrary-length raw body in a single
// file append (no fixed-size formatting buffer, so the body is never truncated).
// Intended for dumping whole protocol frames for diagnostics.
void printWarningBlock(const char* header, const String& body);
