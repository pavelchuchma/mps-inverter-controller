#include "pylontech_can.h"
#include <driver/twai.h>
#include <string.h>

// Frame identifiers broadcast by the BMS, see doc/battery_can_spec.md.
#define CAN_ID_LIMITS 0x351   // CVL, CCL, DCL, DVL
#define CAN_ID_SOC 0x355      // SoC, SoH
#define CAN_ID_MEASURED 0x356 // voltage, current, temperature
#define CAN_ID_ALARMS 0x359   // protection / alarm bitmaps, module count
#define CAN_ID_REQUESTS 0x35C // charge / discharge enable request
#define CAN_ID_VENDOR 0x35E   // manufacturer string "PYLON"

// Inverter heartbeat we may have to send, 8 zero bytes at 1 Hz. Some BMS
// firmware revisions stay quiet or raise a communication-loss alarm without
// it, others ignore it. Only started if the bus stays silent, so a battery
// that broadcasts unprompted is never sent anything.
#define CAN_ID_HEARTBEAT 0x305
#define CAN_HEARTBEAT_INTERVAL_MS 1000
// Silence tolerated before the heartbeat is started.
#define CAN_SILENCE_BEFORE_HEARTBEAT_MS 10000

// Per-identifier print throttle. Every frame is printed while its payload
// keeps changing; an unchanged payload is reprinted at most this often, so a
// steady battery does not flood the console at 6 frames/s.
#define CAN_REPRINT_INTERVAL_MS 5000

// Bus health line interval.
#define CAN_STATUS_INTERVAL_MS 30000

static int g_tx_pin = -1;
static int g_rx_pin = -1;

// Last payload printed per identifier, for the throttle.
struct SeenFrame {
  uint32_t id;
  uint8_t dlc;
  uint8_t data[8];
  unsigned long last_print_ms;
};
#define CAN_MAX_SEEN 12
static SeenFrame g_seen[CAN_MAX_SEEN];
static int g_seen_count = 0;

// Little-endian field accessors. The BMS uses 16-bit values throughout; which
// of them are signed is documented in battery_can_spec.md.
static uint16_t u16le(const uint8_t* d) {
  return (uint16_t)(d[0] | (d[1] << 8));
}
static int16_t i16le(const uint8_t* d) {
  return (int16_t)(d[0] | (d[1] << 8));
}

// Decide whether this frame is worth printing: new identifier, changed
// payload, or the throttle interval has elapsed.
static bool should_print(const twai_message_t& m) {
  for (int i = 0; i < g_seen_count; ++i) {
    if (g_seen[i].id != m.identifier) continue;
    bool changed = g_seen[i].dlc != m.data_length_code
                || memcmp(g_seen[i].data, m.data, m.data_length_code) != 0;
    if (!changed && millis() - g_seen[i].last_print_ms < CAN_REPRINT_INTERVAL_MS) {
      return false;
    }
    g_seen[i].dlc = m.data_length_code;
    memcpy(g_seen[i].data, m.data, 8);
    g_seen[i].last_print_ms = millis();
    return true;
  }
  if (g_seen_count < CAN_MAX_SEEN) {
    g_seen[g_seen_count].id = m.identifier;
    g_seen[g_seen_count].dlc = m.data_length_code;
    memcpy(g_seen[g_seen_count].data, m.data, 8);
    g_seen[g_seen_count].last_print_ms = millis();
    g_seen_count++;
  }
  return true;
}

// Print the decoded interpretation of a known identifier. Scaling and byte
// order are the documented Pylontech layout and are exactly what this sniffer
// exists to confirm — compare against the raw bytes on the same line.
static void print_decoded(const twai_message_t& m) {
  const uint8_t* d = m.data;
  switch (m.identifier) {
    case CAN_ID_LIMITS:
      if (m.data_length_code < 8) break;
      Serial.printf("  limits: CVL %.1f V  CCL %.1f A  DCL %.1f A  DVL %.1f V\n",
                    u16le(d + 0) / 10.0f, i16le(d + 2) / 10.0f,
                    i16le(d + 4) / 10.0f, u16le(d + 6) / 10.0f);
      break;
    case CAN_ID_SOC:
      if (m.data_length_code < 4) break;
      Serial.printf("  SoC %u %%  SoH %u %%\n", u16le(d + 0), u16le(d + 2));
      break;
    case CAN_ID_MEASURED:
      if (m.data_length_code < 6) break;
      Serial.printf("  measured: %.2f V  %.1f A  %.1f C\n",
                    i16le(d + 0) / 100.0f, i16le(d + 2) / 10.0f,
                    i16le(d + 4) / 10.0f);
      break;
    case CAN_ID_ALARMS:
      if (m.data_length_code < 5) break;
      // Bit meanings are vendor-specific and not yet verified, so the bitmaps
      // are shown as-is rather than named.
      Serial.printf("  protection 0x%02X%02X  alarm 0x%02X%02X  modules %u\n",
                    d[1], d[0], d[3], d[2], d[4]);
      break;
    case CAN_ID_REQUESTS:
      if (m.data_length_code < 1) break;
      Serial.printf("  requests: charge %s  discharge %s  force-charge %s/%s  full-charge %s\n",
                    (d[0] & 0x80) ? "yes" : "no", (d[0] & 0x40) ? "yes" : "no",
                    (d[0] & 0x20) ? "yes" : "no", (d[0] & 0x10) ? "yes" : "no",
                    (d[0] & 0x08) ? "yes" : "no");
      break;
    case CAN_ID_VENDOR: {
      char s[9] = {0};
      for (int i = 0; i < m.data_length_code && i < 8; ++i) {
        s[i] = (d[i] >= 32 && d[i] < 127) ? (char)d[i] : '.';
      }
      Serial.printf("  vendor: \"%s\"\n", s);
      break;
    }
    default:
      break;
  }
}

