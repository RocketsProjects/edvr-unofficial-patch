#pragma once

#include "../common/plugin_selection_admission.h"

#include <cstdint>

namespace edvr::openxr::native_fov_startup {

// Select the implementation before constructing NativeRuntimeHost. Keeping
// the admission predicate and factory injectable lets the offline rig count
// both boundaries without loading the production Performance module.
template<class OwnerAllowed, class Factory>
auto selectOps(bool hasAdmissionReservation,
               const edvr::plugin_selection::admission_v1::DecisionV1& decision,
               std::uint32_t generatedGroupIndex,
               OwnerAllowed ownerAllowed,
               Factory factory) noexcept(noexcept(ownerAllowed(decision, generatedGroupIndex)) &&
                                         noexcept(factory())) -> decltype(factory()) {
  if (hasAdmissionReservation && !ownerAllowed(decision, generatedGroupIndex))
    return nullptr;
  return factory();
}

} // namespace edvr::openxr::native_fov_startup
