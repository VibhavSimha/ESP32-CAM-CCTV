#include "wifi_manager.h"
#include "config.h"
#include <WiFi.h>
#include <WiFiManager.h>

namespace {
constexpr uint16_t kWiFiPortalTimeoutS = 180;
constexpr unsigned long kWiFiRetryDelayMs = 5000UL;
constexpr unsigned long kWiFiRuntimeDisconnectDebounceMs = 12000UL;
}

void setupWiFiManager() {
    WiFiManager wm;
    WiFi.persistent(true);
    WiFi.setAutoReconnect(true);
    Serial.println("[WiFi] Auto-reconnect enabled; flash persistence enabled.");
    
    // reset settings if requested or specific condition met
    // wm.resetSettings();
    
    // set dark theme
    wm.setClass("invert");

    // Force autoConnect() to return periodically so we can keep retrying forever.
    wm.setConfigPortalTimeout(kWiFiPortalTimeoutS);

    unsigned long firstFailureAt = 0;
    uint32_t attempt = 0;
    while (true) {
        attempt++;
        Serial.printf("[WiFi] autoConnect attempt #%lu (portal timeout=%us, AP=%s)\n",
                      (unsigned long)attempt,
                      (unsigned)kWiFiPortalTimeoutS,
                      WIFI_AP_NAME);

        bool res = wm.autoConnect(WIFI_AP_NAME);
        if (res) {
            Serial.println("WiFi connected");
            Serial.print("IP Address: ");
            Serial.println(WiFi.localIP());
            return;
        }

        if (firstFailureAt == 0) firstFailureAt = millis();
        unsigned long downFor = millis() - firstFailureAt;
        Serial.printf("[WiFi] autoConnect failed. Retrying in %lums (offline for %lums)\n",
                      (unsigned long)kWiFiRetryDelayMs,
                      downFor);

        delay(kWiFiRetryDelayMs);
    }
}

void loopWiFiManager() {
    static unsigned long wifiLostSince = 0;
    static unsigned long lastReconnectAttempt = 0;

    wl_status_t wifiStatus = WiFi.status();
    if (wifiStatus == WL_CONNECTED) {
        if (wifiLostSince != 0) {
            unsigned long recoveredAfter = millis() - wifiLostSince;
            Serial.printf("[WiFi] Link recovered after %lums. ip=%s\n",
                          recoveredAfter,
                          WiFi.localIP().toString().c_str());
        }
        wifiLostSince = 0;
        lastReconnectAttempt = 0;
        return;
    }

    unsigned long now = millis();
    if (wifiLostSince == 0) {
        wifiLostSince = now;
        Serial.printf("[WiFi] Link lost. status=%d. Starting reconnect watchdog.\n", wifiStatus);
    }

    unsigned long wifiDownFor = now - wifiLostSince;
    if (wifiDownFor >= kWiFiRuntimeDisconnectDebounceMs &&
        (lastReconnectAttempt == 0 || now - lastReconnectAttempt >= kWiFiRetryDelayMs)) {
        lastReconnectAttempt = now;
        Serial.printf("[WiFi] Link down for %lums. Retrying saved Wi-Fi credentials.\n", wifiDownFor);
        WiFi.reconnect();
    }

}
