// aa_off_test -- the AA-off draw path and the cheap instruments it rides on (2026-10-13).
//
//   Cut B: draw_fast_path.h's decisions, the same functions vscreen.cpp's draw thunks and bind hooks call. A model
//          of a run of draws (a claimed draw, a gate-open draw, an unclaimed one) counts the verdict scopes, the
//          engine velocity calls and the shadow records the thunks would make. The model is a transcription of the
//          thunk's branches; the real thunks cannot run without a device, so the rig pins the decisions they take.
//   Cut C: map_wait.h's call counts (exact) and sampled ticks (scaled at take), and map_wait_sample.h's sampler.
//   Cut D: gpu_frame_timing.h's gpuFrameCommandIfLive, with a counting stub for the out-of-line gpuFrameCommand:
//          with the instrument off, no call is made; on, every call is made.
//   Cut E: the overlay's GPU figures (2026-10-14): gpu_busy_os.h's OS busy maths, board and log text, and frame_span_tile.h's
//          choice and text for the GPU BUSY tile and the fps line. No GPU and no kernel call: the readings are fed in.
//
// Exit codes: 0 pass, 1 a check failed, 2 usage. --dry-run touches nothing.
#include "../../src/common/gpu_busy_os.h"
#include "../../src/common/gpu_disjoint_clock.h"
#include "../../src/d3d11/draw_fast_path.h"
#include "../../src/d3d11/frame_span_tile.h"
#include "../../src/d3d11/gpu_frame_timing.h"
#include "../../src/d3d11/map_wait.h"

// The frame span holds at most kFrameSpanSlots pairs of two timestamps each (gpu_census.cpp); every one must fit the
// clock's per-record lease budget (kLeases, 512), or a pair would be refused rather than held.
static_assert(edvr::kFrameSpanSlots * 2u <= edvr::DisjointClock::kLeases,
              "the frame span's timestamps fit the disjoint clock's lease budget");

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace edvr {

// The stub of gpu_frame_timing.cpp's out-of-line call, counting what the thunks would have made.
static unsigned g_gpuCommandCalls = 0;
void gpuFrameCommand(ID3D11DeviceContext*) noexcept { ++g_gpuCommandCalls; }

namespace detail {
thread_local bool g_gpuFrameInternal = false;
std::atomic<bool> g_gpuFrameCommandLive{false};
std::atomic<bool> g_frameWorkArmed{false};
}  // namespace detail

// The stub of gpu_census.cpp's first-command hook (2026-10-14), counting the calls the armed branch makes.
static unsigned g_frameWorkCalls = 0;
void frameWorkFirstCommand(ID3D11DeviceContext*) noexcept { ++g_frameWorkCalls; }

}  // namespace edvr

namespace {

int g_failed = 0, g_checks = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failed;
        std::printf("FAIL: %s\n", what);
    }
}

// Cut B: the fast-path decision over all 32 input combinations.
void testFastPathTable() {
    unsigned taken = 0, mismatch = 0;
    for (unsigned bits = 0; bits < 32; ++bits) {
        const edvr::DrawPathInputs in{(bits & 1) != 0, (bits & 2) != 0, (bits & 4) != 0, (bits & 8) != 0, (bits & 16) != 0};
        const bool expect = !in.claimed && in.aaOff && !in.gateWanted && !in.engineVelocityLive && !in.vrWorld;
        const bool got = edvr::drawFastPathTaken(in);
        taken += got;
        mismatch += got != expect;
    }
    check(mismatch == 0, "fast path: the decision matches its rule for all 32 input combinations");
    check(taken == 1, "fast path: exactly one combination takes it (AA off, no claim, no gate, no engine velocity, no world route)");
    // Each non-AA-off or claimed or gate-open or EV-live or world-route draw is kept on the full path.
    const edvr::DrawPathInputs base{true, false, false, false, false};
    check(edvr::drawFastPathTaken(base), "fast path: the unclaimed AA-off draw is taken");
    edvr::DrawPathInputs c = base; c.claimed = true;
    check(!edvr::drawFastPathTaken(c), "fast path: a fix-claimed draw keeps the full path");
    c = base; c.gateWanted = true;
    check(!edvr::drawFastPathTaken(c), "fast path: an open subscriber gate keeps the full path");
    c = base; c.engineVelocityLive = true;
    check(!edvr::drawFastPathTaken(c), "fast path: live engine velocity (a diagnostic) keeps the full path");
    c = base; c.vrWorld = true;
    check(!edvr::drawFastPathTaken(c), "fast path: the VR world route keeps the full path");
    c = base; c.aaOff = false;
    check(!edvr::drawFastPathTaken(c), "fast path: AA on never takes it (behaviour with AA on is unchanged)");
}

