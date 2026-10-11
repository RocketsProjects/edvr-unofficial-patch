#pragma once

namespace edvr {

template <typename Callback>
bool runExplorerCamOwned(bool ownerAllowed, Callback&& callback) {
    if (!ownerAllowed) return false;
    callback();
    return true;
}

} // namespace edvr
