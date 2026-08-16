#include <Arduino.h>
#include "config.h"
#include "camera_init.h"
#include "wifi_manager.h"
#include "captive_portal.h"
#include "stream_server.h"
#include "tunnel_client.h"
#include "cloud_storage.h"
#include "crypto_auth.h"
#include "motion_pir.h"

void setup() {
    Serial.begin(115200);
    Serial.setDebugOutput(true);
    Serial.println();

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
