# Battery CAN Link – Specification

## Overview

A second, independent link to the **Pylontech US5000** over its **CAN port**,
in addition to the existing console-port link described in
[`pylontech_comm_spec.md`](pylontech_comm_spec.md).

The battery's CAN port was **unused** — the installed inverter is not compatible
with the Pylontech BMS protocol, so nothing was connected to it. That made the
port available for the ESP32, which is now the only other node on it.

> **Status:** Hardware built and verified — transceiver module, resistors and
> RJ45 wiring are in place and the link works. Firmware is at stage 1 (the
> bring-up sniffer, `src/pylontech_can.cpp`). Items below marked *verify* that
> The **protocol is settled**: the vendor specification
> (`CAN-Bus-protocol-PYLON-low-voltage-V1.2-20180408.pdf`) is transcribed under
> [Frame reference](#frame-reference). What remains open is behavioural, not
> structural — see the bring-up checklist.
> The data model and storage plan for stage 2 are in
> [`battery_can_data_spec.md`](battery_can_data_spec.md).

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

The ESP32 is the **only other node** on the bus. Two consequences:

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
module RXD  → 4.7 kΩ → GPIO4,  plus 10 kΩ from GPIO4 to GND
module CANH → battery A/CAN RJ45 pin 4
module CANL → battery A/CAN RJ45 pin 5
```

Pin 4 = CAN-H and pin 5 = CAN-L are confirmed by the US5000 manual. Ground is
already common with the battery through the console port, so `A/CAN` pin 6
(CAN-GND) stays unwired. Both CAN conductors share the 15 m link cable with the
two RS232 links — see [`rj45_cable_wiring.md`](rj45_cable_wiring.md).

### Pin assignment rationale

With every non-strapping GPIO already taken, the original design put both CAN
signals on strapping pins (GPIO12 and GPIO0). GPIO0 turned out to be a mistake
and CAN RX now lives on GPIO4, freed by moving the `BTN_UP` touch pad to GPIO2
(see [`todo/004`](todo/004-move-can-rx-off-gpio0.md)). Both CAN signals sit
**high** at boot (the bus idles recessive and TJA1050's `TXD` has an internal
pull-up), so the choice is constrained:

| Pin | High at boot means | Verdict |
|---|---|---|
| GPIO12 | flash voltage selected as 1.8 V → **board does not boot** | usable only with an external pull-down |
| GPIO4 | not a strapping pin, no auto-reset role | **CAN RX** (was the `BTN_UP` touch pad) |
| GPIO0 | normal boot fine, but the USB-serial auto-reset transistor loads it whenever the CP2102 is powered with its port closed → **CAN RX corrupted** | auto-reset only, nothing else |
| GPIO2 | download mode needs it low or floating; a touch pad is fine, a CAN signal held high would break flashing | `BTN_UP` touch pad |

Hence:

- **`TWAI_TX = GPIO12` + 2.2 kΩ pull-down.** TJA1050's internal pull-up
  (≈ 17–50 kΩ to 5 V) overrides the ESP32's internal pull-down (≈ 45 kΩ). With
  2.2 kΩ the pin settles at ≈ 0.2–0.6 V, below `VIL` = 0.25·VDD = 0.825 V. While
  the driver is running, the ESP32 sources ≈ 1.5 mA into it, which is fine.
- **`TWAI_RX = GPIO4` + 4.7 kΩ/10 kΩ divider.** Gives ≈ 3.4 V from the 5 V
  `RXD` swing. Thévenin ≈ 3.2 kΩ against ≈ 25 pF is an ≈ 80 ns edge — negligible
  against a 2 µs bit time at 500 kbit/s. GPIO4 has no strapping or auto-reset
  role, so nothing else ever drives it.
- **GPIO0 carries CAN RX no more.** The first build had the divider on GPIO0,
  reasoning that `esptool` only needs to sink ≈ 1 mA through the auto-reset
  transistor for download mode. That part was true — remote flashing worked —
  but the reverse case was missed: with the CP2102 powered and its port
  *closed* (DTR/RTS idle) the auto-reset transistor loads GPIO0 and corrupts
  reception (`REC` climbs, `rx = 0`). Opening any serial monitor released the
  pin and "revived" the link, which is the whole story behind
  [`todo/002`](todo/002-can-link-freezes-under-main-firmware.md). GPIO0 is now
  left to the auto-reset/download circuit alone, so remote uploads
  (`pio remote run -t upload`, no access to the BOOT button) keep working.
- **GPIO2 holds the `BTN_UP` touch pad.** The pad is a floating conductor, so
  it satisfies the "low or floating" download-mode requirement; a pull-down is
  optional. If touch on GPIO2 proves unreliable, dropping `BTN_UP` is
  acceptable — the web UI covers it.

Alternative, if the strapping-pin workarounds prove unreliable: drop the console
link and reuse `BATTERY_TX_PIN` (GPIO32) and `BATTERY_RX_PIN` (GPIO39, input
only). No strapping pins, no extra resistors — at the cost of the console-only
fields listed above.

## Protocol

500 kbit/s. Standard 11-bit identifiers. **Pure cyclic broadcast — there is no
request/response.** The BMS transmits the whole set unprompted — the vendor spec says once per
second, **this pack sends it every 2 s** (see
[Measured behaviour](#measured-behaviour-on-this-pack)) — with the frames a few
ms apart:

```
t=0.000  0x351   charge voltage, CCL, DCL
t=0.002  0x355   SoC, SoH
t=0.004  0x356   voltage, current, temperature
t=0.006  0x359   protection / alarm bitmaps, module count
t=0.008  0x35C   charge / discharge enable request
t=0.010  0x35E   "PYLON"
t=2.000  ... repeats   (measured; the vendor spec says t=1.000)
```

### Frames

Six frames from the BMS, one from us. `0x35E` is not in the transmit list of the
vendor spec's own summary but is documented as a frame, and the sniffer sees it.

| ID | Direction | Contents |
|---|---|---|
| `0x351` | BMS → us | charge voltage, CCL, DCL — **the frame of interest** |
| `0x355` | BMS → us | SoC [%], SoH [%] |
| `0x356` | BMS → us | voltage (0.01 V), current (0.1 A), temperature (0.1 °C) |
| `0x359` | BMS → us | protection and alarm bitmaps, module count |
| `0x35C` | BMS → us | charge / discharge / force-charge / full-charge request flags |
| `0x35E` | BMS → us | manufacturer string `PYLON` |
| `0x305` | us → BMS | inverter reply — **not sent; sending it silences this pack**, see below |

## Measured behaviour on this pack

Everything below was observed on the actual US5000 during stage-1 bring-up, and
each item contradicts something the vendor specification says. The frame
*encoding* matched the document exactly — voltage, current, temperature and SoC
agree with the console link to 7 mV and 0.09 A. What did not match is the
surrounding behaviour.

| | vendor spec | this pack |
|---|---|---|
| broadcast period | 1 s | **2 s** (180 frames/min over six identifiers) |
| `0x305` inverter reply | required, every second | **must not be sent** — see [`0x305`](#0x305--inverter-reply-us--bms) |
| `0x351` bytes 6–7 | blank, no DVL | `C2 01` = **45.0 V** |
| `0x355` bytes 4–7 | blank | `09 00 0F 00` = **9 and 15**, meaning unknown |

The two unexplained fields are stable across hours and across a change in SoC,
so they are not derived measurements. Neither is decoded or stored; they are
recorded here so that a future firmware change cannot quietly "fix" the parser
into reading them as something they are not.

### The pack stops broadcasting for hours, and it is not sleep

It went silent for **6 h 42 min** overnight (00:33 → 07:15). The obvious reading
— that the pack sleeps while idle at about −1 A and wakes with the morning
charge — **does not survive the data**: the console link recorded charging
starting around 06:30, three quarters of an hour before CAN came back. Sunrise
is not the trigger.

What the returns do correlate with is an **ESP32 restart**. Both observed
recoveries followed one within ~90 s — a reboot at 00:14 gave frames at 00:16,
one at 07:14 gave frames at 07:15:28 — and during the first of those both serial
links were running normally, so the runtime mute is not the common factor
either.

One mechanism to check before looking further, because it sits in this document
already: `TWAI_TX` is GPIO12 with a 2.2 kΩ pull-down, needed so the board boots
at all (see [Pin assignment rationale](#pin-assignment-rationale)). TJA1050
reads `TXD` **low as dominant**, so from reset until the TWAI driver takes the
pin — a second or more — the transceiver drives the bus dominant and jams it.
The pin analysis above weighs only the strapping behaviour and does not mention
this side effect.

It does not cleanly explain a *recovery*, though, and it cuts the other way:
the same jam should knock a transmitting pack into bus-off, which is what
appears to have happened after the reboot at 00:30, with the pack dying three
minutes later. So the mechanism is a candidate for the faults, not obviously for
the returns.

**Not established.** The next cheap test is to restart the ESP32 while the link
is silent and change nothing else; if frames return within ~90 s again, the
correlation is real and worth chasing.

Either way, `pylontech_can_valid()` is false for hours at a stretch, so a
consumer must treat CAN as an *additional* input and never a required one —
which is what [`battery_can_data_spec.md`](battery_can_data_spec.md) already
calls fail-open for stage 3, now with a measured reason rather than a
precaution. It also means an outage is not automatically a fault; the
`chajda-can-link` series and the 20-minute health line in `app.log` are what
tell the two apart.

### Bus errors: bursty, source not identified

The receiver logs roughly one bus error per minute while frames are flowing,
arriving in bursts rather than steadily — long stretches of nothing, then twenty
in two minutes. No frames are lost to it: `rx_missed` stays 0, the CRC discards
what it should, and the link has held `valid` for hours at a stretch.

Ruled out so far:

- **RS232 crosstalk on the shared cable**, the documented first suspect. Muting
  both serial links did not reduce the rate — see
  [`rj45_cable_wiring.md`](rj45_cable_wiring.md).
- **Relay switching** (boiler, mobile charger). The largest burst observed fell
  in a window where the boiler was off and nothing was switching.
- **The morning charge current**, as a trigger for the pack to start talking —
  charging began about 45 minutes before the link returned.

Still unchecked: **termination at the battery end**, which is the leading
candidate and needs a measurement on site (RJ45 pin 4 ↔ pin 5, battery off).
Reflections on 15 m of unterminated line would fit the symptom, though not
obviously the bursts.

## Frame reference

**Source: `CAN-Bus-protocol-PYLON-low-voltage-V1.2-20180408.pdf`** — the vendor
specification, in `~/data/k porizeni/chata-solar/Pylontech US5000/`. This is
authoritative and supersedes every third-party description; where an earlier
revision of this document disagreed with it, this document was wrong. See
[Third-party sources](#third-party-sources-and-where-they-mislead) for what the
others got right and where they mislead.

Bus: standard 11-bit identifiers, 500 kbit/s, transmission cycle 1 s per the
vendor spec (**2 s measured here**), **little endian** throughout.

### `0x351` — charge voltage and current limits

| bytes | field | type | unit |
|---|---|---|---|
| 0–1 | battery charge voltage (recommended) | u16 | 0.1 V |
| 2–3 | **CCL** — charge current limit | s16, 2's complement | 0.1 A |
| 4–5 | **DCL** — discharge current limit | s16, 2's complement | 0.1 A |
| 6–7 | *blank in the vendor spec* | | |

> **Bytes 6–7 are not DVL.** Earlier revisions of this document, AntBmsToCan and
> the Eneronix write-up all place a discharge voltage limit there. The vendor
> spec leaves both bytes empty — AntBmsToCan even annotates its own DVL write
> *"Debatable if this is needed"*. A DVL there is a de-facto extension some
> vendors emit, not part of this protocol. Read those two bytes raw during
> bring-up and expect zeros; do not build anything on them.
>
> **Measured: they are not zero.** This pack sends `C2 01` = 450 → **45.0 V**,
> unchanged over hours and across a change in SoC — i.e. it does emit the
> de-facto DVL extension. "Expect zeros" was wrong for this pack. It is still
> not decoded or stored: the vendor spec does not define the field, so nothing
> may depend on it.

Note also that byte 0–1 is the *recommended charge voltage* (建议充电电压), not a
hard ceiling.

### `0x355` — SoC / SoH

| bytes | field | type | unit |
|---|---|---|---|
| 0–1 | SoC of a single module, or system average | u16 | 1 % |
| 2–3 | SoH of a single module, or system average | u16 | 1 % |
| 4–7 | *blank in the vendor spec* | | |

> **There is no 0.1 % SoC.** The `SoC * 10` in bytes 4–5 is an AntBmsToCan /
> SimpBMS invention, not Pylontech. SoC resolution stays at 1 %, the same as
> the console link.
>
> **Measured: bytes 4–7 are not empty either.** This pack sends `09 00 0F 00` —
> two u16 values, 9 and 15 — unchanged over hours and across a change in SoC, so
> they are not derived measurements. Meaning unknown; not decoded. The `SoC * 10`
> reading stays excluded regardless: at 93 % that would be 930, not 9.

### `0x356` — measurements

| bytes | field | type | unit |
|---|---|---|---|
| 0–1 | voltage of a single module, or system average | s16, 2's complement | 0.01 V |
| 2–3 | module or system total current | s16, 2's complement | 0.1 A |
| 4–5 | **average cell temperature** | s16, 2's complement | 0.1 °C |
| 6–7 | *blank* | | |

Voltage is **signed** (Eneronix calls it unsigned — it is wrong, though the
distinction cannot matter below 327.67 V). The temperature is the average *cell*
temperature, not a MOSFET or stack sensor.

### `0x359` — protection and alarm

| byte | content |
|---|---|
| 0 | protection, table 1 |
| 1 | protection, table 2 |
| 2 | alarm, table 3 |
| 3 | alarm, table 4 |
| 4 | module numbers — u8 |
| 5 | `'P'` = 0x50 |
| 6 | `'N'` = 0x4E |
| 7 | — |

Protection is a tripped state; alarm is the corresponding warning level. Tables
1/3 and 2/4 share a bit layout:

| bit | tables 1 & 3 (bytes 0, 2) | tables 2 & 4 (bytes 1, 3) |
|---|---|---|
| 0 | — | charge over / high current |
| 1 | cell or module over / high voltage | — |
| 2 | cell or module under / low voltage | — |
| 3 | cell over / high temperature | system error *(t2)* / internal communication fail *(t4)* |
| 4 | cell under / low temperature | — |
| 5 | — | — |
| 6 | — | — |
| 7 | discharge over / high current | — |

Only seven bits are defined across all four bytes. Everything else is blank in
V1.2 — which is a reason to keep storing the bytes raw rather than as named
booleans only: this spec is from 2018 and the US5000 is a later product, so
undefined bits may well carry something.

### `0x35C` — request flags

| byte | content |
|---|---|
| 0 | request flags, table 5 |
| 1 | — |

Table 5:

| bit | mask | meaning |
|---|---|---|
| 7 | 0x80 | charge enable |
| 6 | 0x40 | discharge enable |
| 5 | 0x20 | request force charge I |
| 4 | 0x10 | request force charge II |
| 3 | 0x08 | request full charge |

The vendor notes on these are worth reading in full, because two of them are
directly relevant to this project:

- **Force charge I (bit 5)** is for an inverter that *allows* the battery to
  shut down and can wake it to charge. **Force charge II (bit 4)** is for an
  inverter that does *not* want the battery to shut down and charges it before
  it would. The vendor recommends bit 4. SoC ranges are model-specific — quoted
  for US2000B and US2000B-Plus, **not** for the US5000, so the thresholds here
  are unknown and must be observed.
- **Request full charge (bit 3)** — *"if SOC never higher than 97 % in 30 days,
  will set this flag to 1. And when the SOC is ≥ 97 %, the flag will be 0."* The
  reason given is SoC-estimator drift: without a periodic full charge the
  accumulated error grows until the pack "may not be able to be charged or
  discharged as expected capacity". The vendor's advice to an inverter is to
  charge from the grid while it is set.

  For this system that flag is an actionable control input, not just telemetry —
  see [`battery_can_data_spec.md`](battery_can_data_spec.md), stage 3.

### `0x35E` — manufacturer name

ASCII `PYLON`. The vendor table lists only bytes 0–1 while giving a five-character
value, which is a documentation slip; read the whole payload as ASCII.

### `0x305` — inverter reply, us → BMS

Eight zero bytes, **once per second** — *"Inverter reply every second: 0x305:
00-00-00-00-00-00-00-00"*. So the vendor spec does make it part of the protocol
rather than the optional keepalive earlier revisions of this document assumed.

> **Do not send it to this pack. The controller no longer does.**
>
> Measured during bring-up: with `0x305` transmitted at 1 Hz the pack stopped
> broadcasting altogether. With the controller transmitting *nothing at all* it
> resumed after ~90 s and has broadcast continuously since.
>
> The mechanism is that nothing on this bus acknowledges `0x305`. An
> unacknowledged frame is retransmitted by the CAN controller at bus speed, so a
> single queued reply walks the transmit error counter to bus-off within
> milliseconds — spraying error flags across the bus the whole time, which
> corrupts the pack's own frames and drives *its* error counters up. Some forty
> minutes of that left the pack silent until the bus went quiet again.
>
> The acknowledge bit is a separate matter and is mandatory. `TWAI_MODE_NORMAL`
> acknowledges every received frame, and that is the only thing the controller
> puts on the wire. See `CAN_SEND_HEARTBEAT` in `src/pylontech_can.cpp`; single
> shot (`TWAI_MSG_FLAG_SS`) is also in place so that re-enabling it can never
> reproduce the retry storm.

### Frames that do **not** exist here

`0x35A`, `0x35F`, `0x370`–`0x377` (including the tempting `0x373` cell min/max
voltage and temperature) belong to the Victron/SMA BMS-CAN profile, **not** to
Pylontech. The vendor spec defines exactly the seven frames above. Per-cell
voltage spread is therefore not available over CAN at all. The receive filter
stays accept-all, so anything unexpected still shows up in `/can` — but nothing
should be designed in expectation of it.

## Third-party sources and where they mislead

Kept as a record of what was checked, and because it shows the failure mode.

| source | `0x351`/`0x355`/`0x356` | `0x359` | `0x35C` |
|---|---|---|---|
| [AntBmsToCan][antbms] | right, except invented DVL and 0.1 % SoC | hardcoded zero; borrowed `0x35A`'s 4+4 layout | hardcoded zero |
| [Eneronix][eneronix] | right, except DVL and "unsigned" voltage | one sentence of prose | not mentioned |
| vendor PDF | **authoritative** | full bit tables | full bit tables + semantics |

Three independent descriptions agreeing on `0x351` was not the confirmation it
looked like: all three carry the same DVL error, and two of them the same
missing-bytes optimism about `0x355`. Agreement among derivative sources
propagates their common ancestor's mistakes — it does not test them. Only the
vendor document did.

Two specifics worth recording:

- AntBmsToCan's 4-alarm + 4-warning structure is not a rival reading of `0x359`.
  It is lifted from the dead code still in the same file (lines ~757–862, inside
  a `/* */` block — the SimpBMS-lineage Victron emitter it was derived from),
  where those bytes belong to **`0x35A`**. The author moved it to `0x359` with
  the comment *"Spec for Pylontech addresses this as 0x359"*.
- AntBmsToCan's `0x35E` indexes its name buffer as `[0][1][2][3][1][2][3][4]`
  and so transmits `PYLOYLON`.

[antbms]: https://github.com/dxoverdy/AntBmsToCan/blob/master/AntBmsToCan/AntBmsToCan.ino
[eneronix]: https://eneronix.com/pylontech-protocol-in-inverter-battery-communication/

## Field coverage vs console port

| `PylontechState` field | Available on CAN | Note |
|---|---|---|
| `voltage` | yes, `0x356` | 0.01 V instead of 1 mV |
| `current` | yes, `0x356` | 0.1 A instead of 1 mA |
| `temperature` | yes, `0x356` | stack aggregate, not pack 1 |
| `soc` | yes, `0x355` | same 1 % resolution |
| `basic_status` | derivable | from the sign of the current |
| `max_voltage` | indirect, `0x351` | the recommended charge voltage, not the pack's Max Voltage |
| `cfet_on` / `dfet_on` | degraded, `0x35C` | enable *request* toward the inverter, not actual FET state |
| `bat_events` … `system_alarm` | degraded, `0x359` | ~10 standardised bits instead of four 32-bit vendor masks |
| `heater_on` | **no** | console only |
| `charge_times` | **no** | console only |
| `total_capacity_mah` | **no** | console only (parsed but currently unused) |
| `pack_index` / per-pack data | **no** | CAN is always a stack aggregate |

New data with no console equivalent: **CCL / DCL** (`0x351`), **SoH** (`0x355`),
and the force-charge / request-full-charge flags (`0x35C`). There is no DVL —
see [`0x351`](#0x351--charge-voltage-and-current-limits).

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
   battery transmits unprompted. **Transmit nothing** — the vendor spec asks
   for `0x305` at 1 Hz, but on this pack that is what stops the broadcast; see
   [`0x305`](#0x305--inverter-reply-us--bms).
2. **Parse `0x351`** into a small state struct with a staleness watchdog; expose
   CCL/DCL/charge-voltage through a thread-safe getter and publish them to
   InfluxDB.
   Specified in detail in [`battery_can_data_spec.md`](battery_can_data_spec.md).
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
- [x] Resistors fitted: 2.2 kΩ pull-down on GPIO12, 4.7 kΩ/10 kΩ divider on GPIO4 (moved from GPIO0 on 2026-09-13, see todo/004)
- [ ] Board still boots and still accepts a remote serial upload with the module powered
- [x] `0x305` **not** sent — sending it silences this pack
- [x] Sniffer logs frames
- [x] `0x351` values plausible; bytes 6–7 are **45.0 V**, not empty — a DVL after all
- [x] `0x355` bytes 4–7 are **`09 00 0F 00`**, not empty; meaning unknown
- [x] `0x359` byte 4 = module count (1), bytes 5–6 = `PN`
- [x] `0x35C` byte 0 = `0xC0` in normal operation (charge + discharge enable)
- [x] Console link (`Serial2`) still works alongside CAN

## References

- [TJA1050 datasheet (NXP)](https://doc.platan.ru/pdf/datasheets/fulihao/TJA1050.pdf)
- **`CAN-Bus-protocol-PYLON-low-voltage-V1.2-20180408.pdf`** — the vendor specification, in `~/data/k porizeni/chata-solar/Pylontech US5000/`. Authoritative; transcribed above
- [`battery_can_data_spec.md`](battery_can_data_spec.md) — stage 2: what is parsed, published and stored
- [AntBmsToCan](https://github.com/dxoverdy/AntBmsToCan/blob/master/AntBmsToCan/AntBmsToCan.ino) — an independent emitter of this profile; corroborates `0x351`/`0x355`/`0x356`, silent on `0x359`/`0x35C`
- [Eneronix: Pylontech protocol in inverter-battery communication](https://eneronix.com/pylontech-protocol-in-inverter-battery-communication/) — third description of the same three frames; one prose sentence on `0x359`, nothing on `0x35C`
- `doc/pylontech_comm_spec.md` — the existing console-port link
- `doc/rj45_cable_wiring.md` — how CAN shares the 15 m link cable with both RS232 links
- `doc/ps_rs232_protocol_FULL_ai_ready.txt` — inverter RS232 protocol
