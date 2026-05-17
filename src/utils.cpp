#include "utils.h"
#include <Arduino.h>
#include <LittleFS.h>
#include <stdarg.h>
#include <time.h>

// Format boot-relative milliseconds to HH:MM:SS.sss into provided buffer.
static inline void formatBootTimeMs(char* buf, size_t cap, uint32_t ms) {
  uint32_t total_ms = ms;
  uint32_t total_sec = total_ms / 1000;
  unsigned h = total_sec / 3600;
  unsigned m = (total_sec % 3600) / 60;
  unsigned s = total_sec % 60;
  unsigned ms_part = total_ms % 1000;
  if (cap > 0) {
    snprintf(buf, cap, "%02u:%02u:%02u.%03u", h, m, s, ms_part);
  }
}

// Format current local time as YYYY-MM-DD HH:MM:SS, or fall back to boot-relative
// HH:MM:SS.sss if NTP has not synced yet.
static inline void formatLogTimestamp(char* buf, size_t cap) {
  time_t now = time(nullptr);
  if (now >= 24 * 3600) {
    struct tm tinfo;
    localtime_r(&now, &tinfo);
    strftime(buf, cap, "%Y-%m-%d %H:%M:%S", &tinfo);
  } else {
    formatBootTimeMs(buf, cap, millis());
  }
}

// Shared log emitter: formats timestamp + level + message and writes to Serial
// and /app.log on LittleFS.
static void printLog(const char* level, const char* fmt, va_list ap) {
  char tbuf[24];
  formatLogTimestamp(tbuf, sizeof(tbuf));
  char msg[384];
  vsnprintf(msg, sizeof(msg), fmt, ap);
  char out[440];
  snprintf(out, sizeof(out), "[%s] [%s] %s\n", tbuf, level, msg);
  Serial.print(out);
  if (LittleFS.begin()) {
    File f = LittleFS.open("/app.log", "a");
    if (f) {
      f.print(out);
      f.close();
    }
  }
}

void printWarning(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  printLog("WARN", fmt, ap);
  va_end(ap);
}

void printInfo(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  printLog("INFO", fmt, ap);
  va_end(ap);
}
