#pragma once

#include <Arduino.h>

class WebServer;

void initWebServer();
void handleRoot();
void handleNotFound();

// Provide reset information for JSON status (called from setup())
void webserver_set_reset_info(int reason, const char* reason_str);

// Register HTTP routes (/, /status, /cmd, notFound) on the global `server`
void webserver_setup_routes();

// "<METHOD> <uri> from <ip>" of the request served by the most recent
// server.handleClient() call, or an empty string when it served none.
// Reading it clears it, so the main loop can attribute a slow
// handleClient() to the page that was actually handled.
String webserver_take_last_request();
