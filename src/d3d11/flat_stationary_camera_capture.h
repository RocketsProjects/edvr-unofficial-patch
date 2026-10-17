#pragma once

#include <cstdint>

namespace edvr {

// Temporary, passive F1 instrument. Frame/Selected run on the flat Present
// thread; Refresh is called only by the existing camera-refresh pre-hook.
void flatStationaryCameraFrame(uint64_t completedFrame) noexcept;
void flatStationaryCameraSelected(uint64_t frame, const float camera[6][4],
                                  bool reset, bool menuContract, const void* depth) noexcept;
void flatStationaryCameraRefresh(uintptr_t camera, uintptr_t ctx, uint64_t callNo,
                                 uint64_t callerRva, uint32_t kind) noexcept;

} // namespace edvr
