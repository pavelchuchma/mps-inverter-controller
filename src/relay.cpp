#include "relay.h"
#include <Preferences.h>
#include "inverter_comm.h"
#include "pylontech_comm.h"
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
static constexpr uint16_t RELAY_B_VERIFY_TIMEOUT_MS = 1000;  // opto + RC filter can be slow

// NVS home of the persisted mode flag (see setBoilerManual()) and the target
// temperature (see setBoilerTargetTemp()). Two plain values, no struct.
static const char* const NVS_NAMESPACE = "boiler";
static const char* const NVS_KEY_MANUAL = "manual";
static const char* const NVS_KEY_TARGET = "tgt";

// The boiler heats primarily from PV surplus. Discharge is read directly from the
// battery (Pylontech signed current, + charge / - discharge). Current below the
// (SoC-dependent) limit counts as "discharging" and steps the boiler down. The limit
// also doubles as a deadband around idle so noise near zero does not trip a step-down.
// When the battery is well charged (SoC > BOILER_DISCHARGE_HIGH_SOC) there is enough
// reserve to let the boiler use it as a buffer, so a larger discharge is tolerated and
// the boiler holds a higher stage through brief PV shortfalls. At lower SoC stay
// conservative so the boiler does not drag the battery down.
static constexpr float BOILER_DISCHARGE_A = 2.0f;       // base limit (lower SoC)
static constexpr float BOILER_DISCHARGE_HIGH_A = 7.0f;  // limit when well charged
static constexpr int BOILER_DISCHARGE_HIGH_SOC = 75;    // SoC [%] above which HIGH applies
// Discharge must persist this long before the boiler steps down one level. A brief
// spike must not trip it, so require this much continuous discharge.
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
// PV input voltage marking plausible surplus [V]. Kept below the no-load MPP so
// the boiler can still bootstrap (OFF->500W) on hot days, when a high cell
// temperature depresses panel voltage into the ~330 V range even in good sun.
static constexpr float BOILER_RAISE_MIN_PV_V = 325.0f;
// Minimum time between successive step-ups during normal operation.
static constexpr uint32_t BOILER_RAISE_INTERVAL_MS = 15UL * 60 * 1000;  // 15 min
// Morning gate: in weak morning sun SOC and PV voltage both read "good" while the
// panels cannot yet carry even 500 W (a near-full small battery also makes charge
// current useless as a surplus signal). So step-ups are blocked until this long
// after PV voltage first rises above BOILER_MORNING_PV_V (~dawn). Tracked via
// millis() from the rising edge — no RTC or sunrise time needed.
// Kept short: while the battery is taking its morning charge it holds the MPP (and
// thus PV voltage) down until it is nearly full, after which the inverter floats and
// throttles the panels. A long delay would expire only after that throttling has
// already begun, missing the surplus window. The discharge rule guards the battery
// regardless, so a short gate is safe.
static constexpr float BOILER_MORNING_PV_V = 300.0f;    // PV voltage marking dawn [V]
static constexpr uint32_t BOILER_MORNING_DELAY_MS = 30UL * 60 * 1000;  // 30 min
// Re-arm the morning gate only after PV voltage stays below BOILER_MORNING_PV_V
// for this long (~nightfall). A brief daytime dip (cloud + load spike pulling the
// MPP down) must NOT re-arm it, otherwise the morning delay would restart mid-day.
// millis()-based, so it works without a network clock.
static constexpr uint32_t BOILER_MORNING_REARM_MS = 60UL * 60 * 1000;  // 1 h