// Cut B: the bind records. Only AA off with engine velocity closed skips VsSrv33 and Blend.
void testBindShadows() {
    check(edvr::bindShadowsSkipped(true, false), "bind shadows: skipped with AA off and engine velocity closed");
    check(!edvr::bindShadowsSkipped(true, true), "bind shadows: recorded while engine velocity is live");
    check(!edvr::bindShadowsSkipped(false, false), "bind shadows: recorded with AA on");
    check(!edvr::bindShadowsSkipped(false, true), "bind shadows: recorded with AA on and engine velocity live");
}

// Cut B: a model of the thunks over a run of draws. The model is the branch structure of hookedDraw / hookedDrawIndexed
// / hookedDrawInstanced: the verdict decides the scope; the fast path decides the scope and engine velocity; an
// unclaimed owner draw calls engine velocity on the full path only.
struct DrawTally {
    unsigned scopes = 0;          // forwardWithVerdict scopes entered
    unsigned engineVelocity = 0;  // engineVelocityBeforeDraw calls
    unsigned fastDraws = 0;       // draws issued directly (fast path)
    unsigned realDraws = 0;       // game draws issued, either way
};

DrawTally runDraws(bool aaOff, bool gateWanted, unsigned draws, unsigned claimEvery) {
    DrawTally t;
    for (unsigned i = 0; i < draws; ++i) {
        const bool claimed = claimEvery && (i % claimEvery) == 0;
        const edvr::DrawPathInputs in{aaOff, gateWanted, false, false, claimed};
        const bool fast = edvr::drawFastPathTaken(in);
        if (!fast && !claimed) ++t.engineVelocity;   // the full path's engineVelocityBeforeDraw on an unclaimed owner draw
        if (fast) {
            ++t.fastDraws;
        } else {
            ++t.scopes;
        }
        ++t.realDraws;
    }
    return t;
}

void testThunkModel() {
    // AA off, gate closed, no claims: every draw is fast, no scope, no engine velocity call.
    DrawTally a = runDraws(true, false, 1000, 0);
    check(a.fastDraws == 1000 && a.scopes == 0 && a.engineVelocity == 0 && a.realDraws == 1000,
          "thunk model: AA off, no subscriber, no claims: all 1000 draws skip the scope and engine velocity");
    // AA off, every 10th draw claimed: the claimed draws keep the full path, the rest are fast.
    DrawTally b = runDraws(true, false, 1000, 10);
    check(b.scopes == 100 && b.fastDraws == 900, "thunk model: every claimed draw (100 of 1000) keeps the full path");
    check(b.engineVelocity == 0, "thunk model: no unclaimed draw calls engine velocity while AA is off");
    // AA off with an open gate (a subscriber on): nothing is fast, every draw keeps the full path.
    DrawTally c = runDraws(true, true, 1000, 0);
    check(c.fastDraws == 0 && c.scopes == 1000, "thunk model: an open subscriber gate keeps every draw on the full path");
    // AA on: nothing is fast, whatever the gate and the claims.
    DrawTally d = runDraws(false, false, 1000, 10);
    check(d.fastDraws == 0 && d.scopes == 1000 && d.engineVelocity == 900,
          "thunk model: AA on keeps the full path (unclaimed draws still call engine velocity, as before)");
}

// Cut B: the bind-side model across an AA toggle (on, then off, then on). Recorded binds only when the records are
// wanted; the frame boundary invalidates skipped slots once they are wanted again.
void testBindToggle() {
    unsigned recorded = 0, skipped = 0;
    bool skippedFlag = false;
    // The sequence is AA on, on, off, off, off, on, on: aaOff is the negation.
    const bool aaOffSequence[] = {false, false, true, true, true, false, false};
    for (bool aaOff : aaOffSequence) {
        for (unsigned bind = 0; bind < 10; ++bind) {
            if (edvr::bindShadowsSkipped(aaOff, false)) {
                ++skipped;
                skippedFlag = true;
            } else {
                ++recorded;
            }
        }
        // The frame boundary: skipped slots are invalidated once the records are wanted again.
        if (skippedFlag && !edvr::bindShadowsSkipped(aaOff, false)) skippedFlag = false;
    }
    check(recorded == 40 && skipped == 30, "bind toggle: 40 binds recorded, 30 skipped across on-on-off-off-off-on-on");
    check(!skippedFlag, "bind toggle: back on, the skipped flag is cleared by the frame boundary");
}

