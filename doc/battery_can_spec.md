# Battery CAN Link – Specification (planned)

## Overview

A second, independent link to the **Pylontech US5000** over its **CAN port**,
in addition to the existing console-port link described in
[`pylontech_comm_spec.md`](pylontech_comm_spec.md).

The battery's CAN port is currently **unused** — the installed inverter is not
compatible with the Pylontech BMS protocol, so nothing is connected to it. That
makes the port available for the ESP32.

> **Status:** Not implemented. Hardware (TJA1050 transceiver module) ordered,
> wiring and firmware to follow. Everything below marked *verify* must be
> confirmed against the real hardware during bring-up.

### Why add CAN

- **`0x351` carries CCL** (charge current limit), which the BMS lowers when the
  pack is nearly full, when it is cold, or when a protection is active. This is
  a direct, leading signal that the battery is about to stop absorbing PV power
  — exactly the condition the boiler dump-load logic in `relay.cpp` currently
  infers indirectly from inverter behaviour.
- Data arrives **unsolicited every ~1 s** instead of a 5 s poll cycle.
- CAN has a 15-bit CRC, bit stuffing and hardware retransmission, so corrupted
  frames never reach the application. The console port's consensus-voting
  workaround (`PYLONTECH_CONSENSUS_COUNT`) has no CAN equivalent and is not
  needed.

### Why keep the console port

