// Engine motion's draw wrapper, counted by how each substituted draw ended (H4 of the settlement CPU arc,
// docs\flat-cz-fps-2026-10-11.md, 2026-10-16). Plain data and text: engine_velocity.cpp fills it on the owner thread,
// the flat census (flat_cpu.h) sums it per 5 s window and prints it, and the rig drives all three without a device.
//
// A "run" is one substituted draw: BeginDraw bound engine motion's state, EndDraw either left it bound for the next
// producer draw (lazy), or put the game's state back at once (eager). Eager has two reasons and they are different
// costs: the overlay guard's private t3 is bound (the guarded draws, shader pairs BBE and DB3E), or a diagnostic
// capture is armed (lazy mode off). The D3D calls of a run are the wrapper's own, counted at their call sites
// (engineVelocityNoteStateCalls) across the run's BeginDraw and, for an eager run, its restore. A lazy run's restore
// is paid later, by whichever event flushes it: that is the "lazy flushes" figure, counted apart.
//
// Counts and D3D calls cover every frame. Time is read only on the frames the census clocks the render thread on
// (one in 16), so the time per run divides by the runs of those frames (timedRuns), not by all of them.
#pragma once

#include <cstdint>
#include <cstdio>

namespace edvr {

enum EngineRunKind : unsigned { kRunLazy = 0, kRunEagerGuard, kRunEagerCapture, kRunKinds };
inline constexpr const char* kEngineRunNames[kRunKinds] = {"lazy", "eager guarded (overlay t3)", "eager capture armed"};

struct EngineRunCounts {
    uint64_t runs[kRunKinds] = {};        // substituted draws, by how their state was left
    uint64_t d3dCalls[kRunKinds] = {};    // the wrapper's D3D context calls inside those runs (every frame)
    uint64_t timedRuns[kRunKinds] = {};   // the runs on clocked frames
    uint64_t ticks[kRunKinds] = {};       // their bracket time, QPC ticks
    uint64_t flushes = 0;                 // lazy flushes: the deferred restore, by whatever event forced it (every frame)
    uint64_t flushCalls = 0;
    uint64_t timedFlushes = 0;
    uint64_t flushTicks = 0;
    void add(const EngineRunCounts& o) noexcept {
        for (unsigned k = 0; k < kRunKinds; ++k) {
            runs[k] += o.runs[k];
            d3dCalls[k] += o.d3dCalls[k];
            timedRuns[k] += o.timedRuns[k];
            ticks[k] += o.ticks[k];
        }
        flushes += o.flushes;
        flushCalls += o.flushCalls;
        timedFlushes += o.timedFlushes;
        flushTicks += o.flushTicks;
    }
    uint64_t totalRuns() const noexcept {
        uint64_t n = 0;
        for (unsigned k = 0; k < kRunKinds; ++k) n += runs[k];
        return n;
    }
};

// One window's text for one kind: runs a frame, D3D calls and microseconds per run, and the time a clocked frame spends.
// "-" where nothing was counted or timed (never 0.00: a figure that was not measured is not a fast one).
inline void formatEngineRun(char* out, size_t cap, const char* name, const char* unit, const char* unitPlural, uint64_t runs, uint64_t calls, uint64_t timedRuns,
                            double timedMs, double frames, double clockedFrames) noexcept {
    char perCall[24] = "-", perRun[24] = "-";
    if (runs) std::snprintf(perCall, sizeof(perCall), "%.1f", static_cast<double>(calls) / static_cast<double>(runs));
    if (timedRuns) std::snprintf(perRun, sizeof(perRun), "%.2f", timedMs * 1000.0 / static_cast<double>(timedRuns));
    char perFrame[24] = "-";
    if (clockedFrames > 0.0) std::snprintf(perFrame, sizeof(perFrame), "%.3f", timedMs / clockedFrames);
    std::snprintf(out, cap, "%s %.1f %s/frame (%s D3D calls/%s, %s us/%s, %s ms per clocked frame)", name,
                  frames > 0.0 ? static_cast<double>(runs) / frames : 0.0, unitPlural, perCall, unit, perRun, unit, perFrame);
}

}  // namespace edvr
