#pragma once

// Periodic metrics upload to InfluxDB 2.x.
// Samples all web-UI values every METRICS_SAMPLE_INTERVAL_MS into a RAM buffer
// and flushes the whole buffer in a single line-protocol HTTP POST every
// METRICS_SAMPLES_PER_FLUSH samples (see config.h). Reads only the existing
// thread-safe accessors, so it holds no shared state of its own.

// Start the background upload task. Call once after WiFi/WireGuard are up.
void influx_init();
