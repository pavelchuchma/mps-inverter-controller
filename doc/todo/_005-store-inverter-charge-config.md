---
id: 005
title: Read the inverter charge configuration every 5 minutes and store it
type: enhancement
status: done
priority: medium
component: inverter
created: 2026-09-13
resolved: 2026-09-14
---

# Read the inverter charge configuration every 5 minutes and store it

## Summary

Five inverter settings decide what the charger and the load transfer are
*allowed* to do:

| Setting | QPIRI token | Value on 2026-09-13 | Unit |
| --- | --- | --- | --- |
| Max charge current (Solar + grid) | `[14]` | 20 | A |
| Bulk / absorb voltage | `[10]` | 52.5 | V |
| Float voltage | `[11]` | 51.5 | V |
| Low DC cutoff (LVD) | `[9]` | 46.0 | V |
| Return to battery supply (SBU) | `[22]` | 48.0 | V |

They are the inverter-side counterpart of the limits the BMS publishes over CAN
(`ccl_a`, `charge_v` in `chajda-battery-can`, see
[`../battery_can_data_spec.md`](../battery_can_data_spec.md)). Any future work
that throttles charging to the BMS's current limit has to reason about both
numbers at once — "the BMS allows 20 A and the inverter is configured for 20 A"
is a different situation from "the BMS allows 20 A and the inverter is capped at
10 A", and today the second number is invisible unless somebody opens the
settings page and reads it by eye.

Wanted: the five values sampled every 5 minutes, stored in InfluxDB, and drawn
on the `chajda` dashboard next to the CAN limits.

## Current state

- The background poll task (`inverter_comm.cpp:297`) sends **only `QMOD` and
  `QPIGS`**, every `INVERTER_POLL_INTERVAL_MS` (3 s). Neither carries these
  settings.
- `QPIRI` reaches the wire **only on demand**: `/settings.html` calls
  `GET /inv_config`, which is `handleInvConfig()` (`esp_webserver.cpp:336`)
  issuing `inverter_query_raw("QPIRI", …)` (`inverter_comm.cpp:398`).
- **The firmware does not parse `QPIRI` at all.** It returns the raw payload and
  the whole token mapping lives in `data/settings.js` — the index table in the
  header comment (`settings.js:11-19`) and the row definitions
  (`settings.js:40`, `:70`, `:93`, `:97`, `:101`). That is deliberate; the comment at
  `settings.js:4` says the mapping lives there so it can be changed by
  re-uploading web files without reflashing, because the token order is
  model-dependent.
- Nothing stores any of it, so there is no history at all.

Reference payload read from the live inverter on 2026-09-13, 25 tokens:

```
220.0 25.0 220.0 50.0 23.9 5500 5500 48.0 48.0 46.0 52.5 51.5 2 02 020 0 2 1 6 01 0 0 48.0 0 1
                                               [9]  [10] [11]      [14]               [22]
```

## Proposed implementation

### 1. Parse the five tokens in firmware

New struct in `inverter_comm.h`, alongside `InverterState`:

```c
struct InverterConfig {
  float max_charge_a;    // QPIRI[14] - total charge current limit (solar + grid)
  float bulk_v;          // QPIRI[10]
  float float_v;         // QPIRI[11]
  float lvd_v;           // QPIRI[9]
  float redischarge_v;   // QPIRI[22] - "batt re-discharge V", the SBU return
                         // threshold: load goes back on the battery above this
  uint32_t ts_ms;        // millis() of the last successful read, 0 = never
};
bool inverter_get_config(InverterConfig* out);
```

Parse **only these five indices**, not the whole table. `settings.js` keeps
owning the full 25-token mapping for the UI; duplicating five indices into C++
is the price of storing them, and it is much cheaper than moving the whole table
into firmware and losing the "tweak without reflashing" property.

Validate like `parse_pwr_payload()` does for the console link
(`pylontech_comm.cpp:74`): a token that is not a clean number, or a value
outside a plausible window, rejects the read and keeps the previous values
rather than publishing a zero. Suggested windows: charge current 0–100 A,
bulk/float 40–60 V, LVD 35–50 V, re-discharge 44–60 V. Also require
`tokens >= 25` — a short payload means a truncated frame or a different model,
and indices 14 and 22 would then silently read something else.

### 2. Cadence: one QPIRI per 5 minutes, inside the existing task

Add a `last_config_ms` to `inverter_task()` and issue `QPIRI` when 5 minutes
have elapsed, in the same loop that already owns the serial port. Do **not** add
a second task: `send_command_and_get_payload()` serializes on
`g_inv_serial_mutex` anyway, and one more caller only adds contention.

The pause path needs no extra work — the task already skips the whole body while
`g_inv_paused` (`inverter_comm.cpp:302`), so a crosstalk measurement stays clean.

