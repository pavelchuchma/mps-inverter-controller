#pragma once
#include <Arduino.h>

// Centralized pin configuration for the entire project.
// If you change the wiring, adjust values here and NOT in individual modules.

// --- Inverter UART (via MAX3232) ---
// Feather ESP32 default: RX2=GPIO16, TX2=GPIO17
#define INVERTER_RX_PIN 16
#define INVERTER_TX_PIN 17

// --- LCD QC1602A (4-bit parallel mode) ---
#define LCD_RS 21
#define LCD_EN 22
#define LCD_D4 19
#define LCD_D5 18
#define LCD_D6 5
#define LCD_D7 23

// LCD backlight control (via NPN transistor)
// Connected to GPIO26; drive HIGH to turn backlight ON, LOW to turn OFF.
#define LCD_BACKLIGHT_PIN 26

// --- PWM output ---
#define PWM_PIN 25

// --- Capacitive touch inputs (ESP32 Touch0–Touch3) ---
// Mapped in code as Button0–Button3
// Note: On ESP32, Touch0..3 correspond to GPIO4, GPIO0, GPIO2, GPIO15 respectively.
// Some of these are strapping pins (0, 2, 15) — ensure your hardware accounts for this.
#define BUTTON0_TOUCH T0  // Touch0 (GPIO4)
#define BUTTON1_TOUCH T1  // Touch1 (GPIO0)
#define BUTTON2_TOUCH T2  // Touch2 (GPIO2)
#define BUTTON3_TOUCH T3  // Touch3 (GPIO15)

// Touch threshold for detecting a "press" on Button0.
// Raw values vary by board/environment; adjust after observing `display_update_button0` readings.
// Backlight is activated for 5s on a falling-below-threshold edge.
#define BUTTON0_TOUCH_THRESHOLD 35
