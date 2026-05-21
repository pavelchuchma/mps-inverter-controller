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
static constexpr uint16_t B_VERIFY_TIMEOUT_MS = 1000;  // opto + RC filter can be slow

// volatile: currentPower may be read from another FreeRTOS task
// (inverter control). 1-byte enum reads are atomic on Xtensa.
static volatile BoilerPower currentPower = BOILER_OFF;
static volatile BoilerPower targetPower = BOILER_OFF;
static uint32_t lastStepMs = 0;

// Blocks all stepping (especially the 1000→2000 C-toggle) until the
// opto-isolated verifier confirms B physically closed. Without this gate
// a slow opto filter could let C move while B is still mechanically open
// → L–N short through C's transition arc.
static bool waitingForBVerify = false;
static uint32_t bVerifyStartMs = 0;

// Sticky fault state. Once set, only a reboot clears it.
static volatile bool boilerFault = false;
static const char* boilerFaultReason = nullptr;  // static-string only

static inline bool isCommandedBHigh(BoilerPower p) {
  return STATE_BITS[p] & 0b010;
}

static void writeBits(uint8_t bits) {
  setRelayBoilerA(bits & 0b001);
  setRelayBoilerB(bits & 0b010);
  setRelayBoilerC(bits & 0b100);
}

static void emergencyShutdown(const char* reason) {
  if (boilerFault) return;            // idempotent
  boilerFault = true;
  boilerFaultReason = reason;

  // Spec §"Going to OFF": A=0 first. After A=0, B and C are safe to
  // toggle in any order — A removes L from node X.
  setRelayBoilerA(false);
  delay(RELAY_SETTLE_MS);
  setRelayBoilerB(false);
  delay(RELAY_SETTLE_MS);
  setRelayBoilerC(false);

  currentPower = BOILER_OFF;
  targetPower  = BOILER_OFF;
  lastStepMs   = millis();

  Serial.printf("[BOILER FAULT] emergency shutdown: %s\n",
                reason ? reason : "(unspecified)");
}

void boilerRelayInit() {
  pinMode(RELAY_BOILER_A, OUTPUT);
  pinMode(RELAY_BOILER_B, OUTPUT);
  pinMode(RELAY_BOILER_C, OUTPUT);
  pinMode(RELAY_BOILER_B_VERIFY_PIN, INPUT);  // GPIO36, no internal pull-up
  writeBits(STATE_BITS[BOILER_OFF]);
  currentPower = BOILER_OFF;
  targetPower = BOILER_OFF;
  lastStepMs = millis();
  waitingForBVerify = false;
}

void setBoilerPower(BoilerPower target) {
  if (boilerFault) return;
  targetPower = target;
}

void tickBoiler() {
  if (boilerFault) return;

  uint32_t now = millis();

  if (!isBoilerOn()) {
    // Mains absent at A.COM: no heating possible, no AC for the opto
    // to detect. Cancel any in-flight B-verify wait and steer the
    // chain back to OFF (falls through to the settle/step block).
    waitingForBVerify = false;
    targetPower = BOILER_OFF;
  } else if (waitingForBVerify) {
    // Block all stepping (especially the 1000→2000 C-toggle) until the
    // opto confirms B physically closed. Fault if it doesn't within 1s.
    if (isRelayBoilerBVerifiedOn()) {
      waitingForBVerify = false;
    } else if ((now - bVerifyStartMs) >= B_VERIFY_TIMEOUT_MS) {
      emergencyShutdown("Relay B did not verify within 1s after closing");
    }
    return;
  } else if (isCommandedBHigh(currentPower) && !isRelayBoilerBVerifiedOn()) {
    // Steady-state monitoring: B was previously verified ON (the wait
    // phase above only exits on a HIGH read) and has now dropped.
    emergencyShutdown("Relay B mismatch: commanded ON, sensor reads OFF");
    return;
  }

  if (now - lastStepMs < RELAY_SETTLE_MS) return;
  if (currentPower == targetPower) return;

  // Adjacent STATE_BITS differ in exactly one bit, so each step toggles
  // a single relay along a safe edge of the OFF↔500↔1000↔2000 chain.
  int8_t step = (targetPower > currentPower) ? +1 : -1;
  uint8_t nextRank = (uint8_t)currentPower + step;

  // Detect "this step closes B" (only the 500W→1000W edge on the chain).
  bool willCloseB = !isCommandedBHigh(currentPower) &&
                    (STATE_BITS[nextRank] & 0b010);

  writeBits(STATE_BITS[nextRank]);
  currentPower = (BoilerPower)nextRank;
  lastStepMs = now;

  if (willCloseB) {
    waitingForBVerify = true;
    bVerifyStartMs = now;
  }
}

BoilerPower getBoilerPower() {
  return currentPower;
}

bool isBoilerFault() {
  return boilerFault;
}

const char* getBoilerFaultReason() {
  return boilerFaultReason;
}
