#include "battery_telnet.h"
#include "pylontech_comm.h"
#include "utils.h"
#include <WiFi.h>
#include <HardwareSerial.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define BATTERY_TELNET_PORT 23

// Hard session cap. Matches the 30 min auto-resume fuse used for the web UI
// serial mute: nobody can reach the hardware, so no debug session may be able
// to leave the site unregulated indefinitely.
#define BATTERY_TELNET_SESSION_MS (30UL * 60UL * 1000UL)

// The pause we hand pylontech_comm outlives the session on purpose. If the two
// deadlines were equal, a poll cycle could start in the last moment of the
// session and dump a 'pwr' frame into the operator's terminal.
#define BATTERY_TELNET_PAUSE_MS (BATTERY_TELNET_SESSION_MS + 60UL * 1000UL)

// How long to wait for the poller to actually let go of the UART. It checks
// the pause flag once per cycle, so the wait is however much of a consensus
// read is still in flight — PYLONTECH_MAX_ATTEMPTS * PYLONTECH_READ_WINDOW_MS
// worst case, 3 s. 8 s leaves margin without hanging the operator for long.
#define BATTERY_TELNET_HANDOVER_MS 8000

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

// Wait until the polling task is off the UART, draining whatever the client
// sent in the meantime. A real telnet client opens with a burst of IAC
// negotiation; we answer none of it and this window is where it gets thrown
// away, so nothing of it reaches the battery console.
static bool wait_for_uart(WiFiClient& client) {
  uint32_t start = millis();
  while (millis() - start < BATTERY_TELNET_HANDOVER_MS) {
    while (client.available()) client.read();
    if (!client.connected()) return false;
    if (pylontech_comm_uart_idle()) {
      while (client.available()) client.read();
      return true;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  return false;
}

static void run_session(WiFiClient& client) {
  client.print("Connecting...\r\n");

  // Announce remote echo and suppress-go-ahead so the client drops out of
  // line-at-a-time mode: the console echoes every character itself, and
  // without this the operator sees each line twice. Whatever the client
  // answers is discarded along with the rest of the negotiation burst.
  const uint8_t opts[] = {0xFF, 251, 1, 0xFF, 251, 3};  // IAC WILL ECHO, IAC WILL SGA
  client.write(opts, sizeof(opts));

  pylontech_comm_set_paused(true, BATTERY_TELNET_PAUSE_MS);

  if (!wait_for_uart(client)) {
    if (client.connected()) {
      client.print("Battery poller did not release the port, aborting\r\n");
      client.flush();
      printWarning("[TELNET] poller did not release the UART in %d ms",
                   BATTERY_TELNET_HANDOVER_MS);
    }
    client.stop();
    pylontech_comm_set_paused(false, 0);
    return;
  }

  // Drop whatever the last 'pwr' left in the RX FIFO. collect_response() stops
  // at the end sentinel, so the bytes the console emitted after it are still
  // queued and would land in the operator's terminal as garbage.
  while (Serial2.available()) Serial2.read();

  client.printf("Connected, battery console is yours for %u min\r\n",
                (unsigned)(BATTERY_TELNET_SESSION_MS / 60000UL));
  printInfo("[TELNET] console session opened from %s (polling paused, boiler off)",
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
    client.print("\r\nDisconnecting, battery polling resumes\r\n");
    client.flush();
    vTaskDelay(pdMS_TO_TICKS(100));  // let the last bytes leave before FIN
  }
  client.stop();
  pylontech_comm_set_paused(false, 0);
  printInfo("[TELNET] console session closed (%s), polling resumed",
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

void battery_telnet_init() {
  xTaskCreatePinnedToCore(
    telnet_task,
    "battery_telnet",
    4096,
    NULL,
    1,
    NULL,
    1);
}
