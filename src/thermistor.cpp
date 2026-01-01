#include "thermistor.h"
#include <cmath>

// Local module constants (for NTC 10k B3950 with 10k divider / 3.3V)
static constexpr float TH_R_SERIES_OHMS = 10000.0f;   // series resistor (ohms)
static constexpr float TH_R0_OHMS       = 10000.0f;   // NTC at T0 (ohms)
static constexpr float TH_BETA          = 3950.0f;    // Beta constant (K)
static constexpr float TH_T0_K          = 298.15f;    // 25 °C in Kelvin
static constexpr float TH_VSUPPLY_MV    = 3300.0f;    // divider supply voltage (mV)

float read_thermistor_temp_c(int adc_pin) {
  // Sample in mV and average for stability
  const int samples = 16;
  long sumMv = 0;
  for (int i = 0; i < samples; ++i) {
    int mv = analogReadMilliVolts(adc_pin);
    sumMv += mv;
    delayMicroseconds(500);
  }
  float vout_mv = (float)sumMv / (float)samples;

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
