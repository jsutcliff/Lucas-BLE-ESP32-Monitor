#pragma once
#include <Arduino.h>
#include <stdint.h>

// The BMS protocol spoken over the pack's BLE serial characteristic.
// See docs/protocol.md for the frame layout and field offsets.
namespace lucas {

static const uint8_t kMaxCells = 16;
static const uint8_t kMaxTemps = 8;
static const size_t kMaxFrame = 256;

struct Status {
    bool valid = false;
    uint32_t ageMs = 0;
    uint16_t packMv = 0;
    int32_t currentMa = 0;   // negative = discharging
    uint16_t runtimeMin = 0; // 0 when idle
    uint8_t cycles = 0;
    uint8_t flag0 = 0, flag1 = 0;
    uint8_t soc = 0;
    uint16_t capacity01Ah = 0;
    uint8_t cellCount = 0;
    uint16_t cellMv[kMaxCells] = {0};
    uint8_t tempCount = 0;
    int16_t tempC[kMaxTemps] = {0};
    // Derived, so the dashboard doesn't have to.
    uint16_t cellMinMv = 0, cellMaxMv = 0;
    uint16_t cellSpreadMv = 0;
};

// CRC-16/MODBUS over the whole frame including the leading 0x7E.
uint16_t crc16(const uint8_t* data, size_t len);

// Builds "7E FF 00 <c0> <c1> <len> [payload] <crc_lo> <crc_hi>".
// Returns the frame length, or 0 if it wouldn't fit.
size_t buildRequest(uint8_t c0, uint8_t c1, const uint8_t* payload, size_t payloadLen,
                    uint8_t* out, size_t outCap);

// Convenience wrappers for the three known queries.
size_t requestStatus(uint8_t* out, size_t outCap);    // "21"
size_t requestSettings(uint8_t* out, size_t outCap);  // "11"
size_t requestEventLog(uint8_t* out, size_t outCap);  // "31"

// Feed raw notification chunks in; complete, CRC-checked frames come out via
// the callback. Handles the 20-byte MTU fragmentation.
typedef void (*FrameHandler)(const uint8_t* frame, size_t len);
void resetReassembly();
void feed(const uint8_t* data, size_t len, FrameHandler onFrame);

// Decodes a complete "21" reply. Returns false if it isn't one.
bool decodeStatus(const uint8_t* frame, size_t len, Status* out);

void printStatus(const Status& s);

}  // namespace lucas
