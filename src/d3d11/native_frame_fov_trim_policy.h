#pragma once

#include "../plugins/performance/fov_trim_settings_policy.h"

// Private same-DLL host seam for bootstrap and the production-source fixture.
// The table is copied. Passing nullptr selects absence; a valid replacement or
// absence destroys any current state under the frame mutex. Provider close and
// reacquire retain a selected state and its diagnostic cache.
// A selected state is created only on the first valid beginFrame call.
bool setNativeFrameFovTrimSettingsPolicyOpsV1(
    const edvr::plugins::performance::FovTrimSettingsPolicyOpsV1* ops);

// Startup resolves Performance's owner once before the first device can expose
// a native-frame provider. The admission adapter returns true for Legacy, so
// only a denied/invalid startup installs explicit absence; enabled policy
// remains lazy until the first valid frame.
inline bool applyNativeFrameFovTrimStartupAdmissionV1(
    bool ownerAllowed) {
    if (ownerAllowed) return true;
    return setNativeFrameFovTrimSettingsPolicyOpsV1(nullptr);
}
