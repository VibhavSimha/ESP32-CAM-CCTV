/*
 * esp32tunnel_bore.h — bore.pub TCP tunnel provider
 * Internal — included by esp32tunnel.h, do not include directly.
 *
 * Protocol (bore v0.5+):
 *   Control port 7835, JSON messages delimited by \0
 *   Client sends {"Hello":0}\0       → server replies {"Hello":PORT}\0
 *   Server sends {"Connection":"UUID"}\0  → client opens new TCP to 7835,
 *     sends {"Accept":"UUID"}\0, then proxies raw TCP ↔ local port.
 *
 * Public server: bore.pub (free, no login)
 * Self-hosted:   `bore server` on your VPS
 */

#ifndef ESP32TUNNEL_BORE_H
#define ESP32TUNNEL_BORE_H

// ---------------------------------------------------------------------------
// MARK: Config
// ---------------------------------------------------------------------------

#ifndef BORE_CONTROL_PORT
#define BORE_CONTROL_PORT 7835
#endif

#ifndef BORE_MAX_PROXY
#define BORE_MAX_PROXY 4
#endif

// ---------------------------------------------------------------------------
// MARK: State
// ---------------------------------------------------------------------------

static struct {
  String host, url, lastIP;
  uint16_t remotePort;
  uint16_t localPort;
  _Phase phase;
  unsigned long waitUntil;
  bool ready, stop, started;
} _bore = {};

static WiFiClient _boreCtrl;
static WiFiClient _boreProxy[BORE_MAX_PROXY];
static WiFiClient _boreLocal[BORE_MAX_PROXY];
static volatile bool _slotBusy[BORE_MAX_PROXY] = {false};

// ---------------------------------------------------------------------------
// MARK: Null-delimited JSON read — reads until \0 or timeout
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// MARK: Null-delimited JSON read — reads until \0 or timeout
// ---------------------------------------------------------------------------

static String _boreRecvMsg(WiFiClient &c, int timeoutMs = 5000) {
  char buf[256];
  int pos = 0;
  unsigned long t = millis();
  while (millis() - t < (unsigned long)timeoutMs) {
    if (!c.connected()) return "";
    if (!c.available()) { _DELAY(1); continue; }
    char ch = c.read();
    if (ch == '\0') {
      buf[pos] = '\0';
      return String(buf);
    }
    if (pos < 255) {
      buf[pos++] = ch;
    } else {
      return "";
    }
  }
  return "";
}

// MARK: Send null-terminated JSON
static void _boreSendMsg(WiFiClient &c, const String &msg) {
  c.print(msg);
  c.write((uint8_t)0);
}

// Server messages — {"Hello":PORT}, {"Connection":"UUID"}, {"Error":"MSG"} —
// are parsed with the shared _jInt/_jStr helpers at the call sites.

// ---------------------------------------------------------------------------
// MARK: Proxy — bidirectional TCP copy
// ---------------------------------------------------------------------------

