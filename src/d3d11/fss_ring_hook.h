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

#include "fss_ring_core.h"

namespace edvr {

// Install the hook (once; later calls do nothing). VR profile only; the flat profile is left alone.
void fssRingHookInstall();

// Once per frame at the boundary: whether a zoom/reveal capture is running, which window, which frame. Cheap atomics.
void fssRingHookWindow(bool active, uint32_t windowNo, uint32_t frame);

// The closing lines of a window (said whether or not anything was seen).
void fssRingHookWindowSummary(uint32_t windowNo);

// v8 (flight 0dbd3964): the hook finds the ARRIVAL itself. While the scanner is on show (`wanted`: the chrome is held and the FSS latch is open) every ring-pass call
// is checked, capture or not, and the first call in body mode 2 with the scan alpha ramping up (below 1, direction 0) is a trigger. `wanted == false` re-arms the trigger.
void fssRingHookScan(bool wanted);
// The pending trigger, once: the frame, the alpha and the camera of the call that saw it.
bool fssRingHookTakeArrival(uint32_t* frame, float* alpha, int* camera);
// The first frame inside the current capture where the arrival signature was seen (0: not seen).
uint32_t fssRingHookArrivalInWindow();
// The ring-pass call the calling thread is inside, if the capture is running (the draws made inside it belong to that camera, pass and mode).
bool fssRingHookCurrent(ringhook::Current* out);
// The main camera's id, as read once a window.
bool fssRingHookMainCamera(int* id);

}  // namespace edvr
