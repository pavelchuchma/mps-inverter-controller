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
#include "pylontech_comm.h"
#include "pylontech_can.h"
#include "battery_telnet.h"
#include "phone.h"
#include "phone_charger.h"
#include "influx.h"
#include "relay.h"
#include "soc_guard.h"
#include "utils.h"
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <LittleFS.h>
#include <driver/adc.h>
#include <math.h>
#include <WireGuard-ESP32.h>

IPAddress apIP(192, 168, 4, 1);
IPAddress netMsk(255, 255, 255, 0);

static WireGuard wg;

WebServer server(80);

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

// --- Display row definitions ---
enum DisplayRow : uint8_t {
  ROW_SOC = 0,
  ROW_TEMP,
  ROW_BOILER,
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
  // Disable WiFi modem sleep — RSSI is marginal (-85..-90 dBm), continuous
  // listening keeps the link more robust at the cost of ~30 mA extra draw.
  WiFi.setSleep(false);
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
  Serial.begin(115200);
  // Log reset reason to help diagnose unexpected restarts
  g_reset_reason = esp_reset_reason();
  g_reset_reason_str = resetReasonToStr(g_reset_reason);
  Serial.printf("[BOOT] reset reason=%d (%s)\n", (int)g_reset_reason, g_reset_reason_str);
  // Initialize QC1602A display (4-bit wiring)
  display_init();
  display_set_row_count(ROW_COUNT);

  // Initialize webserver / LittleFS (web UI files in data/ will be uploaded to device)
  initWebServer();

