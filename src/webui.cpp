#include "webui.h"

#include <Update.h>
#include <WebServer.h>

#include "config.h"
#include "dashboard_html.h"
#include "fleet.h"
#include "histstore.h"
#include "netcfg.h"
#include "packstore.h"
#include "blelink.h"

namespace webui {

static WebServer s_server(80);
static bool s_started = false;

static void jsonStr(String& j, const char* s) {
    j += '"';
    for (const char* p = s; *p; p++) {
        if (*p == '"' || *p == '\\') j += '\\';
        if ((uint8_t)*p < 0x20) continue;  // control characters never belong here
        j += *p;
    }
    j += '"';
}

static void handleStatus() {
    String j;
    j.reserve(1536);
    j += "{\"uptime\":";
    j += millis() / 1000;
    j += ",\"heap\":";
    j += (uint32_t)ESP.getFreeHeap();
    size_t fsUsed = 0, fsTotal = 0;
    histstore::usage(&fsUsed, &fsTotal);
    j += ",\"fsUsed\":";
    j += (uint32_t)fsUsed;
    j += ",\"fsTotal\":";
    j += (uint32_t)fsTotal;
    j += ",\"wifi\":{\"ssid\":\"";
    j += netcfg::ssid();
    j += "\",\"ip\":\"";
    j += netcfg::ip();
    j += "\",\"rssi\":";
    j += netcfg::rssi();
    j += "},\"fleet\":{\"running\":";
    j += fleet::running() ? "true" : "false";
    j += ",\"cycle\":";
    j += fleet::cycleSeconds();
    j += "},\"packs\":[";

    for (size_t i = 0; i < fleet::count(); i++) {
        fleet::PackView v;
        if (!fleet::view(i, &v)) continue;
        if (i) j += ',';
        j += "{\"label\":\"";
        j += v.label;
        j += "\",\"mac\":\"";
        j += v.mac;
        j += "\",\"online\":";
        j += v.online ? "true" : "false";
        j += ",\"ageSec\":";
        j += v.lastOkMs ? (millis() - v.lastOkMs) / 1000 : -1;
        j += ",\"polls\":";
        j += v.pollCount;
        j += ",\"fails\":";
        j += v.failCount;

        const lucas::Status& s = v.status;
        j += ",\"valid\":";
        j += s.valid ? "true" : "false";
        j += ",\"v\":";
        j += s.packMv;
        j += ",\"i\":";
        j += s.currentMa;
        j += ",\"soc\":";
        j += s.soc;
        j += ",\"cap\":";
        j += s.capacity01Ah;
        j += ",\"cycles\":";
        j += s.cycles;
        j += ",\"runtime\":";
        j += s.runtimeMin;
        j += ",\"spread\":";
        j += s.cellSpreadMv;
        j += ",\"cells\":[";
        for (uint8_t c = 0; c < s.cellCount; c++) {
            if (c) j += ',';
            j += s.cellMv[c];
        }
        j += "],\"temps\":[";
        for (uint8_t t = 0; t < s.tempCount; t++) {
            if (t) j += ',';
            j += s.tempC[t];
        }
        j += "]}";
    }
    j += "]}";
    s_server.send(200, "application/json", j);
}

// GET /api/history?range=<seconds>
//
// Short ranges come from the live RAM ring at full poll resolution; longer
// ones from the LittleFS archive at one record a minute. Either way the series
// is decimated to at most kMaxPoints so the JSON stays small and the SVG stays
// smooth.
static const size_t kMaxPoints = 240;

static void handleHistory() {
    uint32_t range = s_server.hasArg("range") ? (uint32_t)s_server.arg("range").toInt() : 900;
    if (range < 60) range = 60;
    if (range > histstore::kMaxAgeSec) range = histstore::kMaxAgeSec;

    uint32_t now = histstore::now();
    uint32_t since = now > range ? now - range : 0;
    bool fromArchive = range > 1800;

    static fleet::Sample ring[fleet::kHistory];
    static histstore::Record arc[histstore::kCapacity];

    String j;
    j.reserve(12288);
    j += "{\"now\":";
    j += now;
    j += ",\"range\":";
    j += range;
    j += ",\"source\":\"";
    j += fromArchive ? "archive" : "live";
    j += "\",\"packs\":[";

    for (size_t i = 0; i < fleet::count(); i++) {
        fleet::PackView v;
        if (!fleet::view(i, &v)) continue;

        size_t n = 0;
        if (fromArchive) n = histstore::read(v.mac, since, arc, histstore::kCapacity);
        else n = fleet::history(i, ring, fleet::kHistory);

        // Keep every Nth point; the last point is always included so the chart
        // ends at the newest reading rather than up to N-1 samples short.
        size_t step = 1;
        size_t within = 0;
        for (size_t k = 0; k < n; k++) {
            uint32_t t = fromArchive ? arc[k].t : ring[k].tSec;
            if (t >= since) within++;
        }
        if (within > kMaxPoints) step = (within + kMaxPoints - 1) / kMaxPoints;

        if (i) j += ',';
        j += "{\"label\":";
        jsonStr(j, v.label);
        j += ",\"t\":[";
        String sv = "", si = "", ss = "";
        size_t kept = 0, seen = 0;
        for (size_t k = 0; k < n; k++) {
            uint32_t t = fromArchive ? arc[k].t : ring[k].tSec;
            if (t < since) continue;
            bool last = (k + 1 == n);
            if (seen++ % step && !last) continue;
            if (kept++) { j += ','; sv += ','; si += ','; ss += ','; }
            j += t;
            sv += fromArchive ? arc[k].packMv : ring[k].packMv;
            si += fromArchive ? arc[k].currentMa : ring[k].currentMa;
            ss += fromArchive ? arc[k].soc : ring[k].soc;
        }
        j += "],\"v\":[" + sv + "],\"i\":[" + si + "],\"soc\":[" + ss + "]}";
    }
    j += "]}";
    s_server.send(200, "application/json", j);
}

// GET /api/devices                       list the saved packs and the interval
// GET /api/devices?add=<mac>&label=<s>   add one
// GET /api/devices?remove=<mac>          drop one
// GET /api/devices?rename=<mac>&label=   relabel one
// GET /api/devices?poll=<seconds>        set the polling interval
static void handleDevices() {
    String err;
    if (s_server.hasArg("add")) {
        if (!packstore::add(s_server.arg("add").c_str(), s_server.arg("label").c_str())) {
            err = "could not add - bad address, or the list is full";
        }
    } else if (s_server.hasArg("remove")) {
        if (!packstore::remove(s_server.arg("remove").c_str())) err = "no such device";
    } else if (s_server.hasArg("rename")) {
        if (!packstore::rename(s_server.arg("rename").c_str(), s_server.arg("label").c_str())) {
            err = "no such device";
        }
    }
    if (s_server.hasArg("poll")) {
        fleet::setCycleSeconds((uint32_t)s_server.arg("poll").toInt());
    }

    String j = "{\"ok\":";
    j += err.isEmpty() ? "true" : "false";
    if (!err.isEmpty()) {
        j += ",\"err\":";
        jsonStr(j, err.c_str());
    }
    j += ",\"poll\":";
    j += packstore::pollSeconds();
    j += ",\"max\":";
    j += (uint32_t)packstore::kMaxPacks;
    j += ",\"packs\":[";
    for (size_t i = 0; i < packstore::count(); i++) {
        packstore::Entry e;
        if (!packstore::get(i, &e)) continue;
        if (i) j += ',';
        j += "{\"mac\":";
        jsonStr(j, e.mac);
        j += ",\"label\":";
        jsonStr(j, e.label);
        j += '}';
    }
    j += "]}";
    s_server.send(200, "application/json", j);
}

// GET /api/scan            latest results
// GET /api/scan?start=1    queue a scan for the poller task to run
static void handleScan() {
    if (s_server.hasArg("start")) {
        uint32_t secs = s_server.hasArg("secs") ? (uint32_t)s_server.arg("secs").toInt() : 8;
        fleet::requestScan(secs);
    }
    String j = "{\"scanning\":";
    j += fleet::scanning() ? "true" : "false";
    j += ",\"ageSec\":";
    j += fleet::lastScanMs() ? (millis() - fleet::lastScanMs()) / 1000 : -1;
    j += ",\"results\":[";
    bool first = true;
    for (size_t i = 0; i < blelink::seenCount(); i++) {
        String mac, name;
        int rssi = 0;
        bool likely = false;
        if (!blelink::seenAt(i, &mac, &name, &rssi, &likely)) continue;
        if (!first) j += ',';
        first = false;
        j += "{\"mac\":";
        jsonStr(j, mac.c_str());
        j += ",\"name\":";
        jsonStr(j, name.c_str());
        j += ",\"rssi\":";
        j += rssi;
        j += ",\"likely\":";
        j += likely ? "true" : "false";
        j += ",\"known\":";
        j += packstore::has(mac.c_str()) ? "true" : "false";
        j += '}';
    }
    j += "]}";
    s_server.send(200, "application/json", j);
}

static void handleControl() {
    String what = s_server.arg("do");
    if (what == "start") fleet::start();
    else if (what == "stop") fleet::stop();
    else if (what == "cycle") fleet::setCycleSeconds((uint32_t)s_server.arg("s").toInt());
    else {
        s_server.send(400, "application/json", "{\"ok\":false,\"err\":\"unknown action\"}");
        return;
    }
    s_server.send(200, "application/json", "{\"ok\":true}");
}

static void handleUpdateDone() {
    bool failed = Update.hasError();
    netcfg::setOtaBusy(false);
    s_server.sendHeader("Connection", "close");
    s_server.send(failed ? 500 : 200, "application/json",
                  failed ? "{\"ok\":false}" : "{\"ok\":true}");
    if (!failed) {
        delay(400);
        ESP.restart();
    }
}

static void handleUpdateUpload() {
    HTTPUpload& up = s_server.upload();
    if (up.status == UPLOAD_FILE_START) {
        netcfg::setOtaBusy(true);  // stop the poller touching the radio mid-flash
        Serial.printf("OTA: receiving %s\n", up.filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (Update.write(up.buf, up.currentSize) != up.currentSize) Update.printError(Serial);
    } else if (up.status == UPLOAD_FILE_END) {
        if (Update.end(true)) Serial.printf("OTA: %u bytes written, rebooting\n", up.totalSize);
        else Update.printError(Serial);
    }
}

void begin() {
    s_server.on("/", HTTP_GET, []() {
        s_server.sendHeader("Content-Encoding", "gzip");
        s_server.sendHeader("Cache-Control", "no-store");
        s_server.send_P(200, "text/html", (PGM_P)DASHBOARD_HTML_GZ,
                        DASHBOARD_HTML_GZ_LEN);
    });
    s_server.on("/api/status", HTTP_GET, handleStatus);
    s_server.on("/api/history", HTTP_GET, handleHistory);
    s_server.on("/api/control", HTTP_GET, handleControl);
    s_server.on("/api/devices", HTTP_GET, handleDevices);
    s_server.on("/api/scan", HTTP_GET, handleScan);
    s_server.on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
    s_server.onNotFound([]() { s_server.send(404, "text/plain", "not here"); });
}

void loop() {
    if (!netcfg::connected()) return;
    if (!s_started) {
        s_server.begin();
        s_started = true;
    }
    s_server.handleClient();
}

}  // namespace webui
