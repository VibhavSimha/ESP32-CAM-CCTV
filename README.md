# ESP32-CAM Cloud CCTV

This project is a minimal, zero-router-configuration CCTV solution for the AI-Thinker ESP32-CAM.
It utilizes an outbound WebSocket tunnel (`localtunnel` via `esp32-tunnel`) to proxy a local MJPEG stream to a public URL, ensuring no port forwarding or ISP changes are required.

## Features
- **Minimal MJPEG Stream**: Stripped down `esp_http_server` running on port 80.
- **Outbound Tunnel**: Secure remote access via `localtunnel`.
- **HTTP Basic Auth**: Secures the stream and endpoints.
- **Cloud Storage**: Uploads motion-triggered clips to Supabase.
- **PIR Motion Detection**: Hardware interrupt-driven burst capture (GPIO 13).

## Prerequisites
- AI-Thinker ESP32-CAM (OV2640, PSRAM)
- Arduino IDE 2.x
- ESP32 board package (≥ 3.0.x)

## Required Libraries
Install these via the Arduino Library Manager:
- `esp32-tunnel` (by HamzaYslmn)
- `WiFiManager` (by tzapu)
- `ESPSupabase` (by jhagas)
- `ArduinoJson`

## Quick Start & Setup
1. Copy `config.example.h` to `config.h` and update credentials (DO NOT commit `config.h`).
2. Open `firmware/esp32-cam-cloud-cctv.ino` in Arduino IDE.
3. Select board: `AI Thinker ESP32-CAM` (Enable PSRAM).
4. Select Partition Scheme: `Custom` (Ensure `partitions.csv` is used).
5. Flash the board.
6. **Connect to the Captive Portal (WiFi Setup)**: We use `WiFiManager` to avoid hardcoding WiFi passwords into your code. 
   - When the ESP32 boots for the first time, it will host its own WiFi network named `ESP32-CAM-Setup`. 
   - Connect to this network using your phone or PC. A captive portal page will automatically pop up.
   - Click "Configure WiFi", select your home WiFi network, and enter your password.
   - The ESP32 will save these credentials to its permanent memory (NVS) and reboot.
   - *Note: If you have flashed this specific ESP32 board before with other projects, it might already remember your WiFi credentials from its permanent memory and connect automatically (skipping the portal).*

## Verification & Testing (Next Steps)
Now that your ESP32-CAM is flashed and connected to WiFi, here is how you verify each feature:

### 1. Verify the Tunnel (Internet Live Stream)
- Open the Serial Monitor in the Arduino IDE (baud rate: 115200).
- Wait a few seconds for the tunnel to connect. You will see a line print out: `Tunnel URL: https://<your-subdomain>.loca.lt`
- **Disconnect your phone from your home WiFi** (so you are using mobile data) to prove it works over the internet.
- Open that `loca.lt` URL in your phone's browser, appending `/stream` to the end (e.g. `https://cctv-dev-local.loca.lt/stream`).
- A prompt will ask for a username and password. Enter the ones you defined in `config.h` (default: `admin` / `changeme12345678`).
- You should now see the live video feed!

### 2. Verify Motion Capture (Supabase)
- Wave your hand in front of the PIR sensor.
- Check the Serial Monitor. You should see logs indicating motion was detected and frames are uploading.
- Go to your Supabase project dashboard -> **Storage** -> `cctv-clips` bucket.
- Open the `events/` folder. You should see `frame_0.jpg` through `frame_4.jpg` uploaded.

## Documentation
- [Flashing Guide](docs/FLASHING.md)
- [Supabase Setup](docs/SUPABASE_SETUP.md)
- [Wiring Guide](docs/WIRING.md)
