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
// RELAY_BOILER_B_VERIFY_PIN GPIO36 |  3 || 22 | GPIO22  LCD_EN
//          BATTERY_RX_PIN   GPIO39 |  4 || 23 | GPIO1   Serial TX0
//         THERMISTOR_L_PIN  GPIO34 |  5 || 24 | GPIO3   Serial RX0
//         THERMISTOR_H_PIN  GPIO35 |  6 || 25 | GPIO21  LCD_RS
//          BATTERY_TX_PIN   GPIO32 |  7 || 26 | GND
//           BOILER_ON_PIN   GPIO33 |  8 || 27 | GPIO19  LCD_D4
//     RELAY_MOBILE_CHARGER  GPIO25 |  9 || 28 | GPIO18  LCD_D5
//        LCD_BACKLIGHT_PIN  GPIO26 | 10 || 29 | GPIO5   LCD_D6 (boot)
//          RELAY_BOILER_A   GPIO27 | 11 || 30 | GPIO17  INVERTER_RX_PIN
//          RELAY_BOILER_B   GPIO14 | 12 || 31 | GPIO16  INVERTER_TX_PIN
// BATTERY_CAN_TX_PIN (boot) GPIO12 | 13 || 32 | GPIO4   BATTERY_CAN_RX_PIN
//                              GND | 14 || 33 | GPIO0   (boot, auto-reset only)
//          RELAY_BOILER_C   GPIO13 | 15 || 34 | GPIO2   BTN_UP_TOUCH (boot)
//                    (SPI)  GPIO9  | 16 || 35 | GPIO15  BTN_DOWN_TOUCH (boot)
//                    (SPI)  GPIO10 | 17 || 36 | GPIO8   (SPI)
//                    (SPI)  GPIO11 | 18 || 37 | GPIO7   (SPI)
//                              5V0 | 19 || 38 | GPIO6   (SPI)
//
// GPIO6-11 = internal SPI flash, do not use
// (boot) = strapping pin, affects boot behavior

// --- Metrics upload to InfluxDB ---
// Sample all UI values every 10s into a RAM buffer; flush the whole buffer to
// InfluxDB in a single HTTP POST once per minute (6 samples/batch). Keeps the
// 10s resolution for retrospective power-management analysis while sparing the
// RPi's SD card (one request/min instead of six).
#define METRICS_SAMPLE_INTERVAL_MS 10000
#define METRICS_SAMPLES_PER_FLUSH  6
#define METRICS_HTTP_TIMEOUT_MS    5000

// --- Inverter UART (via MAX3232) ---
// Feather ESP32 default: RX2=GPIO16, TX2=GPIO17
#define INVERTER_RX_PIN 17
#define INVERTER_TX_PIN 16

// --- Battery UART ---
#define BATTERY_RX_PIN 39  // SVN, input-only — sufficient for RX
#define BATTERY_TX_PIN 32

// --- Battery CAN (via TJA1050) ---
// See doc/battery_can_spec.md "Pin assignment rationale" and doc/todo/004.
// GPIO12 (strapping) needs an external 2.2k pull-down or the board will not
// boot. RX sits behind a 4.7k/10k divider from the transceiver's 5 V RXD on
// GPIO4 — NOT GPIO0: the USB-serial auto-reset transistor loads GPIO0 whenever
// the CP2102 is powered with its port closed and corrupts CAN reception
// (root cause of todo 002). GPIO0 is left to the auto-reset circuit only.
#define BATTERY_CAN_TX_PIN 12
#define BATTERY_CAN_RX_PIN 4

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
// Boiler element control: see doc/heater_relay_control_spec.md and
// doc/boiler_relay_schema.png for the SPDT wiring of A, B, C.
#define RELAY_BOILER_A            27
#define RELAY_BOILER_B            14
#define RELAY_BOILER_C            13
#define RELAY_MOBILE_CHARGER 25

// --- Inputs ---
#define BOILER_ON_PIN 33  // supports INPUT_PULLDOWN

// Opto-isolated AC voltage detector wired across relay B's NO contact
// (between node Y = R1.bottom/R2.top and N = B.COM). Reads HIGH (cold)
// when B has physically energized to NO; LOW (hot) when B is still in NC
// while the load is energized. Used by tickBoiler() to verify B's actual
// state before/while toggling relay C. GPIO36 is input-only (ADC1_CH0)
// and has NO internal pull-up — opto module must provide its own pull-up
// on the output line, or add an external 10k to 3V3.
#define RELAY_BOILER_B_VERIFY_PIN 36

// --- Capacitive touch inputs (ESP32 Touch) ---
// Physical button positions: Up, Down

// GPIO2 is a strapping pin (must read low at boot for normal boot and UART
// download). A bare touch pad may float, so fit an external pull-down
// (e.g. 10k to GND) on GPIO2; touchRead() needs no pinMode() of its own.
// If GPIO2 proves unreliable, dropping BTN_UP is acceptable (web UI covers it).
#define BTN_UP_TOUCH 2  // Touch2 (GPIO2)
#define BTN_DOWN_TOUCH 15  // Touch3 (GPIO15)

// Press detection is adaptive. On the classic ESP32 touchRead() returns a
// *lower* count when the pad is touched (measured: idle 44-54, pressed 0-20),
// so the idle baseline is the maximum of a smoothed reading over the last
// BTN_TOUCH_BASELINE_WINDOW_S seconds and a press is a sudden drop of at least
// BTN_TOUCH_PRESS_DELTA counts below it. A held finger can never raise the
// baseline, and slow drift (humidity, temperature, pad wear) is followed
// automatically. The buttons are inactive until the window has filled once
// after boot. A fixed threshold (30) previously drifted into "never reacts" or
// "fires constantly" depending on conditions at the cottage.
#define BTN_TOUCH_BASELINE_WINDOW_S 15
#define BTN_TOUCH_PRESS_DELTA 20    // raw <= baseline - 20 -> pressed
#define BTN_TOUCH_RELEASE_DELTA 12  // raw >= baseline - 12 -> released (hysteresis)
#define BTN_TOUCH_CONFIRM_SAMPLES 2 // consecutive 50 ms samples needed for a press

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
