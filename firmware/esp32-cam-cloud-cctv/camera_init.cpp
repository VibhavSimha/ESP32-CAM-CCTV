#include "camera_init.h"
#include <Arduino.h>
#include "camera_pins.h"

#include "esp_camera.h"

// The flash LED (LED_GPIO_NUM / GPIO4 on the AI-Thinker board) is driven with a
// single, plain-digital mechanism throughout the firmware. An earlier version
// attached this pin to an LEDC PWM channel here while stream_server.cpp drove
// the very same pin with pinMode()/digitalWrite(); the two owners fought over
// the pad, producing a glitchy LED and transient instability whenever the flash
// was toggled during a stream. Using digital output everywhere removes that
// contention.
static void enableLed(bool on) {
#if defined(LED_GPIO_NUM)
  digitalWrite(LED_GPIO_NUM, on ? HIGH : LOW);
#endif
}

esp_err_t initCamera() {
  Serial.println("[Camera] Initializing...");
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.frame_size = FRAMESIZE_UXGA;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 12;
  config.fb_count = 1;

  if (psramFound()) {
    Serial.println("[Camera] PSRAM found — using GRAB_LATEST, 2 frame buffers, quality 10");
    config.jpeg_quality = 10;
    config.fb_count = 2;
    config.grab_mode = CAMERA_GRAB_LATEST;
  } else {
    Serial.println("[Camera] WARNING: No PSRAM — falling back to SVGA, DRAM, 1 buffer");
    config.frame_size = FRAMESIZE_SVGA;
    config.fb_location = CAMERA_FB_IN_DRAM;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[Camera] ERROR: esp_camera_init failed: 0x%x\n", err);
    return err;
  }
  Serial.println("[Camera] esp_camera_init OK");

  sensor_t *sensor = esp_camera_sensor_get();
  if (sensor && config.pixel_format == PIXFORMAT_JPEG) {
    sensor->set_framesize(sensor, FRAMESIZE_QVGA);
    Serial.printf("[Camera] Sensor PID: 0x%02X | Frame size set to QVGA (320x240)\n", sensor->id.PID);
  }

#if defined(LED_GPIO_NUM)
  setupLedFlash(LED_GPIO_NUM);
  Serial.printf("[Camera] LED flash configured on GPIO %d\n", LED_GPIO_NUM);
#endif

  Serial.printf("[Camera] Init complete. Free heap: %u\n", ESP.getFreeHeap());
  return ESP_OK;
}

void setupLedFlash(int pin) {
  pinMode(pin, OUTPUT);
  digitalWrite(pin, LOW);
}

void cameraSetStreaming(bool streaming) {
  (void)streaming;
}

void cameraLedForCapture(bool on) {
  enableLed(on);
}
