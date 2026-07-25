#include "crypto_auth.h"
#include "config.h"

#include <Preferences.h>
#include <ArduinoJson.h>
#include <string.h>
#include <new>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <mbedtls/ecdh.h>
#include <mbedtls/ecp.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>
#include <mbedtls/base64.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/platform_util.h>

// -----------------------------------------------------------------------------
// Device static X25519 keypair (persisted in NVS namespace "crypto").
// NVS key names MUST stay <= 15 chars (Preferences limit): "x25519_priv" (11)
// and "x25519_pub" (10) are within budget — do not lengthen them.
// -----------------------------------------------------------------------------
static uint8_t s_priv[32];      // device X25519 private scalar
static uint8_t s_pub[32];       // device X25519 public key (u-coordinate)
static bool    s_ready = false;

static mbedtls_ctr_drbg_context s_drbg;
static mbedtls_entropy_context  s_entropy;

// -----------------------------------------------------------------------------
// Concurrency: the ESP-IDF httpd services requests from multiple sockets, so the
// session table, the DRBG (mbedTLS CTR_DRBG is NOT thread-safe on its own), and
// login CPU-cost must all be serialized. One recursive mutex guards them.
// -----------------------------------------------------------------------------
static SemaphoreHandle_t s_lock = NULL;
static inline void LOCK()   { if (s_lock) xSemaphoreTakeRecursive(s_lock, portMAX_DELAY); }
static inline void UNLOCK() { if (s_lock) xSemaphoreGiveRecursive(s_lock); }

// -----------------------------------------------------------------------------
// Session token table (small in-RAM ring). Tokens expire after SESSION_TTL_MS.
// -----------------------------------------------------------------------------
#define SESSION_SLOTS     4
#define SESSION_TTL_MS    (6UL * 60UL * 60UL * 1000UL)   // 6 hours
#define TOKEN_RAW_LEN     32
#define LOGIN_FAIL_GAP_MS 500     // BUG2: backoff applied ONLY after a failed login

struct Session {
  char          token[45];   // base64 of 32 bytes + NUL (44 chars)
  unsigned long expiresAt;
};
static Session s_sessions[SESSION_SLOTS];
static unsigned long s_lastFailedLogin = 0;   // BUG2: only FAILED attempts set this

static size_t b64enc(const uint8_t *in, size_t inlen, char *out, size_t outcap) {
  size_t olen = 0;
  mbedtls_base64_encode((unsigned char *)out, outcap, &olen, in, inlen);
  if (olen < outcap) out[olen] = '\0';
  return olen;
}

static int b64dec(const char *in, size_t inlen, uint8_t *out, size_t outcap, size_t *olen) {
  return mbedtls_base64_decode(out, outcap, olen, (const unsigned char *)in, inlen);
}

// Portable constant-time equality (fixed work regardless of where bytes differ).
// Avoids mbedtls_ct_memcmp, which is absent on older mbedTLS. Length is compared
// first; token/credential lengths are not secret.
static bool ctEqual(const char *a, const char *b) {
  size_t la = strlen(a), lb = strlen(b);
  if (la != lb) return false;
  volatile uint8_t diff = 0;
  for (size_t i = 0; i < la; i++) {
    diff |= (uint8_t)((uint8_t)a[i] ^ (uint8_t)b[i]);
  }
  return diff == 0;
}

// -----------------------------------------------------------------------------
// RNG
// -----------------------------------------------------------------------------
static bool rngInit() {
  mbedtls_entropy_init(&s_entropy);
  mbedtls_ctr_drbg_init(&s_drbg);
  const char *pers = "esp32cam-crypto";
  int rc = mbedtls_ctr_drbg_seed(&s_drbg, mbedtls_entropy_func, &s_entropy,
                                 (const unsigned char *)pers, strlen(pers));
  if (rc != 0) {
    Serial.printf("[Crypto] ctr_drbg_seed failed: -0x%04x\n", -rc);
    return false;
  }
  return true;
}

