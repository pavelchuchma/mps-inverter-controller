#include "relay.h"
#include "inverter_comm.h"
#include "influx.h"
#include "utils.h"

// Human-readable boiler power labels for logging.
static const char* const POWER_LABELS[4] = { "OFF", "500W", "1000W", "2000W" };

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

// The boiler must heat only from PV surplus, never from the battery (it is
// small and would drain quickly). Any battery discharge above this current [A]
// counts as "discharging".
static constexpr float BOILER_MAX_BATT_DISCHARGE_A = 0.0f;
// Discharge must persist this long before the boiler steps down one level. With
// inverter data refreshed only every INVERTER_POLL_INTERVAL_MS (3 s) this is
// just a few samples, so it is kept generous to give the inverter time to react
// before we intervene.
static constexpr uint32_t BOILER_DISCHARGE_OFF_MS = 10000;

// --- Automatic power regulation thresholds ---
// AC output active power above this [W] forces the boiler off (inverter overload
// guard). If the boiler is wired on the inverter output its own draw counts
// toward this, so OFF (not a single step down) is the safe action.
static constexpr int BOILER_OVERLOAD_W = 5000;
// Overload must persist this long before acting. Inverter data refreshes only
// every INVERTER_POLL_INTERVAL_MS, so wait one refresh period + 1 s to ride out
// a single transient spike between polls.
static constexpr uint32_t BOILER_OVERLOAD_OFF_MS = INVERTER_POLL_INTERVAL_MS + 1000;
// Conditions under which a step up is allowed (PV surplus is plausible).
static constexpr int BOILER_RAISE_MIN_SOC = 70;         // battery SOC [%]
static constexpr float BOILER_RAISE_MIN_PV_V = 350.0f;  // PV input voltage [V]
// Minimum time between successive step-ups during normal operation.
static constexpr uint32_t BOILER_RAISE_INTERVAL_MS = 15UL * 60 * 1000;  // 15 min
// Morning gate: in weak morning sun SOC and PV voltage both read "good" while the
// panels cannot yet carry even 500 W (a near-full small battery also makes charge
// current useless as a surplus signal). So step-ups are blocked until this long
// after PV voltage first rises above BOILER_MORNING_PV_V (~dawn). Tracked via
// millis() from the rising edge — no RTC or sunrise time needed.
static constexpr float BOILER_MORNING_PV_V = 300.0f;    // PV voltage marking dawn [V]
static constexpr uint32_t BOILER_MORNING_DELAY_MS = 2UL * 60 * 60 * 1000;  // 2 h
// Re-arm the morning gate only after PV voltage stays below BOILER_MORNING_PV_V
// for this long (~nightfall). A brief daytime dip (cloud + load spike pulling the
// MPP down) must NOT re-arm it, otherwise the morning delay would restart mid-day.
// millis()-based, so it works without a network clock.
static constexpr uint32_t BOILER_MORNING_REARM_MS = 60UL * 60 * 1000;  // 1 h

// BOILER_ON_PIN reads a clean steady level when the boiler is genuinely on (LOW) or
// off (HIGH), but the high-impedance INPUT_PULLUP picks up brief (sub-10 ms) noise
// glitches that flip a single read. An un-debounced read let one glitch trigger a
// spurious autoRegulate() step-up that the next read immediately reverted. Require the
// raw level to hold continuously for this long before the reported state follows it,
// rejecting glitches in either direction. Negligible vs. the heating timescale; long
// enough to swamp any plausible glitch. See sampleBoilerInput().
static constexpr uint32_t BOILER_INPUT_DEBOUNCE_MS = 200;

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

// Tracks how long the battery has been continuously discharging, so the boiler
// is only stepped down after BOILER_DISCHARGE_OFF_MS of sustained discharge.
static bool battDischarging = false;
static uint32_t dischargeStartMs = 0;

// Tracks how long the AC output has been continuously overloaded, so a brief
// load spike does not force the boiler off (see BOILER_OVERLOAD_OFF_MS).
static bool overloading = false;
static uint32_t overloadStartMs = 0;

// Last time the commanded target changed (up or down). Used to space out the
// automatic step-ups (BOILER_RAISE_INTERVAL_MS).
static uint32_t lastPowerChangeMs = 0;