// Cut C: the sampler and the scaling.
void testSampler() {
    uint32_t seq = 0;
    unsigned sampled = 0;
    unsigned firstAt = 0;
    for (unsigned i = 1; i <= 1600; ++i) {
        if (edvr::mapWaitSampleNext(seq)) {
            if (!sampled) firstAt = i;
            ++sampled;
        }
    }
    check(sampled == 100, "sampler: 1 in 16 -- 100 of 1600 calls are sampled");
    check(firstAt == 16, "sampler: the first sampled call is the 16th");
    check(edvr::mapWaitScaleSampled(3) == 48, "sampler: a sampled total scales by 16");
    check(edvr::mapWaitScaleSampled(0) == 0, "sampler: an empty total scales to zero");
}

// Cut C: the Map totals through the real map_wait.cpp: calls exact, ticks scaled by the rate at take.
void testMapTotals() {
    edvr::mapWaitTake();   // zero the totals
    for (int i = 0; i < 1600; ++i) edvr::mapWaitCall(D3D11_MAP_WRITE_DISCARD);
    for (int i = 0; i < 100; ++i) edvr::mapWaitCall(D3D11_MAP_READ);
    edvr::mapWaitNote(D3D11_MAP_READ, 1000);     // one sampled read of 1000 ticks
    edvr::mapWaitNote(D3D11_MAP_WRITE, 500);     // one sampled write of 500 ticks
    const edvr::MapWaitTotals t = edvr::mapWaitTake();
    check(t.writeCalls == 1600 && t.readCalls == 100, "map totals: call counts are exact (1600 writes, 100 reads)");
    check(t.readTicks == 16000 && t.writeTicks == 8000, "map totals: the sampled ticks are scaled by 16 at take");
    check(t.longestTicks == 1000, "map totals: the longest call is a measured one, not scaled");
    const edvr::MapWaitTotals again = edvr::mapWaitTake();
    check(again.readCalls == 0 && again.writeCalls == 0 && again.readTicks == 0,
          "map totals: taking zeroes the window");
}

// Cut D: the inline guard. With the instrument off, the thunks make no call; on, every call is made.
void testGpuGuard() {
    using namespace edvr;
    // The work span's arm (2026-10-14): one more relaxed load in the same gate. Armed, every command reaches the first-command
    // hook; disarmed, none does, and the frame's first command is the one that begins the span.
    detail::g_frameWorkArmed.store(false, std::memory_order_relaxed);
    g_frameWorkCalls = 0;
    for (int i = 0; i < 100; ++i) gpuFrameCommandIfLive(nullptr);
    check(g_frameWorkCalls == 0, "work span: disarmed, no command reaches the first-command hook");
    detail::g_frameWorkArmed.store(true, std::memory_order_relaxed);
    for (int i = 0; i < 100; ++i) gpuFrameCommandIfLive(nullptr);
    check(g_frameWorkCalls == 100, "work span: armed, every command reaches the first-command hook");
    detail::g_frameWorkArmed.store(false, std::memory_order_relaxed);
    detail::g_gpuFrameCommandLive.store(false, std::memory_order_relaxed);
    g_gpuCommandCalls = 0;
    for (int i = 0; i < 1000; ++i) gpuFrameCommandIfLive(nullptr);
    check(g_gpuCommandCalls == 0, "gpu guard: with app_gpu_timing off, 1000 thunk calls make no gpuFrameCommand call");
    detail::g_gpuFrameCommandLive.store(true, std::memory_order_relaxed);
    g_gpuCommandCalls = 0;
    for (int i = 0; i < 1000; ++i) gpuFrameCommandIfLive(nullptr);
    check(g_gpuCommandCalls == 1000, "gpu guard: with app_gpu_timing on, every thunk call is made");
    detail::g_gpuFrameInternal = true;   // inside EDVR's own GPU timing call: never re-entered
    g_gpuCommandCalls = 0;
    gpuFrameCommandIfLive(nullptr);
    check(g_gpuCommandCalls == 0, "gpu guard: the reentrancy flag suppresses the call");
    detail::g_gpuFrameInternal = false;
    detail::g_gpuFrameCommandLive.store(false, std::memory_order_relaxed);
}

// The Monitor page's GPU BUSY tile (2026-10-14): the OS busy figure as the value, the elapsed figure (the application-render mean, or
// the frame span with the clock off) labelled in the sub-line; "--" for the value only when the OS has no figure. A sub-line holds
// 39 characters (PerfTile::sub), so every string below must fit.
edvr::GpuBusyReading busyReading(double ms, double pct) {
    edvr::GpuBusyReading r;
    r.have = true;
    r.busyMs = ms;
    r.percent = pct;
    r.failure = edvr::GpuBusyFail::None;
    return r;
}
edvr::GpuBusyReading noBusy(edvr::GpuBusyFail f) {
    edvr::GpuBusyReading r;
    r.failure = f;
    return r;
}

