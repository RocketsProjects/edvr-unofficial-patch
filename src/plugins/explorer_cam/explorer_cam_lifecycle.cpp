#include "explorer_cam_lifecycle.h"

#include "../../d3d11/explorer_cam.h"

namespace edvr::plugins::explorer_cam {
namespace {

void frame(void*, uint32_t frameNo) {
    explorerCamFrameBoundary(frameNo);
}

void shutdown(void*) {
    explorerCamShutdown();
}

const EdvrPluginLifecycleOps kExplorerCamLifecycleOps = {
    sizeof(EdvrPluginLifecycleOps),
    plugins::kPluginExplorerCam,
    "explorer-cam",
    nullptr,
    nullptr,
    frame,
    shutdown,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    0,
};

}  // namespace

const EdvrPluginLifecycleOps& explorerCamLifecycleOps() {
    return kExplorerCamLifecycleOps;
}

}  // namespace edvr::plugins::explorer_cam
