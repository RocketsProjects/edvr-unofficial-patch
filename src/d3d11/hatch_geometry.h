#pragma once
// The hatch geometry probe (TEMPORARY, Built v8.2, v13.8; part of hotkey.hatch_probe). While the hatch probe is on, one frame in six is sampled: the
// store observer keeps the records named BlastShield, Hatch, Shutter, Door, Collar, Lid, Cover, Gate, Hanger or Hangar, and every pad; the frame tick
// expresses them in the own pad's frame and follows them across samples, and watches the lower cycle. The pure half is hatch_geometry_core.h.
#include <cstdint>

namespace edvr {

// The hatch probe turned on: resets the tracks and arms the first-sample name count. Off: closes an open lower cycle and logs the counts.
void hatchGeometryOn(uint32_t frame);
void hatchGeometryOff(uint32_t frame);
// The render thread's frame tick (while the hatch probe is on): processes a collected sample and arms the next one on every sixth frame.
void hatchGeometryFrame(uint32_t frame);
// Whether the probe wants the store observed (the hatch probe is on).
bool hatchGeometryWanted() noexcept;
// Whether a sampled frame is being collected (the store's visits are kept only then).
bool hatchGeometryCollecting() noexcept;
// The admission store calls this for each record it writes, while a sample is collected.
void hatchGeometryAfterStore(uintptr_t bucket, uintptr_t record) noexcept;

}  // namespace edvr
