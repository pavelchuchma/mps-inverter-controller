#pragma once
#include <Arduino.h>

// Raw TCP/telnet bridge to the Pylontech console UART (Serial2).
//
// Connecting a client hands it the battery console directly: bytes typed go to
// the console, everything the console says comes back. This is the only way to
// run console commands other than 'pwr' remotely, since nobody can reach the
// hardware.
//
// The periodic 'pwr' poll cannot share the UART, so a session pauses it via
// pylontech_comm_set_paused() for the whole time it is connected. That has two
// consequences that are load-bearing, not incidental:
//   - Console battery data goes invalid: the LCD shows `--`, /status reports
//     `bav: false` and chajda-battery gets a gap. Boiler regulation is not
//     affected, it runs on the CAN link (relay.cpp).
//   - The session is capped at BATTERY_TELNET_SESSION_MS and the pause deadline
//     is deliberately longer, so polling can never resume underneath a live
//     session and start injecting 'pwr' into it.
//
// A session shows up in GET /status as `slp` (console link paused) and in
// app.log as the [TELNET] session open/close lines.
void battery_telnet_init();
