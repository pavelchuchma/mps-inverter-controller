#pragma once

class WebServer;

void initWebServer();
void handleRoot();
void handleNotFound();

// Provide reset information for JSON status (called from setup())
void webserver_set_reset_info(int reason, const char* reason_str);

// Register HTTP routes (/, /status, /cmd, notFound) on the global `server`
void webserver_setup_routes();
