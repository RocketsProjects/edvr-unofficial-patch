#pragma once

namespace edvr::native_render_policy {

// Keep the host's runtime/profile guards beside the published owner decision.
// The result is immutable for the process after admission publication.
inline bool resolutionPolicyStartupAllowed(bool runtimeFeaturesAllowed,
                                           bool profileSupportsPerformance,
                                           bool ownerAllowed) noexcept {
    return runtimeFeaturesAllowed && profileSupportsPerformance && ownerAllowed;
}

} // namespace edvr::native_render_policy
