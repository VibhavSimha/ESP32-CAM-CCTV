#include "tunnel_client.h"
#include "config.h"

#include <esp32tunnel.h>
#include <WiFi.h>

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
    Serial.println();
    Serial.println("================ Tunnel Initialization ================");

    tunnelLog(true); // Enable library debug logs
    Serial.println("[Tunnel] Library debug logging ENABLED.");

    Serial.printf("[Tunnel] Free Heap before init: %u bytes\n", ESP.getFreeHeap());

    Serial.print("[Tunnel] WiFi Status: ");
    Serial.println(WiFi.status());

    Serial.print("[Tunnel] Local IP: ");
    Serial.println(WiFi.localIP());

    Serial.print("[Tunnel] Hostname: ");
    Serial.println(WiFi.getHostname());

#if CONFIG_TUNNEL_MODE == CONFIG_TUNNEL_MODE_SELFHOST

    Serial.println("[Tunnel] Mode: SELFHOST");

    Serial.print("[Tunnel] Tunnel ID: ");
    Serial.println(CONFIG_SELFHOST_TUNNEL_ID);

    Serial.print("[Tunnel] Server: ");
    Serial.println("esp32-tunnel.onrender.com");

    Serial.print("[Tunnel] Expected Public URL: http://esp32-tunnel.onrender.com/");
    Serial.print(CONFIG_SELFHOST_TUNNEL_ID);
    Serial.println("/view");

    IPAddress ip;
    if (WiFi.hostByName("esp32-tunnel.onrender.com", ip)) {
        Serial.print("[Tunnel] DNS resolved to: ");
        Serial.println(ip);
    } else {
        Serial.println("[Tunnel] DNS resolution FAILED!");
    }

    Serial.println("[Tunnel] Calling tunnelPublic()...");
    tunnelPublic();
    Serial.println("[Tunnel] tunnelPublic() returned.");

    Serial.println("[Tunnel] Calling tunnelSetup()...");
    tunnelSetup(SELFHOST,
                "esp32-tunnel.onrender.com/" CONFIG_SELFHOST_TUNNEL_ID);
    Serial.println("[Tunnel] tunnelSetup() returned.");

#else

    Serial.println("[Tunnel] Mode: BORE");

    Serial.print("[Tunnel] BORE Server: ");
    Serial.println(CONFIG_BORE_SERVER);

    IPAddress ip;
    if (WiFi.hostByName(CONFIG_BORE_SERVER, ip)) {
        Serial.print("[Tunnel] DNS resolved to: ");
        Serial.println(ip);
    } else {
        Serial.println("[Tunnel] DNS resolution FAILED!");
    }

#if CONFIG_BORE_REMOTE_PORT != 0
    Serial.printf("[Tunnel] Requested fixed BORE port %u (library will ignore this).\n",
                  CONFIG_BORE_REMOTE_PORT);
#endif

    Serial.println("[Tunnel] Calling tunnelSetup()...");
    tunnelSetup(BORE, CONFIG_BORE_SERVER);
    Serial.println("[Tunnel] tunnelSetup() returned.");

#endif

    Serial.print("[Tunnel] tunnelReady() immediately after setup: ");
    Serial.println(tunnelReady() ? "TRUE" : "FALSE");

    Serial.print("[Tunnel] tunnelURL(): ");
    Serial.println(tunnelURL());

    Serial.printf("[Tunnel] Free Heap after init: %u bytes\n", ESP.getFreeHeap());

    Serial.println("=======================================================");
    Serial.println();
}

void handleTunnel() {
    static unsigned long lastStatusLog = 0;
    static bool firstPrintDone = false;

    bool ready = tunnelReady();

    if (ready) {
        if (!firstPrintDone || millis() - lastStatusLog > 30000) {
            lastStatusLog = millis();
            firstPrintDone = true;

            Serial.println();
            Serial.println("============= TUNNEL CONNECTED =============");
            Serial.print("[Tunnel] URL: ");
            Serial.println(tunnelURL());
            Serial.printf("[Tunnel] Heap: %u bytes\n", ESP.getFreeHeap());
            Serial.println("============================================");
        }
    } else {
        if (millis() - lastStatusLog > 5000) {
            lastStatusLog = millis();

            Serial.println();
            Serial.println("------------- Tunnel Status -------------");
            Serial.println("[Tunnel] tunnelReady(): FALSE");
            Serial.printf("[Tunnel] Uptime: %lu ms\n", millis());
            Serial.printf("[Tunnel] Heap: %u bytes\n", ESP.getFreeHeap());

            Serial.print("[Tunnel] WiFi Status: ");
            Serial.println(WiFi.status());

            Serial.print("[Tunnel] Local IP: ");
            Serial.println(WiFi.localIP());

            Serial.print("[Tunnel] tunnelURL(): ");
            Serial.println(tunnelURL());

            Serial.println("------------------------------------------");
        }
    }
}