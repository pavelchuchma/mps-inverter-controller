#pragma once
#include <Arduino.h>
#include "config.h"

// --- Boiler element power control ---
// Two 1 kW resistive elements (R1, R2 @ 230 V AC) and three SPDT relays
// (A, B, C). Convention: state = 1 = relay energized = COM↔NO,
//                       state = 0 = relay de-energized = COM↔NC.
//
// Wiring (see doc/heater_relay_control_spec.md and doc/boiler_relay_schema.png):
//   L → A.COM,  N → B.COM,  C.COM → R1.top
//   R1.bottom = R2.top  (node Y);  B.NO → Y
//   C.NO = A.NO = R2.bottom (node X)
//   C.NC = B.NC (node Z);  A.NC unused
//
// Canonical states (A, B, C):
//   OFF      (0, 0, 0)  – 0 W
//   500 W    (1, 0, 0)  – R1 + R2 in series
//   1000 W   (1, 1, 0)  – only R2
//   2000 W   (1, 1, 1)  – R1 || R2
//
// Safety: toggling C while (B==0 && A==1) shorts L–N through C's
// transition arc. The state machine therefore moves only along the
// linear chain OFF ↔ 500 W ↔ 1000 W ↔ 2000 W, one relay per step,
// with a settling delay between steps.
//
// API is non-blocking: setBoilerPower() only stores the target, and
// tickBoiler() — called from the main loop — advances by exactly one
// rank per tick, no faster than RELAY_SETTLE_MS apart. getBoilerPower()
// returns the actually-achieved state (used by the inverter control).
//
// Physical B verification: an opto-isolated AC voltage detector wired
// across B's NO contact (between node Y and N) reports whether B has
// physically energized. tickBoiler() consults it on every settled tick
// where the *current* state has commanded B=1 (1000 W or 2000 W). On a
// mismatch (commanded B=1 but sensor reads "hot" = N is at Z, not Y) that
// persists past a short debounce window (a brief glitch on the high-impedance
// GPIO36 opto is ignored), the controller calls emergencyShutdown(): drives
// A=0, B=0, C=0 in that order, sets a sticky boilerFault flag. Once faulted,
// setBoilerPower() and tickBoiler() are no-ops until reboot.

enum BoilerPower : uint8_t {
  BOILER_OFF    = 0,
  BOILER_500W   = 1,
  BOILER_1000W  = 2,
  BOILER_2000W  = 3,
};

// Maps a boiler power rank to its electrical power in watts.
inline int boilerPowerToWatts(BoilerPower power) {
  switch (power) {
  case BOILER_500W:  return 500;
  case BOILER_1000W: return 1000;
  case BOILER_2000W: return 2000;
  default:           return 0;
  }
}

// Individual heating relay setters (on = relay energized = COM↔NO).
inline void setRelayBoilerA(bool on) {
  digitalWrite(RELAY_BOILER_A, on ? HIGH : LOW);
}
inline bool isRelayBoilerAOn() {
  return digitalRead(RELAY_BOILER_A) == HIGH;
}

inline void setRelayBoilerB(bool on) {
  digitalWrite(RELAY_BOILER_B, on ? HIGH : LOW);
}
inline bool isRelayBoilerBOn() {
  return digitalRead(RELAY_BOILER_B) == HIGH;
}

inline void setRelayBoilerC(bool on) {
  digitalWrite(RELAY_BOILER_C, on ? HIGH : LOW);
}
inline bool isRelayBoilerCOn() {
  return digitalRead(RELAY_BOILER_C) == HIGH;
}

// Reads the opto-isolated AC voltage detector across relay RelayB.NO contact.
// HIGH = "cold" (Y at N via B.NO, B is physically energized).
// LOW  = "hot"  (Y not at N — B failed to close to NO).
inline bool isRelayBoilerBVerifiedOn() {
  return digitalRead(RELAY_BOILER_B_VERIFY_PIN) == HIGH;
}

void boilerRelayInit();
void setBoilerPower(BoilerPower power);
void tickBoiler();
BoilerPower getBoilerPower();

// Sticky fault state. Set by emergencyShutdown() when relay B's commanded
// state disagrees with the verifier sensor. Cleared only by reboot.
// While set, setBoilerPower() and tickBoiler() are no-ops.
bool isBoilerFault();
const char* getBoilerFaultReason();   // nullptr if no fault

// Debounced BOILER_ON_PIN state — true when boiler is reported as on. The raw pin
// picks up brief noise glitches, so the value is debounced in relay.cpp
// (sampleBoilerInput) rather than read directly here.
bool isBoilerOn();

// --- Mobile charger relay ---
// HIGH = on (charging enabled), LOW = off
inline void setMobileCharger(bool on) {
  digitalWrite(RELAY_MOBILE_CHARGER, on ? HIGH : LOW);
}

inline bool isMobileChargerOn() {
  return digitalRead(RELAY_MOBILE_CHARGER) == HIGH;
}
