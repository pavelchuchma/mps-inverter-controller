#include "phone_charger.h"
#include "phone.h"
#include "relay.h"
#include "utils.h"
#include <string.h>

void tickPhoneCharger() {
  PhoneState s;
  bool valid = phone_get_status(&s);
  uint32_t age_ms = millis() - s.ts_ms;
  bool stale = !valid || age_ms > PHONE_STATUS_STALE_MS;

  bool want_on;
  const char* reason;
  if (stale) {
    want_on = true;
    reason = "stale";
  } else if (strcmp(s.battery_status, "FULL") == 0) {
    want_on = false;
    reason = "FULL";
  } else if (s.battery_percentage <= PHONE_BATT_LOW) {
    want_on = true;
    reason = "low";
  } else if (s.battery_percentage >= PHONE_BATT_HIGH) {
    want_on = false;
    reason = "high";
  } else {
    return;  // inside hysteresis band — keep current state
  }

  bool was_on = isMobileChargerOn();
  if (was_on == want_on) return;

  setMobileCharger(want_on);
  if (stale) {
    printInfo("[CHARGER] %s -> %s (reason=%s, valid=%d age=%us)",
                 was_on ? "ON" : "OFF",
                 want_on ? "ON" : "OFF",
                 reason,
                 (int)valid,
                 (unsigned)(age_ms / 1000));
  } else {
    printInfo("[CHARGER] %s -> %s (reason=%s, %d%% status=%s age=%us)",
                 was_on ? "ON" : "OFF",
                 want_on ? "ON" : "OFF",
                 reason,
                 s.battery_percentage,
                 s.battery_status,
                 (unsigned)(age_ms / 1000));
  }
}
