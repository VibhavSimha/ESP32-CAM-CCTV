#include "cloud_storage.h"
#include "config.h"
#include <ESPSupabase.h>
#include <Preferences.h>
#include <WiFi.h>
#include <time.h>
#include "esp_camera.h"

Supabase supabase;
Preferences preferences;

static int frame_index = 0;
static uint32_t upload_seq = 0;   // monotonic counter to disambiguate same-ms uploads
static String pendingTunnelUrl;
static unsigned long nextTunnelPublishAttempt = 0;
static uint8_t tunnelPublishFailures = 0;
static uint16_t captureFailStreak = 0;
static unsigned long captureFailSince = 0;
static uint16_t uploadFailStreak = 0;
static unsigned long uploadFailSince = 0;

#ifndef SUPABASE_UPLOAD_MAX_RETRIES
#define SUPABASE_UPLOAD_MAX_RETRIES 2
#endif

#ifndef SUPABASE_UPLOAD_RETRY_DELAY_MS
#define SUPABASE_UPLOAD_RETRY_DELAY_MS 400UL
#endif

#ifndef SUPABASE_UPLOAD_FAIL_REBOOT_AFTER_COUNT
#define SUPABASE_UPLOAD_FAIL_REBOOT_AFTER_COUNT 120
#endif

#ifndef SUPABASE_UPLOAD_FAIL_REBOOT_AFTER_MS
#define SUPABASE_UPLOAD_FAIL_REBOOT_AFTER_MS (15UL * 60UL * 1000UL)
#endif

#ifndef SUPABASE_CAPTURE_FAIL_REBOOT_AFTER_COUNT
#define SUPABASE_CAPTURE_FAIL_REBOOT_AFTER_COUNT 200
#endif

#ifndef SUPABASE_CAPTURE_FAIL_REBOOT_AFTER_MS
#define SUPABASE_CAPTURE_FAIL_REBOOT_AFTER_MS (15UL * 60UL * 1000UL)
#endif

// ---------------------------------------------------------------------------
// MARK: Background network task (issue #81)
//
// ESPSupabase's upload()/insert() do a hand-rolled, UNBOUNDED response read:
//   while (client.connected()) { ... line = client.readStringUntil('\n') ... }
// (see jhagas/ESPSupabase src/Supabase.cpp). If the TCP connection goes
// silently dead -- common with flaky tunnels/NAT timeouts -- while
// WiFiClientSecure::connected() keeps reporting true, readStringUntil() times
// out on every call but the outer loop never exits: it hangs FOREVER. Both
// calls used to run directly on the Arduino loop() task, so one wedged
// upload/publish froze EVERYTHING loop() drives -- WiFi reconnection,
// captive-portal recovery, tunnel start/stop -- a true non-recoverable state
// needing a manual power cycle.
//
// Fix: run all Supabase network I/O on a dedicated background task (mirrors
// the always-on task pattern already proven in src/esp32tunnel.h's
// _tunTaskFn). loop() now only ever hands off a job and returns immediately;
// it can never block inside Supabase code again. upload() and insert() share
// ONE non-thread-safe Supabase instance, so jobs are processed one at a time
// through a single-slot mailbox -- safe because there is exactly one
// producer (the Arduino loop() task, via uploadFrameToCloud()/
// loopCloudStorage()) and one consumer (this task), the same
// single-producer/single-consumer flag handoff already used for the
// _slotBusy[] flags in esp32tunnel_bore.h.
//
// loopCloudStorage() -- still polled every tick from the now never-blocked
// loop() -- doubles as the wedge-watchdog: if the background task has been
// busy longer than the EXISTING SUPABASE_UPLOAD_FAIL_REBOOT_AFTER_MS budget,
// it reboots via ESP.restart(). That is what guarantees recovery even when
// the background task itself is the thing that is permanently stuck.
// ---------------------------------------------------------------------------

enum CloudJobType { CLOUD_JOB_NONE = 0, CLOUD_JOB_UPLOAD_FRAME, CLOUD_JOB_PUBLISH_URL };

