#include "tunnel_client.h"
#include "config.h"

#include "src/esp32tunnel.h"
#include <WiFi.h>
#include "cloud_storage.h"

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

  tunnelLog(true);  // Enable library debug logs
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

  Serial.print("[Tunnel] Expected Public URL: https://esp32-tunnel.onrender.com/");
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
  String url = String("https://esp32-tunnel.onrender.com/") + CONFIG_SELFHOST_TUNNEL_ID;

  Serial.print("[Tunnel] Setup URL = ");
  Serial.println(url);

  tunnelSetup(SELFHOST, url.c_str());
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
  static bool wasReady = false;
  static bool watchdogLogged = false;
  static unsigned long lastWifiReconnectAttempt = 0;
  static bool tunnelStoppedForWifi = false;
  static unsigned long wifiConnectedSince = 0;
  static unsigned long wifiLostSince = 0;

  if (!watchdogLogged) {
    watchdogLogged = true;
    Serial.println("[Tunnel] Main-loop watchdog enabled.");
  }
  tunnelWatchdog();

  wl_status_t wifiStatus = WiFi.status();
  if (wifiStatus != WL_CONNECTED) {
    unsigned long now = millis();
    wifiConnectedSince = 0;
    if (wifiLostSince == 0) wifiLostSince = now;      // start debounce timer

    // Only act after WiFi has been down continuously for >5s. A transient
    // status=6 blip caused by socket churn must NOT restart the tunnel.
    if (now - wifiLostSince > 5000) {
      if (!tunnelStoppedForWifi) {
        tunnelStoppedForWifi = true;
        wasReady = false;
        Serial.printf("[Tunnel] WiFi durably lost (%lums). Stopping tunnel. status=%d ip=%s heap=%u\n",
            now - wifiLostSince, wifiStatus, WiFi.localIP().toString().c_str(), ESP.getFreeHeap());
        tunnelStop();
      }
      if (now - lastWifiReconnectAttempt > 5000) {
        lastWifiReconnectAttempt = now;
        Serial.printf("[WiFi] Link down. status=%d ip=%s heap=%u. Calling WiFi.reconnect().\n",
            wifiStatus, WiFi.localIP().toString().c_str(), ESP.getFreeHeap());
        WiFi.reconnect();
      }
    }
  } else if (tunnelStoppedForWifi) {
    unsigned long now = millis();
    wifiLostSince = 0;                                 // reset debounce
    if (wifiConnectedSince == 0) {
      wifiConnectedSince = now;
      Serial.printf("[WiFi] Reconnected. ip=%s heap=%u. Waiting for link to stabilize before tunnel restart.\n",
          WiFi.localIP().toString().c_str(), ESP.getFreeHeap());
    } else if (now - wifiConnectedSince > 3000) {
      tunnelStoppedForWifi = false;
      Serial.printf("[Tunnel] Restarting tunnel after WiFi recovery. stable_for=%lums ip=%s heap=%u\n",
          now - wifiConnectedSince, WiFi.localIP().toString().c_str(), ESP.getFreeHeap());
      tunnelBegin();
    }
  } else {
    wifiLostSince = 0;                                 // WiFi fine, tunnel up: clear debounce
  }

  bool ready = tunnelReady();

  if (ready) {
    if (!wasReady) {
      Serial.println();
      Serial.println("============= TUNNEL CONNECTED =============");
      Serial.print("[Tunnel] URL: ");
      Serial.println(tunnelURL());
      Serial.printf("[Tunnel] Heap: %u bytes\n", ESP.getFreeHeap());
      Serial.println("============================================");
      
      publishTunnelUrl(tunnelURL());
      wasReady = true;
    }
  } else {
    wasReady = false;
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