void testGpuTimeTile() {
    char v[24], sub[40];
    // Busy present, application clock on with samples: the application mean, labelled.
    edvr::GpuFigures f = edvr::chooseGpuFigures(busyReading(5.04, 56.0), true, true, true, 12.34, true, 4.0);
    bool num = edvr::gpuBusyTileText(f, v, sizeof(v), sub, sizeof(sub));
    check(num && std::strcmp(v, "5.0") == 0 && std::strcmp(sub, "app render 12.3 ms (elapsed)") == 0,
          "tile: busy is the value; app_gpu_timing on shows the application mean labelled app render");
    // Busy present, clock off, a span: the span, labelled.
    f = edvr::chooseGpuFigures(busyReading(5.04, 56.0), false, false, true, 9.7, true, 3.0);
    num = edvr::gpuBusyTileText(f, v, sizeof(v), sub, sizeof(sub));
    check(num && std::strcmp(v, "5.0") == 0 && std::strcmp(sub, "span 9.7 ms (elapsed)") == 0,
          "tile: clock off shows the frame span labelled span beside the busy value");
    // Busy absent (the failure paths): "--", never a number, and the elapsed figure still labelled.
    f = edvr::chooseGpuFigures(noBusy(edvr::GpuBusyFail::NoProcs), false, false, true, 9.7, true, 3.0);
    num = edvr::gpuBusyTileText(f, v, sizeof(v), sub, sizeof(sub));
    check(!num && std::strcmp(v, "--") == 0 && std::strcmp(sub, "span 9.7 ms; no OS GPU figure") == 0,
          "tile: no OS figure prints '--' and keeps the span, labelled");
    // Nothing at all: '--' with a short reason; before the first window the reason is 'no sample yet'.
    f = edvr::chooseGpuFigures(noBusy(edvr::GpuBusyFail::NotStarted), false, false, false, 0.0, false, 0.0);
    num = edvr::gpuBusyTileText(f, v, sizeof(v), sub, sizeof(sub));
    check(!num && std::strcmp(v, "--") == 0 && std::strcmp(sub, "OS GPU stats: no sample yet") == 0, "tile: nothing yet prints '--' and says so");
    f = edvr::chooseGpuFigures(noBusy(edvr::GpuBusyFail::ReadFailed), false, false, false, 0.0, false, 0.0);
    edvr::gpuBusyTileText(f, v, sizeof(v), sub, sizeof(sub));
    check(std::strcmp(sub, "OS GPU stats unavailable") == 0, "tile: a failed sampler says the stats are unavailable");
    // Busy present, no elapsed figure yet: the sub-line names the missing one by its label.
    f = edvr::chooseGpuFigures(busyReading(5.04, 56.0), false, false, false, 0.0, false, 0.0);
    edvr::gpuBusyTileText(f, v, sizeof(v), sub, sizeof(sub));
    check(std::strcmp(v, "5.0") == 0 && std::strcmp(sub, "span: no sample yet") == 0, "tile: busy without a span says the span has no sample yet");
    f = edvr::chooseGpuFigures(busyReading(5.04, 56.0), true, false, false, 0.0, true, 3.0);
    edvr::gpuBusyTileText(f, v, sizeof(v), sub, sizeof(sub));
    check(std::strcmp(sub, "app render: no sample yet") == 0, "tile: clock on, no application sample: the label stays app render, the span is never read");
    // Every sub-line fits a tile's 39 characters.
    f = edvr::chooseGpuFigures(noBusy(edvr::GpuBusyFail::NoProcs), true, true, true, 123.4, true, 3.0);
    edvr::gpuBusyTileText(f, v, sizeof(v), sub, sizeof(sub));
    check(std::strlen(sub) <= 39, "tile: the longest sub-line fits a tile");
    // A tiny buffer truncates, never overruns.
    char tiny[2];
    edvr::gpuBusyTileText(f, tiny, sizeof(tiny), sub, sizeof(sub));
    check(std::strlen(tiny) < sizeof(tiny), "tile: a short value buffer is truncated, not overrun");
}

