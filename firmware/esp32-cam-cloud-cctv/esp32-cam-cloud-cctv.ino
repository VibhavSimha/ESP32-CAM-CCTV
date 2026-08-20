#include <Arduino.h>
#include <cstring>
#include "config.h"
#include "camera_init.h"
#include "wifi_manager.h"
#include "captive_portal.h"
#include "stream_server.h"
#include "tunnel_client.h"
#include "cloud_storage.h"
#include "crypto_auth.h"
#include "motion_pir.h"

// Print one aligned "[Config] <label> <status>" line (helper for logConfigStatus).
static void logConfigLine(const char* label, bool ok, const char* okText, const char* badText) {
    Serial.printf("[Config] %-24s %s\n", label, ok ? okText : badText);
}

// Report whether each configuration macro has a real value WITHOUT printing any
// secret (issue #44). This is a boot-time diagnostic so an operator can confirm
// at a glance that config.h is filled in correctly and rule out a plain
// misconfiguration as the cause of connectivity / captive-portal problems. Only
// SET / NOT-SET / still-the-example-default is logged — never the actual values.
static void logConfigStatus() {
    Serial.println("========== Configuration status (secrets hidden) ==========");

    // Camera viewer login.
    logConfigLine("Camera login user", strlen(CONFIG_HTTP_USER) > 0, "SET", "NOT SET");
    bool passSet  = strlen(CONFIG_HTTP_PASS) > 0;
    bool passWeak = passSet && (strcmp(CONFIG_HTTP_PASS, "changeme12345678") == 0);
    logConfigLine("Camera login password", passSet && !passWeak, "SET",
                  passSet ? "SET but still the EXAMPLE DEFAULT — change it!" : "NOT SET");

    // Device pubkey pinning (login MITM hardening).
    logConfigLine("Device pubkey pinned", strlen(CONFIG_DEVICE_PUBKEY_B64) > 0,
                  "SET (pinned)", "NOT PINNED (login works but is not MITM-protected)");

    // Public tunnel.
#if CONFIG_TUNNEL_MODE == CONFIG_TUNNEL_MODE_BORE
    Serial.printf("[Config] %-24s %s\n", "Tunnel mode", "BORE");
    logConfigLine("BORE server", strlen(CONFIG_BORE_SERVER) > 0, "SET", "NOT SET");
#elif CONFIG_TUNNEL_MODE == CONFIG_TUNNEL_MODE_SELFHOST
    Serial.printf("[Config] %-24s %s\n", "Tunnel mode", "SELFHOST");
    logConfigLine("Selfhost tunnel ID",
                  strlen(CONFIG_SELFHOST_TUNNEL_ID) > 0 &&
                      strcmp(CONFIG_SELFHOST_TUNNEL_ID, "00000000-0000-4000-8000-000000000000") != 0,
                  "SET", "NOT SET (still the example placeholder)");
#else
    Serial.printf("[Config] %-24s %s\n", "Tunnel mode", "UNKNOWN");
#endif

    // Supabase cloud storage.
    logConfigLine("Supabase URL",
                  strlen(SUPABASE_URL) > 0 && strcmp(SUPABASE_URL, "https://xxxx.supabase.co") != 0,
                  "SET", "NOT CONFIGURED (still the example placeholder)");
    logConfigLine("Supabase anon key",
                  strlen(SUPABASE_ANON_KEY) > 0 && strcmp(SUPABASE_ANON_KEY, "your-anon-key") != 0,
                  "SET", "NOT CONFIGURED (still the example placeholder)");
    logConfigLine("Supabase bucket", strlen(SUPABASE_BUCKET) > 0, "SET", "NOT SET");

    // Wi-Fi setup AP + feature toggles.
    logConfigLine("Wi-Fi setup AP name", strlen(WIFI_AP_NAME) > 0, "SET", "NOT SET");
    logConfigLine("Captive-portal login", ENABLE_CAPTIVE_PORTAL_LOGIN != 0, "ENABLED", "DISABLED");
    logConfigLine("PIR motion", ENABLE_PIR_MOTION != 0, "ENABLED", "DISABLED");
    logConfigLine("Captive probe URL", strlen(CAPTIVE_PROBE_URL) > 0, "SET", "NOT SET");

    // MAC spoof status: show spoofed MAC if set, otherwise "DISABLED (factory MAC)".
    {
        const char* spoofMac = CAPTIVE_MAC_SPOOF;
        if (spoofMac && spoofMac[0] != '\0') {
            Serial.printf("[Config] %-24s ACTIVE (%s)\n", "MAC spoof (issue #50)", spoofMac);
        } else {
            logConfigLine("MAC spoof (issue #50)", false, "ACTIVE", "DISABLED (factory MAC)");
        }
    }

    Serial.println("===========================================================");
}

