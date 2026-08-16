#pragma once

// =============================================================================
//  config.h — DEVICE SECRETS (gitignored, never commit this file)
// =============================================================================
//  Copy config.example.h to config.h and fill in your values.
//  This is the ONLY file you need to edit before flashing.
//
//  See docs/CONFIG_SETUP.md for a step-by-step first-flash walkthrough,
//  including the ONE-TIME device-pubkey pinning step (CONFIG_DEVICE_PUBKEY_B64).
// =============================================================================

// -----------------------------------------------------------------------------
// CAMERA VIEWER LOGIN (credentials)
// -----------------------------------------------------------------------------
// These credentials protect the camera feed from unauthorised access.
//
// Login is ALWAYS an ENCRYPTED login. The browser derives an AES-256-GCM key via
// X25519 ECDH + HKDF-SHA256 with the device key, and encrypts your
// username/password (plus a single-use /nonce) before sending them to /login.
// The ESP32 decrypts with its private key and issues a session token.
// Credentials are never sent in plaintext over the plain-HTTP BORE tunnel.
//
// The login crypto is bundled (pure JS) in the /view page, so it works over
// plain http:// as well — it does NOT depend on window.crypto.subtle.
//
// See docs/SECURITY.md for the full threat model, and docs/CONFIG_SETUP.md for
// the one-time device-key pinning that hardens the login against a
// pubkey-substitution MITM.
//
//   Username: CONFIG_HTTP_USER
//   Password: CONFIG_HTTP_PASS
//
// Use a strong, unique password (16+ chars). Never use "changeme".
// -----------------------------------------------------------------------------
#define CONFIG_HTTP_USER     "admin"
#define CONFIG_HTTP_PASS     "changeme12345678"

// -----------------------------------------------------------------------------
// DEVICE X25519 PUBLIC KEY — PINNED (one-time setup, see docs/CONFIG_SETUP.md)
// -----------------------------------------------------------------------------
// The device X25519 keypair is generated on first boot and PERSISTED in NVS, so
// its public key is STABLE across reboots. Pinning that public key here lets the
// browser encrypt the login DIRECTLY to the trusted device key instead of a key
// fetched from /pubkey over the untrusted plain-HTTP tunnel — defeating an
// attacker who would otherwise substitute their own key (MITM).
//
// ONE-TIME SETUP (per device — you only do this once):
//   1. Leave this EMPTY ("") for the very first flash.
//   2. Flash + open the Serial Monitor. Copy the value printed at boot:
//        [Crypto] Device pubkey (b64): <VALUE>
//      (The /view page also shows a ⚠ banner with this value while unpinned.)
//   3. Paste <VALUE> below and re-flash ONCE:
//        #define CONFIG_DEVICE_PUBKEY_B64 "<VALUE>"
//   4. Done — permanent. The key only changes if you erase NVS / replace the board.
//
// While EMPTY: login still works (the page fetches /pubkey), but a prominent
// warning banner is shown because the login is NOT MITM-protected.
// When SET: the browser uses ONLY this pinned key; if it does not match the
// device, login fails (the intended MITM-abort). The firmware logs at boot
// whether this value matches the live device key.
// -----------------------------------------------------------------------------
#define CONFIG_DEVICE_PUBKEY_B64 ""

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
// SECURITY: BORE is plain HTTP (no TLS). The bundled JS login crypto works over
// plain HTTP, and pinning CONFIG_DEVICE_PUBKEY_B64 hardens it against key
// substitution. For FULL-channel confidentiality (including the MJPEG stream)
// and server authentication, front BORE with an HTTPS reverse proxy, or use
// SELFHOST/HTTPS where the hardware permits. See docs/SECURITY.md.
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
//
// Frames use unique timestamped names (issue #11); cap the bucket with the
// server-side pg_cron FIFO job in docs/SUPABASE_RETENTION.md.
// -----------------------------------------------------------------------------
#define SUPABASE_URL         "https://xxxx.supabase.co"
#define SUPABASE_ANON_KEY    "your-anon-key"
#define SUPABASE_BUCKET      "cctv-clips"
#define STORAGE_FRAME_LIMIT  200   // retention target (see docs/SUPABASE_RETENTION.md)

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

// -----------------------------------------------------------------------------
// POST-CONNECT CAPTIVE-PORTAL LOGIN (issue #33)
// -----------------------------------------------------------------------------
// SEPARATE from the WiFiManager SSID/password onboarding above. After the board
// has ALREADY joined your router's Wi-Fi, some networks (hotels, hostels, campus
// / ISP hotspots) still require a one-time browser login on a "captive portal"
// page before the internet works. When ENABLE_CAPTIVE_PORTAL_LOGIN is 1 the
// firmware:
//   1. Probes for internet right after Wi-Fi connects (CAPTIVE_PROBE_URL).
//   2. If a portal intercepts the probe, it fetches the portal login page and
//      AUTO-DETECTS the username/password fields — no field names are hardcoded.
//   3. Serves a local helper page at http://<device-ip>/portal where you enter
//      the ISP portal username/password; the board submits the login for you.
//   4. Falls back to "open the portal in your browser" for challenge/JS portals
//      (e.g. MikroTik CHAP) that cannot be automated safely.
//
// While a captive portal is blocking the internet, background Supabase uploads
// and the remote tunnel are PAUSED and the serial log prints a clear step-by-step
// banner with the exact URL to open (http://<device-ip>/portal). A connectivity
// heartbeat re-probes every 30s and resumes cloud uploads automatically once you
// have logged in — via the /portal helper or a manual browser login (issue #40).
//
// Only your Wi-Fi credentials (via WiFiManager/NVS) are persisted — the ISP
// portal username/password are used once and never stored. Set to 0 to disable
// the whole feature (the firmware then behaves exactly as before, with cloud
// uploads never gated on connectivity). See docs/CONFIG_SETUP.md for details.
// -----------------------------------------------------------------------------
#define ENABLE_CAPTIVE_PORTAL_LOGIN 1

// Plain-HTTP endpoint that returns "204 No Content" on an open network. A
// captive portal intercepts it with a redirect or a login page, which is how we
// detect the portal. Keep it HTTP (not HTTPS) so interception is observable.
#define CAPTIVE_PROBE_URL           "http://connectivitycheck.gstatic.com/generate_204"

// Per-request timeout (ms) for the probe and portal submit.
#define CAPTIVE_PROBE_TIMEOUT_MS    6000

// Give up automated submission after this many failed attempts and steer the
// user to the manual browser fallback.
#define CAPTIVE_MAX_LOGIN_ATTEMPTS  3

// Connectivity-heartbeat cadence (ms). While OFFLINE (behind a portal) the
// firmware re-probes every CAPTIVE_PERIODIC_REPROBE_MS so it notices as soon as
// you log in. While ONLINE it re-probes every CAPTIVE_ONLINE_HEARTBEAT_MS so a
// portal that re-appears (e.g. an expiring ISP session) is caught and cloud
// uploads are paused again (issue #40).
#define CAPTIVE_PERIODIC_REPROBE_MS  30000UL
#define CAPTIVE_ONLINE_HEARTBEAT_MS  60000UL
