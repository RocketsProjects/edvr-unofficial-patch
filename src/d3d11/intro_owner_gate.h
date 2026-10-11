#pragma once

#include "../common/plugin_selection_admission.h"
#include "plugin_registry.h"

#include <utility>

namespace edvr {

struct IntroStartupAdmission {
    bool ownerAllowed = false;
    bool strictSelection = false;
    bool strictModeSelection = false;
};

inline bool strictAdmissionNeedsVScreenHooks(
    const IntroStartupAdmission& admission,
    bool diagnosticsOwnerAllowed,
    bool temporalAAOwnerAllowed) noexcept {
    return admission.strictModeSelection &&
        (diagnosticsOwnerAllowed || temporalAAOwnerAllowed);
}

template <class OwnerAllowed, class IsSelected>
inline IntroStartupAdmission introStartupAdmissionWithValidators(
    const plugin_selection::admission_v1::DecisionV1& decision,
    bool querySucceeded, OwnerAllowed&& ownerAllowed,
    IsSelected&& isSelected) noexcept {
    if (!querySucceeded) return {};
    IntroStartupAdmission result{};
    result.ownerAllowed = ownerAllowed(decision);
    result.strictSelection = result.ownerAllowed &&
        decision.mode == static_cast<std::uint32_t>(
            plugin_selection::admission_v1::Mode::AdmittedSelection);
    if (decision.mode == static_cast<std::uint32_t>(
            plugin_selection::admission_v1::Mode::AdmittedSelection)) {
        for (std::uint32_t i = 0; i < plugins::kPluginCount; ++i) {
            if (isSelected(decision, i)) {
                result.strictModeSelection = true;
                break;
            }
        }
    }
    return result;
}

inline IntroStartupAdmission introStartupAdmission(
    const plugin_selection::admission_v1::DecisionV1& decision,
    bool querySucceeded) noexcept {
    return introStartupAdmissionWithValidators(
        decision, querySucceeded,
        [](const auto& d) noexcept {
            return plugin_selection::admission_v1::ownerAllowedForStartup(
                d, plugins::kPluginIntro);
        },
        [](const auto& d, std::uint32_t i) noexcept {
            return plugin_selection::admission_v1::selected(d, i);
        });
}

#ifdef EDVR_SELECTION_ADMISSION_TEST
inline IntroStartupAdmission introStartupAdmissionWithCatalog(
    const plugin_selection::admission_v1::DecisionV1& decision,
    bool querySucceeded,
    plugin_selection::admission_v1::test_seam::CatalogViewV1 catalog) noexcept {
    return introStartupAdmissionWithValidators(
        decision, querySucceeded,
        [catalog](const auto& d) noexcept {
            return plugin_selection::admission_v1::test_seam::
                ownerAllowedForStartupWithCatalog(
                    d, plugins::kPluginIntro, catalog);
        },
        [catalog](const auto& d, std::uint32_t i) noexcept {
            return plugin_selection::admission_v1::test_seam::
                selectedWithCatalog(d, i, catalog);
        });
}
#endif

template <class Callback>
bool runIntroOwned(bool ownerAllowed, Callback&& callback) {
    if (!ownerAllowed) return false;
    std::forward<Callback>(callback)();
    return true;
}

}  // namespace edvr