// The work span against the frame period (2026-10-14), in a simulated slack frame: 90 Hz, the GPU idle until the game's first command,
// then busy briefly, then idle until the next boundary. The work span is first command to door end; the boundary-to-boundary figure
// is the frame period. The arithmetic only (frameWorkSpanMs); the GPU's own timestamps are not simulated here.
void testFrameWorkSlack() {
    const double period = 1000.0 / 90.0;   // 11.11 ms at 90 Hz
    // One slack frame: the first command at 6.0 ms into the frame, the door end at 8.5 ms. The span is the work, not the period.
    const double slack = edvr::frameWorkSpanMs(6.0, 8.5);
    check(slack > 0.0 && slack < period, "work span: a slack frame's span is less than the frame period (11.1 ms at 90 Hz)");
    check(std::fabs(slack - 2.5) < 1e-9, "work span: the span is the door end minus the first command");
    // A sequence of slack frames: every span stays under the period, and the boundary-to-boundary figure would be the period each time.
    bool allUnder = true;
    for (int i = 0; i < 100; ++i) {
        const double first = 4.0 + (i % 5) * 0.5;      // the GPU starts the frame's work between 4.0 and 6.0 ms
        const double end = first + 2.0 + (i % 3) * 0.7; // and works for 2.0 to 3.4 ms
        const double span = edvr::frameWorkSpanMs(first, end);
        allUnder = allUnder && span > 0.0 && span < period;
    }
    check(allUnder, "work span: every frame of a 100-frame slack run is under the frame period");
    check(edvr::frameWorkSpanMs(8.0, 7.0) == 0.0, "work span: an out-of-order pair clamps to zero, not a negative frame");
}

// The in-headset fps overlay's line (2026-10-14): "gpu busy A ms" is the OS figure, the second figure is labelled. The tile and the line
// both call chooseGpuFigures, so they cannot choose apart; this runs the choice and the line the overlay prints.
void testOverlayLine() {
    char line[120];
    using edvr::GpuTimeSource;
    // Clock off, a span in the series, a busy figure: both, labelled.
    edvr::GpuFigures f = edvr::chooseGpuFigures(busyReading(5.04, 56.0), false, false, true, 4.26, true, 3.0);
    check(f.span.source == GpuTimeSource::WorkSpan && std::fabs(f.span.ms - 4.26) < 1e-9, "overlay: clock off with the series holding spans picks the work span mean");
    edvr::overlayLineText(line, sizeof(line), 89.6, f, "cpu", true, 2.34);
    check(std::strcmp(line, "90 fps   gpu busy 5.0 ms   span 4.3 ms   cpu 2.3 ms") == 0, "overlay: busy and span, labelled, in the line");
    // Clock off, the series aged out but a pair completed: the last pair.
    f = edvr::chooseGpuFigures(busyReading(5.04, 56.0), false, false, false, 0.0, true, 5.04);
    check(f.span.source == GpuTimeSource::WorkSpan && std::fabs(f.span.ms - 5.04) < 1e-9, "overlay: clock off, series empty, a completed pair: the last pair");
    // No OS figure: '--' for busy, the span still shown.
    f = edvr::chooseGpuFigures(noBusy(edvr::GpuBusyFail::NoStatistics), false, false, true, 4.26, true, 3.0);
    edvr::overlayLineText(line, sizeof(line), 90.0, f, "cpu", true, 2.34);
    check(std::strcmp(line, "90 fps   gpu busy -- ms   span 4.3 ms   cpu 2.3 ms") == 0, "overlay: no OS figure prints gpu busy '--' and keeps the span");
    // Nothing anywhere: '--' everywhere, never a number.
    f = edvr::chooseGpuFigures(noBusy(edvr::GpuBusyFail::NotStarted), false, false, false, 0.0, false, 0.0);
    edvr::overlayLineText(line, sizeof(line), 90.0, f, "cpu", false, 0.0);
    check(f.span.source == GpuTimeSource::None && std::strcmp(line, "90 fps   gpu busy -- ms   span -- ms   cpu -- ms") == 0,
          "overlay: nothing to show prints '--' for both and the cpu");
    // Clock on keeps its behaviour: the application mean when ready and sampled, else '--' (the work span is never read), labelled app render.
    f = edvr::chooseGpuFigures(busyReading(5.04, 56.0), true, true, true, 12.34, true, 4.0);
    check(f.span.source == GpuTimeSource::ApplicationRender && std::fabs(f.span.ms - 12.34) < 1e-9, "overlay: clock on and ready shows the application render mean");
    f = edvr::chooseGpuFigures(busyReading(5.04, 56.0), true, false, true, 12.34, true, 4.0);
    check(f.span.source == GpuTimeSource::None, "overlay: clock on but the snapshot not ready is '--', not the span");
    edvr::overlayLineText(line, sizeof(line), 90.0, f, "cpu", true, 1.0);
    check(std::strcmp(line, "90 fps   gpu busy 5.0 ms   app render -- ms   cpu 1.0 ms") == 0, "overlay: clock on without a sample labels the dashes app render");
    f = edvr::chooseGpuFigures(busyReading(5.04, 56.0), true, true, false, 0.0, true, 4.0);
    check(f.span.source == GpuTimeSource::None, "overlay: clock on with no application samples is '--', not the span");
    edvr::overlayLineText(line, sizeof(line), 72.0, edvr::chooseGpuFigures(busyReading(9.96, 80.0), true, true, true, 9.96, false, 0.0), "cpu (pre-submit)", true, 6.0);
    check(std::strcmp(line, "72 fps   gpu busy 10.0 ms   app render 10.0 ms   cpu (pre-submit) 6.0 ms") == 0, "overlay: clock on prints the application mean labelled, and the stand-in label");
    check(std::strlen(line) < 110, "overlay: the longest line fits the menu's 120-character overlay buffer with room");
    // A short buffer truncates, never overruns.
    char tiny[8];
    edvr::overlayLineText(tiny, sizeof(tiny), 90.0, f, "cpu", true, 1.0);
    check(std::strlen(tiny) < sizeof(tiny), "overlay: a short line buffer is truncated, not overrun");
    // The census field the log parser reads (tools\edvr_log.py --gpu-work-span): its exact text.
    char field[160];
    edvr::formatFrameWorkSpanField(field, sizeof(field), 0, 0, 0, 0.0);
    check(std::strcmp(field, "frame work span: completed 0, skipped 0, invalid 0, last - ms") == 0, "census field: nothing completed prints 'last -'");
    edvr::formatFrameWorkSpanField(field, sizeof(field), 5400, 3, 2, 4.2567);
    check(std::strcmp(field, "frame work span: completed 5400, skipped 3, invalid 2, last 4.257 ms") == 0, "census field: counts and the last pair in ms");
}

