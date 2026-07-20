#pragma once

// Copy this file to config.h and fill in your values.
// config.h is gitignored — never commit secrets.

// HTTP Basic Auth (required for /stream and /capture)
#define CONFIG_HTTP_USER     "admin"
#define CONFIG_HTTP_PASS     "change-me-to-strong-password"

// localtunnel subdomain (unique, non-guessable)
#define CONFIG_TUNNEL_SUBDOMAIN "cctv-your-unique-id"

// Supabase (https://supabase.com — free tier)
#define SUPABASE_URL         "https://xxxx.supabase.co"
#define SUPABASE_ANON_KEY    "your-anon-key"
#define SUPABASE_BUCKET      "cctv-clips"
#define STORAGE_FRAME_LIMIT  200

// PIR motion capture
#define ENABLE_PIR_MOTION    1
#define PIR_GPIO             13
#define PIR_DEBOUNCE_MS      10000
#define MOTION_BURST_COUNT   5
#define MOTION_BURST_INTERVAL_MS 500

// WiFiManager AP name when provisioning
#define WIFI_AP_NAME         "ESP32-CAM-Setup"
