# RJ45 Link Cable – Wiring

## Overview

All three data links between the ESP32 and the power hardware share a single
**~15 m Ethernet patch cable (UTP, 8 conductors)**:

| Link | Peer | Doc |
|---|---|---|
| Inverter RS232, 2400 8N1 | inverter RS232 port | [`ps_rs232_protocol_FULL_ai_ready.txt`](ps_rs232_protocol_FULL_ai_ready.txt) |
| Battery console RS232, 115200 8N1 | Pylontech US5000 `Console` port | [`pylontech_comm_spec.md`](pylontech_comm_spec.md) |
| Battery CAN, 500 kbit/s | Pylontech US5000 `A/CAN` port | [`battery_can_spec.md`](battery_can_spec.md) |

Six signals plus ground fit into the eight conductors, and — as long as each
device connector is populated with **only its own pins** — the assignments do
not collide. The cable is therefore wired **straight through, 1:1**: conductor
*n* at the ESP end carries device pin *n*. At the far end the cable fans out
into three RJ45 plugs.

> **Status:** the CAN link is not implemented yet (see
> [`battery_can_spec.md`](battery_can_spec.md)); its conductors can be left
> unterminated at the ESP end until the transceiver is fitted.

## Connector pinouts (from the vendor manuals)

Inverter RS232 port, from the device's point of view
(`ps_rs232_protocol_FULL_ai_ready.txt` §1):

| Pin | Signal |
|---|---|
| 1 | TXD — inverter transmits |
| 2 | RXD — inverter receives |
| 8 | GND |
| 3–7 | not connected |

Pylontech US5000 `Console` port (US5000 user manual, *Console*):

| Pin | Signal |
|---|---|
| 3 | 232-TX — battery transmits |
| 4 | +5…+12 V wake-up input |
| 5 | GND for wake-up |
| 6 | 232-RX — battery receives |
| 8 | 232-GND |

Wake-up pulse: ≥ 0.5 s at 5–15 mA, then the voltage must be removed for normal
operation. Both RS232 ports swing at true RS232 levels, so each needs its own
MAX3232 at the ESP end.

Pylontech US5000 `A/CAN` and `B/RS485` ports (US5000 user manual, *Definition of
RJ45 Port Pin*) — both ports carry the same signals:

| Pin | Signal |
|---|---|
| 1–3 | must stay unconnected |
| 4 | CAN-H |
| 5 | CAN-L |
| 6 | CAN-GND |
| 7 | 485A |
| 8 | 485B |

## Cable mapping

Left: RJ45 pin at the ESP board. Right: pin and port at the far end.

