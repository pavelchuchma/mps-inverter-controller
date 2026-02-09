#pragma once
#include <Arduino.h>
#include "config.h"

// Initialize the LCD. Call from setup().
void display_init();

// Update first line to show battery SOC (percentage float, e.g. 62.3).
// This only updates the first row; it is safe to call frequently.
void display_update_batt_soc(float soc);

// Update second line to show measured temperatures in °C.
// Format: "Temp: HH.H/LL.LC" where HH.H is High and LL.L is Low.
// This only updates the second row; safe to call periodically.
void display_update_temperature(float temp_h, float temp_l);

// Show Button0 raw touch value.
void display_update_button0(uint16_t value);

// Print formatted text to LCD line (0 or 1), auto-cleared with spaces to 16 chars.
void lcd_printf_line(uint8_t line, const char* fmt, ...);

// Check if LCD backlight is currently on.
inline boolean isBacklightOn();

// Turn on the LCD backlight and reset the timeout counter.
void displayBacklightOn();

// Check and handle LCD backlight timeout (call periodically).
void checkDisplayBacklightTimeout();
