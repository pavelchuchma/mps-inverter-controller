#pragma once
#include <Arduino.h>
#include "config.h"

#define DISPLAY_MAX_ROWS 8

// Initialize the LCD. Call from setup().
void display_init();

// --- Scrollable row-based display ---

// Set total number of logical rows (max DISPLAY_MAX_ROWS).
void display_set_row_count(uint8_t count);

// Update one row's text (index < row_count). Does NOT redraw.
void display_set_row(uint8_t index, const char* text);

// Redraw visible rows to LCD (only writes lines that actually changed).
void display_redraw();

// Scroll up/down cyclically and redraw.
void display_scroll_up();
void display_scroll_down();

// Print formatted text to LCD line (0 or 1), auto-cleared with spaces to 16 chars.
// Bypasses the row system — use for transient messages (e.g. WiFi connect).
void lcd_printf_line(uint8_t line, const char* fmt, ...);

// Check if LCD backlight is currently on.
inline boolean isBacklightOn();

// Turn on the LCD backlight and reset the timeout counter.
void displayBacklightOn();

// Check and handle LCD backlight timeout (call periodically).
void checkDisplayBacklightTimeout();
