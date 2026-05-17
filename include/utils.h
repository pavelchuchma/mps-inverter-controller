#pragma once

// Logging helpers. Each call prints "[timestamp] [LEVEL] message" to Serial
// and appends it to /app.log on LittleFS. Timestamp is local wall-clock time
// once NTP is synced; otherwise boot-relative HH:MM:SS.sss.
void printWarning(const char* fmt, ...);
void printInfo(const char* fmt, ...);
