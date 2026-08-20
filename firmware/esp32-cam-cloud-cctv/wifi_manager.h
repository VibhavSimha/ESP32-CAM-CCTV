#pragma once

// Connect to Wi-Fi via WiFiManager. Applies MAC spoofing first when
// CAPTIVE_MAC_SPOOF is set to a non-empty string in config.h (issue #50).
void setupWiFiManager();
