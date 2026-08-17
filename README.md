# ESP32-CAM Cloud CCTV

A minimal, zero-router-configuration CCTV solution for the AI-Thinker ESP32-CAM.
An outbound TCP tunnel (BORE) exposes your local camera server to a public URL with no port-forwarding or ISP changes required.

---

## Credentials — What Goes Where

> This is the most important section. Read it before flashing.

| Credential | Where you set it | Where you use it |
|---|---|---|
| **Camera viewer login** | `firmware/config.h` — `CONFIG_HTTP_USER` / `CONFIG_HTTP_PASS` | Browser prompt when opening `/view` URL |
| **WiFi password** | **Not in code** — entered via captive portal on first boot | Saved automatically to ESP32 flash (NVS) |
| **Supabase API key** | `firmware/config.h` — `SUPABASE_ANON_KEY` | ESP32 uses it internally to upload motion frames |

### Camera viewer login (HTTP Basic Auth)
When you open your public camera URL in a browser, it will show a login prompt. Enter:
- **Username**: value of `CONFIG_HTTP_USER` in `config.h` (default: `admin`)
- **Password**: value of `CONFIG_HTTP_PASS` in `config.h` (default: `changeme12345678`)

> Change the default password before exposing your camera to the internet.

### WiFi credentials
WiFi is **never hardcoded**. Instead, on first boot the ESP32 sets up its own temporary hotspot named `ESP32-CAM-Setup`. You connect to it, fill in your home WiFi details on the popup webpage, and the board saves them permanently. See the **Quick Start** section below for the step-by-step.

---

## Features
- **JPEG-polling viewer** (`/view`) — works through any HTTP tunnel
- **Outbound tunnel** — BORE (`bore.pub`) by default, no port-forwarding needed
- **HTTP Basic Auth** — secures all camera endpoints
- **WiFiManager** — captive portal WiFi provisioning, no hardcoded credentials
- **Supabase cloud storage** — 200-frame circular motion buffer
- **PIR motion detection** — HC-SR501 on GPIO 13, 5-frame burst capture

---

## Prerequisites

### Hardware
- AI-Thinker ESP32-CAM (OV2640, PSRAM)
- FTDI/USB-TTL adapter (3.3V logic) — for initial flashing only
- Stable 5V ≥ 1A power supply (do NOT power from FTDI 5V pin — causes brownout resets)
- Optional: HC-SR501 PIR sensor for motion detection

### Arduino IDE Board Settings
| Setting | Value |
|---|---|
| Board | AI Thinker ESP32-CAM |
| PSRAM | Enabled |
| Partition Scheme | Custom (select `firmware/partitions.csv`) |
| Upload Speed | 115200 |
| Flash Mode | QIO |
| Flash Frequency | 80 MHz |

### Required Libraries (install via Library Manager)
| Library | Author | Purpose |
|---|---|---|
| `esp32-tunnel` | HamzaYslmn | BORE outbound tunnel |
| `WiFiManager` | tzapu | Captive portal WiFi setup |
| `ESPSupabase` | jhagas | Supabase storage upload |
| `ArduinoJson` | bblanchon | JSON for Supabase metadata |

---

## Quick Start & Setup

### Step 1 — Configure secrets
```
firmware/config.h   ← create this file (copy from config.example.h)
```
Open `config.example.h`, read every comment, then save a copy as `config.h` with your real values.
`config.h` is gitignored — it will never be committed.

The minimum you must change before going live:
```cpp
#define CONFIG_HTTP_PASS  "your-strong-unique-password"
```

### Step 2 — Flash
1. Wire FTDI to ESP32-CAM (see [docs/FLASHING.md](docs/FLASHING.md)). Pull GPIO 0 to GND.
2. Open `firmware/esp32-cam-cloud-cctv.ino` in Arduino IDE.
3. Select the board settings from the table above.
4. Click Upload.
5. Disconnect GPIO 0 from GND. Press RESET.

### Step 3 — Connect to WiFi (first boot only)
1. On your phone, open WiFi settings.
2. Connect to the hotspot named **`ESP32-CAM-Setup`**.
3. A webpage will appear automatically (captive portal).
4. Tap **Configure WiFi**, select your home network, enter its password.
5. The board saves the credentials and reboots. Done — it will connect automatically every time from now on.

> **Already connected?** If your board was previously used with other firmware on the same WiFi network, it may remember the credentials and skip straight to connecting — you'll see `AutoConnect: SUCCESS` in the Serial Monitor.

> **Behind a captive portal?** On networks that require a one-time browser login *after* joining Wi-Fi (hotel/hostel/campus/ISP hotspots), the firmware auto-detects the portal and serves a helper page at `http://<device-ip>/portal`. Enter the ISP portal username/password there and it logs in for you (field names are auto-detected, never hardcoded). Because hotspots authorise each device **by MAC address**, entering the credentials on `/portal` is what logs the **camera itself** in — logging in on your own phone/PC only grants *that* device (and often just shows "already logged in"). **MikroTik** hotspots that use a CHAP (MD5 challenge) login are handled automatically — the board hashes the password locally just like the portal's JavaScript would, and follows any landing-page redirect to reach the real login form. Portals with no form or JavaScript-only logins fall back to a "finish in your browser" link. Until the login succeeds, cloud uploads are **paused** and the Serial Monitor prints a clear step-by-step banner telling you exactly which URL to open; a connectivity heartbeat resumes uploads automatically once you are online. See [docs/CONFIG_SETUP.md](docs/CONFIG_SETUP.md#post-connect-captive-portal-login-issue-33).

