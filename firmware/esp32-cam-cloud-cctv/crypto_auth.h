#pragma once

#include "esp_http_server.h"
#include <Arduino.h>

// =============================================================================
//  crypto_auth.h — X25519 ECDH + AES-256-GCM encrypted login for the plain-HTTP
//  BORE tunnel, plus session-token auth for the camera endpoints.
//
//  WHY NOT RSA: RSA-2048 keygen/decrypt on the ESP32 (mbedTLS) is heap- and
//  CPU-heavy on an already heap-constrained device. X25519 keygen + shared
//  secret derivation are ~milliseconds with a tiny footprint, and an ephemeral
//  browser keypair gives forward secrecy.
//
//  LOGIN FLOW (browser <-> device), all base64 on the wire:
//   1. GET /pubkey  -> device X25519 public key (used only when the key is not
//      pinned client-side; see pinning below).
//   2. GET /nonce   -> single-use, short-lived replay nonce (issue #12).
//   3. Browser derives AES-256-GCM key = HKDF-SHA256(ECDH(device_pub, eph_priv),
//      info "esp32cam-ecdh-aes256gcm"), then AES-256-GCM-encrypts
//      { "user":..., "pass":..., "nonce":... } with a random 12B IV.
//   4. POST /login { epk, iv, ct, tag } -> device decrypts, verifies + CONSUMES
//      the nonce, checks credentials, and issues a 6h session token.
//
//  The /view browser crypto is BUNDLED (pure JS: TweetNaCl X25519 + AES-256-GCM
//  + HKDF-SHA256), so login works over plain http:// — it does NOT depend on
//  window.crypto.subtle (which browsers disable outside secure contexts).
//
//  THREAT MODEL (IMPORTANT):
//   - This protects the LOGIN CREDENTIALS. The MJPEG /stream still flows in
//     plaintext over BORE.
//   - Pubkey-substitution MITM is mitigated by PINNING the device public key in
//     the client (CONFIG_DEVICE_PUBKEY_B64), verified out-of-band from the
//     serial console. When pinned, the browser never trusts /pubkey.
//   - Replay of a captured /login body is prevented by the single-use /nonce.
//   - RESIDUAL RISK: because the /view HTML itself is served over plain HTTP, a
//     MITM that REWRITES the served HTML/JS (including the pinned constant or the
//     crypto code) can still defeat client-side checks. The only COMPLETE fix is
//     TLS: front BORE with an HTTPS reverse proxy, or use HTTPS where the
//     hardware permits. See docs/SECURITY.md and docs/CONFIG_SETUP.md.
// =============================================================================

// Initialise crypto: load or (first boot) generate + persist the device X25519
// keypair in NVS. Logs the device pubkey (b64) and, if CONFIG_DEVICE_PUBKEY_B64
// is set, whether it matches. Safe to call once from setup().
void setupCryptoAuth();

// Register /pubkey (GET), /nonce (GET), and /login (POST) handlers on the given
// server. Call after httpd_start(). All three are intentionally UNAUTHENTICATED.
void registerCryptoAuthHandlers(httpd_handle_t server);

// Validate the session token carried on an incoming request. Accepts, in order:
// X-Session header (fetch), ?token=<sid> query param (MJPEG <img>, issue #10), or
// Cookie: sid=... . Returns true if the request carries a live, unexpired
// session established via the encrypted-login flow.
bool cryptoAuthCheckSession(httpd_req_t *req);

// Convenience: enforce a session on a request; if missing/invalid sends a 401
// and returns false so the handler can `return`.
bool cryptoAuthRequire(httpd_req_t *req);
