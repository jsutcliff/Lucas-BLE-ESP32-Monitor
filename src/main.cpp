// esp-lucas - BLE battery monitor for LiFePO4 packs with a bolt-on BMS.
//
// Polls each configured pack over BLE in turn and serves a dashboard on the
// local network. Everything is configured from the web UI; the serial console
// here is a small maintenance shell.

#include <Arduino.h>
#include <NimBLEDevice.h>

#include "blelink.h"
#include "fleet.h"
#include "netcfg.h"
#include "packstore.h"
#include "poller.h"
#include "util.h"
#include "webui.h"

static void printHelp() {
    Serial.println(R"(
  status                   packs, poll counters and the dashboard URL
  scan [secs]              list nearby BLE devices (pauses polling)
  packs                    the saved device list

  fleet start | fleet stop pause or resume polling
  fleet <secs>             seconds per polling cycle

  wifi                     network status and the dashboard URL
  wifi portal              raise the WiFi setup access point again
  wifi forget              erase the saved network and reboot into setup

  log on | log off         print every decoded frame as it arrives
  reboot
  help)");
}

// Splits off the first whitespace-delimited word; `line` is advanced past it.
static String nextWord(String& line) {
    line.trim();
    int space = line.indexOf(' ');
    if (space < 0) {
        String w = line;
        line = "";
        return w;
    }
    String w = line.substring(0, space);
    line = line.substring(space + 1);
    line.trim();
    return w;
}

static void doScan(const String& arg) {
    uint32_t secs = arg.isEmpty() ? 8 : (uint32_t)arg.toInt();
    Serial.printf("scanning %us...\n", (unsigned)secs);
    fleet::requestScan(secs);

    // The poller task owns the radio, so the scan happens there; wait it out.
    uint32_t deadline = millis() + (secs + 15) * 1000;
    while (millis() < deadline) {
        delay(200);
        if (fleet::lastScanMs() && !fleet::scanning()) break;
    }

    Serial.println("  address            rssi  name");
    for (size_t i = 0; i < blelink::seenCount(); i++) {
        String mac, name;
        int rssi = 0;
        bool likely = false;
        if (!blelink::seenAt(i, &mac, &name, &rssi, &likely)) continue;
        Serial.printf("  %s  %4d  %s%s\n", mac.c_str(), rssi, name.c_str(),
                      likely ? "   (speaks a serial-over-BLE profile)" : "");
    }
    Serial.println("add devices from the dashboard's Devices tab");
}

static void handleCommand(String line) {
    util::OutGuard guard;
    String cmd = nextWord(line);
    if (cmd.isEmpty()) return;
    cmd.toLowerCase();

    if (cmd == "help" || cmd == "?") {
        printHelp();
    } else if (cmd == "status") {
        fleet::printStatus();
        netcfg::status();
    } else if (cmd == "scan") {
        doScan(line);
    } else if (cmd == "packs") {
        packstore::print();
    } else if (cmd == "fleet") {
        if (line.isEmpty()) fleet::printStatus();
        else if (line.equalsIgnoreCase("start")) fleet::start();
        else if (line.equalsIgnoreCase("stop")) fleet::stop();
        else if (isdigit((int)line[0])) fleet::setCycleSeconds((uint32_t)line.toInt());
        else Serial.println("usage: fleet [start|stop|<secs>]");
    } else if (cmd == "wifi") {
        if (line.isEmpty()) netcfg::status();
        else if (line.equalsIgnoreCase("portal")) netcfg::openPortal();
        else if (line.equalsIgnoreCase("forget")) netcfg::forget();
        else Serial.println("usage: wifi [portal|forget]");
    } else if (cmd == "log") {
        bool on = !line.equalsIgnoreCase("off");
        poller::setQuiet(!on);
        Serial.printf("frame logging %s\n", on ? "on" : "off");
    } else if (cmd == "reboot") {
        Serial.println("rebooting");
        delay(200);
        ESP.restart();
    } else {
        Serial.printf("unknown command \"%s\" - try `help`\n", cmd.c_str());
    }
}

void setup() {
    // A roomy TX ring: BLE and WiFi both stall the CPU in bursts, and a full
    // ring is what turns interleaved writes into corrupted output.
    Serial.setTxBufferSize(4096);
    Serial.begin(115200);
    util::begin();
    delay(300);
    Serial.println("\n  esp-lucas :: BLE battery monitor\n  type `help` for commands\n");

    blelink::begin();
    poller::begin();
    packstore::begin();
    fleet::begin();
    netcfg::begin();
    webui::begin();
    fleet::start();

    Serial.print("> ");
}

void loop() {
    poller::loop();
    netcfg::loop();
    webui::loop();

    static String line;
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\r') continue;
        if (c == '\n') {
            Serial.println();
            handleCommand(line);
            line = "";
            Serial.print("> ");
        } else if (c == 8 || c == 127) {  // backspace
            if (line.length()) {
                line.remove(line.length() - 1);
                Serial.print("\b \b");
            }
        } else if (line.length() < 250) {
            line += c;
            Serial.print(c);  // local echo, so typed commands show in the log
        }
    }
    delay(5);
}
