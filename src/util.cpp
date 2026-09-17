#include "util.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdarg.h>

namespace util {

static SemaphoreHandle_t s_outLock = nullptr;

// ESP-IDF logging (NimBLE's included) goes straight to the UART FIFO, while
// Arduino's Serial goes through the driver's TX ring. Two writers on one UART
// is what produced duplicated, truncated console output. Routing IDF logs
// through Serial puts everything on one path, under one lock.
static int logThroughSerial(const char* fmt, va_list args) {
    char buf[256];
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    if (n <= 0) return n;
    size_t len = (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1;
    OutGuard guard;
    Serial.write((const uint8_t*)buf, len);
    return n;
}

void begin() {
    // Recursive: a guarded log site may call another that guards too.
    if (!s_outLock) s_outLock = xSemaphoreCreateRecursiveMutex();
    esp_log_set_vprintf(logThroughSerial);
}

void outLock() {
    if (s_outLock) xSemaphoreTakeRecursive(s_outLock, portMAX_DELAY);
}

void outUnlock() {
    if (s_outLock) xSemaphoreGiveRecursive(s_outLock);
}

void printStamp() {
    uint32_t ms = millis();
    char buf[16];
    snprintf(buf, sizeof(buf), "[%6lu.%03lu] ", (unsigned long)(ms / 1000),
             (unsigned long)(ms % 1000));
    Serial.print(buf);
}

String toHex(const uint8_t* data, size_t len, char sep) {
    String s;
    s.reserve(len * 3);
    for (size_t i = 0; i < len; i++) {
        if (i && sep) s += sep;
        char b[3];
        snprintf(b, sizeof(b), "%02X", data[i]);
        s += b;
    }
    return s;
}

String toAscii(const uint8_t* data, size_t len) {
    String s;
    s.reserve(len);
    for (size_t i = 0; i < len; i++) {
        s += (data[i] >= 0x20 && data[i] <= 0x7E) ? (char)data[i] : '.';
    }
    return s;
}

void hexdump(const uint8_t* data, size_t len, const char* indent) {
    for (size_t off = 0; off < len; off += 16) {
        size_t n = min((size_t)16, len - off);
        char head[8];
        snprintf(head, sizeof(head), "%04X  ", (unsigned)off);
        String line = String(indent) + head;
        String hex = toHex(data + off, n);
        while (hex.length() < 16 * 3 - 1) hex += ' ';
        Serial.println(line + hex + "  |" + toAscii(data + off, n) + "|");
    }
}


NimBLEUUID parseUuid(const char* text) {
    String t(text);
    t.trim();
    if (t.startsWith("0x") || t.startsWith("0X")) t = t.substring(2);
    return NimBLEUUID(std::string(t.c_str()));
}

bool uuidEq(const NimBLEUUID& a, const NimBLEUUID& b) {
    // Copy before to128(): it normalises in place and we must not mutate the
    // caller's UUID (the characteristic's own, for instance).
    NimBLEUUID x(a), y(b);
    return x.to128() == y.to128();
}


}  // namespace util
