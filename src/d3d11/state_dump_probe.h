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
#include <string>

namespace edvr {

// Once, at install, after the diagnostic keys are read: says what is armed.
void stateDumpInstall(bool flatProfile);
// A press of hotkey.state_dump (or the diag timer): starts one capture at the next frame. Refused while one runs.
void stateDumpRequest(const char* trigger);
// The render thread's Present tick, once per owned frame: collects a finished frame's row, starts or finishes a capture.
void stateDumpFrame(uint32_t frame);

// For the asset census (asset_census.cpp), which reads the same four docking readers for one frame. stateDumpCensusArm(true) installs
// the reader detours if they are not yet in and opens their observers; (false) closes the census's share and leaves a capture's own
// observers as they are. It returns the detour status ("" when off). The snapshot is read-only.
struct StateReaderSnapshot {
    bool installed = false;        // all four detours are in place
    uint32_t calls[4] = {};        // cumulative calls while observed (a capture start resets them)
    uint32_t codes[4] = {};        // the AL code the last call returned
    uintptr_t objects[4] = {};     // the object the last call named
    // TEMPORARY (Built v8): every distinct object the readers were called with, up to eight, remembered while the observers are armed (the
    // hatch probe, the census or a capture). Replaces the AAB8 suffix pin, which never matched.
    uintptr_t remembered[8] = {};
    uint32_t rememberedCount = 0;
};
std::string stateDumpCensusArm(bool on);
void stateDumpCensusSnapshot(StateReaderSnapshot* out);

// TEMPORARY (Built v8, hotkey.hatch_probe): a press toggles the hatch-state probe. On: the four reader detours are installed if they are not,
// the observers stay armed, the state-name table's pointers are logged once, and every frame each remembered object's state is read; a
// "hatch probe:" line is written only when a value changes (capped at 400 per session). Off: the last line count is logged.
void hatchProbeToggle(const char* trigger, uint32_t frame);

}  // namespace edvr
