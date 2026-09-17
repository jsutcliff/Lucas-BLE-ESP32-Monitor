#include "histstore.h"

#include <LittleFS.h>
#include <Preferences.h>

#include "util.h"

namespace histstore {

// Header, then `kCapacity` fixed-size records used as a ring.
struct Header {
    uint32_t magic;
    uint16_t version;
    uint16_t recSize;
    uint32_t head;   // next slot to write
    uint32_t count;  // valid records, <= kCapacity
};

static const uint32_t kMagic = 0x4C554341;  // "LUCA"
static const uint16_t kVersion = 1;

static bool s_mounted = false;
static uint32_t s_base = 0;        // device-seconds accumulated before this boot
static uint32_t s_lastPersist = 0;

static void path(const char* mac, char* out, size_t cap) {
    // "aa:bb:cc:dd:ee:ff" -> "/h_aabbccddeeff.bin"
    char clean[16];
    size_t n = 0;
    for (const char* p = mac; *p && n + 1 < sizeof(clean); p++) {
        if (*p != ':') clean[n++] = (char)tolower((int)*p);
    }
    clean[n] = 0;
    snprintf(out, cap, "/h_%s.bin", clean);
}

bool begin() {
    // format-on-failure, so a fresh board or a changed partition table just works
    s_mounted = LittleFS.begin(true);
    if (!s_mounted) {
        Serial.println("history: LittleFS mount failed - history will not persist");
    }

    Preferences p;
    p.begin("lucas", true);
    s_base = p.getUInt("clock", 0);
    p.end();

    Serial.printf("history: %s, resuming clock at %lus\n",
                  s_mounted ? "LittleFS mounted" : "no filesystem",
                  (unsigned long)s_base);
    return s_mounted;
}

bool mounted() { return s_mounted; }

uint32_t now() { return s_base + millis() / 1000; }

// Cheap enough once a minute, and it keeps the clock monotonic across reboots.
static void persistClock() {
    uint32_t t = now();
    if (t - s_lastPersist < kPeriodSec) return;
    s_lastPersist = t;
    Preferences p;
    p.begin("lucas", false);
    p.putUInt("clock", t);
    p.end();
}

void append(const char* mac, const Record& r) {
    if (!s_mounted) return;
    char fn[32];
    path(mac, fn, sizeof(fn));

    Header h;
    File f = LittleFS.open(fn, "r+");
    if (!f || f.read((uint8_t*)&h, sizeof(h)) != sizeof(h) || h.magic != kMagic ||
        h.version != kVersion || h.recSize != sizeof(Record)) {
        if (f) f.close();
        f = LittleFS.open(fn, "w+");
        if (!f) return;
        h = {kMagic, kVersion, (uint16_t)sizeof(Record), 0, 0};
        f.write((const uint8_t*)&h, sizeof(h));
    }

    f.seek(sizeof(Header) + h.head * sizeof(Record));
    f.write((const uint8_t*)&r, sizeof(Record));
    h.head = (h.head + 1) % kCapacity;
    if (h.count < kCapacity) h.count++;
    f.seek(0);
    f.write((const uint8_t*)&h, sizeof(h));
    f.close();

    persistClock();
}

size_t read(const char* mac, uint32_t sinceT, Record* out, size_t cap) {
    if (!s_mounted || !cap) return 0;
    char fn[32];
    path(mac, fn, sizeof(fn));
    File f = LittleFS.open(fn, "r");
    if (!f) return 0;

    Header h;
    if (f.read((uint8_t*)&h, sizeof(h)) != sizeof(h) || h.magic != kMagic ||
        h.recSize != sizeof(Record) || h.count == 0) {
        f.close();
        return 0;
    }
    if (h.count > kCapacity) h.count = kCapacity;

    size_t n = 0;
    uint32_t start = (h.head + kCapacity - h.count) % kCapacity;  // oldest first
    for (uint32_t k = 0; k < h.count && n < cap; k++) {
        uint32_t slot = (start + k) % kCapacity;
        f.seek(sizeof(Header) + slot * sizeof(Record));
        Record r;
        if (f.read((uint8_t*)&r, sizeof(r)) != sizeof(r)) break;
        if (r.t >= sinceT) out[n++] = r;
    }
    f.close();
    return n;
}

void erase(const char* mac) {
    if (!s_mounted) return;
    char fn[32];
    path(mac, fn, sizeof(fn));
    LittleFS.remove(fn);
}

void usage(size_t* usedBytes, size_t* totalBytes) {
    if (usedBytes) *usedBytes = s_mounted ? LittleFS.usedBytes() : 0;
    if (totalBytes) *totalBytes = s_mounted ? LittleFS.totalBytes() : 0;
}

}  // namespace histstore
