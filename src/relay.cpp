#include "relay.h"

// State encoded as 3-bit bitmask CBA  (bit0=A, bit1=B, bit2=C).
// Index = power rank along the safe chain.
static constexpr uint8_t STATE_BITS[4] = {
  0b000,  // OFF      (A=0, B=0, C=0)
  0b001,  // 500 W    (A=1, B=0, C=0)
  0b011,  // 1000 W   (A=1, B=1, C=0)
  0b111,  // 2000 W   (A=1, B=1, C=1)
};

static constexpr uint16_t RELAY_SETTLE_MS = 30;  // spec ≥ 30 ms

// volatile: currentPower may be read from another FreeRTOS task
// (inverter control). 1-byte enum reads are atomic on Xtensa.
static volatile BoilerPower currentPower = BOILER_OFF;
static volatile BoilerPower targetPower  = BOILER_OFF;
static uint32_t lastStepMs = 0;

static void writeBits(uint8_t bits) {
  setRelayBoilerA(bits & 0b001);
  setRelayBoilerB(bits & 0b010);
  setRelayBoilerC(bits & 0b100);
}

void boilerRelayInit() {
  pinMode(RELAY_BOILER_A, OUTPUT);
  pinMode(RELAY_BOILER_B, OUTPUT);
  pinMode(RELAY_BOILER_C, OUTPUT);
  writeBits(STATE_BITS[BOILER_OFF]);
  currentPower = BOILER_OFF;
  targetPower  = BOILER_OFF;
  lastStepMs   = millis();
}

void setBoilerPower(BoilerPower target) {
  targetPower = target;
}

void tickBoiler() {
  if (currentPower == targetPower) return;
  uint32_t now = millis();
  if (now - lastStepMs < RELAY_SETTLE_MS) return;

  // Adjacent STATE_BITS differ in exactly one bit, so each step toggles
  // a single relay along a safe edge of the OFF↔500↔1000↔2000 chain.
  int8_t step = (targetPower > currentPower) ? +1 : -1;
  uint8_t nextRank = (uint8_t)currentPower + step;
  writeBits(STATE_BITS[nextRank]);
  currentPower = (BoilerPower)nextRank;
  lastStepMs = now;
}

BoilerPower getBoilerPower() {
  return currentPower;
}