| RJ45 pin (ESP) | T568B conductor | Signal | Device: pin + port |
|---|---|---|---|
| 1 | white/orange | inverter TX → ESP RX (MAX3232 #1) | pin 1 — inverter, RS232 |
| 2 | orange | inverter RX ← ESP TX (MAX3232 #1) | pin 2 — inverter, RS232 |
| 3 | white/green | 232-TX → ESP RX (MAX3232 #2) | pin 3 — battery, Console |
| 4 | blue | CAN-H | pin 4 — battery, A/CAN |
| 5 | white/blue | CAN-L | pin 5 — battery, A/CAN |
| 6 | green | 232-RX ← ESP TX (MAX3232 #2) | pin 6 — battery, Console |
| 7 | white/brown | spare | — |
| 8 | brown | common signal ground | pin 8 — inverter, RS232 **and** pin 8 — battery, Console |

Conductor 8 is the only one that branches: it feeds the inverter's GND and the
battery console's 232-GND from one wire. Both land on the ESP's single ground
node anyway (the two MAX3232 modules share it), so joining them at the far-end
junction changes nothing electrically.

### Why 1:1 is also the electrically right mapping

Straight-through wiring happens to put every link on its own T568B twisted
pair — which matters most for CAN:

| Pair | Conductors | Carries |
|---|---|---|
| orange | 1, 2 | inverter TX/RX |
| green | 3, 6 | console TX/RX |
| blue | 4, 5 | **CAN-H / CAN-L** |
| brown | 7, 8 | spare + ground |

Any other assignment would split at least one link across two pairs.

### CAN-GND

`A/CAN` pin 6 is left unconnected. CAN needs ground only as a common-mode
reference, not as a return path, and that reference already arrives over
conductor 8 via the console port's 232-GND — the two are the same net inside the
BMS.

*Verify during bring-up:* measure the resistance between `A/CAN` pin 6 and
`Console` pin 8 with the battery on and nothing else connected.

- ≈ 0 Ω — same net, as assumed. Leave `A/CAN` pin 6 unwired.
- open / non-trivial resistance — the CAN ground is separate. Then run CAN-GND
  on its own conductor (`A/CAN` pin 6 → cable pin 7) rather than bonding it to
  conductor 8 at the junction, which would defeat the separation.

## Populating the far-end plugs

Each of the three plugs must carry **only its own pins**; every other contact
stays empty. A straight-through plug would be actively harmful:

- **A/CAN plug — pins 4, 5 only.** Pins 7 and 8 on that port are 485A/485B;
  conductor 8 (ground) landing on pin 8 would drive the RS485 bus. The manual
  also requires pins 1–3 to stay unconnected.
- **Console plug — pins 3, 6, 8 only.** Pins 4 and 5 are the wake-up input;
  conductor 4 carries CAN-H and must not reach it.
- **Inverter plug — pins 1, 2, 8 only.**

## Termination and layout

The cable *is* the CAN bus, with exactly one node at each end — the correct
topology, no stubs. The TJA1050 module must therefore sit directly at the ESP
board's RJ45 jack, not on a branch. Its on-board 120 Ω terminates the ESP end.

**Measured on site 2026-09-01: the battery end is NOT terminated.** RJ45
pin 4 ↔ pin 5 (the blue pair) on the switched-off battery reads **32 kΩ** —
the differential input impedance of an unpowered CAN transceiver, exactly
what an unterminated node looks like. A fitted 120 Ω would dominate the
reading (120 ‖ 32 000 ≈ 119.6 Ω). Flipping the DIP switch changed nothing,
which kills the "DIP2 enables termination" assumption — on the US series the
DIP block is the pack address selector (and even where a BMS does switch
termination, it does so electronically, invisible on an unpowered pack).
The ESP side of the cable measured the expected 120 Ω.

The bus has therefore been running 500 kbit/s over 15 m with termination at
one end only. That is the leading explanation for both the permanent
~1/min background bus errors and the error-storm collapses of
[`todo/002`](todo/002-can-link-freezes-under-main-firmware.md): reflections
from the open end corrupt the pack's own transmissions, its TEC climbs, and
its CAN controller eventually latches off. **Fix: a 120 Ω resistor across
pins 4–5 at the battery end of the cable.** Verification after fitting it:
the pair measures ~60 Ω end to end, and the background bus_err rate drops
to ~zero.

The console link runs 115200 baud at ±12 V RS232 swing right next to the CAN
pair. CAN is differential and CRC-protected, so this should be tolerable, but it
is the first suspect if CAN error counters climb. The console link itself
already sees roughly one corrupted byte per 10 kB over this cable, which is why
`pylontech_comm.cpp` uses consensus voting.

> **Tested, and it is not the culprit.** CAN error counters did climb, so both
> RS232 links were muted at runtime (`serial_links` on `/cmd`) while the pack
> was broadcasting, then restored, with the pack awake and charging throughout:
>
> | serial links | duration | bus errors | rate |
> |---|---|---|---|
> | muted | 16 min | 22 | 1.4 / min |
> | active | 17.5 min | 16 | 0.9 / min |
>
> Same order of magnitude, and if anything worse with the cable quiet. The
> errors arrive in bursts either way, and the largest burst — 21 in two minutes
> — fell inside the *muted* window, with the boiler off and no relay switching
> either. Restoring the RS232 traffic did not disturb the pack at all: it stayed
> valid across the transition, no bus-off, no missed frames.
>
> So the RS232 traffic on this cable is exonerated. Whatever produces the bursts
> is something else, and termination (above) has not been checked.

## Bring-up checklist

- [ ] Cable wired 1:1, conductor 8 branched to inverter pin 8 and console pin 8
- [ ] Each plug populated with only its own pins (verify with a continuity test
      before plugging anything in)
- [ ] `A/CAN` pin 6 ↔ `Console` pin 8 measured; CAN-GND left unwired only if ≈ 0 Ω
- [ ] Both RS232 links still work after the CAN plug is connected
