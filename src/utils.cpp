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

// Rotate /app.log when it grows past LOG_MAX_BYTES by dropping the oldest
// LOG_DROP_BYTES (rounded up to the next newline so the surviving log starts
// on a clean line). With a typical ~2 KB/h rate this fires once every few
// weeks. The copy uses a small stack buffer to avoid large heap allocations.
static const size_t LOG_MAX_BYTES  = 100UL * 1024UL;
static const size_t LOG_DROP_BYTES = 50UL * 1024UL;

static void rotateAppLog() {
  File src = LittleFS.open("/app.log", "r");
  if (!src) return;

  src.seek(LOG_DROP_BYTES, SeekSet);
  // Advance to the next newline so we don't keep a mid-line fragment as the
  // new first entry. Scan at most ~1 KB; if no '\n' is found, fall through
  // and start copying from current position.
  for (int i = 0; i < 1024 && src.available(); ++i) {
    int c = src.read();
    if (c == '\n' || c < 0) break;
  }

  File dst = LittleFS.open("/app.log.tmp", "w");
  if (!dst) { src.close(); return; }

  uint8_t buf[512];
  while (true) {
    int n = src.read(buf, sizeof(buf));
    if (n <= 0) break;
    dst.write(buf, (size_t)n);
  }
  src.close();
  dst.close();

  LittleFS.remove("/app.log");
  LittleFS.rename("/app.log.tmp", "/app.log");
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
      size_t after = f.size();
      f.close();
      if (after > LOG_MAX_BYTES) {
        rotateAppLog();
      }
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
