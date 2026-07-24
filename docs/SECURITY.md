# Security model — encrypted login over the public tunnel

This document explains how the ESP32-CAM protects the viewer login and, just as
importantly, what it does **not** protect. Read this before exposing a camera to
the internet.

## Encrypted login (X25519 ECDH + AES-256-GCM)

The camera is reachable through a **BORE tunnel, which is plain HTTP (no TLS)**.
Sending HTTP Basic Auth over that tunnel would transmit `base64(user:pass)` —
effectively plaintext. To avoid that, the login is encrypted end-to-end between
the browser and the device using an elliptic-curve key exchange:

1. **Device keypair.** On first boot the ESP32 generates an **X25519** keypair
   and persists it in NVS (`Preferences`, namespace `crypto`). The public key is
   stable across reboots.
2. **`GET /pubkey`** (unauthenticated) returns the device X25519 public key
   (32 raw bytes, base64) as JSON: `{"alg":"X25519","pubkey":"<b64>"}`.
3. **Browser.** Using WebCrypto, the browser imports the device key, generates an
   **ephemeral** X25519 keypair, derives the ECDH shared secret, runs it through
   **HKDF-SHA256** (info `esp32cam-ecdh-aes256gcm`) to get a 256-bit AES key, and
   **AES-256-GCM**-encrypts `{"user":...,"pass":...}` with a fresh 12-byte IV.
4. **`POST /login`** (unauthenticated) carries the browser ephemeral public key,
   IV, ciphertext, and GCM tag (all base64). The ESP32 derives the same shared
   secret with mbedTLS (`mbedtls_ecdh`, Curve25519), decrypts + verifies the tag,
   and validates the credentials against `CONFIG_HTTP_USER` / `CONFIG_HTTP_PASS`.
5. **Session token.** On success the device issues a random 32-byte session token
   (base64) with a 6-hour TTL, returned as a `Set-Cookie: sid=...` and in the
   JSON body. Subsequent requests to `/stream`, `/capture`, and `/flash` present
   this token (cookie for the MJPEG `<img>`, or `X-Session` header for `fetch`),
   so the expensive key exchange happens **once per login**, not per request.

### Why not RSA?
RSA-2048 keygen and RSA-OAEP decrypt on the ESP32 (mbedTLS) are heap- and
CPU-heavy on an already heap-constrained device. X25519 keygen and shared-secret
derivation are on the order of **milliseconds** with a tiny footprint, and the
ephemeral browser key provides **forward secrecy** that RSA-OAEP would not.

## What this protects
- The **username and password** are never sent in plaintext over the tunnel.
- Per-login forward secrecy (a compromised session later does not reveal the
  password used to log in).

## What this does NOT protect (important caveats)
- **The video stream is still plaintext.** The MJPEG `/stream` frames flow
  unencrypted over BORE. Anyone able to observe the tunnel traffic can watch the
  feed.
- **MITM on the pubkey exchange.** Because `/pubkey` is fetched over plain HTTP
  with no server authentication, an active attacker could substitute their own
  public key and decrypt the login. ECDH alone cannot prevent this without a
  trusted channel to distribute/verify the device key.
- **`crypto.subtle` availability.** Browsers only expose `window.crypto.subtle`
  in **secure contexts** (HTTPS or `http://localhost`). Over plain
  `http://bore.pub:<port>` many browsers disable it, so the encrypted login form
  will show an error and cannot run. 

## Recommended: use the HTTPS SELFHOST tunnel
For a genuinely secure deployment, switch to SELFHOST mode
(`CONFIG_TUNNEL_MODE_SELFHOST`), served over HTTPS at
`https://esp32-tunnel.onrender.com/<CONFIG_SELFHOST_TUNNEL_ID>/view`. This:
- encrypts the **entire** channel (stream included) with TLS,
- authenticates the server (defeating the pubkey-substitution MITM), and
- makes `crypto.subtle` available so the encrypted login works everywhere.

Over HTTPS the ECDH login is defense-in-depth; the channel itself already
protects everything.

## Endpoint summary
| Endpoint        | Auth            | Notes                                        |
|-----------------|-----------------|----------------------------------------------|
| `GET /pubkey`   | none            | Device X25519 public key                     |
| `POST /login`   | none            | ECDH+AES-GCM encrypted credentials → session |
| `GET /view`     | none (loads UI) | Serves the login form + viewer               |
| `GET /stream`   | session         | MJPEG stream (sid cookie)                     |
| `GET /capture`  | session         | Single JPEG                                  |
| `GET /flash`    | session         | Reader (`{"flash":0|1}`) / setter (`?s=1|0`) |
| `GET /health`   | none            | Heap/uptime only                             |
