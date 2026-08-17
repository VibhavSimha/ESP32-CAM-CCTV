# config.h setup — first flash & one-time device-key pinning

This is the only file you edit before flashing: copy `config.example.h` to
`config.h` and fill in your values. Below is the full first-time workflow,
including the **one-time** device public-key pinning that hardens the encrypted
login against a man-in-the-middle (MITM) on the plain-HTTP BORE tunnel.

> TL;DR: flash once with `CONFIG_DEVICE_PUBKEY_B64 ""`, copy the pubkey the
> device prints on the serial console, paste it back into `config.h`, re-flash
> once. Done forever (the key is stored in NVS and survives reboots).

---

## 1. Create your `config.h`
```bash
cd firmware/esp32-cam-cloud-cctv
cp config.example.h config.h
```

## 2. Set the basics in `config.h`
- `CONFIG_HTTP_USER` / `CONFIG_HTTP_PASS` — your viewer login (use a strong,
  unique 16+ char password; never leave `changeme...`).
- `SUPABASE_URL`, `SUPABASE_ANON_KEY`, `SUPABASE_BUCKET` — from
  Supabase → Project Settings → API (use the **anon** key, never `service_role`).
- Leave `CONFIG_DEVICE_PUBKEY_B64 ""` **empty for now** (step 4 fills it).
- WiFi is NOT set here — it's provisioned via the `ESP32-CAM-Setup` captive
  portal on first boot.

## 3. First flash (unpinned)
Flash the firmware and open the Serial Monitor (115200 baud). At boot you'll see:
```
[Crypto] Device pubkey (b64): aMIu0mUdA6RprawE5sIVc8O4Kl3gMBhwXwACtOdWjSs=
[Crypto] CONFIG_DEVICE_PUBKEY_B64 is empty — pin the value above and re-flash (see docs/CONFIG_SETUP.md).
```
At this point login already works, but the `/view` page shows a ⚠ banner:
> ⚠ Device key not pinned — login is NOT protected against MITM. Set
> CONFIG_DEVICE_PUBKEY_B64 in config.h to the value below and re-flash.
> Device key: `aMIu0mUdA6Rpr…`

The banner value and the serial value are the **same** key — that's your
out-of-band verification.

## 4. Pin the key (the one-time step)
Copy the exact base64 string from the serial line and paste it into `config.h`:
```c
#define CONFIG_DEVICE_PUBKEY_B64 "aMIu0mUdA6RprawE5sIVc8O4Kl3gMBhwXwACtOdWjSs="
```

## 5. Re-flash once
Flash again. Now at boot you should see:
```
[Crypto] Loaded persisted X25519 keypair from NVS
[Crypto] Device pubkey (b64): aMIu0mUdA6RprawE5sIVc8O4Kl3gMBhwXwACtOdWjSs=
[Crypto] Pinned CONFIG_DEVICE_PUBKEY_B64 MATCHES device key. Good.
```
The ⚠ banner is gone, and the browser now encrypts the login **directly to the
pinned key** — it no longer trusts `/pubkey`. An attacker who substitutes a
different key can no longer decrypt your login (the device simply can't decrypt
what was encrypted to the wrong key → login aborts).

---

## Why this is a one-off (per device)
The X25519 keypair is generated **once on first boot** and persisted in NVS
(`Preferences`, namespace `crypto`). It is reused across every reboot and every
new BORE URL. You therefore pin **once per physical device**. You only need to
repeat this if you:
- erase NVS / do a full flash-erase, or
- move `config.h` to a **different** board (each board has its own key).

## Verifying the pin later
The pinned value in `config.h` must equal the serial `\[Crypto\] Device pubkey
(b64): …` line. The firmware checks this at boot and prints either
`… MATCHES device key. Good.` or a `WARNING: … does NOT match …`. If it warns,
re-copy the printed value into `config.h` and re-flash.

## What pinning does and does NOT protect
- ✅ Prevents an attacker from substituting their **own** device key to capture
  the login (naive pubkey-substitution MITM).
- ✅ Combined with the single-use `/nonce`, prevents replay of a captured login.
- ⚠ Does **not** fully stop an active MITM who rewrites the served `/view`
  HTML/JS itself (including the pinned constant) — because that page is
  delivered over plain HTTP. The only complete fix is TLS. See
  [docs/SECURITY.md](SECURITY.md) for the full threat model and the HTTPS
  reverse-proxy recommendation.