Do **not** clear the in-RAM values on pause the way `InverterState` is cleared:
they are a configuration, not a measurement, and the last known value is still
the best answer `/settings.html` can give while the link is muted. That is about
the RAM snapshot only — **it must not turn into writing that stale value to
InfluxDB**. The stored series records reads, not beliefs: one point per
successful `QPIRI`, nothing at all while the link is down. `g_inverter_data_valid`
is the wrong gate (it is cleared by a pause even though the configuration has not
become untrue), and so is "write every grid tick from the RAM copy" (that
manufactures data nobody read). See the write rule in section 3.

**Why 5 minutes.** These change only when somebody writes them — from the
settings page or the inverter's front panel — so the sampling rate is about how
quickly a change should show up, not about resolution. Against that, every QPIRI
puts another RS232 exchange on the shared 15 m cable that
[`../rj45_cable_wiring.md`](../rj45_cable_wiring.md) names as the first suspect
whenever CAN error counters climb. 5 minutes is 288 extra exchanges a day
against the ~28 800 `QMOD`+`QPIGS` pairs the 3 s poll already sends, i.e. a 1 %
increase in traffic — negligible, and worth confirming as such on the link
counters after it lands.

### 3. Store as `chajda-inverter-config`

A separate measurement, not extra fields on `chajda-inverter`: this is
configuration on a 5-minute cadence, while `chajda-inverter` is measurement on
the 10 s grid, and mixing them would leave most points with the config fields
missing.

Written from `append_sample()` in `influx.cpp` next to the existing blocks, but
**only when `ts_ms` differs from the one already written** — one point per
successful read, not one per grid tick. Consequences, and they are the intended
ones:

- a paused or dead link produces **no points**, so the series has a gap for
  exactly as long as the configuration went unconfirmed;
- the last point before the gap keeps the last value actually read, and the gap
  says "not re-read since" rather than asserting the setting stayed put;
- a read that returns the same values as the previous one still writes a point
  (`ts_ms` moved), which is what keeps the series dense enough to distinguish
  "unchanged" from "unknown".

| field | type | source |
| --- | --- | --- |
| `max_charge_a` | float | QPIRI[14] |
| `bulk_v` | float | QPIRI[10] |
| `float_v` | float | QPIRI[11] |
| `lvd_v` | float | QPIRI[9] |
| `redischarge_v` | float | QPIRI[22] |

### 4. Grafana

One panel in the existing "Baterie – CAN (BMS)" row, or a new "Měnič –
konfigurace" row if it grows:

- **Charge limits**: `chajda-inverter-config.max_charge_a` against
  `chajda-battery-can.ccl_a` and the actual `chajda-battery-can.current_a`. This
  is the panel the whole ticket exists for — it shows which of the two limits is
  binding at any moment.
- **Charge voltages**: `bulk_v`, `float_v`, `lvd_v`, `redischarge_v` against
  `chajda-battery-can.charge_v` (CVL) and the measured pack voltage. `lvd_v` and
  `redischarge_v` are the bottom pair — cutoff and the threshold the load comes
  back at — so the gap between them and the measured voltage is the real
  discharge headroom, which is what a deep-winter evening eats into.

Both are step-shaped series: set `lineInterpolation: stepAfter` and `fn: last`
in `aggregateWindow` — a `mean` across a window in which the setting changed
would invent a value that was never configured.

**These two panels must not inherit the dashboard-wide gap settings**, and this
is the one place on the dashboard where that is true. Everything else samples
every 10 s, so `createEmpty: true` + `spanNulls: false` puts roughly one value
in every aggregation window (~20-30 s at the default 6 h range) and a break
means real missing data. A 5-minute series on that same ~25 s grid has a value
in about **one window in twelve**; with `createEmpty: true` the other eleven are
null and `spanNulls: false` breaks at every one of them, so the panel degrades
into isolated dots and a genuine outage becomes indistinguishable from the
normal sampling pattern.

Use instead:

| option | value | why |
| --- | --- | --- |
| `createEmpty` | `false` | do not manufacture nulls between 5-minute samples |
| `spanNulls` | `900000` (15 min) | bridge normal spacing plus two missed reads |
| `lineInterpolation` | `stepAfter` | a setting holds its value until it is changed |

This is also the one series where a fixed `spanNulls` threshold is safe. The
objection to thresholds elsewhere is that `v.windowPeriod` scales with the zoom
level, so a fixed one eventually falls below the point spacing and shatters the
graph. Here the spacing is pinned at 5 minutes by the poll cadence, independent
of zoom, so a 15-minute threshold keeps meaning the same thing at every range.

## Open questions

- **Token order is model-dependent.** The indices above are confirmed against
  this inverter's live payload and against `settings.js`, but the firmware
  parser should be verified against the raw `/inv_config` dump once after it
  lands, not trusted from the code.
