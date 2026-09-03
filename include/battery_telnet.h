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
//   - Battery data goes invalid, so relay.cpp forces the boiler off. A session
//     means the boiler does not heat.
//   - The session is capped at BATTERY_TELNET_SESSION_MS and the pause deadline
//     is deliberately longer, so polling can never resume underneath a live
//     session and start injecting 'pwr' into it.
//
// Debug/test tool only — do not use it at the same time as the web UI's
// serial_links mute, both drive the same pause flag.
// A session shows up in GET /status as `slp` (serial links paused), same as the
// web UI mute, and in app.log as the [TELNET] session open/close lines.
void battery_telnet_init();