// --- Battery-driven regulation (Pylontech) ---
// All battery signals come directly from the Pylontech console; the inverter's
// battery fields are no longer used for boiler control. Two SoC bands:
//   < BOILER_RAISE_MIN_SOC -> boiler OFF (let the battery recharge)
//   >= BOILER_RAISE_MIN_SOC -> step up via either path; the discharge rule caps it:
//     A) charge surplus  - the battery is accepting charge, so charge power is a
//        direct surplus signal (fast, margin-limited).
//     B) throttled surplus - near full the inverter floats and throttles the MPPT,
//        so charge power reads ~0 even in full sun; treat high PV voltage + no
//        discharge as surplus and probe upward (slow, discharge rule backs off).
// On the charge-surplus path a step up is allowed only when the power flowing into
// the battery exceeds the extra load the next stage adds, times this margin. The
// 20 % reserve keeps the stage from immediately causing discharge and oscillating.
static constexpr float BOILER_STEP_MARGIN = 1.20f;
// Watts the next step up adds, indexed by the current target rank
// (OFF/500W/1000W/2000W). 0 at the top rank (no step).
static constexpr int STEP_UP_INCREMENT_W[4] = { 500, 500, 1000, 0 };
// Settle time between charge-surplus step-ups: long enough for at least one fresh
// Pylontech sample (PYLONTECH_POLL_INTERVAL_MS, 5 s) to reflect the new load
// before re-evaluating. The discharge rule still catches any overshoot in ~10 s.
static constexpr uint32_t BOILER_BATT_RAISE_INTERVAL_MS = 60UL * 1000;  // 1 min
// After the discharge rule steps the boiler down (the panels could not carry the
// stage it probed), the throttled-surplus path must hold the lower stage at least
// this long before probing up again. Lets it settle on the sustainable ceiling and
// re-probe only occasionally as the sun changes, instead of oscillating up/down.
static constexpr uint32_t BOILER_REPROBE_MS = 30UL * 60 * 1000;  // 30 min

// BOILER_ON_PIN reads a clean steady level when the boiler is genuinely on (LOW) or
// off (HIGH), but the high-impedance INPUT_PULLUP picks up brief (sub-10 ms) noise
// glitches that flip a single read. An un-debounced read let one glitch trigger a
// spurious autoRegulate() step-up that the next read immediately reverted. Require the
// raw level to hold continuously for this long before the reported state follows it,
// rejecting glitches in either direction. Negligible vs. the heating timescale; long
// enough to swamp any plausible glitch. See sampleBoilerInput().
static constexpr uint32_t BOILER_INPUT_DEBOUNCE_MS = 200;

// The B-verify opto on GPIO36 (input-only, no internal pull-up, reading AC) can
// momentarily read LOW on a noise glitch or an AC zero-crossing notch. An
// un-debounced single LOW read tripped the sticky boilerFault, requiring a reboot.
// Require the commanded-ON-but-reads-OFF mismatch to persist this long before
// faulting. Any verified-ON read in between clears the timer (a genuinely
// stuck-open B reads LOW continuously and still faults within this window). Long
// enough to swamp several AC half-cycles, negligible vs. the 15-min step cadence.
// See tickBoiler().
static constexpr uint32_t BOILER_RELAY_B_VERIFY_DEBOUNCE_MS = 200;

// Tank sensors read NAN until the first ADC sample (~1 s after setup()). A sensor
// still invalid this long after the first evaluation is reported in the log; see
// updateTargetReached(). Measured from the first tick, not from power-on: the
// WiFi connect in setup() alone takes longer than this.
static constexpr uint32_t BOILER_TANK_SENSOR_GRACE_MS = 5000;

// volatile: currentPower may be read from another FreeRTOS task
// (inverter control). 1-byte enum reads are atomic on Xtensa.
static volatile BoilerPower currentPower = BOILER_OFF;
static volatile BoilerPower targetPower = BOILER_OFF;
static uint32_t lastStepMs = 0;

// Blocks all stepping (especially the 1000→2000 C-toggle) until the
// opto-isolated verifier confirms B physically closed. Without this gate
// a slow opto filter could let C move while B is still mechanically open
// → L–N short through C's transition arc.
static bool waitingForRelayBVerify = false;
static uint32_t relayBVerifyStartMs = 0;

// When the steady-state B mismatch (commanded ON, sensor reads OFF) first
// appeared. 0 = currently no mismatch. Debounces emergencyShutdown() so a brief
// glitch on GPIO36 does not trip the sticky fault (see BOILER_RELAY_B_VERIFY_DEBOUNCE_MS).
static uint32_t relayBMismatchSinceMs = 0;

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

