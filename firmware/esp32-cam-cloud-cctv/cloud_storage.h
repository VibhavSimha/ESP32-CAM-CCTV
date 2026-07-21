#pragma once

#include <Arduino.h>

void setupCloudStorage();
void uploadFrameToCloud();
void publishTunnelUrl(String url);
