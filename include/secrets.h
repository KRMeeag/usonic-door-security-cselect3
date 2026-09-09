#pragma once
// ---------------------------------------------------------------------------
// Local secrets — fill in and rebuild. Keep the auth token private.
//
// The three BLYNK_* defines come from the Blynk console (Device Info tab) and
// MUST be visible before <BlynkSimpleEsp32.h> is included, which is why they
// live here and secrets.h is included first in main.cpp.
// ---------------------------------------------------------------------------

#define BLYNK_TEMPLATE_ID   "TMPLxxxxxxxx"
#define BLYNK_TEMPLATE_NAME  "Door Intrusion Detection"
#define BLYNK_AUTH_TOKEN     "PASTE-DEVICE-AUTH-TOKEN"

// 2.4 GHz only — the ESP32 has no 5 GHz radio. Campus WiFi is often 5 GHz or a
// captive portal, so test on a phone hotspot and bring one to the demo.
#define WIFI_SSID  "your-2G4-ssid"
#define WIFI_PASS  "your-wifi-password"
