#include "tunnel_client.h"
#include "config.h"

#include <esp32tunnel.h>

void tunnelBegin() {
    Serial.println("Starting esp32-tunnel with BORE (More reliable, no TLS overhead)...");
    tunnelLog(true); // Enable library debug logs
    tunnelSetup(BORE);
}

void handleTunnel() {
    static unsigned long lastStatusLog = 0;
    static bool firstPrintDone = false;
    
    if (tunnelReady()) {
        if (!firstPrintDone || millis() - lastStatusLog > 30000) {
            lastStatusLog = millis();
            firstPrintDone = true;
            Serial.print(">>> TUNNEL CONNECTED! URL: ");
            Serial.println(tunnelURL());
        }
    } else {
        if (millis() - lastStatusLog > 5000) {
            lastStatusLog = millis();
            Serial.println("[Tunnel] Status: tunnelReady() is FALSE. Waiting...");
        }
    }
}
