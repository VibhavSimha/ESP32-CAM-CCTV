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

#include <new>

#ifndef ESP32TUNNEL_BORE_H
#define ESP32TUNNEL_BORE_H

// ---------------------------------------------------------------------------
// MARK: Config
// ---------------------------------------------------------------------------

#ifndef BORE_CONTROL_PORT
#define BORE_CONTROL_PORT 7835
#endif

#ifndef BORE_MAX_PROXY
#define BORE_MAX_PROXY 2
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
static volatile unsigned long _slotLastActive[BORE_MAX_PROXY] = {0};
static volatile bool _slotWatchdogArmed[BORE_MAX_PROXY] = {false};
static volatile bool _slotBackpressured[BORE_MAX_PROXY] = {false};
static volatile unsigned long _slotBackpressureSince[BORE_MAX_PROXY] = {0};
static TaskHandle_t _boreProxyTaskHandle[BORE_MAX_PROXY] = {nullptr};

// Per-slot task argument (static so it outlives the launching function)
struct _BoreProxyArg { int slot; };
static _BoreProxyArg _boreProxyArgs[BORE_MAX_PROXY];

// When a Connection arrives while streaming is active we defer it rather than
// drop it.  Saving the UUID here means the *next* _boreServe() iteration —
// which runs on every main-loop tick — can accept it as soon as the streaming
// slot is released.  Without buffering, a flash-toggle request sent right
// after the browser closes the stream src races with the proxy-task exit and
// the Connection message gets dropped, causing the request to time out.
static String _boreDeferredUUID = "";

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
    Serial.printf("[Tunnel] Slot %d FATAL: malloc(2048) failed! heap=%u. Aborting proxy.\n",
                  slot, ESP.getFreeHeap());
    _slotBackpressured[slot] = false;
    _slotBackpressureSince[slot] = 0;
    _slotWatchdogArmed[slot] = false;
    _slotLastActive[slot] = 0;
    _slotBusy[slot] = false;
    return;
  }

  Serial.printf("[Tunnel] Proxy task started in slot %d. heap=%u\n", slot, ESP.getFreeHeap());

  unsigned long idle = millis();
  unsigned long start = millis();
  size_t totalRx = 0, totalTx = 0;
  bool backpressureWarning = false;

  size_t lastTxSnapshot = 0;
  unsigned long lastTxProgress = millis();
  unsigned long lastHeartbeat = millis();
  bool streamingActive = false;

  // Arm the watchdog with a real timestamp BEFORE anything can block.
  _slotBackpressured[slot] = false;
  _slotBackpressureSince[slot] = 0;
  _slotLastActive[slot] = start;
  _slotWatchdogArmed[slot] = true;
  Serial.printf("[Tunnel] Slot %d watchdog armed at %lums.\n", slot, start);

  const char *exitReason = "unknown";

  while (remote.connected() && local.connected() &&
         millis() - idle < (streamingActive ? 30000UL : 8000UL)) {
    bool activity = false;
    _slotLastActive[slot] = millis(); // pet watchdog each iteration (proves task is looping)

    if (millis() - lastHeartbeat > 10000) {
      lastHeartbeat = millis();
      Serial.printf("[Tunnel] Slot %d alive: TX=%u RX=%u idle=%lums bp=%d r=%d l=%d heap=%u\n",
                    slot, totalTx, totalRx, millis() - idle, _slotBackpressured[slot],
                    remote.connected(), local.connected(), ESP.getFreeHeap());
    }

    // Throughput stall detector (genuine WAN black-hole while streaming).
    if (streamingActive) {
      if (millis() - lastTxProgress > 10000) {
        Serial.printf("[Tunnel] Slot %d STALL CHECK: TX now=%u snapshot=%u\n",
                      slot, totalTx, lastTxSnapshot);
        if (totalTx == lastTxSnapshot) {
          Serial.printf("[Tunnel] Slot %d STALL: TX frozen at %u bytes for 10s! Killing slot.\n",
                        slot, totalTx);
          exitReason = "stall-tx-frozen";
          break;
        }
        lastTxSnapshot = totalTx;
        lastTxProgress = millis();
      }
    }

    // ---- Remote (WAN) -> Local (camera server) ----
    if (remote.available()) {
      int n = remote.read(buf, 2048);
      if (n < 0) {
        Serial.printf("[Tunnel] Slot %d WAN->LAN read error (n=%d). r=%d l=%d. Breaking.\n",
                      slot, n, remote.connected(), local.connected());
        exitReason = "wan-read-error";
        break;
      }
      if (n > 0) {
        int written = 0;
        unsigned long innerDeadline = millis() + 3000;
        while (written < n && remote.connected() && local.connected()) {
          if (millis() > innerDeadline) {
            Serial.printf("[Tunnel] Slot %d WAN->LAN DEADLOCK! written=%d/%d after 3s. Killing.\n",
                          slot, written, n);
            break;
          }
          size_t w = local.write(buf + written, n - written);
          if (w == 0) {
            _DELAY(2);
          } else {
            written += w;
            _slotLastActive[slot] = millis(); // progress = liveness
          }
        }
        if (written < n) {
          Serial.printf("[Tunnel] Slot %d WAN->LAN write incomplete (%d/%d). r=%d l=%d. Breaking.\n",
                        slot, written, n, remote.connected(), local.connected());
          exitReason = "wan->lan-incomplete";
          break;
        }
        totalRx += written;
        activity = true;
        backpressureWarning = false;
      }
    }

    // ---- Local (camera server) -> Remote (WAN) ----
    if (local.available()) {
      int n = local.read(buf, 2048);
      if (n < 0) {
        Serial.printf("[Tunnel] Slot %d LAN->WAN read error (n=%d). r=%d l=%d. Breaking.\n",
                      slot, n, remote.connected(), local.connected());
        exitReason = "lan-read-error";
        break;
      }
      if (n > 0) {
        streamingActive = true;
        _slotBackpressured[slot] = true;
        if (_slotBackpressureSince[slot] == 0) _slotBackpressureSince[slot] = millis();
        int written = 0;
        unsigned long innerDeadline = millis() + 3000;
        while (written < n && remote.connected() && local.connected()) {
          if (millis() > innerDeadline) {
            Serial.printf("[Tunnel] Slot %d LAN->WAN DEADLOCK! written=%d/%d after 3s heap=%u. Killing.\n",
                          slot, written, n, ESP.getFreeHeap());
            break;
          }
          // Issue #23: pet the watchdog before every write attempt so the
          // bpStuck detector sees recent activity even when write() blocks for
          // its full SO_SNDTIMEO window (now capped at 3s, below the 4s threshold).
          _slotLastActive[slot] = millis();
          size_t w = remote.write(buf + written, n - written);
          if (w == 0) {
            if (!backpressureWarning) {
              Serial.printf("[Tunnel] Slot %d WAN buffer full! Engaging backpressure... heap=%u\n",
                            slot, ESP.getFreeHeap());
              backpressureWarning = true;
            }
            _slotBackpressured[slot] = true; // keep flag while stalled
            _DELAY(2);
          } else {
            written += w;
            backpressureWarning = false;
            _slotLastActive[slot] = millis(); // progress = liveness
            // Backpressure relieved as soon as a write succeeds.
            _slotBackpressured[slot] = false;
            _slotBackpressureSince[slot] = 0;
          }
        }
        if (written < n) {
          Serial.printf("[Tunnel] Slot %d LAN->WAN write incomplete (%d/%d). r=%d l=%d. Breaking.\n",
                        slot, written, n, remote.connected(), local.connected());
          // Clear BP flags on this exit path too (prevents latched bpStuck).
          _slotBackpressured[slot] = false;
          _slotBackpressureSince[slot] = 0;
          exitReason = "lan->wan-incomplete";
          break;
        }
        _slotBackpressured[slot] = false;
        _slotBackpressureSince[slot] = 0;
        totalTx += written;
        lastTxProgress = millis();
        activity = true;
        backpressureWarning = false;
      }
    }

    if (activity) idle = millis();
    else _DELAY(1);
  }

  // Determine why the loop ended (for the log).
  if (exitReason[0] == 'u') { // still "unknown" => while() condition went false
    if (remote.connected() && local.connected()) exitReason = "idle-timeout";
    else if (!remote.connected()) exitReason = "wan-dropped";
    else exitReason = "lan-dropped";
  }

  bool naturalExit = remote.connected() && local.connected();
  Serial.printf("[Tunnel] Slot %d: Loop exit (%s). natural=%d r=%d l=%d TX=%u RX=%u dur=%lums\n",
                slot, exitReason, naturalExit, remote.connected(), local.connected(),
                totalTx, totalRx, millis() - start);

  UBaseType_t stackHWM = uxTaskGetStackHighWaterMark(nullptr);
  Serial.printf("[Tunnel] Slot %d: Stack HWM: %u words. heap=%u\n", slot, stackHWM, ESP.getFreeHeap());

  free(buf);
  remote.stop();
  local.stop();
  // Single, authoritative cleanup of ALL slot state (no path skips this).
  _slotBackpressured[slot] = false;
  _slotBackpressureSince[slot] = 0;
  _slotWatchdogArmed[slot] = false;
  _slotLastActive[slot] = 0;
  _slotBusy[slot] = false;
  Serial.printf("[Tunnel] Slot %d: released.\n", slot);
}

