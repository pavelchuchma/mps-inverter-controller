#pragma once
#include <Arduino.h>

// Centralized pin configuration for the entire project.
// If you change the wiring, adjust values here and NOT in individual modules.
//
// ESP32-WROOM-32 (38-pin DevKit) pinout:
// (keep columns aligned after every change)
//
//                              3V3 |  1 || 20 | GND
//                           EN/RST |  2 || 21 | GPIO23  LCD_D7
//           BOILER_ON_PIN   GPIO36 |  3 || 22 | GPIO22  LCD_EN
//          BATTERY_RX_PIN   GPIO39 |  4 || 23 | GPIO1   Serial TX0
//         THERMISTOR_L_PIN  GPIO34 |  5 || 24 | GPIO3   Serial RX0
//         THERMISTOR_H_PIN  GPIO35 |  6 || 25 | GPIO21  LCD_RS
//          BATTERY_TX_PIN   GPIO32 |  7 || 26 | GND
//                   (free)  GPIO33 |  8 || 27 | GPIO19  LCD_D4
//     RELAY_MOBILE_CHARGER  GPIO25 |  9 || 28 | GPIO18  LCD_D5
//        LCD_BACKLIGHT_PIN  GPIO26 | 10 || 29 | GPIO5   LCD_D6 (boot)
//   RELAY_BOILER_COMMON_N   GPIO27 | 11 || 30 | GPIO17  INVERTER_TX_PIN
//         RELAY_BOILER_L2   GPIO14 | 12 || 31 | GPIO16  INVERTER_RX_PIN
//                   (boot)  GPIO12 | 13 || 32 | GPIO4   BTN_UP_TOUCH
//                              GND | 14 || 33 | GPIO0   (boot)
//         RELAY_BOILER_L1   GPIO13 | 15 || 34 | GPIO2   (boot)
//                    (SPI)  GPIO9  | 16 || 35 | GPIO15  BTN_DOWN_TOUCH (boot)
//                    (SPI)  GPIO10 | 17 || 36 | GPIO8   (SPI)
//                    (SPI)  GPIO11 | 18 || 37 | GPIO7   (SPI)
//                              5V0 | 19 || 38 | GPIO6   (SPI)
//
// GPIO6-11 = internal SPI flash, do not use
// (boot) = strapping pin, affects boot behavior

// --- Inverter UART (via MAX3232) ---
// Feather ESP32 default: RX2=GPIO16, TX2=GPIO17
#define INVERTER_RX_PIN 16
#define INVERTER_TX_PIN 17

// --- Battery UART ---
#define BATTERY_RX_PIN 39  // SVN, input-only — sufficient for RX
#define BATTERY_TX_PIN 32

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

// --- Relay outputs ---
#define RELAY_BOILER_L1           13
#define RELAY_BOILER_L2           14
#define RELAY_BOILER_COMMON_N     27
#define RELAY_MOBILE_CHARGER 25

// --- Inputs ---
#define BOILER_ON_PIN 36  // SVP, input-only

// --- Capacitive touch inputs (ESP32 Touch) ---
// Physical button positions: Up, Down

#define BTN_UP_TOUCH    4   // Touch0 (GPIO4)
#define BTN_DOWN_TOUCH 15  // Touch3 (GPIO15)

// Touch threshold for detecting a press. Raw values vary by board/environment.
#define BTN_TOUCH_THRESHOLD 35

// --- NTC Thermistor (temperature sensor) ---
// Wiring per: https://www.smartlab.at/a-diy-guide-measuring-water-temperature-with-an-ntc-10k-thermistor-and-esp32/
// Use GPIO34 (ADC1_CH6) as analog input. Divider: 3.3V --- R_SERIES(10k) ---[ADC pin]--- NTC(10k) --- GND
// Note: GPIO34 is input-only on ESP32, suitable for ADC.
#define THERMISTOR_L_PIN 34
#define THERMISTOR_H_PIN 35

// At microsecond speeds, the functions from gpio.h are too heavy
#define GPIO_FAST_SET_1(gpio_num) GPIO.out_w1ts |= (0x1 << gpio_num)
#define GPIO_FAST_SET_0(gpio_num) GPIO.out_w1tc |= (0x1 << gpio_num)
#define GPIO_FAST_OUTPUT_ENABLE(gpio_num) GPIO.enable_w1ts |= (0x1 << gpio_num)
#define GPIO_FAST_GET_LEVEL(gpio_num) ((GPIO.in >> gpio_num) & 0x1)
