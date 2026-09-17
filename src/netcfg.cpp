#include "netcfg.h"

#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <WiFiManager.h>

#include "config.h"
#include "util.h"

namespace netcfg {

static WiFiManager s_wm;
static bool s_portalOpen = false;
static bool s_announced = false;
static bool s_mdnsUp = false;
static bool s_otaBusy = false;

static void startMdnsAndOta() {
    if (s_mdnsUp) return;
    if (MDNS.begin(MDNS_HOSTNAME)) {
        MDNS.addService("http", "tcp", 80);
        s_mdnsUp = true;
    }
    ArduinoOTA.setHostname(MDNS_HOSTNAME);
    ArduinoOTA.onStart([]() {
        s_otaBusy = true;
        Serial.println("\nOTA: update starting - pausing the BLE poller");
    });
    ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
        static int lastPct = -1;
        int pct = total ? (int)(done * 100 / total) : 0;
        if (pct != lastPct && pct % 10 == 0) {
            Serial.printf("OTA: %d%%\n", pct);
            lastPct = pct;
        }
    });
    ArduinoOTA.onEnd([]() { Serial.println("OTA: done, rebooting"); });
    ArduinoOTA.onError([](ota_error_t e) {
        s_otaBusy = false;
        Serial.printf("OTA: failed (%u)\n", e);
    });
    ArduinoOTA.begin();
}

void begin() {
    WiFi.mode(WIFI_STA);
    // WiFi modem sleep is mandatory when BLE is also up - the ESP32 aborts at
    // boot without it, because the two stacks time-share one radio.
    WiFi.setSleep(true);

    s_wm.setConfigPortalBlocking(false);  // keep polling BLE while the portal is up
    s_wm.setConfigPortalTimeout(0);       // leave it up until someone uses it
    s_wm.setDarkMode(true);
    s_wm.setTitle("Lucas pack monitor");

    Serial.println("wifi: connecting to the saved network...");
    if (s_wm.autoConnect(SETUP_AP_SSID, SETUP_AP_PASS)) {
        Serial.printf("wifi: connected to \"%s\"\n", WiFi.SSID().c_str());
    } else {
        s_portalOpen = true;
        Serial.printf(
            "wifi: no saved network - setup AP is up.\n"
            "      join \"%s\" (password \"%s\") and a captive portal will open,\n"
            "      or browse to http://192.168.4.1/\n",
            SETUP_AP_SSID, SETUP_AP_PASS);
    }
}

void loop() {
    if (s_portalOpen) s_wm.process();

    if (WiFi.status() == WL_CONNECTED) {
        if (!s_announced) {
            s_announced = true;
            s_portalOpen = false;
            startMdnsAndOta();
            Serial.printf("\nwifi: %s   dashboard at http://%s/  (or http://%s.local/)\n\n",
                          WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(),
                          MDNS_HOSTNAME);
        }
        ArduinoOTA.handle();
        return;
    }
    s_announced = false;
}

bool connected() { return WiFi.status() == WL_CONNECTED; }
String ip() { return connected() ? WiFi.localIP().toString() : String("-"); }
int rssi() { return connected() ? WiFi.RSSI() : 0; }
String ssid() { return connected() ? WiFi.SSID() : String(""); }
bool otaInProgress() { return s_otaBusy; }
void setOtaBusy(bool busy) { s_otaBusy = busy; }

void openPortal() {
    s_portalOpen = true;
    s_wm.startConfigPortal(SETUP_AP_SSID, SETUP_AP_PASS);
    Serial.printf("wifi: setup AP \"%s\" is up at http://192.168.4.1/\n", SETUP_AP_SSID);
}

void forget() {
    s_wm.resetSettings();
    Serial.println("wifi: saved network erased - rebooting into setup mode");
    delay(300);
    ESP.restart();
}

void status() {
    util::OutGuard guard;
    Serial.printf("wifi:  %s", connected() ? "connected" : "disconnected");
    if (connected()) {
        Serial.printf("  ssid=\"%s\"  ip=%s  rssi=%d dBm  http://%s.local/",
                      WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(),
                      (int)WiFi.RSSI(), MDNS_HOSTNAME);
    } else if (s_portalOpen) {
        Serial.printf("  setup AP \"%s\" is up at http://192.168.4.1/", SETUP_AP_SSID);
    }
    Serial.println();
}

}  // namespace netcfg
