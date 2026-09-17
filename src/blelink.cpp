#include "blelink.h"

#include <vector>

#include "config.h"
#include "util.h"

namespace blelink {

struct SeenDevice {
    NimBLEAddress addr;
    String name;
    int rssi;
    bool serialProfile;
};

// Remembers which address type actually worked for a MAC. Without this, every
// reconnect to a pack that was not in the last scan burns a full connect
// timeout on the wrong type before falling back.
struct KnownAddrType {
    String mac;
    uint8_t type;
};

static std::vector<SeenDevice> s_seen;
static std::vector<KnownAddrType> s_addrTypes;
static NimBLEClient* s_client = nullptr;
static NotifySink s_sink = nullptr;
static bool s_quiet = false;
static uint8_t s_connectTimeout = 10;

static const size_t kMaxSeen = 40;

static void discoverAll();  // defined with the GATT helpers below

// ------------------------------------------------------------- discovery --

// These two 16-bit services are what the cheap BLE-to-UART bridges inside
// bolt-on BMS modules advertise. It is a hint for ordering the device picker,
// never a filter - anything discovered can still be added.
static bool advertisesSerialProfile(const uint8_t* payload, size_t len) {
    size_t i = 0;
    while (i < len) {
        uint8_t fieldLen = payload[i];
        if (fieldLen == 0 || i + fieldLen >= len + 1) break;
        uint8_t type = payload[i + 1];
        if (type == 0x02 || type == 0x03) {  // 16-bit service UUID list
            for (uint8_t k = 2; k + 1 < fieldLen + 1; k += 2) {
                uint16_t uuid = (uint16_t)payload[i + k] | ((uint16_t)payload[i + k + 1] << 8);
                if (uuid == 0xFFE0 || uuid == 0xFFF0) return true;
            }
        }
        i += fieldLen + 1;
    }
    return false;
}

class ScanCallbacks : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* dev) override {
        NimBLEAddress addr = dev->getAddress();
        for (auto& d : s_seen) {
            if (d.addr == addr) {
                d.rssi = dev->getRSSI();
                return;
            }
        }
        if (s_seen.size() >= kMaxSeen) return;

        SeenDevice d;
        d.addr = addr;
        d.rssi = dev->getRSSI();
        d.name = dev->haveName() ? dev->getName().c_str() : "";
        d.serialProfile = advertisesSerialProfile(dev->getPayload(), dev->getPayloadLength());
        s_seen.push_back(d);
    }
};

static ScanCallbacks s_scanCb;

void scan(uint32_t seconds) {
    s_seen.clear();
    NimBLEScan* scanner = NimBLEDevice::getScan();
    scanner->setAdvertisedDeviceCallbacks(&s_scanCb, /*wantDuplicates=*/false);
    scanner->setActiveScan(true);  // also request the scan response payload
    scanner->setInterval(97);
    scanner->setWindow(67);
    scanner->setMaxResults(0);  // we keep our own list; saves heap
    scanner->start(seconds, false);
    scanner->stop();
    scanner->clearResults();
}

size_t seenCount() { return s_seen.size(); }

bool seenAt(size_t index, String* mac, String* name, int* rssi, bool* serialProfile) {
    if (index >= s_seen.size()) return false;
    const SeenDevice& d = s_seen[index];
    if (mac) *mac = d.addr.toString().c_str();
    if (name) *name = d.name;
    if (rssi) *rssi = d.rssi;
    if (serialProfile) *serialProfile = d.serialProfile;
    return true;
}

// --------------------------------------------------------- notifications --

static void onNotify(NimBLERemoteCharacteristic* chr, uint8_t* data, size_t len,
                     bool isNotify) {
    if (s_sink) s_sink(chr, data, len);
}

// ------------------------------------------------------ client callbacks --

class ClientCallbacks : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient* c) override {
        if (!s_quiet) {
            util::printStamp();
            Serial.printf("connected to %s\n", c->getPeerAddress().toString().c_str());
        }
        // Ask for a fast, steady interval - slow links drop BMS frames.
        c->updateConnParams(12, 24, 0, 400);
    }
    void onDisconnect(NimBLEClient* c) override {
        if (s_quiet) return;
        util::printStamp();
        Serial.println("disconnected");
    }
};

