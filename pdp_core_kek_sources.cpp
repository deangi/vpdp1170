#include "config.h"

// Source imports live in the individual kek_src_*.cpp wrappers. Arduino
// compiles each sketch-root .cpp as a separate translation unit; doing that
// avoids duplicate file-scope symbols that appear if several upstream .cpp
// files are #included into one large bundle.
