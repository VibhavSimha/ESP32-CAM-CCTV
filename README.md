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

## Quick Start
1. Copy `config.example.h` to `config.h` and update credentials (DO NOT commit `config.h`).
2. Open `firmware/esp32-cam-cloud-cctv.ino` in Arduino IDE.
3. Select board: `AI Thinker ESP32-CAM` (Enable PSRAM).
4. Select Partition Scheme: `Custom` (Ensure `partitions.csv` is used).
5. Flash the board.
6. Connect to the `ESP32-CAM-Setup` WiFi network to configure your home WiFi credentials.
7. Check the Serial monitor for the tunnel URL to view the live stream over the internet.

## Documentation
- [Flashing Guide](docs/FLASHING.md)
- [Supabase Setup](docs/SUPABASE_SETUP.md)
- [Wiring Guide](docs/WIRING.md)
