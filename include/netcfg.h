#pragma once
#include <Arduino.h>

// WiFi onboarding and over-the-air updates.
//
// On first boot (or when the saved network is gone) the board raises its own
// access point and serves a captive portal to pick a network - no credentials
// ever live in the repo. Once it's on the LAN it accepts OTA firmware uploads,
// both from PlatformIO (`pio run -e ota -t upload`) and from the dashboard's
// own upload form.
namespace netcfg {

void begin();
void loop();

bool connected();
String ip();
int rssi();
String ssid();

void openPortal();  // force the setup AP back up
void forget();      // erase the saved network and reopen the portal
void status();

// Set by the OTA handler so the poller can stand down mid-flash.
bool otaInProgress();
void setOtaBusy(bool busy);  // web-upload OTA tells the poller to stand down

}  // namespace netcfg
