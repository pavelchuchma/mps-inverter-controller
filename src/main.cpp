#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include "credentials.h"
#include "config.h"
#include "thermistor.h"
#include "esp_webserver.h"
#include "display.h"
#include "inverter_comm.h"
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdarg.h>
#include <LittleFS.h>
#include <driver/adc.h>
#include <math.h>
#include <WireGuard-ESP32.h>

IPAddress apIP(192, 168, 4, 1);
IPAddress netMsk(255, 255, 255, 0);

static WireGuard wg;

WebServer server(80);

// Format boot-relative milliseconds to HH:MM:SS.sss into provided buffer.
static inline void formatBootTimeMs(char* buf, size_t cap, uint32_t ms) {
  uint32_t total_ms = ms;
  uint32_t total_sec = total_ms / 1000;
  unsigned h = total_sec / 3600;
  unsigned m = (total_sec % 3600) / 60;
  unsigned s = total_sec % 60;
  unsigned ms_part = total_ms % 1000;
  if (cap > 0) {
    snprintf(buf, cap, "%02u:%02u:%02u.%03u", h, m, s, ms_part);
  }
}

// Print a WARN log with boot-relative timestamp. Accepts printf-style args.
static inline void printWarning(const char* fmt, ...) {
  char tbuf[16];
  formatBootTimeMs(tbuf, sizeof(tbuf), millis());
  char msg[384];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  char out[420];
  snprintf(out, sizeof(out), "[%s] [WARN] %s\n", tbuf, msg);
  Serial.print(out);
  // Append warning into LittleFS logfile
  if (LittleFS.begin()) {
    File f = LittleFS.open("/app.log", "a");
    if (f) {
      f.print(out);
      f.close();
    }
  }
}

// --------- App state ----------
// ---- Reset reason (persisted from setup) ----
static esp_reset_reason_t g_reset_reason = ESP_RST_UNKNOWN;
static const char* g_reset_reason_str = "UNKNOWN";

static const char* resetReasonToStr(esp_reset_reason_t r) {
  switch (r) {
  case ESP_RST_UNKNOWN:      return "ESP_RST_UNKNOWN: Reset reason can not be determined";
  case ESP_RST_POWERON:      return "ESP_RST_POWERON: Reset due to power-on event";
  case ESP_RST_EXT:          return "ESP_RST_EXT: Reset by external pin (not applicable for ESP32)";
  case ESP_RST_SW:           return "ESP_RST_SW: Software reset via esp_restart";
  case ESP_RST_PANIC:        return "ESP_RST_PANIC: Software reset due to exception/panic";
  case ESP_RST_INT_WDT:      return "ESP_RST_INT_WDT: Reset due to interrupt watchdog";
  case ESP_RST_TASK_WDT:     return "ESP_RST_TASK_WDT: Reset due to task watchdog";
  case ESP_RST_WDT:          return "ESP_RST_WDT: Reset due to other watchdogs";
  case ESP_RST_DEEPSLEEP:    return "ESP_RST_DEEPSLEEP: Reset after exiting deep sleep mode";
  case ESP_RST_BROWNOUT:     return "ESP_RST_BROWNOUT: Brownout reset (software or hardware)";
  case ESP_RST_SDIO:         return "ESP_RST_SDIO: Reset over SDIO";
  default:                   return "OTHER";
  }
}

int outputLimitW = 2000;
float outputDutyCycle = 0.0f; // 0.0 - 1.0 (represented as percent in UI)

// --- Display row definitions ---
enum DisplayRow : uint8_t {
  ROW_SOC = 0,
  ROW_TEMP,
  ROW_PV_POWER,
  ROW_BATT_POWER,
  ROW_COUNT
};


// --------- Embedded web UI helpers (LittleFS) ----------

void connectToWiFi() {
  displayBacklightOn();
  lcd_printf_line(0, "WiFi: %s", WIFI_SSID);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint16_t connectWaiting = 0;
  while (WiFi.status() != WL_CONNECTED) {
    lcd_printf_line(1, "%ds...", connectWaiting / 4);
    delay(250);
    Serial.printf(".");
    if (connectWaiting++ > 240) { // ~60 seconds
      Serial.printf("Failed to connect, restarting!\n");
      ESP.restart();
    }
  }
  lcd_printf_line(1, "Pripojeno");
}

void createWiFiAP() {
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, apIP, netMsk);
  WiFi.softAP(AP_SSID, AP_PASS);

  Serial.print("AP IP: "); Serial.println(WiFi.softAPIP());
}

void syncNTP() {
  lcd_printf_line(1, "NTP sync...");
  Serial.println("NTP: synchronizing time...");
  // CET-1CEST,M3.5.0,M10.5.0/3 = Czech timezone with automatic DST
  configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.nist.gov");

  time_t now = time(nullptr);
  int attempts = 0;
  while (now < 24 * 3600 && attempts < 20) {
    delay(500);
    now = time(nullptr);
    attempts++;
  }

  if (now < 24 * 3600) {
    Serial.println("NTP: sync failed (timeout)");
    lcd_printf_line(1, "NTP FAIL");
  } else {
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    Serial.printf("NTP: synced, %02d:%02d:%02d (%s)\n",
                  timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec,
                  timeinfo.tm_isdst ? "CEST" : "CET");
    lcd_printf_line(1, "NTP OK");
  }
}