void setup() {
    Serial.begin(115200);
    Serial.setDebugOutput(true);
    Serial.println();

    // Log which configuration values are set (no secrets) so a plain
    // misconfiguration can be ruled out up-front (issue #44).
    logConfigStatus();

    // 1. Initialize Camera
    if (initCamera() != ESP_OK) {
        Serial.println("Camera initialization failed");
        delay(1000);
        ESP.restart();
    }

    // 2. Restore persisted global flash (LED) state and apply to GPIO.
    setupFlashState();

    // 3. Initialize WiFi via WiFiManager
    setupWiFiManager();

    // 3b. Post-connect captive-portal probe/login (issue #33). Runs ONLY after
    //     Wi-Fi is already joined; a no-op on open networks. The local helper
    //     page is served by the camera web server started in step 6.
    captivePortalBegin();

    // 3c. Re-print the configuration status and dump a full captive-portal /
    //     network / heap diagnostics block now that Wi-Fi is up. The very first
    //     configuration banner (logged from the top of setup()) is frequently
    //     lost or corrupted in a serial capture that only attaches during the
    //     flash->run reset — which is exactly why the "[Config] … SET/NOT SET"
    //     lines were missing from the issue #48 logs. Repeating them here, at a
    //     stable point after the network is up, guarantees they are always
    //     visible, and the diagnostics block gives a one-shot picture of the
    //     portal state (also available live at http://<device-ip>/portal/diag).
    Serial.println("[Boot] Re-printing configuration + diagnostics after Wi-Fi connect:");
    logConfigStatus();
    captivePortalPrintDiagnostics();

    // 4. Initialize Cloud Storage (Supabase)
    setupCloudStorage();

    // 5. Initialize crypto auth (X25519 keypair from NVS or first-boot gen)
    setupCryptoAuth();

    // 6. Start Local Stream Server
    startCameraServer();

    // 7. Initialize Localtunnel
    tunnelBegin();

    // 8. Initialize PIR Motion Sensor (no-op when ENABLE_PIR_MOTION == 0)
    setupPIR();

    Serial.println("System setup complete.");
}

void loop() {
    static unsigned long lastHeartbeat = 0;
    if (millis() - lastHeartbeat > 10000) {
        lastHeartbeat = millis();
        Serial.printf("[Heartbeat] Uptime: %lu ms. Free Heap: %u\n", millis(), ESP.getFreeHeap());
    }

    // Keep localtunnel alive
    handleTunnel();

    // Post-connect captive-portal state machine tick (verify/recover). No-op
    // unless a portal login was just submitted.
    captivePortalLoop();

    // Retry best-effort cloud status updates without blocking local serving
    loopCloudStorage();

    // Handle PIR motion detection (disabled by default via ENABLE_PIR_MOTION)
    loopPIR();

    // Background autonomous CCTV upload (only when no clients are watching).
    // When a browser client connects it becomes the uploader (best-effort per
    // frame) and this stops via active_stream_clients.
    //
    // Issue #27: skip the upload when heap is below MIN_HEAP_FOR_UPLOAD or
    // while the bore tunnel has an active proxy slot. An idle HTTPS upload
    // takes ~20 KB of heap for the TLS connection; running it concurrently
    // with a tunnel proxy (which holds its own socket buffers + 2 KB copy
    // buffer + task stack) can collapse free heap to ~34 KB, causing crypto
    // login rejections and tunnel write stalls. Deferring the upload when
    // the tunnel is busy or heap is tight prevents these collisions.
    //
    // Issue #40: only attempt Supabase once the internet-connectivity heartbeat
    // has CONFIRMED reachability (captivePortalIsOnline()). Behind an ISP captive
    // portal the network is joined but the internet is blocked, so every upload
    // fails with HTTP 0 and floods the log; pausing here keeps the device quiet
    // and lets the operator see the /portal login instructions instead. Uploads
    // resume automatically once the captive-portal heartbeat clears.
    static unsigned long lastIdleUpload = 0;
    if (active_stream_clients == 0 &&
        millis() - lastIdleUpload > 3000 &&
        ESP.getFreeHeap() >= MIN_HEAP_FOR_UPLOAD &&
        !isTunnelSlotBusy() &&
        captivePortalIsOnline()) {
        lastIdleUpload = millis();
        Serial.println("[Idle] No clients streaming. Performing autonomous background upload.");
        uploadFrameToCloud();
    }
}
