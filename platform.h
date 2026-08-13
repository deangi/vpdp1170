#pragma once
#include <Arduino.h>
#include "config.h"

// Compile-time DZ11 + second Telnet. Set to 0 to strip from the binary for
// ~10–15% higher status-bar MIPS when multi-user serial is unused (see
// docs/dz11.md). Runtime [dz11] in wificonfig.ini is ignored when this is 0.
#ifndef VPDP_ENABLE_DZ11
#define VPDP_ENABLE_DZ11 0
#endif

#define HOST_LOG_TAG "vpdp1170"
#include "host_lib/log/host_log.h"