#ifndef CLOUD_STORAGE_TASK_STACK
// WiFiClientSecure/HTTPClient (used internally by ESPSupabase) already run
// safely from this project's Arduino loop() task, whose default stack is
// 8192 bytes (see the TLS fetches in captive_portal.cpp) -- reuse that
// proven figure for this task.
#define CLOUD_STORAGE_TASK_STACK 8192
#endif
#ifndef CLOUD_STORAGE_TASK_PRIO
#define CLOUD_STORAGE_TASK_PRIO 1
#endif
#ifndef CLOUD_STORAGE_TASK_CORE
// MUST stay 1 -- do not move this back to core 0.
//
// Core 1 is where the Arduino loop() task, the tunnel control task
// (_tunTaskFn / TUN_TASK_CORE in src/esp32tunnel.h) and the bore "accept"
// tasks (same TUN_TASK_CORE in src/esp32tunnel_bore.h) already run every
// long/blocking network call this project makes. That is not an accident:
// ESP-IDF's Task Watchdog Timer only monitors the core 0 idle task (IDLE0)
// by default -- core 1's idle task (IDLE1) is NOT watchdog-monitored, which
// is exactly why none of that existing code has to worry about an
// occasional slow socket call panicking the whole MCU.
//
// This task used to be pinned to core 0, on the mistaken belief that "the
// bore proxy tasks" ran there too -- only the data-forwarding connection
// (_boreProxyConn, spawned with a literal `0` in esp32tunnel_bore.h) does;
// every long-lived task, including the ones above, is on core 1.
// _boreProxyConn() spends most of its time blocked on socket I/O, but a
// sustained transfer (e.g. a long live /stream view) could in principle keep
// it continuously busy -- so it now also carries its own explicit, bounded
// forced-yield guard (BORE_PROXY_FORCE_YIELD_MS in esp32tunnel_bore.h) rather
// than relying on that alone. Pinning this (cloud_storage) task to core 0 put
// the exact hang the header comment above warns about (ESPSupabase's
// hand-rolled, not-guaranteed-to-yield response read loop) on the one core
// whose idle task IS watchdog-monitored: instead of being caught by the
// graceful SUPABASE_UPLOAD_FAIL_REBOOT_AFTER_MS wedge-watchdog below, a
// merely slow (but otherwise healthy) multi-second upload starved IDLE0 and
// made esp_task_wdt hard-panic the MCU well before that budget was ever
// reached -- the "task_wdt ... CPU 0: cloud_storage / Aborting" crash loop
// seen in the field (issue #81 regression, see issue #81 comment timestamped
// 2026-10-06). Running Supabase's socket/DNS I/O in true cross-core
// parallel with this project's own Wi-Fi reconnect handling also raced with
// the network stack during a Wi-Fi relink and triggered a separate
// "assert failed: udp_remove ... Required to lock TCPIP core
// functionality!" crash in that same log. Keeping this task on core 1 fixes
// both.
#define CLOUD_STORAGE_TASK_CORE 1
#endif

static TaskHandle_t cloudTaskHandle = nullptr;

// Single-slot job mailbox. cloudJobType/cloudJobFb/cloudJobFilename/
// cloudJobUrl are written by loop() (producer) BEFORE raising
// cloudJobPending, and are only read by the background task (consumer)
// after it observes cloudJobPending==true; the consumer clears
// cloudJobPending only once it is fully done, so the producer never
// touches them again until then. Plain volatiles (no mutex) are sufficient
// for this single-producer/single-consumer handoff, consistent with the
// rest of this codebase's cross-task signalling style.
static volatile CloudJobType cloudJobType = CLOUD_JOB_NONE;
static volatile bool cloudJobPending = false;    // slot occupied: queued and/or running
static volatile bool cloudJobBusy = false;       // consumer is actively inside the Supabase call
static volatile unsigned long cloudJobBusySince = 0;
static camera_fb_t *cloudJobFb = nullptr;        // valid for CLOUD_JOB_UPLOAD_FRAME
static String cloudJobFilename;                  // valid for CLOUD_JOB_UPLOAD_FRAME
static String cloudJobUrl;                       // valid for CLOUD_JOB_PUBLISH_URL
static volatile bool cloudJobPublishSucceeded = false;  // result of the last PUBLISH_URL job

