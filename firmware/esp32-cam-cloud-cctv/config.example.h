#pragma once

// =============================================================================
//  config.h — DEVICE SECRETS (gitignored, never commit this file)
// =============================================================================
//  Copy config.example.h to config.h and fill in your values.
//  This is the ONLY file you need to edit before flashing.
// =============================================================================

// -----------------------------------------------------------------------------
// CAMERA VIEWER LOGIN (HTTP Basic Auth)
// -----------------------------------------------------------------------------
// These credentials protect the camera feed from unauthorised access.
// You will be prompted for these when you open the /view URL in your browser.
//
//   Username: CONFIG_HTTP_USER
//   Password: CONFIG_HTTP_PASS
//
// Use a strong, unique password (16+ chars). Never use "changeme".
// -----------------------------------------------------------------------------
#define CONFIG_HTTP_USER     "admin"
#define CONFIG_HTTP_PASS     "changeme12345678"

// -----------------------------------------------------------------------------
// WIFI CREDENTIALS
// -----------------------------------------------------------------------------
// You do NOT enter WiFi credentials here.
// WiFi is configured via a captive portal on first boot:
//
//   1. Power on the ESP32-CAM (no WiFi configured yet).
//   2. A hotspot named "ESP32-CAM-Setup" will appear on your phone/laptop.
//   3. Connect to it — a setup webpage will pop up automatically.
//   4. Select your home WiFi network and enter its password.
//   5. The board saves the credentials to internal flash (NVS) and reboots.
//   6. From now on it connects automatically — no cables or re-flashing needed.
//
// To reset WiFi (e.g. moving to a new network):
//   Press the board's RESET button while holding GPIO 0 LOW for 3 seconds.
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// LOCALTUNNEL SUBDOMAIN (only used if switching back to LOCALTUNNEL provider)
// -----------------------------------------------------------------------------
// If you switch from BORE back to LOCALTUNNEL, this becomes your public URL:
//   https://<CONFIG_TUNNEL_SUBDOMAIN>.loca.lt/view
// Pick a random, non-guessable name — the URL is semi-public.
// Currently unused because LOCALTUNNEL requires TLS which exceeds ESP32 RAM.
// -----------------------------------------------------------------------------
#define CONFIG_TUNNEL_SUBDOMAIN "cctv-dev-local"

// -----------------------------------------------------------------------------
// SUPABASE — Cloud Motion Storage
// -----------------------------------------------------------------------------
// Required for PIR motion capture uploads.
// Get these values from: Supabase Dashboard → Project Settings → API
//
//   SUPABASE_URL      = "https://<your-project-id>.supabase.co"
//   SUPABASE_ANON_KEY = the "anon public" key (safe to use in firmware)
//
// DO NOT use the "service_role" key — it has admin access to your database.
// -----------------------------------------------------------------------------
#define SUPABASE_URL         "https://xxxx.supabase.co"
#define SUPABASE_ANON_KEY    "your-anon-key"
#define SUPABASE_BUCKET      "cctv-clips"
#define STORAGE_FRAME_LIMIT  200   // circular buffer: overwrites oldest frame

// -----------------------------------------------------------------------------
// PIR MOTION SENSOR
// -----------------------------------------------------------------------------
// Set ENABLE_PIR_MOTION to 0 if you have no PIR wired up.
// PIR_GPIO: HC-SR501 OUT pin → GPIO 13 on AI-Thinker ESP32-CAM
// PIR_DEBOUNCE_MS: ignore re-triggers for this many milliseconds after motion
// MOTION_BURST_COUNT: number of frames to capture per motion event
// MOTION_BURST_INTERVAL_MS: delay between each burst frame
// -----------------------------------------------------------------------------
#define ENABLE_PIR_MOTION        1
#define PIR_GPIO                 13
#define PIR_DEBOUNCE_MS          10000
#define MOTION_BURST_COUNT       5
#define MOTION_BURST_INTERVAL_MS 500

// -----------------------------------------------------------------------------
// WIFI AP NAME (captive portal hotspot name shown on your phone)
// -----------------------------------------------------------------------------
#define WIFI_AP_NAME         "ESP32-CAM-Setup"
