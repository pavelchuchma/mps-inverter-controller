#pragma once
#include <Arduino.h>

// Raw TCP/telnet bridge to the Pylontech console UART (Serial2).
//
// Connecting a client hands it the battery console directly: bytes typed go to
// the console, everything the console says comes back. This is the only way to
// run console commands other than 'pwr' remotely, since nobody can reach the
// hardware.
//
// This bridge is the only user of the console UART: the firmware takes all
// battery telemetry from the CAN link (pylontech_can.h), and the periodic
// 'pwr' poll that used to share this port was removed. The bridge therefore
// owns Serial2 outright and opens it in battery_telnet_init().
//
// A session is capped at BATTERY_TELNET_SESSION_MS so a client that goes
// away without closing the socket cannot hold the port forever; only one
// client is served at a time. Sessions show up in app.log as the [TELNET]
// open/close lines.
void battery_telnet_init(int rx_pin, int tx_pin);
