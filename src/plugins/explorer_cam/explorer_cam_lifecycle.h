#pragma once

#include "../../d3d11/plugin_registry.h"

namespace edvr::plugins::explorer_cam {

// Immutable record for Explorer Cam's existing frame and module teardown API.
const EdvrPluginLifecycleOps& explorerCamLifecycleOps();

}  // namespace edvr::plugins::explorer_cam