static void markCaptureFailureAndMaybeReboot() {
  captureFailStreak++;
  if (captureFailSince == 0) captureFailSince = millis();
  unsigned long downFor = millis() - captureFailSince;
  Serial.printf("[Supabase] Capture failure streak=%u (for %lums)\n",
                (unsigned)captureFailStreak, downFor);
  if (captureFailStreak >= SUPABASE_CAPTURE_FAIL_REBOOT_AFTER_COUNT &&
      downFor >= SUPABASE_CAPTURE_FAIL_REBOOT_AFTER_MS) {
    Serial.printf("[Supabase] Camera capture failed for %lums (%u times). Rebooting for recovery.\n",
                  downFor, (unsigned)captureFailStreak);
    delay(1000);
    ESP.restart();
  }
}

static void resetCaptureFailureState() {
  if (captureFailStreak > 0) {
    Serial.printf("[Supabase] Camera capture recovered after %u failure(s).\n",
                  (unsigned)captureFailStreak);
  }
  captureFailStreak = 0;
  captureFailSince = 0;
}

static void markUploadFailureAndMaybeReboot(int code) {
  uploadFailStreak++;
  if (uploadFailSince == 0) uploadFailSince = millis();
  unsigned long downFor = millis() - uploadFailSince;
  Serial.printf("[Supabase] Upload failure streak=%u (HTTP %d, for %lums)\n",
                (unsigned)uploadFailStreak, code, downFor);
  if (uploadFailStreak >= SUPABASE_UPLOAD_FAIL_REBOOT_AFTER_COUNT &&
      downFor >= SUPABASE_UPLOAD_FAIL_REBOOT_AFTER_MS) {
    Serial.printf("[Supabase] Upload path unhealthy for %lums (%u failures). Rebooting for recovery.\n",
                  downFor, (unsigned)uploadFailStreak);
    delay(1000);
    ESP.restart();
  }
}

static void resetUploadFailureState() {
  if (uploadFailStreak > 0) {
    Serial.printf("[Supabase] Upload path recovered after %u failure(s).\n",
                  (unsigned)uploadFailStreak);
  }
  uploadFailStreak = 0;
  uploadFailSince = 0;
}

static bool publishTunnelUrlNow(const String &url) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("[Supabase] Tunnel URL publish deferred: WiFi status=%d ip=%s\n",
                  WiFi.status(), WiFi.localIP().toString().c_str());
    return false;
  }

  Serial.printf("[Supabase] Publishing tunnel URL: %s | heap: %u\n", url.c_str(), ESP.getFreeHeap());
  unsigned long t0 = millis();

  String insertJson = "{\"url\":\"" + url + "\"}";
  int code = supabase.insert("camera_status", insertJson, false);
  unsigned long elapsed = millis() - t0;
  if (code == 201 || code == 200 || code == 204) {
    Serial.printf("[Supabase] Tunnel URL published OK (HTTP %d) in %lums\n", code, elapsed);
    return true;
  }

  Serial.printf("[Supabase] ERROR: Failed to publish tunnel URL. HTTP code: %d (took %lums)\n", code, elapsed);
  return false;
}

