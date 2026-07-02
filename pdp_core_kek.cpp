#include "config.h"

#if VPDP1170_USE_KEK_CORE && VPDP1170_BUILD_KEK_ADAPTER
// Arduino only compiles .cpp files in the sketch root. Keep the staged kek
// adapter in kek_port/ for clarity, and include it here only when the kek core
// is explicitly selected and the adapter source dependency set is ready.
#include "kek_port/pdp_core_kek.cpp.disabled"
#endif
