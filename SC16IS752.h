#pragma once

// Local bring-up shim for kek optional serial devices.
//
// Upstream kek can use SC16IS752 UART expanders for DC11/DZ11 serial lines on
// ESP32. The first vpdp1170 kek slice stubs those optional devices out, but
// bus.cpp still includes their headers. Provide the type so those headers can
// compile without installing the hardware-specific library.

class SC16IS752 {
};
