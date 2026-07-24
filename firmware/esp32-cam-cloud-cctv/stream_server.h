#pragma once

extern int active_stream_clients;

void startCameraServer();

// Restore + apply the NVS-persisted global flash (LED) state at boot.
void setupFlashState();
