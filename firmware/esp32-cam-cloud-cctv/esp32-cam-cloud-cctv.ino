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
    // Keep localtunnel alive
    tunnelLoop();
    
    // Handle PIR motion detection and cloud upload
    loopPIR();
}