// Runs on the background task: the actual (potentially slow/hangable)
// supabase.upload() call plus all of the pre-existing retry/bookkeeping
// logic. Always consumes (returns) fb exactly once.
static void performFrameUploadJob(camera_fb_t *fb, const String &filename) {
  Serial.printf("[Supabase] Uploading %s (%u bytes)...\n", filename.c_str(), fb->len);

  int code = -1;
  unsigned long elapsed = 0;
  bool uploaded = false;
  uint8_t attemptsUsed = 0;
  for (uint8_t attempt = 0; attempt <= SUPABASE_UPLOAD_MAX_RETRIES; attempt++) {
    attemptsUsed = (uint8_t)(attempt + 1);

    // Issue #81 regression guard: don't open a brand-new Supabase socket
    // while Wi-Fi itself is down/mid-reconnect. supabase.upload() has no
    // such check (unlike publishTunnelUrlNow() above), so a retry landing
    // exactly in a Wi-Fi link flap would start a fresh DNS lookup/TLS
    // connect while the network interface is being torn down and rebuilt
    // underneath it -- observed in the field as an lwIP "assert failed:
    // udp_remove ... Required to lock TCPIP core functionality!" abort.
    // Skip straight to the normal retry/backoff path instead of touching
    // the socket layer.
    if (WiFi.status() != WL_CONNECTED) {
      code = -1;
      elapsed = 0;
      Serial.printf("[Supabase] Upload attempt %u/%u skipped: WiFi not connected (status=%d)\n",
                    (unsigned)attemptsUsed,
                    (unsigned)(SUPABASE_UPLOAD_MAX_RETRIES + 1),
                    WiFi.status());
    } else {
      unsigned long t0 = millis();
      code = supabase.upload(SUPABASE_BUCKET, filename, "image/jpeg", fb->buf, fb->len);
      elapsed = millis() - t0;
    }
    if (code == 200 || code == 201 || code == 409) {
      uploaded = true;
      break;
    }
    if (attempt < SUPABASE_UPLOAD_MAX_RETRIES) {
      Serial.printf("[Supabase] Upload attempt %u/%u failed (HTTP %d). Retrying in %lums\n",
                    (unsigned)attemptsUsed,
                    (unsigned)(SUPABASE_UPLOAD_MAX_RETRIES + 1),
                    code,
                    (unsigned long)SUPABASE_UPLOAD_RETRY_DELAY_MS);
      delay(SUPABASE_UPLOAD_RETRY_DELAY_MS);
    }
  }

  // ALWAYS rotate so a single failure (or an unexpected 409) can never pin the
  // uploader on one name. The unique timestamp already prevents collisions; the
  // rotating index is kept for compatibility/telemetry.
  upload_seq++;
  frame_index = (frame_index + 1) % STORAGE_FRAME_LIMIT;
  preferences.putInt("frame_index", frame_index);

  if (uploaded && (code == 200 || code == 201)) {
    resetUploadFailureState();
    Serial.printf("[Supabase] Upload OK in %lums (attempt %u). seq=%lu index=%d\n",
                  elapsed, (unsigned)attemptsUsed, (unsigned long)upload_seq, frame_index);
  } else if (uploaded && code == 409) {
    resetUploadFailureState();
    // Should no longer happen with unique names, but treat as benign if it does.
    Serial.printf("[Supabase] Upload skipped: resource already exists (409) in %lums (attempt %u). seq=%lu\n",
                  elapsed, (unsigned)attemptsUsed, (unsigned long)upload_seq);
  } else {
    Serial.printf("[Supabase] Upload FAILED after %u attempt(s). HTTP code: %d (last took %lums). "
                  "Dropping frame and continuing with newer frames. seq=%lu\n",
                  (unsigned)attemptsUsed, code, elapsed, (unsigned long)upload_seq);
    markUploadFailureAndMaybeReboot(code);
  }

  esp_camera_fb_return(fb);
}

// Runs on the background task: the actual supabase.insert() call for a
// tunnel-URL publish. Result is picked up by loopCloudStorage() once the job
// finishes (cloudJobPending clears).
static void performTunnelPublishJob(const String &url) {
  cloudJobPublishSucceeded = publishTunnelUrlNow(url);
}

