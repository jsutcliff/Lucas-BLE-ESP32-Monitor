#include "fleet.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "config.h"
#include "histstore.h"
#include "netcfg.h"
#include "poller.h"
#include "blelink.h"
#include "util.h"

namespace fleet {

struct PackState {
    PackView view;
    Sample ring[kHistory];
    size_t head = 0;   // next write position
    size_t used = 0;

    // Averaged into one archived record per histstore::kPeriodSec, so a
    // 24-hour view costs 1440 records instead of one per poll.
    uint32_t accCount = 0;
    uint32_t accMv = 0;
    int64_t accMa = 0;
    uint32_t accSoc = 0;
    int32_t accTemp = 0;
    uint32_t nextArchiveT = 0;
};

static PackState s_packs[packstore::kMaxPacks];
static size_t s_count = 0;
static uint32_t s_listGen = 0;
static volatile uint32_t s_scanRequest = 0;  // seconds, 0 = none pending
static volatile bool s_scanning = false;
static volatile uint32_t s_lastScanMs = 0;
static SemaphoreHandle_t s_lock = nullptr;
static TaskHandle_t s_task = nullptr;
static volatile bool s_running = false;
static volatile uint32_t s_cycleSec = 10;

// How long to wait for the pack to answer a status request before giving up.
static const uint32_t kReplyTimeoutMs = 3000;

static void lock() { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock() { xSemaphoreGive(s_lock); }

// Pull the saved list in. A slot whose address changed is a different pack, so
// its counters and history start again rather than blending two devices.
static void syncList() {
    if (s_listGen == packstore::generation()) return;
    lock();
    size_t n = packstore::count();
    if (n > packstore::kMaxPacks) n = packstore::kMaxPacks;
    for (size_t i = 0; i < n; i++) {
        packstore::Entry e;
        if (!packstore::get(i, &e)) continue;
        PackState& p = s_packs[i];
        if (strcmp(p.view.mac, e.mac) != 0) {
            p = PackState();
            snprintf(p.view.mac, sizeof(p.view.mac), "%s", e.mac);
        }
        snprintf(p.view.label, sizeof(p.view.label), "%s", e.label);
    }
    s_count = n;
    s_listGen = packstore::generation();
    s_cycleSec = packstore::pollSeconds();
    unlock();
}

static void record(PackState& p, const lucas::Status& st) {
    Sample s;
    s.tSec = histstore::now();
    s.packMv = st.packMv;
    s.currentMa = st.currentMa;
    s.soc = st.soc;
    s.tempC = st.tempCount ? st.tempC[0] : 0;
    p.ring[p.head] = s;
    p.head = (p.head + 1) % kHistory;
    if (p.used < kHistory) p.used++;

    p.accCount++;
    p.accMv += s.packMv;
    p.accMa += s.currentMa;
    p.accSoc += s.soc;
    p.accTemp += s.tempC;

    if (!p.nextArchiveT) p.nextArchiveT = s.tSec + histstore::kPeriodSec;
    if (s.tSec < p.nextArchiveT) return;

    histstore::Record r;
    r.t = s.tSec;
    r.packMv = (uint16_t)(p.accMv / p.accCount);
    r.currentMa = (int32_t)(p.accMa / (int64_t)p.accCount);
    r.soc = (uint8_t)(p.accSoc / p.accCount);
    r.tempC = (int16_t)(p.accTemp / (int32_t)p.accCount);
    histstore::append(p.view.mac, r);

    p.accCount = 0; p.accMv = 0; p.accMa = 0; p.accSoc = 0; p.accTemp = 0;
    p.nextArchiveT = s.tSec + histstore::kPeriodSec;
}

// One pack, one cycle: connect, ask, wait, disconnect.
static void pollPack(size_t i) {
    PackState& p = s_packs[i];
    lucas::Status st;
    bool ok = false;

    if (blelink::connectTo(p.view.mac)) {
        poller::forgetSubscription();
        if (poller::requestStatus()) {
            uint32_t deadline = millis() + kReplyTimeoutMs;
            while (millis() < deadline) {
                if (poller::takeStatus(&st)) {
                    ok = true;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(20));
            }
        }
        blelink::disconnect();
        // Let the stack finish tearing the link down before the next connect.
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    lock();
    p.view.pollCount++;
    if (ok) {
        p.view.status = st;
        p.view.online = true;
        p.view.lastOkMs = millis();
        record(p, st);
    } else {
        p.view.failCount++;
        // One miss on a weak link isn't "offline"; three cycles of silence is.
        if (millis() - p.view.lastOkMs > s_cycleSec * 1000 * 3 + 5000) {
            p.view.online = false;
        }
    }
    unlock();
}

static void task(void*) {
    for (;;) {
        syncList();

        // A queued scan takes priority: the user is waiting on it, and nothing
        // else may touch the radio while it runs.
        uint32_t scanSecs = s_scanRequest;
        if (scanSecs && !netcfg::otaInProgress()) {
            s_scanRequest = 0;
            s_scanning = true;
            blelink::scan(scanSecs);
            s_lastScanMs = millis();
            s_scanning = false;
            continue;
        }

        if (!s_running || netcfg::otaInProgress() || s_count == 0) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        uint32_t cycleStart = millis();
        for (size_t i = 0; i < s_count && s_running; i++) {
            if (s_scanRequest) break;  // don't make the user wait out a full cycle
            pollPack(i);
        }
        uint32_t spent = millis() - cycleStart;
        uint32_t budget = s_cycleSec * 1000;
        vTaskDelay(pdMS_TO_TICKS(spent < budget ? budget - spent : 200));
    }
}

void begin() {
    s_lock = xSemaphoreCreateMutex();
    histstore::begin();
    syncList();
    xTaskCreatePinnedToCore(task, "fleet", 6144, nullptr, 1, &s_task, 1);
}

void requestScan(uint32_t seconds) {
    s_scanRequest = seconds < 3 ? 3 : (seconds > 30 ? 30 : seconds);
}

bool scanning() { return s_scanning; }
uint32_t lastScanMs() { return s_lastScanMs; }

void start() {
    if (s_running) {
        Serial.println("fleet: already running");
        return;
    }
    // The console's BLE commands and the poller would fight over one radio, so
    // the fleet task takes it over and goes quiet.
    blelink::setQuiet(true);
    blelink::setConnectTimeout(5);
    poller::setQuiet(true);
    poller::setInterval(0);
    s_running = true;
    Serial.printf("fleet: polling %u pack(s) every %us\n", (unsigned)s_count,
                  (unsigned)s_cycleSec);
}

void stop() {
    s_running = false;
    blelink::setQuiet(false);
    blelink::setConnectTimeout(10);
    poller::setQuiet(false);
    Serial.println("fleet: stopped - the console owns the radio again");
}

bool running() { return s_running; }
size_t count() { return s_count; }

bool view(size_t index, PackView* out) {
    if (index >= s_count) return false;
    lock();
    *out = s_packs[index].view;
    unlock();
    return true;
}

size_t history(size_t index, Sample* out, size_t cap) {
    if (index >= s_count) return 0;
    lock();
    PackState& p = s_packs[index];
    size_t n = p.used < cap ? p.used : cap;
    // Walk back from the newest so a full ring yields the most recent `n`.
    size_t start = (p.head + kHistory - n) % kHistory;
    for (size_t k = 0; k < n; k++) out[k] = p.ring[(start + k) % kHistory];
    unlock();
    return n;
}

void setCycleSeconds(uint32_t seconds) {
    packstore::setPollSeconds(seconds);
    s_cycleSec = packstore::pollSeconds();
    Serial.printf("fleet: cycle set to %us\n", (unsigned)s_cycleSec);
}

uint32_t cycleSeconds() { return s_cycleSec; }

void printStatus() {
    util::OutGuard guard;
    Serial.printf("fleet: %s, cycle %us\n", s_running ? "running" : "stopped",
                  (unsigned)s_cycleSec);
    for (size_t i = 0; i < s_count; i++) {
        PackView v;
        view(i, &v);
        Serial.printf("  %-8s %s  %-7s  polls %lu ok / %lu failed",
                      v.label, v.mac, v.online ? "online" : "offline",
                      (unsigned long)(v.pollCount - v.failCount),
                      (unsigned long)v.failCount);
        if (v.status.valid) {
            Serial.printf("   %u.%03uV  %ld mA  SOC %u%%", v.status.packMv / 1000,
                          v.status.packMv % 1000, (long)v.status.currentMa,
                          v.status.soc);
        }
        Serial.println();
    }
}

}  // namespace fleet
