#pragma once
// The asset census (asset_census.cpp; TEMPORARY, hotkey.asset_census, docs\concourse-culling-2026-10-11.md, Built v7). A press arms one
// frame. The next frame's records, as the admission store observes them, are grouped by their identity chain (asset_census_core.h),
// and the CSV is written at the Present after that frame: edvr_logs\asset_census_<stamp>.csv, one row per identity. The docking-state
// readers, the remembered docking objects and the Status words are logged with it, so the capture records the hatch state. Read-only: no record is culled
// and nothing is written into the engine's memory.
#include <cstdint>

namespace edvr {

// A press of hotkey.asset_census: arms one frame. Refused while one is armed or collecting.
void assetCensusRequest(const char* trigger, uint32_t frame);
// The render thread's Present tick: starts the collection, and writes the CSV at the Present after it. A branch when idle.
void assetCensusFrame(uint32_t frame);
// Whether the admission store's gate should be open for the census (armed or collecting). The store hook asks it.
bool assetCensusWanted() noexcept;
// Whether the census is collecting: the store observer calls assetCensusAfterStore only then.
bool assetCensusCollecting() noexcept;
// The admission store calls this after it has written a record (bucket = the collection, record = its record).
void assetCensusAfterStore(uintptr_t bucket, uintptr_t record) noexcept;

}  // namespace edvr
