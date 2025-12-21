#include <WebServer.h>
#include "esp_webserver.h"
#include <LittleFS.h>
#include <Arduino.h>

// `server` is defined in main.cpp; declare it here for use in this TU.
extern WebServer server;

void initWebServer() {
  if (!LittleFS.begin()) {
    Serial.println("LittleFS mount failed");
  }
}

void handleRoot() {
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "-1");
  File f = LittleFS.open("/index.html", "r");
  if (!f) {
    server.send(500, "text/plain", "index.html not found");
    return;
  }
  server.streamFile(f, "text/html; charset=utf-8");
  f.close();
}

void handleNotFound() {
  String uri = server.uri();
  String path = uri;
  if (!path.startsWith("/")) path = "/" + path;

  if (LittleFS.exists(path)) {
    File f = LittleFS.open(path, "r");
    String contentType = "text/plain";
    if (path.endsWith(".html")) contentType = "text/html; charset=utf-8";
    else if (path.endsWith(".css")) contentType = "text/css";
    else if (path.endsWith(".js")) contentType = "application/javascript";
    else if (path.endsWith(".png")) contentType = "image/png";
    else if (path.endsWith(".svg")) contentType = "image/svg+xml";

    server.streamFile(f, contentType);
    f.close();
    return;
  }

  server.send(404, "text/plain", "Not found");
}
