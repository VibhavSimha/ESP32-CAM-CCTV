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
    String json = "{\"id\":1,\"url\":\"" + url + "\",\"updated_at\":\"now()\"}";
    unsigned long t0 = millis();
    int code = supabase.upsert("camera_status", json, false);
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
    camera_fb_t * fb = esp_camera_fb_get();
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
