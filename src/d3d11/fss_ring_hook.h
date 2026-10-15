#pragma once
// The ring pass observer: a READ-ONLY CodeHook at the entry of the game's ring pass (EliteDangerous64.exe+0x36558A0, build 332841) that, inside an FSS zoom
// window (fss_tiles.cpp's capture), logs per call the camera ID, the stereo role, the pass, the body mode, the offscreen-target flag and the scan alpha. It
// changes nothing: every call is forwarded unchanged. The question and the offsets are in fss_ring_core.h and docs/fss-scanner.md ("engine RE of the ring
// reveal (Opus)"); the reading table is in the 2026-10-15 (v5) entry.
//
// Installed once at startup in the VR profile, with the same relay machinery and the same refusals as the vertex-buffer resync: a build that is not 332841,
// a prologue that is not the one read from the exe, or a CodeHook refusal is ONE log line (`ring hook inert -- why`) and the hook stays out. While armed it says so,
// and every window closes with a line that counts the calls it saw (zero included), so a hook that never ran is told from one that saw nothing.
#include <cstdint>

namespace edvr {

// Install the hook (once; later calls do nothing). VR profile only; the flat profile is left alone.
void fssRingHookInstall();

// Once per frame at the boundary: whether a zoom/reveal capture is running, which window, which frame. Cheap atomics.
void fssRingHookWindow(bool active, uint32_t windowNo, uint32_t frame);

// The closing lines of a window (said whether or not anything was seen).
void fssRingHookWindowSummary(uint32_t windowNo);

}  // namespace edvr
