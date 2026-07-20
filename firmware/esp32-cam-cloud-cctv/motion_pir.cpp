#include "motion_pir.h"
#include "config.h"
#include "cloud_storage.h"
#include <Arduino.h>

static unsigned long lastMotionTime = 0;
static bool burstActive = false;
static int burstCount = 0;
static unsigned long lastBurstTime = 0;

void IRAM_ATTR pirInterrupt() {
    // Only detect rising edge and respect debounce
    if (millis() - lastMotionTime > PIR_DEBOUNCE_MS) {
        lastMotionTime = millis();
        burstActive = true;
        burstCount = 0;
        lastBurstTime = 0;
    }
}

void setupPIR() {
#if ENABLE_PIR_MOTION
    pinMode(PIR_GPIO, INPUT_PULLDOWN);
    attachInterrupt(digitalPinToInterrupt(PIR_GPIO), pirInterrupt, RISING);
    Serial.println("PIR motion detection enabled on GPIO " + String(PIR_GPIO));
#endif
}

void loopPIR() {
#if ENABLE_PIR_MOTION
    if (burstActive) {
        if (millis() - lastBurstTime >= MOTION_BURST_INTERVAL_MS) {
            lastBurstTime = millis();
            
            Serial.printf("Motion detected! Capturing frame %d of %d\n", burstCount + 1, MOTION_BURST_COUNT);
            uploadFrameToCloud();
            
            burstCount++;
            if (burstCount >= MOTION_BURST_COUNT) {
                burstActive = false;
                Serial.println("Motion burst complete");
            }
        }
    }
#endif
}