static ClientCallbacks s_clientCb;

// -------------------------------------------------------------- lifecycle --

void begin() {
    NimBLEDevice::init(MDNS_HOSTNAME);
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);
    NimBLEDevice::setMTU(517);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
    NimBLEDevice::setSecurityAuth(false, false, false);
}

// ------------------------------------------------------------- connection --

bool isConnected() { return s_client && s_client->isConnected(); }

static bool connectAddress(const NimBLEAddress& addr) {
    if (!s_client) {
        s_client = NimBLEDevice::createClient();
        s_client->setClientCallbacks(&s_clientCb, false);
        s_client->setConnectionParams(12, 24, 0, 400);
    }
    if (s_client->isConnected()) s_client->disconnect();
    s_client->setConnectTimeout(s_connectTimeout);

    if (!s_client->connect(addr, true)) return false;

    String mac(addr.toString().c_str());
    bool known = false;
    for (auto& k : s_addrTypes) {
        if (k.mac.equalsIgnoreCase(mac)) {
            k.type = addr.getType();
            known = true;
        }
    }
    if (!known) s_addrTypes.push_back({mac, addr.getType()});

    discoverAll();
    return true;
}

bool connectTo(const char* mac) {
    String t(mac);
    t.trim();
    if (t.isEmpty()) return false;

    // Seen before? Go straight to the address type that worked last time.
    for (const auto& k : s_addrTypes) {
        if (!k.mac.equalsIgnoreCase(t)) continue;
        if (connectAddress(NimBLEAddress(std::string(t.c_str()), k.type))) return true;
        break;  // the remembered type failed - fall through and try both
    }
    for (uint8_t type = 0; type <= 1; type++) {
        if (connectAddress(NimBLEAddress(std::string(t.c_str()), type))) return true;
    }
    return false;
}

void disconnect() {
    if (s_client && s_client->isConnected()) s_client->disconnect();
}

// ------------------------------------------------------------------ GATT --

// NimBLE's `refresh` flag deletes and rebuilds every remote service and
// characteristic object, invalidating any pointer we still hold. So: discover
// once, right after connecting, then always read the cache.
static void discoverAll() {
    if (!isConnected()) return;
    for (auto* svc : *s_client->getServices(true)) {
        for (auto* chr : *svc->getCharacteristics(true)) {
            chr->getDescriptors(true);
        }
    }
}

static NimBLERemoteCharacteristic* findChar(const char* uuid) {
    if (!isConnected()) return nullptr;
    NimBLEUUID want = util::parseUuid(uuid);
    for (auto* svc : *s_client->getServices(false)) {
        for (auto* chr : *svc->getCharacteristics(false)) {
            if (util::uuidEq(chr->getUUID(), want)) return chr;
        }
    }
    return nullptr;
}

bool setSubscribe(const char* uuid, bool enable) {
    NimBLERemoteCharacteristic* chr = findChar(uuid);
    if (!chr) return false;
    return enable ? chr->subscribe(chr->canNotify(), onNotify, true) : chr->unsubscribe(true);
}

bool writeChar(const char* uuid, const uint8_t* data, size_t len, bool response) {
    NimBLERemoteCharacteristic* chr = findChar(uuid);
    if (!chr) return false;
    if (!chr->canWrite() && !chr->canWriteNoResponse()) return false;

    // Honour what the characteristic actually supports, whatever was asked for.
    bool withResponse = response && chr->canWrite();
    if (!chr->canWriteNoResponse()) withResponse = true;
    return chr->writeValue(data, len, withResponse);
}

void setNotifySink(NotifySink sink) { s_sink = sink; }
void setQuiet(bool quiet) { s_quiet = quiet; }
void setConnectTimeout(uint8_t seconds) { s_connectTimeout = seconds; }

}  // namespace blelink
