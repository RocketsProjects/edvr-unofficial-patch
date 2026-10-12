#pragma once
// The builder probe (TEMPORARY, hotkey.builder_probe, docs\concourse-culling-2026-10-11.md, Built v8). A CodeHook on the record builder
// 0x42B4420, the one function both admission loops call for each record they admit (Dynamic actors D1 and D2). The pure half (the entry
// agreement, the call sites, the grouping, the CSV and the summary) is in builder_probe_core.h.
//
// Nothing is installed at launch. The detour goes in on the first want: a press of hotkey.builder_probe, or the sealed trial on. Its gate is
// closed when nothing wants it, so the relay goes straight to the builder.
//
// A press arms one frame: the next frame's builder calls are recorded (the record, its name, its view mask, admitted flag, sphere, distance,
// the caller), and at the Present after that the frame is written to edvr_logs\builder_probe_<stamp>.csv (grouped by name, then the calls),
// with a "builder probe:" summary line. Every frame while the sealed trial is on, the calls are classed for the trial's window line:
// builder_after_cull= (the record the trial culled in that frame) and builder_unvisited= (the store did not visit it in that frame).
//
// The builder takes six arguments and reads four plus the sixth; its return value is undefined (docs Built v8). Its callers ignore it.
#include <cstdint>

namespace edvr {

// Once, at install: says what is armed. Nothing is hooked here.
void builderProbeInstall(bool flatProfile);
// A press of hotkey.builder_probe: arms one frame. Refused while one is armed or collecting.
void builderProbeRequest(const char* trigger, uint32_t frame);
// The render thread's frame tick: classes the frame's calls, writes a collected frame, starts an armed one, and sets the gates.
void builderProbeFrame(uint32_t frame);
// Whether the probe wants the admission store observed (armed, collecting, or the sealed trial on).
bool builderProbeWanted() noexcept;
// The admission store calls these for each record it writes, while wanted (bucket = the collection, record = its record).
void builderProbeAfterStore(uintptr_t bucket, uintptr_t record) noexcept;
// The sealed trial calls this when it culls a record, so the builder calls of that record can be classed.
void builderProbeNoteCulled(uintptr_t record) noexcept;
// Sets the builder gate from what wants it. The admission store's recompute calls this.
void builderProbeRecompute() noexcept;

}  // namespace edvr
