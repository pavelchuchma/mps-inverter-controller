---
id: 008
title: Boiler target temperature — virtual thermostat set from the web UI
type: enhancement
status: in-progress
priority: medium
component: boiler
created: 2026-09-25
---

# Boiler target temperature — virtual thermostat set from the web UI

## Summary

The boiler stops heating only when the physical thermostat on the tank opens
and cuts the 230 V feed that `BOILER_ON_PIN` reads (see
[`006-boiler-manual-mode.md`](006-boiler-manual-mode.md), "Force-off"). Its
set point can only be changed on site, and the tank has two temperature
sensors (`g_temp_h`, `g_temp_l`) the controller already reads every second.

Wanted: a **target temperature** selectable on the main page and persisted
across reboots. The controller heats until **both** sensors are above the
target, with a small hysteresis, and then treats the boiler exactly as if the
physical thermostat had opened. Every existing rule (PV surplus, battery
discharge, SoC band, AC overload, Relay B verify, Manual mode) keeps applying
unchanged; the target is one more force-off reason, nothing else.

The physical thermostat stays wired and keeps acting as the hardware safety
limit. The virtual one is meant to sit *below* it in normal use.

## Expected behavior

### Virtual thermostat (firmware)

- Target `T` in °C, integer, allowed range 10..60, default 45.
- Let `Tmin = min(g_temp_h, g_temp_l)`.
  - "reached" becomes true when `Tmin >= T`.
  - "reached" becomes false again when `Tmin <= T - 3` (hysteresis 3 °C, so
    ±1 °C sensor jitter around the set point cannot chatter the relays).
- While either sensor reads `NAN`, the virtual thermostat is **out of
  action**: "reached" is cleared and the tank heats up to the physical
  thermostat, exactly as before this feature. (Holding the last state was
  considered and rejected: a sensor dying in "reached" would block heating
  until the next reboot, i.e. a day without hot water, and a target change
  could not clear it. Heating a few degrees higher is the lesser evil, and
  the physical thermostat is the safety limit either way.) A warning is
  logged once when the readings go invalid (after a 5 s grace period at boot,
  so the gap before the first ADC sample is not reported) and once when they
  come back.
- Changing `T` re-evaluates immediately with the same rule (a lower target on
  a hot tank flips to "reached" at once; a higher one resumes heating).
- "reached" is a **force-off reason** in `tickBoiler()`, evaluated right after
  `boiler input off` and before the data-validity checks:
  `target temperature reached`. It therefore behaves exactly like the physical
  thermostat in both modes:
  - Auto: the target goes `OFF`; once the tank cools by 3 °C the regulation
    resumes by itself.
  - Manual: the target is overwritten with `OFF`, the mode stays Manual and
    nothing restarts when the tank cools (same as today with the physical
    input, see 006).
- Priority of the two thermostats does not matter for control (both are
  force-offs); it matters only for what the UI shows (below).

| | today | with target |
| --- | --- | --- |
| heat stops when | physical thermostat opens | physical opens **or** `Tmin >= T` |
| heat may resume when | physical closes | physical closed **and** `Tmin <= T - 3` |
| Manual mode after a stop | stays `OFF` | stays `OFF` |
| reboot | — | `T` restored from NVS, "reached" recomputed from live readings |

### Persistence

NVS through `Preferences`, same namespace `boiler` as the Manual flag, new
key `tgt` (u8). Read once in `boilerRelayInit()` with default 45, written only
when the value changes from the UI. 006 said "if a second value ever needs
persisting, design that separately": this is that value, and one u8 next to
one bool in the same namespace is still simple enough that a struct or a
version field would be over-engineering.

### API

- `POST /cmd` `{ "name": "set_boiler_target_temp", "value": 10..60 }`.
  Out-of-range → `bad_value`.
- `/status` (bare payload, the main page needs it) gains:
  - `tt` — target temperature [°C]
  - `tr` — `true` while the virtual thermostat reports "reached"

  `bt` cannot be used: `details.js` already reads it as the battery console
  temperature.

