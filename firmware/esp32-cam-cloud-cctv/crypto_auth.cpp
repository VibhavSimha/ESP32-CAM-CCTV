#include "crypto_auth.h"
#include "config.h"

#include <Preferences.h>
#include <ArduinoJson.h>
#include <string.h>

#include <mbedtls/ecdh.h>
#include <mbedtls/ecp.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>
#include <mbedtls/base64.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>

// -----------------------------------------------------------------------------
// Device static X25519 keypair (persisted in NVS namespace "crypto").
// -----------------------------------------------------------------------------
static uint8_t s_priv[32];      // device X25519 private scalar
static uint8_t s_pub[32];       // device X25519 public key (u-coordinate)
static bool    s_ready = false;

static mbedtls_ctr_drbg_context s_drbg;
static mbedtls_entropy_context  s_entropy;

// -----------------------------------------------------------------------------
// Session token table (small in-RAM ring). Tokens expire after SESSION_TTL_MS.
// -----------------------------------------------------------------------------
#define SESSION_SLOTS     4
#define SESSION_TTL_MS    (6UL * 60UL * 60UL * 1000UL)   // 6 hours
#define TOKEN_RAW_LEN     32

struct Session {
  char          token[45];   // base64 of 32 bytes + NUL (44 chars)
  unsigned long expiresAt;
};
static Session s_sessions[SESSION_SLOTS];

static size_t b64enc(const uint8_t *in, size_t inlen, char *out, size_t outcap) {
  size_t olen = 0;
  mbedtls_base64_encode((unsigned char *)out, outcap, &olen, in, inlen);
  if (olen < outcap) out[olen] = '\0';
  return olen;
}

static int b64dec(const char *in, size_t inlen, uint8_t *out, size_t outcap, size_t *olen) {
  return mbedtls_base64_decode(out, outcap, olen, (const unsigned char *)in, inlen);
}

static bool ctEqual(const char *a, const char *b) {
  // constant-time-ish compare over equal-length strings
  size_t la = strlen(a), lb = strlen(b);
  if (la != lb) return false;
  uint8_t diff = 0;
  for (size_t i = 0; i < la; i++) diff |= (uint8_t)(a[i] ^ b[i]);
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
  Serial.printf("[Crypto] setup took %lums, heap %u -> %u\n",
                millis() - t0, h0, ESP.getFreeHeap());
}

// -----------------------------------------------------------------------------
// ECDH shared secret: device private + browser ephemeral public (32 bytes LE).
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