---

## Viewing the Live Feed

1. Open the **Serial Monitor** in Arduino IDE (baud rate: `115200`).
2. After boot, wait ~5 seconds. In default BORE mode you will see:
   ```
   >>> TUNNEL CONNECTED! URL: http://bore.pub:XXXXX
   ```
3. Note the port number. With public `bore.pub`, it is assigned by the server and can change every reboot.
4. Open a browser and go to:
   ```
   http://bore.pub:XXXXX/view
   ```
   > ⚠️ You must use **`http://`** not `https://`. BORE is a plain TCP tunnel. If your browser forces HTTPS, open an Incognito/Private window and type the URL manually starting with `http://`.
5. Enter your credentials when prompted:
   - **Username**: `CONFIG_HTTP_USER` from `config.h`
   - **Password**: `CONFIG_HTTP_PASS` from `config.h`
6. The live camera feed will load at ~3-5 fps.

### Stable URL Option (Supabase Sync)

Public `bore.pub` is highly stable on the ESP32-CAM because it uses raw TCP (no TLS overhead). However, the port changes every reboot. We cannot use `CONFIG_TUNNEL_MODE_SELFHOST` on the ESP32-CAM because the HTTPS handshake required by most tunnel servers (like Render/Localtunnel) exceeds the ESP32-CAM's available PSRAM when the camera is running, leading to brownouts and connection hangs.

To solve this, the firmware now automatically syncs the latest BORE URL to your Supabase project.

1. In Supabase, run this SQL query to create the tracking table:
   ```sql
   CREATE TABLE camera_status (
       id SERIAL PRIMARY KEY,
       url TEXT NOT NULL,
       created_at TIMESTAMP WITH TIME ZONE DEFAULT timezone('utc'::text, now()) NOT NULL
   );
   ALTER TABLE camera_status ENABLE ROW LEVEL SECURITY;
   CREATE POLICY "Allow public read" ON camera_status FOR SELECT USING (true);
   CREATE POLICY "Allow public insert" ON camera_status FOR INSERT WITH CHECK (true);
   ```
2. Now, instead of checking the Serial Monitor, just bookmark this REST API URL:
   `https://<YOUR_SUPABASE_ID>.supabase.co/rest/v1/camera_status?select=url,created_at&order=id.desc&limit=1&apikey=<YOUR_ANON_KEY>`
3. Whenever you click it, you will see the latest URL in JSON format. Just copy and paste it into your browser!

### Endpoints
| URL | Auth | Description |
|---|---|---|
| `/view` | ✅ Required | **Use this** — JPEG-polling HTML viewer |
| `/capture` | ✅ Required | Single JPEG snapshot |
| `/stream` | ✅ Required | Raw MJPEG (LAN only — hangs through BORE tunnel) |
| `/health` | ❌ None | JSON uptime and free heap |

---

## Motion Capture Verification (Supabase + PIR)

1. Complete [docs/SUPABASE_SETUP.md](docs/SUPABASE_SETUP.md) first.
2. Wire PIR sensor as shown in [docs/WIRING.md](docs/WIRING.md).
3. Set `ENABLE_PIR_MOTION 1` in `config.h`.
4. Wave your hand in front of the PIR sensor.
5. Serial Monitor should log:
   ```
   Motion detected! Capturing frame 1 of 5
   Uploading events/frame_0.jpg ...
   Upload successful!
   ```
6. Check **Supabase Dashboard → Storage → cctv-clips → events/** for uploaded frames.

---

## Known Behaviours

| Behaviour | Explanation |
|---|---|
| Port changes every reboot | Expected in default BORE mode. `bore.pub` assigns a random public port; use `CONFIG_TUNNEL_MODE_SELFHOST` for a stable URL. |
| Is BORE public facing? | Yes. `http://bore.pub:XXXXX/view` is reachable from the internet while the tunnel is connected. HTTP Basic Auth is your protection. |
| LAN vs remote access | The bore URL is for access from anywhere. On the same WiFi/LAN, use the ESP32's local IP for lower latency and fewer relay resets. |
| `No core dump partition found` on boot | Normal — we removed the unused core dump partition to save flash space. |
| `gpio_install_isr_service already installed` | Normal — the camera driver installs the ISR first; Arduino re-installs it for PIR. Harmless. |
| Camera not found on reboot | Check the camera ribbon cable — the latch is fragile. Reseat it firmly. |
| Brownout reset (`rst:0x1`) | Power issue — do not power ESP32-CAM from FTDI. Use a dedicated 5V ≥ 1A supply. |

---

## Documentation
- [docs/FLASHING.md](docs/FLASHING.md) — FTDI wiring and Arduino IDE settings
- [docs/SUPABASE_SETUP.md](docs/SUPABASE_SETUP.md) — Supabase bucket, RLS policies, metadata table
- [docs/WIRING.md](docs/WIRING.md) — PIR sensor wiring to ESP32-CAM GPIO 13