### Main page (`data/index.html`, `data/app.js`)

- **The `Boiler` section is removed** — the power buttons move to the settings
  page (below) and the power / thermostat state duplicated the flow diagram.
- New section **`Cílová teplota`** between the battery bar and the footer:
  - header dot + text with three states, derived from `tr` and `bo`:
    - `tr` → grey, `dosaženo`, note `teplota nad T°`
    - `!tr && !bo` → grey, `vypnul fyzický termostat`, note
      `spodní teplota X°, fyzický termostat je nastavený níž` — the case where
      the physical set point is below the target and needs adjusting on site
    - `!tr && bo` → green, `ohřívá` while power flows, `žádá teplo` while
      the regulation waits for surplus; note `spodní teplota X°, chybí N°`, or
      `teplota není k dispozici, hřeje po fyzický termostat` when a sensor is `null`
    - "reached" wins over the physical input: when both sensors are above the
      target it is irrelevant that the physical thermostat opened too.
  - big number: the target.
  - while either temperature is `null` the buttons are disabled (the stored
    target stays highlighted): changing the target has no effect until both
    readings are back, and a control that does nothing only confuses.
  - segmented control with 8 buttons `10 20 30 35 40 45 50 55`, one row,
    same look as the former power buttons. Each cell is tinted with the tank
    colour of its temperature (`tempColor()`, blue at 0 °C → red at 60 °C);
    the selected one is painted solid with white text and a `°` suffix.
  - one line below, only while Manual: `Ruční režim: regulace vypnutá, boiler
    drží výkon z nastavení.` (replaces today's `manualwarn`).
- The flow diagram keeps showing the effective heat request as today (tank
  edge, running wire, watts under the tank): `heating = !bf && bo && !tr &&
  bp > 0`.
- Mockup (private artifact, session of 2026-09-25):
  https://claude.ai/artifact/2AYfk87uqyyDoCLSaLQgA5

### Settings page (`data/settings.html`, `data/settings.js`)

- New section **`Boiler`** placed **above** `SoC guard`. It absorbs today's
  `Automatická regulace boileru` section (which moves up from below SoC guard)
  and adds the power buttons:
  - status pill + `Zapnout regulaci / Vypnout (ručně)` toggle, as today;
  - a row `Vyp / 500 / 1000 / 2000` in the page's plain button style, current
    power highlighted;
  - the power buttons are **enabled only in Manual** — in Auto the regulation
    overrides a click within a minute, so they would only confuse. They stay
    visible (greyed) so the level Auto currently holds is readable;
  - they are also disabled while `bf`, `!bo` or `tr` (nothing can heat), like
    the main page did; the mode toggle is never gated (Manual + OFF must be
    selectable while the thermostat is open, see 006);
  - helper text: `Vypnuto (ruční režim): drží zvolený výkon, dokud ho
    nezměníš. Vypne ho jen termostat, cílová teplota, prázdná baterie nebo
    zapnutí regulace.`
  - the page loads `/status?full=1` only on Refresh and after an action, so a
    power click re-fetches to show the resulting state.

### Details page (`data/details.js`)

Two rows in the boiler block: `Cílová teplota` (`tt` °C) and `Virtuální
termostat` (`tr` → `dosaženo` / `žádá teplo`).

### InfluxDB (`influx.cpp`)

`chajda-boiler` gains `target_temp` (int) and `target_reached` (bool), so the
history tells a virtual stop from a physical one. Grafana panel update is
optional and not part of this item.

### LCD

`ROW_TEMP` shows the target next to the two sensors, whole degrees only (the
tenths are on the web pages), with one marker character carrying the
thermostat state (the HD44780 charset has no Czech diacritics, so a marker
beats a mangled word):

```
T: 52/38 >45°C    heating allowed: target not reached, physical input on
T: 52/46 =45°C    reached: both sensors at or above the target
T: 52/38 x45°C    target not reached but the physical thermostat opened
T: --/38 >45°C    invalid sensor (NAN) — target out of action, heats to the physical thermostat
```

Layout `T: %s/%s %c%u°C`: sensors as `%2.0f` (`52`, ` 8`, `-2`), `--` when
invalid; 14 characters, the marker follows the same precedence as the main
page (`=` wins over `x`). `ROW_BOILER` is unchanged.

## Implementation

- `relay.h` / `relay.cpp`
  - `setBoilerTargetTemp(uint8_t)`, `getBoilerTargetTemp()`,
    `isBoilerTargetReached()`.
  - `static uint8_t boilerTargetTempC` (volatile, read from the status /
    Influx tasks), `static bool targetReached`, `static bool tempValid`
    (for the one-shot warnings).
  - `updateTargetReached(now)` called from `tickBoiler()` before the force-off
    chain, reading `g_temp_h` / `g_temp_l` from `inverter_comm.h` (already
    included). Logs `Boiler target 45 C reached (H 52.0 / L 45.3)` and
    `... below 42 C, heating allowed` via `printInfo()`; snapshots with
    `influx_log_event()` like a target change.
  - force-off reason `"target temperature reached"` inserted after
    `"boiler input off"`.
  - `setBoilerTargetTemp()`: range check, `printInfo("Boiler target temp 45
    -> 50 C (Web UI)")`, `influx_log_event()`, NVS write with the same
    open-failed warning pattern as `setBoilerManual()`, then an immediate
    re-evaluation.
  - `boilerRelayInit()`: read `tgt` with default 45 next to `manual`.
- `esp_webserver.cpp`: `set_boiler_target_temp` in `handleCommand()`; `tt`,
  `tr` in `makeStatusJson()` (bare part).
- `influx.cpp`: two fields on `chajda-boiler`.
- `data/index.html`, `data/app.js`: remove the Boiler section, `setBoiler()`
  and the button handling; add the target section and its render; keep
  `heating` for the diagram.
- `data/settings.html`, `data/settings.js`: merged Boiler section above SoC
  guard with the power buttons (`set_boiler` moves here); remove the old
  regulation section.
- `data/details.js`: two rows.
- `main.cpp`: `task_update_temperature()` prints the new `ROW_TEMP` layout;
  `format_temp_str()` switches to whole degrees (`%2.0f`, `--` when NAN) and
  the marker comes from `isBoilerTargetReached()` / `isBoilerOn()`.
- Web files change → LittleFS re-upload via `/upload` (see `CLAUDE.md`);
  firmware via `platformio remote run -t upload`, only after explicit go-ahead.

## Verification plan (after deploy)

1. `/status` shows `tt: 45`, `tr` matching `min(th, tl) >= 45`;
   `app.log` has no target line at boot beyond the NVS restore.
2. Main page: pick `50` → button turns solid, `app.log` gets
   `Boiler target temp 45 -> 50 C (Web UI)`; after a reboot `/status` still
   shows `tt: 50`.
3. With the tank warm, pick a target below both sensors → within one tick
   `app.log` has `Boiler target ... reached` and `Boiler target ... -> OFF
   (target temperature reached)`; the diagram shows the tank idle; the
   settings-page power buttons are disabled.
4. Raise the target above the sensors again → heating allowed line; in Auto
   the regulation steps up on its own, in Manual nothing restarts.
5. Set the target above the physical thermostat's set point and wait for it
   to open: the main page shows `vypnul fyzický termostat`, not `dosaženo`.
6. Unplug / short a sensor (`th` or `tl` = null) while "reached": within one
   tick `tr` drops to false and the boiler may heat again; one warning in
   `app.log`, an info line when it recovers, then "reached" recomputed.
7. `chajda-boiler.target_temp` and `target_reached` appear in InfluxDB.
8. LCD `ROW_TEMP` reads `T: 52/38 >45°C` and the marker follows steps 3–5
   (`=` when reached, `x` when only the physical thermostat opened).

## Resolution

_Not resolved yet._