// Debounced boiler-input state (see BOILER_INPUT_DEBOUNCE_MS / sampleBoilerInput()).
// boilerInputState is the reported (debounced) value; boilerInputLastRaw and
// boilerInputStableSinceMs track how long the raw pin has held its current level.
// boilerInputInit adopts the very first raw sample without waiting for the window.
static bool boilerInputState = false;
static bool boilerInputLastRaw = false;
static uint32_t boilerInputStableSinceMs = 0;
static bool boilerInputInit = false;

// Morning gate state: when PV voltage first crossed BOILER_MORNING_PV_V today
// and whether it is currently above it. pvInit guards the first reading after
// boot (dawn time is then unknown). pvBelowSinceMs debounces re-arming so a brief
// daytime dip does not reset the gate (see BOILER_MORNING_REARM_MS).
static bool pvInit = false;
static bool pvAbove300 = false;
static uint32_t pvCrossed300Ms = 0;
static uint32_t pvBelowSinceMs = 0;  // when PV first dropped below 300 V (0 = above)

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
  // Snapshot the pre-shutdown state before relays and currentPower are zeroed.
  influx_log_event();
  boilerFault = true;
  boilerFaultReason = reason;

  // Spec §"Going to OFF": A=0 first. After A=0, B and C are safe to
  // toggle in any order — A removes L from node X.
  setRelayBoilerA(false);
  delay(RELAY_SETTLE_MS);
  setRelayBoilerB(false);
  delay(RELAY_SETTLE_MS);
  setRelayBoilerC(false);

  BoilerPower prev = currentPower;
  currentPower = BOILER_OFF;
  targetPower  = BOILER_OFF;
  lastStepMs   = millis();

  printWarning("Boiler %s -> OFF (emergency: %s)", POWER_LABELS[prev],
               reason ? reason : "unspecified");
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
  lastPowerChangeMs = millis();
  waitingForBVerify = false;
}

// Set the commanded power target, logging the change (with reason) to the app
// log. The intermediate ramp states (currentPower) are an implementation
// detail and are not logged. No-op when the target is unchanged.
static void setBoilerTarget(BoilerPower target, const char* reason) {
  if (target == targetPower) return;
  printInfo("Boiler target %s -> %s (%s)", POWER_LABELS[targetPower],
            POWER_LABELS[target], reason);
  // Snapshot the state that drove this decision before applying the change.
  influx_log_event();
  targetPower = target;
  lastPowerChangeMs = millis();
}

void setBoilerPower(BoilerPower target) {
  if (boilerFault) return;
  setBoilerTarget(target, "Web UI");
}

