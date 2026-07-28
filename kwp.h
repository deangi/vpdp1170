#pragma once
#include <stdint.h>

// Runtime gate for the kek KW11-P device (kek_kwp.cpp). When false, kek
// absorbs the CSR window as a stub. Set from [diag] kwp_enabled.
namespace kwp {

extern bool enabled;

}  // namespace kwp