void initWireGuard() {
  lcd_printf_line(1, "WireGuard...");
  Serial.printf("WireGuard: connecting to %s:%d\n", WG_ENDPOINT, WG_ENDPOINT_PORT);

  IPAddress localIp;
  localIp.fromString(WG_LOCAL_IP);
  wg.begin(localIp, WG_PRIVATE_KEY, WG_ENDPOINT, WG_PEER_PUBLIC_KEY, WG_ENDPOINT_PORT);

  Serial.printf("WireGuard: tunnel up, local IP %s\n", WG_LOCAL_IP);
  lcd_printf_line(1, "WG OK");
}

void initializeWiFi() {
  connectToWiFi();
  // createWiFiAP();

  Serial.printf("\nWiFi connected, IP address: ");
  Serial.println(WiFi.localIP());

  syncNTP();
  initWireGuard();

  if (!MDNS.begin("inverter")) {
    Serial.println("ERROR: setting up MDNS responder!");
  } else {
    Serial.println("mDNS responder started");
  }
}


void setup() {
  Serial.begin(921600);
  // Log reset reason to help diagnose unexpected restarts
  g_reset_reason = esp_reset_reason();
  g_reset_reason_str = resetReasonToStr(g_reset_reason);
  Serial.printf("[BOOT] reset reason=%d (%s)\n", (int)g_reset_reason, g_reset_reason_str);
  // Also log reboot reason as a WARN (will append to LittleFS via printWarning)
  printWarning("[BOOT] reset reason=%d (%s)", (int)g_reset_reason, g_reset_reason_str);
  // Initialize QC1602A display (4-bit wiring)
  display_init();
  display_set_row_count(ROW_COUNT);

  // Initialize webserver / LittleFS (web UI files in data/ will be uploaded to device)
  initWebServer();

  initializeWiFi();
  // Provide reset info and register HTTP routes
  webserver_set_reset_info((int)g_reset_reason, g_reset_reason_str);
  webserver_setup_routes();
  server.begin();

  // Setup PWM output pin
  pinMode(PWM_PIN, OUTPUT);
  digitalWrite(PWM_PIN, LOW);
  Serial.println("HTTP :80");

  // Configure ADC for thermistors on GPIO34 and GPIO35
  analogReadResolution(12); // 12-bit (0..4095), default on ESP32 but explicit
  analogSetPinAttenuation(THERMISTOR_L_PIN, ADC_11db); // ~0..3.3V range
  analogSetPinAttenuation(THERMISTOR_H_PIN, ADC_11db); // ~0..3.3V range

  // Optional: quick probe log
  float tL = read_thermistor_temp_c(THERMISTOR_L_PIN);
  float tH = read_thermistor_temp_c(THERMISTOR_H_PIN);
  if (!isnan(tL)) {
    Serial.printf("Thermistor L initial T = %.2f °C\n", tL);
  } else {
    Serial.printf("Thermistor L initial read invalid (check wiring/divider).\n");
  }
  if (!isnan(tH)) {
    Serial.printf("Thermistor H initial T = %.2f °C\n", tH);
  } else {
    Serial.printf("Thermistor H initial read invalid (check wiring/divider).\n");
  }

  // Initialize inverter RS232 communication (background task)
  inverter_comm_init();
}


// --- Simple periodic task scheduler (Task array + function pointers) ---
struct Task {
  uint32_t period;   // period in ms
  uint32_t lastRun;  // last run time (ms since boot)
  void (*fn)();      // function to execute
};

// --- Button state tracking ---
struct BtnState {
  bool pressed;
  uint32_t pressStartMs;
};

static BtnState btnStates[4] = {{false, 0}, {false, 0}, {false, 0}, {false, 0}};

// --- Button handlers (called on button events) ---
// Button names reflect physical position: Up, Left, Down, Right
void onBtnUpPress() { display_scroll_up(); }
void onBtnUpRelease(int durationMs) {}

void onBtnLeftPress() {}
void onBtnLeftRelease(int durationMs) {}

void onBtnDownPress() { display_scroll_down(); }
void onBtnDownRelease(int durationMs) {}

void onBtnRightPress() {}
void onBtnRightRelease(int durationMs) {}

// Button handler dispatch (indexed: 0=Up, 1=Left, 2=Down, 3=Right)
typedef void (*BtnPressFn)();
typedef void (*BtnReleaseFn)(int);

static const BtnPressFn btnPressHandlers[4] = {
  &onBtnUpPress,
  &onBtnLeftPress,
  &onBtnDownPress,
  &onBtnRightPress
};

static const BtnReleaseFn btnReleaseHandlers[4] = {
  &onBtnUpRelease,
  &onBtnLeftRelease,
  &onBtnDownRelease,
  &onBtnRightRelease
};

