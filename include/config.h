#pragma once

// Network identity. Change these before flashing if you run more than one
// monitor on the same network, or want a different setup password.
#define MDNS_HOSTNAME "lucas"         // http://lucas.local/ and the OTA target
#define SETUP_AP_SSID "lucas-setup"   // captive portal raised when unconfigured
#define SETUP_AP_PASS "lucaslucas"    // >= 8 characters, or the AP comes up open
