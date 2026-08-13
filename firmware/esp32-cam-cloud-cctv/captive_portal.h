#pragma once

// =============================================================================
//  captive_portal.h — post-connect, browser-assisted captive-portal login (#33)
//
//  SCOPE: this runs ONLY after WiFiManager has already joined the router network
//  (see wifi_manager.cpp — that step is UNCHANGED). It is completely separate
//  from the encrypted camera-viewer auth in crypto_auth.cpp.
//
//  FLOW:
//   1. captivePortalBegin() probes for internet (a generate_204 request) right
//      after Wi-Fi connects. If the network passes cleanly -> nothing to do.
//   2. If a captive portal intercepts the probe, the firmware fetches the portal
//      login page and AUTO-DETECTS the username/password field names (it never
//      hardcodes them; see captive_portal_parse.h).
//   3. A local helper page at /portal accepts the ISP username/password and,
//      for a simple form-based portal, submits the login from the ESP32.
//   4. Challenge/JS-only/tokenized portals fall back to a browser-assisted
//      manual path (the /portal page links out to the real portal URL).
//
//  Only Wi-Fi credentials (via WiFiManager/NVS) and a lightweight "portal seen"
//  marker are persisted — portal credentials are NOT stored.
// =============================================================================

#include "esp_http_server.h"

enum PortalState {
    PORTAL_STATE_UNKNOWN = 0,   // not yet probed
    PORTAL_STATE_OPEN,          // internet reachable, no captive portal
    PORTAL_STATE_CAPTIVE,       // captive portal detected, login page fetched
    PORTAL_STATE_LOGIN_PENDING, // waiting for the user to submit credentials
    PORTAL_STATE_SUBMITTED,     // credentials submitted to the portal
    PORTAL_STATE_SUCCESS,       // internet reachable after login
    PORTAL_STATE_FAILED,        // login attempt failed (recoverable, retryable)
    PORTAL_STATE_UNSUPPORTED    // challenge/JS portal -> manual browser fallback
};

// Probe for internet / captive-portal interception. Call once after Wi-Fi is
// connected (from setup(), after setupWiFiManager()). Non-blocking beyond the
// short probe timeout; safe to call when the feature is disabled (no-op).
void captivePortalBegin();

// Periodic tick: handles the post-submit re-probe and retry/timeout recovery.
// Call from loop(); cheap no-op when idle or disabled.
void captivePortalLoop();

// Register the local helper endpoints (/portal, /portal/status, /portal/login)
// on the running camera web server. Call from startCameraServer() after
// httpd_start(), like registerCryptoAuthHandlers().
void registerCaptivePortalHandlers(httpd_handle_t server);

// Current state (for diagnostics / other modules). 
PortalState captivePortalGetState();
