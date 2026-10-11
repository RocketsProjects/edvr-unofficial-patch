#pragma once
// The shadow gate probe (TEMPORARY, the D0 diagnostic of
// docs/station-interior-2026-10-11.md): reads, for one frame, which of the four
// admission gates keeps a station hull out of the sun's shadow views. Read-only:
// every detour forwards its arguments and returns the original's result, and
// the one engine call it makes (the planes test) is a pure function of its inputs.
//
//   hotkey.shadow_gate_probe   a key, empty is off (the default). A press asks for one capture of the next frame.
//   diag.shadow_gate_probe     off | on (default off). On waits for the first in-scene frame (VR only; flat has
//                              no scene flag, so flat uses the key), then 20 s, then asks once.
//
// The output: "shadow gate probe:" lines in the graphics log (a header, one line
// per candidate record) and edvr_logs\shadow_gate_<stamp>.csv, one row per
// candidate record and view. The gate decode and the line and CSV formats are in
// shadow_gate_core.h.
#include <cstdint>

namespace edvr {

// Once, at install, after the diagnostic keys are read: says what is armed.
void shadowGateProbeInstall(bool flatProfile);
// A press of hotkey.shadow_gate_probe (or the diag timer): arms one capture at the next frame. Refused while one runs.
void shadowGateProbeRequest(const char* trigger);
// The render thread's Present tick, once per owned frame: starts a pending capture and finishes a running one.
void shadowGateProbeFrame(uint32_t frame);

}  // namespace edvr
