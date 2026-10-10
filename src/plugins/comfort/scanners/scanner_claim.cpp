#include "scanner_claim.h"

#include "../../../d3d11/device_hook.h"
#include "../../../d3d11/fss_panel.h"
#include "../../../d3d11/fss_reveal.h"

namespace edvr::plugins::comfort::scanners {

bool scannerPanelClaim(ID3D11DeviceContext* context, char kind,
                       std::uint32_t count, std::uint32_t instances,
                       const ScannerClaimHostView& host,
                       FssPanelObservation* observation) {
    if (!observation) {
        if (fssPanelWantsDraws() && host.bodyFrame != 0 &&
            host.frameNo - host.bodyFrame <= 2) {
            return fssPanelOnEyeDraw(context, kind, count, instances);
        }
        return false;
    }

    const bool enabled = fssPanelWantsDraws();
    observation->outerEnabled = {true, true, enabled};
    if (!enabled) return false;

    const std::uint32_t bodyFrame = host.bodyFrame;
    observation->bodyFrame = {true, true, bodyFrame};
    if (bodyFrame == 0) return false;

    const std::uint32_t frameNo = host.frameNo;
    observation->frameNo = {true, true, frameNo};
    if (frameNo - bodyFrame > 2) return false;

    return fssPanelOnEyeDrawObserved(context, kind, count, instances,
                                     *observation);
}

bool scannerRevealClaim(ID3D11DeviceContext* context, char kind,
                        std::uint32_t count, std::uint32_t instances,
                        const ScannerClaimHostView& host,
                        FssRevealObservation* observation) {
    if (!observation) {
        if (fssRevealWantsDraws() &&
            ((host.bodyFrame != 0 && host.frameNo - host.bodyFrame <= 2) ||
             (host.jumpFrame != 0 && host.frameNo - host.jumpFrame <= 600 &&
              deviceHookFssModeLatch())) &&
            fssRevealOnEyeDraw(context, kind, count, instances)) {
            return true;
        }
        return false;
    }

    const bool steady = detail::g_fssRevealSteady;
    observation->outerSteady = {true, true, steady};
    bool lockstep = false;
    if (!steady) {
        lockstep = detail::g_fssRevealLockstep;
        observation->outerLockstep = {true, true, lockstep};
    }
    if (!steady && !lockstep) return false;

    const std::uint32_t bodyFrame = host.bodyFrame;
    observation->bodyFrame = {true, true, bodyFrame};
    bool inWindow = false;
    if (bodyFrame != 0) {
        const std::uint32_t bodyFrameNo = host.frameNo;
        observation->bodyFrameNo = {true, true, bodyFrameNo};
        inWindow = bodyFrameNo - bodyFrame <= 2;
    }

    if (!inWindow) {
        const std::uint32_t jumpFrame = host.jumpFrame;
        observation->jumpFrame = {true, true, jumpFrame};
        if (jumpFrame != 0) {
            const std::uint32_t jumpFrameNo = host.jumpFrameNo;
            observation->jumpFrameNo = {true, true, jumpFrameNo};
            if (jumpFrameNo - jumpFrame <= 600) {
                const bool modeLatch = deviceHookFssModeLatch();
                observation->modeLatch = {true, true, modeLatch};
                inWindow = modeLatch;
            }
        }
    }
    if (!inWindow) return false;

    return fssRevealOnEyeDrawObserved(context, kind, count, instances,
                                      *observation);
}

}  // namespace edvr::plugins::comfort::scanners
