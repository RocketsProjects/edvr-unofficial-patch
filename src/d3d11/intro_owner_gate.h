#pragma once

#include "../common/plugin_selection_admission.h"
#include "plugin_registry.h"

#include <utility>

namespace edvr {

struct IntroStartupAdmission {
    bool ownerAllowed = false;
    bool strictSelection = false;
};

inline IntroStartupAdmission introStartupAdmission(
    const plugin_selection::admission_v1::DecisionV1& decision,
    bool querySucceeded) noexcept {
    if (!querySucceeded) return {};
    IntroStartupAdmission result{};
    result.ownerAllowed = plugin_selection::admission_v1::ownerAllowedForStartup(
        decision, plugins::kPluginIntro);
    result.strictSelection = result.ownerAllowed &&
        decision.mode == static_cast<std::uint32_t>(
            plugin_selection::admission_v1::Mode::AdmittedSelection);
    return result;
}

template <class Callback>
bool runIntroOwned(bool ownerAllowed, Callback&& callback) {
    if (!ownerAllowed) return false;
    std::forward<Callback>(callback)();
    return true;
}

}  // namespace edvr
