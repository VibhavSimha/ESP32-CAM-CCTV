#include <Arduino.h>
#include "config.h"
#include "camera_init.h"
#include "wifi_manager.h"
#include "stream_server.h"
#include "tunnel_client.h"
#include "cloud_storage.h"
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

    // 2. Initialize WiFi via WiFiManager
    setupWiFiManager();
    
    // 3. Initialize Cloud Storage (Supabase)
    setupCloudStorage();
    
    // 4. Start Local Stream Server
    startCameraServer();
    
    // 5. Initialize Localtunnel
    tunnelBegin();
    
    // 6. Initialize PIR Motion Sensor
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

    // Retry best-effort cloud status updates without blocking local serving
    loopCloudStorage();
    
    // Handle PIR motion detection and cloud upload
    loopPIR();
    
    // Background autonomous CCTV upload (when no clients are watching)
    static unsigned long lastIdleUpload = 0;
    if (active_stream_clients == 0 && millis() - lastIdleUpload > 15000) {
        lastIdleUpload = millis();
        Serial.println("[Idle] No clients streaming. Performing autonomous background upload.");
        uploadFrameToCloud();
    }
}
