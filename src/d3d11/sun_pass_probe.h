#pragma once
// The hangar sun probe (TEMPORARY; src\d3d11\sun_pass_probe_core.h says what it pins and why; docs\hangar-sunlight-2026-10-11.md says what it is for).
// A CodeHook on RenderDirLights (EliteDangerous64.exe+0x5AC9C0, build 332841) that marks the sun pass on the thread, a skip of the sun pass's draws behind
// a toggle, and a log of the Status.json words and of one byte group the light dispatch reads. Both profiles; any gate miss is one log line and nothing
// patched.
#include <cstdint>

namespace edvr {

// Installs the hook once (later calls do nothing). Called from device_hook beside the other engine hooks, in BOTH profiles.
void sunPassProbeInstall(bool flatProfile);

// Once per presented frame, from the Present path: the Status words (logged on change), the memory watch (logged on change), and the 10 s window.
void sunPassProbeFrame(uint32_t frame);

// The hotkey (hotkey.sun_pass_skip) and the initial state (diag.sun_pass_skip): flips the skip and logs the flip with its frame.
void sunPassProbeToggle(uint32_t frame);

// Called once per draw by the draw thunks (vscreen.cpp beginPanelOverride). True means the draw must not be forwarded: it was issued inside the sun
// pass and the toggle is on. Counts every draw issued inside the sun pass, toggle on or off.
bool sunPassProbeSkipDraw() noexcept;

}  // namespace edvr
