---
id: 004
title: Move CAN RX off GPIO0 (shared with the serial auto-reset circuit)
type: task
status: open
priority: high
component: battery-can
created: 2026-09-13
---

# Move CAN RX off GPIO0 (shared with the serial auto-reset circuit)

## Summary

`TWAI_RX = GPIO0`, which is also the pin the USB-serial auto-reset circuit
drives (DTR/RTS → GPIO0 through the download/reset transistor). Whenever the
CP2102 on the debug pizero is **plugged in but its port is closed**, the
transistor loads GPIO0 and corrupts CAN reception. Opening the port (any
monitor, or just asserting DTR/RTS) releases GPIO0 and CAN runs cleanly.

This single fact is the real cause behind the whole of [`002`](002-can-link-freezes-under-main-firmware.md):
the "link freezes, a monitor connect revives it" mystery was never about
firmware, bus-off, termination, grounding or the battery — it was GPIO0 being
pulled by the auto-reset transistor.

The fix is to move CAN RX to a pin with no strapping/auto-reset role. The only
free-ish candidates are input-only pins (all taken) or a touch button, so this
is a small pin reshuffle, not a free pin.

## Root cause

`doc/battery_can_spec.md` (pin assignment rationale) documents `TWAI_RX =
GPIO0` behind a 4.7k/10k divider from the transceiver's 5 V RXD, and notes
GPIO0 is "usable, but the auto-reset circuit actively drives it." That last
clause is the bug: GPIO0 has two masters — CAN receive input, and the
esptool download/reset line driven by DTR/RTS from the CP2102.

- Port **closed** (CP2102 powered, DTR/RTS idle): auto-reset transistor loads
  GPIO0 → CAN RX corrupted (`REC` climbs, `rx = 0`, bursts incomplete).
- Port **open** (DTR/RTS asserted): transistor releases GPIO0 → the divider
  holds a clean level → CAN RX works, `REC → 0`.

`config.h:56` — `BATTERY_CAN_RX_PIN 0`.

## Observed behavior (proof)

On 2026-09-13, over `ssh pi@10.200.0.20` (pizero), the port was opened with
DTR/RTS asserted and **not a single byte was read**:

```
port closed:  rx=53   REC=85  last frame 23 min ago     ← dead
port open:    +12s rx=92  REC=85
              +30s rx=146
              +45s rx=194 REC=0   ← full ~192/min, errors gone
uptime kept increasing → no reset involved
```

A holder process (port open, DTR/RTS asserted, no reads) kept CAN at a clean
`rx +180/min, REC 0, err 0` for as long as it ran. Removing it dropped CAN
again within seconds. Baud rates rule out a serial multiplex to free a UART
pin: battery console is 115200, inverter is 2400 — one UART cannot read both.

## Proposed fix

Reshuffle two pins (both changes go together with the hardware rework):

1. **CAN RX: GPIO0 → GPIO4.** GPIO4 (Touch0) is bidirectional, not a strapping
   pin, and has no auto-reset role. Move the 4.7k/10k divider from the
   transceiver's RXD from GPIO0 to GPIO4. GPIO0 then returns to serving only
   the auto-reset/download circuit, so **remote flashing keeps working**.
2. **BTN_UP touch: GPIO4 → GPIO2.** GPIO2 (Touch2) is strapping but has **no**
   auto-reset transistor, so touch sensing stays clean even with the pizero
   plugged in. Add a pull-down on GPIO2 so it reads low at boot (strapping
   requirement); an idle touch pad alone may float. If GPIO2 proves fiddly,
   dropping BTN_UP entirely is an acceptable fallback (UP is a comfort control;
   the web UI covers it).

CAN TX stays on GPIO12 — it is an output and works fine; do not touch it.

### config.h changes (apply WITH the hardware, not before)

Applying these before the divider is physically moved would point the running
firmware at an unconnected pin and kill CAN. Do them in the same step as the
rework:

- `BATTERY_CAN_RX_PIN 0` → `4`
- `BTN_UP_TOUCH 4` → `2` (Touch0 → Touch2; update the comment)
- add a pull-down / `pinMode` note for GPIO2 boot level
- refresh the pinout comment block at the top of `config.h`
- update `doc/battery_can_spec.md` pin rationale (GPIO0 no longer CAN RX)

## Verification

After the rework and config change, with the pizero **plugged in and no
monitor open** (the exact state that used to kill CAN):

- `/can` shows `valid=true`, `rx` climbing at ~180/min, `REC 0`, `err 0`.
- Link survives the evening end-of-charge and overnight (the failures that
  filled `002`).
- `pio remote run -t upload` still flashes (GPIO0 auto-reset intact).
- BTN_UP touch still registers (or is intentionally dropped).

## Why not now

The rework is soldering at the cottage; the deployment is remote and visited
rarely. Until then CAN runs whenever a serial monitor is open on the pizero
(DTR/RTS asserted). The temporary port-holder workaround was **removed** on
2026-09-13 so the monitor and `pio remote` upload stay free of extra magic —
the trade-off is that CAN is down while the pizero is idle with no monitor.

## Related

- [`002`](002-can-link-freezes-under-main-firmware.md) — the freeze
  investigation this explains and closes out.
- [`../battery_can_spec.md`](../battery_can_spec.md) — pin assignment
  rationale to update.
