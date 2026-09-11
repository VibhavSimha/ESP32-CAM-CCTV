#include "wifi_manager.h"
#include "config.h"
#include <WiFi.h>
#include <WiFiManager.h>

#ifndef WIFI_MANAGER_PORTAL_TIMEOUT_S
#define WIFI_MANAGER_PORTAL_TIMEOUT_S 180
#endif

#ifndef WIFI_MANAGER_RETRY_DELAY_MS
#define WIFI_MANAGER_RETRY_DELAY_MS 5000UL
#endif

#ifndef WIFI_MANAGER_REBOOT_AFTER_MS
#define WIFI_MANAGER_REBOOT_AFTER_MS (20UL * 60UL * 1000UL)
#endif

void setupWiFiManager() {
    WiFiManager wm;
    WiFi.persistent(true);
    WiFi.setAutoReconnect(true);
    Serial.println("[WiFi] Auto-reconnect enabled; flash persistence enabled.");
    
    // reset settings if requested or specific condition met
    // wm.resetSettings();
    
    // set dark theme
    wm.setClass("invert");

    // Force autoConnect() to return periodically so we can keep retrying forever
    // and escalate to a full reboot only after a very long outage window.
    wm.setConfigPortalTimeout(WIFI_MANAGER_PORTAL_TIMEOUT_S);

    unsigned long firstFailureAt = 0;
    uint32_t attempt = 0;
    while (true) {
        attempt++;
        Serial.printf("[WiFi] autoConnect attempt #%lu (portal timeout=%us, AP=%s)\n",
                      (unsigned long)attempt,
                      (unsigned)WIFI_MANAGER_PORTAL_TIMEOUT_S,
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
                      (unsigned long)WIFI_MANAGER_RETRY_DELAY_MS,
                      downFor);

        if (downFor >= WIFI_MANAGER_REBOOT_AFTER_MS) {
            Serial.printf("[WiFi] Offline for %lums despite retries. Rebooting for recovery.\n", downFor);
            delay(1000);
            ESP.restart();
        }

        delay(WIFI_MANAGER_RETRY_DELAY_MS);
    }
}
