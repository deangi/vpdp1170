// Forwarder for kek's bundled ArduinoJson copy.
//
// The vpdp1170 sketch does not otherwise depend on ArduinoJson, but selected
// kek headers include <ArduinoJson.h>. Keeping this forwarder at sketch root
// lets those includes resolve without installing a separate global library.
// The vendored ArduinoJson headers also include nested files with angle
// brackets such as <ArduinoJson/Array/ElementProxy.hpp>, so the sketch root
// also carries a mirrored ArduinoJson/ header directory copied from
// _upstream_kek/arduinojson/src/ArduinoJson.

#pragma once

#include "_upstream_kek/arduinojson/ArduinoJson.h"