static void print_frame(const twai_message_t& m) {
  Serial.printf("[CAN] id=0x%03X dlc=%d raw:", (unsigned)m.identifier, m.data_length_code);
  for (int i = 0; i < m.data_length_code; ++i) Serial.printf(" %02X", m.data[i]);
  Serial.println();
  print_decoded(m);
}

static void print_bus_status(const char* prefix) {
  twai_status_info_t st;
  if (twai_get_status_info(&st) != ESP_OK) return;
  Serial.printf("[CAN] %s state=%d tx_err=%u rx_err=%u bus_err=%u tx_failed=%u missed=%u\n",
                prefix, (int)st.state, (unsigned)st.tx_error_counter,
                (unsigned)st.rx_error_counter, (unsigned)st.bus_error_count,
                (unsigned)st.tx_failed_count, (unsigned)st.rx_missed_count);
}

static bool driver_start() {
  twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(
      (gpio_num_t)g_tx_pin, (gpio_num_t)g_rx_pin, TWAI_MODE_NORMAL);
  g.rx_queue_len = 32;
  twai_timing_config_t t = TWAI_TIMING_CONFIG_500KBITS();
  twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();

  esp_err_t err = twai_driver_install(&g, &t, &f);
  if (err != ESP_OK) {
    Serial.printf("[CAN] twai_driver_install failed: %s\n", esp_err_to_name(err));
    return false;
  }
  err = twai_start();
  if (err != ESP_OK) {
    Serial.printf("[CAN] twai_start failed: %s\n", esp_err_to_name(err));
    twai_driver_uninstall();
    return false;
  }
  return true;
}

// A controller alone on the bus gets no acknowledge, so its error counter
// climbs until it goes bus-off and stops. Recovery is not automatic, so bring
// it back explicitly — otherwise powering the ESP32 up before the battery
// would leave the link dead until a reboot.
static void recover_if_bus_off() {
  twai_status_info_t st;
  if (twai_get_status_info(&st) != ESP_OK) return;
  if (st.state != TWAI_STATE_BUS_OFF) return;

  Serial.println("[CAN] bus-off, recovering");
  if (twai_initiate_recovery() != ESP_OK) return;
  // Recovery completes after 128 idle bus periods; poll for the stopped state.
  for (int i = 0; i < 50; ++i) {
    vTaskDelay(pdMS_TO_TICKS(100));
    if (twai_get_status_info(&st) == ESP_OK && st.state == TWAI_STATE_STOPPED) {
      if (twai_start() == ESP_OK) Serial.println("[CAN] recovered");
      return;
    }
  }
  Serial.println("[CAN] recovery did not complete");
}

static void pylontech_can_task(void* arg) {
  (void)arg;

  if (!driver_start()) {
    vTaskDelete(NULL);
    return;
  }
  Serial.println("[CAN] listening at 500 kbit/s (normal mode, acknowledging)");

  unsigned long last_rx = millis();
  unsigned long last_heartbeat = 0;
  unsigned long last_status = millis();
  bool heartbeat_started = false;
  uint32_t rx_count = 0;

  for (;;) {
    twai_message_t m;
    if (twai_receive(&m, pdMS_TO_TICKS(200)) == ESP_OK) {
      last_rx = millis();
      rx_count++;
      if (should_print(m)) print_frame(m);
      if (heartbeat_started) {
        // The battery talks on its own after all; stop pretending to be an
        // inverter so the capture reflects the real, unprompted broadcast.
        Serial.println("[CAN] frames arriving, stopping the 0x305 heartbeat");
        heartbeat_started = false;
      }
    }

    // Nothing heard for a while: try the inverter heartbeat, in case this BMS
    // firmware waits for it.
    if (!heartbeat_started && millis() - last_rx > CAN_SILENCE_BEFORE_HEARTBEAT_MS) {
      Serial.printf("[CAN] silent for %lu s, starting 0x305 heartbeat at 1 Hz\n",
                    (unsigned long)(CAN_SILENCE_BEFORE_HEARTBEAT_MS / 1000));
      print_bus_status("before heartbeat:");
      heartbeat_started = true;
      last_heartbeat = 0;
    }

    if (heartbeat_started && millis() - last_heartbeat >= CAN_HEARTBEAT_INTERVAL_MS) {
      last_heartbeat = millis();
      twai_message_t hb;
      memset(&hb, 0, sizeof(hb));
      hb.identifier = CAN_ID_HEARTBEAT;
      hb.data_length_code = 8;
      esp_err_t err = twai_transmit(&hb, pdMS_TO_TICKS(100));
      if (err != ESP_OK) {
        // Expected when nothing else is on the bus: no node acknowledges.
        Serial.printf("[CAN] heartbeat transmit failed: %s\n", esp_err_to_name(err));
      }
      recover_if_bus_off();
    }

    if (millis() - last_status >= CAN_STATUS_INTERVAL_MS) {
      last_status = millis();
      Serial.printf("[CAN] %u frames received so far\n", (unsigned)rx_count);
      print_bus_status("status:");
      recover_if_bus_off();
    }
  }
}

void pylontech_can_init(int tx_pin, int rx_pin) {
  g_tx_pin = tx_pin;
  g_rx_pin = rx_pin;
  Serial.printf("[CAN] init, TX=GPIO%d RX=GPIO%d\n", tx_pin, rx_pin);
  xTaskCreatePinnedToCore(pylontech_can_task, "pylontech_can", 4096, NULL, 1, NULL, 1);
}
