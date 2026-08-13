#pragma once

// M4: modified from stream tasks and read from the main loop's idle uploader;
// declared volatile so the count is always re-read (matches definition).
extern volatile int active_stream_clients;

void startCameraServer();

// Register the local ISP captive-portal helper page on the main web server.
void registerCaptivePortalHandlers(httpd_handle_t server);

// Restore + apply the NVS-persisted global flash (LED) state at boot.
void setupFlashState();
