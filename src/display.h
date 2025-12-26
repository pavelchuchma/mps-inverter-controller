#pragma once
#include <Arduino.h>
#include "config.h"

// Initialize the LCD. Call from setup().
void display_init();

// Update first line to show battery SOC (percentage float, e.g. 62.3).
// This only updates the first row; it is safe to call frequently.
void display_update_batt_soc(float soc);
