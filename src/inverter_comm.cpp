#include "inverter_comm.h"
#include <HardwareSerial.h>

// demoMode is defined in main.cpp; when true we should mock data
extern bool demoMode;

static SemaphoreHandle_t g_inv_mutex = NULL;

InverterState g_inverter_status = { 0 };
bool g_inverter_data_valid = false;
float g_temp_h = NAN;
float g_temp_l = NAN;
char g_inverter_mode_code = '\0';
char g_inverter_mode_name[32] = "Unknown";

// CRC-16/XMODEM implementation
static uint16_t crc16_xmodem(const uint8_t* data, size_t len) {
  uint16_t crc = 0x0000;
  for (size_t i = 0; i < len; ++i) {
    crc ^= ((uint16_t)data[i]) << 8;
    for (int b = 0; b < 8; ++b) {
      if (crc & 0x8000) crc = (crc << 1) ^ 0x1021;
      else crc <<= 1;
    }
  }
  return crc & 0xFFFF;
}

static void adjust_crc_bytes(uint8_t& hi, uint8_t& lo) {
  // Device increments reserved values 0x28 '(' , 0x0D CR, 0x0A LF
  const uint8_t RESERVED[] = { 0x28, 0x0D, 0x0A };
  for (uint8_t r : RESERVED) {
    if (hi == r) hi = (hi + 1) & 0xFF;
    if (lo == r) lo = (lo + 1) & 0xFF;
  }
}

// Build frame: payload ASCII + CRC(hi,lo adjusted) + CR
static void build_frame(const String& payload, uint8_t* out, size_t& out_len) {
  size_t plen = payload.length();
  memcpy(out, (const char*)payload.c_str(), plen);
  uint16_t crc = crc16_xmodem((const uint8_t*)payload.c_str(), plen);
  uint8_t hi = (crc >> 8) & 0xFF;
  uint8_t lo = crc & 0xFF;
  adjust_crc_bytes(hi, lo);
  out[plen + 0] = hi;
  out[plen + 1] = lo;
  out[plen + 2] = 0x0D; // CR
  out_len = plen + 3;
}

// Read from Serial1 until CR or timeout. Returns length in bytes stored in buf.
static size_t read_until_cr(HardwareSerial& s, uint8_t* buf, size_t max_len, unsigned long timeout_ms) {
  size_t idx = 0;
  unsigned long start = millis();
  while (idx < max_len) {
    if (s.available()) {
      int b = s.read();
      if (b < 0) break;
      buf[idx++] = (uint8_t)b;
      if ((uint8_t)b == 0x0D) break;
    } else {
      if (millis() - start >= timeout_ms) break;
      vTaskDelay(pdMS_TO_TICKS(2));
    }
  }
  return idx;
}

// Debug helper: print payload (between '(' and CRC), raw hex and ASCII
static void debug_print_rx(const uint8_t* rx, size_t rx_len) {
  if (!rx || rx_len == 0) return;

  bool has_cr = (rx[rx_len - 1] == 0x0D);
  size_t body_len = has_cr ? (rx_len - 1) : rx_len;
  if (body_len >= 3 && rx[0] == 0x28) {
    size_t payload_len_with_paren = body_len - 2; // includes leading '('
    size_t payload_ascii_len = (payload_len_with_paren > 0) ? payload_len_with_paren - 1 : 0;
    if (payload_ascii_len > 0) {
      String payload_str((const char*)(rx + 1), payload_ascii_len);
      Serial.print("[INV] RX (payload): ");
      Serial.println(payload_str);
    }
  }

  Serial.print("[INV] RX (hex): ");
  for (size_t i = 0; i < rx_len; ++i) {
    Serial.printf("%02X ", rx[i]);
  }
  Serial.println();

  Serial.print("[INV] RX (ascii): ");
  Serial.write(rx, rx_len);
  Serial.println();
}

