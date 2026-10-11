#pragma once

#include <utility>

namespace edvr {

// Runs host-owned Cockpit Visuals work only when startup admission allowed
// the owner. Keeping this tiny helper shared lets fixtures exercise the same
// absence boundary used by vScreen.
template <class Callback>
bool runCockpitVisualsOwned(bool ownerAllowed, Callback&& callback) {
    if (!ownerAllowed) return false;
    std::forward<Callback>(callback)();
    return true;
}

}  // namespace edvr
