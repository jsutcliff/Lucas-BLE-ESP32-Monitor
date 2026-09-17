#pragma once
#include <Arduino.h>
#include <NimBLEDevice.h>

// The BLE radio: discovery, one connection at a time, and raw access to the
// characteristic a pack's BMS talks over. Only the fleet poller drives this,
// so there is never more than one caller competing for the radio.
namespace blelink {

void begin();

// --- discovery ---
void scan(uint32_t seconds);
size_t seenCount();
bool seenAt(size_t index, String* mac, String* name, int* rssi, bool* serialProfile);

// --- connection ---
bool connectTo(const char* mac);
void disconnect();
bool isConnected();

// --- characteristics ---
bool setSubscribe(const char* uuid, bool enable);
bool writeChar(const char* uuid, const uint8_t* data, size_t len, bool response);

// Raw notification tap, so the protocol decoder sees every byte without having
// to own the subscription.
typedef void (*NotifySink)(NimBLERemoteCharacteristic* chr, const uint8_t* data,
                           size_t len);
void setNotifySink(NotifySink sink);

void setQuiet(bool quiet);               // silence connect/disconnect chatter
void setConnectTimeout(uint8_t seconds);

}  // namespace blelink
