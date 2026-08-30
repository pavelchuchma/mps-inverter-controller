# Pylontech Battery Communication – Specification

## Overview

The controller reads telemetry from a **Pylontech US5000** battery over its
**console (debug) port** using the text-based `pwr` console command rather than
the binary RS-485 BMS protocol. Communication runs in a dedicated FreeRTOS
background task on the ESP32.

The console link shares a long RJ45 cable with two other links and is not
error-free (see [Link reliability](#link-reliability)), so the reader is built
around the assumption that individual bytes get corrupted. Three independent
layers — end sentinel, per-field validation, and consensus voting — keep bad
data from ever reaching the rest of the firmware.

Source files:
- `include/pylontech_comm.h` — public API, `PylontechState`, timing/protocol constants
- `src/pylontech_comm.cpp` — UART setup, polling task, parser, consensus logic
- `src/main.cpp` — calls `pylontech_comm_init()` during setup

## Wiring

The battery console port connects to the ESP32 via **UART2 (`Serial2`)**.

| Signal | ESP32 pin | Notes |
|---|---|---|
| Battery TX → ESP32 RX | GPIO 39 (`SVN`) | Input-only pin — sufficient for RX |
| ESP32 TX → Battery RX | GPIO 32 | Used to send the `pwr` command |
| GND | GND | Common ground required |

Pins are defined in `include/config.h` (`BATTERY_RX_PIN`, `BATTERY_TX_PIN`)
and passed to `pylontech_comm_init(rx_pin, tx_pin)`.

The battery's `Console` connector carries true RS232 levels, not logic levels,
so a MAX3232 sits between it and the ESP32. Connector pinout and the shared
RJ45 link cable: [`rj45_cable_wiring.md`](rj45_cable_wiring.md).

## Serial parameters

| Parameter | Value | Constant |
|---|---|---|
| Baud rate | 115200 | `PYLONTECH_BAUD` |
| Framing | 8N1 | `SERIAL_8N1` |
| Command | `pwr 1\r` | `PYLONTECH_CMD` (CR-terminated) |

Only pack 1 is polled. `pwr` without an index would list every pack, which the
parser does not handle — see [Known gaps](#known-gaps).

## Polling cycle

A background task (`pylontech_task`, pinned to core 1, 4 KB stack, priority 1)
repeats the following loop:

1. Read `pwr` **back-to-back, with no delay between attempts**, until
   `PYLONTECH_CONSENSUS_COUNT` consecutive frames agree or
   `PYLONTECH_MAX_ATTEMPTS` is exhausted.
2. On consensus: publish the accepted state under the mutex, set
   `g_pylontech_data_valid = true`, reset the consecutive-failure counter.
3. On no consensus: log one `printWarning` line and increment the
   consecutive-failure counter; invalidate the data only once it reaches
   `PYLONTECH_FAIL_INVALIDATE_THRESHOLD`.
4. Print a status snapshot to Serial.
5. Sleep `PYLONTECH_POLL_INTERVAL_MS` before the next cycle.

### Layer 1 — frame collection

`pylontech_collect_response()` flushes TX, discards stale RX bytes, sends the
command, then accumulates bytes until it sees an **end-of-response sentinel**:
either the string `Command completed successfully` or a trailing `$$` prompt.

It deliberately does **not** stop on an idle gap. The console can pause
mid-frame, and an early stop would hand the parser a truncated response, which
would then publish zero-defaulted fields. A window expiry without the sentinel
returns failure, so the caller keeps the last good values instead.

`PYLONTECH_READ_WINDOW_MS` (300 ms) only bounds reads that never see the
sentinel — a successful read returns the instant the sentinel arrives, so a
larger window would not slow the common path. 300 ms is roughly 2× the worst
observed frame time and bounds the dead-line case to
`MAX_ATTEMPTS × 300 ms = 3 s`, comfortably inside one 5 s poll interval.

### Layer 2 — field validation

`parse_pwr_payload()` walks the response line by line, splits each on `:`, and
takes the first whitespace-separated token of the right-hand side as the value.

Every numeric field must be a **clean integer token** (`is_int_token()`:
optional `-` then digits only). This matters because `String::toInt()` silently
turns a corrupted token such as `mV` into `0`. Where a zero or garbage value is
physically impossible, the value must additionally fall inside a plausible
range:

| Field | Token check | Range check |
|---|---|---|
| `Voltage` | yes | 40000 … 60000 mV |
| `Max Voltage` | yes | 45000 … 60000 mV |
| `Temperature` | yes | −30000 … 80000 mC |
| `Coulomb` (SoC) | yes | 0 … 100 % |
| `Current` | yes | none (signed, wide) |
| `Total Coulomb` | yes | none |
| `Charge Times` | yes | none |
| `Bat/Power Events`, `System Fault/Alarm` | **none** — `strtoul()` | none |
| `Basic Status` | **none** — copied as string | none |
| `CFetState`, `DFetState`, `Heater Status` | **none** — compared to `ON` | none |

A single bad field rejects the **whole frame**. A frame is also rejected if it
carries no `Voltage` or no `Coulomb` line at all, which is what a corrupted
*label* looks like. Rejections go to Serial only, not to the persistent app log:
with consensus re-reads a stray bad frame is expected and self-healing, and
logging it would flood the log. Only a whole failed cycle is persisted.

The `Protect ENA` line and the `*. Status: Normal` lines are intentionally
ignored.

### Layer 3 — consensus voting

Validation cannot catch a corrupted byte that lands inside a plausible value
(`49806` → `49306` is still a valid pack voltage), and the fields marked *none*
above are not validated at all. Consensus covers that gap: a value is accepted
only once **`PYLONTECH_CONSENSUS_COUNT` consecutive frames agree**, on the
assumption that random corruption will not reproduce identically that many times
in a row. A failed read or a mismatch resets the streak.

`states_match()` compares the analog fields that legitimately jitter between
back-to-back reads within a tolerance, and **everything else exactly** — SoC,
`Basic Status`, counters, and all four event bitmasks — so that a corrupted byte
anywhere in them breaks the consensus:

| Field | Comparison |
|---|---|
| `voltage`, `max_voltage` | ± `PYLONTECH_VOLTAGE_TOL` (0.02 V) |
| `current` | ± `PYLONTECH_CURRENT_TOL` (0.5 A) |
| `temperature` | ± `PYLONTECH_TEMP_TOL` (0.2 °C) |
| everything else | exact |

### Layer 4 — dropout tolerance

A cycle that exhausts all attempts without consensus does **not** invalidate the
data. `g_pylontech_data_valid` is cleared only after
`PYLONTECH_FAIL_INVALIDATE_THRESHOLD` consecutive failed cycles, so an occasional
single dropout does not ripple into the consumers — most importantly the boiler
relay chain, which forces itself off when battery data is invalid
(`src/relay.cpp`).

### Constants

| Constant | Value | Meaning |
|---|---|---|
| `PYLONTECH_POLL_INTERVAL_MS` | 5000 ms | Delay between poll cycles |
| `PYLONTECH_READ_WINDOW_MS` | 300 ms | Max wait for the end sentinel of one read |
| `PYLONTECH_CONSENSUS_COUNT` | 3 | Consecutive matching frames required |
| `PYLONTECH_MAX_ATTEMPTS` | 10 | Reads per cycle before giving up |
| `PYLONTECH_VOLTAGE_TOL` | 0.02 V | Match tolerance, pack and max voltage |
| `PYLONTECH_CURRENT_TOL` | 0.5 A | Match tolerance, current |
| `PYLONTECH_TEMP_TOL` | 0.2 °C | Match tolerance, temperature |
| `PYLONTECH_FAIL_INVALIDATE_THRESHOLD` | 3 | Failed cycles before data goes invalid |

## Public API

```c
// Parsed battery status, values in natural units (V / A / °C / % / mAH).
struct PylontechState {
  bool     cfet_on, dfet_on, heater_on;
  int      pack_index;
  float    voltage, current, temperature, max_voltage;
  int      soc, total_capacity_mah, charge_times;
  char     basic_status[12];
  uint32_t bat_events, power_events, system_fault, system_alarm;
  uint32_t ts_ms;              // millis() when these values were last updated
};

// Initialize the battery console UART (Serial2) and start the polling task.
void pylontech_comm_init(int rx_pin, int tx_pin);

// Thread-safe snapshot copy of the latest battery status.
bool pylontech_get_status(PylontechState* out);

// Thread-safe read of the battery data validity flag.
bool pylontech_data_valid();
```

`current` is signed: **positive = charging, negative = discharging**. Both
accessors take an internal mutex; the raw globals `g_pylontech_status` and
`g_pylontech_data_valid` are exported for convenience but are not synchronized.

Called once from `setup()` in `main.cpp`:

```c
pylontech_comm_init(BATTERY_RX_PIN, BATTERY_TX_PIN);
```

## Consumers

Battery SoC, voltage and current come from this link, **not** from the inverter,
and each consumer honours the validity flag independently:

| Consumer | Uses | Behaviour when invalid |
|---|---|---|
| `src/main.cpp` — LCD | `ROW_SOC`, `ROW_BATT_POWER` (`voltage × current`) | shows `--` |
| `src/influx.cpp` | `chajda-battery` measurement, all parsed fields | writes nothing → gap in Grafana instead of zeros |
| `src/esp_webserver.cpp` | JSON keys `bv`, `bc`, `bs`, `bm`, `bav` | `bav: false` for the web UI |
| `src/relay.cpp` | boiler auto-regulation input | forces the relay chain off (`"battery data invalid"`) |

## Link reliability

The console link runs 115200 baud at RS232 levels down a long RJ45 cable shared
with the inverter RS232 link and the battery CAN pair
([`rj45_cable_wiring.md`](rj45_cable_wiring.md)). It sees roughly **one
corrupted byte per 10 kB**, which is the reason the whole validate-and-vote
scheme exists.

Baseline measured over a 3.5-minute serial capture with the battery in a steady
state (49.80 V, −1.13 A, 20.3 °C, 96 % throughout):

| Metric | Value |
|---|---|
| Poll cycles | 42 |
| `pwr` reads issued | 157 (minimum possible: 126) |
| Cycles resolved in the minimum 3 reads | 31 / 42 (74 %) |
| Cycles needing retries | 11 / 42, worst case 8 reads |
| Frames rejected by the parser | 5 (3 corrupted values, 2 corrupted labels) |
| Cycles that failed entirely | **0** |
| Reads that hit the 300 ms window | **0** |
| Response time | 108–135 ms typical, 2 outliers at ~280 ms |
| Frame size | 926 B; 10 reads came in at 931–932 B — extra echo/prompt bytes on the first read after the idle gap, not corruption |

Useful as a comparison point: a marked rise in retries per cycle, or any
`no consensus` / `no response` line, points at the cable or the connectors
rather than at the firmware.

### Log lines

| Line | Level | Meaning |
|---|---|---|
| `[BAT] response: N bytes in T ms` | Serial | One complete frame received |
| `[BAT] rejected frame: bad field "L" value "V"` | Serial | Validation caught a corrupted field |
| `[BAT] failed to parse pwr response` | Serial | Frame rejected (bad field, or missing Voltage/SoC) |
| `[BAT] no response from battery console (waited T ms)` | Serial | Read window expired without a sentinel |
| `[BAT] no consensus after N attempts (M bad frames)` | app log | Whole cycle failed |

## `pwr` response format

Example raw response to `pwr 1` (one pack), with the per-line serial timestamps
stripped:

```
 CFetState:ON
 DFetState:ON
 ----------------------------
 Power  1

 Voltage         : 49280       mV
 Current         : 0           mA
 Temperature     : 16600       mC
 Coulomb         : 58          %
 Total Coulomb   : 100000      mAH
 Max Voltage     : 54000       mV
 Charge Times    : 0
 Basic Status    : Idle
 Volt Status     : Normal
 Current Status  : Normal
 Tmpr. Status    : Normal
 Coul. Status    : Normal
 Soh. Status     : Normal
 Heater Status   : OFF
 Protect ENA     : BOV BHV BLV BUV POV PHV PLV PUV CBOT CBHT CBLT CBUT DBOT DBHT DBLT DBUT POT PHT COC COC2 COCA DOCA DOC DOC2 SC LCOUL
 Bat Events      : 0x0
 Power Events    : 0x0
 System Fault    : 0x0
 System Alarm    : 0x0
 ----------------------------
Command completed successfully
$$
```

### Field reference

| Field | Example | Unit | Meaning | Parsed into |
|---|---|---|---|---|
| `CFetState` | `ON` | — | Charge FET state | `cfet_on` |
| `DFetState` | `ON` | — | Discharge FET state | `dfet_on` |
| `Power` | `1` | — | Pack index this block describes | `pack_index` |
| `Voltage` | `49280` | mV | Pack voltage (≈ 49.28 V) | `voltage` (V) |
| `Current` | `0` | mA | Pack current; + charge / − discharge | `current` (A) |
| `Temperature` | `16600` | mC | Temperature in milli-°C | `temperature` (°C) |
| `Coulomb` | `58` | % | State of Charge | `soc` |
| `Total Coulomb` | `100000` | mAH | Total/rated capacity | `total_capacity_mah` |
| `Max Voltage` | `54000` | mV | Max charge voltage | `max_voltage` (V) |
| `Charge Times` | `0` | — | Cumulative charge cycle count | `charge_times` |
| `Basic Status` | `Idle` | — | Idle / Charge / Dischg | `basic_status` |
| `Volt Status` | `Normal` | — | Voltage status | *ignored* |
| `Current Status` | `Normal` | — | Current status | *ignored* |
| `Tmpr. Status` | `Normal` | — | Temperature status | *ignored* |
| `Coul. Status` | `Normal` | — | Coulomb/SoC status | *ignored* |
| `Soh. Status` | `Normal` | — | State of Health status | *ignored* |
| `Heater Status` | `OFF` | — | Internal heater state | `heater_on` |
| `Protect ENA` | `BOV …` | — | Enabled protection flags | *ignored* |
| `Bat Events` | `0x0` | hex | Battery event bitmask | `bat_events` |
| `Power Events` | `0x0` | hex | Power event bitmask | `power_events` |
| `System Fault` | `0x0` | hex | System fault bitmask | `system_fault` |
| `System Alarm` | `0x0` | hex | System alarm bitmask | `system_alarm` |

### Parsing notes

- The command echo (`pwr 1`) is echoed back before the data block.
- The `Power N` pack header has **no colon**, so it is special-cased.
- Field lines follow `<label> : <value> <unit>` with variable whitespace —
  split on `:` and then on whitespace rather than on fixed columns.
- Each pack is delimited by a `----------------------------` rule.
- The response terminates with `Command completed successfully` followed by the
  `$$` console prompt; either is accepted as the end sentinel.
- All numeric values are integers in milli-units (mV / mA / mC / mAH) except
  `Coulomb` (% SoC) and `Charge Times` (count).
- `Basic Status` reports `Dischg`, not `Discharge`, while discharging.

## Console command reference (`help`)

Output of the `help` command on the US5000 console port. These are the
commands available in **user mode**; `login` switches to admin mode (unlocking
commands marked with `!`).

```
bat      Battery data show - bat [pwr][index]
data     History data load - data [event/history/pov/puv/plv/bov/buv/inputov/coc/coca/doc/doca/sc/use][item]
datalist Show recorded data - datalist [event/history/pov/puv/plv/bov/buv/inputov/coc/coca/doc/doca/sc/use][item/bat][batnun][volt/curr/temp/coul][item]
disp     Display Info at regular intervals - disp [(pwrs pwrNo)/val]/[(bats batNo)/volt/curr/temp]
getpwr   Get power Info - getpwr
help     Help [cmd]
info     Device infomation - info
log      Log information show - log
login   !Login Admin mode - login [password]
logout   user mode  - logout
pwr      Power data show - pwr [index]
shut     Shut down - shut
stat     Statistic data show - stat
time     Time - time [year] [month] [day] [hour] [minute] [second]
cmudtesttime Device Test Time - cmudtesttime [year] [month] [day] [hour] [minute] [second]
trst     Test Soft Reset - trst
topen    open wire show
```

Commands of interest for telemetry:

| Command | Purpose |
|---|---|
| `pwr` | Power data per pack (currently polled by the firmware) |
| `bat` | Per-battery cell data — `bat [pwr][index]` |
| `getpwr` | Aggregated power info |
| `info` | Device information (model, firmware, serial) |
| `stat` | Statistics |

## Known gaps

- **Multi-pack output is not handled.** The command is hardcoded to `pwr 1`;
  a bare `pwr` would emit one block per pack and the parser would let later
  blocks overwrite earlier ones.
- **Event bitmasks are parsed but not acted on.** `bat_events`, `power_events`,
  `system_fault` and `system_alarm` are forwarded to InfluxDB, but nothing
  decodes them or raises an alert.
- **Some fields bypass validation entirely** (see the table in
  [Layer 2](#layer-2--field-validation)): the four event bitmasks go through
  `strtoul()`, and `Basic Status` is copied verbatim. Consensus is the only
  thing standing between a corrupted byte there and InfluxDB, so lowering
  `PYLONTECH_CONSENSUS_COUNT` would open a real hole.
- **Per-cell data is not read.** The `bat` command exposes per-cell voltages and
  temperatures; the firmware does not poll it.
