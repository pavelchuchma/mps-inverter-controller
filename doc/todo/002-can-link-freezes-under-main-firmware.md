---
id: 002
title: CAN link freezes under the main firmware but not under the bring-up sniffer
type: bug
status: open
priority: high
component: battery-can
created: 2026-08-31
---

# CAN link freezes under the main firmware but not under the bring-up sniffer

## Summary

The parsed CAN firmware on `main` (commit `74e7279`) receives frames for
somewhere between 2 and 50 minutes and then stops receiving entirely, and does
not recover without a reboot. The original bring-up sniffer, running the same
driver configuration on the same hardware, ran **102 minutes without losing a
single burst**. The fault therefore looks like ours, not the pack's.

The frame *encoding* is not in question: values decode correctly and agree with
the console link to 7 mV and 0.09 A. This is purely about the link stopping.

## Observed behaviour

### The freeze signature

When it stops, our side looks perfectly healthy:

```
state=1 (RUNNING)  rx frozen  missed=0  recov=0  bus_err frozen  tx_failed=0
```

`state` is RUNNING, the task keeps looping and keeps writing its once-a-minute
health line, `/can` keeps answering — but `twai_receive()` never returns another
frame. `bus_err` freezing alongside `rx` is notable: the controller is not even
seeing traffic it fails to decode.

Runs observed under `main`-family firmware:

| window | firmware | outcome |
|---|---|---|
| 00:16–00:29 | transmit disabled | 13 min, **ended by a deliberate reboot** |
| 00:31–00:33 | transmit disabled | **died after 2 min**, degraded to 105 frames/min first |
| 07:15–08:04 | transmit disabled | 49 min, **ended by a deliberate reboot** |
| 08:05–08:19 | transmit disabled | **died after 14 min**, full 180 frames/min right to the end |
| 08:38, 08:59, 09:30 | 0x305 re-enabled, CAN init moved before WiFi | never received a single frame |

Two spontaneous failures, and they do not share a shape: one degraded first, the
other was cut off at full rate.

### The sniffer, as a control

`diag/can-sniffer-instrumented` (`d555697`) is the original sniffer plus a
once-a-minute `app.log` line, a 700 kB log cap and a 6144-byte task stack —
nothing else. On the same hardware, same day:

```
102 consecutive minute lines, no gap
rate 178–185 frames/min, mean 179.9, none below 178
REC 0   TEC 0   missed 0   tx_failed 0   no recoveries
```

The pack sends 6 frames every 2 s = 30 bursts/min = 180 frames/min. A minute
boundary cutting a burst explains ±6 frames, i.e. 174–186, so the entire
observed spread is sampling jitter with **no room for even one lost burst**.

`tx_failed = 0` is an independent check on the same thing: the sniffer starts its
`0x305` heartbeat after 10 s of silence and every heartbeat into a silent bus
fails, so the counter would move the moment the pack went quiet for 10 s. It
never moved.

**Conclusion: the pack did not stop, not once, in 102 minutes.**

## What this rules out

Each of these was believed at some point during the session and is now
contradicted by data:

- **The pack sleeps when idle and wakes with the morning charge.** Charging
  started around 06:30 per the console link; CAN stayed dead until 07:15.
- **RS232 crosstalk from the shared 15 m cable.** Muting both serial links at
  runtime did not lower the CAN error rate (22 errors in 16 min muted vs 16 in
  17.5 min active), and the largest burst fell in the *muted* window. See
  [`../rj45_cable_wiring.md`](../rj45_cable_wiring.md).
- **Bus errors damaging the link.** The sniffer accumulates them at the same
  ~1/min and is completely unaffected: 127 bus errors over 102 min with REC 0,
  TEC 0 and no lost frames. They are harmless.
- **The pack stopping on its own.** See above.
- **The heartbeat or a driver restart being needed for stability.** The sniffer
  ran 102 minutes with `tx_failed = 0`, so its heartbeat never fired and it
  never went bus-off. Neither mechanism was involved in staying up.

## What is still open

**Why the receiver goes deaf.** The freeze signature points at our side rather
than the pack, but the mechanism is unknown, and one observation argues against
the obvious explanation: `can_bus_reset` on `/cmd` does a full
`twai_driver_uninstall()` + `driver_start()`, and running it twice (09:04,
09:20) did not bring the link back. If a driver restart cures a deaf receiver,
that should have worked. It is possible the pack was genuinely quiet at that
point for its own reason — it had just reached 100 % SoC with CCL at 0 and
charge-enable cleared — so this is confounded rather than decisive.

**What revives a dead link.** Eight for eight, a link that had stopped came back
within ~90 s of an ESP32 boot whose jam was long (~16 s, CAN init after
`initializeWiFi()`, or ~35 s during a flash), and never came back after a boot
with a short jam or after a deliberate 2 s dominant hold. The mechanism is
`TWAI_TX` = GPIO12 carrying a 2.2 kΩ pull-down so the board boots, while
TJA1050 reads TXD low as **dominant** — so from reset until the driver claims
the pin, the transceiver jams the bus. Any node on it reaches bus-off within a
millisecond. Whether the revival is the jam itself, its release, or the full
chip reset is not established.

## Suspects

Ordered by what differs during *steady-state* operation, since the sniffer stays
up without ever transmitting or restarting anything:

1. **`twai_get_status_info()` plus a mutex take/give on every loop iteration** —
   `twai_receive` has a 20 ms timeout, so this runs ~50×/s, against the
   sniffer's 200 ms.
2. **Per-frame mutex** for `record_raw()`, plus the webserver task taking the
   same mutex from `/can`.
3. **The burst-commit rewrite** generally — the sniffer's loop shape is proven,
   the restructured one is not.

Explicitly *not* suspects: the minute health line (the instrumented sniffer
writes one too and is stable) and the log rotation changes.

## Proposed fix

Do not add a watchdog or restore the aggressive heartbeat — the evidence says
neither is what keeps the sniffer alive. Instead **narrow the difference**: bring
`main`'s receive loop back to the sniffer's shape (200 ms receive timeout, no
per-frame mutex, status polling only in the periodic branch) while keeping the
parser, `/can`, and the health line, then run it against the same minute-resolution
log and see whether it survives as long.

`main` already logs frame rate and `tx_failed` every minute, so the next failure
will answer the one question this session could not: whether a freeze is preceded
by silence on the bus, or whether the receiver goes deaf mid-traffic.

## Current state

- **On the device:** `diag/can-sniffer-instrumented` (`d555697`), left running
  deliberately to extend the control observation. `main` is *not* deployed.
- **`main`:** `74e7279`, all diagnostics committed, nothing pushed.
- **Worktree:** `/tmp/can-sniffer`. macOS runs `com.apple.tmp_cleaner` daily at
  midnight, so recreate it from the branch if it disappears — the source is in
  git, so nothing is lost.
- **Temporary settings to revert** once this is closed: `app.log` cap raised
  from 100 kB to 700–768 kB, and the health line at 1 minute instead of 20.

## Related

- [`../battery_can_spec.md`](../battery_can_spec.md) — link, frame layouts,
  measured deviations from the vendor document
- [`../battery_can_data_spec.md`](../battery_can_data_spec.md) — the data model
  this work was implementing; commit 2 (storage) is blocked until the link holds
- [`../rj45_cable_wiring.md`](../rj45_cable_wiring.md) — cable, and the
  crosstalk test that exonerated it. Termination at the battery end (DIP2, and
  a measurement across RJ45 pins 4↔5) remains unverified, but is now a weak
  suspect: the bus carries 180 frames/min with zero receive errors.
