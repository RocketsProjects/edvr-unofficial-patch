// The view the aim comes from (view_source.h; TEMPORARY). The explorer cam and the seated view read the rows the temporal pass chose for the
// frame, which are the game's view-to-world [R | c] (view_source_core.h). The origin is the render context's view 0 origin (ctx + 0x40 +
// 0x540), reported alongside so the frames can be checked against each other. On foot, without the explorer cam, the chosen rows are not
// the on-foot camera and no on-foot camera is read yet, so the source is the origin and no pose is claimed.
#include "view_source.h"

#include <windows.h>

#include <cmath>
#include <cstring>

#include "explorer_cam.h"    // explorerCamSessionActive()
#include "journal_watch.h"   // journalOnFootKnown(), journalOnFoot()
#include "temporal_pass.h"   // temporalPassChosenRows()
#include "ui_layer.h"        // uiLayerLiveForWorldRoute(), uiLayerWorldScreenHeld()

namespace edvr::view {
namespace {

constexpr uintptr_t kCtxView0 = 0x40;      // the render context's first view
constexpr uintptr_t kViewOrigin = 0x540;   // the view's float3 origin

bool readMem(void* dst, uintptr_t src, size_t n) noexcept {
    if (!src) return false;
    __try {
        std::memcpy(dst, reinterpret_cast<const void*>(src), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The on-foot test the vscreen footprint uses (vscreen.cpp, the frame boundary): the world route's screen flag while the route is live,
// else the journal's on-foot state.
bool onFootNow() noexcept {
    return uiLayerLiveForWorldRoute() ? uiLayerWorldScreenHeld() : (journalOnFootKnown() && journalOnFoot());
}

float distance3(const float a[3], const float b[3]) noexcept {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

}  // namespace

bool resolveView(uintptr_t ctx, Resolved* out) noexcept {
    if (!out) return false;
    *out = Resolved{};
    out->originOk = ctx != 0 && readMem(out->origin, ctx + kCtxView0 + kViewOrigin, sizeof(out->origin));
    const bool explorer = explorerCamSessionActive();
    const bool foot = onFootNow();
    // The chosen rows are read where they are the camera: with the explorer cam on (its eye rows follow the free camera) and seated. On
    // foot without the explorer cam they are not the on-foot camera, so none are read.
    bool rowsOk = false;
    if (explorer || !foot) {
        float rows[12] = {};
        bool bound = false;
        Pose p;
        if (temporalPassChosenRows(rows, &bound) && poseFromRows(rows, &p)) {
            out->pose = p;
            rowsOk = true;
        }
    }
    out->source = chooseSource(explorer, foot, rowsOk);
    out->poseOk = rowsOk;
    if (rowsOk && out->originOk) out->originGapM = distance3(out->pose.pos, out->origin);
    return rowsOk || out->originOk;
}

}  // namespace edvr::view
