#pragma once

#include "../../d3d11/plugin_registry.h"

namespace edvr::plugins::comfort {

enum : uint32_t {
    kComfortLifecycleTransitionFlashFrame = 1,
};

// Immutable lifecycle record for the transition-flash module. Its state stays
// in the existing public API implementation.
const EdvrPluginLifecycleOps& comfortLifecycleOps();

}  // namespace edvr::plugins::comfort
