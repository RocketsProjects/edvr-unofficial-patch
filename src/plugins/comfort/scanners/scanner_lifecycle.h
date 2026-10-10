#pragma once

#include "../../../d3d11/plugin_registry.h"

namespace edvr::plugins::scanners {

enum : uint32_t {
    kScannerLifecycleConfigurePanel = 1,
    kScannerLifecycleConfigureReveal = 2,
    kScannerLifecycleRevealFrame = 3,
    kScannerLifecycleShutdownPanel = 4,
};

// Immutable lifecycle record; module state remains in the existing scanner APIs.
const EdvrPluginLifecycleOps& scannerLifecycleOps();

}  // namespace edvr::plugins::scanners
