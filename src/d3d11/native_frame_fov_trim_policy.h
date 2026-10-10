#pragma once

#include "../plugins/performance/fov_trim_settings_policy.h"

// Private same-DLL host seam for bootstrap and the production-source fixture.
// The table is copied. Passing nullptr selects absence; a valid replacement or
// absence destroys any current state under the frame mutex. Provider close and
// reacquire retain a selected state and its diagnostic cache.
// A selected state is created only on the first valid beginFrame call.
bool setNativeFrameFovTrimSettingsPolicyOpsV1(
    const edvr::plugins::performance::FovTrimSettingsPolicyOpsV1* ops);