// Last time the discharge rule stepped the boiler down. The throttled-surplus path
// holds off probing up for BOILER_REPROBE_MS after this (see autoRegulate()).
// Initialised in boilerRelayInit() so the window is already elapsed at boot.
static uint32_t lastDischargeStepDownMs = 0;

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

// Manual mode: hold the commanded target, skip autoRegulate(). Loaded from NVS in
// boilerRelayInit(), written by setBoilerManual(). volatile like currentPower —
// read from other tasks (status JSON, InfluxDB sampler).
static volatile bool boilerManual = false;

// Virtual thermostat state (see relay.h / doc/todo/008). The target is loaded
// from NVS in boilerRelayInit(), written by setBoilerTargetTemp(). volatile like
// boilerManual — read from the status JSON and the InfluxDB sampler.
// targetReached is the hysteresis output; tankTempValid tracks whether the last
// evaluation had both sensors, so a sensor dropping out is logged once (-1 =
// nothing evaluated yet, so the NAN readings before the first ADC sample after
// boot are not reported as a failure).
static volatile uint8_t boilerTargetTempC = BOILER_TARGET_DEFAULT_C;
static volatile bool targetReached = false;
static int8_t tankTempValid = -1;
static uint32_t firstTankEvalMs = 0;  // millis() of the first evaluation (0 = none yet)

static inline bool isCommandedRelayBHigh(BoilerPower p) {
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
  // Pre-date so the reprobe window is already elapsed at boot (modular arithmetic
  // keeps "now - lastDischargeStepDownMs >= BOILER_REPROBE_MS" true from tick one).
  lastDischargeStepDownMs = millis() - BOILER_REPROBE_MS;
  waitingForRelayBVerify = false;

  // Restore the mode, never the power: a restart of unknown cause (midnight
  // reboot, panic, WiFi watchdog) must not re-engage a load by itself. Read-only
  // begin() fails while the namespace does not exist yet (first boot after a
  // flash); getBool() then falls back to the default, i.e. Auto.
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, true);
  boilerManual = prefs.getBool(NVS_KEY_MANUAL, false);
  uint8_t tgt = prefs.getUChar(NVS_KEY_TARGET, BOILER_TARGET_DEFAULT_C);
  prefs.end();
  if (boilerManual) printInfo("Boiler mode Manual restored from NVS, target OFF");
  // Clamp a corrupt value rather than trusting it; "reached" is recomputed from
  // live readings on the first tick.
  boilerTargetTempC = (tgt < BOILER_TARGET_MIN_C || tgt > BOILER_TARGET_MAX_C)
                        ? BOILER_TARGET_DEFAULT_C : tgt;
  printInfo("Boiler target temp %u C (NVS)", (unsigned)boilerTargetTempC);
}

// Virtual thermostat: recompute targetReached from the two tank sensors with a
// BOILER_TARGET_HYST_C hysteresis around the target. Both sensors must be at or
// above the target to flip to "reached"; the cooler one dropping to
// target - hysteresis flips it back. While either sensor reads NAN the virtual
// thermostat is out of action: "reached" is cleared and the tank heats up to the
// physical thermostat, exactly as before this feature. Losing hot water to a dead
// sensor would be worse than heating a few degrees higher, and the physical
// thermostat is the safety limit either way. Called every tick and right after
// the target changes.
static void updateTargetReached() {
  float th = g_temp_h, tl = g_temp_l;
  bool valid = !isnan(th) && !isnan(tl);
  uint32_t now = millis();
  if (firstTankEvalMs == 0) firstTankEvalMs = now ? now : 1;
  if ((int8_t)valid != tankTempValid) {
    if (!valid) {
      // The ADC is first sampled ~1 s after the loop starts; do not report that
      // gap as a failure. A sensor still invalid after the grace period is reported.
      if (tankTempValid < 0 && now - firstTankEvalMs < BOILER_TANK_SENSOR_GRACE_MS) return;
      printWarning("Boiler tank sensor invalid (H %.1f / L %.1f), heating up to the physical thermostat",
                   th, tl);
    } else if (tankTempValid >= 0) {
      printInfo("Boiler tank sensors valid again (H %.1f / L %.1f)", th, tl);
    }
    tankTempValid = valid;
  }
  if (!valid) {
    if (targetReached) {
      influx_log_event();
      targetReached = false;
    }
    return;
  }

  float tmin = th < tl ? th : tl;
  uint8_t tgt = boilerTargetTempC;
  if (!targetReached && tmin >= tgt) {
    printInfo("Boiler target %u C reached (H %.1f / L %.1f)", (unsigned)tgt, th, tl);
    influx_log_event();
    targetReached = true;
  } else if (targetReached && tmin <= (float)tgt - BOILER_TARGET_HYST_C) {
    printInfo("Boiler tank below %u C (H %.1f / L %.1f), heating allowed",
              (unsigned)(tgt - BOILER_TARGET_HYST_C), th, tl);
    influx_log_event();
    targetReached = false;
  }
}

