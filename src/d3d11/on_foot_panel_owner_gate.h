#pragma once

#include <utility>

namespace edvr {

template <class Callback>
bool runOnFootPanelOwned(bool ownerAllowed, Callback&& callback) {
    if (!ownerAllowed) return false;
    std::forward<Callback>(callback)();
    return true;
}

}  // namespace edvr
