#pragma once

// =============================================================================
//  config.h — DEVICE SECRETS (gitignored, never commit this file)
// =============================================================================
//  Copy config.example.h to config.h and fill in your values.
//  This is the ONLY file you need to edit before flashing.
// =============================================================================

// -----------------------------------------------------------------------------
// CAMERA VIEWER LOGIN (credentials)
// -----------------------------------------------------------------------------
// These credentials protect the camera feed from unauthorised access.
//
// Login is ALWAYS an ENCRYPTED login: the browser fetches the device X25519
// public key from /pubkey, performs ECDH + HKDF-SHA256 to derive an AES-256-GCM
// key, and encrypts your username/password before sending them to /login. The
// ESP32 decrypts with its private key and issues a session token. Credentials
// are never sent in plaintext over the plain-HTTP BORE tunnel.
//
// See docs/SECURITY.md for the full threat model and the important caveat that
// crypto.subtle may be unavailable over plain http:// — the HTTPS SELFHOST
// tunnel is recommended for a fully-encrypted channel.
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
// PUBLIC TUNNEL
// -----------------------------------------------------------------------------
// Default BORE URL:
//   http://bore.pub:<server-assigned-port>/view
// The BORE port is assigned by the public server and can change on reboot.
//
// SECURITY: BORE is plain HTTP (no TLS). For a fully-encrypted channel and to
// make browser crypto.subtle available for the encrypted login, use SELFHOST
// mode, which is served over HTTPS at:
//   https://esp32-tunnel.onrender.com/<CONFIG_SELFHOST_TUNNEL_ID>/view
// -----------------------------------------------------------------------------
#define CONFIG_TUNNEL_MODE_BORE      1
#define CONFIG_TUNNEL_MODE_SELFHOST  2
#define CONFIG_TUNNEL_MODE           CONFIG_TUNNEL_MODE_BORE

#define CONFIG_BORE_SERVER           "bore.pub"
#define CONFIG_BORE_REMOTE_PORT      0
#define CONFIG_SELFHOST_TUNNEL_ID    "00000000-0000-4000-8000-000000000000"

// BORE mode: public URL is http://bore.pub:<server-assigned-port>/view.
// The esp32-tunnel BORE API currently has no fixed remote-port parameter.
// SELFHOST mode: public URL is stable at:
// http://esp32-tunnel.onrender.com/<CONFIG_SELFHOST_TUNNEL_ID>/view

// -----------------------------------------------------------------------------
// SUPABASE — Cloud Motion Storage
// -----------------------------------------------------------------------------
// Required for cloud frame uploads.
// Get these values from: Supabase Dashboard → Project Settings → API
//
//   SUPABASE_URL      = "https://<your-project-id>.supabase.co"
//   SUPABASE_ANON_KEY = the "anon public" key (safe to use in firmware)
//
// DO NOT use the "service_role" key — it has admin access to your database.
//
// Upload behavior:
//   - No client connected: firmware uploads 1 frame every 3s (idle uploader).
//   - Browser client connected: firmware stops; the browser uploads received
//     frames best-effort, dropping frames while an upload is in flight AND
//     honoring STORAGE_UPLOAD_MIN_GAP_MS between completed uploads so a fast
//     client cannot hammer Supabase.
// -----------------------------------------------------------------------------
#define SUPABASE_URL         "https://xxxx.supabase.co"
#define SUPABASE_ANON_KEY    "your-anon-key"
#define SUPABASE_BUCKET      "cctv-clips"
#define STORAGE_FRAME_LIMIT  200   // circular buffer: overwrites oldest frame

// Minimum spacing (ms) between browser best-effort uploads. Lower = more frames
// uploaded (more Supabase writes/egress); higher = gentler. 0 disables the floor
// (still bounded by the in-flight drop guard).
#define STORAGE_UPLOAD_MIN_GAP_MS 250

// -----------------------------------------------------------------------------
// PIR MOTION SENSOR
// -----------------------------------------------------------------------------
// PIR is DISABLED by default (not currently of interest). When disabled, the
// firmware idle uploader (1 frame / 3s when no client is connected) is the sole
// firmware-side upload path. Set ENABLE_PIR_MOTION to 1 to re-enable PIR bursts.
//
// PIR_GPIO: HC-SR501 OUT pin → GPIO 13 on AI-Thinker ESP32-CAM
// PIR_DEBOUNCE_MS: ignore re-triggers for this many milliseconds after motion
// MOTION_BURST_COUNT: number of frames to capture per motion event
// MOTION_BURST_INTERVAL_MS: delay between each burst frame
// -----------------------------------------------------------------------------
#define ENABLE_PIR_MOTION        0
#define PIR_GPIO                 13
#define PIR_DEBOUNCE_MS          10000
#define MOTION_BURST_COUNT       5
#define MOTION_BURST_INTERVAL_MS 500

// -----------------------------------------------------------------------------
// WIFI AP NAME (captive portal hotspot name shown on your phone)
// -----------------------------------------------------------------------------
#define WIFI_AP_NAME         "ESP32-CAM-Setup"