bool setBoilerTargetTemp(uint8_t celsius) {
  if (celsius < BOILER_TARGET_MIN_C || celsius > BOILER_TARGET_MAX_C) return false;
  if (celsius == boilerTargetTempC) return true;
  printInfo("Boiler target temp %u -> %u C (Web UI)", (unsigned)boilerTargetTempC,
            (unsigned)celsius);
  influx_log_event();
  boilerTargetTempC = celsius;

  Preferences prefs;
  if (prefs.begin(NVS_NAMESPACE, false)) {
    prefs.putUChar(NVS_KEY_TARGET, celsius);
    prefs.end();
  } else {
    printWarning("Boiler target temp: NVS open failed, value not persisted");
  }
  // Re-evaluate at once so a lower target on a hot tank stops the heating on
  // the next tick and a higher one lets it resume without waiting a loop.
  updateTargetReached();
  return true;
}

uint8_t getBoilerTargetTemp() {
  return boilerTargetTempC;
}

bool isBoilerTargetReached() {
  return targetReached;
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
  setBoilerTarget(target, boilerManual ? "Web UI, manual" : "Web UI");
}

void setBoilerManual(bool on) {
  if (on == boilerManual) return;
  printInfo("Boiler mode %s -> %s (Web UI), target %s", boilerManual ? "Manual" : "Auto",
            on ? "Manual" : "Auto", POWER_LABELS[targetPower]);
  // Snapshot the state around the switch, like a target change.
  influx_log_event();
  boilerManual = on;
  // The auto trackers were frozen while Manual held the target. Restart them so
  // the first Auto tick does not act on timing accumulated before the switch:
  // the discharge rule waits its full BOILER_DISCHARGE_OFF_MS again and the
  // step-up paths their full interval.
  battDischarging = false;
  lastPowerChangeMs = millis();

  Preferences prefs;
  if (prefs.begin(NVS_NAMESPACE, false)) {
    prefs.putBool(NVS_KEY_MANUAL, on);
    prefs.end();
  } else {
    printWarning("Boiler mode: NVS open failed, mode not persisted");
  }
}

bool isBoilerManual() {
  return boilerManual;
}

// Rule 1 tracker: AC output overload must persist BOILER_OVERLOAD_OFF_MS before
// it counts. Shared by autoRegulate() and manualHold() — the overload guard is
// about the inverter, not the battery, so it applies in both modes.
static bool updateOverload(const InverterState& s, uint32_t now) {
  bool overload = s.ac_active_w > BOILER_OVERLOAD_W;
  if (overload && !overloading) {
    overloading = true;
    overloadStartMs = now;
  } else if (!overload) {
    overloading = false;
  }
  return overloading && (now - overloadStartMs >= BOILER_OVERLOAD_OFF_MS);
}

// Manual mode counterpart of autoRegulate(): keep the commanded target, act only
// on a sustained AC overload. The force-offs run before this in tickBoiler().
static void manualHold(uint32_t now) {
  InverterState s;
  inverter_get_status(&s);
  if (updateOverload(s, now)) setBoilerTarget(BOILER_OFF, "AC overload");
}

