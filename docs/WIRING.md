# Wiring Guide

## PIR Motion Sensor (HC-SR501)

The PIR motion sensor is used for triggering the camera to capture a burst of frames.

| HC-SR501 Pin | ESP32-CAM Pin | Note |
|--------------|---------------|------|
| VCC          | 5V            | Requires 5V to function reliably |
| GND          | GND           | Common ground |
| OUT          | GPIO 13       | Avoid using GPIO 12/15 (boot constraints) and GPIO 0 |

### Notes
- Ensure the PIR sensor is placed away from the camera's flash LED to prevent false triggers if the LED is used.
- Adjust the sensitivity and time delay potentiometers on the HC-SR501 module for optimal performance. The ESP32 code provides a 10s software debounce, but hardware tuning is also recommended.
