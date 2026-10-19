#include "thermistor.h"
#include <algorithm>
#include <cmath>

// Local module constants (for NTC 10k B3950 with 10k divider / 3.3V)
static constexpr float TH_R_SERIES_OHMS = 10000.0f;   // series resistor (ohms)
static constexpr float TH_R0_OHMS       = 10000.0f;   // NTC at T0 (ohms)
static constexpr float TH_BETA          = 3950.0f;    // Beta constant (K)
static constexpr float TH_T0_K          = 298.15f;    // 25 °C in Kelvin
static constexpr float TH_VSUPPLY_MV    = 3300.0f;    // divider supply voltage (mV)

float read_thermistor_temp_c(int adc_pin) {
  // Spread the samples over a full 50 Hz period and take the median, so single
  // ADC outliers (WiFi bursts, switching noise) do not shift the result.
  const int samples = 21;
  int mv[samples];
  for (int i = 0; i < samples; ++i) {
    mv[i] = analogReadMilliVolts(adc_pin);
    delayMicroseconds(1000);
  }
  std::nth_element(mv, mv + samples / 2, mv + samples);
  float vout_mv = (float)mv[samples / 2];

  // Guard against rail limits
  if (vout_mv <= 0.1f || vout_mv >= (TH_VSUPPLY_MV - 0.1f)) {
    return NAN;
  }

  // Compute NTC resistance from divider
  // Vout = Vs * Rntc / (Rser + Rntc)  => Rntc = Rser * Vout / (Vs - Vout)
  float denom = (TH_VSUPPLY_MV - vout_mv);
  if (!(denom > 0.0f)) return NAN;
  float r_ntc = TH_R_SERIES_OHMS * (vout_mv) / denom;
  if (!(r_ntc > 0.0f)) return NAN;

  // Beta equation: 1/T = 1/T0 + (1/B)*ln(R/R0)
  float invT = (1.0f / TH_T0_K) + (1.0f / TH_BETA) * logf(r_ntc / TH_R0_OHMS);
  float tK = 1.0f / invT;
  float tC = tK - 273.15f;
  return tC;
}

float temp_median_push(TempMedian& m, float reading) {
  m.buf[m.next] = reading;
  m.next = (m.next + 1) % TEMP_MEDIAN_WINDOW;
  if (m.count < TEMP_MEDIAN_WINDOW) ++m.count;

  float valid[TEMP_MEDIAN_WINDOW];
  int n = 0;
  for (int i = 0; i < m.count; ++i) {
    if (!isnan(m.buf[i])) valid[n++] = m.buf[i];
  }
  if (n * 2 <= m.count) return NAN;
  std::nth_element(valid, valid + n / 2, valid + n);
  return valid[n / 2];
}
