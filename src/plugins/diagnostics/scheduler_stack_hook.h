#pragma once
// CodeHook installation for SchedulerStackProbe. Kept apart from the probe
// so test rigs can link the probe with stub hook functions (the pattern set
// by kinematic_eval_probe/hook).
//
// Targets 2/3 (per-record drain 0x42DF940, reset+repopulate 0x36A0F50)
// carry this file's own CodeHooks. Targets 0/1 (the worker entries
// 0x4321940/0x4320340) already carry the kinematic eval hook's patch --
// CodeHook refuses to patch a function whose first instruction is a jump --
// so they are observed through the core kinematic relay's separate scheduler
// observer slot. Diagnostics registers that slot only while the probe is
// armed; the relay skips this feed whenever the slot is absent.
#include <cstdint>

namespace edvr {

class SchedulerStackProbe;

// Installs process-lifetime hooks on first call. Same return vocabulary as
// attachKinematicEvalHooks: "installed", "identity_mismatch" (the
// executable is not the hash-verified build), "opcode_mismatch" (a target
// does not carry the expected prologue or our own patch),
// "install_failed", "observer_busy".
const char* attachSchedulerStackHooks(SchedulerStackProbe* probe) noexcept;
// Stops new callbacks into this probe. Hooks and relays stay installed for
// the process lifetime; the probe itself is a global, so a callback that
// already loaded the pointer finishes safely.
void detachSchedulerStackHooks(SchedulerStackProbe* probe) noexcept;
// True when both own targets still carry OUR patch (used to accept an
// already-hooked prologue during validation).
bool schedulerStackHooksMatch(uintptr_t base) noexcept;

} // namespace edvr
