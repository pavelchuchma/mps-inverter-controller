# AGENTS.md

## Project: ESP32 FV Dashboard (Web UI + WebSocket)

### Purpose
This project implements an ESP32-based Wi‑Fi Access Point that serves a web
dashboard showing live photovoltaic inverter data.

Current state:
- Data are **mocked**
- Dashboard updates via **WebSocket push**
- Basic control commands are supported via WebSocket (mock only)

Future goal:
- Connect to a real solar inverter (likely Modbus RTU / TCP)
- Replace mock data with live values
- Implement safe control commands (limits, modes, etc.)

---

## High-level Architecture

Browser (HTML + JS)
        |
        |  WebSocket (JSON)
        v
ESP32 (AP mode)
        |
        |  (future)
        v
Solar inverter / battery (Modbus, UART, TCP...)

---

## Components
- WiFi AP (ESP32 in WIFI_AP mode)
 - HTTP server (port 80, serves HTML)
- WebSocket server (port 81, JSON protocol)
- Frontend: plain HTML/CSS/JS

---

## Networking
- SSID: FV-Dashboard
- Security: WPA2-PSK (Wi-Fi password only)
- ESP32 IP: 192.168.4.1
- HTTP: port 80
- WebSocket: port 81

---

## WebSocket Protocol

### Status (ESP32 → client)
{
  "type": "status",
  "pv_w": 1350,
  "batt_soc": 62.3,
  "batt_v": 52.14,
  "load_w": 820,
  "grid_ok": true,
  "state": "Running",
  "ts_ms": 12345678,
  "demo": true,
  "output_limit_w": 2000
}

### Command (client → ESP32)
{
  "type": "cmd",
  "name": "set_output_limit_w",
  "value": 2500
}

Supported commands (mock):
- set_demo (bool)
- set_output_limit_w (int, 0–10000)

### ACK
{
  "type": "ack",
  "ok": true,
  "msg": "output limit updated"
}

### Error
{
  "type": "err",
  "ok": false,
  "code": "range",
  "msg": "output_limit_w out of range"
}

---

## Code Structure

src/
 └─ main.cpp
platformio.ini
AGENTS.md

Key areas:
- updateMock()
- handleCommand()
- wsEvent()
- wsBroadcastStatus()

---

## PlatformIO Dependencies

lib_deps =
  bblanchon/ArduinoJson
  links2004/WebSockets

---

## Current Limitations
- No auth beyond Wi-Fi password
- No TLS
- No persistence
- Mock data only
- HTML embedded in firmware

---

## Next Steps
1. Replace mock with real inverter layer
2. Add req_id to protocol
3. Persist settings in NVS
4. Add safety checks
5. Move UI to LittleFS

---

## Design Rules
- Do not block in WS handlers
- Push-based updates only
- Keep protocol backward compatible
- Prefer simplicity over abstraction