## Post-connect captive-portal login (issue #33)
This is **separate** from the `ESP32-CAM-Setup` onboarding above (which chooses
your router SSID/password and is unchanged). Some networks let the board join
Wi-Fi but still gate the *internet* behind a one-time browser login page (hotel /
hostel / campus / ISP hotspots). When `ENABLE_CAPTIVE_PORTAL_LOGIN` is `1`
(default), the firmware handles this **after** Wi-Fi is already connected:

1. It probes `CAPTIVE_PROBE_URL` (a plain-HTTP `generate_204` endpoint). A clean
   network returns `204` and nothing happens.
2. If a captive portal intercepts the probe, the firmware fetches the portal
   login page and **auto-detects** the username and password fields — the field
   names are **not** hardcoded, so it adapts to different ISPs (it uses the first
   text-like input as the username and the `type=password` input as the
   password, echoing any hidden fields on submit). **MikroTik** hotspots that use
   a CHAP (MD5 challenge) login are handled too: the board reads the page's
   `chap-id`/`chap-challenge`, hashes the password locally as
   `MD5(chap-id + password + chap-challenge)` — exactly like the portal's own
   JavaScript — and submits that, so the plaintext password never leaves the
   board (issue #42).
3. Open `http://<device-ip>/portal` in a browser on the same network. Enter the
   ISP portal username/password; the board submits the login for you and
   re-checks connectivity.

### Connectivity heartbeat gates the cloud (issue #40)
Behind a captive portal the board is *joined* to Wi-Fi but the internet is still
blocked, so Supabase uploads and the remote tunnel would otherwise fail on every
attempt (`HTTP 0`) and flood the serial log. To prevent that, the firmware only
talks to Supabase **after** the connectivity heartbeat has confirmed the internet
is reachable:

- While a captive portal (or an unreachable probe) is detected, background cloud
  uploads are **paused** and the serial log prints a clear, step-by-step banner
  telling you exactly which URL to open (`http://<device-ip>/portal`).
- The heartbeat re-probes every `CAPTIVE_PERIODIC_REPROBE_MS` (30 s). As soon as
  it detects the portal has been cleared — via the `/portal` helper **or** a
  manual browser login on any device — it logs `Cloud uploads resume` and the
  autonomous uploader starts again automatically, no reboot required.

### Configuration knobs (`config.h`)
| Macro | Default | Purpose |
| --- | --- | --- |
| `ENABLE_CAPTIVE_PORTAL_LOGIN` | `1` | Set to `0` to disable the whole feature (cloud uploads are then never gated). |
| `CAPTIVE_PROBE_URL` | `http://connectivitycheck.gstatic.com/generate_204` | Plain-HTTP 204 probe used to detect interception. |
| `CAPTIVE_PROBE_TIMEOUT_MS` | `6000` | Per-request timeout for the probe/submit. |
| `CAPTIVE_MAX_LOGIN_ATTEMPTS` | `3` | Attempts before steering the user to the manual browser fallback. |
| `CAPTIVE_PERIODIC_REPROBE_MS` | `30000` | Heartbeat cadence while **offline** (waiting for the portal login). |
| `CAPTIVE_ONLINE_HEARTBEAT_MS` | `60000` | Heartbeat cadence while **online** (to catch a portal that re-appears). |
| `CAPTIVE_LOG_PORTAL_PAGE` | `1` | Dump the full fetched portal login page to the serial console when a portal is detected (diagnostic — see the exact HTML/fields to parse). Set to `0` to silence. |

### Scope & fallback
- **Persistence:** only the Wi-Fi credentials (WiFiManager/NVS) plus a lightweight
  "portal seen" marker are stored. The ISP portal username/password are used once
  and **never** persisted.
- **Unsupported portals:** pages with no `<form>`, JavaScript-only portals, or
  challenge/response logins other than MikroTik CHAP are detected and the device
  shows a link to open the real portal page and finish the login manually.
  (MikroTik CHAP logins **are** automated — see step 2 above.) When a portal is
  detected the firmware also prints the **full fetched login page** to the serial
  console (`CAPTIVE_LOG_PORTAL_PAGE`, default on) so you can see the exact
  HTML/fields it received and report them.
- **Recovery:** if login fails or the network changes, the device stays reachable
  through its own `ESP32-CAM-Setup` AP and `/portal` page so you can retry
  without re-flashing.

## Related
- [docs/SECURITY.md](SECURITY.md) — encrypted-login threat model.
- [docs/SUPABASE_SETUP.md](SUPABASE_SETUP.md) — bucket + credentials.
- [docs/SUPABASE_RETENTION.md](SUPABASE_RETENTION.md) — server-side FIFO cap.
