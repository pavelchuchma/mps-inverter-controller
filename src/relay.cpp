#include "relay.h"

// Relay convention: HIGH = relay not energized (off), LOW = relay energized (on)

static BoilerPower currentPower = BOILER_OFF;
static bool roundRobin500 = false;
static bool roundRobin1000 = false;

// Apply relay outputs. true = relay energized (on), false = relay off.
// COMMON_N carries the highest current (sum of both elements at 2000W),
// so it always switches with zero load:
//   1) disconnect all endpoints (no current flows)
//   2) set COMMON_N
//   3) connect endpoints
static void applyRelays(bool l1, bool l2, bool commonN) {
  setRelayBoilerL1(false);
  setRelayBoilerL2(false);
  setRelayBoilerCommonN(commonN);
  setRelayBoilerL1(l1);
  setRelayBoilerL2(l2);
}

void boilerRelayInit() {
  pinMode(RELAY_BOILER_L1, OUTPUT);
  pinMode(RELAY_BOILER_L2, OUTPUT);
  pinMode(RELAY_BOILER_COMMON_N, OUTPUT);
  setRelayBoilerL1(false);
  setRelayBoilerL2(false);
  setRelayBoilerCommonN(false);
  currentPower = BOILER_OFF;
}

void setBoilerPower(BoilerPower power) {
  switch (power) {
  case BOILER_OFF:
    //  L1=N, L2=N, Common=disconnected
    applyRelays(false, false, false);
    break;

  case BOILER_500W:
    // Two elements in series — alternates current direction
    //  A: L1=L, L2=N, Common=disconnected
    //  B: L1=N, L2=L, Common=disconnected
    roundRobin500 = !roundRobin500;
    if (roundRobin500) {
      applyRelays(true, false, false);
    } else {
      applyRelays(false, true, false);
    }
    break;

  case BOILER_1000W:
    // One element at full voltage — alternates which element
    //  A: L1=L, L2=N, Common=N  → E1 powered
    //  B: L1=N, L2=L, Common=N  → E2 powered
    roundRobin1000 = !roundRobin1000;
    if (roundRobin1000) {
      applyRelays(true, false, true);
    } else {
      applyRelays(false, true, true);
    }
    break;

  case BOILER_2000W:
    // Both elements at full voltage
    //  L1=L, L2=L, Common=N
    applyRelays(true, true, true);
    break;
  }

  currentPower = power;
}

BoilerPower getBoilerPower() {
  return currentPower;
}
