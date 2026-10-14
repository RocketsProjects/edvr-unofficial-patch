#pragma once
// The planet LOD trace (TEMPORARY instrument; planet_lod_trace_core.h says what it reads and why, docs\planet-lod-streaming-2026-10-14.md has the flight plan). Five read-only CodeHooks
// on Frontier's terrain generation scheduler and tile tree, build 332841 only. No key: installed once at startup in both profiles, inert on any other build. Delete when the arc closes.
#include <cstddef>
#include <cstdint>

namespace edvr {

// Install the hooks (once; later calls do nothing). One log line says live or inert.
void planetLodTraceInstall();

// Once a second from device_hook's config tick. Every five seconds, while generation work was queued, writes the summary, budget, tree and threads lines; otherwise at most one
// idle line a minute. `nowMs` is a monotonic millisecond clock.
void planetLodTracePoll(uint64_t nowMs);

#ifdef EDVR_PLT_TEST
// The rig's seams: drive the real detours against synthetic game memory, with a stand-in for the original function behind each.
void* planetLodTraceTestDetour(int site);                  // plt::Site
void planetLodTraceTestSetForward(int site, uintptr_t fn);
void planetLodTraceTestSetImage(uintptr_t lo, uintptr_t hi);
void planetLodTraceTestMarkLive();                          // as if every hook had installed
void planetLodTraceTestReset();
size_t planetLodTraceTestLineCount();
const char* planetLodTraceTestLine(size_t i);
#endif

}  // namespace edvr
