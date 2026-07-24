#include "wifi_manager.h"
#include "config.h"
#include <WiFi.h>
#include <WiFiManager.h>

void setupWiFiManager() {
    WiFiManager wm;
    WiFi.setTxPower(WIFI_POWER_8_5dBm);
    WiFi.persistent(false);
    WiFi.setAutoReconnect(true);
    Serial.println("[WiFi] TX Power set to 8.5dBm; Auto-reconnect enabled; flash persistence disabled.");
    
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
