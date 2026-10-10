#pragma once

#include "../../d3d11/plugin_registry.h"
#include <cstdint>

namespace edvr::plugins::diagnostics {

// The core registry sees only the stable lifecycle record and its opaque
// module-owned state. Probe implementation types remain private here.
const EdvrPluginLifecycleOps& schedulerLifecycleOps();

} // namespace edvr::plugins::diagnostics

extern "C" {
// Scalar C ABI used by the module's real lifecycle callbacks. The returned
// state is static for the process lifetime, so detached in-flight callbacks
// remain safe.
void* edvrDiagnosticsSchedulerState();
void edvrDiagnosticsSchedulerConfigure(void* state, uint32_t enabled);
void edvrDiagnosticsSchedulerFrame(void* state, uint32_t presentFrame);
void edvrDiagnosticsSchedulerShutdown(void* state);
}
