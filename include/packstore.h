#pragma once
#include <Arduino.h>

// The list of packs to monitor, and the poll interval, kept in NVS so they
// survive a reboot or a firmware update. Managed from the dashboard - the
// addresses in config.h are only the seed for a board that has never been
// configured.
namespace packstore {

static const size_t kMaxPacks = 8;

struct Entry {
    char mac[18];    // "aa:bb:cc:dd:ee:ff"
    char label[24];
};

void begin();

size_t count();
bool get(size_t index, Entry* out);
bool has(const char* mac);

bool add(const char* mac, const char* label);
bool remove(const char* mac);
bool rename(const char* mac, const char* label);

uint32_t pollSeconds();
void setPollSeconds(uint32_t seconds);

// Bumped on every change, so the poller can notice the list moved under it.
uint32_t generation();

void print();

}  // namespace packstore
