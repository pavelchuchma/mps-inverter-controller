#pragma once
#include <Arduino.h>

// Read temperature from an NTC 10k B3950 connected in a voltage divider.
// - adc_pin: ADC pin number (e.g., 34 for GPIO34/ADC1_CH6)
// Returns: temperature in °C; on error/invalid reading returns NAN.
float read_thermistor_temp_c(int adc_pin);
