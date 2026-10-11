#pragma once

#include "../plugins/performance/texture_sampler_policy.h"

#include <cstring>

namespace edvr {
namespace {

using SamplerPolicyFactoryV1 =
    const plugins::performance::TextureSamplerPolicyOpsV1* (*)();

// Resolve the host's private sampler-policy boundary before installing the
// sampler hook. A missing factory and an invalid Ops record leave a safe,
// absent policy and zeroed settings; only a valid configure callback can read
// config or ask the host for Elite's HMD multiplier.
inline bool samplerStartupConfigure(
    SamplerPolicyFactoryV1 factory, void* hostContext,
    plugins::performance::ReadEliteHmdMultiplierV1 readMultiplier,
    const plugins::performance::TextureSamplerPolicyOpsV1** policyOut,
    plugins::performance::TextureSamplerSettingsV1* settingsOut) {
    if (!policyOut || !settingsOut) return false;

    *policyOut = nullptr;
    std::memset(settingsOut, 0, sizeof(*settingsOut));
    if (!factory) return false;

    const plugins::performance::TextureSamplerPolicyOpsV1* ops = factory();
    if (!ops || ops->structSize != sizeof(*ops) ||
        ops->version != plugins::performance::kTextureSamplerPolicyOpsVersion1 ||
        !ops->configure) {
        return false;
    }

    *policyOut = ops;
    ops->configure(ops->context, hostContext, readMultiplier, settingsOut);
    return settingsOut->biasAutoRequested && !settingsOut->autoSourceAvailable;
}

// Owner admission is resolved before any private policy factory, module
// config read, or host-side Elite settings lookup. A deferred/error decision
// therefore leaves no stale policy state and performs no optional startup
// work. The legacy path passes true to preserve its historical setup.
inline bool samplerStartupConfigureForOwner(
    bool ownerAllowed, SamplerPolicyFactoryV1 factory, void* hostContext,
    plugins::performance::ReadEliteHmdMultiplierV1 readMultiplier,
    const plugins::performance::TextureSamplerPolicyOpsV1** policyOut,
    plugins::performance::TextureSamplerSettingsV1* settingsOut) {
    if (!policyOut || !settingsOut) return false;

    *policyOut = nullptr;
    std::memset(settingsOut, 0, sizeof(*settingsOut));
    if (!ownerAllowed) return false;

    return samplerStartupConfigure(factory, hostContext, readMultiplier,
                                   policyOut, settingsOut);
}

}  // namespace
}  // namespace edvr