// Automatic power regulation from inverter state. Only called while the inverter
// data is valid and the boiler is in normal operation (no fault, not mid-B-verify).
// Snapshots the inverter data once, updates the morning gate and applies the
// rules in priority order: AC overload (OFF) > battery discharge (one step down)
// > PV surplus (one step up).
static void autoRegulate(uint32_t now) {
  InverterState s;
  inverter_get_status(&s);

  // Battery state comes directly from the Pylontech console. tickBoiler() only
  // calls autoRegulate() while pylontech_data_valid(), so this snapshot is fresh.
  PylontechState b;
  pylontech_get_status(&b);

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
  bool sustainedOverload = updateOverload(s, now);

  // Rule 2: sustained battery discharge -> step down one level. A brief spike
  // must not trip it, so require BOILER_DISCHARGE_OFF_MS of continuous discharge.
  // Pylontech current is signed (+ charge / - discharge). Tolerate a larger discharge
  // while the battery is well charged (see BOILER_DISCHARGE_HIGH_SOC).
  float dischargeLimitA = (b.soc > BOILER_DISCHARGE_HIGH_SOC)
                            ? BOILER_DISCHARGE_HIGH_A : BOILER_DISCHARGE_A;
  bool discharging = b.current < -dischargeLimitA;
  if (discharging && !battDischarging) {
    battDischarging = true;
    dischargeStartMs = now;
  } else if (!discharging) {
    battDischarging = false;
  }
  bool sustainedDischarge = battDischarging &&
                            (now - dischargeStartMs >= BOILER_DISCHARGE_OFF_MS);

  // Rule 1 (overload) and Rule 2 (discharge) apply in every band and take
  // priority over any step up.
  if (sustainedOverload) {
    setBoilerTarget(BOILER_OFF, "AC overload");
    return;
  }
  if (sustainedDischarge && targetPower > BOILER_OFF) {
    setBoilerTarget((BoilerPower)(targetPower - 1), "battery discharge");
    dischargeStartMs = now;  // restart timer so the next step-down waits again
    lastDischargeStepDownMs = now;  // hold the throttled-surplus probe (reprobe gate)
    return;
  }

  // SoC bands (battery SOC straight from the Pylontech).
  if (b.soc < BOILER_RAISE_MIN_SOC) {
    // Below the floor: keep the boiler off so the battery can recharge.
    setBoilerTarget(BOILER_OFF, "battery SOC low");
  } else if (targetPower < BOILER_2000W) {
    // At/above the floor two independent step-up paths can raise the boiler; the
    // discharge rule above caps any overshoot. "Up" is optimistic, "down" is guarded.

    // Path A (charge surplus): the battery is actively accepting charge, so its
    // charge power is a direct surplus signal. Step up only when the power flowing
    // into the battery exceeds the extra load the next stage adds (plus the
    // BOILER_STEP_MARGIN reserve). current > 0 means charging. Fast cadence.
    float chargeW = b.current > 0 ? b.voltage * b.current : 0.0f;
    bool chargeSurplus =
        (now - lastPowerChangeMs >= BOILER_BATT_RAISE_INTERVAL_MS) &&
        chargeW > STEP_UP_INCREMENT_W[targetPower] * BOILER_STEP_MARGIN;

    // Path B (throttled surplus): near full the inverter floats and throttles the
    // MPPT, so charge power reads ~0 even in full sun (high pv_v, tiny current). With
    // no other load this deadlocks the boiler. Treat "sun available (high pv_v) AND
    // battery not discharging" as surplus and probe upward on the slow cadence; the
    // discharge rule backs it off if the panels cannot carry it. Gated by the morning
    // warm-up and held BOILER_REPROBE_MS after the last discharge step-down so it
    // settles on the sustainable ceiling instead of oscillating.
    bool throttledSurplus =
        morningPassed &&
        s.pv_input_voltage > BOILER_RAISE_MIN_PV_V &&
        b.current > -dischargeLimitA &&
        (now - lastPowerChangeMs >= BOILER_RAISE_INTERVAL_MS) &&
        (now - lastDischargeStepDownMs >= BOILER_REPROBE_MS);

    if (chargeSurplus) {
      setBoilerTarget((BoilerPower)(targetPower + 1), "battery charge surplus");
    } else if (throttledSurplus) {
      setBoilerTarget((BoilerPower)(targetPower + 1), "PV surplus");
    }
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
  // Sample the boiler input before the fault short-circuit below, so the
  // reported isBoilerOn() keeps tracking the real pin even while faulted.
  // Otherwise a latched fault would freeze the metric at its pre-fault value
  // until reboot (reported "on" stays stale the whole time the fault is set).
  uint32_t now = millis();
  sampleBoilerInput(now);
  // Same reasoning for the virtual thermostat: keep it tracking the tank even
  // while faulted so the reported state stays live.
  updateTargetReached();

  if (boilerFault) return;

  // Mains absent at A.COM (no heating possible, no AC for the opto to detect),
  // tank at its target (virtual thermostat, same semantics as the physical one)
  // or inverter data stale/lost (comms down for several consecutive polls) —
  // force the chain back to OFF. Automatic regulation runs only when none holds.
  const char* forceOffReason = nullptr;
  if (!isBoilerOn()) forceOffReason = "boiler input off";
  else if (targetReached) forceOffReason = "target temperature reached";
  else if (!inverter_data_valid()) forceOffReason = "inverter data invalid";
  else if (!pylontech_data_valid()) forceOffReason = "battery data invalid";

  if (forceOffReason) {
    // Cancel any in-flight B-verify wait and steer the chain back to OFF
    // (falls through to the settle/step block). Reset the auto trackers so we
    // do not act on stale timing when normal operation resumes.
    waitingForRelayBVerify = false;
    setBoilerTarget(BOILER_OFF, forceOffReason);
    overloading = false;
    battDischarging = false;
    relayBMismatchSinceMs = 0;
  } else if (waitingForRelayBVerify) {
    // Block all stepping (especially the 1000→2000 C-toggle) until the
    // opto confirms B physically closed. Fault if it doesn't within 1s.
    if (isRelayBoilerBVerifiedOn()) {
      waitingForRelayBVerify = false;
    } else if ((now - relayBVerifyStartMs) >= RELAY_B_VERIFY_TIMEOUT_MS) {
      emergencyShutdown("Relay B did not verify within 1s after closing");
    }
    return;
  } else if (isCommandedRelayBHigh(currentPower) && !isRelayBoilerBVerifiedOn()) {
    // Steady-state monitoring: B was previously verified ON (the wait
    // phase above only exits on a HIGH read) and has now dropped. Debounce:
    // tolerate brief glitches on the high-impedance GPIO36 opto and fault only
    // if the mismatch persists (see BOILER_RELAY_B_VERIFY_DEBOUNCE_MS). Returning here
    // also blocks all stepping while a mismatch is pending, so no C-toggle can
    // occur with B open.
    if (relayBMismatchSinceMs == 0) relayBMismatchSinceMs = now;
    if (now - relayBMismatchSinceMs >= BOILER_RELAY_B_VERIFY_DEBOUNCE_MS) {
      emergencyShutdown("Relay B mismatch: commanded ON, sensor reads OFF");
    }
    return;
  } else {
    // Normal operation: B verified (or not commanded ON) — clear the mismatch
    // debounce and adjust the target from inverter state. Manual holds the
    // commanded target instead; the force-offs above apply in both modes.
    relayBMismatchSinceMs = 0;
    if (boilerManual) manualHold(now);
    else autoRegulate(now);
  }

  if (now - lastStepMs < RELAY_SETTLE_MS) return;
  if (currentPower == targetPower) return;

  // Adjacent STATE_BITS differ in exactly one bit, so each step toggles
  // a single relay along a safe edge of the OFF↔500↔1000↔2000 chain.
  int8_t step = (targetPower > currentPower) ? +1 : -1;
  uint8_t nextRank = (uint8_t)currentPower + step;

  // Detect "this step closes B" (only the 500W→1000W edge on the chain).
  bool willCloseRelayB = !isCommandedRelayBHigh(currentPower) &&
                    (STATE_BITS[nextRank] & 0b010);

  writeBits(STATE_BITS[nextRank]);
  currentPower = (BoilerPower)nextRank;
  lastStepMs = now;

  if (willCloseRelayB) {
    waitingForRelayBVerify = true;
    relayBVerifyStartMs = now;
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
