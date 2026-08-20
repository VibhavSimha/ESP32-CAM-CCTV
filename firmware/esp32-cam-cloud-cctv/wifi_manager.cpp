#include "wifi_manager.h"
#include "config.h"
#include <WiFi.h>
#include <WiFiManager.h>
#include <esp_wifi.h>

#include <cstdio>

#ifndef WIFI_STA_MAC_OVERRIDE
#define WIFI_STA_MAC_OVERRIDE ""
#endif

static bool parseMacOverride(const char* value, uint8_t out[6]) {
    if (!value || !value[0]) return false;
    unsigned int b[6] = {0, 0, 0, 0, 0, 0};
    int n = std::sscanf(value, "%2x:%2x:%2x:%2x:%2x:%2x",
                        &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]);
    if (n != 6) {
        n = std::sscanf(value, "%2x-%2x-%2x-%2x-%2x-%2x",
                        &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]);
    }
    if (n != 6) return false;
    for (int i = 0; i < 6; i++) out[i] = (uint8_t)b[i];
    return true;
}

static void maybeApplyStaMacOverride() {
    const char* configured = WIFI_STA_MAC_OVERRIDE;
    if (!configured || !configured[0]) return;

    uint8_t mac[6];
    if (!parseMacOverride(configured, mac)) {
        Serial.printf("[WiFi] Invalid WIFI_STA_MAC_OVERRIDE '%s' (expected AA:BB:CC:DD:EE:FF).\n",
                      configured);
        return;
    }

    esp_err_t err = esp_wifi_set_mac(WIFI_IF_STA, mac);
    if (err != ESP_OK) {
        Serial.printf("[WiFi] Failed to apply WIFI_STA_MAC_OVERRIDE '%s' (esp_err=%d).\n",
                      configured, (int)err);
        return;
    }
    Serial.printf("[WiFi] STA MAC override applied: %s\n", configured);
}

void setupWiFiManager() {
    WiFiManager wm;
    WiFi.mode(WIFI_STA); // ensure STA interface exists before optional MAC override
    maybeApplyStaMacOverride();
    WiFi.persistent(true);
    WiFi.setAutoReconnect(true);
    Serial.println("[WiFi] Auto-reconnect enabled; flash persistence disabled.");
    
    // reset settings if requested or specific condition met
    // wm.resetSettings();
    
    // set dark theme
    wm.setClass("invert");
    
    // AP name from config
    bool res = wm.autoConnect(WIFI_AP_NAME);
    
    if(!res) {
        Serial.println("Failed to connect, restarting...");
        delay(3000);
        ESP.restart();
    } 
    else {
        Serial.println("WiFi connected");
        Serial.print("IP Address: ");
        Serial.println(WiFi.localIP());
    }
}
