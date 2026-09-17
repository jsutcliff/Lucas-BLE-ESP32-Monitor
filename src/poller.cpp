#include "poller.h"

#include "lucas.h"
#include "blelink.h"
#include "util.h"

namespace poller {

static const char* kPipeUuid = "ffe1";  // write and notify, same characteristic

static uint32_t s_intervalMs = 0;
static uint32_t s_nextPollMs = 0;
static bool s_subscribed = false;
static lucas::Status s_last;
static bool s_fresh = false;
static bool s_quiet = false;

static void onFrame(const uint8_t* frame, size_t len) {
    if (frame[3] == '2' && frame[4] == '1') {
        lucas::Status s;
        if (lucas::decodeStatus(frame, len, &s)) {
            s_last = s;
            s_fresh = true;
        }
    }
    if (s_quiet) return;

    util::OutGuard guard;
    util::printStamp();
    Serial.printf("FRAME  cmd=\"%c%c\"  %u bytes  CRC ok\n", frame[3], frame[4],
                  (unsigned)len);

    Serial.println("  payload:");
    util::hexdump(frame + 6, len - 8, "    ");

    if (frame[3] == '2' && frame[4] == '1') {
        if (s_last.valid) lucas::printStatus(s_last);
        else Serial.println("  (status frame didn't decode - layout may differ)");
    }
    Serial.println();
}

static void onNotify(NimBLERemoteCharacteristic* chr, const uint8_t* data, size_t len) {
    lucas::feed(data, len, onFrame);
}

void begin() { blelink::setNotifySink(onNotify); }

void setQuiet(bool quiet) { s_quiet = quiet; }

bool takeStatus(lucas::Status* out) {
    if (!s_fresh) return false;
    *out = s_last;
    s_fresh = false;
    return true;
}

// The fleet poller reconnects constantly, and a subscription does not survive
// a disconnect - so it must be re-established on every new link.
void forgetSubscription() { s_subscribed = false; lucas::resetReassembly(); }

// The pack only answers a subscriber, so make sure we are one.
static bool ensureSubscribed() {
    if (!blelink::isConnected()) {
        Serial.println("not connected - try `bat 1` or `bat 2`");
        return false;
    }
    if (!s_subscribed) {
        if (!blelink::setSubscribe(kPipeUuid, true)) return false;
        s_subscribed = true;
        lucas::resetReassembly();
    }
    return true;
}

static bool send(size_t (*build)(uint8_t*, size_t)) {
    if (!ensureSubscribed()) return false;
    uint8_t frame[32];
    size_t n = build(frame, sizeof(frame));
    if (!n) return false;
    return blelink::writeChar(kPipeUuid, frame, n, false);
}

bool requestStatus() { return send(lucas::requestStatus); }

void setInterval(uint32_t seconds) {
    s_intervalMs = seconds * 1000;
    s_nextPollMs = millis();
    Serial.printf("polling %s\n",
                  seconds ? (String("every ") + seconds + "s").c_str() : "stopped");
}


void loop() {
    if (!s_intervalMs || !blelink::isConnected()) return;
    if ((int32_t)(millis() - s_nextPollMs) < 0) return;
    s_nextPollMs = millis() + s_intervalMs;
    requestStatus();
}

}  // namespace poller