// -----------------------------------------------------------------------------
// X25519 keypair via mbedTLS ECP (Curve25519 / MBEDTLS_ECP_DP_CURVE25519).
// -----------------------------------------------------------------------------
static bool genKeypair() {
  mbedtls_ecp_group grp;
  mbedtls_mpi d;
  mbedtls_ecp_point Q;
  mbedtls_ecp_group_init(&grp);
  mbedtls_mpi_init(&d);
  mbedtls_ecp_point_init(&Q);

  bool ok = false;
  do {
    if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519) != 0) break;
    if (mbedtls_ecdh_gen_public(&grp, &d, &Q, mbedtls_ctr_drbg_random, &s_drbg) != 0) break;
    // Curve25519 private scalar and public u-coord are 32 little-endian bytes.
    if (mbedtls_mpi_write_binary_le(&d, s_priv, 32) != 0) break;
    if (mbedtls_mpi_write_binary_le(&Q.MBEDTLS_PRIVATE(X), s_pub, 32) != 0) break;
    ok = true;
  } while (0);

  mbedtls_ecp_point_free(&Q);
  mbedtls_mpi_free(&d);
  mbedtls_ecp_group_free(&grp);
  return ok;
}

void setupCryptoAuth() {
  memset(s_sessions, 0, sizeof(s_sessions));

  if (!s_lock) s_lock = xSemaphoreCreateRecursiveMutex();
  if (!s_lock) {
    Serial.println("[Crypto] FATAL: could not create mutex — encrypted login unavailable");
    return;
  }

  uint32_t h0 = ESP.getFreeHeap();
  unsigned long t0 = millis();

  if (!rngInit()) {
    Serial.println("[Crypto] RNG init FAILED — encrypted login unavailable");
    return;
  }

  Preferences p;
  p.begin("crypto", false);
  size_t plen = p.getBytesLength("x25519_priv");
  size_t qlen = p.getBytesLength("x25519_pub");
  if (plen == 32 && qlen == 32) {
    p.getBytes("x25519_priv", s_priv, 32);
    p.getBytes("x25519_pub", s_pub, 32);
    s_ready = true;
    Serial.println("[Crypto] Loaded persisted X25519 keypair from NVS");
  } else {
    Serial.println("[Crypto] No stored keypair — generating (first boot)");
    if (genKeypair()) {
      p.putBytes("x25519_priv", s_priv, 32);
      p.putBytes("x25519_pub", s_pub, 32);
      s_ready = true;
      Serial.println("[Crypto] Generated + persisted X25519 keypair");
    } else {
      Serial.println("[Crypto] Keypair generation FAILED");
    }
  }
  p.end();

  char pubb64[64];
  b64enc(s_pub, 32, pubb64, sizeof(pubb64));
  Serial.printf("[Crypto] Device pubkey (b64): %s\n", pubb64);
  Serial.printf("[Crypto] setup took %lums, heap %u -> %u (delta %d)\n",
                millis() - t0, h0, ESP.getFreeHeap(), (int)ESP.getFreeHeap() - (int)h0);
}

// -----------------------------------------------------------------------------
// ECDH shared secret: device private + browser ephemeral public (32 bytes LE).
// Caller MUST hold the lock (uses s_drbg).
// -----------------------------------------------------------------------------
static bool ecdhShared(const uint8_t *peerPub, uint8_t out[32]) {
  mbedtls_ecp_group grp;
  mbedtls_mpi d, z;
  mbedtls_ecp_point Qp;
  mbedtls_ecp_group_init(&grp);
  mbedtls_mpi_init(&d);
  mbedtls_mpi_init(&z);
  mbedtls_ecp_point_init(&Qp);

  bool ok = false;
  do {
    if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519) != 0) break;
    if (mbedtls_mpi_read_binary_le(&d, s_priv, 32) != 0) break;
    // Peer public: set X (u-coord) LE, Z = 1.
    if (mbedtls_mpi_read_binary_le(&Qp.MBEDTLS_PRIVATE(X), peerPub, 32) != 0) break;
    if (mbedtls_mpi_lset(&Qp.MBEDTLS_PRIVATE(Z), 1) != 0) break;
    if (mbedtls_ecdh_compute_shared(&grp, &z, &Qp, &d,
                                    mbedtls_ctr_drbg_random, &s_drbg) != 0) break;
    if (mbedtls_mpi_write_binary_le(&z, out, 32) != 0) break;
    ok = true;
  } while (0);

  mbedtls_ecp_point_free(&Qp);
  mbedtls_mpi_free(&z);
  mbedtls_mpi_free(&d);
  mbedtls_ecp_group_free(&grp);
  return ok;
}

// HKDF-SHA256(shared) -> 32-byte AES key. Use a fixed info string; the ephemeral
// browser key already provides freshness, and GCM uses a random IV per message.
static bool deriveAesKey(const uint8_t shared[32], uint8_t aesKey[32]) {
  const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  const unsigned char info[] = "esp32cam-ecdh-aes256gcm";
  int rc = mbedtls_hkdf(md, NULL, 0, shared, 32, info, sizeof(info) - 1, aesKey, 32);
  return rc == 0;
}

