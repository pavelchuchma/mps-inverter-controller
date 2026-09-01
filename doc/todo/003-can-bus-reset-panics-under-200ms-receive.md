---
id: 003
title: can_bus_reset panics the ESP while the CAN task sits in twai_receive
type: bug
status: open
priority: medium
component: battery-can
created: 2026-09-01
---

# can_bus_reset panics the ESP while the CAN task sits in twai_receive

## Summary

`POST /cmd {"name":"can_bus_reset"}` at 13:29:40 on 2026-09-01 rebooted the
controller with `ESP_RST_PANIC` about one second into the 2 s dominant hold.

`pylontech_can_force_bus_reset()` runs on the webserver task and calls
`twai_stop()` + `twai_driver_uninstall()` while the CAN task keeps looping.
Since `7488cfe` the receive timeout is 200 ms, so the CAN task is inside
`twai_receive()` essentially always — uninstalling the driver under it is a
near-guaranteed crash. Under the earlier 20 ms loop the same race existed but
lost far more often (both invocations on 2026-08-31 survived).

## Fix sketch

The reset must be executed *by the CAN task itself*, not under it: set a
`g_bus_reset_requested` flag from the webserver handler, have the task check
it once per loop iteration and perform stop/uninstall/dominant-hold/restart
from its own context. The handler waits (with timeout) for a result flag, or
just returns "scheduled".

The accidental effect was still delivered — the panic boot jams the bus via
the GPIO12 pull-down like any other boot — which is why this stayed hidden
behind the very behaviour the command imitates.
