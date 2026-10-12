#pragma once
// The builder probe (TEMPORARY, hotkey.builder_probe, docs\concourse-culling-2026-10-11.md, Built v8). A CodeHook on the record builder
// 0x42B4420, the one function both admission loops call for each record they admit (Dynamic actors D1 and D2). The pure half (the entry
// agreement, the call sites, the grouping, the CSV and the summary) is in builder_probe_core.h.
//
// Nothing is installed at launch. The detour goes in on the first want: a press of hotkey.builder_probe, or the sealed trial on. Its gate is
// closed when nothing wants it, so the relay goes straight to the builder.
//
// A press arms one frame: the next frame's builder calls are recorded (the record, its name, its view mask, admitted flag, sphere, distance,
// the caller, the thread), and at the Present after that the frame is written to edvr_logs\builder_probe_<stamp>.csv (grouped by name, then
// the calls), with a "builder probe:" summary line and a thread line.
// Built v8.1: with only the sealed trial on, the detours do no container work. A builder call whose record was cleared at entry (rec+0x234
// reads 0) is counted in an atomic, and the tick passes it to the trial's window as builder_after_cull=. The F9 frame's containers are guarded
// by the collector's lock (builder_probe_core.h).
//
// The builder takes six arguments and reads four plus the sixth; its return value is undefined (docs Built v8). Its callers ignore it.
#include <cstdint>

namespace edvr {

// Once, at install: says what is armed. Nothing is hooked here.
void builderProbeInstall(bool flatProfile);
// A press of hotkey.builder_probe: arms one frame. Refused while one is armed or collecting.
void builderProbeRequest(const char* trigger, uint32_t frame);
// The render thread's frame tick: passes the window's count to the trial, writes a collected frame, starts an armed one, and sets the gates.
void builderProbeFrame(uint32_t frame);
// Whether the probe wants the builder detour (armed, collecting, or the sealed trial on).
bool builderProbeWanted() noexcept;
// Built v9: whether the builder calls reach the probes through the bracket (the path the draw-list probe's path A taps need).
bool builderProbeOnBracket() noexcept;
// Whether a frame is being collected (the store's visits are recorded only then).
bool builderProbeCollecting() noexcept;
// The admission store calls this for each record it writes, while a frame is collected (bucket = the collection, record = its record).
void builderProbeAfterStore(uintptr_t bucket, uintptr_t record) noexcept;
// Sets the builder gate from what wants it. The admission store's recompute calls this.
void builderProbeRecompute() noexcept;

}  // namespace edvr
