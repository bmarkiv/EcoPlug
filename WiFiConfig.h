#pragma once

#include <stdint.h>

namespace WiFiConfig {

static constexpr const char* kApSsid = "SetupAP";
static constexpr const char* kApPassword = "setup123";

// AP mode network settings. Edit kApIp when you want a different portal IP.
static constexpr const char* kApIp = "10.0.1.1";
static constexpr const char* kApSubnet = "255.255.255.0";

}  // namespace WiFiConfig