// Automatic power regulation from inverter state. Only called while the inverter
// data is valid and the boiler is in normal operation (no fault, not mid-B-verify).
// Snapshots the inverter data once, updates the morning gate and applies the
// rules in priority order: AC overload (OFF) > battery discharge (one step down)
// > PV surplus (one step up).
static void autoRegulate(uint32_t now) {
  InverterState s;
  inverter_get_status(&s);

  // Morning gate: block step-ups until BOILER_MORNING_DELAY_MS after PV voltage
  // first rises above BOILER_MORNING_PV_V (~dawn). The gate is re-armed only after
  // PV stays below that threshold for BOILER_MORNING_REARM_MS (~nightfall), so a
  // brief daytime dip does not restart the delay mid-day.
  bool pvUp = s.pv_input_voltage > BOILER_MORNING_PV_V;
  if (!pvInit) {
    // First valid reading after boot. Dawn time is unknown, so if PV is already
    // up assume the morning warm-up has passed and open the gate (rule 2 still
    // protects the battery). Modular subtraction is correct even for small now.
    pvInit = true;
    pvAbove300 = pvUp;
    pvCrossed300Ms = pvUp ? (now - BOILER_MORNING_DELAY_MS) : now;
    pvBelowSinceMs = 0;
  } else if (pvUp) {
    if (!pvAbove300) {
      pvAbove300 = true;
      pvCrossed300Ms = now;  // dawn rising edge
    }
    pvBelowSinceMs = 0;  // sun is back — cancel any pending re-arm
  } else {  // PV below 300 V
    if (pvAbove300) {
      if (pvBelowSinceMs == 0) pvBelowSinceMs = now;
      if (now - pvBelowSinceMs >= BOILER_MORNING_REARM_MS) {
        pvAbove300 = false;  // sustained low (~nightfall) — re-arm for next day
      }
    }
  }
  bool morningPassed = pvAbove300 &&
                       (now - pvCrossed300Ms >= BOILER_MORNING_DELAY_MS);

  // Rule 1: sustained AC output overload -> force OFF.
  bool overload = s.ac_active_w > BOILER_OVERLOAD_W;
  if (overload && !overloading) {
    overloading = true;
    overloadStartMs = now;
  } else if (!overload) {
    overloading = false;
  }
  bool sustainedOverload = overloading &&
                           (now - overloadStartMs >= BOILER_OVERLOAD_OFF_MS);

  // Rule 2: sustained battery discharge -> step down one level. A brief spike
  // must not trip it, so require BOILER_DISCHARGE_OFF_MS of continuous discharge.
  bool discharging = s.batt_discharge_current > BOILER_MAX_BATT_DISCHARGE_A;
  if (discharging && !battDischarging) {
    battDischarging = true;
    dischargeStartMs = now;
  } else if (!discharging) {
    battDischarging = false;
  }
  bool sustainedDischarge = battDischarging &&
                            (now - dischargeStartMs >= BOILER_DISCHARGE_OFF_MS);

  if (sustainedOverload) {
    setBoilerTarget(BOILER_OFF, "AC overload");
  } else if (sustainedDischarge && targetPower > BOILER_OFF) {
    setBoilerTarget((BoilerPower)(targetPower - 1), "battery discharge");
    dischargeStartMs = now;  // restart timer so the next step-down waits again
  } else if (targetPower < BOILER_2000W && morningPassed &&
             s.batt_soc > BOILER_RAISE_MIN_SOC &&
             s.pv_input_voltage > BOILER_RAISE_MIN_PV_V &&
             (now - lastPowerChangeMs >= BOILER_RAISE_INTERVAL_MS)) {
    setBoilerTarget((BoilerPower)(targetPower + 1), "PV surplus");
  }
}

// Sample the raw BOILER_ON_PIN and update the debounced boilerInputState. Called
// once per loop from tickBoiler(), so the cache stays fresh for all isBoilerOn()
// callers (which run in the same loop thread).
static void sampleBoilerInput(uint32_t now) {
  bool raw = (digitalRead(BOILER_ON_PIN) == LOW);  // LOW = on
  if (raw != boilerInputLastRaw) {
    boilerInputLastRaw = raw;
    boilerInputStableSinceMs = now;  // raw just changed — restart the stability timer
  }
  if (!boilerInputInit) {
    boilerInputState = raw;  // adopt the first sample after boot immediately
    boilerInputInit = true;
  } else if (raw != boilerInputState &&
             (now - boilerInputStableSinceMs) >= BOILER_INPUT_DEBOUNCE_MS) {
    boilerInputState = raw;  // raw held its new level long enough — commit it
  }
}

bool isBoilerOn() {
  return boilerInputState;
}

void tickBoiler() {
  if (boilerFault) return;

  uint32_t now = millis();
  sampleBoilerInput(now);

  // Mains absent at A.COM (no heating possible, no AC for the opto to detect) or
  // inverter data stale/lost (comms down for several consecutive polls) — force
  // the chain back to OFF. Automatic regulation runs only when neither holds.
  const char* forceOffReason = nullptr;
  if (!isBoilerOn()) forceOffReason = "boiler input off";
  else if (!inverter_data_valid()) forceOffReason = "inverter data invalid";

  if (forceOffReason) {
    // Cancel any in-flight B-verify wait and steer the chain back to OFF
    // (falls through to the settle/step block). Reset the auto trackers so we
    // do not act on stale timing when normal operation resumes.
    waitingForBVerify = false;
    setBoilerTarget(BOILER_OFF, forceOffReason);
    overloading = false;
    battDischarging = false;
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
  } else {
    // Normal operation: adjust the target from inverter state.
    autoRegulate(now);
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
