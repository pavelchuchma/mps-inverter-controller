#pragma once
#include <Arduino.h>

// ---------------------------------------------------------------------------
// Pylontech US5000 CAN link — bring-up sniffer (stage 1 of
// doc/battery_can_spec.md).
//
// Receives the BMS broadcast and prints it decoded to Serial. No parsed state
// is published yet: byte order and scaling have to be confirmed against real
// captures first, which is what this module is for. Every line carries the raw
// payload next to the decoded values so the two can be checked against each
// other.
//
// The driver runs in TWAI_MODE_NORMAL because the ESP32 is the only other node
// on the battery's CAN port. CAN needs an acknowledge bit from a second node;
// in listen-only mode the battery would never see one, would retransmit every
// frame and drop into error-passive. Normal mode acknowledges without
// transmitting anything of our own — that part is handled by the controller,
// not by application code.
// ---------------------------------------------------------------------------

// Install the TWAI driver on the given pins and start the receive task.
void pylontech_can_init(int tx_pin, int rx_pin);
