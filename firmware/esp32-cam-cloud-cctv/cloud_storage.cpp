#include "cloud_storage.h"
#include "config.h"
#include <ESPSupabase.h>
#include <Preferences.h>
#include "esp_camera.h"

Supabase supabase;
Preferences preferences;

static int frame_index = 0;

void setupCloudStorage() {
    preferences.begin("cctv", false);
    frame_index = preferences.getInt("frame_index", 0);
    supabase.begin(SUPABASE_URL, SUPABASE_ANON_KEY);
    Serial.println("Supabase configured");
}

void uploadFrameToCloud() {
    camera_fb_t * fb = esp_camera_fb_get();
    if (!fb) {
        Serial.println("Camera capture failed for cloud upload");
        return;
    }

    String filename = "events/frame_" + String(frame_index) + ".jpg";
    Serial.print("Uploading ");
    Serial.print(filename);
    Serial.print(" (Size: ");
    Serial.print(fb->len);
    Serial.println(" bytes)...");

    int code = supabase.upload(SUPABASE_BUCKET, filename, "image/jpeg", fb->buf, fb->len);
    
    if (code == 200) {
        Serial.println("Upload successful!");
        
        // Optionally insert metadata here if motion_events table exists
        /*
        String json = "{\"frame_index\":" + String(frame_index) + ",\"file_path\":\"" + filename + "\"}";
        supabase.insert("motion_events", json, false);
        */

        frame_index = (frame_index + 1) % STORAGE_FRAME_LIMIT;
        preferences.putInt("frame_index", frame_index);
    } else {
        Serial.print("Upload failed, HTTP code: ");
        Serial.println(code);
    }

    esp_camera_fb_return(fb);
}

void publishTunnelUrl(String url) {
    String json = "{\"url\":\"" + url + "\"}";
    int code = supabase.insert("camera_status", json, false);
    if (code == 201 || code == 200 || code == 204) {
        Serial.println("[Supabase] Successfully published tunnel URL!");
    } else {
        Serial.printf("[Supabase] Failed to publish tunnel URL. HTTP code: %d\n", code);
    }
}
