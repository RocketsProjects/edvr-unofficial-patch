#include "comfort_lifecycle.h"

#include "../../common/config.h"
#include "../../d3d11/transition_flash_eye_base.h"

namespace edvr::plugins::comfort {
namespace {

void configure(void* rawConfig) {
    if (!rawConfig) return;
    transitionFlashEyeBaseConfigure(*static_cast<Config*>(rawConfig));
}

void frame(void*, uint32_t frameNo) {
    transitionFlashEyeBaseFrameBoundary(frameNo);
}

void shutdown(void*) {
    transitionFlashEyeBaseShutdown();
}

void frameStage(void*, uint32_t stage, ID3D11DeviceContext*,
                uint32_t frameNo) {
    if (stage == kComfortLifecycleTransitionFlashFrame)
        transitionFlashEyeBaseFrameBoundary(frameNo);
}

const EdvrPluginLifecycleOps kComfortLifecycleOps = {
    sizeof(EdvrPluginLifecycleOps),
    plugins::kPluginComfort,
    "comfort",
    nullptr,
    configure,
    frame,
    shutdown,
    nullptr,
    frameStage,
    nullptr,
    nullptr,
    edvr::kComfortTransitionFlashLifecycleComponent,
};

}  // namespace

const EdvrPluginLifecycleOps& comfortLifecycleOps() {
    return kComfortLifecycleOps;
}

}  // namespace edvr::plugins::comfort