// Send ASCII command and read response. Returns payload (inside '('.. ).
// On CRC mismatch the function prints the raw response and returns false.
static bool send_command_and_get_payload(const String& cmd, String& out_payload) {
  HardwareSerial& ser = Serial1;
  uint8_t tx[128];
  size_t tx_len = 0;
  build_frame(cmd, tx, tx_len);

  // Flush RX and TX buffers
  while (ser.available()) ser.read();
  ser.write(tx, tx_len);
  ser.flush();

  // Wait for response up to 1000ms
  uint8_t rx[512];
  size_t rx_len = read_until_cr(ser, rx, sizeof(rx), 1000);
  if (rx_len == 0) {
    Serial.printf("[INV] No response for cmd '%s'\n", cmd.c_str());
    return false;
  }

  // Response should end with CR
  if (rx[rx_len - 1] != 0x0D) {
    Serial.printf("[INV] Incomplete response for cmd '%s' (no CR)\n", cmd.c_str());
    return false;
  }

  // Print raw response immediately for debugging (before CRC check)
  // debug_print_rx(rx, rx_len);

  // body without CR
  size_t body_len = rx_len - 1;
  if (body_len < 3) {
    Serial.printf("[INV] Response too short for cmd '%s'\n", cmd.c_str());
    return false; // at least '(' + CRC(2)
  }

  uint8_t recv_crc_hi = rx[body_len - 2];
  uint8_t recv_crc_lo = rx[body_len - 1];
  // payload includes leading '('
  size_t payload_len = body_len - 2;

  uint16_t calc = crc16_xmodem(rx, payload_len);
  uint8_t calc_hi = (calc >> 8) & 0xFF;
  uint8_t calc_lo = calc & 0xFF;
  adjust_crc_bytes(calc_hi, calc_lo);
  bool crc_ok = (recv_crc_hi == calc_hi) && (recv_crc_lo == calc_lo);

  if (!crc_ok) {
    Serial.printf("[INV] CRC MISMATCH for cmd '%s' - recv: %02X %02X calc: %02X %02X\n",
      cmd.c_str(), recv_crc_hi, recv_crc_lo, calc_hi, calc_lo);
    // Print raw response (single helper)
    debug_print_rx(rx, rx_len);
    return false; // do not process further when CRC fails
  }

  // Validate leading '('
  if (rx[0] != 0x28) return false; // '('

  out_payload = String((const char*)(rx + 1), payload_len - 1);
  return true;
}

// Parse QMOD payload (first char is code)
static void parse_qmod_payload(const String& p) {
  char code = p.length() ? p.charAt(0) : '\0';
  const char* names[] = { "Power On","Standby","Line","Battery","Fault","Power saving","Unknown" };
  const char map[] = { 'P','S','L','B','F','H','?' };
  const char* name = "Unknown";
  for (size_t i = 0; i < sizeof(map); ++i) {
    if (map[i] == code) { name = names[i]; break; }
  }

  if (g_inv_mutex) xSemaphoreTake(g_inv_mutex, portMAX_DELAY);
  g_inverter_mode_code = code;
  strncpy(g_inverter_mode_name, name, sizeof(g_inverter_mode_name) - 1);
  g_inverter_mode_name[sizeof(g_inverter_mode_name) - 1] = '\0';
  if (g_inv_mutex) xSemaphoreGive(g_inv_mutex);
}

