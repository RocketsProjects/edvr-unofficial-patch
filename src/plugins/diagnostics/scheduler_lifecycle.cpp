#include "scheduler_lifecycle.h"
#include "scheduler_stack_probe.h"
#include "../../common/config.h"

namespace {

void configure(void* rawConfig) {
    if (!rawConfig) return;
    auto* config = static_cast<edvr::Config*>(rawConfig);
    edvrDiagnosticsSchedulerConfigure(
        edvrDiagnosticsSchedulerState(),
        config->getBool("advanced.scheduler_probe", false) ? 1u : 0u);
}

void frame(void* state, uint32_t sceneFrame) {
    edvrDiagnosticsSchedulerFrame(state, sceneFrame);
}

void shutdown(void* state) {
    edvrDiagnosticsSchedulerShutdown(state);
}

const EdvrPluginLifecycleOps kSchedulerLifecycleOps = {
    sizeof(EdvrPluginLifecycleOps),
    edvr::plugins::kPluginDiagnostics,
    "diagnostics",
    &edvr::schedulerStackProbe,
    configure,
    frame,
    shutdown,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    0u,
};

} // namespace

namespace edvr::plugins::diagnostics {

const EdvrPluginLifecycleOps& schedulerLifecycleOps() {
    return kSchedulerLifecycleOps;
}

} // namespace edvr::plugins::diagnostics

extern "C" void* edvrDiagnosticsSchedulerState() {
    return &edvr::schedulerStackProbe;
}

extern "C" void edvrDiagnosticsSchedulerConfigure(void* state,
                                                    uint32_t enabled) {
    if (state != &edvr::schedulerStackProbe) return;
    edvr::schedulerStackProbeConfigure(enabled != 0);
}

extern "C" void edvrDiagnosticsSchedulerFrame(void* state,
                                               uint32_t presentFrame) {
    if (state == &edvr::schedulerStackProbe)
        edvr::schedulerStackProbe.notePresentFrame(presentFrame);
}

extern "C" void edvrDiagnosticsSchedulerShutdown(void* state) {
    if (state != &edvr::schedulerStackProbe) return;
    edvr::schedulerStackProbeShutdown();
}
