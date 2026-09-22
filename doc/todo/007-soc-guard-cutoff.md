---
id: 007
title: SoC guard — stop discharging at 15 % BMS SoC by driving the inverter's LVD
type: enhancement
status: in-progress
priority: medium
component: inverter
created: 2026-09-20
---

# SoC guard — stop discharging at 15 % BMS SoC by driving the inverter's LVD

## Summary

Wanted: the inverter output goes off when the battery reaches **15 % SoC** and
comes back at **25 %**, with the SoC taken from the Pylontech BMS over CAN.

The inverter cannot do this by itself. It is an off-grid installation, so the
only setting that ever stops a discharge is the low DC cut-off (program 29,
RS232 `PSDV`, QPIRI[9], `lvd_v`), and that setting is a voltage. The
"back to utility" (program 12, `PBCV`) and "back to battery" (program 13,
`PBDV`, QPIRI[22]) thresholds only govern switching between grid and battery;
with no grid they have no effect and are **not touched** by this change.

A voltage threshold cannot express a SoC on LiFePO4. From the InfluxDB history
(`chajda-battery`, 2026-06-30 … 09-20, 5-minute means at |I| < 2 A) the resting
pack voltage is 49.75 V everywhere between 44 % and 96 % SoC — flat. Below
~25 % the voltage does fall (roughly 48.5 V at 25 %, 48.0 V at 20 %, 47.8 V
at 15 %, 47.3 V at 10 %, resting), but the load sag on top of it is of the
same size as the whole usable band: ~0.35 V at 17 A measured (≈ 20 mΩ), so
~0.8–1 V at a 2 kW draw. A fixed cut-off of 48 V would trip at anything from
15 % (idle) to 30–35 % (2 kW), and the rebound after the trip would restart
the output and trip it again on the next load.

So the guard lives in the ESP: it knows the BMS SoC and it already writes
`PSDV` (allowlisted in `esp_webserver.cpp:340` for the settings page). It moves
the cut-off between two values on SoC edges, so the inverter's own protection
does the switching, with the ESP only deciding *when* it is armed.

## Expected behavior

| Event | Condition | Action |
| --- | --- | --- |
| arm | BMS SoC ≤ 15 % | write `PSDV48.0` |
| disarm | BMS SoC ≥ 25 % | write `PSDV46.0` |

- **Arm at 15 %.** 48.0 V is the upper end of the PSDV range on a 48 V unit
  (`settings.js:104`: 40.0–48.0). The resting voltage at 15 % is ~47.8 V, so
  the inverter trips immediately, not on the next load step. The output goes
  off with fault 04 "battery low"; the PV charger keeps working.
- **Between 15 % and 25 %** the cut-off stays at 48 V. The output runs only
  while PV holds the pack above it, i.e. the sun runs the cottage and the
  battery does not. That is the intent, not a side effect.
- **Disarm at 25 %.** The pack is at ~48.5 V resting, well above 46 V, so the
  inverter restarts regardless of whatever restart hysteresis it has built in.
  Nobody has to know that number.
- **Writes happen on the edge only.** Two writes on a bad day, none on a normal
  one. The value lives in the inverter's EEPROM, so no periodic refresh.
- **Every write is verified.** The config poll (`inverter_comm.cpp:445`,
  every `INVERTER_CONFIG_INTERVAL_MS` = 5 min) already reads QPIRI[9] into
  `lvd_v`. When the guard's intended value and `lvd_v` disagree — a NAK, a lost
  frame, someone editing program 29 on the LCD — log a warning and rewrite,
  at most once per config interval.
- **Stale SoC changes nothing.** `pylontech_can_valid()` false (no burst for
  `CAN_STALE_MS` = 10 s, `pylontech_can.h`), or `soc_ts_ms` older than 5 min:
  keep the last written state, log once on entering the stale condition, not
  every tick. Blind switching is worse than none.
- **Boot.** Read the current `lvd_v` from the first QPIRI and a valid SoC before
  deciding anything. If `lvd_v` is 48.0 and SoC is inside 15–25 %, the guard is
  armed from before the reboot (the nightly 00:00 restart, `main.cpp:450`, goes
  through here every day): keep it, do not disarm. Only SoC ≥ 25 % disarms.
- **Guard off** (the toggle below): while off the guard writes nothing. The
  switch-off itself makes one exception: if program 29 reads 48.0 V at that
  moment (the guard was armed), it writes `PSDV46.0` once, so switching the
  guard off does not leave the output dead until somebody edits row 29 by
  hand. Switching off in the disarmed state writes nothing. (Changed
  2026-09-22; the original "off means hands off, use row 29" rule left a
  trap for the rare off-while-armed case.)