  initializeWiFi();
  // First wall-clock-stamped log line — NTP has been attempted by now, so
  // this entry is normally timestamped with real local time (unlike the
  // earlier [BOOT] line above, which uses boot-relative HH:MM:SS.sss).
  printInfo("[BOOT] up, reset=%d (%s), IP=%s, RSSI=%d",
            (int)g_reset_reason, g_reset_reason_str,
            WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
  // Provide reset info and register HTTP routes
  webserver_set_reset_info((int)g_reset_reason, g_reset_reason_str);
  webserver_setup_routes();
  server.begin();

  Serial.println("HTTP :80");

  // Battery CAN link: decode the BMS broadcast into a published state.
  // Nothing consumes it yet — GET /can is where the decoding is checked, see
  // doc/battery_can_data_spec.md commit 1.
  //
  // Started HERE, after WiFi, and the position is load-bearing — it is the
  // sniffer's, and the boot jam it produces is part of the proven shape
  // (todo 002). TWAI_TX is GPIO12, which needs a 2.2 kOhm pull-down so the
  // board boots at all, and TJA1050 reads TXD low as *dominant* — so from
  // reset until twai_driver_install() claims the pin, the transceiver jams
  // the bus, here for the ~16 s WiFi bring-up takes. Every observed revival
  // of a pack that had stopped broadcasting followed a boot with a long jam
  // of exactly this shape; the boots with a short jam (init before WiFi)
  // never received a frame. It must also follow initWebServer(), which
  // mounts LittleFS, or the task's first app.log lines are dropped.
  pylontech_can_init(BATTERY_CAN_TX_PIN, BATTERY_CAN_RX_PIN);

  // Initialize relay outputs
  boilerRelayInit();
  pinMode(RELAY_MOBILE_CHARGER, OUTPUT);
  setMobileCharger(true);

  pinMode(BOILER_ON_PIN, INPUT_PULLUP);

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
   inverter_comm_init(INVERTER_RX_PIN, INVERTER_TX_PIN);

  // Initialize Pylontech battery console communication (background task)
  pylontech_comm_init(BATTERY_RX_PIN, BATTERY_TX_PIN);

  // Expose the battery console on telnet :23 for interactive debugging. Must
  // follow pylontech_comm_init(), which owns Serial2.
  battery_telnet_init();

  // Start polling phone status endpoint (background task, 30s interval)
  phone_comm_init();

  // Start metrics upload to InfluxDB (background task, samples every 10s,
  // flushes one batched POST per minute)
  influx_init();

  // SoC guard: reads its switch from NVS; the first decision waits for the
  // first QPIRI read (INVERTER_CONFIG_FIRST_DELAY_MS) inside soc_guard_tick().
  soc_guard_init();
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
  uint8_t belowCount;  // consecutive samples below the press threshold
  // Adaptive idle baseline: per-second maxima of a smoothed reading over the
  // last BTN_TOUCH_BASELINE_WINDOW_S seconds. The smoothing (EMA, alpha 1/4)
  // keeps a single upward noise spike from inflating the baseline for the
  // whole window; press detection itself uses the raw sample so it stays fast.
  uint16_t smoothed;
  uint16_t buckets[BTN_TOUCH_BASELINE_WINDOW_S];
  uint8_t bucketsFilled;
  uint32_t lastBucketSec;
};

static BtnState btnStates[2] = {};

// Idle baseline = max over the filled buckets (0 until the first sample).
static uint16_t btn_baseline(const BtnState& b) {
  uint16_t base = 0;
  for (uint8_t i = 0; i < b.bucketsFilled; i++) {
    if (b.buckets[i] > base) base = b.buckets[i];
  }
  return base;
}

// Feed one sample into the rolling baseline window. Returns true once the
// window has been filled at least once since boot (buttons armed).
static bool btn_update_baseline(BtnState& b, uint16_t raw, uint32_t nowMs) {
  if (b.bucketsFilled == 0 && b.lastBucketSec == 0) {
    b.smoothed = raw;  // seed the filter on the very first sample
  } else {
    b.smoothed = (uint16_t)((b.smoothed * 3 + raw + 2) / 4);
  }

  const uint32_t sec = nowMs / 1000;
  const uint8_t idx = (uint8_t)(sec % BTN_TOUCH_BASELINE_WINDOW_S);
  if (sec != b.lastBucketSec || b.bucketsFilled == 0) {
    // New second: start a fresh bucket (overwrites the one from N seconds ago).
    b.lastBucketSec = sec;
    b.buckets[idx] = b.smoothed;
    if (b.bucketsFilled < BTN_TOUCH_BASELINE_WINDOW_S) b.bucketsFilled++;
  } else if (b.smoothed > b.buckets[idx]) {
    b.buckets[idx] = b.smoothed;
  }
  return b.bucketsFilled >= BTN_TOUCH_BASELINE_WINDOW_S;
}

// --- Button handlers (called on button events) ---
void onBtnUpPress() { display_scroll_up(); }
void onBtnUpRelease(int durationMs) {}

void onBtnDownPress() { display_scroll_down(); }
void onBtnDownRelease(int durationMs) {}

// Button handler dispatch (indexed: 0=Up, 1=Down)
typedef void (*BtnPressFn)();
typedef void (*BtnReleaseFn)(int);

static const BtnPressFn btnPressHandlers[2] = {
  &onBtnUpPress,
  &onBtnDownPress
};

static const BtnReleaseFn btnReleaseHandlers[2] = {
  &onBtnUpRelease,
  &onBtnDownRelease
};

static void task_scan_touch() {
  const uint8_t touches[2] = {BTN_UP_TOUCH, BTN_DOWN_TOUCH};
  static const char* const names[2] = {"up", "down"};
  static bool armedLogged = false;
  const uint32_t nowMs = millis();
  bool btnStateChanged = false;
  bool allArmed = true;

  for (int i = 0; i < 2; i++) {
    BtnState& b = btnStates[i];
    const uint16_t raw = touchRead(touches[i]);
    const bool armed = btn_update_baseline(b, raw, nowMs);
    if (!armed) {
      allArmed = false;
      continue;  // still calibrating after boot: buttons inactive
    }

    const uint16_t base = btn_baseline(b);
    // A touch lowers the count, so a press is a drop below the idle baseline.
    // Hysteresis: press needs a bigger drop than release, and the press must
    // hold for BTN_TOUCH_CONFIRM_SAMPLES consecutive scans.
    const bool belowPress = (base > 0) && ((uint32_t)raw * 100 <= (uint32_t)base * BTN_TOUCH_PRESS_PCT);
    const bool aboveRelease = (base == 0) || ((uint32_t)raw * 100 >= (uint32_t)base * BTN_TOUCH_RELEASE_PCT);

    if (!b.pressed) {
      b.belowCount = belowPress ? (uint8_t)(b.belowCount + 1) : 0;
      if (b.belowCount >= BTN_TOUCH_CONFIRM_SAMPLES) {
        b.pressed = true;
        b.pressStartMs = nowMs;
        b.belowCount = 0;
        printInfo("BTN %s press (raw=%u base=%u)", names[i], raw, base);
        btnPressHandlers[i]();
        btnStateChanged = true;
      }
    } else if (aboveRelease) {
      b.pressed = false;
      int durationMs = (int)(nowMs - b.pressStartMs);
      printInfo("BTN %s release after %d ms (raw=%u base=%u)", names[i], durationMs, raw, base);
      btnReleaseHandlers[i](durationMs);
      btnStateChanged = true;
    }
  }

  if (allArmed && !armedLogged) {
    armedLogged = true;
    printInfo("Touch buttons armed: baseline up=%u down=%u",
              btn_baseline(btnStates[0]), btn_baseline(btnStates[1]));
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

  // SoC and battery power come directly from the Pylontech console (its own
  // validity flag), independent of the inverter link.
  PylontechState bat;
  pylontech_get_status(&bat);
  if (!g_pylontech_data_valid) {
    display_set_row(ROW_SOC, "SoC: --");
    display_set_row(ROW_BATT_POWER, "Bat: --");
  } else {
    // " G" while the SoC guard holds the cut-off at 48 V.
    SocGuardState sg = {};
    soc_guard_get(&sg);
    snprintf(buf, sizeof(buf), "SoC: %d%%%s", bat.soc,
             (sg.enabled && sg.armed) ? " G" : "");
    display_set_row(ROW_SOC, buf);

    // current is signed: + charge / - discharge.
    int batt_w = (int)(bat.voltage * bat.current);
    snprintf(buf, sizeof(buf), "Bat: %dW", batt_w);
    display_set_row(ROW_BATT_POWER, buf);
  }

  // PV power still comes from the inverter.
  if (!g_inverter_data_valid) {
    display_set_row(ROW_PV_POWER, "PV: --");
  } else {
    int pv_w = (int)(s.pv_input_current_batt * s.pv_input_voltage);
    snprintf(buf, sizeof(buf), "PV: %dW", pv_w);
    display_set_row(ROW_PV_POWER, buf);
  }
  display_redraw();
}

// Whole degrees, two characters wide ("52", " 8", "-2"); "--" when invalid.
// The tenths are on the web pages; the LCD row needs the space for the target.
static void format_temp_str(char* buf, float temp) {
  if (isnan(temp)) {
    strcpy(buf, "--");
  } else {
    dtostrf(temp, 2, 0, buf);
  }
}

// LCD temperature row: "T: 52/38 >45°C" — both tank sensors and the virtual
// thermostat target, with one marker for the thermostat state:
//   '>' heating allowed (target not reached, physical input on)
//   '=' target reached (both sensors at or above it)
//   'x' target not reached but the physical thermostat opened
// '=' wins over 'x', matching the main page (doc/todo/008).
static void task_update_temperature() {
  g_temp_l = read_thermistor_temp_c(THERMISTOR_L_PIN);
  g_temp_h = read_thermistor_temp_c(THERMISTOR_H_PIN);

  char h_str[6], l_str[6], buf[17];
  format_temp_str(h_str, g_temp_h);
  format_temp_str(l_str, g_temp_l);
  char mark = isBoilerTargetReached() ? '=' : (isBoilerOn() ? '>' : 'x');
  snprintf(buf, sizeof(buf), "T: %s/%s %c%u\xDF" "C", h_str, l_str, mark,
           (unsigned)getBoilerTargetTemp());
  display_set_row(ROW_TEMP, buf);
  display_redraw();
}

static void task_update_boiler() {
  char buf[17];
  if (isBoilerFault()) {
    snprintf(buf, sizeof(buf), "Boiler: ERROR");
  } else {
    static const char* labels[] = {"OFF", "500W", "1000W", "2000W"};
    snprintf(buf, sizeof(buf), "Boiler: %s%s", labels[getBoilerPower()],
             isBoilerManual() ? " M" : "");
  }
  display_set_row(ROW_BOILER, buf);
  display_redraw();
}

// Reboot once a day at midnight as a stability measure — the firmware has
// occasionally panicked after ~a week of uptime (suspected memory
// fragmentation or a library leak). Guards: only act once NTP has set a real
// wall clock, and only after the device has been up long enough that the
// post-reboot device (back online within the same 00:00 minute) does not
// immediately reboot again.
static void task_midnight_reboot() {
  time_t now = time(nullptr);
  if (now < 24 * 3600) return;             // NTP not synced yet
  if (millis() < 5UL * 60 * 1000) return;  // avoid re-trigger right after the reboot
  struct tm t;
  localtime_r(&now, &t);
  if (t.tm_hour == 0 && t.tm_min == 0) {
    printWarning("Midnight scheduled restart for stability");
    delay(100);  // let the log line flush over serial
    ESP.restart();
  }
}

// Runtime WiFi safety net. The core's auto-reconnect normally re-associates on
// its own when the AP comes back, but it can occasionally give up for good
// (e.g. after an AP restart). So: after ~2 min offline force an explicit
// reconnect (retried every minute), and after ~10 min offline reboot as a hard
// fallback. The boot-time connect timeout only covers the initial connect.
static void task_wifi_health() {
  static uint32_t disconnectedSinceMs = 0;  // 0 = currently connected
  static uint32_t lastReconnectMs = 0;

  if (WiFi.isConnected()) {
    if (disconnectedSinceMs != 0) {
      printInfo("WiFi reconnected (RSSI %d dBm)", (int)WiFi.RSSI());
      disconnectedSinceMs = 0;
      lastReconnectMs = 0;
    }
    return;
  }

  uint32_t now = millis();
  if (disconnectedSinceMs == 0) {
    disconnectedSinceMs = now;
    printWarning("WiFi link lost, waiting for auto-reconnect");
    return;
  }

  uint32_t downMs = now - disconnectedSinceMs;

  if (downMs >= 10UL * 60 * 1000) {
    printWarning("WiFi down for >10 min, restarting");
    delay(100);  // let the log line flush over serial
    ESP.restart();
  }

  // Force a reconnect once the core's auto-reconnect has had a couple of
  // minutes, then retry once a minute.
  if (downMs >= 2UL * 60 * 1000 &&
      (lastReconnectMs == 0 || (now - lastReconnectMs) >= 60UL * 1000)) {
    printWarning("WiFi down for %us, forcing reconnect", (unsigned)(downMs / 1000));
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    lastReconnectMs = now;
  }
}

// Task table and their periods
static Task tasks[] = {
  {  50u,      0u, &task_scan_touch },
  { 250u,      0u, &refresh_inverter_status },
  { 1000u,     0u, &task_update_temperature },
  { 1000u,     0u, &task_update_boiler },
  { 1000u,     0u, &checkDisplayBacklightTimeout },
  { 1000u,     0u, &soc_guard_tick },
  { 10000u,    0u, &tickPhoneCharger },
  { 30000u,    0u, &task_midnight_reboot },
  { 30000u,    0u, &task_wifi_health },
};

void loop() {
  uint32_t t0 = millis();
  server.handleClient();
  tickBoiler();
  uint32_t t1 = millis();

  // Log if handleClient takes unusually long (indicates blocking)
  uint32_t hdlDur = t1 - t0;
  if (hdlDur > 300) {
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

  // Small yield to allow WiFi/RTOS background tasks to run and avoid starvation
  delay(5);
}