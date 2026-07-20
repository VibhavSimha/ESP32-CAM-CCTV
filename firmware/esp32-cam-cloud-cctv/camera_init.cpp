#include "camera_init.h"

#include "camera_pins.h"

#include "esp32-hal-ledc.h"
#include "esp_camera.h"

static int ledDuty = 0;
static bool ledStreaming = false;

static void enableLed(bool on) {
#if defined(LED_GPIO_NUM)
  int duty = on ? ledDuty : 0;
  if (on && ledStreaming && ledDuty > 255) {
    duty = 255;
  }
  ledcWrite(LED_GPIO_NUM, duty);
#endif
}

esp_err_t initCamera() {
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
    config.jpeg_quality = 10;
    config.fb_count = 2;
    config.grab_mode = CAMERA_GRAB_LATEST;
  } else {
    config.frame_size = FRAMESIZE_SVGA;
    config.fb_location = CAMERA_FB_IN_DRAM;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    return err;
  }

  sensor_t *sensor = esp_camera_sensor_get();
  if (sensor && config.pixel_format == PIXFORMAT_JPEG) {
    sensor->set_framesize(sensor, FRAMESIZE_QVGA);
  }

#if defined(LED_GPIO_NUM)
  setupLedFlash(LED_GPIO_NUM);
#endif

  return ESP_OK;
}

void setupLedFlash(int pin) {
  ledcAttach(pin, 5000, 8);
  ledDuty = 0;
}

void cameraSetStreaming(bool streaming) {
  ledStreaming = streaming;
}

void cameraLedForCapture(bool on) {
  enableLed(on);
}
