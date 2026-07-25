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
2. **Device key source.** If `CONFIG_DEVICE_PUBKEY_B64` is **pinned** (see
   [docs/CONFIG_SETUP.md](CONFIG_SETUP.md)), the browser uses that trusted key
   directly. Otherwise it falls back to **`GET /pubkey`** (unauthenticated), which
   returns the device X25519 public key (32 raw bytes, base64) as
   `{"alg":"X25519","pubkey":"<b64>"}` — and the `/view` page shows a warning
   banner because an unpinned key is MITM-substitutable.
3. **Replay nonce.** The browser fetches **`GET /nonce`** (unauthenticated), a
   single-use, ~60s-TTL random value.
4. **Browser crypto (bundled, pure JS).** The `/view` page bundles a pure-JS
   implementation — **TweetNaCl** for X25519 (`nacl.scalarMult`) plus
   **AES-256-GCM** and **HKDF-SHA256** — so the login runs in **any** context,
   including plain `http://`. It generates an **ephemeral** X25519 keypair,
   derives the ECDH shared secret with the device key, runs it through
   **HKDF-SHA256** (info `esp32cam-ecdh-aes256gcm`) to get a 256-bit AES key, and
   **AES-256-GCM**-encrypts `{"user":...,"pass":...,"nonce":...}` with a fresh
   12-byte IV. `window.crypto.subtle` is used as a fast path when available
   (HTTPS/localhost) but is **not required**.
5. **`POST /login`** (unauthenticated) carries the browser ephemeral public key,
   IV, ciphertext, and GCM tag (all base64). The ESP32 derives the same shared
   secret with mbedTLS (`mbedtls_ecdh`, Curve25519), decrypts + verifies the tag,
   **verifies and consumes the nonce** (rejecting replays), and validates the
   credentials against `CONFIG_HTTP_USER` / `CONFIG_HTTP_PASS`.
6. **Session token.** On success the device issues a random 32-byte session token
   (base64) with a 6-hour TTL, returned as a `Set-Cookie: sid=...` and in the
   JSON body. Subsequent requests to `/stream`, `/capture`, and `/flash` present
   this token (cookie or `?token=` for the MJPEG `<img>`, or `X-Session` header
   for `fetch`), so the expensive key exchange happens **once per login**.

### Why not `crypto.subtle` only?
Browsers only expose `window.crypto.subtle` in **secure contexts** (HTTPS or
`http://localhost`). Over plain `http://bore.pub:<port>` it is `undefined`, which
previously broke login entirely (issue #12). Bundling the crypto in JS removes
that dependency. `crypto.getRandomValues` — used for the IV and ephemeral key —
**is** available in non-secure contexts, so randomness is not affected.

### Why not RSA?
RSA-2048 keygen and RSA-OAEP decrypt on the ESP32 (mbedTLS) are heap- and
CPU-heavy on an already heap-constrained device. X25519 keygen and shared-secret
derivation are on the order of **milliseconds** with a tiny footprint, and the
ephemeral browser key provides **forward secrecy** that RSA-OAEP would not.

## Defense against pubkey-substitution MITM: pin the device key
Because `/pubkey` is fetched over plain HTTP, an active attacker could serve
their **own** X25519 key, decrypt the login, and relay to the device. The
mitigation is **out-of-band key pinning**:

- The device prints its public key on the serial console at boot:
  `[Crypto] Device pubkey (b64): <VALUE>`.
- You copy that value into `CONFIG_DEVICE_PUBKEY_B64` in `config.h` and re-flash
  **once** (the keypair is NVS-persisted, so this is a one-off per device).
- The browser then encrypts **only** to the pinned key and never trusts
  `/pubkey`. A substituted key no longer decrypts the login → login aborts.

Full step-by-step: [docs/CONFIG_SETUP.md](CONFIG_SETUP.md).

## Replay protection: the `/nonce` endpoint
Each login includes a single-use nonce (from `GET /nonce`) inside the encrypted
plaintext. The device consumes it on use, so a captured `/login` body cannot be
replayed. Nonces expire after ~60s.

## What this protects
- The **username and password** are never sent in plaintext over the tunnel.
- Per-login **forward secrecy** (a compromised session later does not reveal the
  password used to log in).
- **Naive pubkey substitution** (when the key is pinned) and **replay** (via the
  nonce) are prevented.

## What this does NOT protect (important caveats)
- **The video stream is still plaintext.** The MJPEG `/stream` frames flow
  unencrypted over BORE. Anyone able to observe the tunnel traffic can watch the
  feed.
- **An active MITM that rewrites the served page.** Because the `/view` HTML +
  JS (including the pinned key constant and the bundled crypto) are delivered
  over plain HTTP, an attacker who can modify traffic in-flight could serve a
  tampered page that leaks credentials — pinning and JS-crypto are client-side
  checks delivered over the very channel being attacked. **This is the residual
  risk that only TLS can close.**

## The only complete fix: TLS
For a genuinely secure deployment, terminate TLS in front of the device:
- **Front BORE with an HTTPS reverse proxy** (e.g. a Cloudflare Tunnel or a small
  HTTPS relay) so the browser talks HTTPS end-to-end; or
- use an **HTTPS tunnel** where the hardware permits.

Note: running HTTPS **on the ESP32-CAM itself** (SELFHOST) is constrained — the
TLS handshake plus the camera can exceed available PSRAM and cause brownouts (see
the README). A reverse proxy in front of BORE avoids that while still giving you
TLS. Over HTTPS, the ECDH login becomes defense-in-depth; the channel itself
already protects everything, authenticates the server, and makes `crypto.subtle`
available.

## Endpoint summary
| Endpoint        | Auth            | Notes                                        |
|-----------------|-----------------|----------------------------------------------|
| `GET /pubkey`   | none            | Device X25519 public key (unpinned fallback) |
| `GET /nonce`    | none            | Single-use replay nonce (issue #12)          |
| `POST /login`   | none            | ECDH+AES-GCM encrypted creds + nonce → session |
| `GET /view`     | none (loads UI) | Serves the login form + viewer (bundled JS crypto) |
| `GET /stream`   | session         | MJPEG stream (sid cookie or `?token=`)       |
| `GET /capture`  | session         | Single JPEG                                  |
| `GET /flash`    | session         | Reader (`{"flash":0|1}`) / setter (`?s=1|0`) |
| `GET /health`   | none            | Heap/uptime only                             |
