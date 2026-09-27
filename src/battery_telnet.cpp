#include "battery_telnet.h"
#include "utils.h"
#include <WiFi.h>
#include <HardwareSerial.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define BATTERY_TELNET_PORT 23

// Console port serial settings (US5000 console port).
#define BATTERY_CONSOLE_BAUD 115200

// Hard session cap: nobody can reach the hardware, so a client that vanishes
// without closing the socket must not be able to hold the port forever.
#define BATTERY_TELNET_SESSION_MS (30UL * 60UL * 1000UL)

// A real telnet client opens with a burst of IAC negotiation. We answer none
// of it, so the first moments of a session are spent discarding it — this is
// how long that window is, so nothing of the burst reaches the battery console.
#define BATTERY_TELNET_NEGOTIATION_MS 300

static WiFiServer g_telnet_server(BATTERY_TELNET_PORT);

// Inbound byte filter state, reset per session.
struct TelnetFilter {
  enum Iac { IAC_NONE, IAC_CMD, IAC_OPT } iac;  // position within an IAC sequence
  bool after_cr;                                // previous byte was CR
};

// Console -> client. A lone 0xFF would be read by the client as the start of a
// telnet command and would eat the next byte; RFC 854 says to double it. The
// RS232 line is noisy enough (~1 bad byte / 10 kB) that this happens for real.
// Escaped into one buffer and sent as a single write — with NoDelay set, a
// per-byte write would put a whole 900 B console frame on the wire as 900
// packets.
static void write_to_client(WiFiClient& client, const uint8_t* buf, int n) {
  uint8_t out[256];
  int o = 0;
  for (int i = 0; i < n; ++i) {
    if (buf[i] == 0xFF) out[o++] = 0xFF;
    out[o++] = buf[i];
  }
  client.write(out, o);
}

// Client -> console. Two jobs:
//   - Discard IAC command sequences. We never agree to anything, so a
//     compliant client has no reason to send a subnegotiation (IAC SB); only
//     the 2-byte command and 3-byte WILL/WONT/DO/DONT forms are handled.
//   - Collapse the NVT encodings of Enter (CR LF and CR NUL) to the bare CR
//     the console expects, and translate a lone LF to CR.
static void forward_to_console(WiFiClient& client, TelnetFilter& f) {
  while (client.available()) {
    int b = client.read();
    if (b < 0) break;
    uint8_t c = (uint8_t)b;

    if (f.iac == TelnetFilter::IAC_CMD) {
      // WILL/WONT/DO/DONT are followed by an option byte, the rest are not.
      f.iac = (c >= 251 && c <= 254) ? TelnetFilter::IAC_OPT : TelnetFilter::IAC_NONE;
      continue;
    }
    if (f.iac == TelnetFilter::IAC_OPT) {
      f.iac = TelnetFilter::IAC_NONE;
      continue;
    }
    if (c == 0xFF) {
      f.iac = TelnetFilter::IAC_CMD;
      continue;
    }

    if (f.after_cr) {
      f.after_cr = false;
      if (c == '\n' || c == '\0') continue;  // second half of an Enter
    }
    if (c == '\r') {
      f.after_cr = true;
      Serial2.write('\r');
      continue;
    }
    Serial2.write(c == '\n' ? '\r' : c);
  }
}

// Discard the client's opening negotiation burst (see
// BATTERY_TELNET_NEGOTIATION_MS). Returns false if the client went away.
static bool drain_negotiation(WiFiClient& client) {
  uint32_t start = millis();
  while (millis() - start < BATTERY_TELNET_NEGOTIATION_MS) {
    while (client.available()) client.read();
    if (!client.connected()) return false;
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  while (client.available()) client.read();
  return true;
}

static void run_session(WiFiClient& client) {
  client.print("Connecting...\r\n");

  // Announce remote echo and suppress-go-ahead so the client drops out of
  // line-at-a-time mode: the console echoes every character itself, and
  // without this the operator sees each line twice. Whatever the client
  // answers is discarded along with the rest of the negotiation burst.
  const uint8_t opts[] = {0xFF, 251, 1, 0xFF, 251, 3};  // IAC WILL ECHO, IAC WILL SGA
  client.write(opts, sizeof(opts));

  if (!drain_negotiation(client)) {
    client.stop();
    return;
  }

  // Drop whatever the console emitted while nobody was listening (its own
  // periodic output, the tail of a previous session), so it does not land in
  // the operator's terminal as garbage.
  while (Serial2.available()) Serial2.read();

  client.printf("Connected, battery console is yours for %u min\r\n",
                (unsigned)(BATTERY_TELNET_SESSION_MS / 60000UL));
  printInfo("[TELNET] console session opened from %s",
            client.remoteIP().toString().c_str());

  TelnetFilter filter = {};
  uint8_t buf[128];  // escaped into out[256] in write_to_client()
  uint32_t deadline = millis() + BATTERY_TELNET_SESSION_MS;
  uint32_t last_reject_check = 0;

  while (client.connected() && (int32_t)(millis() - deadline) < 0) {
    int n = 0;
    while (Serial2.available() && n < (int)sizeof(buf)) buf[n++] = (uint8_t)Serial2.read();
    if (n) write_to_client(client, buf, n);

    bool had_input = client.available() > 0;
    forward_to_console(client, filter);

    // Turn away a second client instead of letting it queue in the backlog
    // and stare at a dead port — both would be writing into the same UART.
    if (millis() - last_reject_check >= 500) {
      last_reject_check = millis();
      WiFiClient other = g_telnet_server.available();
      if (other) {
        other.print("Battery console is already in use\r\n");
        other.stop();
      }
    }

    if (!n && !had_input) vTaskDelay(pdMS_TO_TICKS(5));
  }

  bool timed_out = client.connected();
  if (timed_out) {
    client.print("\r\nDisconnecting, session cap reached\r\n");
    client.flush();
    vTaskDelay(pdMS_TO_TICKS(100));  // let the last bytes leave before FIN
  }
  client.stop();
  printInfo("[TELNET] console session closed (%s)",
            timed_out ? "30 min cap" : "client disconnected");
}

static void telnet_task(void* arg) {
  (void)arg;
  // setNoDelay before begin(): the flag is applied to each socket as it is
  // accepted, and an interactive console must not sit in Nagle's delay.
  g_telnet_server.setNoDelay(true);
  g_telnet_server.begin();
  for (;;) {
    WiFiClient client = g_telnet_server.available();
    if (!client) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    run_session(client);
  }
}

void battery_telnet_init(int rx_pin, int tx_pin) {
  Serial2.begin(BATTERY_CONSOLE_BAUD, SERIAL_8N1, rx_pin, tx_pin);
  xTaskCreatePinnedToCore(
    telnet_task,
    "battery_telnet",
    4096,
    NULL,
    1,
    NULL,
    1);
}
