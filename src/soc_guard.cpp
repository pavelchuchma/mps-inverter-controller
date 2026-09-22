#include "soc_guard.h"
#include <Preferences.h>
#include <math.h>
#include "inverter_comm.h"
#include "pylontech_can.h"
#include "influx.h"
#include "utils.h"

static const char* const NVS_NAMESPACE = "socguard";
static const char* const NVS_KEY_ENABLED = "enabled";

// Touched from the main loop only (tick, the /cmd handler and the LCD task all
// run there), plus read-only snapshots from the influx task. Plain copies of
// bools and floats are fine for those.
static SocGuardState g = {};
// QPIRI timestamp the last verification looked at, so each read is judged once.
static uint32_t last_checked_cfg_ts = 0;
static bool stale_logged = false;

// Write program 29 and log the outcome. Returns true on ACK.
static bool write_lvd(float v, const char* why) {
  char cmd[16];
  snprintf(cmd, sizeof(cmd), "PSDV%.1f", v);
  String resp;
  bool sent = inverter_query_raw(cmd, resp);
  bool ack = sent && resp == "ACK";
  g.last_write_ms = millis();
  if (g.last_write_ms == 0) g.last_write_ms = 1;
  g.last_write_ok = ack;
  if (ack) {
    printInfo("SoC guard: %s, %s -> ACK", why, cmd);
  } else {
    printWarning("SoC guard: %s, %s -> %s", why, cmd,
                 sent ? resp.c_str() : "NO_RESPONSE");
  }
  return ack;
}

void soc_guard_init() {
  // Read-only begin() fails while the namespace does not exist yet (first boot
  // after a flash); getBool() then falls back to the default, i.e. off, so a
  // fresh firmware changes nothing until somebody turns the guard on.
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, true);
  g.enabled = prefs.getBool(NVS_KEY_ENABLED, false);
  prefs.end();
  g.intended_lvd_v = SOC_GUARD_LVD_NORMAL_V;
  if (g.enabled) printInfo("SoC guard: enabled (restored from NVS)");
}

void soc_guard_set_enabled(bool on) {
  if (on == g.enabled) return;
  printInfo("SoC guard: %s -> %s (Web UI)", g.enabled ? "on" : "off", on ? "on" : "off");
  influx_log_event();
  g.enabled = on;
  // Re-derive armed from the live cut-off on the next tick rather than trusting
  // whatever the guard believed before it was switched off.
  g.decided = false;
  stale_logged = false;

  // Switching off while armed: hand the inverter back its normal cut-off once,
  // so "guard off" does not leave the output dead at 48 V until somebody edits
  // program 29 by hand. Judged on the live QPIRI value, not on g.armed, which
  // may be undecided right after boot. Off in the disarmed state writes nothing.
  // A NAK is logged by write_lvd() and not retried: the guard is off now.
  if (!on) {
    InverterConfig cfg = {};
    if (!inverter_get_config(&cfg)) {
      printInfo("SoC guard: off before the first QPIRI, LVD left as is");
    } else if (fabsf(cfg.lvd_v - SOC_GUARD_LVD_ARMED_V) < SOC_GUARD_LVD_EPS_V) {
      g.armed = false;
      g.intended_lvd_v = SOC_GUARD_LVD_NORMAL_V;
      write_lvd(SOC_GUARD_LVD_NORMAL_V, "disarmed by switch-off");
    }
  }

  Preferences prefs;
  if (prefs.begin(NVS_NAMESPACE, false)) {
    prefs.putBool(NVS_KEY_ENABLED, on);
    prefs.end();
  } else {
    printWarning("SoC guard: NVS open failed, switch not persisted");
  }
}

void soc_guard_get(SocGuardState* out) {
  if (out) *out = g;
}

void soc_guard_tick() {
  if (!g.enabled) return;

  // Nothing before the first QPIRI (INVERTER_CONFIG_FIRST_DELAY_MS after boot):
  // the guard must never act on a cut-off it has not read.
  InverterConfig cfg = {};
  if (!inverter_get_config(&cfg)) return;

  uint32_t now = millis();
  PylontechCanState can = {};
  pylontech_can_get(&can);
  bool soc_fresh = pylontech_can_valid() && can.soc_ts_ms != 0
                   && (now - can.soc_ts_ms) <= SOC_GUARD_SOC_MAX_AGE_MS;
  if (!soc_fresh) {
    if (!stale_logged) {
      printWarning("SoC guard: SoC stale, holding (%s)",
                   g.armed ? "armed" : "disarmed");
      stale_logged = true;
    }
    return;
  }
  if (stale_logged) {
    printInfo("SoC guard: SoC back, %d %%", can.soc);
    stale_logged = false;
  }

  if (!g.decided) {
    // First decision of this boot (or since the switch was flipped): the
    // truthful armed state is whatever program 29 holds right now. After the
    // nightly reboot inside the hysteresis band this keeps 48 V rather than
    // disarming at 16 %.
    g.armed = fabsf(cfg.lvd_v - SOC_GUARD_LVD_ARMED_V) < SOC_GUARD_LVD_EPS_V;
    g.intended_lvd_v = g.armed ? SOC_GUARD_LVD_ARMED_V : SOC_GUARD_LVD_NORMAL_V;
    g.decided = true;
    last_checked_cfg_ts = cfg.ts_ms;
    printInfo("SoC guard: start, LVD %.1f V -> %s, SoC %d %%", cfg.lvd_v,
              g.armed ? "armed" : "disarmed", can.soc);
  }

  bool want = g.armed;
  if (!g.armed && can.soc <= SOC_GUARD_ARM_PCT) want = true;
  else if (g.armed && can.soc >= SOC_GUARD_DISARM_PCT) want = false;

  if (want != g.armed) {
    // Snapshot the state that drove the decision, like a boiler target change.
    influx_log_event();
    char why[40];
    snprintf(why, sizeof(why), "%s at %d %%", want ? "armed" : "disarmed", can.soc);
    g.armed = want;
    g.intended_lvd_v = want ? SOC_GUARD_LVD_ARMED_V : SOC_GUARD_LVD_NORMAL_V;
    g.soc_at_last_change = can.soc;
    // A failed write is not retried here: the next QPIRI read shows the
    // mismatch and the verification below rewrites it, once per read.
    write_lvd(g.intended_lvd_v, why);
    last_checked_cfg_ts = cfg.ts_ms;
    return;
  }

  // Verification: judge each QPIRI read once, and only a read taken after the
  // last write - the snapshot can be up to one interval old and would otherwise
  // show the previous value right after a successful write.
  if (cfg.ts_ms == last_checked_cfg_ts) return;
  if (g.last_write_ms != 0 && (int32_t)(cfg.ts_ms - g.last_write_ms) <= 0) return;
  last_checked_cfg_ts = cfg.ts_ms;
  if (fabsf(cfg.lvd_v - g.intended_lvd_v) >= SOC_GUARD_LVD_EPS_V) {
    printWarning("SoC guard: lvd_v %.1f != intended %.1f, rewriting", cfg.lvd_v,
                 g.intended_lvd_v);
    write_lvd(g.intended_lvd_v, "rewrite");
  }
}
