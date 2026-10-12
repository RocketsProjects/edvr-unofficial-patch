// The draw-list probe's tap globals (TEMPORARY, Built v9). No dependencies: linked by every build that links engine_velocity.cpp.
#include "drawlist_taps.h"

namespace edvr {

std::atomic<bool> g_drawlistCollecting{false};
std::atomic<const DrawlistTapFns*> g_drawlistTaps{nullptr};

}  // namespace edvr