// The OS busy maths (gpu_busy_os.h). Readings are fed in the way the sampler feeds them: wall time in microseconds, the render loop's
// frame count, and each node's running time in 100 ns units.
void testGpuBusyMaths() {
    using edvr::GpuBusyAccumulator;
    using Result = GpuBusyAccumulator::Result;
    constexpr unsigned N = 3;   // node 0 is the 3D engine, node 1 a copy engine, node 2 an unreadable counted node
    const bool counted[N] = {true, false, true};
    bool usable[N] = {true, true, false};
    uint64_t run[N] = {1000, 50, 0};
    GpuBusyAccumulator acc;
    edvr::GpuBusyWindow w;
    // The first reading is the baseline and makes no window.
    check(acc.feed(1000000, 100, run, usable, counted, N, &w) == Result::Baseline && !w.valid, "busy: the first reading is a baseline");
    // One second at 90 fps with the 3D node running 450 ms: busy is 5.0 ms a frame, 45% of the period.
    run[0] += 4500000;   // 450 ms in 100 ns units
    run[1] += 100000;    // the copy engine 10 ms
    check(acc.feed(2000000, 190, run, usable, counted, N, &w) == Result::Window && w.valid, "busy: the second reading makes a window");
    check(std::fabs(w.utilization - 0.45) < 1e-9, "busy: utilisation is running time over wall time (450 ms over 1000 ms)");
    check(std::fabs(w.busyMsPerFrame - 5.0) < 1e-9, "busy: ms per frame is the running time over the frames (450 ms / 90 frames)");
    check(w.busiestNode == 0 && std::fabs(w.nodeUtil[1] - 0.01) < 1e-9, "busy: the copy engine is measured but not the figure");
    // 100 ns units, not microseconds: the RTX 5090 reading, 20548170 units for 2.05 s of work.
    check(std::fabs(20548170.0 * static_cast<double>(edvr::kGpuBusyUnitNs) / 1e6 - 2054.817) < 1e-6, "busy: running time is in 100 ns units");
    // The sum of engines is not the figure: two counted nodes both busy at 60% report 60%, not 120%.
    {
        const bool two[2] = {true, true};
        const bool ok[2] = {true, true};
        uint64_t r2[2] = {0, 0};
        GpuBusyAccumulator a2;
        edvr::GpuBusyWindow w2;
        a2.feed(0, 0, r2, ok, two, 2, &w2);
        r2[0] += 6000000;
        r2[1] += 6000000;
        a2.feed(1000000, 90, r2, ok, two, 2, &w2);
        check(w2.valid && std::fabs(w2.utilization - 0.6) < 1e-9 && std::fabs(w2.busyMsPerFrame - 600.0 / 90.0) < 1e-9,
              "busy: overlapping engines are a maximum, not a sum");
    }
    // Too soon: a reading 100 ms later makes no window and keeps the baseline, so the next one spans the whole second.
    run[0] += 100000;
    check(acc.feed(2100000, 200, run, usable, counted, N, &w) == Result::Dropped && !w.valid, "busy: a window under 250 ms is not read");
    run[0] += 4400000;
    check(acc.feed(3000000, 280, run, usable, counted, N, &w) == Result::Window, "busy: the next reading spans from the kept baseline");
    check(std::fabs(w.wallMs - 1000.0) < 1e-6 && std::fabs(w.frames - 90.0) < 1e-9, "busy: the kept baseline's window is 1000 ms and 90 frames");
    // No frames in the window (a pause): no per-frame figure, and the baseline moves on.
    run[0] += 100;
    check(acc.feed(4000000, 280, run, usable, counted, N, &w) == Result::Dropped && !w.valid, "busy: a window with no frame has no figure");
    // A counter that goes backwards (a reset): dropped, and the next window is fresh.
    run[0] = 10;
    check(acc.feed(5000000, 370, run, usable, counted, N, &w) == Result::Dropped, "busy: a counter going backwards drops the window");
    run[0] += 3000000;
    check(acc.feed(6000000, 460, run, usable, counted, N, &w) == Result::Window && std::fabs(w.utilization - 0.3) < 1e-9,
          "busy: the window after a reset is fresh");
    // A little over 100% (clock skew) is held at 100%; far over is garbage and dropped.
    run[0] += 10300000;
    check(acc.feed(7000000, 550, run, usable, counted, N, &w) == Result::Window && w.clamped && std::fabs(w.utilization - 1.0) < 1e-9,
          "busy: 103% is clamped to 100%");
    run[0] += 20000000;
    check(acc.feed(8000000, 640, run, usable, counted, N, &w) == Result::Dropped && !w.valid, "busy: 200% is garbage and the window is dropped");
    // All nodes uncounted: no figure.
    {
        const bool onlyCopy[2] = {false, false};
        const bool ok[2] = {true, true};
        uint64_t r2[2] = {0, 0};
        GpuBusyAccumulator a2;
        edvr::GpuBusyWindow w2;
        a2.feed(0, 0, r2, ok, onlyCopy, 2, &w2);
        r2[0] += 1000000;
        check(a2.feed(1000000, 90, r2, ok, onlyCopy, 2, &w2) == Result::Dropped && !w2.valid, "busy: a machine with no counted node has no figure");
    }
    // A node count that changes rebaselines; a bad call is refused.
    check(acc.feed(9000000, 730, run, usable, counted, 2, &w) == Result::Baseline, "busy: a changed node count starts over");
    check(acc.feed(9000000, 730, nullptr, usable, counted, 2, &w) == Result::Dropped, "busy: a null reading is refused");
    check(acc.feed(9000000, 730, run, usable, counted, edvr::kGpuBusyMaxNodes + 1, &w) == Result::Dropped, "busy: too many nodes is refused");
}

