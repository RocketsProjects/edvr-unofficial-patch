#pragma once
// The draw-list probe (TEMPORARY, Built v9; drawlist_probe.cpp, the pure half in drawlist_probe_core.h). hotkey.drawlist_probe arms it; it
// collects three consecutive frames from the next frame tick, writes "drawlist probe:" lines and edvr_logs\drawlist_<stamp>.csv, and does
// nothing else. Its taps (drawlist_taps.h) sit inside functions that already run: the builder bracket observer, the engine motion emit and
// pool observers, and the substituted-draw counter. It adds no detour. Off, each tap is one atomic load.
#include <cstdint>

#include "drawlist_taps.h"

namespace edvr {

// A press of hotkey.drawlist_probe: arms one window (refused, and said so, while one is armed or collecting).
void drawlistRequest(const char* trigger, uint32_t frame);
// The render thread's frame tick (device_hook.cpp, after the builder probe's tick): closes the open frame, opens the next, writes the window.
void drawlistFrame(uint32_t frame);
// Whether the builder observer must be attached: armed or collecting (builder_probe.cpp's want includes it).
bool drawlistWanted() noexcept;

}  // namespace edvr