- **`QPIGS` already reports a "configuration changed" bit** — device status
  `b6`, "1 changed / 0 unchanged" (`ps_rs232_protocol_FULL_ai_ready.txt:169`),
  parsed today into `InverterState.device_status_bits`. If that bit behaves
  usefully it is a better trigger than a blind timer: re-read `QPIRI`
  immediately when it flips and a settings change shows up in seconds instead of
  up to 5 minutes. Unknown whether it latches until acknowledged or self-clears,
  so this needs observing before anything depends on it. The 5-minute poll
  should land first and stay as the floor either way.
- **Re-read after a write.** `POST /inv_set` changes these values;
  `handleInvSet()` (`esp_webserver.cpp:421`) should trigger a config re-read so
  the stored series records the change at its real time rather than up to 5
  minutes later.
- **`QMCHGCR`** (the list of charge-current values the inverter accepts) is
  fetched by `/inv_config` too, but it is a capability list, not a setting.
  Nothing to store.

## First deploy panicked — what it cost and what changed

The first build of this (commit `b3e8fda`, flashed 2026-09-13 23:45) put the
device into a **reboot loop**: `ESP_RST_PANIC` every ~3 seconds, 24 boots
between 23:45:54 and 23:47:08, no data reaching InfluxDB and no `[INV] config`
line ever written. Recovered by reflashing the previous firmware over the
PlatformIO serial agent.

Cause: the parser split the payload into `String toks[32]` on `inverter_task`,
whose stack is 4096 B and which already spends ~1 kB of it on the
`String toks[64]` inside `parse_qpigs_payload()`. Three seconds after boot is
exactly when the task's first iteration reaches QPIRI.

Three changes came out of it, all in `b3e8fda`'s successor:

- **The parser allocates per token, not per payload.** `token_count()` /
  `token_at()` walk the string and build one `String` per field read.
  `parse_qpigs_payload()` still uses the array and is left alone, but nothing
  new should copy that pattern.
- **`inverter_task` stack 4096 → 6144.** The parser no longer needs it, but the
  task had no headroom to absorb *anything*, which is what made a routine
  addition fatal.
- **The first QPIRI waits `INVERTER_CONFIG_FIRST_DELAY_MS` (60 s) after boot.**
  This is the part worth keeping as a habit: a fault in code that runs three
  seconds into boot costs remote access entirely, because the device never stays
  up long enough to serve `/app.log` or accept a new image over the network. Had
  the serial agent not been reachable, this would have needed a drive to the
  site. New code paths on a device that cannot be touched should run *after* the
  device is reachable, not before.

The lesson generalises past this ticket: there is no runtime visibility of stack
headroom anywhere in this firmware, so every task's margin is an assumption. The
first-read log line now prints `uxTaskGetStackHighWaterMark()` for
`inverter_task`; doing the same for the other tasks would turn the next overflow
into a warning rather than a reboot loop.

## Out of scope

The expected values in `settings.js` (rows `26`, `27`, `29`) are still the ones
from the manual for the **old VRLA bank** — 55.2 / 54.5 / 46.5 V — while the
inverter is really configured at 52.5 / 51.5 / 46.0 V, which matches the
Pylontech pack (CAN reports CVL 52.8 V). Row `13` (SBU return) likewise expects
50 V against an actual 48.0 V. The settings page therefore flags those rows as
mismatches against a target that no longer applies. Real, but a separate piece of
work; do not fold it into this one.

## Resolution

Closed **2026-09-14**. Implemented in `b3e8fda` and fixed in `8618994` (the
stack overflow described above); the Grafana panels landed with the dashboard in
`454d4f7`.

The open question — "verify the parser against the raw dump once after it lands"
— is answered. Read back from the live inverter on 2026-09-14:

```
/inv_config  220.0 25.0 220.0 50.0 23.9 5500 5500 48.0 48.0 46.0 52.5 51.5 2 02 020 0 2 1 6 01 0 0 48.0 0 1
app.log      [INV] config: chg 20 A  bulk 52.5 V  float 51.5 V  LVD 46.0 V  SBU 48.0 V  (task stack free 3672 B)
```

All five indices (9, 10, 11, 14, 22) map to the values the firmware published,
so the token order taken from `settings.js` is confirmed against this model.
`inverter_task` reports 3672 B of stack headroom after the read, i.e. the
6144 B stack is right.

Not done, and deliberately not folded in here:

- **Re-read `QPIRI` after `POST /inv_set`**, so a write shows up in the series
  at its real time instead of up to 5 minutes later.
- **Trigger on the `b6` "settings changed" status bit** instead of the blind
  timer. Both were listed above as later work and still are; neither blocks
  anything that exists today.
- The `settings.js` expected values still describing the **old VRLA bank** — see
  "Out of scope" above. Still real, still separate.