void testGpuBusyBoard() {
    edvr::GpuBusyBoard board;
    edvr::GpuBusyReading r = board.read(1000);
    check(!r.have && r.failure == edvr::GpuBusyFail::NotStarted, "board: before any window there is no figure and the reason is 'not started'");
    edvr::GpuBusyWindow w;
    w.valid = true;
    w.busyMsPerFrame = 5.4321;
    w.utilization = 0.4876;
    board.publish(w, 2000);
    r = board.read(2500);
    check(r.have && std::fabs(r.busyMs - 5.432) < 1e-9 && std::fabs(r.percent - 48.8) < 1e-9, "board: the published window reads back (0.001 ms, 0.1%)");
    r = board.read(2000 + edvr::kGpuBusyMaxAgeMs);
    check(r.have, "board: a figure exactly at the age limit is still current");
    r = board.read(2001 + edvr::kGpuBusyMaxAgeMs);
    check(!r.have, "board: a figure older than three seconds is gone");
    board.fail(edvr::GpuBusyFail::ReadFailed);
    r = board.read(2100);
    check(!r.have && r.failure == edvr::GpuBusyFail::ReadFailed, "board: a failure clears the figure and names the reason");
    board.publish(w, 3000);
    r = board.read(3100);
    check(r.have && r.failure == edvr::GpuBusyFail::None, "board: a figure after a failure clears the failure");
    edvr::GpuBusyWindow bad;   // not valid
    board.fail(edvr::GpuBusyFail::BadReadings);
    board.publish(bad, 3200);
    check(!board.read(3300).have, "board: an invalid window is never published");
    w.busyMsPerFrame = 1e12;   // beyond the packed field: saturates, no wrap
    w.utilization = 99.0;
    board.publish(w, 4000);
    r = board.read(4100);
    check(r.have && r.busyMs > 4000000.0 && r.percent <= 3276.8, "board: out-of-range values saturate");
    check(std::strcmp(edvr::gpuBusyFailText(edvr::GpuBusyFail::NoProcs), "D3DKMTQueryStatistics is not available on this system") == 0,
          "failure text: the missing-API reason");
}

