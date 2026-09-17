#pragma once
#include <Arduino.h>

#include "lucas.h"

// Drives the pack protocol over the BLE link and decodes what comes back.
namespace poller {

void begin();
void setQuiet(bool quiet);  // suppress per-frame console output
void loop();

// Sends one request and lets the reply come back through the notify sink.
bool requestStatus();

// Hands over the most recent decoded status and clears the fresh flag.
// Returns false if nothing new has arrived since the last call.
bool takeStatus(lucas::Status* out);

// 0 stops the repeating poll.
void setInterval(uint32_t seconds);

// Call after a disconnect: subscriptions do not survive one.
void forgetSubscription();


}  // namespace poller
