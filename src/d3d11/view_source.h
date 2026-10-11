#pragma once
// The view the aim comes from (view_source.cpp; TEMPORARY, the look probe and the sealed-hangar trial). Resolves which camera renders
// the frame and where it is: the explorer cam (its session on) and the seated view read the rows the temporal pass chose (the game's
// view-to-world [R | c]); on foot, without the explorer cam, no pose is read (the on-foot camera's rows are not read yet), and the view 0
// origin is the only position. Read-only: nothing is written and nothing is chosen.
#include <cstdint>

#include "view_source_core.h"

namespace edvr::view {

struct Resolved {
    Source source = Source::kOrigin;   // where the pose came from; kOrigin when only the origin is known
    Pose pose;                         // world position and forward; valid when poseOk
    bool poseOk = false;
    float origin[3] = {};              // the render context's view 0 origin (ctx + 0x40 + 0x540): the position the earlier code used
    bool originOk = false;
    float originGapM = -1.0f;          // distance from the pose position to the origin (-1 when either is missing)
    const char* reason = "";           // why the pose is or is not there: a sentence for the log (never empty after a resolve)
};

// Resolves the frame's view. ctx is the render context (a bucket's first qword); null reads only the rows. False when neither the rows
// nor the origin can be read.
bool resolveView(uintptr_t ctx, Resolved* out) noexcept;

}  // namespace edvr::view