// AES-256-GCM decrypt. Returns plaintext length or -1 on auth failure.
static int aesGcmDecrypt(const uint8_t *key, const uint8_t *iv, size_t ivlen,
                         const uint8_t *ct, size_t ctlen,
                         const uint8_t *tag, size_t taglen,
                         uint8_t *pt) {
  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  int rc = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256);
  if (rc == 0) {
    rc = mbedtls_gcm_auth_decrypt(&gcm, ctlen, iv, ivlen, NULL, 0,
                                  tag, taglen, ct, pt);
  }
  mbedtls_gcm_free(&gcm);
  return rc == 0 ? (int)ctlen : -1;
}

// Caller MUST hold the lock (uses s_drbg + s_sessions).
static void issueSession(char *outToken, size_t cap) {
  uint8_t raw[TOKEN_RAW_LEN];
  mbedtls_ctr_drbg_random(&s_drbg, raw, sizeof(raw));
  char b64[45];
  b64enc(raw, sizeof(raw), b64, sizeof(b64));

  // Find a free/expired slot (or overwrite the soonest-expiring).
  unsigned long now = millis();
  int slot = 0;
  unsigned long soonest = (unsigned long)-1;
  for (int i = 0; i < SESSION_SLOTS; i++) {
    if (s_sessions[i].token[0] == '\0' || now > s_sessions[i].expiresAt) { slot = i; break; }
    if (s_sessions[i].expiresAt < soonest) { soonest = s_sessions[i].expiresAt; slot = i; }
  }
  strncpy(s_sessions[slot].token, b64, sizeof(s_sessions[slot].token) - 1);
  s_sessions[slot].token[sizeof(s_sessions[slot].token) - 1] = '\0';
  s_sessions[slot].expiresAt = now + SESSION_TTL_MS;
  strncpy(outToken, b64, cap - 1);
  outToken[cap - 1] = '\0';
}

// Match a cookie token only at a proper name boundary ("sid=" at start of the
// header or right after "; ") so e.g. "xsid=" cannot false-match.
static bool extractCookieToken(const char *cookie, char *out, size_t cap) {
  const char *p = cookie;
  while (p && *p) {
    bool atBoundary = (p == cookie) || (p[-1] == ' ');
    if (atBoundary && strncmp(p, "sid=", 4) == 0) {
      p += 4;
      size_t i = 0;
      while (*p && *p != ';' && *p != ' ' && i < cap - 1) out[i++] = *p++;
      out[i] = '\0';
      return i > 0;
    }
    p++;
  }
  return false;
}

// Issue #10: an MJPEG <img> cannot send custom headers, so /stream (and /capture
// when loaded as an <img>) authenticate via a ?token=<sid> query param. Extract
// it from the request URL query string. token/sid are URL-safe base64 so no
// percent-decoding is needed here.
static bool extractQueryToken(httpd_req_t *req, char *out, size_t cap) {
  size_t qlen = httpd_req_get_url_query_len(req);
  if (qlen == 0 || qlen >= 512) return false;
  char *q = (char *)malloc(qlen + 1);
  if (!q) return false;
  bool got = false;
  if (httpd_req_get_url_query_str(req, q, qlen + 1) == ESP_OK) {
    if (httpd_query_key_value(q, "token", out, cap) == ESP_OK && out[0] != '\0') {
      got = true;
    }
  }
  free(q);
  return got;
}

bool cryptoAuthCheckSession(httpd_req_t *req) {
  char token[64] = {0};
  bool have = false;

  // 1) Prefer explicit header (used by fetch()).
  size_t hl = httpd_req_get_hdr_value_len(req, "X-Session");
  if (hl > 0 && hl < sizeof(token)) {
    if (httpd_req_get_hdr_value_str(req, "X-Session", token, sizeof(token)) == ESP_OK) have = true;
  }
  // 2) Issue #10: ?token=<sid> query param (used by the MJPEG <img> stream).
  if (!have) {
    have = extractQueryToken(req, token, sizeof(token));
  }
  // 3) Fall back to Cookie: sid=...
  if (!have) {
    size_t cl = httpd_req_get_hdr_value_len(req, "Cookie");
    if (cl > 0 && cl < 512) {
      char *cookie = (char *)malloc(cl + 1);
      if (cookie) {
        if (httpd_req_get_hdr_value_str(req, "Cookie", cookie, cl + 1) == ESP_OK) {
          have = extractCookieToken(cookie, token, sizeof(token));
        }
        free(cookie);
      }
    }
  }
  if (!have) return false;

  bool valid = false;
  unsigned long now = millis();
  LOCK();
  for (int i = 0; i < SESSION_SLOTS; i++) {
    if (s_sessions[i].token[0] == '\0') continue;
    if (now > s_sessions[i].expiresAt) continue;
    if (ctEqual(s_sessions[i].token, token)) { valid = true; break; }
  }
  UNLOCK();
  return valid;
}

