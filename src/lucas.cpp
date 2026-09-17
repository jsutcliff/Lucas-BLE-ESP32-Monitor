#include "lucas.h"

#include "util.h"

namespace lucas {

uint16_t crc16(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
        }
    }
    return crc;
}

size_t buildRequest(uint8_t c0, uint8_t c1, const uint8_t* payload, size_t payloadLen,
                    uint8_t* out, size_t outCap) {
    size_t total = 6 + payloadLen + 2;
    if (total > outCap || total > 255) return 0;
    out[0] = 0x7E;
    out[1] = 0xFF;  // src: host
    out[2] = 0x00;  // dst: BMS
    out[3] = c0;
    out[4] = c1;
    out[5] = (uint8_t)total;
    if (payloadLen) memcpy(out + 6, payload, payloadLen);
    uint16_t crc = crc16(out, 6 + payloadLen);
    out[6 + payloadLen] = (uint8_t)crc;         // low byte first
    out[7 + payloadLen] = (uint8_t)(crc >> 8);
    return total;
}

size_t requestStatus(uint8_t* out, size_t cap) { return buildRequest('2', '1', nullptr, 0, out, cap); }
size_t requestSettings(uint8_t* out, size_t cap) { return buildRequest('1', '1', nullptr, 0, out, cap); }
size_t requestEventLog(uint8_t* out, size_t cap) { return buildRequest('3', '1', nullptr, 0, out, cap); }

// ------------------------------------------------------------ reassembly --

static uint8_t s_buf[kMaxFrame];
static size_t s_have = 0;

void resetReassembly() { s_have = 0; }

void feed(const uint8_t* data, size_t len, FrameHandler onFrame) {
    for (size_t i = 0; i < len; i++) {
        // Resync: a frame can only start with 0x7E.
        if (s_have == 0 && data[i] != 0x7E) continue;
        if (s_have >= kMaxFrame) s_have = 0;
        s_buf[s_have++] = data[i];

        if (s_have < 6) continue;              // length field not in yet
        size_t total = s_buf[5];
        if (total < 8 || total > kMaxFrame) {  // implausible length, resync
            s_have = 0;
            continue;
        }
        if (s_have < total) continue;

        uint16_t want = (uint16_t)s_buf[total - 2] | ((uint16_t)s_buf[total - 1] << 8);
        if (crc16(s_buf, total - 2) == want) {
            if (onFrame) onFrame(s_buf, total);
        } else {
            util::OutGuard guard;
            util::printStamp();
            Serial.printf("CRC mismatch on a %u-byte frame - discarded\n", (unsigned)total);
        }
        s_have = 0;
    }
}

// --------------------------------------------------------------- decoding --

bool decodeStatus(const uint8_t* frame, size_t len, Status* out) {
    if (len < 8 || frame[0] != 0x7E) return false;
    if (frame[3] != '2' || frame[4] != '1') return false;

    const uint8_t* p = frame + 6;
    size_t n = len - 8;  // payload length

    Status s;
    if (n < 31) return false;
    s.packMv = (uint16_t)p[9] | ((uint16_t)p[10] << 8);
    s.currentMa = (int32_t)((uint32_t)p[11] | ((uint32_t)p[12] << 8) |
                            ((uint32_t)p[13] << 16) | ((uint32_t)p[14] << 24));
    s.runtimeMin = (uint16_t)p[23] | ((uint16_t)p[24] << 8);
    s.cycles = p[25];
    s.flag0 = p[2];
    s.flag1 = p[3];
    s.soc = p[27];
    s.capacity01Ah = (uint16_t)p[28] | ((uint16_t)p[29] << 8);
    s.cellCount = p[30];
    if (s.cellCount > kMaxCells) return false;
    if (n < (size_t)(31 + 3 * s.cellCount)) return false;

    for (uint8_t i = 0; i < s.cellCount; i++) {
        size_t o = 31 + 3 * i;
        s.cellMv[i] = (uint16_t)p[o] | ((uint16_t)p[o + 1] << 8);
    }

    // Temperatures follow the cell block as 16-bit big-endian values. This is
    // the least certain part of the decode - see docs/protocol.md.
    size_t t = 31 + 3 * s.cellCount;
    while (t + 1 < n && s.tempCount < kMaxTemps) {
        s.tempC[s.tempCount++] = (int16_t)(((uint16_t)p[t] << 8) | p[t + 1]);
        t += 2;
    }

    s.cellMinMv = 0xFFFF;
    for (uint8_t i = 0; i < s.cellCount; i++) {
        if (s.cellMv[i] < s.cellMinMv) s.cellMinMv = s.cellMv[i];
        if (s.cellMv[i] > s.cellMaxMv) s.cellMaxMv = s.cellMv[i];
    }
    if (s.cellCount == 0) s.cellMinMv = 0;
    s.cellSpreadMv = (uint16_t)(s.cellMaxMv - s.cellMinMv);
    s.valid = true;
    *out = s;
    return true;
}

void printStatus(const Status& s) {
    util::OutGuard guard;
    long ma = s.currentMa;
    long watt_mw = (long)((int64_t)s.packMv * s.currentMa / 1000);
    Serial.printf("  pack     %u.%03u V   %ld.%03ld A   %ld.%01ld W\n", s.packMv / 1000,
                  s.packMv % 1000, ma / 1000, labs(ma) % 1000, watt_mw / 1000,
                  (labs(watt_mw) % 1000) / 100);
    Serial.printf("  charge   SOC %u%%   capacity %u.%u Ah   cycles %u   runtime %u min\n",
                  s.soc, s.capacity01Ah / 10, s.capacity01Ah % 10, s.cycles, s.runtimeMin);
    Serial.printf("  flags    %02X %02X\n", s.flag0, s.flag1);
    Serial.printf("  cells    %u", s.cellCount);
    for (uint8_t i = 0; i < s.cellCount; i++) {
        Serial.printf("   %u.%03uV", s.cellMv[i] / 1000, s.cellMv[i] % 1000);
    }
    Serial.println();
    Serial.printf("  spread   %u mV (min %u, max %u)\n", s.cellSpreadMv, s.cellMinMv,
                  s.cellMaxMv);
    Serial.print("  temps   ");
    for (uint8_t i = 0; i < s.tempCount; i++) Serial.printf("  %d C", s.tempC[i]);
    Serial.println(s.tempCount ? "" : "  (none decoded)");

    uint32_t sum = 0;
    for (uint8_t i = 0; i < s.cellCount; i++) sum += s.cellMv[i];
    Serial.printf("  check    cells sum to %lu mV vs reported pack %u mV\n",
                  (unsigned long)sum, s.packMv);
}

}  // namespace lucas
