#pragma once

// M4: modified from stream tasks and read from the main loop's idle uploader;
// declared volatile so the count is always re-read (matches definition).
extern volatile int active_stream_clients;

void startCameraServer();

// Restore + apply the NVS-persisted global flash (LED) state at boot.
void setupFlashState();
