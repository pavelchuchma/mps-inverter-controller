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

// --- PWM output ---
#define PWM_PIN 25
