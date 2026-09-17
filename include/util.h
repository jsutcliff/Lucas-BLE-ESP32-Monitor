#pragma once
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <stddef.h>
#include <stdint.h>

namespace util {

// Serial is written from three places at once: the Arduino loop, the NimBLE
// host task (connection callbacks and notifications) and the fleet poller
// task. Concurrent writes corrupt the UART TX ring - output comes back
// duplicated and truncated at buffer boundaries - so every multi-line log site
// takes this lock for the whole message.
void begin();
void outLock();
void outUnlock();

struct OutGuard {
    OutGuard() { outLock(); }
    ~OutGuard() { outUnlock(); }
};

// Milliseconds since boot, printed as "[  12.345]" so log lines stay ordered
// and easy to scan.
void printStamp();

// "DD 03 00 1B" style, space separated, uppercase.
String toHex(const uint8_t* data, size_t len, char sep = ' ');

// Non-printable bytes render as '.', so a frame's embedded ASCII (model
// strings, serial numbers) jumps out of the log.
String toAscii(const uint8_t* data, size_t len);

// Classic 16-bytes-per-line hexdump, each line prefixed with `indent`.
void hexdump(const uint8_t* data, size_t len, const char* indent = "    ");


// Accepts "fff1", "0xfff1", "0000fff1-0000-1000-8000-00805f9b34fb".
NimBLEUUID parseUuid(const char* text);

// Compares regardless of whether either side is in 16/32/128-bit form.
bool uuidEq(const NimBLEUUID& a, const NimBLEUUID& b);


}  // namespace util