static void task_scan_touch() {
  const uint8_t touches[4] = {BTN_UP_TOUCH, BTN_LEFT_TOUCH, BTN_DOWN_TOUCH, BTN_RIGHT_TOUCH};
  const uint32_t nowMs = millis();
  bool btnStateChanged = false;

  for (int i = 0; i < 4; i++) {
    uint16_t raw = touchRead(touches[i]);
    bool nowPressed = (raw <= BTN_TOUCH_THRESHOLD);
    if (nowPressed) {
      Serial.printf("T%d=%u %s\n", i, (unsigned)raw, nowPressed ? "PRESSED" : "RELEASED");
    }

    // Detect falling edge (press)
    if (nowPressed && !btnStates[i].pressed) {
      btnStates[i].pressed = true;
      btnStates[i].pressStartMs = nowMs;
      btnPressHandlers[i]();
      btnStateChanged = true;
    }
    // Detect rising edge (release)
    else if (!nowPressed && btnStates[i].pressed) {
      btnStates[i].pressed = false;
      int durationMs = (int)(nowMs - btnStates[i].pressStartMs);
      btnReleaseHandlers[i](durationMs);
      btnStateChanged = true;
    }
  }

  // Activate backlight on any button event
  if (btnStateChanged) {
    displayBacklightOn();
  }
}

static void refresh_inverter_status() {
  InverterState s = {};
  inverter_get_status(&s);

  char buf[17];
  if (!g_inverter_data_valid) {
    display_set_row(ROW_SOC, "SoC: --");
    display_set_row(ROW_PV_POWER, "PV: --");
    display_set_row(ROW_BATT_POWER, "Bat: --");
  } else {
    snprintf(buf, sizeof(buf), "SoC: %d%%", s.batt_soc);
    display_set_row(ROW_SOC, buf);

    int pv_w = (int)(s.pv_input_current * s.pv_input_voltage);
    snprintf(buf, sizeof(buf), "PV: %dW", pv_w);
    display_set_row(ROW_PV_POWER, buf);

    int charge_w = (int)(s.batt_voltage * s.batt_charge_current);
    int discharge_w = -(int)(s.batt_voltage * s.batt_discharge_current);
    snprintf(buf, sizeof(buf), "Bat: %d/%dW", charge_w, discharge_w);
    display_set_row(ROW_BATT_POWER, buf);
  }
  display_redraw();
}

static void format_temp_str(char* buf, float temp) {
  if (isnan(temp)) {
    strcpy(buf, "--.-");
  } else if (temp < 0) {
    dtostrf(temp, 3, 0, buf);
  } else {
    dtostrf(temp, 4, 1, buf);
  }
}

static void task_update_temperature() {
  g_temp_l = read_thermistor_temp_c(THERMISTOR_L_PIN);
  g_temp_h = read_thermistor_temp_c(THERMISTOR_H_PIN);

  char h_str[6], l_str[6], buf[17];
  format_temp_str(h_str, g_temp_h);
  format_temp_str(l_str, g_temp_l);
  snprintf(buf, sizeof(buf), "T: %s/%s\xDF" "C", h_str, l_str);
  display_set_row(ROW_TEMP, buf);
  display_redraw();
}

static void task_diag_heap() {
  // Periodic diagnostics to catch memory/stack issues causing resets after hours
  size_t freeHeap = ESP.getFreeHeap();
  size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
  printWarning("heap free=%u, largest=%u", (unsigned)freeHeap, (unsigned)largest);
}

// Task table and their periods
static Task tasks[] = {
  {  50u,      0u, &task_scan_touch },
  { 250u,      0u, &refresh_inverter_status },
  { 1000u,     0u, &task_update_temperature },
  { 1000u,     0u, &checkDisplayBacklightTimeout },
  { 600000u,   0u, &task_diag_heap }
};

void loop() {
  uint32_t t0 = millis();
  server.handleClient();
  uint32_t t1 = millis();

  // Log if handleClient takes unusually long (indicates blocking)
  uint32_t hdlDur = t1 - t0;
  if (hdlDur > 100) {
    printWarning("server.handleClient() took %ums", hdlDur);
  }

  // --- Periodic tasks via a simple Task array ---
  uint32_t nowMs = millis();
  for (auto &t : tasks) {
    if ((uint32_t)(nowMs - t.lastRun) >= t.period) {
      // Keep a stable cadence — advance by the period, not snap to nowMs
      t.lastRun += t.period;
      t.fn();
    }
  }

  // Software PWM: period 2000 ms (2s). Drive `PWM_PIN` HIGH for
  // outputDutyCycle * period, otherwise LOW. `outputDutyCycle` is 0.0-1.0.
  const uint32_t pwmPeriodMs = 2000;
  uint32_t phase = millis() % pwmPeriodMs;
  uint32_t onTime = (uint32_t)round(outputDutyCycle * (float)pwmPeriodMs);
  digitalWrite(PWM_PIN, (phase < onTime) ? HIGH : LOW);
  // Small yield to allow WiFi/RTOS background tasks to run and avoid starvation
  delay(5);
}