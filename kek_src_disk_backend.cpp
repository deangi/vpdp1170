#include "config.h"
#if VPDP1170_USE_KEK_CORE && VPDP1170_BUILD_KEK_ADAPTER
#include "_upstream_kek/disk_backend.cpp"
// RP06 lives here (not with RL/RK) so file-scope symbols like regnames
// do not collide, and so Arduino does not need a newly discovered root .cpp.
#include "_upstream_kek/rp06.cpp"
#endif
