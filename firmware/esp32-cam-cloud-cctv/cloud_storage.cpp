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

void setupCloudStorage() {
  preferences.begin("cctv", false);
  frame_index = preferences.getInt("frame_index", 0);
  supabase.begin(SUPABASE_URL, SUPABASE_ANON_KEY);
  Serial.printf("[Supabase] Configured. URL: %s | Circular frame index: %d\n", SUPABASE_URL, frame_index);
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

  Serial.printf("[Supabase] Capturing frame for cloud upload... heap: %u\n", ESP.getFreeHeap());
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("[Supabase] ERROR: Camera capture failed for cloud upload");
    markCaptureFailureAndMaybeReboot();
    return;
  }
  resetCaptureFailureState();

  String filename = buildFrameName();
  Serial.printf("[Supabase] Uploading %s (%u bytes)...\n", filename.c_str(), fb->len);

  int code = -1;
  unsigned long elapsed = 0;
  bool uploaded = false;
  uint8_t attemptsUsed = 0;
  for (uint8_t attempt = 0; attempt <= SUPABASE_UPLOAD_MAX_RETRIES; attempt++) {
    attemptsUsed = (uint8_t)(attempt + 1);
    unsigned long t0 = millis();
    code = supabase.upload(SUPABASE_BUCKET, filename, "image/jpeg", fb->buf, fb->len);
    elapsed = millis() - t0;
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

void publishTunnelUrl(String url) {
  pendingTunnelUrl = url;
  tunnelPublishFailures = 0;
  nextTunnelPublishAttempt = 0;
  Serial.printf("[Supabase] Queued tunnel URL publish: %s\n", pendingTunnelUrl.c_str());
  loopCloudStorage();
}

void loopCloudStorage() {
  if (!pendingTunnelUrl.length()) return;
  unsigned long now = millis();
  if (nextTunnelPublishAttempt != 0 && now < nextTunnelPublishAttempt) return;

  if (publishTunnelUrlNow(pendingTunnelUrl)) {
    pendingTunnelUrl = "";
    tunnelPublishFailures = 0;
    nextTunnelPublishAttempt = 0;
    return;
  }

  tunnelPublishFailures++;
  unsigned long backoff = min(60000UL, 5000UL * (unsigned long)tunnelPublishFailures);
  nextTunnelPublishAttempt = now + backoff;
  Serial.printf("[Supabase] Tunnel URL publish retry scheduled in %lums (failure #%u)\n",
                backoff, tunnelPublishFailures);
}