static void _boreWatchdog(const char *source) {
  for (int i = 0; i < BORE_MAX_PROXY; i++) {
    if (!_slotBusy[i] || !_slotWatchdogArmed[i]) continue;

    // Single volatile read of the activity timestamp, THEN read the clock.
    // Reading in this order + the (now < last) guard below makes the
    // subtraction underflow-proof (fixes age=4294967295ms false freeze).
    unsigned long last = _slotLastActive[i];
    if (last == 0) continue;                 // proxy task hasn't armed yet
    unsigned long now = millis();
    if (now < last) {                        // clock skew / just-petted on other core
      // Not an error — just skip this pass. Log at debug volume only.
      // Serial.printf("[Tunnel] WATCHDOG(%s): Slot %d skew last=%lu now=%lu; skipping.\n",
      //               source, i, last, now);
      continue;
    }

    unsigned long age = now - last;

    unsigned long bpSince = _slotBackpressureSince[i];
    // "Stuck" = backpressured AND no forward byte progress for >8s.
    // Issue #27: the 4s threshold was too tight — SO_SNDTIMEO on ESP32 lwIP
    // may not reliably cut off a blocking write() within 3s under WAN
    // congestion. Raise both guards to 8s so transient network hiccups
    // (bore.pub congestion, client buffer stall) do not kill live streams.
    // With SO_SNDTIMEO=3s working correctly, each write returns within 3s,
    // petting the watchdog (age resets), so age never exceeds ~3s and the
    // 8s guard is never falsely triggered. The guard only fires when write()
    // genuinely blocks for 8s with no progress — a real hang.
    bool bpStuck = _slotBackpressured[i] &&
                   bpSince > 0 &&
                   (now - bpSince > 8000) &&
                   (age > 8000);

    // Normal freeze: task blocked inside write() with no progress for 12s,
    // and not currently in a (legitimate, short) backpressure spell.
    bool frozen = (age > 12000) && !_slotBackpressured[i];

    if (frozen || bpStuck) {
      Serial.printf("[Tunnel] WATCHDOG(%s): Slot %d KILL. reason=%s age=%lums bpSince=%lums heap=%u wifi=%d r=%d l=%d\n",
                    source, i, frozen ? "frozen12s" : "bpStuck8s",
                    age, bpSince ? (now - bpSince) : 0UL,
                    ESP.getFreeHeap(), WiFi.status(),
                    _boreProxy[i].connected(), _boreLocal[i].connected());
      _boreProxy[i].stop();
      _boreLocal[i].stop();
      _slotWatchdogArmed[i] = false;
      _slotLastActive[i] = 0;
      _slotBackpressured[i] = false;
      _slotBackpressureSince[i] = 0;
    }
  }
}

