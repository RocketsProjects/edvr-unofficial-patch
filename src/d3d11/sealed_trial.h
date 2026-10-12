#pragma once
// The sealed-hangar trial (TEMPORARY, the manual trial of docs/concourse-culling-2026-10-11.md). Off by default; a press of
// hotkey.sealed_trial toggles it, and diag.sealed_trial_radius_m sets the starting sphere (default 80 m). While it is on, a press
// of hotkey.sealed_trial_smaller or hotkey.sealed_trial_larger steps the radius one rung down or up the ladder 80, 60, 40, 30, 20,
// 15, 10 m (clamped at both ends).
//
// While on:
//   * In the admission store (store_hook.cpp), each record the store has written is judged by the sphere rule
//     (sealed_trial_core.h): a record that contains the camera, or whose sphere intersects the sphere of the given radius
//     around the camera, is kept; any other record has its view mask (rec+0x208) and admitted flag (rec+0x234) cleared,
//     so it is culled from every view. The traversal then skips it.
//   * The sun pair (VS 7E38A6AA1269C901 with PS 7CECABDE34FFBE9E) is skipped in every frame (sun_pass_probe.cpp asks).
//   * A line every 5 s gives the records culled and kept, the sun pairs skipped, the time the rule took and the radius. A radius
//     step closes the window of the old radius first, so each rung has its own line.
// While off, nothing is observed, nothing is written, the sun pair is drawn as it was, and the step keys only log a refusal.
#include <cstdint>
#include <string>

namespace edvr {

// Once, at install: reads diag.sealed_trial_radius_m, diag.sealed_trial_mode and diag.sealed_trial_builder, and says what is armed.
void sealedTrialInstall(bool flatProfile);
// A press of hotkey.sealed_trial: turns the trial on or off, and logs the change.
void sealedTrialToggle(const char* trigger, uint32_t frame);
// A press of hotkey.sealed_trial_smaller (dir -1) or hotkey.sealed_trial_larger (dir +1): one rung while the trial is on.
void sealedTrialStep(int dir, const char* trigger, uint32_t frame);
// The render thread's per-frame tick: the 5-second line while the trial is on, and the kept-records report's frames.
void sealedTrialFrame(uint32_t frame);
// A press of hotkey.sealed_trial_report: arms one frame of the kept-records report (the trial must be on).
void sealedTrialReportRequest(const char* trigger, uint32_t frame);

// Whether the trial is on (one atomic load).
bool sealedTrialOn() noexcept;
// Whether the sun pair is skipped (the trial's sun rule; the same as sealedTrialOn).
bool sealedTrialSkipsSunPair() noexcept;
// The sun pass probe counts a skipped sun pair here.
void sealedTrialNoteSunSkip() noexcept;
// The admission store calls this after it has written a record (bucket = the collection, record = its record).
void sealedTrialAfterStore(uintptr_t bucket, uintptr_t record) noexcept;

// TEMPORARY (named mode, hotkey.sealed_trial_mode): a press cycles the record rule (sphere, named) while the trial is on; each change is
// logged, and the window of the old rule closes first. Off, the key only logs a refusal.
void sealedTrialModeStep(const char* trigger, uint32_t frame);
// TEMPORARY (named mode, Built v8.3b): steps the CURRENT size's cell radius one rung (dir -1 smaller, +1 larger) while the trial is on in named
// mode (hotkey.sealed_cell_smaller and hotkey.sealed_cell_larger); each change is logged with its size.
void sealedTrialCellStep(int dir, const char* trigger, uint32_t frame);
// TEMPORARY (builder probe, builder_probe.cpp, Built v8.1): the builder calls since the last tick whose record was cleared at the builder's entry.
// Added to the trial's window line (builder_after_cull=) at the probe's frame tick, before sealedTrialFrame.
void sealedTrialNoteBuilderCalls(uint64_t afterCull) noexcept;
// TEMPORARY, shared by the trial, the builder probe and the kept report: a record's name (rec+0x18 -> node, node+0 -> text, the census's accept
// rules), cached per node for the frame. False when no name can be read. Takes the trial's lock, so it is safe from any thread.
bool sealedTrialRecordName(uintptr_t record, std::string* out) noexcept;
// The same name, copied into a caller's buffer with no allocation (the hatch geometry probe's store path). False when no name can be read.
bool sealedTrialRecordNameInto(uintptr_t record, char* out, size_t cap) noexcept;

}  // namespace edvr