The ESP, the relays and the phone have their own supply and stay up through
an LVD shutdown (verified in production, see
[`006-boiler-manual-mode.md`](006-boiler-manual-mode.md), "Force-off is
unchanged"). The boiler force-off on `BOILER_ON_PIN` fires on the same event
and needs no change.

## Implementation

- New `soc_guard.h` / `soc_guard.cpp` (own module, `inverter_comm.cpp` is
  already at the edge of its task stack — see the note at
  `inverter_comm.cpp:493`). The header holds the constants
  `SOC_GUARD_ARM_PCT 15`, `SOC_GUARD_DISARM_PCT 25`, `SOC_GUARD_LVD_ARMED_V
  48.0f`, `SOC_GUARD_LVD_NORMAL_V 46.0f`, `SOC_GUARD_SOC_MAX_AGE_MS 300000`;
  nothing is hard-coded in the logic. API: `soc_guard_init()`,
  `soc_guard_tick()` (main-loop task table, 1 s), `soc_guard_set_enabled()`,
  `soc_guard_get(SocGuardState*)`. State: `enabled`, `armed`, `decided`,
  `intended_lvd_v`, `last_write_ms`, `last_write_ok`, `soc_at_last_change`.
  Writes go through `inverter_query_raw("PSDVnn.n")` with the same ACK check
  `handleInvSet()` uses.
- Verification judges each QPIRI read once (`last_checked_cfg_ts`) and only a
  read taken after the last write; otherwise the up-to-5-minute-old snapshot
  would show the previous value right after a good write and trigger a
  rewrite loop. A failed edge write is therefore retried by the next config
  read, not by the tick.
- Persistence: `enabled` in NVS via `Preferences`, namespace `socguard`, key
  `enabled` (bool), default **false** — the first boot after flashing changes
  nothing until somebody turns it on. Same pattern and reasoning as the boiler
  manual flag (`relay.cpp:240`). `armed` is **not** stored; it is re-derived
  from `lvd_v` on the first tick after boot or after the switch is flipped,
  which is the truthful state.
- `esp_webserver.cpp`: `POST /cmd` `{ "name": "set_soc_guard", "value": 0|1 }`;
  `/status` gains `sg` (enabled), `sga` (armed), `sgl` (intended LVD V), `sgok`
  (last write ACKed, absent until the first write), `sgarm` / `sgdis` (the
  thresholds, so the page shows the firmware's numbers).
- `influx.cpp`: new measurement `chajda-soc-guard` with `enabled`, `armed`,
  `intended_lvd_v`, written on every sample and in the event snapshots. Not
  fields on `chajda-inverter-config` as first planned: that block is written
  only when a QPIRI read lands, so an arm would show up to five minutes late
  and never in an `influx_log_event()` snapshot. Arm/disarm and the switch call
  `influx_log_event()` like a boiler target change.
- `data/settings.html` + `settings.js`: a `SoC guard` block under the settings
  table with the state pill, the on/off button (with confirm) and the two
  thresholds read from `/status`. The program 29 row keeps working as a manual
  override and its note says the guard moves it.
- `main.cpp`: LCD SoC row appends ` G` while armed (there is no inverter row).
- `grafana/chajda.json`: panel 13 (boiler state timeline) gains refId `D`
  "SoC guard armed" from `chajda-soc-guard.armed`; title now "Boiler input /
  fault / manual / SoC guard".

Build: `pio run` passes (flash 87.3 %, RAM 16.2 %).

## Open questions / verify on site before enabling

1. ~~**Does the inverter accept `PSDV48.0`?**~~ **Verified 2026-09-20 23:57**:
   `PSDV48.0` → ACK, `PSDV46.0` → ACK two seconds later, QPIRI[9] read back
   46.0 afterwards; battery at 92 %, 49.77 V, no load, AC output stayed up.
   (48.0 is the documented upper bound, `doc/ps_rs232_protocol_FULL_ai_ready.txt`
   §5.15.) Note: `/inv_config` right after a write returned an empty QPIRI
   twice — the inverter seems to need a moment after an EEPROM write — the
   guard's verification reads the 5-minute poll, so this does not affect it.
2. **Does the output restart on its own after a "battery low" shutdown once
   the cut-off is lowered again?** Assumed yes (it restarts today after the
   morning charge lifts the pack above 46 V); nobody has watched the
   lower-the-threshold path yet. If it needs a manual reset, the guard is
   useless and the fallback is an AC contactor driven by a spare relay.
3. **Whether 15 / 25 are the final numbers.** With the boiler already shed at
   70 % the cottage's own draw is ~10 %/night (006), so 15 % leaves roughly one
   more night than the 46 V LVD (~5 %). Keeping 15 % unused costs that night
   in winter; the BMS protects the cells at 44.5 V either way.

## Verification plan (after deploy)

1. ~~Flash, guard off~~ — done 2026-09-20 23:44: `/status` showed `sg: false`,
   `lvd_v` 46.0, no guard write.
2. ~~Turn the guard on with the battery full~~ — done 2026-09-20 23:57 at 92 %:
   `app.log` got `SoC guard: off -> on (Web UI)` and `SoC guard: start, LVD
   46.0 V -> disarmed, SoC 92 %`; `sga: false`, no write.
3. Manual + 2 kW boiler (006) on an evening to drain past 15 %: `app.log` gets
   `SoC guard armed at 15 %, PSDV48.0 -> ACK`, the inverter drops the output
   within a minute, `chajda-inverter-config.lvd_v` reads 48.0 at the next
   config poll, the boiler force-off logs `boiler input off`.
4. Next morning: output restarts on PV, SoC climbs; at 25 % `app.log` gets
   `SoC guard disarmed at 25 %, PSDV46.0 -> ACK` and `lvd_v` returns to 46.0.
5. Set program 29 to 47.0 by hand while armed: within 5 min a warning `SoC
   guard: lvd_v 47.0 != intended 48.0, rewriting` and the value is back.
6. Pull the CAN plug (or wait for a CAN outage in the log) while between 15 %
   and 25 %: no write, one `SoC guard: SoC stale, holding` line.

## Resolution

_Not resolved yet._
