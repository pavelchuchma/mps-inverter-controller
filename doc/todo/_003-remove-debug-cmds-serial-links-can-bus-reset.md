---
id: 003
title: Remove the debug-only /cmd commands serial_links and can_bus_reset
type: task
status: done
priority: low
component: battery-can
created: 2026-09-01
resolved: 2026-09-14
---

# Remove the debug-only `/cmd` commands `serial_links` and `can_bus_reset`

## Summary

Both commands were built during the CAN freeze investigation
([`002`](_002-can-link-freezes-under-main-firmware.md)) and neither has a
purpose now that the root cause is known and fixed
([`004`](_004-move-can-rx-off-gpio0.md)):

- **`serial_links`** mutes both RS232 links for up to 30 min to test whether
  crosstalk on the shared 15 m cable was corrupting CAN. It was not — the mute
  changed nothing, and `002` closed that lead.
- **`can_bus_reset`** holds the bus dominant for 2 s and restarts the TWAI
  driver, imitating the boot-time jam that "revived" a silent pack. The pack
  was never silent; GPIO0 was. The command also panics the controller (see
  below), and the `link_healthy` guard makes it unrunnable while the link is
  up, so on a healthy system it can do nothing at all.

Once CAN has run for **24 h on the full 15 m cable** after the GPIO4 rework,
delete both instead of fixing anything. Neither the web UI nor any script
calls them.

## History: `can_bus_reset` panics the ESP (original bug, not fixed)

`POST /cmd {"name":"can_bus_reset"}` at 13:29:40 on 2026-09-01 rebooted the
controller with `ESP_RST_PANIC` about one second into the 2 s dominant hold.

`pylontech_can_force_bus_reset()` runs on the webserver task and calls
`twai_stop()` + `twai_driver_uninstall()` while the CAN task keeps looping.
Since `7488cfe` the receive timeout is 200 ms, so the CAN task is inside
`twai_receive()` essentially always — uninstalling the driver under it is a
near-guaranteed crash. Under the earlier 20 ms loop the same race existed but
lost far more often (both invocations on 2026-08-31 survived).

The proper fix would have been to run the reset from the CAN task itself
(request flag, checked once per loop). **Decided 2026-09-13: not worth doing.**
The command imitated a cure for a disease that did not exist; the accidental
panic boot delivered the same GPIO12 jam anyway, which is why this stayed
hidden behind the very behaviour the command was meant to imitate.

## What to delete

`src/esp_webserver.cpp`
- the `serial_links` and `can_bus_reset` branches in `handleCommand()`
- `SERIAL_PAUSE_MAX_MS`

`src/pylontech_can.cpp`, `include/pylontech_can.h`
- `pylontech_can_force_bus_reset()` and the dominant-hold duration constant
  next to `CAN_TRACE_LIMITS` (only caller was the command)

`src/inverter_comm.cpp`, `include/inverter_comm.h`
- `inverter_comm_set_paused()` — the command was its only caller

**Keep** `pylontech_comm_set_paused()`: the battery telnet console
(`battery_telnet.cpp`) pauses the console poller with it for as long as a
client is connected. Its header comment mentions the `serial_links` mute as
the other driver of the same flag; reword it.

Docs:
- `doc/battery_can_data_spec.md` (~line 289) documents `serial_links` as a
  feature — remove that paragraph.
- `doc/rj45_cable_wiring.md` (~line 156) mentions it as the crosstalk test
  that was run — historical record, leave as is.

## Verification

- `pio run` builds with no unused-function warnings from the removed helpers.
- `POST /cmd` with either name returns `unknown_cmd`.
- Telnet console on port 23 still pauses and resumes the console poller.
- The remaining commands (`set_boiler`, `clear_log`, `restart`) still work.

## Why not now

Waiting for the 24 h soak of `004` on the full cable. If the link were to
fail again, `can_bus_reset` would be the one remote lever left (with its
panic, it still delivers the jam), so it stays until the soak is over. Then
both go in one commit together with closing `002` and `004`.

## Resolution

Closed **2026-09-14**, both commands removed once [`004`](_004-move-can-rx-off-gpio0.md)
was closed and the emergency lever they represented was no longer needed.

Removed:

- `esp_webserver.cpp` — the `serial_links` and `can_bus_reset` branches of
  `handleCommand()` and `SERIAL_PAUSE_MAX_MS`.
- `pylontech_can.cpp` / `.h` — `pylontech_can_force_bus_reset()` and
  `CAN_BUS_RESET_DOMINANT_MS`.
- `inverter_comm.cpp` / `.h` — the whole pause path, not just the setter:
  `inverter_comm_set_paused()`, `inverter_comm_paused()`, `g_inv_paused`, the
  task's pause branch and the `inverter_query_raw()` guard. With the only caller
  gone the flag could never become true again, so leaving it would have left a
  dead branch in the poll loop.
- `doc/battery_can_data_spec.md` — the section documenting the mute as a
  feature. The mention in `rj45_cable_wiring.md` stays: there it is the record
  of a test that was run, not a description of something that exists.

`pylontech_comm_set_paused()` was **kept** — `battery_telnet.cpp` pauses the
console poller with it for the length of a telnet session. Its header comment
and `battery_telnet.h` no longer mention the `serial_links` mute, and `/status`
`slp` now reports that one link rather than the union of two.

The `can_bus_reset` panic documented above was never fixed: the command is gone
instead, which is what this issue proposed.
