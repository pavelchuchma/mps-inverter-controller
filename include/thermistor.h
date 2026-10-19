#pragma once
#include <Arduino.h>

// Read temperature from an NTC 10k B3950 connected in a voltage divider.
// - adc_pin: ADC pin number (e.g., 34 for GPIO34/ADC1_CH6)
// Returns: temperature in °C; on error/invalid reading returns NAN.
float read_thermistor_temp_c(int adc_pin);

// Rolling median over the last TEMP_MEDIAN_WINDOW readings (one per second).
// NAN readings occupy a slot but are not counted; the median is NAN while they
// make up half of the filled window or more.
static constexpr int TEMP_MEDIAN_WINDOW = 60;

struct TempMedian {
  float buf[TEMP_MEDIAN_WINDOW];
  int count = 0;  // filled slots
  int next = 0;   // slot the next reading goes to
};

// Stores the reading and returns the median of the window.
float temp_median_push(TempMedian& m, float reading);
