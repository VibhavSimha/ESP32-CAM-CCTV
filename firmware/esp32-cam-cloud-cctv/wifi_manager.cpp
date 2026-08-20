#include "wifi_manager.h"
#include "config.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <esp_wifi.h>

// Apply the MAC address specified by CAPTIVE_MAC_SPOOF before connecting.
// Returns true on success (or when spoofing is disabled), false on parse/apply error.
static bool applyMacSpoof() {
#ifndef CAPTIVE_MAC_SPOOF
    return true;
#else
    const char* macStr = CAPTIVE_MAC_SPOOF;
    if (!macStr || macStr[0] == '\0') {
        return true; // spoofing disabled
    }

    // Parse "AA:BB:CC:DD:EE:FF" (colon or dash separated, any case).
    uint8_t mac[6];
    int parsed = 0;
    const char* p = macStr;
    while (*p && parsed < 6) {
        // skip separators
        while (*p == ':' || *p == '-') p++;
        if (!*p) break;
        char hi = *p++;
        char lo = *p ? *p++ : 0;
        auto hexVal = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int h = hexVal(hi), l = hexVal(lo);
        if (h < 0 || l < 0) {
            Serial.printf("[WiFi] CAPTIVE_MAC_SPOOF: invalid MAC string '%s' — using factory MAC.\n", macStr);
            return false;
        }
        mac[parsed++] = (uint8_t)((h << 4) | l);
    }
    if (parsed != 6) {
        Serial.printf("[WiFi] CAPTIVE_MAC_SPOOF: could not parse 6 bytes from '%s' — using factory MAC.\n", macStr);
        return false;
    }

    // Unicast bit must be clear (bit 0 of first octet); multicast MACs are
    // rejected by esp_wifi_set_mac and could prevent connecting entirely.
    if (mac[0] & 0x01) {
        Serial.printf("[WiFi] CAPTIVE_MAC_SPOOF: '%s' is a multicast/broadcast MAC — using factory MAC.\n", macStr);
        return false;
    }

    // Wi-Fi must be initialised (but not yet connected) before set_mac is called.
    WiFi.mode(WIFI_STA);
    esp_err_t err = esp_wifi_set_mac(WIFI_IF_STA, mac);
    if (err != ESP_OK) {
        Serial.printf("[WiFi] CAPTIVE_MAC_SPOOF: esp_wifi_set_mac failed (0x%x) — using factory MAC.\n", (unsigned)err);
        return false;
    }
    Serial.printf("[WiFi] MAC spoofed to %02X:%02X:%02X:%02X:%02X:%02X (CAPTIVE_MAC_SPOOF active — issue #50).\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    Serial.println("[WiFi] NOTE: keep the donor device off this Wi-Fi to avoid ARP conflicts.");
    return true;
#endif
}

void setupWiFiManager() {
    // Apply MAC spoof BEFORE WiFiManager initialises the radio, so the ISP
    // portal sees the already-authorised MAC on the very first association
    // (issue #50). applyMacSpoof() sets WIFI_STA mode internally; WiFiManager
    // is fine with that and will reconnect using the spoofed MAC.
    applyMacSpoof();

    WiFiManager wm;
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