CAN does **not** carry `heater_on`, `charge_times`, `total_capacity_mah`, exact
`cfet_on`/`dfet_on`, per-pack data, or the full 32-bit vendor event bitmasks.
All of those except `total_capacity_mah` and per-pack data are published to
InfluxDB from the console link (`influx.cpp:114-123`) and would be lost. Both links therefore run in parallel — see
[Field coverage](#field-coverage-vs-console-port).

## Topology

The ESP32 will be the **only other node** on the bus. Two consequences:

1. **The driver must run in `TWAI_MODE_NORMAL`, not listen-only.** CAN requires
   an ACK bit from another node. In listen-only mode the battery would receive
   no acknowledgement for any frame, retransmit indefinitely and drop into
   error-passive. In normal mode the ESP32 acknowledges even though it transmits
   nothing of its own. The ACK is a bit-layer function of the controller — it
   requires no application code beyond selecting the mode.
2. **Both bus ends need 120 Ω.** The transceiver module carries one on board
   (keep it). *Verify* whether the battery terminates internally: measure
   between RJ45 pins 4 and 5 with the battery **switched off** — 120 Ω means it
   does, open circuit means a second 120 Ω must be added at the battery end.

If the ESP32 is powered down while the battery keeps transmitting, the battery
goes error-passive and recovers by itself once the ESP32 returns. No harm.

## Hardware

### Transceiver: TJA1050

Chosen over MCP2551. Rationale:

| | MCP2551 | **TJA1050** |
|---|---|---|
| `TXD` VIH at 5 V | 0.7·VCC = 3.5 V ✗ | **2.0 V** ✓ |
| Extra parts for a 3.3 V MCU | HCT buffer + divider | **divider only** |

TJA1050 key figures (NXP datasheet, static characteristics):

| Parameter | Value | Consequence |
|---|---|---|
| `VCC` | 4.75 – 5.25 V | **must be powered from 5 V**, not 3.3 V |
| `TXD` VIH min | 2.0 V | ESP32's 3.3 V drives it directly, no buffer |
| `TXD` IIL | −100 … −300 µA at 0 V | internal pull-up ≈ 17–50 kΩ to VCC — see GPIO12 note |
| `RXD` | push-pull, IOH −2 … −15 mA | swings to ≈ 5 V — **must not reach a GPIO directly** |
| pin `S` | high-speed is default when unconnected | must not be tied to VCC — see below |

### Checks before wiring the module

1. **Pin S (package pin 8) must be at GND or unconnected.** If the board ties it
   to VCC the transceiver is in *silent mode* with the transmitter disabled,
   which means **no ACK** — fatal here, since we are the only other node. Fix by
   rerouting to GND. Measure pin 8 → GND and pin 8 → VCC.
2. **120 Ω present between CANH and CANL** on the module.
3. **VCC wired to 5 V**, not 3.3 V.

### Wiring

```
module VCC  → 5 V (VIN)
module GND  → GND
module TXD  ← GPIO12,  plus 2.2 kΩ from GPIO12 to GND
module RXD  → 4.7 kΩ → GPIO0,  plus 10 kΩ from GPIO0 to GND
module CANH → battery RJ45 pin 4      (verify against the US5000 manual)
module CANL → battery RJ45 pin 5      (verify)
```

Ground is already common with the battery through the console port.

### Pin assignment rationale

Only GPIO0, GPIO2 and GPIO12 are free, and all three are strapping pins. Both
CAN signals sit **high** at boot (the bus idles recessive and TJA1050's `TXD`
has an internal pull-up), so the choice is constrained:

| Pin | High at boot means | Verdict |
|---|---|---|
| GPIO12 | flash voltage selected as 1.8 V → **board does not boot** | usable only with an external pull-down |
| GPIO2 | normal boot fine; UART download mode blocked, and nothing on the board pulls it low → **flashing breaks permanently** | avoid |
| GPIO0 | normal boot fine; download mode needs it low, but the auto-reset circuit actively drives it | usable |

Hence:

- **`TWAI_TX = GPIO12` + 2.2 kΩ pull-down.** TJA1050's internal pull-up
  (≈ 17–50 kΩ to 5 V) overrides the ESP32's internal pull-down (≈ 45 kΩ). With
  2.2 kΩ the pin settles at ≈ 0.2–0.6 V, below `VIL` = 0.25·VDD = 0.825 V. While
  the driver is running, the ESP32 sources ≈ 1.5 mA into it, which is fine.
- **`TWAI_RX = GPIO0` + 4.7 kΩ/10 kΩ divider.** Gives ≈ 3.4 V from the 5 V
  `RXD` swing. Thévenin ≈ 3.2 kΩ against ≈ 25 pF is an ≈ 80 ns edge — negligible
  against a 2 µs bit time at 500 kbit/s. `esptool` only has to sink ≈ 1 mA to
  hold GPIO0 low for download mode, which the auto-reset transistor manages.
  This matters because firmware is uploaded remotely
  (`pio remote run -t upload`) with no access to the BOOT button.
- **GPIO2 stays unused.**

Alternative, if the strapping-pin workarounds prove unreliable: drop the console
link and reuse `BATTERY_TX_PIN` (GPIO32) and `BATTERY_RX_PIN` (GPIO39, input
only). No strapping pins, no extra resistors — at the cost of the console-only
fields listed above.

## Protocol

500 kbit/s. Standard 11-bit identifiers. **Pure cyclic broadcast — there is no
request/response.** The BMS transmits the whole set unprompted, typically once
per second, with the frames a few ms apart:

```
t=0.000  0x351   CVL, CCL, DCL, DVL
t=0.002  0x355   SoC, SoH
t=0.004  0x356   voltage, current, temperature
t=0.006  0x359   protection / alarm bitmaps, module count
t=0.008  0x35C   charge / discharge enable request
t=0.010  0x35E   "PYLON"
t=1.000  ... repeats
```

### Frames

| ID | Direction | Contents |
|---|---|---|
| `0x351` | BMS → us | CVL, CCL, DCL, DVL — **the frame of interest** |
| `0x355` | BMS → us | SoC [%], SoH [%] |
| `0x356` | BMS → us | voltage (0.01 V), current (0.1 A), temperature (0.1 °C) |
| `0x359` | BMS → us | protection and alarm bitmaps, module count |
| `0x35C` | BMS → us | charge / discharge enable request flags |
| `0x35E` | BMS → us | manufacturer string `PYLON` |
| `0x305` | us → BMS | inverter heartbeat, 8 bytes, all zero, 1 Hz |

`0x305` carries no query semantics — it only tells the BMS that the counterpart
is alive. Some firmware revisions do not start transmitting, or raise a
communication-loss alarm, without it; others ignore it entirely. *Verify:* only
send it if nothing arrives during bring-up.

### `0x351` layout (DLC 8, little-endian)

| Bytes | Field | Type | Unit |
|---|---|---|---|
| 0–1 | **CVL** — charge voltage limit | uint16 | 0.1 V |
| 2–3 | **CCL** — max charge current | int16 | 0.1 A |
| 4–5 | **DCL** — max discharge current | int16 | 0.1 A |
| 6–7 | **DVL** — discharge voltage limit | uint16 | 0.1 V |

Expected US5000 values: CVL ≈ 53.2 V, DVL ≈ 47 V, CCL/DCL in the tens to
hundreds of amps scaled by module count. Pylontech sends DCL as a positive
magnitude. *Verify byte order and scaling against captured data* before wiring
CCL into any control decision.

CCL drops toward 0 as the pack approaches full (taper), at low temperature, and
under an active protection. CCL = 0 means "do not charge".

## Field coverage vs console port

| `PylontechState` field | Available on CAN | Note |
|---|---|---|
| `voltage` | yes, `0x356` | 0.01 V instead of 1 mV |
| `current` | yes, `0x356` | 0.1 A instead of 1 mA |
| `temperature` | yes, `0x356` | stack aggregate, not pack 1 |
| `soc` | yes, `0x355` | same 1 % resolution |
| `basic_status` | derivable | from the sign of the current |
| `max_voltage` | indirect, `0x351` | CVL is semantically a limit, not the pack's Max Voltage |
| `cfet_on` / `dfet_on` | degraded, `0x35C` | enable *request* toward the inverter, not actual FET state |
| `bat_events` … `system_alarm` | degraded, `0x359` | ~10 standardised bits instead of four 32-bit vendor masks |
| `heater_on` | **no** | console only |
| `charge_times` | **no** | console only |
| `total_capacity_mah` | **no** | console only (parsed but currently unused) |
| `pack_index` / per-pack data | **no** | CAN is always a stack aggregate |

New data with no console equivalent: **CCL / DCL / DVL** (`0x351`), **SoH**
(`0x355`), force-charge and request-full-charge flags (`0x35C`).

Boiler regulation is unaffected: `autoRegulate()` in `relay.cpp` reads only
`b.soc`, `b.current` and `b.voltage`, all of which CAN provides. The power
figure `b.voltage * b.current` becomes quantised to ≈ 5 W steps, irrelevant
against the boiler's 500 W steps.

## Firmware

The TWAI driver is part of ESP-IDF and ships with the Arduino core — no library
dependency is needed.

```c
#include <driver/twai.h>

twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(
    (gpio_num_t)BATTERY_CAN_TX_PIN, (gpio_num_t)BATTERY_CAN_RX_PIN,
    TWAI_MODE_NORMAL);              // NORMAL, so the battery gets its ACK
twai_timing_config_t t = TWAI_TIMING_CONFIG_500KBITS();
twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();
twai_driver_install(&g, &t, &f);
twai_start();
```

### Structural differences from `pylontech_comm.cpp`

- No poll loop, no command, no retry counters. A background task blocks in
  `twai_receive()` with a timeout and dispatches on the identifier.
- Validity is a **staleness watchdog**: keep the timestamp of the last received
  frame and invalidate after a few seconds of silence, rather than counting
  failed poll cycles (`PYLONTECH_FAIL_INVALIDATE_THRESHOLD`).
- No consensus voting — CAN's CRC already rejects corrupted frames.
- Frames are independent, so there is no atomic snapshot across identifiers.
  Timestamp each frame and take the mutex once per burst.

### Planned stages

1. **Sniffer.** `TWAI_MODE_NORMAL`, accept-all filter, log every received
   identifier and payload. Validates wiring, termination, ACK, and whether the
   battery transmits unprompted. If nothing arrives within ~10 s, start sending
   `0x305` at 1 Hz and retry.
2. **Parse `0x351`** into a small state struct with a staleness watchdog; expose
   CCL/DCL/CVL/DVL through a thread-safe getter and publish them to InfluxDB.
3. **Consume CCL** in `relay.cpp` as an input to the boiler dump-load decision,
   replacing the indirect inference of inverter throttling.

### Explicitly out of scope for now

Writing limits back to the inverter (`MCHGC`, `PCVV`, `PBFT`, `PSDV` — see
`doc/ps_rs232_protocol_FULL_ai_ready.txt` §5). Those commands write to the
inverter's **EEPROM**, which has a finite endurance of order 10^5 writes;
mirroring CCL at frame rate would wear it out. If it is ever implemented it
needs rounding **down** to the discrete set returned by `QMCHGCR`, hysteresis,
a rate limit of minutes, and `ACK`/`NAK` checking. The benefit is small — the
BMS protects itself with its own FETs — so this is deliberately deferred.

## Bring-up checklist

- [ ] Module: pin S at GND (not VCC), 120 Ω present, VCC to 5 V
- [ ] Battery RJ45: confirm pin 4 = CAN-H, pin 5 = CAN-L
- [ ] Battery termination: measure RJ45 pin 4 ↔ pin 5 with the battery off
- [ ] Resistors fitted: 2.2 kΩ pull-down on GPIO12, 4.7 kΩ/10 kΩ divider on GPIO0
- [ ] Board still boots and still accepts a remote serial upload with the module powered
- [ ] Sniffer logs frames; if silent, add the `0x305` heartbeat
- [ ] `0x351` byte order and scaling confirmed against plausible values
- [ ] Console link (`Serial2`) still works alongside CAN

## References

- [TJA1050 datasheet (NXP)](https://doc.platan.ru/pdf/datasheets/fulihao/TJA1050.pdf)
- `doc/pylontech_comm_spec.md` — the existing console-port link
- `doc/ps_rs232_protocol_FULL_ai_ready.txt` — inverter RS232 protocol
