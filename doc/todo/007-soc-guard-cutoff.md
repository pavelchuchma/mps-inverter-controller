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
2. ~~**Does the output restart on its own after a "battery low" shutdown once
   the cut-off is lowered again?**~~ **Answered 2026-09-24, and the answer is
   no** — see "First real trip" under Resolution. The inverter does not merely
   drop the output: off-grid at night it shuts down completely, and the dawn
   restart tripped the BMS. The AC-contactor fallback is back on the table.
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

_Not resolved yet._ The guard itself works as designed; the mechanism it relies
on (the inverter's LVD) does not. Findings from the first real trip below.

### First real trip, 2026-09-24 — inverter shut down for the night, BMS tripped at dawn

Test: boiler 2 kW in manual from 00:20 CEST, −44 A, SoC 92 → 15 %. The pack
had never been below 50 % since the installation, so this was the first time
the inverter ever reached its low DC cut-off. Sources: `app.log`, InfluxDB
`chajda-inverter`, `chajda-inverter-config`, `chajda-battery-can`,
`chajda-battery`. Times CEST.

| Time | Event |
| --- | --- |
| 02:04:59 | `SoC guard: armed at 15 %, PSDV48.0 -> ACK` (pack 47.4 V under load) |
| 02:05:02 | AC output off (`boiler input off`) — the intended part |
| 02:05:21 | Last inverter reply: mode `D`, batt 48.0 V. Then **silence for 4 h 19 min**: no QPIGS, no QPIRI. The unit powered itself off; pack rested at 48.8 V, 0 A, DFET on |
| 06:24:32 | Dawn, PV 135 V / 20 W: inverter wakes in mode `S`, reports batt 48.8 V |
| 06:24:41 | **BMS protection 0x0000 → 0x0080** (0x359 byte 0 bit 7, *discharge over current*), `dchg 0`, DCL 0 A — 9 s after the wake-up |
| 06:25:43 / 06:25:55 | BMS re-enables discharge after 62 s, trips again 12 s later |
| 06:26:57 / 06:27:09 | Third attempt, trips again and **stays tripped** (latched) until the power cycle |
| 06:27:14 | Inverter sees 26.4 V on its battery terminals, then goes silent again for 13 min |
| 06:40–08:39 | Inverter alive on PV only: cycles `P`/`S`/`B` (repeated reboots on a few tens of watts), batt_v 0 — the "Battery open" fault on the LCD |
| 08:39:56 | On site: battery switched off (CAN + RS485 down) |
| 08:41:09 | Panels disconnected; the inverter answers 10 s more, then dies |
| 08:41:30 | Battery on: protection 0x0000, DFET on, SoC 15 % |
| 08:41:39–08:42:00 | Inverter boots from the battery: `P` → `S` → **`D` again** — LVD is still 48.0 V (armed), pack 48.9 V |
| 08:42:29 | Panels back: `S`, charging 3 → 19 A; 08:43:40 mode `B`, output up |
| 09:20:44 | `SoC guard: disarmed at 25 %, PSDV46.0 -> ACK`, QPIRI confirms 46.0 at 09:23 |

What it means:

- **The BMS did not trip on SoC or voltage.** 48.8 V is 3.05 V per cell; an
  under-voltage would be bit 2, a low-SoC cut would not arrive four hours after
  15 % and would not clear after 62 s. Bit 7 is the vendor's *discharge over
  current* (the table in `battery_can_spec.md` stops at bit 6 — to be added).
  The trips line up with the inverter reconnecting the battery from a cold
  state: the unit had been fully off, its DC bus discharged, and it woke on a
  20 W PV trickle. The CAN telemetry (10 s samples, min −0.6 A) cannot see a
  millisecond inrush, so this is inferred from the timing, but whatever the
  waveform was, the BMS's diagnosis was over-current, not empty battery.
- **Why only a full power cycle helped.** The third trip latches; only
  switching the battery off clears it, and the Pylontech power-on sequence
  pre-charges the inverter (soft start), which the automatic retry does not.
  The inverter stayed alive on PV in the fault state and did not retry by
  itself. And with the guard armed (LVD 48.0 V) a battery-only boot goes
  straight back to `D` (seen at 08:41:59): the unit came up only once the
  panels were reconnected.
- **The design assumption was wrong.** "The output goes off with fault 04, the
  PV charger keeps working" holds by day only. Off-grid at night the inverter
  has no source it accepts once the battery is under the cut-off, so it shuts
  down entirely (mode `D`, RS232 dead). Open question 2 is answered: it does
  not come back on its own, and the dawn restart is what tripped the BMS.
- **Not specific to 15 %.** Any night-time LVD trip does the same, including
  the original 46 V one; it simply never happened before. The guard did not
  introduce the failure, it caused the first occurrence.
- The ESP and the guard behaved exactly as specified throughout (edge writes,
  ACKs, verification, disarm at 25 %).

Data-quality notes: mode `D` is not in the QMOD table of
`ps_rs232_protocol_FULL_ai_ready.txt` (presumably "shutdown"); the
`chajda-inverter` series has gaps 02:05–06:24 and 06:27–06:40 because the
RS232 died with the unit; the RS485 `power_events` word flashed 0x08000000,
0x00800000 and 0x00010000 at the three trips (bits not decoded).

Next steps to decide:

1. Stop using the LVD as the shedding mechanism. Drop AC loads with a contactor
   on a spare relay instead, so the inverter never enters `D` (the fallback
   named in open question 2).
2. If the LVD stays: alert on `protection != 0` and on inverter silence longer
   than a minute, since both went unnoticed until the morning.
3. Docs: add bit 7 to the `0x359` table in `battery_can_spec.md`, and `D` to the
   QMOD list.