// Parse QPIGS payload tokens and update g_inverter_status
static void parse_qpigs_payload(const String& p) {
  // Tokens separated by a single space
  const int MAX_TOK = 64;
  String toks[MAX_TOK];
  int tcount = 0;
  int start = 0;
  for (int i = 0; i <= (int)p.length() && tcount < MAX_TOK; ++i) {
    if (i == (int)p.length() || p.charAt(i) == ' ') {
      if (i - start > 0) {
        toks[tcount++] = p.substring(start, i);
      }
      start = i + 1;
    }
  }

  // Expect a complete set of items (indexes 0..20 => 21 tokens)
  const int EXPECTED_TOKENS = 21;
  bool valid_data = (tcount >= EXPECTED_TOKENS);

  InverterState s = { 0 };
  if (valid_data) {
    // Full set received -> fill values and mark data as valid
    s.ts_ms = millis();

    s.grid_voltage = toks[0].toFloat();
    s.grid_frequency = toks[1].toFloat();
    s.ac_out_voltage = toks[2].toFloat();
    s.ac_out_frequency = toks[3].toFloat();
    s.ac_apparent_va = toks[4].toInt();
    s.ac_active_w = toks[5].toInt();
    s.load_percent = toks[6].toInt();
    s.bus_voltage = toks[7].toFloat();
    s.batt_voltage = toks[8].toFloat();
    s.batt_charge_current = toks[9].toFloat();
    s.batt_soc = toks[10].toInt();
    s.heatsink_temp = toks[11].toFloat();
    s.pv_input_current = toks[12].toFloat();
    s.pv_input_voltage = toks[13].toFloat();
    s.batt_voltage_from_scc = toks[14].toFloat();
    s.batt_discharge_current = toks[15].toFloat();
    s.device_status_bits = (uint8_t)(toks[16].toInt() & 0xFF);
    s.batt_fan_offset_10mv = toks[17].toInt();
    s.eeprom_version = toks[18].toInt();
    s.pv_charging_power = toks[19].toInt();
    s.additional_status_bits = (uint8_t)(toks[20].toInt() & 0xFF);
  }

  if (g_inv_mutex) xSemaphoreTake(g_inv_mutex, portMAX_DELAY);
  g_inverter_status = s;
  // If we parsed a full set, consider data valid; otherwise remains as previously set
  g_inverter_data_valid = valid_data;
  if (g_inv_mutex) xSemaphoreGive(g_inv_mutex);
}

// Print full status and mode to Serial (thread-safe snapshot)
static void print_status_and_mode_snapshot() {
  InverterState s;
  char mode_code = '\0';
  char mode_name[32] = { 0 };
  bool valid;

  if (g_inv_mutex) xSemaphoreTake(g_inv_mutex, portMAX_DELAY);
  s = g_inverter_status;
  mode_code = g_inverter_mode_code;
  strncpy(mode_name, g_inverter_mode_name, sizeof(mode_name) - 1);
  valid = g_inverter_data_valid;
  if (g_inv_mutex) xSemaphoreGive(g_inv_mutex);

  Serial.println("--- Inverter Status Snapshot ---");
  if (!valid) {
    Serial.println("Read failed, no data available");
  } else {
    Serial.printf("Mode: %c (%s)\n", mode_code ? mode_code : '?', mode_name);
    Serial.printf("Grid V: %.2f V, Grid F: %.2f Hz\n", s.grid_voltage, s.grid_frequency);
    Serial.printf("AC Out V: %.2f V, AC Out F: %.2f Hz\n", s.ac_out_voltage, s.ac_out_frequency);
    Serial.printf("Apparent VA: %d VA, Active W: %d W, Load %%: %d\n", s.ac_apparent_va, s.ac_active_w, s.load_percent);
    Serial.printf("BUS V: %.2f V, Batt V: %.2f V, Batt Charge I: %.2f A, Batt SOC: %d %%\n", s.bus_voltage, s.batt_voltage, s.batt_charge_current, s.batt_soc);
    Serial.printf("Heatsink: %.2f C, PV I: %.2f A, PV V: %.2f V\n", s.heatsink_temp, s.pv_input_current, s.pv_input_voltage);
    Serial.printf("Batt V from SCC: %.2f V, Batt Disch I: %.2f A\n", s.batt_voltage_from_scc, s.batt_discharge_current);
    Serial.printf("Device status bits: 0x%02X, Additional status bits: 0x%02X\n", s.device_status_bits, s.additional_status_bits);
    Serial.printf("Batt fan offset: %d (10mV), EEPROM ver: %d, PV charging power: %d W\n", s.batt_fan_offset_10mv, s.eeprom_version, s.pv_charging_power);
    Serial.printf("Timestamp: %u ms\n", (unsigned)s.ts_ms);
  }
  Serial.println("---------------------------------");
}