bool cryptoAuthRequire(httpd_req_t *req) {
  if (cryptoAuthCheckSession(req)) return true;
  httpd_resp_set_status(req, "401 Unauthorized");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_send(req, "login required", HTTPD_RESP_USE_STRLEN);
  return false;
}

// -----------------------------------------------------------------------------
// HTTP handlers
// -----------------------------------------------------------------------------
static esp_err_t pubkey_handler(httpd_req_t *req) {
  if (!s_ready) { httpd_resp_send_500(req); return ESP_FAIL; }
  char pubb64[64];
  b64enc(s_pub, 32, pubb64, sizeof(pubb64));

  StaticJsonDocument<128> doc;
  doc["alg"] = "X25519";
  doc["pubkey"] = pubb64;   // 32 raw bytes, base64
  char out[160];
  serializeJson(doc, out);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
}

// POST /login body (JSON):
//   { "epk": b64(32B browser ephemeral pub),
//     "iv":  b64(12B),
//     "ct":  b64(ciphertext),
//     "tag": b64(16B GCM tag) }
// Plaintext is JSON { "user":..., "pass":... }.
//
// Memory: the big scratch buffers (ct/pt) and the request JSON are heap-
// allocated, NOT stack, so this handler stays well within the httpd task stack.
//
// Rate limiting (BUG2): a backoff of LOGIN_FAIL_GAP_MS is applied ONLY after a
// FAILED attempt. A valid login is never rate-limited, and a flood of bogus
// requests cannot starve a legitimate one.
static esp_err_t login_handler(httpd_req_t *req) {
  if (!s_ready) { httpd_resp_send_500(req); return ESP_FAIL; }

  uint32_t h0 = ESP.getFreeHeap();
  unsigned long t0 = millis();

  // Backoff: reject only while we are inside the window opened by a PRIOR FAILURE.
  LOCK();
  unsigned long now = millis();
  bool backoff = (s_lastFailedLogin != 0 && (now - s_lastFailedLogin) < LOGIN_FAIL_GAP_MS);
  UNLOCK();
  if (backoff) {
    Serial.println("[Crypto] login rejected: backoff after recent failure");
    httpd_resp_set_status(req, "429 Too Many Requests");
    httpd_resp_set_hdr(req, "Retry-After", "1");
    httpd_resp_send(req, "slow down", HTTPD_RESP_USE_STRLEN);
    return ESP_FAIL;
  }

  int total = req->content_len;
  if (total <= 0 || total > 2048) {
    Serial.printf("[Crypto] login rejected: bad content_len=%d\n", total);
    LOCK(); s_lastFailedLogin = millis(); UNLOCK();
    httpd_resp_send_408(req);
    return ESP_FAIL;
  }

  // Heap scratch: request body + decode buffers + plaintext (kept OFF the stack).
  char    *body  = (char *)malloc(total + 1);
  uint8_t *ctbuf = (uint8_t *)malloc(1024);
  uint8_t *ptbuf = (uint8_t *)malloc(1024);
  DynamicJsonDocument *in = new (std::nothrow) DynamicJsonDocument(1024);
  if (!body || !ctbuf || !ptbuf || !in) {
    Serial.println("[Crypto] login rejected: OOM allocating scratch");
    free(body); free(ctbuf); free(ptbuf); delete in;
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  esp_err_t result = ESP_FAIL;
  const char *failReason = "unknown";
  int httpStatusFail = 0;

  do {
    int recvd = 0;
    bool recvOk = true;
    while (recvd < total) {
      int r = httpd_req_recv(req, body + recvd, total - recvd);
      if (r <= 0) { recvOk = false; break; }
      recvd += r;
    }
    if (!recvOk) { failReason = "recv"; httpStatusFail = 408; break; }
    body[total] = '\0';

    if (deserializeJson(*in, body)) { failReason = "bad-json"; httpStatusFail = 400; break; }

    const char *epk_b = (*in)["epk"] | "";
    const char *iv_b  = (*in)["iv"]  | "";
    const char *ct_b  = (*in)["ct"]  | "";
    const char *tag_b = (*in)["tag"] | "";

    uint8_t epk[32], iv[16], tag[16];
    size_t epklen = 0, ivlen = 0, ctlen = 0, taglen = 0;

    bool okdec =
      b64dec(epk_b, strlen(epk_b), epk, sizeof(epk), &epklen) == 0 && epklen == 32 &&
      b64dec(iv_b,  strlen(iv_b),  iv,  sizeof(iv),  &ivlen) == 0 && ivlen == 12 &&
      b64dec(tag_b, strlen(tag_b), tag, sizeof(tag), &taglen) == 0 && taglen == 16 &&
      b64dec(ct_b,  strlen(ct_b),  ctbuf, 1024, &ctlen) == 0 && ctlen > 0 && ctlen <= 1000;

    if (!okdec) { failReason = "bad-fields"; httpStatusFail = 400; break; }

    uint8_t shared[32], aesKey[32];
    LOCK();
    bool derived = ecdhShared(epk, shared) && deriveAesKey(shared, aesKey);
    UNLOCK();
    if (!derived) {
      mbedtls_platform_zeroize(shared, sizeof(shared));
      mbedtls_platform_zeroize(aesKey, sizeof(aesKey));
      failReason = "ecdh"; httpStatusFail = 500; break;
    }

    int ptlen = aesGcmDecrypt(aesKey, iv, ivlen, ctbuf, ctlen, tag, taglen, ptbuf);
    mbedtls_platform_zeroize(shared, sizeof(shared));
    mbedtls_platform_zeroize(aesKey, sizeof(aesKey));
    if (ptlen < 0 || ptlen >= 1024) { failReason = "decrypt"; httpStatusFail = 401; break; }
    ptbuf[ptlen] = '\0';

    StaticJsonDocument<256> creds;
    if (deserializeJson(creds, (const char *)ptbuf)) {
      mbedtls_platform_zeroize(ptbuf, 1024);
      failReason = "bad-creds"; httpStatusFail = 400; break;
    }
    const char *user = creds["user"] | "";
    const char *pass = creds["pass"] | "";

    bool credsOk = ctEqual(user, CONFIG_HTTP_USER) && ctEqual(pass, CONFIG_HTTP_PASS);
    mbedtls_platform_zeroize(ptbuf, 1024);   // wipe decrypted credentials ASAP
    if (!credsOk) { failReason = "creds"; httpStatusFail = 401; break; }

    char token[64];
    LOCK();
    issueSession(token, sizeof(token));
    s_lastFailedLogin = 0;   // BUG2: success clears any prior backoff window
    UNLOCK();

    char setcookie[128];
    snprintf(setcookie, sizeof(setcookie),
             "sid=%s; Path=/; Max-Age=21600; HttpOnly; SameSite=Lax", token);

    StaticJsonDocument<128> out;
    out["token"] = token;
    char outbuf[160];
    serializeJson(out, outbuf);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Set-Cookie", setcookie);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, outbuf, HTTPD_RESP_USE_STRLEN);
    result = ESP_OK;
    Serial.printf("[Crypto] login OK — session issued in %lums, heap %u -> %u\n",
                  millis() - t0, h0, ESP.getFreeHeap());
  } while (0);

  if (result != ESP_OK && httpStatusFail != 0) {
    // BUG2: open the backoff window ONLY on failure.
    LOCK(); s_lastFailedLogin = millis(); UNLOCK();

    const char *st = "400 Bad Request";
    if (httpStatusFail == 401) st = "401 Unauthorized";
    else if (httpStatusFail == 408) st = "408 Request Timeout";
    else if (httpStatusFail == 500) st = "500 Internal Server Error";
    Serial.printf("[Crypto] login rejected: %s (%lums, heap=%u)\n",
                  failReason, millis() - t0, ESP.getFreeHeap());
    httpd_resp_set_status(req, st);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, failReason, HTTPD_RESP_USE_STRLEN);
  }

  // Wipe + free all scratch (no leaks on any path).
  mbedtls_platform_zeroize(ptbuf, 1024);
  free(body);
  free(ctbuf);
  free(ptbuf);
  delete in;
  return result;
}

void registerCryptoAuthHandlers(httpd_handle_t server) {
  httpd_uri_t pubkey_uri = { .uri = "/pubkey", .method = HTTP_GET,  .handler = pubkey_handler, .user_ctx = NULL };
  httpd_uri_t login_uri  = { .uri = "/login",  .method = HTTP_POST, .handler = login_handler,  .user_ctx = NULL };
  httpd_register_uri_handler(server, &pubkey_uri);
  httpd_register_uri_handler(server, &login_uri);
  Serial.println("[Crypto] Registered /pubkey (GET) and /login (POST)");
}
