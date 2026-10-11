#pragma once
// The hangar sun probe (TEMPORARY; src\d3d11\sun_pass_probe_core.h says what it keys on and why; docs\hangar-sunlight-2026-10-11.md says what it is for).
// A skip of the sun pass's draws behind a toggle, keyed on the bound vertex and pixel shader pair (no engine hook), and a log of the Status.json words, of
// one byte group the light dispatch reads (build 332841 only), and of the sun pair's draw counts in each 10 s window.
#include <cstdint>

namespace edvr {

// Arms the probe once (later calls do nothing). Called from device_hook beside the other engine hooks, in BOTH profiles.
void sunPassProbeInstall(bool flatProfile);

// Once per presented frame, from the Present path: the Status words (logged on change), the memory watch (logged on change), and the 10 s window.
void sunPassProbeFrame(uint32_t frame);

// The hotkey (hotkey.sun_pass_skip) and the initial state (diag.sun_pass_skip): flips the skip and logs the flip with its frame.
void sunPassProbeToggle(uint32_t frame);

// Called once per draw by the draw thunks (vscreen.cpp beginPanelOverride), for draws on the immediate context, with the bound VS and PS content
// hashes. True means the draw must not be forwarded: its shaders are the sun pair and the toggle is on. Counts every pair draw, toggle on or off.
bool sunPassProbeSkipDraw(uint64_t vs, uint64_t ps) noexcept;

}  // namespace edvr