// Demo-mode mock tick: updates only selected properties and timestamp.
// Thread-safe: reads previous state under mutex, modifies required fields, writes back.
static void inverter_mock_tick() {
  InverterState s;
  if (g_inv_mutex) xSemaphoreTake(g_inv_mutex, portMAX_DELAY);
  s = g_inverter_status; // start from last state to preserve other fields
  if (g_inv_mutex) xSemaphoreGive(g_inv_mutex);

  static float t = 0.0f;
  t += 0.12f;

  // Mock required properties
  s.grid_voltage = 230.0f + 5.0f * sinf(t * 0.7f);
  s.load_percent = (int)(35 + 25 * (sinf(t * 0.5f) * 0.5f + 0.5f));
  s.batt_voltage = 52.1f + 0.45f * sinf(t * 0.6f);
  s.batt_charge_current = 8.0f + 3.0f * sinf(t * 0.9f);
  s.batt_soc = (int)(60 + 12 * sinf(t * 0.25f));
  s.pv_input_current = 10.0f + 4.0f * sinf(t);
  s.pv_input_voltage = 280.0f + 15.0f * sinf(t * 0.4f);
  s.pv_charging_power = (int)(1350 + 300 * sinf(t));
  s.ts_ms = millis();

  if (g_inv_mutex) xSemaphoreTake(g_inv_mutex, portMAX_DELAY);
  g_inverter_status = s;
  g_inverter_data_valid = true; // In demo mode we always claim data are valid
  if (g_inv_mutex) xSemaphoreGive(g_inv_mutex);
}

// Background task that queries QMOD and QPIGS periodically
static void inverter_task(void* arg) {
  (void)arg;
  for (;;) {
    if (demoMode) {
      inverter_mock_tick();
    } else {
      // Real mode: query inverter
      // QMOD
      String payload;
      bool failed = false;
      if (send_command_and_get_payload("QMOD", payload)) {
        parse_qmod_payload(payload);
      } else {
        failed = true;
      }

      // QPIGS
      payload = String();
      if (send_command_and_get_payload("QPIGS", payload)) {
        parse_qpigs_payload(payload);
      } else {
        failed = true;
      }
      
      if (failed) {
        // On any failure, mark data as invalid
        if (g_inv_mutex) xSemaphoreTake(g_inv_mutex, portMAX_DELAY);
        g_inverter_data_valid = false;
        if (g_inv_mutex) xSemaphoreGive(g_inv_mutex);
      }

      // Print snapshot after each poll cycle
      print_status_and_mode_snapshot();
    }

    vTaskDelay(pdMS_TO_TICKS(INVERTER_POLL_INTERVAL_MS));
  }
}

void inverter_comm_init() {
  if (!g_inv_mutex) {
    g_inv_mutex = xSemaphoreCreateMutex();
  }
  // Initialize Serial1 for RS232 via MAX3232 at 2400 8N1
  Serial1.begin(2400, SERIAL_8N1, INVERTER_RX_PIN, INVERTER_TX_PIN);

  // Create background task
  xTaskCreatePinnedToCore(
    inverter_task,
    "inverter_task",
    4096,
    NULL,
    1,
    NULL,
    1);
}

bool inverter_get_status(InverterState* out) {
  if (!out) return false;
  if (g_inv_mutex) xSemaphoreTake(g_inv_mutex, portMAX_DELAY);
  *out = g_inverter_status;
  if (g_inv_mutex) xSemaphoreGive(g_inv_mutex);
  return true;
}

bool inverter_get_mode(char* out_code, char* out_name, size_t name_cap) {
  if (out_code) {
    if (g_inv_mutex) xSemaphoreTake(g_inv_mutex, portMAX_DELAY);
    *out_code = g_inverter_mode_code;
    if (g_inv_mutex) xSemaphoreGive(g_inv_mutex);
  }
  if (out_name && name_cap > 0) {
    if (g_inv_mutex) xSemaphoreTake(g_inv_mutex, portMAX_DELAY);
    strncpy(out_name, g_inverter_mode_name, name_cap - 1);
    out_name[name_cap - 1] = '\0';
    if (g_inv_mutex) xSemaphoreGive(g_inv_mutex);
  }
  return true;
}
