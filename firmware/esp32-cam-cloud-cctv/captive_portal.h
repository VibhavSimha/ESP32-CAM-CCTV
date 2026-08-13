#pragma once

#include "esp_http_server.h"

// Best-effort post-connect ISP captive portal helper.
// Call after Wi-Fi is connected. The module probes for captive behavior,
// exposes a local /portal helper page, and can submit a simple HTML form when
// the portal target is known.
void setupCaptivePortal();

// Register the helper page on the main camera HTTP server.
void registerCaptivePortalHandlers(httpd_handle_t server);
