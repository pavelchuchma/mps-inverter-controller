# Pylontech Battery Communication – Specification

## Overview

The controller reads telemetry from a **Pylontech US5000** battery over its
**console (debug) port** using the text-based `pwr` console command rather than
the binary RS-485 BMS protocol. Communication runs in a dedicated FreeRTOS
background task on the ESP32.

> **Status:** First iteration only — the task verifies connectivity by sending
> the `pwr` command periodically and dumping the **raw** console response to the
> serial log. No response parsing is implemented yet.

Source files:
- `include/pylontech_comm.h` — public API and timing/protocol constants
- `src/pylontech_comm.cpp` — UART setup, polling task, raw dump
- `src/main.cpp` — calls `pylontech_comm_init()` during setup

## Wiring

The battery console port connects to the ESP32 via **UART2 (`Serial2`)**.

| Signal      | ESP32 pin | Notes                                  |
|-------------|-----------|----------------------------------------|
| Battery TX → ESP32 RX | GPIO 39 (`SVN`) | Input-only pin — sufficient for RX |
| ESP32 TX → Battery RX | GPIO 32         | Used to send the `pwr` command     |
| GND         | GND       | Common ground required                 |

Pins are defined in `include/config.h` (`BATTERY_RX_PIN`, `BATTERY_TX_PIN`)
and passed to `pylontech_comm_init(rx_pin, tx_pin)`.

> The console port operates at logic levels; check the battery's console
> connector pinout and level requirements before wiring directly to the ESP32.

## Serial parameters

| Parameter | Value      | Constant                |
|-----------|------------|-------------------------|
| Baud rate | 115200     | `PYLONTECH_BAUD`        |
| Framing   | 8N1        | `SERIAL_8N1`            |
| Command   | `pwr 1\r`  | `PYLONTECH_CMD` (CR-terminated) |

## Polling cycle

A background task (`pylontech_task`, pinned to core 1, 4 KB stack) repeats the
following loop:

1. Wait `PYLONTECH_POLL_INTERVAL_MS` (5000 ms) between cycles.
2. Flush the TX buffer and discard any stale RX bytes.
3. Send the `pwr 1\r` command.
4. Collect the response within a read window, echoing each received byte to the
   main serial console between `[BAT] --- pwr response begin ---` and
   `[BAT] --- pwr response end ---` markers.
5. If no bytes arrive at all, log `[BAT] no response from battery console`.

### Response collection timing

| Constant                    | Value   | Meaning                                            |
|-----------------------------|---------|----------------------------------------------------|
| `PYLONTECH_READ_WINDOW_MS`  | 2000 ms | Maximum total time to collect one response         |
| `PYLONTECH_IDLE_GAP_MS`     | 300 ms  | Once data has started, stop after this quiet gap   |

The reader stops as soon as either the idle gap elapses after the last received
byte (normal case) or the overall read window expires (fallback).

## Public API

```c
// Initialize battery console UART (Serial2) and start the background polling task.
// First iteration only: sends 'pwr' periodically and dumps the raw response
// to the serial console, no parsing.
void pylontech_comm_init(int rx_pin, int tx_pin);
```

Called once from `setup()` in `main.cpp`:

```c
pylontech_comm_init(BATTERY_RX_PIN, BATTERY_TX_PIN);
```

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

| Field           | Example   | Unit | Meaning                                              |
|-----------------|-----------|------|------------------------------------------------------|
| `CFetState`     | `ON`      | —    | Charge FET state (ON/OFF)                            |
| `DFetState`     | `ON`      | —    | Discharge FET state (ON/OFF)                         |
| `Power`         | `1`       | —    | Pack index this block describes                     |
| `Voltage`       | `49280`   | mV   | Pack voltage (≈ 49.28 V)                            |
| `Current`       | `0`       | mA   | Pack current; signed (+ charge / − discharge)       |
| `Temperature`   | `16600`   | mC   | Temperature in milli-°C (≈ 16.6 °C)                 |
| `Coulomb`       | `58`      | %    | State of Charge (SoC)                               |
| `Total Coulomb` | `100000`  | mAH  | Total/rated capacity                                |
| `Max Voltage`   | `54000`   | mV   | Max charge voltage (≈ 54.0 V)                       |
| `Charge Times`  | `0`       | —    | Cumulative charge cycle count                       |
| `Basic Status`  | `Idle`    | —    | Idle / Charge / Discharge                           |
| `Volt Status`   | `Normal`  | —    | Voltage status                                      |
| `Current Status`| `Normal`  | —    | Current status                                      |
| `Tmpr. Status`  | `Normal`  | —    | Temperature status                                  |
| `Coul. Status`  | `Normal`  | —    | Coulomb/SoC status                                  |
| `Soh. Status`   | `Normal`  | —    | State of Health status                              |
| `Heater Status` | `OFF`     | —    | Internal heater state                               |
| `Protect ENA`   | `BOV …`   | —    | Enabled protection flags (space-separated list)     |
| `Bat Events`    | `0x0`     | hex  | Battery event bitmask (0 = none)                    |
| `Power Events`  | `0x0`     | hex  | Power event bitmask                                 |
| `System Fault`  | `0x0`     | hex  | System fault bitmask                                |
| `System Alarm`  | `0x0`     | hex  | System alarm bitmask                                |

### Parsing notes

- The command echo (`pwr 1`) is echoed back before the data block.
- Each pack is delimited by a `----------------------------` rule; `pwr` with no
  index lists all packs, each in its own block.
- Field lines follow `<label> : <value> <unit>` with variable whitespace —
  split on `:` and then on whitespace rather than on fixed columns.
- The response terminates with `Command completed successfully` followed by the
  `$$` console prompt. These two markers (or the closing rule) are reliable
  end-of-response sentinels, more robust than the current idle-gap timeout.
- All numeric values are integers in milli-units (mV / mA / mC / mAH) except
  `Coulomb` (% SoC) and `Charge Times` (count).

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

| Command  | Purpose                                                        |
|----------|----------------------------------------------------------------|
| `pwr`    | Power data per pack (currently polled by the firmware)         |
| `bat`    | Per-battery cell data — `bat [pwr][index]`                     |
| `getpwr` | Aggregated power info                                          |
| `info`   | Device information (model, firmware, serial)                  |
| `stat`   | Statistics                                                     |

## Next steps (not yet implemented)

- Parse the `pwr` response into structured fields (voltage, current, SoC,
  per-cell data, temperatures).
- Expose parsed values to the rest of the firmware (display rows
  `ROW_BATT_POWER`, SoC, charge/discharge power are currently sourced from the
  inverter, not the battery console).
- Handle multi-pack output (`pwr` lists all packs) and error/timeout recovery.
