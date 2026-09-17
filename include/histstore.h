#pragma once
#include <Arduino.h>

// Long-term history on LittleFS: one fixed-size circular file per pack, one
// averaged record a minute, 24 hours deep. The RAM ring in fleet.cpp still
// serves the live high-resolution view; this is what survives a reboot.
namespace histstore {

struct Record {
    uint32_t t;  // device-seconds, see now()
    uint16_t packMv;
    int32_t currentMa;
    uint8_t soc;
    int16_t tempC;
};

static const uint32_t kPeriodSec = 60;    // one archived record per minute
static const uint32_t kCapacity = 1440;   // 24 h per pack
static const uint32_t kMaxAgeSec = kPeriodSec * kCapacity;

bool begin();
bool mounted();

// Monotonic device-seconds, carried across reboots in NVS. Deliberately not
// wall-clock: the charts plot relative time, and a counter that only ever goes
// forwards can't produce the backwards jumps an unsynced RTC would.
//
// Time does NOT advance while the board is off, so a power cut leaves a gap
// that looks like no elapsed time. Records carry enough resolution for the
// chart to spot the discontinuity and break the line rather than draw through
// it - see the gap handling in web/dashboard.html.
uint32_t now();

void append(const char* mac, const Record& r);

// Records with t >= sinceT, oldest first. Returns how many landed in `out`.
size_t read(const char* mac, uint32_t sinceT, Record* out, size_t cap);

void erase(const char* mac);
void usage(size_t* usedBytes, size_t* totalBytes);

}  // namespace histstore
