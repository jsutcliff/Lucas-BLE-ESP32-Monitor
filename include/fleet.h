#pragma once
#include <Arduino.h>

#include "lucas.h"
#include "packstore.h"

// Round-robin monitor: connects to each saved pack in turn, asks for one
// status frame, disconnects, moves on. One BLE radio, any number of packs.
//
// It owns the radio and runs on its own FreeRTOS task, so a slow or failing
// pack can never stall the web server. Scans are queued through here too, for
// the same reason - only one thing may drive the radio at a time.
namespace fleet {

struct PackView {
    char label[24];
    char mac[18];
    bool online;
    uint32_t lastOkMs;
    uint32_t pollCount;
    uint32_t failCount;
    lucas::Status status;
};

struct Sample {
    uint32_t tSec;
    uint16_t packMv;
    int32_t currentMa;
    uint8_t soc;
    int16_t tempC;
};

static const size_t kHistory = 180;

void begin();
void start();
void stop();
bool running();

size_t count();
bool view(size_t index, PackView* out);
size_t history(size_t index, Sample* out, size_t cap);

// Queues a scan for the poller task to run between cycles - the web request
// returns immediately and the UI polls for the result.
void requestScan(uint32_t seconds);
bool scanning();
uint32_t lastScanMs();

void setCycleSeconds(uint32_t seconds);
uint32_t cycleSeconds();
void printStatus();

}  // namespace fleet
