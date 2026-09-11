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

#ifndef TUNNEL_NOT_READY_RECOVERY_MS
#define TUNNEL_NOT_READY_RECOVERY_MS 45000UL
#endif

#ifndef TUNNEL_SOFT_RESTART_COOLDOWN_MS
#define TUNNEL_SOFT_RESTART_COOLDOWN_MS 30000UL
#endif

#ifndef TUNNEL_MAX_SOFT_RECOVERY_ATTEMPTS
#define TUNNEL_MAX_SOFT_RECOVERY_ATTEMPTS 12
#endif

#ifndef TUNNEL_NOT_READY_REBOOT_AFTER_MS
#define TUNNEL_NOT_READY_REBOOT_AFTER_MS (15UL * 60UL * 1000UL)
#endif

#ifndef TUNNEL_WIFI_LOST_REBOOT_AFTER_MS
#define TUNNEL_WIFI_LOST_REBOOT_AFTER_MS (15UL * 60UL * 1000UL)
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
  static unsigned long tunnelNotReadySince = 0;
  static unsigned long lastTunnelSoftRestart = 0;
  static uint16_t tunnelSoftRestartCount = 0;

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
    tunnelNotReadySince = 0;
    lastTunnelSoftRestart = 0;
    tunnelSoftRestartCount = 0;

    // Only act after WiFi has been down continuously for >5s. A transient
    // status=6 blip caused by socket churn must NOT restart the tunnel.
    if (now - wifiLostSince > 12000) {
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
      unsigned long wifiDownFor = now - wifiLostSince;
      if (wifiDownFor >= TUNNEL_WIFI_LOST_REBOOT_AFTER_MS) {
        Serial.printf("[WiFi] Link down for %lums despite reconnect attempts. Rebooting for recovery.\n",
                      wifiDownFor);
        delay(1000);
        ESP.restart();
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
    tunnelNotReadySince = 0;
    lastTunnelSoftRestart = 0;
    tunnelSoftRestartCount = 0;
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
    unsigned long now = millis();
    bool canRecoverTunnel = (wifiStatus == WL_CONNECTED) && !tunnelStoppedForWifi;
    if (!canRecoverTunnel) {
      tunnelNotReadySince = 0;
      lastTunnelSoftRestart = 0;
      tunnelSoftRestartCount = 0;
    } else {
      if (tunnelNotReadySince == 0) tunnelNotReadySince = now;
      unsigned long downFor = now - tunnelNotReadySince;

      if (downFor >= TUNNEL_NOT_READY_RECOVERY_MS &&
          (lastTunnelSoftRestart == 0 ||
           now - lastTunnelSoftRestart >= TUNNEL_SOFT_RESTART_COOLDOWN_MS) &&
          !isTunnelSlotBusy()) {
        tunnelSoftRestartCount++;
        lastTunnelSoftRestart = now;
        Serial.printf("[Tunnel] Not ready for %lums. Soft recovery %u/%u: restarting tunnel client.\n",
                      downFor,
                      (unsigned)tunnelSoftRestartCount,
                      (unsigned)TUNNEL_MAX_SOFT_RECOVERY_ATTEMPTS);
        tunnelStop();
        delay(150);
        tunnelBegin();
      }

      if (downFor >= TUNNEL_NOT_READY_REBOOT_AFTER_MS ||
          tunnelSoftRestartCount >= TUNNEL_MAX_SOFT_RECOVERY_ATTEMPTS) {
        Serial.printf("[Tunnel] Tunnel unavailable for %lums after %u soft recoveries. Rebooting for recovery.\n",
                      downFor,
                      (unsigned)tunnelSoftRestartCount);
        delay(1000);
        ESP.restart();
      }
    }

    if (millis() - lastStatusLog > 5000) {
      lastStatusLog = millis();
      unsigned long downFor = (tunnelNotReadySince == 0) ? 0 : (millis() - tunnelNotReadySince);

      Serial.println();
      Serial.println("------------- Tunnel Status -------------");
      Serial.println("[Tunnel] tunnelReady(): FALSE");
      Serial.printf("[Tunnel] Uptime: %lu ms\n", millis());
      Serial.printf("[Tunnel] Heap: %u bytes\n", ESP.getFreeHeap());
      Serial.printf("[Tunnel] Not-ready duration: %lu ms | soft recoveries: %u\n",
                    downFor, (unsigned)tunnelSoftRestartCount);

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

void tunnelStopNow() {
  tunnelStop();
}

bool isTunnelSlotBusy() {
  return tunnelBusy();
}
