#pragma once

#include "../plugins/cockpit_visuals/target_sharp_policy.h"

namespace edvr {

// Private rig seam. Ops metadata is copied and validated; context remains
// caller-owned until the next successful switch. nullptr selects absence;
// the exact targetSharpPolicyOpsV1() pointer resets to the builtin selection.
// A successful switch calls the previously selected Ops' shutdown first;
// invalid metadata, a throwing shutdown, or an outstanding begin/end bracket
// leaves the old selection intact. A throwing End, or one still reporting
// engaged, retains the same bracket for retry; Begin cannot replace it.
bool targetSharpSetPolicyOpsV1ForTest(const TargetSharpPolicyOpsV1* ops) noexcept;

}  // namespace edvr