static void _boreProxyConn(WiFiClient &remote, WiFiClient &local, int slot) {
  uint8_t *buf = (uint8_t *)malloc(2048);
  if (!buf) {
    Serial.printf("[Tunnel] Slot %d FATAL: malloc failed! Aborting proxy.\n", slot);
    _slotBusy[slot] = false;
    return;
  }
  
  Serial.printf("[Tunnel] Proxy task started in slot %d\n", slot);

  unsigned long idle = millis();
  unsigned long start = millis();
  size_t totalRx = 0, totalTx = 0;
  bool backpressureWarning = false;

  // Throughput stall detector: if we TX bytes to WAN but progress stops, the WAN is a black hole
  size_t lastTxSnapshot = 0;
  unsigned long lastTxProgress = millis();
  unsigned long lastHeartbeat = millis();
  bool streamingActive = false; // true once we've seen any LAN->WAN traffic

  while (remote.connected() && local.connected() && millis() - idle < 30000) {
    bool activity = false;

    // Periodic heartbeat: log proxy state every 10s so we can see it's alive
    if (millis() - lastHeartbeat > 10000) {
      lastHeartbeat = millis();
      Serial.printf("[Tunnel] Slot %d alive: TX=%u RX=%u idle=%lums r=%d l=%d\n",
                    slot, totalTx, totalRx, millis() - idle,
                    remote.connected(), local.connected());
    }

    // Throughput stall detector: if streaming and no TX progress for 10s → WAN black hole
    if (streamingActive && millis() - lastTxProgress > 10000) {
      if (totalTx == lastTxSnapshot) {
        Serial.printf("[Tunnel] Slot %d STALL: TX frozen at %u bytes for 10s! WAN may be silently dropping. Killing slot.\n",
                      slot, totalTx);
        break;
      }
      lastTxSnapshot = totalTx;
      lastTxProgress = millis();
    }

    // Remote (WAN) -> Local (camera server)
    if (remote.available()) {
      int n = remote.read(buf, 2048);
      if (n > 0) {
        int written = 0;
        unsigned long innerDeadline = millis() + 8000; // 8s ABSOLUTE deadline for this entire chunk
        while (written < n && remote.connected() && local.connected()) {
          if (millis() > innerDeadline) {
            Serial.printf("[Tunnel] Slot %d WAN->LAN DEADLOCK! written=%d/%d after 8s. Killing slot.\n", slot, written, n);
            break;
          }
          size_t w = local.write(buf + written, n - written);
          if (w == 0) {
            _DELAY(2);
          } else {
            written += w;
          }
        }
        if (written < n) {
          Serial.printf("[Tunnel] Slot %d WAN->LAN write incomplete (%d/%d bytes). r=%d l=%d. Breaking.\n",
                        slot, written, n, remote.connected(), local.connected());
          break;
        }
        totalRx += written;
        activity = true;
        backpressureWarning = false;
      }
    }

    // Local (camera server) -> Remote (WAN)
    if (local.available()) {
      int n = local.read(buf, 2048);
      if (n > 0) {
        streamingActive = true; // we've started carrying LAN traffic
        int written = 0;
        unsigned long innerDeadline = millis() + 8000; // 8s ABSOLUTE deadline for this entire chunk
        while (written < n && remote.connected() && local.connected()) {
          if (millis() > innerDeadline) {
            Serial.printf("[Tunnel] Slot %d LAN->WAN DEADLOCK! written=%d/%d after 8s. Killing slot.\n", slot, written, n);
            break;
          }
          size_t w = remote.write(buf + written, n - written);
          if (w == 0) {
            if (!backpressureWarning) {
              Serial.printf("[Tunnel] Slot %d WAN buffer full! Engaging backpressure...\n", slot);
              backpressureWarning = true;
            }
            _DELAY(2);
          } else {
            written += w;
            backpressureWarning = false;
          }
        }
        if (written < n) {
          Serial.printf("[Tunnel] Slot %d LAN->WAN write incomplete (%d/%d bytes). r=%d l=%d. Breaking.\n",
                        slot, written, n, remote.connected(), local.connected());
          break;
        }
        totalTx += written;
        lastTxProgress = millis(); // TX made progress
        activity = true;
        backpressureWarning = false;
      }
    }

    if (activity) idle = millis();
    else _DELAY(1);
  }


  const char *reason = "Idle timeout (30s)";
  if (!remote.connected()) reason = "WAN disconnect";
  else if (!local.connected()) reason = "Local disconnect";
  else reason = "Write failure / timeout";

  Serial.printf("[Tunnel] Proxy slot %d closed. Reason: %s. Duration: %lums, TX: %u bytes, RX: %u bytes\n",
                slot, reason, millis() - start, totalTx, totalRx);
  Serial.printf("[Tunnel] Slot %d final state: remote.connected=%d, local.connected=%d, idle_age=%lums\n",
                slot, remote.connected(), local.connected(), millis() - idle);

  free(buf);
  remote.stop();
  local.stop();
  _slotBusy[slot] = false;
}

// MARK: Handle a single Connection message — open accept stream + proxy
static void _boreAccept(const String &uuid, int slot) {
  WiFiClient &proxy = _boreProxy[slot];
  WiFiClient &local = _boreLocal[slot];

  Serial.printf("[Tunnel] Slot %d: Connecting proxy socket to bore.pub...\n", slot);
  if (!_tcpConnectHost(proxy, _bore.host.c_str(), BORE_CONTROL_PORT)) {
    Serial.printf("[Tunnel] Slot %d: FAILED to connect proxy socket to bore.pub! Releasing slot.\n", slot);
    _slotBusy[slot] = false;
    return;
  }
  Serial.printf("[Tunnel] Slot %d: Sending Accept for UUID=%s\n", slot, uuid.c_str());
  _boreSendMsg(proxy, "{\"Accept\":\"" + uuid + "\"}");

  Serial.printf("[Tunnel] Slot %d: Connecting local socket to 127.0.0.1:%d...\n", slot, _bore.localPort);
  if (!_tcpConnect(local, IPAddress(127, 0, 0, 1), _bore.localPort)) {
    Serial.printf("[Tunnel] Slot %d: FAILED to connect local socket to camera server! Releasing slot.\n", slot);
    proxy.stop();
    _slotBusy[slot] = false;
    return;
  }
  proxy.setTimeout(3);  // 3 SECONDS (ESP32 WiFiClient uses seconds!)
  local.setTimeout(3);
  proxy.setNoDelay(true);
  local.setNoDelay(true);
  Serial.printf("[Tunnel] Slot %d: Both sockets connected. Entering proxy loop.\n", slot);
  _boreProxyConn(proxy, local, slot);
}

// ---------------------------------------------------------------------------
// MARK: Init + serve
// ---------------------------------------------------------------------------

