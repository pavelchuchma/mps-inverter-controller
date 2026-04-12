#pragma once
#include <Arduino.h>
#include "config.h"

// Mobile charger relay: HIGH = on (charging enabled), LOW = off
inline void setMobileCharger(bool on) {
  digitalWrite(RELAY_MOBILE_CHARGER, on ? HIGH : LOW);
}

inline bool isMobileChargerOn() {
  return digitalRead(RELAY_MOBILE_CHARGER) == HIGH;
}
