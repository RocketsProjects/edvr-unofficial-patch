#pragma once

#include <utility>

namespace edvr {

template <class Callback>
bool runDiagnosticsOwned(bool ownerAllowed, Callback&& callback) {
    if (!ownerAllowed) return false;
    std::forward<Callback>(callback)();
    return true;
}

}  // namespace edvr
