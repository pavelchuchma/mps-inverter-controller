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

## 2026-09-01: the reshaped loop died too — and the fall was finally captured

The sniffer-shaped loop (`7488cfe`) went down 16 minutes after boot, so **the
loop shape was not the cause** — all three steady-state suspects above are
cleared. In hindsight there is a stronger argument that should have been made
first: while frames flow, the only thing either firmware puts on the wire is
the hardware-generated acknowledge bit. Software shape cannot be seen by the
pack, so it could never have been what kills it. The sniffer's 22 clean hours
were correlated with something else.

What the freeze actually looks like, now that a minute line straddles it
(07:52–07:53):

```
07:50:32  boiler relay ON, 500 W target (battery current 11.6 -> 1.7 A)
07:52:03  rx +180  err +2   REC 0   TEC 0    — perfectly healthy
07:52:46  last frame ever received
07:52:56  link down; silence probe starts
07:53:03  rx +128  err +12  REC 79  TEC 184  — the fall, mid-minute
07:53:34  bus-off (probes into a dead bus)
```

**The receiver did not go deaf — it watched the bus die.** REC 79 with a
burst of bus errors means the controller was actively receiving garbage as
the pack fell off the wire. From then on every 0x305 probe (1/s, ~20 000 by
13:30) went unacknowledged: there is no transmitter left on the bus. The BMS
console link answered normally the whole time, and `/status` showed the
battery charging and later Idle at 100 % — **the pack is alive; only its CAN
interface stops.** The session-old open question is answered, and the todo's
premise ("looks like ours, not the pack's") is inverted.

The revival lore also broke today:

- 13:25 ESP restart — now a long-jam boot again (init after WiFi) — did
  *not* revive it (was 8/8 before);
- 13:29 `can_bus_reset` (2 s dominant) did not either — and instead crashed
  the ESP: with the 200 ms receive timeout the CAN task is almost always
  inside `twai_receive()` when the webserver task uninstalls the driver →
  `ESP_RST_PANIC`. See todo 003.

Open leads, roughly in order:

1. **Physical layer, back on top.** A healthy receiver watching the bus
   degrade into an error storm is what a physical-layer fault looks like.
   Termination at the battery end (DIP2, ~60 Ω across CAN-H/L) is still
   unverified — measure on the next site visit.
2. **The boiler relay switched to 500 W two minutes before the fall.** One
   data point only (yesterday's 08:19 death was ~28 min after a boiler OFF),
   but the boiler is a switched resistive load on the same site wiring —
   worth watching for on the next freeze.
3. **Does our probing keep the pack down?** Every observed period with
   active transmission into the dead bus ended without a revival. A build
   with the probe disabled would give the pack a perfectly silent bus for
   hours and settle this.
4. **Reviving via the console link** — the protocol has `trst` (Test Soft
   Reset). Deployed 2026-09-01 ~16:42 as `POST /cmd {"name":"bat_trst"}` on
   `diag/can-sniffer-instrumented` (`35c6c87`): the webserver sets a flag,
   the battery polling task sends `trst` within one poll cycle and logs the
   raw response. **Armed but not yet fired.**

Yesterday the pack came back at 07:15 (after ~6.5 h dead, morning charge
underway) and 09:52; whether time/SoC/charge state is the real revival
variable is still unknown.

## Current state

- **2026-09-01 morning: the control extended itself overnight.** The sniffer
  ran 09:52 → 00:00 (14 h, ended only by the scheduled midnight restart) and
  again 00:00 → 07:35 (7.6 h, still clean at flash time): rate 175–185/min,
  no gap over 90 s, TEC 0, tx_failed 0. One rx-queue overflow at 00:14
  (missed 6, a single burst) with no effect on the link. Full log saved to
  `logs/app-2026-08-31_sniffer-control.log` (git-ignored).
- **The proposed fix is implemented and deployed** (`7488cfe`, flashed
  2026-09-01 ~07:45): receive timeout back to 200 ms, no shared state on the
  receive path (raw table task-local, one mutex take per burst), status
  polling only in the periodic branches, the 1 Hz peer-gated 0x305 replaced
  by the sniffer's silence probe (single-shot), and CAN init moved back after
  `initializeWiFi()` — the sniffer's long-jam boot position, the only boot
  shape that has ever revived a silent pack.
- **2026-09-01 13:30: link down since 07:52**, pack CAN-silent, ESP cycling
  probe/recovery. Freeze log saved to
  `logs/app-2026-09-01_reshaped-loop-freeze.log`.
- **`main`:** `7488cfe` (+ docs), nothing pushed.
- **Temporary settings to revert** once this is closed: `app.log` cap raised
  from 100 kB to 768 kB, and the health line at 1 minute instead of 20.

## Related

- [`../battery_can_spec.md`](../battery_can_spec.md) — link, frame layouts,
  measured deviations from the vendor document
- [`../battery_can_data_spec.md`](../battery_can_data_spec.md) — the data model
  this work was implementing; commit 2 (storage) is blocked until the link holds
- [`../rj45_cable_wiring.md`](../rj45_cable_wiring.md) — cable, and the
  crosstalk test that exonerated it. Termination at the battery end (DIP2, and
  a measurement across RJ45 pins 4↔5) remains unverified, but is now a weak
  suspect: the bus carries 180 frames/min with zero receive errors.
