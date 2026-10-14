// The render thread's CPU profile, armed by key (src\common\cpu_sampler.h has the instrument; docs\engine-render-performance-2026-09-19.md
// has the arc). Two diagnostic keys, both temporary:
//
//   hotkey.profile_cpu   a key, empty is off (the default in VR and flat). A press asks for one capture; the render thread
//                        starts it at its next Present and samples it for ten seconds.
//   diag.profile_cpu     off | on (default off). `on` makes one capture about 20 s after the first in-scene frame (VR:
//                        the shared scene flag; flat has none, so `on` waits there and says so).
//
// Off costs no thread and no sample: the frame tick is one branch and the Present hook one relaxed load. Every capture
// writes "cpu profile:" lines to the graphics log (start, the report, end) and edvr_logs\cpu_profile_<stamp>.csv.
#pragma once

namespace edvr {

// ensureState, after the diagnostic keys are bound: reads diag.profile_cpu and says, once, what is armed.
void cpuProfileConfigure();

// A press of hotkey.profile_cpu (tickHotkeys, the render thread's frame tick). Refused, and said so, while a capture is
// already waiting or running.
void cpuProfileRequest(const char* trigger);

// tickHotkeys, once a frame: diag.profile_cpu = on waits for the first in-scene frame, then 20 s, then asks once.
void cpuProfileFrameTick();

// hookedPresent, on the presenting (render) thread, after the real Present: starts a requested capture on this thread,
// which is the thread the capture samples. One relaxed load when nothing is requested.
void cpuProfileBeat();

}  // namespace edvr
