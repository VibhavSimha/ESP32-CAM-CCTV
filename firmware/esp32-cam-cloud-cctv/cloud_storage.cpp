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
    return;
  }

  String filename = buildFrameName();
  Serial.printf("[Supabase] Uploading %s (%u bytes)...\n", filename.c_str(), fb->len);

  unsigned long t0 = millis();
  int code = supabase.upload(SUPABASE_BUCKET, filename, "image/jpeg", fb->buf, fb->len);
  unsigned long elapsed = millis() - t0;

  // ALWAYS rotate so a single failure (or an unexpected 409) can never pin the
  // uploader on one name. The unique timestamp already prevents collisions; the
  // rotating index is kept for compatibility/telemetry.
  upload_seq++;
  frame_index = (frame_index + 1) % STORAGE_FRAME_LIMIT;
  preferences.putInt("frame_index", frame_index);

  if (code == 200 || code == 201) {
    Serial.printf("[Supabase] Upload OK in %lums. seq=%lu index=%d\n",
                  elapsed, (unsigned long)upload_seq, frame_index);
  } else if (code == 409) {
    // Should no longer happen with unique names, but treat as benign if it does.
    Serial.printf("[Supabase] Upload skipped: resource already exists (409) in %lums. seq=%lu\n",
                  elapsed, (unsigned long)upload_seq);
  } else {
    Serial.printf("[Supabase] Upload FAILED. HTTP code: %d (took %lums). seq=%lu\n",
                  code, elapsed, (unsigned long)upload_seq);
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