static bool _boreInit() {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (_boreCtrl.connected()) _boreCtrl.stop();

  if (!_tcpConnectHost(_boreCtrl, _bore.host.c_str(), BORE_CONTROL_PORT)) return false;
  _setKeepAlive(_boreCtrl);

  // MARK: Send Hello — request random port (0)
  _boreSendMsg(_boreCtrl, "{\"Hello\":0}");

  String resp = _boreRecvMsg(_boreCtrl, 10000);
  if (!resp.length()) { _boreCtrl.stop(); return false; }

  // Check for error
  String err = _jStr(resp, "Error");
  if (err.length()) { _boreCtrl.stop(); return false; }

  int port = _jInt(resp, "Hello", -1);
  if (port <= 0) { _boreCtrl.stop(); return false; }

  _bore.remotePort = (uint16_t)port;
  _bore.url = "http://" + _bore.host + ":" + String(port);
  _bore.ready = true;
  return true;
}

static unsigned long _lastBorePing = 0;

static bool _boreServe() {
  if (!_boreCtrl.connected()) return false;

  // Application-level keepalive to detect silent WAN drops
  if (millis() - _lastBorePing > 30000) {
    _lastBorePing = millis();
    _boreSendMsg(_boreCtrl, "{\"Ping\":1}");
    if (_boreCtrl.getWriteError()) {
      Serial.printf("[Tunnel] ERROR: Control socket write failed (silent drop detected)!\n");
      return false;
    }
  }

  // Drain all queued control messages in a loop (Bug #7)
  while (_boreCtrl.connected() && _boreCtrl.available()) {
    String msg = _boreRecvMsg(_boreCtrl, 500); // 500ms safe timeout (Bug #5)
    if (!msg.length()) break;

    // Heartbeat — ignore
    if (msg.indexOf("\"Heartbeat\"") >= 0) continue;

    // Connection request
    String uuid = _jStr(msg, "Connection");
    if (!uuid.length()) continue;

    // Find a free proxy slot
    int slot = -1;
    for (int i = 0; i < BORE_MAX_PROXY; i++) {
      if (!_slotBusy[i]) {
        slot = i; break;
      }
    }
    if (slot < 0) continue; // all slots busy

    _slotBusy[slot] = true; // Mark busy immediately so concurrent connections don't steal it
    
    // Dispatch each proxy connection to its own FreeRTOS task (Bug #1)
    struct _ProxyArgs { String uuid; int slot; };
    auto *args = new _ProxyArgs{uuid, slot};
    xTaskCreatePinnedToCore([](void *p) {
      auto *a = (_ProxyArgs*)p;
      _boreAccept(a->uuid, a->slot);
      delete a;
      vTaskDelete(nullptr);
    }, "boreProxy", 6144, args, 1, nullptr, TUN_TASK_CORE);
  }

  return true;
}

// ---------------------------------------------------------------------------
// MARK: Internal API (called by dispatch in esp32tunnel.h)
// ---------------------------------------------------------------------------

static void _boreBegin(const char *host, uint16_t localPort) {
  if (_bore.started) return;
  _bore.host      = (host && strlen(host) > 0) ? host : "bore.pub";
  _bore.localPort = localPort > 0 ? localPort : TUN_PORT;
  _bore.stop      = false;
  _bore.ready     = false;
  _bore.started   = true;
  _bore.url       = "(connecting...)";
  _bore.phase     = _PH_INIT;
}

static unsigned long _boreBackoff = 2000;

static void _boreLoop() {
  if (!_bore.started || _bore.stop) return;
  switch (_bore.phase) {
    case _PH_IDLE: break;
    case _PH_INIT:
      if (_boreInit()) { _bore.phase = _PH_SERVE; _boreBackoff = 2000; }
      else {
        _bore.phase = _PH_WAIT;
        _bore.waitUntil = millis() + _boreBackoff;
        _boreBackoff = min(_boreBackoff * 2, 60000UL);
      }
      break;
    case _PH_SERVE:
      if (!_boreServe()) {
        _boreCtrl.stop(); _bore.ready = false;
        if (!TUN_RECONNECT) { _bore.phase = _PH_IDLE; _bore.started = false; }
        else { _bore.phase = _PH_WAIT; _bore.waitUntil = millis() + _boreBackoff; _boreBackoff = min(_boreBackoff * 2, 60000UL); }
      }
      break;
    case _PH_WAIT:
      if (millis() >= _bore.waitUntil) _bore.phase = _PH_INIT;
      break;
  }
}

static void _boreStop() {
  _bore.stop = true;
  _boreCtrl.stop();
  for (int i = 0; i < BORE_MAX_PROXY; i++) {
    _boreProxy[i].stop();
    _boreLocal[i].stop();
    _slotBusy[i] = false;
  }
  _bore.ready   = false;
  _bore.started = false;
  _bore.phase   = _PH_IDLE;
  _bore.url     = "(stopped)";
}

#endif
