---
id: 006
title: Manual boiler mode — hold the commanded power, suspend the automatic regulation
type: enhancement
status: in-progress
priority: medium
component: boiler
created: 2026-09-18
---

# Manual boiler mode — hold the commanded power, suspend the automatic regulation

## Summary

The boiler buttons on the main page (`OFF / 500W / 1000W / 2000W`) set a
target that `autoRegulate()` (`relay.cpp:250`) overrides within seconds: rule 2
steps the boiler down after 10 s of battery discharge above 7 A (2 A below 75 %
SoC), and the SoC band forces `OFF` below 70 %. A manually chosen 2 kW at night
is therefore back at `OFF` in about half a minute. There is no way to *hold* a
power level.

Two things need that hold:

- **Manual + power** — deliberately draining the battery, e.g. to observe a
  charge from a low SoC on the CAN bus (CVL/CCL, see
  [`../battery_can_data_spec.md`](../battery_can_data_spec.md)). The inverter
  charges to 52.5 V, below the BMS's 52.8 V CVL, and the pack sits at 100 % every
  evening; the cottage's own consumption drops it only ~10 %/night. The boiler is
  the only controllable load on site (2 kW ≈ 100 % → 30 % of a US5000 in ~1.7 h).
- **Manual + OFF** — keeping the boiler off for days so the battery fills, e.g.
  after a long overcast spell, without `autoRegulate()` re-enabling it on the
  first sunny morning.

Wanted: a `Manual / Auto` toggle next to the power buttons. In `Manual` the
boiler keeps whatever target is commanded — no step-ups, no step-downs — while
every hardware safety path keeps working exactly as today.

## Expected behavior

| | Auto (today) | Manual |
| --- | --- | --- |
| step-up paths (charge surplus, PV surplus, morning gate) | yes | **no** |
| rule 2 — step down on sustained discharge | yes | **no** |
| SoC band `< 70 % → OFF` | yes | **no** |
| force-off: boiler input, inverter data invalid, battery data invalid | yes | yes → target `OFF`, mode stays Manual |
| rule 1 — AC overload > 5 kW → `OFF` | yes | yes |
| Relay B verify, sticky fault | yes | yes |
| power buttons | set the target, Auto then overrides it | set the held target |
| boot / midnight reboot | Auto, `OFF` | **Manual restored**, `OFF` |
| expiry | — | none |

**Force-off is unchanged and outside the mode switch.** Both ways a manual run
ends by itself — the thermostat opening when the water is hot, and the inverter
dropping its 230 V output at the 46.0 V LVD when the battery is empty — arrive
as the same signal: `BOILER_ON_PIN` reads the phase at `A.COM`, i.e. *behind*
the thermostat *and* the inverter output. `tickBoiler()` (`relay.cpp:403-416`)
evaluates it before and independently of `autoRegulate()`, so Manual only
replaces the final `else` branch. The ESP, the relays and the phone have their
own supply, so the controller stays up through an LVD shutdown and opens the
relays as soon as the input drops. This path fires daily in production
(`app.log`, 2026-09-14 … 09-18: 6 × `boiler input off`, 1 × `battery
discharge`, 0 × `data invalid`).

**Force-off overwrites the target, not the mode.** After the thermostat or the
LVD ends a manual run, the target is `OFF` and Manual keeps holding `OFF`. When
the phase returns — the water cools, the inverter restarts in the morning —
nothing restarts on its own; the operator has to press a power button again.
The UI shows `Manual · OFF`, which is the truthful state: Auto is not running
and the boiler is idle.

**No expiry.** An expiry only fits Manual + power; for Manual + OFF it would do
the opposite of the intent (Auto would switch the boiler back on after the
timeout). Manual + power does not need one: the boiler is a finite thermal
store, so a held power always ends through `BOILER_ON_PIN` — thermostat or
empty battery — and then stays `OFF` as above. The `data invalid` force-offs can
end a run early too; the 4-day log shows none, so they are kept as they are.

## Persistence

