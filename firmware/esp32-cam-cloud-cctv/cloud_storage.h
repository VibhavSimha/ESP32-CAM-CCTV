#pragma once

#include <Arduino.h>

// Minimum free heap (bytes) required before attempting an HTTPS cloud upload.
// An upload allocates ~20 KB for the TLS session; running concurrently with
// an active bore tunnel proxy can collapse free heap to ~34 KB, causing
// crypto-login rejections and tunnel write stalls (issue #27).
// This value can be overridden in config.h before including this header.
#ifndef MIN_HEAP_FOR_UPLOAD
#define MIN_HEAP_FOR_UPLOAD 65000U
#endif

void setupCloudStorage();
void uploadFrameToCloud();
void publishTunnelUrl(String url);
void loopCloudStorage();