static void cloudStorageTaskFn(void *) {
  for (;;) {
    if (cloudJobPending) {
      cloudJobBusy = true;
      cloudJobBusySince = millis();

      if (cloudJobType == CLOUD_JOB_UPLOAD_FRAME) {
        camera_fb_t *fb = cloudJobFb;
        String filename = cloudJobFilename;
        cloudJobFb = nullptr;
        performFrameUploadJob(fb, filename);
      } else if (cloudJobType == CLOUD_JOB_PUBLISH_URL) {
        performTunnelPublishJob(cloudJobUrl);
      }

      cloudJobBusy = false;
      cloudJobBusySince = 0;
      cloudJobType = CLOUD_JOB_NONE;
      cloudJobPending = false;  // release the mailbox LAST: done, slot free again
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// Idempotent: safe to call every tick. Retries if an earlier attempt failed
// (e.g. transient heap fragmentation at boot), so a one-off spawn failure can
// never permanently disable cloud uploads (issue #81: never non-recoverable).
static void startCloudStorageTaskIfNeeded() {
  if (cloudTaskHandle) return;
  BaseType_t ok = xTaskCreatePinnedToCore(cloudStorageTaskFn, "cloud_storage",
                                          CLOUD_STORAGE_TASK_STACK, nullptr,
                                          CLOUD_STORAGE_TASK_PRIO, &cloudTaskHandle,
                                          CLOUD_STORAGE_TASK_CORE);
  if (ok != pdPASS) {
    cloudTaskHandle = nullptr;
    Serial.printf("[Supabase] FAILED to spawn background cloud task (heap=%u). Will keep retrying.\n",
                  ESP.getFreeHeap());
  }
}

void setupCloudStorage() {
  preferences.begin("cctv", false);
  frame_index = preferences.getInt("frame_index", 0);
  supabase.begin(SUPABASE_URL, SUPABASE_ANON_KEY);
  Serial.printf("[Supabase] Configured. URL: %s | Circular frame index: %d\n", SUPABASE_URL, frame_index);
  startCloudStorageTaskIfNeeded();
}

// Issue #11: previously every upload used the fixed name events/frame_<index>.jpg,
// and index only advanced on HTTP 2xx. supabase.upload() is a create-only POST
// (no upsert), so once events/frame_1.jpg existed the server returned
// 409 Duplicate -> the code saw a non-2xx, never advanced the index, and retried
// the SAME name forever. Fix: build a UNIQUE, timestamped name per upload so a
// name can never collide, and ALWAYS rotate the index/seq so a single failure
// can't wedge the loop.
static String buildFrameName() {
  // Prefer wall-clock time if NTP/SNTP has synced; otherwise fall back to uptime.
  time_t now = time(nullptr);
  char stamp[32];
  if (now > 1700000000) {  // plausibly a real epoch (past ~2023-11)
    struct tm tmv;
    gmtime_r(&now, &tmv);
    // events/2026-07-25T08-57-41Z_<seq>.jpg  (colons avoided for object keys)
    strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H-%M-%SZ", &tmv);
  } else {
    // No clock yet: use uptime milliseconds, still unique with the seq counter.
    snprintf(stamp, sizeof(stamp), "up%lu", (unsigned long)millis());
  }
  String name = "events/frame_" + String(stamp) + "_" + String(upload_seq) + ".jpg";
  return name;
}

void uploadFrameToCloud() {
  // Issue #81: keep the background task alive even if an earlier spawn
  // attempt failed (e.g. transient heap fragmentation at boot).
  startCloudStorageTaskIfNeeded();

  // Issue #27: defence-in-depth heap guard. The TLS handshake + HTTP body for
  // an upload allocates ~20 KB of transient heap. If the bore tunnel proxy is
  // also active (holding socket buffers + a 2 KB copy buffer) the combined
  // pressure can collapse free heap to ~34 KB, triggering crypto login
  // rejections and tunnel write stalls. The primary guard is in the main-loop
  // caller (via isTunnelSlotBusy + MIN_HEAP_FOR_UPLOAD), but this inner check
  // protects callers that bypass the main-loop guard.
  if (ESP.getFreeHeap() < MIN_HEAP_FOR_UPLOAD) {
    Serial.printf("[Supabase] Upload deferred: low heap (%u < %u)\n",
                  ESP.getFreeHeap(), MIN_HEAP_FOR_UPLOAD);
    return;
  }

  // Issue #81: the background task serializes every Supabase call through
  // one non-thread-safe instance. If the slot is occupied -- including by a
  // prior upload stuck inside ESPSupabase's unbounded read loop -- drop this
  // frame instead of queuing/blocking; loopCloudStorage()'s watchdog reboots
  // if the task is ever truly wedged.
  if (!cloudTaskHandle || cloudJobPending) {
    Serial.println("[Supabase] Upload skipped: background cloud task busy or unavailable");
    return;
  }

  Serial.printf("[Supabase] Capturing frame for cloud upload... heap: %u\n", ESP.getFreeHeap());
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("[Supabase] ERROR: Camera capture failed for cloud upload");
    markCaptureFailureAndMaybeReboot();
    return;
  }
  resetCaptureFailureState();

  // Hand off to the background task and return immediately -- loop() must
  // never block inside the (potentially hanging) network call.
  cloudJobFb = fb;
  cloudJobFilename = buildFrameName();
  cloudJobType = CLOUD_JOB_UPLOAD_FRAME;
  cloudJobPending = true;
}

void publishTunnelUrl(String url) {
  pendingTunnelUrl = url;
  tunnelPublishFailures = 0;
  nextTunnelPublishAttempt = 0;
  Serial.printf("[Supabase] Queued tunnel URL publish: %s\n", pendingTunnelUrl.c_str());
  loopCloudStorage();
}

void loopCloudStorage() {
  // Issue #81: keep the background task alive even if an earlier spawn
  // attempt failed. Cheap no-op once the task is running.
  startCloudStorageTaskIfNeeded();

  // Wedge-watchdog: loop() itself can no longer block inside a Supabase call,
  // but the background task still could, if ESPSupabase ever hangs. Reboot
  // once a job has been busy past the existing upload-fail budget -- the
  // only way to recover a truly wedged network call -- instead of waiting
  // forever.
  if (cloudJobBusy) {
    unsigned long busyFor = millis() - cloudJobBusySince;
    if (busyFor >= SUPABASE_UPLOAD_FAIL_REBOOT_AFTER_MS) {
      Serial.printf("[Supabase] Background task wedged for %lums (job type %d). Rebooting for recovery.\n",
                    busyFor, (int)cloudJobType);
      delay(1000);
      ESP.restart();
    }
  }

  static bool awaitingPublishResult = false;
  static String awaitingPublishUrl;

  if (awaitingPublishResult) {
    if (cloudJobPending) return;  // previously-submitted publish job still running
    awaitingPublishResult = false;

    if (awaitingPublishUrl == pendingTunnelUrl) {
      if (cloudJobPublishSucceeded) {
        pendingTunnelUrl = "";
        tunnelPublishFailures = 0;
        nextTunnelPublishAttempt = 0;
        return;
      }
      tunnelPublishFailures++;
      unsigned long backoff = min(60000UL, 5000UL * (unsigned long)tunnelPublishFailures);
      nextTunnelPublishAttempt = millis() + backoff;
      Serial.printf("[Supabase] Tunnel URL publish retry scheduled in %lums (failure #%u)\n",
                    backoff, tunnelPublishFailures);
      return;
    }
    // pendingTunnelUrl changed while that job was in flight (superseded by a
    // newer publishTunnelUrl() call) -- fall through to submit it below.
  }

  if (!pendingTunnelUrl.length()) return;
  unsigned long now = millis();
  if (nextTunnelPublishAttempt != 0 && now < nextTunnelPublishAttempt) return;
  if (!cloudTaskHandle || cloudJobPending) return;  // slot busy/unavailable; retry next tick

  awaitingPublishUrl = pendingTunnelUrl;
  cloudJobUrl = pendingTunnelUrl;
  cloudJobType = CLOUD_JOB_PUBLISH_URL;
  cloudJobPending = true;
  awaitingPublishResult = true;
}
