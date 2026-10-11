#pragma once
// The state dump (TEMPORARY, the M2 measurement of docs/concourse-culling-2026-10-11.md): for 600 owned
// frames, what the four docking-state readers returned and on which object, the bytes that changed in
// each object (and in its +0x340 sub-object and the object its +0x240 pointer names), the journal's
// Docked, Undocked, Disembark and Embark counts, and the Status.json Flags and Flags2 words, all on the
// Present frame clock. Read-only: each reader detour forwards the call and returns the reader's own
// result; nothing calls the game's getters; every engine read is validated (SEH) before use.
//
//   hotkey.state_dump   a key, empty is off (the default). A press starts one 600-frame capture at the next frame.
//   diag.state_dump     off | on (default off). On waits for the first in-scene frame (VR only; flat has no scene
//                       flag, so flat uses the key), then 20 s, then starts once.
//
// Output: "state dump:" lines in the graphics log (a start line, one line per event, a done line) and
// edvr_logs\state_dump_<stamp>.csv, one row per frame (state_dump_core.h has the columns).
#include <cstdint>

namespace edvr {

// Once, at install, after the diagnostic keys are read: says what is armed.
void stateDumpInstall(bool flatProfile);
// A press of hotkey.state_dump (or the diag timer): starts one capture at the next frame. Refused while one runs.
void stateDumpRequest(const char* trigger);
// The render thread's Present tick, once per owned frame: collects a finished frame's row, starts or finishes a capture.
void stateDumpFrame(uint32_t frame);

}  // namespace edvr
