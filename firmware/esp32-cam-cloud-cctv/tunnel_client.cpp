#include "tunnel_client.h"
#include "config.h"

#include <esp32tunnel.h>

#ifndef CONFIG_TUNNEL_MODE_BORE
#define CONFIG_TUNNEL_MODE_BORE 1
#endif

#ifndef CONFIG_TUNNEL_MODE_SELFHOST
#define CONFIG_TUNNEL_MODE_SELFHOST 2
#endif

#ifndef CONFIG_TUNNEL_MODE
#define CONFIG_TUNNEL_MODE CONFIG_TUNNEL_MODE_BORE
#endif

#ifndef CONFIG_BORE_SERVER
#define CONFIG_BORE_SERVER "bore.pub"
#endif

#ifndef CONFIG_BORE_REMOTE_PORT
#define CONFIG_BORE_REMOTE_PORT 0
#endif

#ifndef CONFIG_SELFHOST_TUNNEL_ID
#define CONFIG_SELFHOST_TUNNEL_ID "esp32-cam"
#endif

void tunnelBegin() {
    tunnelLog(true); // Enable library debug logs

#if CONFIG_TUNNEL_MODE == CONFIG_TUNNEL_MODE_SELFHOST
    Serial.println("Starting esp32-tunnel SELFHOST provider for a stable public URL...");
    Serial.print("[Tunnel] Expected URL: http://esp32-tunnel.onrender.com/");
    Serial.print(CONFIG_SELFHOST_TUNNEL_ID);
    Serial.println("/view");
    tunnelPublic();
    tunnelSetup(SELFHOST, "esp32-tunnel.onrender.com/" CONFIG_SELFHOST_TUNNEL_ID);
#else
    Serial.println("Starting esp32-tunnel with BORE (low RAM, public TCP tunnel)...");
    Serial.print("[Tunnel] BORE server: ");
    Serial.println(CONFIG_BORE_SERVER);
#if CONFIG_BORE_REMOTE_PORT != 0
    Serial.printf("[Tunnel] Requested fixed BORE port %u, but esp32-tunnel BORE does not expose a fixed-port API. Using the server-assigned port.\n", CONFIG_BORE_REMOTE_PORT);
#endif
    tunnelSetup(BORE, CONFIG_BORE_SERVER);
#endif
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
