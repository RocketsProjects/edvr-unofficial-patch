#include "scanner_lifecycle.h"

#include "../../../common/config.h"
#include "../../../d3d11/fss_panel.h"
#include "../../../d3d11/fss_reveal.h"

namespace edvr::plugins::scanners {
namespace {

void configure(void* opaqueConfig) {
    if (!opaqueConfig) return;
    Config& cfg = *static_cast<Config*>(opaqueConfig);
    fssPanelConfigure(cfg);
    fssRevealConfigure(cfg);
}

void frame(void*, uint32_t) {
    fssRevealFrameBoundary();
}

void shutdown(void*) {
    // VScreen runs the panel stage and core rectangle cleanup first; this is
    // the final reveal teardown at the former direct reveal callsite.
    fssRevealShutdown();
}

void configureStage(void*, uint32_t stage, void* opaqueConfig) {
    if (!opaqueConfig) return;
    Config& cfg = *static_cast<Config*>(opaqueConfig);
    switch (stage) {
    case kScannerLifecycleConfigurePanel: fssPanelConfigure(cfg); break;
    case kScannerLifecycleConfigureReveal: fssRevealConfigure(cfg); break;
    default: break;
    }
}

void frameStage(void*, uint32_t stage, ID3D11DeviceContext*, uint32_t) {
    if (stage == kScannerLifecycleRevealFrame) fssRevealFrameBoundary();
}

void shutdownStage(void*, uint32_t stage) {
    if (stage == kScannerLifecycleShutdownPanel) fssPanelShutdown();
}

const EdvrPluginLifecycleOps kScannerLifecycleOps = {
    sizeof(EdvrPluginLifecycleOps),
    plugins::kPluginComfort,
    "comfort",
    nullptr,
    configure,
    frame,
    shutdown,
    configureStage,
    frameStage,
    shutdownStage,
    nullptr,
    edvr::kComfortScannersLifecycleComponent,
};

}  // namespace

const EdvrPluginLifecycleOps& scannerLifecycleOps() {
    return kScannerLifecycleOps;
}

}  // namespace edvr::plugins::scanners
