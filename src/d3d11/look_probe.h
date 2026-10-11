#pragma once
// The look probe (TEMPORARY, the aiming measurement of docs/concourse-culling-2026-10-11.md). A press of hotkey.look_probe
// captures one frame, then reports:
//   (a) the records whose bounding sphere the camera's forward ray meets within 2 km, nearest first (up to 64): address,
//       radius, entry distance, bucket, base or child role, the base record, child count, and the vtable RVAs of the node
//       and of the record's first qword (the cheap identities; no name strings are reachable from them);
//   (b) the draws of that frame whose visible samples cover the centre pixel of their viewport (up to 64): vertex and pixel
//       shader hashes, draw kind, count, instances, blend and depth-state summaries, and the render target's size. The test is
//       a one-pixel scissored duplicate of each draw (the first 256 draws with a plain render target and no UAV or stream
//       output bindings), read back by an occlusion query; the duplicate touches one pixel of the render target.
// The camera: the main view's origin; the forward: the near plane's inward normal of the main view's frustum.
// Output: edvr_logs\look_probe_<stamp>.csv (look_probe_core.h has the columns) and one "look probe:" summary line.
#include <cstdint>

struct ID3D11DeviceContext;

namespace edvr {

// Once, at install: says what is armed.
void lookProbeInstall(bool flatProfile);
// A press of hotkey.look_probe: arms one capture at the next frame. Refused while one runs.
void lookProbeRequest(const char* trigger);
// The render thread's Present tick: arms, finishes the look frame, and reports once the centre tests have read back.
void lookProbeFrame(uint32_t frame);

// The admission store's consumer: whether the look frame is collecting buckets (store_hook.cpp asks).
bool lookProbeCollecting() noexcept;
void lookProbeAfterStore(uintptr_t bucket, uintptr_t record) noexcept;

// The draw wrappers ask whether to bracket a draw (one atomic load when no look frame runs).
bool lookProbeWantsDraws() noexcept;
// A draw of the look frame, before the census bracket. replay re-issues the same draw through the original entry point
// (null: the draw is listed without the centre test). kind is D, I, N, X, Y, Z or A.
using LookReplayFn = void (*)(void* arg);
void lookProbeDraw(ID3D11DeviceContext* ctx, char kind, uint32_t count, uint32_t instances, LookReplayFn replay,
                   void* arg) noexcept;

}  // namespace edvr