// MARK: Handle a single Connection message — open accept stream + proxy
static void _boreAccept(const String &uuid, int slot) {
  WiFiClient &proxy = _boreProxy[slot];
  WiFiClient &local = _boreLocal[slot];

  _slotWatchdogArmed[slot] = false;
  _slotLastActive[slot] = 0;
  Serial.printf("[Tunnel] Slot %d: Connecting proxy socket to bore.pub...\n", slot);
  if (!_tcpConnectHost(proxy, _bore.host.c_str(), BORE_CONTROL_PORT)) {
    Serial.printf("[Tunnel] Slot %d: FAILED to connect proxy socket to bore.pub! Releasing slot.\n", slot);
    _slotWatchdogArmed[slot] = false;
    _slotLastActive[slot] = 0;
    _slotBusy[slot] = false;
    return;
  }
  Serial.printf("[Tunnel] Slot %d: Sending Accept for UUID=%s\n", slot, uuid.c_str());
  _boreSendMsg(proxy, "{\"Accept\":\"" + uuid + "\"}");

  Serial.printf("[Tunnel] Slot %d: Connecting local socket to 127.0.0.1:%d...\n", slot, _bore.localPort);
  if (!_tcpConnect(local, IPAddress(127, 0, 0, 1), _bore.localPort)) {
    Serial.printf("[Tunnel] Slot %d: FAILED to connect local socket to camera server! Releasing slot.\n", slot);
    proxy.stop();
    _slotWatchdogArmed[slot] = false;
    _slotLastActive[slot] = 0;
    _slotBusy[slot] = false;
    return;
  }
  proxy.setTimeout(3);  // 3 SECONDS (ESP32 WiFiClient uses seconds!)
  local.setTimeout(3);
  proxy.setNoDelay(true);
  local.setNoDelay(true);
  _setKeepAlive(proxy);
  // Issue #23: _setKeepAlive() sets SO_SNDTIMEO=8s, which exceeds the old
  // 4-second bpStuck watchdog threshold.  Cap it at 3s (matching the proxy
  // inner-write deadline) so that each blocking write() attempt times out
  // well within the bpStuck window.
  // Issue #27: bpStuck threshold raised to 8s, keeping SO_SNDTIMEO at 3s
  // maintains the invariant: if SO_SNDTIMEO works, write() returns within 3s
  // and the watchdog pet (age) resets — bpStuck never fires falsely. If
  // SO_SNDTIMEO is unreliable (observed on some ESP32 lwIP configurations),
  // the 8s bpStuck guard still catches a genuine hang without killing streams
  // on transient congestion.
#ifdef ESP32
  {
    int _pfd = proxy.fd();
    if (_pfd >= 0) {
      struct timeval _tv = { 3, 0 };
      setsockopt(_pfd, SOL_SOCKET, SO_SNDTIMEO, &_tv, sizeof(_tv));
    }
  }
#endif
  Serial.printf("[Tunnel] Slot %d: Both sockets connected. Spawning proxy task on Core 0.\n", slot);

  // Spawn proxy on Core 0 so Core 1 (tunnel task) stays free to run the Watchdog!
  //
  // Stack sizing (heap-pressure / "FAILED to spawn proxy task" churn):
  // A FreeRTOS task stack must be a single CONTIGUOUS internal-DRAM block, so
  // under fragmentation this xTaskCreate can fail even when ESP.getFreeHeap()
  // still reports 50-60KB free (exactly what the field log showed). The proxy
  // loop keeps its 2KB copy buffer on the heap (malloc), not the stack, and the
  // task's own logged Stack HWM stayed ~1.9KB used across every run. A 12KB
  // stack was therefore ~6x over-provisioned; shrinking it to 6KB roughly halves
  // the contiguous block this spawn needs (still >3x the observed peak usage),
  // which markedly reduces the spawn-failure churn without risking overflow.
  _boreProxyArgs[slot] = {slot};
  BaseType_t taskOk = xTaskCreatePinnedToCore(
    [](void *arg) {
      _BoreProxyArg *a = (_BoreProxyArg *)arg;
      _boreProxyConn(_boreProxy[a->slot], _boreLocal[a->slot], a->slot);
      _boreProxyTaskHandle[a->slot] = nullptr;
      vTaskDelete(nullptr);
    },
    "bore_proxy", 6144, &_boreProxyArgs[slot], 5, &_boreProxyTaskHandle[slot], 0 // Core 0! Stack=6144 bytes
  );
  if (taskOk != pdPASS) {
    Serial.printf("[Tunnel] Slot %d: FAILED to spawn proxy task! Closing sockets. Heap=%u\n", slot, ESP.getFreeHeap());
    proxy.stop();
    local.stop();
    _boreProxyTaskHandle[slot] = nullptr;
    _slotWatchdogArmed[slot] = false;
    _slotLastActive[slot] = 0;
    _slotBusy[slot] = false;
  }
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

// Helper: try to spawn an accept task for a given UUID on any free slot.
// Returns true when the task was successfully enqueued, false otherwise.
static bool _boreAcceptDeferred(const String &uuid) {
  if (ESP.getFreeHeap() < 35000) return false;
  int slot = -1;
  for (int i = 0; i < BORE_MAX_PROXY; i++) {
    if (!_slotBusy[i]) { slot = i; break; }
  }
  if (slot < 0) return false;
  _slotBusy[slot] = true;
  struct _BoreDeferredAcceptArgs { String uuid; int slot; };
  auto *dargs = new (std::nothrow) _BoreDeferredAcceptArgs{uuid, slot};
  if (!dargs) { _slotBusy[slot] = false; return false; }
  BaseType_t ok = xTaskCreatePinnedToCore([](void *p) {
    auto *a = (_BoreDeferredAcceptArgs*)p;
    _boreAccept(a->uuid, a->slot);
    delete a;
    vTaskDelete(nullptr);
  }, "boreProxy", 6144, dargs, 1, nullptr, TUN_TASK_CORE);
  if (ok != pdPASS) {
    Serial.printf("[Tunnel] Slot %d: FAILED to spawn deferred accept (heap=%u). Releasing.\n",
                  slot, ESP.getFreeHeap());
    delete dargs;
    _slotBusy[slot] = false;
    return false;
  }
  return true;
}

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

  // Process any previously buffered Connection (deferred because a slot was
  // streaming).  Now that we're back in the main loop, check whether the slot
  // has been released and accept the request if so.
  if (_boreDeferredUUID.length() > 0) {
    bool anyActive = false;
    for (int i = 0; i < BORE_MAX_PROXY; i++) {
      if (_slotBusy[i] && _slotLastActive[i] != 0 && !_slotBackpressured[i]) {
        anyActive = true; break;
      }
    }
    if (!anyActive) {
      Serial.printf("[Tunnel] Processing deferred UUID=%s\n", _boreDeferredUUID.c_str());
      String deferred = _boreDeferredUUID;
      _boreDeferredUUID = "";
      _boreAcceptDeferred(deferred); // best-effort; ignore failure (bore will retry)
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

    // Low heap guard: defer accepts under 35k free heap to protect WiFi stack.
    // Issue #31: previously used `continue` (silently drops the UUID). Changed to
    // buffer the UUID in _boreDeferredUUID so it is retried on the next
    // _boreServe() call once heap recovers. Without this, a flash-toggle request
    // that arrives while the stream holds heap at 30-35KB was permanently lost —
    // bore.pub issues a UUID, ESP32 drops it, the browser's /flash fetch times out,
    // and the LED never changes state. The deferred path retries up to bore.pub's
    // UUID timeout window (~10s), by which time the stream proxy task has exited
    // and heap has fully recovered to ~70KB.
    if (ESP.getFreeHeap() < 35000) {
      Serial.printf("[Tunnel] Low heap (%u) — deferring accept (will retry).\n",
                    ESP.getFreeHeap());
      _boreDeferredUUID = uuid; // buffer for retry once heap recovers
      continue;
    }

    // Defer speculative 2nd connection while a slot is actively streaming.
    // Instead of dropping the UUID, save it so the next _boreServe() iteration
    // can accept it once the stream slot is released.  This prevents the race
    // where a flash-toggle request arrives just before the proxy task exits —
    // without buffering the Connection message would be silently lost and the
    // /flash fetch would time out.
    bool anyStreaming = false;
    for (int i = 0; i < BORE_MAX_PROXY; i++) {
      if (_slotBusy[i] && _slotLastActive[i] != 0 && !_slotBackpressured[i]) {
        anyStreaming = true;
        break;
      }
    }
    if (anyStreaming) {
      Serial.printf("[Tunnel] Active stream in progress — buffering 2nd accept UUID for retry.\n");
      _boreDeferredUUID = uuid; // overwrite: only the most-recent pending request matters
      continue;
    }

    // Find a free proxy slot
    int slot = -1;
    for (int i = 0; i < BORE_MAX_PROXY; i++) {
      if (!_slotBusy[i]) {
        slot = i; break;
      }
    }
    if (slot < 0) continue; // all slots busy

    _slotBusy[slot] = true; // reserve immediately

    struct _ProxyArgs { String uuid; int slot; };
    auto *args = new (std::nothrow) _ProxyArgs{uuid, slot};
    if (!args) {
      Serial.printf("[Tunnel] Slot %d: new _ProxyArgs failed (OOM heap=%u). Releasing slot.\n",
                    slot, ESP.getFreeHeap());
      _slotBusy[slot] = false;
      continue;
    }
    BaseType_t ok = xTaskCreatePinnedToCore([](void *p) {
      auto *a = (_ProxyArgs*)p;
      _boreAccept(a->uuid, a->slot);
      delete a;
      vTaskDelete(nullptr);
    }, "boreProxy", 6144, args, 1, nullptr, TUN_TASK_CORE);
    if (ok != pdPASS) {
      Serial.printf("[Tunnel] Slot %d: FAILED to spawn accept task (heap=%u). Releasing slot.\n",
                    slot, ESP.getFreeHeap());
      delete args;              // ← was leaked before: real fix
      _slotBusy[slot] = false;
    }
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
  if (!_bore.started) return;

  // Global Watchdog: Check if any proxy task is permanently blocked in WiFiClient::write()
  // This MUST run unconditionally, even if the control socket is disconnected!
  static unsigned long lastWdLog = 0;
  if (millis() - lastWdLog > 10000) {
    lastWdLog = millis();
    // Serial.printf("[Tunnel] Main loop watchdog active.\n"); // Optional heartbeat
  }

  _boreWatchdog("tunnel");

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
    _slotWatchdogArmed[i] = false;
    _slotLastActive[i] = 0;
    _slotBusy[i] = false;
  }
  _bore.ready   = false;
  _bore.started = false;
  _bore.phase   = _PH_IDLE;
  _bore.url     = "(stopped)";
}

#endif
