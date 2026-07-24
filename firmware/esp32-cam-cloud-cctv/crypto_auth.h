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
//  THREAT MODEL (IMPORTANT): This protects the LOGIN CREDENTIALS only. The MJPEG
//  stream still flows in plaintext over BORE, and the /pubkey exchange over
//  plain HTTP is subject to MITM (an attacker could substitute their own key).
//  For full-channel confidentiality/integrity + server authentication, use the
//  HTTPS SELFHOST tunnel (CONFIG_TUNNEL_MODE_SELFHOST) instead of plain BORE.
// =============================================================================

// Initialise crypto: load or (first boot) generate + persist the device X25519
// keypair in NVS. Safe to call once from setup().
void setupCryptoAuth();

// Register /pubkey and /login handlers on the given server. Call after
// httpd_start(). /pubkey and /login are intentionally UNAUTHENTICATED.
void registerCryptoAuthHandlers(httpd_handle_t server);

// Validate the session token carried on an incoming request (Cookie: sid=... or
// X-Session header). Returns true if the request carries a live, unexpired
// session established via the encrypted-login flow.
bool cryptoAuthCheckSession(httpd_req_t *req);

// Convenience: enforce a session on a request; if missing/invalid sends a 401
// and returns false so the handler can `return`.
bool cryptoAuthRequire(httpd_req_t *req);