bool cryptoAuthCheckSession(httpd_req_t *req) {
  char token[64] = {0};
  bool have = false;

  // Prefer explicit header, fall back to Cookie: sid=...
  size_t hl = httpd_req_get_hdr_value_len(req, "X-Session");
  if (hl > 0 && hl < sizeof(token)) {
    if (httpd_req_get_hdr_value_str(req, "X-Session", token, sizeof(token)) == ESP_OK) have = true;
  }
  if (!have) {
    size_t cl = httpd_req_get_hdr_value_len(req, "Cookie");
    if (cl > 0) {
      char *cookie = (char *)malloc(cl + 1);
      if (cookie && httpd_req_get_hdr_value_str(req, "Cookie", cookie, cl + 1) == ESP_OK) {
        char *p = strstr(cookie, "sid=");
        if (p) {
          p += 4;
          size_t i = 0;
          while (*p && *p != ';' && *p != ' ' && i < sizeof(token) - 1) token[i++] = *p++;
          token[i] = '\0';
          have = i > 0;
        }
      }
      free(cookie);
    }
  }
  if (!have) return false;

  unsigned long now = millis();
  for (int i = 0; i < SESSION_SLOTS; i++) {
    if (s_sessions[i].token[0] == '\0') continue;
    if (now > s_sessions[i].expiresAt) continue;
    if (ctEqual(s_sessions[i].token, token)) return true;
  }
  return false;
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
static esp_err_t login_handler(httpd_req_t *req) {
  if (!s_ready) { httpd_resp_send_500(req); return ESP_FAIL; }

  int total = req->content_len;
  if (total <= 0 || total > 2048) { httpd_resp_send_408(req); return ESP_FAIL; }
  char *body = (char *)malloc(total + 1);
  if (!body) { httpd_resp_send_500(req); return ESP_FAIL; }
  int recvd = 0;
  while (recvd < total) {
    int r = httpd_req_recv(req, body + recvd, total - recvd);
    if (r <= 0) { free(body); httpd_resp_send_408(req); return ESP_FAIL; }
    recvd += r;
  }
  body[total] = '\0';

  StaticJsonDocument<1024> in;
  DeserializationError jerr = deserializeJson(in, body);
  free(body);
  if (jerr) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_send(req, "bad json", HTTPD_RESP_USE_STRLEN);
    return ESP_FAIL;
  }

  const char *epk_b = in["epk"] | "";
  const char *iv_b  = in["iv"]  | "";
  const char *ct_b  = in["ct"]  | "";
  const char *tag_b = in["tag"] | "";

  uint8_t epk[32], iv[16], tag[16];
  uint8_t ct[1024], pt[1024];
  size_t epklen = 0, ivlen = 0, ctlen = 0, taglen = 0;

  bool okdec =
    b64dec(epk_b, strlen(epk_b), epk, sizeof(epk), &epklen) == 0 && epklen == 32 &&
    b64dec(iv_b,  strlen(iv_b),  iv,  sizeof(iv),  &ivlen) == 0 && ivlen == 12 &&
    b64dec(tag_b, strlen(tag_b), tag, sizeof(tag), &taglen) == 0 && taglen == 16 &&
    b64dec(ct_b,  strlen(ct_b),  ct,  sizeof(ct),  &ctlen) == 0 && ctlen > 0;

  if (!okdec) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_send(req, "bad fields", HTTPD_RESP_USE_STRLEN);
    return ESP_FAIL;
  }

  uint8_t shared[32], aesKey[32];
  if (!ecdhShared(epk, shared) || !deriveAesKey(shared, aesKey)) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  int ptlen = aesGcmDecrypt(aesKey, iv, ivlen, ct, ctlen, tag, taglen, pt);
  memset(shared, 0, sizeof(shared));
  memset(aesKey, 0, sizeof(aesKey));
  if (ptlen < 0) {
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_send(req, "decrypt failed", HTTPD_RESP_USE_STRLEN);
    return ESP_FAIL;
  }
  pt[ptlen] = '\0';

  StaticJsonDocument<256> creds;
  if (deserializeJson(creds, (const char *)pt)) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_send(req, "bad creds", HTTPD_RESP_USE_STRLEN);
    return ESP_FAIL;
  }
  const char *user = creds["user"] | "";
  const char *pass = creds["pass"] | "";

  if (!ctEqual(user, CONFIG_HTTP_USER) || !ctEqual(pass, CONFIG_HTTP_PASS)) {
    memset(pt, 0, sizeof(pt));
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_send(req, "invalid credentials", HTTPD_RESP_USE_STRLEN);
    return ESP_FAIL;
  }
  memset(pt, 0, sizeof(pt));

  char token[64];
  issueSession(token, sizeof(token));

  char setcookie[128];
  snprintf(setcookie, sizeof(setcookie), "sid=%s; Path=/; Max-Age=21600; SameSite=Lax", token);

  StaticJsonDocument<128> out;
  out["token"] = token;
  char outbuf[160];
  serializeJson(out, outbuf);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Set-Cookie", setcookie);
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  Serial.println("[Crypto] Login OK — session issued");
  return httpd_resp_send(req, outbuf, HTTPD_RESP_USE_STRLEN);
}

void registerCryptoAuthHandlers(httpd_handle_t server) {
  httpd_uri_t pubkey_uri = { .uri = "/pubkey", .method = HTTP_GET,  .handler = pubkey_handler, .user_ctx = NULL };
  httpd_uri_t login_uri  = { .uri = "/login",  .method = HTTP_POST, .handler = login_handler,  .user_ctx = NULL };
  httpd_register_uri_handler(server, &pubkey_uri);
  httpd_register_uri_handler(server, &login_uri);
  Serial.println("[Crypto] Registered /pubkey (GET) and /login (POST)");
}
