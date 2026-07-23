#include "cloud_storage.h"
#include "config.h"
#include <ESPSupabase.h>
#include <Preferences.h>
#include <WiFi.h>
#include "esp_camera.h"

Supabase supabase;
Preferences preferences;

static int frame_index = 0;
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
  // Check if row with id=1 exists
  supabase.from("camera_status").select("*").eq("id", "1");
  String response = supabase.doSelect();

  bool exists = false;

  // Parse response
  DynamicJsonDocument doc(1024);
  DeserializationError err = deserializeJson(doc, response);

  if (!err && doc.is<JsonArray>() && doc.size() > 0) {
    exists = true;
  }

  int code;

  if (exists) {
    // Only update the URL
    String updateJson =
      "{\"url\":\"" + url + "\",\"created_at\":\"now()\"}";

    code = supabase
             .from("camera_status")
             .update("camera_status")
             .eq("id", "1")
             .doUpdate(updateJson);
  } else {
    // Create the row
    String insertJson =
      "{\"id\":1,\"url\":\"" + url + "\",\"created_at\":\"now()\"}";

    code = supabase.insert("camera_status", insertJson, false);
  }
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

void uploadFrameToCloud() {
  Serial.printf("[Supabase] Capturing frame for cloud upload... heap: %u\n", ESP.getFreeHeap());
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("[Supabase] ERROR: Camera capture failed for cloud upload");
    return;
  }

  String filename = "events/frame_" + String(frame_index) + ".jpg";
  Serial.printf("[Supabase] Uploading %s (%u bytes)...\n", filename.c_str(), fb->len);

  unsigned long t0 = millis();
  int code = supabase.upload(SUPABASE_BUCKET, filename, "image/jpeg", fb->buf, fb->len);
  unsigned long elapsed = millis() - t0;

  if (code == 200 || code == 201) {
    Serial.printf("[Supabase] Upload OK in %lums. Frame index -> %d\n", elapsed, (frame_index + 1) % STORAGE_FRAME_LIMIT);
    frame_index = (frame_index + 1) % STORAGE_FRAME_LIMIT;
    preferences.putInt("frame_index", frame_index);
  } else {
    Serial.printf("[Supabase] Upload FAILED. HTTP code: %d (took %lums)\n", code, elapsed);
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
