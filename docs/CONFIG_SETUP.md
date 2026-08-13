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

## Related
- [docs/SECURITY.md](SECURITY.md) — encrypted-login threat model.
- [docs/SUPABASE_SETUP.md](SUPABASE_SETUP.md) — bucket + credentials.
- [docs/SUPABASE_RETENTION.md](SUPABASE_RETENTION.md) — server-side FIFO cap.

---

## ISP captive-portal login (post-connect, optional)

This is separate from both the WiFiManager SSID/password step and the encrypted
`/view` viewer login. Some networks join Wi-Fi successfully but still hold
internet access behind an ISP/venue browser login page.

- **When it runs:** only *after* Wi-Fi is connected. On boot the firmware probes
  `http://connectivitycheck.gstatic.com/generate_204`. A `204` means real
  internet (no portal → step skipped). A `200`/`3xx` interception means a captive
  portal is present. A transport error is treated as *offline*, **not** captive,
  so a temporarily-offline device is never falsely trapped.
- **How the user logs in:** while a portal is pending, opening the device root
  (`http://<device-ip>/`) redirects the browser to the built-in `/portal` helper
  page **once**. For a simple HTML form the ESP32 submits the entered
  username/password itself; portals needing JavaScript/tokens/device binding must
  be completed manually in the browser.
- **One-time behavior:** after login restores connectivity, the root link stops
  redirecting to `/portal` and forwards to `/view` for the rest of the session.
  Only the Wi-Fi credentials persist — ISP portal credentials are **not** stored.