void testGpuBusyLogText() {
    using namespace edvr;
    GpuBusyNodeInfo nodes[3];
    nodes[0].id = 0; nodes[0].engineType = 1; nodes[0].counted = true; nodes[0].usable = true;
    nodes[1].id = 4; nodes[1].engineType = 6; nodes[1].counted = false; nodes[1].usable = true;
    std::strcpy(nodes[2].name, "VR"); nodes[2].id = 11; nodes[2].engineType = 0; nodes[2].usable = false;
    GpuBusyLogWindow lw;
    // Two one-second windows: 90 frames at 5.0 ms (45%), then 60 frames at 8.0 ms (48%).
    GpuBusyWindow a;
    a.valid = true; a.wallMs = 1000.0; a.frames = 90.0; a.utilization = 0.45; a.busyMsPerFrame = 5.0; a.nodeCount = 3;
    a.nodeUtil[0] = 0.45; a.nodeUtil[1] = 0.01;
    GpuBusyWindow b = a;
    b.frames = 60.0; b.utilization = 0.48; b.busyMsPerFrame = 8.0; b.nodeUtil[0] = 0.48; b.nodeUtil[1] = 0.03;
    lw.add(a);
    lw.add(b);
    check(std::fabs(lw.busyMsPerFrame() - (5.0 * 90.0 + 8.0 * 60.0) / 150.0) < 1e-9, "log: the interval mean is weighted by frames");
    check(std::fabs(lw.percent() - 46.5) < 1e-9, "log: the interval percent is weighted by wall time");
    char line[400];
    formatGpuBusyLogLine(line, sizeof(line), lw, nodes, 3, 0, 9.7031);
    check(std::strcmp(line, "gpu busy (os): 6.200 ms/frame (46.5% of the period), span 9.703 ms, "
                            "source=d3dkmt nodes=0:3D=46.5%*,4:Copy=2.0%(not counted)") == 0,
          "log: the 30 s line's exact text");
    formatGpuBusyLogLine(line, sizeof(line), lw, nodes, 3, 0, -1.0);
    check(std::strstr(line, "span - ms") != nullptr, "log: no span in the interval prints a dash");
    formatGpuBusyStartLine(line, sizeof(line), "NVIDIA GeForce RTX 5090", 0, 0x13628, nodes, 3, 1000);
    check(std::strcmp(line, "gpu busy (os): adapter \"NVIDIA GeForce RTX 5090\" luid=00000000:00013628, nodes=0:3D*,4:Copy,11:VR(unreadable) "
                            "(* = counted: the 3D engines; busy = the busiest counted node), sampled every 1000 ms, source=d3dkmt") == 0,
          "log: the start-up line's exact text");
    formatGpuBusyFailLine(line, sizeof(line), GpuBusyFail::NoProcs);
    check(std::strcmp(line, "gpu busy (os): unavailable (D3DKMTQueryStatistics is not available on this system); the overlay shows \"--\" for GPU busy, "
                            "source=d3dkmt") == 0,
          "log: the failure line's exact text");
    char tiny[10];
    formatGpuBusyLogLine(tiny, sizeof(tiny), lw, nodes, 3, 0, 1.0);
    check(std::strlen(tiny) < sizeof(tiny), "log: a short buffer is truncated, not overrun");
}

void usage() {
    std::puts("usage: aa_off_test --self-test | --dry-run");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--dry-run") == 0) {
        std::puts("aa_off_test: dry run, nothing written");
        return 0;
    }
    if (argc != 2 || std::strcmp(argv[1], "--self-test") != 0) {
        usage();
        return 2;
    }
    testFastPathTable();
    testBindShadows();
    testThunkModel();
    testBindToggle();
    testSampler();
    testMapTotals();
    testGpuGuard();
    testGpuTimeTile();
    testFrameWorkSlack();
    testOverlayLine();
    testGpuBusyMaths();
    testGpuBusyBoard();
    testGpuBusyLogText();
    std::printf("aa_off_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