`task_midnight_reboot()` (`main.cpp:450`) restarts the ESP every night at
00:00 (log: 2026-09-15 … 09-18, 00:00:11–20 each night). A RAM-only mode would
silently cancel Manual + OFF every night, which defeats its multi-day purpose.
So the **mode is persisted, the power never is**:

| | stored | after a restart |
| --- | --- | --- |
| mode Auto / Manual | yes | restored |
| held power | **no** | always `OFF` |

A restart whose cause is unknown (midnight, panic, WiFi watchdog) must never
re-engage a 2 kW load by itself. Manual + power therefore ends at midnight as
Manual + `OFF`; Manual + OFF survives unchanged; Auto behaves as today.

Storage: NVS through the Arduino `Preferences` library — namespace `boiler`,
key `manual` (bool). Written only when the toggle is pressed (a handful of
writes per month, no wear concern), read once in `boilerRelayInit()` with
default `false`, so the first boot after a flash is Auto exactly as today. The
default `featheresp32` partition table carries a 20 KB `nvs` partition that the
WiFi stack already uses on this device, so the mechanism is known to work
there. NVS also survives a power cycle of the ESP and a firmware reflash; only
an explicit `erase_flash` clears it.

Rejected alternatives:

- `RTC_DATA_ATTR` — no flash write, survives `ESP.restart()` and a panic, but
  **not** a power-on, and reads garbage after one without a magic/checksum.
  Behaviour would depend on the reset type, which is exactly the opacity to
  avoid.
- A file in LittleFS — the FS exists (web files, `/app.log`), but it is more
  code than `Preferences`, shares the partition with the web files that
  `/upload` rewrites, and NVS is the idiomatic home for a single flag.

Store the one bool only — no struct, no versioning. If a second value ever
needs persisting, design that separately.

## Implementation

- `relay.h` / `relay.cpp`: `setBoilerManual(bool)`, `isBoilerManual()`;
  `tickBoiler()` calls `manualHold(now)` instead of `autoRegulate(now)` while
  Manual is set. `manualHold()` applies only rule 1 (AC overload), through the
  same `updateOverload()` helper `autoRegulate()` uses. Switching modes resets
  the discharge tracker and `lastPowerChangeMs`, so the first Auto tick does not
  act on timing accumulated while Manual held the target. The mode change is
  logged via `printInfo()` and snapshotted with `influx_log_event()` like a
  target change.
- `esp_webserver.cpp`: `POST /cmd` `{ "name": "set_boiler_manual", "value":
  0|1 }`; `/status` gains `bman` (`bm` is already the battery mode).
- `influx.cpp`: `chajda-boiler` gains `manual` (bool), so the history tells a
  deliberate discharge from a regulation decision.
- `data/index.html` + `app.js`: a fifth button after `2000W` showing the
  current mode (`Auto` / `Manual`, highlighted when Manual). It is *not* gated
  on the boiler input like the power buttons are — Manual + OFF must be
  selectable while the thermostat is open.
- `main.cpp`: LCD boiler row appends ` M` while Manual.
- `grafana/chajda.json`: the "Boiler input / fault" state timeline gains the
  `manual` series (panel 13, refId `C`).

Build: `pio run` passes (flash 87.1 %). Not flashed yet.

## Verification plan (after deploy)

1. `/status` shows `bman: false`; LCD row reads `Boiler: OFF`.
2. Press `Manual` → button turns amber, `app.log` gets `Boiler mode Auto ->
   Manual (Web UI), target OFF`; LCD row reads `Boiler: OFF M`.
3. Press `500W` in Manual at a time Auto would step it down (evening, battery
   discharging): the target must hold past the 10 s discharge window.
4. Wait for the thermostat (or switch the boiler input off): target goes `OFF`
   with `boiler input off`, button stays `Manual`, nothing restarts when the
   input returns.
5. Cross midnight in Manual: after the 00:00 reboot `app.log` has `Boiler mode
   Manual restored from NVS, target OFF` and `/status` shows `bman: true`,
   `bp: 0`.
6. `chajda-boiler.manual` appears in InfluxDB and on the Grafana panel.

## Resolution

_Not resolved yet._
