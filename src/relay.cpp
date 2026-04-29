#include "relay.h"

// State encoded as 3-bit bitmask CBA  (bit0=A, bit1=B, bit2=C).
// Index = power rank along the safe chain.
static constexpr uint8_t STATE_BITS[4] = {
  0b000,  // OFF      (A=0, B=0, C=0)
  0b100,  // 500 W    (A=0, B=0, C=1)
  0b110,  // 1000 W   (A=0, B=1, C=1)
  0b111,  // 2000 W   (A=1, B=1, C=1)
};

static constexpr uint16_t RELAY_SETTLE_MS = 30;  // spec ≥ 30 ms

static BoilerPower currentPower = BOILER_OFF;

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
}

void setBoilerPower(BoilerPower target) {
  if (target == currentPower) return;

  // Walk the chain one rank at a time. Adjacent STATE_BITS differ in
  // exactly one bit, so each step toggles a single relay along a
  // safe edge (OFF↔500, 500↔1000, 1000↔2000).
  int8_t step = (target > currentPower) ? +1 : -1;
  for (int8_t r = (int8_t)currentPower; r != (int8_t)target; r += step) {
    writeBits(STATE_BITS[r + step]);
    delay(RELAY_SETTLE_MS);
  }
  currentPower = target;
}

BoilerPower getBoilerPower() {
  return currentPower;
}
