// The OS GPU busy sampler's glue (2026-10-14): src\common\gpu_busy_os.h holds the maths and the text, gpu_busy_sampler.cpp
// the D3DKMT calls and the thread. Nothing here is on the frame path but one relaxed add.
#pragma once

#include "../common/gpu_busy_os.h"

struct ID3D11Device;

namespace edvr {

// The render thread, once per owned Present, from hookedPresent (device_hook.cpp) beside vramWatchTick. Counts the frame; the
// first call (VR profile only) reads the device's adapter LUID and starts the sampler thread. A relaxed add and one load
// after that. Never calls the kernel.
void gpuBusyFrame(ID3D11Device* device) noexcept;

// The latest figure, for the overlay and the Monitor tile; have=false (with the reason) before the first window, after a
// failure and when the figure is more than three seconds old. Any thread.
GpuBusyReading gpuBusyReading() noexcept;

// Stops the sampler thread (gpuCensusShutdown). The thread is detached; the DLL is pinned, so nothing waits for it.
void gpuBusyShutdown() noexcept;

}  // namespace edvr
