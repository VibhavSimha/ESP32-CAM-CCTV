#include "tunnel_client.h"
#include "config.h"

#define TUN_LOCALTUNNEL
#include <esp32tunnel.h>

void tunnelBegin() {
    Serial.println("Starting esp32-tunnel with LOCALTUNNEL...");
    tunnelSetup(LOCALTUNNEL, CONFIG_TUNNEL_SUBDOMAIN);
}

void tunnelLoop() {
    ::tunnelLoop(); // Call the global tunnelLoop provided by esp32tunnel library
    
    static unsigned long lastTunnelPrint = 0;
    if (tunnelReady() && millis() - lastTunnelPrint > 30000) {
        lastTunnelPrint = millis();
        Serial.print("Tunnel URL: ");
        Serial.println(tunnelURL());
    }
}
