# Flashing the ESP32-CAM

## Hardware Setup
To flash the AI-Thinker ESP32-CAM, you need an FTDI/USB-TTL adapter.
- **3.3V Logic**: Ensure your FTDI is set to 3.3V logic.
- **Power**: Use a stable 5V power supply.

## Wiring for Flash Mode
| FTDI | ESP32-CAM |
|------|-----------|
| 5V   | 5V        |
| GND  | GND       |
| TX   | U0R       |
| RX   | U0T       |
| GND  | GPIO 0 (BOOT) |

**Important:** GPIO 0 must be connected to GND during boot to enter flashing mode.
After flashing, disconnect GPIO 0 from GND and press the RESET button on the board to run the firmware.

## IDE Settings
- **Board**: AI Thinker ESP32-CAM
- **PSRAM**: Enabled
- **Partition Scheme**: Custom (uses the `partitions.csv` in the firmware folder)
- **Upload Speed**: 115200
- **Flash Mode**: QIO
- **Flash Frequency**: 80 MHz
