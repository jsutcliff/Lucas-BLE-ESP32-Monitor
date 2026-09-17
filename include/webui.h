#pragma once
#include <Arduino.h>

// The dashboard: a self-contained page plus a small JSON API, served straight
// off the board. Also hosts the browser-based OTA upload.
namespace webui {

void begin();
void loop();

}  // namespace webui
