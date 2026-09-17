#include "packstore.h"

#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "histstore.h"
#include "util.h"

namespace packstore {

static Entry s_packs[kMaxPacks];
static size_t s_count = 0;
static uint32_t s_pollSec = 10;
static uint32_t s_generation = 1;
static SemaphoreHandle_t s_lock = nullptr;

static void lock() { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock() { if (s_lock) xSemaphoreGive(s_lock); }

// Addresses are compared and stored lowercase so "AA:BB" and "aa:bb" are one
// device however they were typed or discovered.
static void normalise(const char* in, char* out, size_t cap) {
    size_t n = 0;
    for (const char* p = in; *p && n + 1 < cap; p++) {
        out[n++] = (char)tolower((int)*p);
    }
    out[n] = 0;
}

static bool validMac(const char* mac) {
    int digits = 0, colons = 0;
    for (const char* p = mac; *p; p++) {
        if (isxdigit((int)*p)) digits++;
        else if (*p == ':') colons++;
        else return false;
    }
    return digits == 12 && colons == 5;
}

static void save() {
    Preferences p;
    p.begin("lucas", false);
    p.putBytes("packs", s_packs, s_count * sizeof(Entry));
    p.putUInt("npacks", s_count);
    p.putUInt("pollsec", s_pollSec);
    p.end();
    s_generation++;
}

void begin() {
    s_lock = xSemaphoreCreateMutex();

    Preferences p;
    p.begin("lucas", true);
    size_t stored = p.getUInt("npacks", 0xFFFFFFFF);
    s_pollSec = p.getUInt("pollsec", 10);
    if (stored != 0xFFFFFFFF) {
        s_count = stored > kMaxPacks ? kMaxPacks : stored;
        p.getBytes("packs", s_packs, s_count * sizeof(Entry));
        p.end();
        Serial.printf("packs: %u loaded from NVS\n", (unsigned)s_count);
        return;
    }
    p.end();

    // Never configured: start empty. Devices are added from the dashboard.
    s_count = 0;
    save();
    Serial.println("packs: none configured - add them on the dashboard's Devices tab");
}

size_t count() {
    lock();
    size_t n = s_count;
    unlock();
    return n;
}

bool get(size_t index, Entry* out) {
    lock();
    bool ok = index < s_count;
    if (ok) *out = s_packs[index];
    unlock();
    return ok;
}

bool has(const char* mac) {
    char want[18];
    normalise(mac, want, sizeof(want));
    lock();
    bool found = false;
    for (size_t i = 0; i < s_count && !found; i++) found = strcmp(s_packs[i].mac, want) == 0;
    unlock();
    return found;
}

bool add(const char* mac, const char* label) {
    char want[18];
    normalise(mac, want, sizeof(want));
    if (!validMac(want)) return false;
    if (has(want)) return true;  // idempotent - adding twice is not an error

    lock();
    bool ok = s_count < kMaxPacks;
    if (ok) {
        snprintf(s_packs[s_count].mac, sizeof(s_packs[s_count].mac), "%s", want);
        snprintf(s_packs[s_count].label, sizeof(s_packs[s_count].label), "%s",
                 (label && *label) ? label : want);
        s_count++;
        save();
    }
    unlock();
    return ok;
}

bool remove(const char* mac) {
    char want[18];
    normalise(mac, want, sizeof(want));
    lock();
    bool found = false;
    for (size_t i = 0; i < s_count; i++) {
        if (strcmp(s_packs[i].mac, want) != 0) continue;
        histstore::erase(s_packs[i].mac);  // don't leave an orphan history file
        for (size_t k = i; k + 1 < s_count; k++) s_packs[k] = s_packs[k + 1];
        s_count--;
        found = true;
        save();
        break;
    }
    unlock();
    return found;
}

bool rename(const char* mac, const char* label) {
    char want[18];
    normalise(mac, want, sizeof(want));
    lock();
    bool found = false;
    for (size_t i = 0; i < s_count; i++) {
        if (strcmp(s_packs[i].mac, want) != 0) continue;
        snprintf(s_packs[i].label, sizeof(s_packs[i].label), "%s", label);
        found = true;
        save();
        break;
    }
    unlock();
    return found;
}

uint32_t pollSeconds() { return s_pollSec; }

void setPollSeconds(uint32_t seconds) {
    lock();
    s_pollSec = seconds < 3 ? 3 : (seconds > 3600 ? 3600 : seconds);
    save();
    unlock();
}

uint32_t generation() { return s_generation; }

void print() {
    util::OutGuard guard;
    Serial.printf("packs: %u saved, polling every %us\n", (unsigned)count(),
                  (unsigned)s_pollSec);
    for (size_t i = 0; i < count(); i++) {
        Entry e;
        if (get(i, &e)) Serial.printf("  %-2u %s  %s\n", (unsigned)i, e.mac, e.label);
    }
}

}  // namespace packstore
