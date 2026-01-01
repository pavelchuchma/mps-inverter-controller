#pragma once
#include <Arduino.h>
#include "config.h"

// Initialize the LCD. Call from setup().
void display_init();

// Update first line to show battery SOC (percentage float, e.g. 62.3).
// This only updates the first row; it is safe to call frequently.
void display_update_batt_soc(float soc);

// Update second line to show measured temperature in °C.
// This only updates the second row; safe to call periodically.
void display_update_temperature(float temp_c);

// Show Button0 raw touch value.
void display_update_button0(uint16_t value);
