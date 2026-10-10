// Build gate for the SchedulerStackProbe's capture/table logic -- the parts
// a flight would exercise at 36-540 calls/frame: the stride-tolerant stack
// scan (range filter, gap budget, cap, fault handling), the FNV-1a-64
// signature, and the per-target signature table (aggregation, overflow).
// arm()/finish() are NOT used (they would hook the test process); state is
// seeded directly, the pattern kinematic_probe_test set. The fault fixture
// runs against a page this process owns.
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <sstream>
#include <string>
#define private public
#include "../../src/plugins/diagnostics/scheduler_stack_probe.h"
#undef private
#include "../../src/common/log.h"
// The implementation under test is compiled INTO this TU: MSVC encodes
// access specifiers in decorated names, so a separately linked
// scheduler_stack_probe.obj would not resolve the private members this rig
// seeds (the kinematic_probe_test pattern).
#include "../../src/plugins/diagnostics/scheduler_stack_probe.cpp"
#include "../../src/plugins/diagnostics/scheduler_lifecycle.cpp"

namespace {
bool g_configEnabled = false;
unsigned g_attachCalls = 0, g_detachCalls = 0, g_logNotes = 0;
edvr::SchedulerStackProbe* g_attachedProbe = nullptr;
const char* g_attachResult = "installed";
}

namespace edvr {
// The lifecycle adapter and probe are real. Only the executable CodeHooks,
// config storage and disk logging are replaced; the fixture never patches
// the test process or touches a game/runtime/device.
const char* attachSchedulerStackHooks(SchedulerStackProbe* probe) noexcept {
    ++g_attachCalls;
    if (std::strcmp(g_attachResult, "installed") == 0) g_attachedProbe = probe;
    return g_attachResult;
}
void detachSchedulerStackHooks(SchedulerStackProbe* probe) noexcept {
    if (g_attachedProbe == probe) {
        ++g_detachCalls;
        g_attachedProbe = nullptr;
    }
}
Config& Config::get() { static Config config; return config; }
bool Config::getBool(const char* key, bool def) const {
    return std::strcmp(key, "advanced.scheduler_probe") == 0 ? g_configEnabled : def;
}
Log& Log::get() { static Log instance; return instance; }
Log::~Log() = default;
void Log::note(const char*, ...) { ++g_logNotes; }
}

using namespace edvr;

namespace {
unsigned checks = 0, failures = 0;
void check(bool ok, const char* name) {
    ++checks;
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", name); }
}

constexpr uintptr_t kBase = 0x100000000ull; // synthetic image base for the tests
constexpr uintptr_t kLo = kBase + SchedulerStackProbe::kRangeLo;
constexpr uintptr_t kHi = kBase + SchedulerStackProbe::kRangeHi;

// Seed as arm() leaves the probe, minus the hook install: cleared, active,
// imageBase_ carrying the synthetic base so crafted stacks fall in range.
void seed(SchedulerStackProbe& p) {
    p.clearLocked();
    p.imageBase_ = kBase;
    p.active_.store(true, std::memory_order_release);
}

// The capture: an in-range image with junk between the return addresses.
// image[i] slots chosen so hits and misses interleave within the gap
// budget; the scan must collect exactly the in-range values in order.
void caseCaptureCollectsInRangeWithGaps() {
    uint64_t image[64]{};
    image[0] = kLo + 0x100;          // immediate caller (the stub)
    image[1] = 0xDEADBEEFull;        // local, not a return address
    image[2] = kBase + 0x4321940ull; // scheduler candidate (.text, in range)
    image[3] = 0x1234ull;
    image[4] = 0x1235ull;
    image[5] = kHi - 0x10;           // deepest frame
    uint64_t out[SchedulerStackProbe::kStackCap]{};
    bool faulted = true;
    const uint32_t n = SchedulerStackProbe::captureStack(
        reinterpret_cast<uintptr_t>(&image[0]), kLo, kHi, out,
        SchedulerStackProbe::kStackCap, &faulted);
    check(n == 3, "capture: three in-range qwords collected");
    check(!faulted, "capture: no fault on an owned buffer");
    check(out[0] == image[0] && out[1] == image[2] && out[2] == image[5],
          "capture: hits in scan order, junk skipped");
    check(out[1] > kLo && out[1] < kHi, "capture: collected VA is inside the range");
}

// More than kMaxGaps consecutive misses end the scan: deeper real frames
// must not be reached across a region that cannot be a stack.
void caseCaptureGapBudget() {
    uint64_t image[64]{};
    image[0] = kLo + 0x100;
    for (uint32_t i = 1; i <= SchedulerStackProbe::kMaxGaps + 1; ++i)
        image[i] = 0x1000 + i; // all misses, one past the budget
    image[SchedulerStackProbe::kMaxGaps + 2] = kLo + 0x200; // beyond the wall
    uint64_t out[SchedulerStackProbe::kStackCap]{};
    bool faulted = false;
    const uint32_t n = SchedulerStackProbe::captureStack(
        reinterpret_cast<uintptr_t>(&image[0]), kLo, kHi, out,
        SchedulerStackProbe::kStackCap, &faulted);
    check(n == 1, "capture: the gap budget truncates the scan");
    check(out[0] == image[0], "capture: the prefix before the wall is kept");
}

// kStackCap in-range values: the signature is bounded even on an
// artificially dense stack.
void caseCaptureCap() {
    uint64_t image[64]{};
    for (uint32_t i = 0; i < 40; ++i) image[i] = kLo + i * 0x20;
    uint64_t out[SchedulerStackProbe::kStackCap]{};
    bool faulted = false;
    const uint32_t n = SchedulerStackProbe::captureStack(
        reinterpret_cast<uintptr_t>(&image[0]), kLo, kHi, out,
        SchedulerStackProbe::kStackCap, &faulted);
    check(n == SchedulerStackProbe::kStackCap, "capture: capped at kStackCap");
    check(out[SchedulerStackProbe::kStackCap - 1] == image[SchedulerStackProbe::kStackCap - 1],
          "capture: the cap keeps the deepest collected frame");
}

// A fault mid-scan keeps the collected prefix and says so: a garbage RSP
// must read as a faulted capture, never as a shallow stack.
void caseCaptureFaultKeepsPrefix() {
    // Two committed pages; the scan starts one qword before a NOACCESS page
    // so the second read faults.
    uint8_t* region = static_cast<uint8_t*>(VirtualAlloc(nullptr, 8192,
                                        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(region != nullptr, "capture fault: fixture pages allocated");
    if (!region) return;
    DWORD oldProtect = 0;
    check(VirtualProtect(region + 4096, 4096, PAGE_NOACCESS, &oldProtect) != 0,
          "capture fault: guard page armed");
    uint64_t* last = reinterpret_cast<uint64_t*>(region + 4096 - 8);
    *last = kLo + 0x100;
    uint64_t out[SchedulerStackProbe::kStackCap]{};
    bool faulted = false;
    const uint32_t n = SchedulerStackProbe::captureStack(
        reinterpret_cast<uintptr_t>(last), kLo, kHi, out,
        SchedulerStackProbe::kStackCap, &faulted);
    check(n == 1 && out[0] == *last, "capture fault: the prefix survives");
    check(faulted, "capture fault: the fault is reported");
    VirtualFree(region, 0, MEM_RELEASE);
}

// Golden FNV-1a-64 vectors (reference values computed offline; the empty
// list is the offset basis, the FNV spec's own check).
void caseFnvGolden() {
    const uint64_t three[] = {0x1462B4A0ull, 0x144321940ull, 0x1436A1000ull};
    check(SchedulerStackProbe::fnv1a64(three, 3) == 0xDA6A4E19B4964CA1ull,
          "fnv: three-word golden vector");
    const uint64_t one[] = {0x1462B4A0ull};
    check(SchedulerStackProbe::fnv1a64(one, 1) == 0x8744B2F324534AF7ull,
          "fnv: one-word golden vector");
    check(SchedulerStackProbe::fnv1a64(nullptr, 0) == 0xCBF29CE484222325ull,
          "fnv: the empty list is the offset basis");
}

// The table path: identical stacks aggregate under one signature with the
// call count, and an inactive probe counts nothing.
void caseNoteEntryAggregates() {
    SchedulerStackProbe p;
    seed(p);
    // Zeroed across the whole scan window, not just the slots used. noteEntry
    // reads until kMaxGaps misses in a row, and an 8-slot image left fewer
    // than kMaxGaps zeros after its last hit, so the scan ran past the array
    // into whatever the stack held there. An in-range leftover (the capture
    // cases above fill their images with in-range values) made the depth 3:
    // a pooled build failed "the exemplar kept the real depth" once in a run
    // where ten standalone runs of the same binary passed (2026-09-29).
    uint64_t image[SchedulerStackProbe::kScanSlots]{};
    image[0] = kLo + 0x100;
    image[1] = 0x99;
    image[2] = kLo + 0x200;
    const uintptr_t rsp = reinterpret_cast<uintptr_t>(&image[0]);
    p.noteEntry(0, rsp);
    p.noteEntry(0, rsp);
    SchedulerStackProbe::Summary s = p.summary();
    check(s.calls[0] == 2, "noteEntry: both calls counted");
    check(s.unique[0] == 1, "noteEntry: one signature for two identical stacks");
    check(s.overflow[0] == 0 && s.collisions[0] == 0, "noteEntry: no overflow, no collisions");
    {
        std::lock_guard<std::mutex> lock(p.mutex_);
        check(p.targets_[0].sigs[0].count == 2, "noteEntry: the signature's count aggregated");
        check(p.targets_[0].sigs[0].depth == 2, "noteEntry: the exemplar kept the real depth");
        check(p.targets_[0].sigs[0].stack[0] == image[0] &&
              p.targets_[0].sigs[0].stack[1] == image[2],
              "noteEntry: the exemplar is a raw sample of the stack");
    }
    check(s.calls[1] == 0 && s.calls[2] == 0 && s.calls[3] == 0,
          "noteEntry: other targets untouched");
    p.active_.store(false, std::memory_order_release);
    p.noteEntry(0, rsp);
    s = p.summary();
    check(s.calls[0] == 2, "noteEntry: an inactive probe counts nothing");
    check(s.faults[0] == 0, "noteEntry: a clean capture faults nothing");
}

// kSigCap distinct signatures: the 65th distinct one is counted as
// overflow, the table stays bounded, earlier signatures still aggregate.
void caseNoteEntryOverflow() {
    SchedulerStackProbe p;
    seed(p);
    // The whole scan window, zeroed, for the reason caseNoteEntryAggregates
    // gives: one hit then seven zeros is fewer than kMaxGaps.
    uint64_t image[SchedulerStackProbe::kScanSlots]{};
    const uintptr_t rsp = reinterpret_cast<uintptr_t>(&image[0]);
    for (uint32_t i = 0; i < SchedulerStackProbe::kSigCap + 6; ++i) {
        image[0] = kLo + 0x100 + i * 0x40; // distinct caller per signature
        p.noteEntry(2, rsp);
    }
    image[0] = kLo + 0x100; // back to the first signature
    p.noteEntry(2, rsp);
    const SchedulerStackProbe::Summary s = p.summary();
    check(s.calls[2] == SchedulerStackProbe::kSigCap + 7, "overflow: every call counted");
    check(s.unique[2] == SchedulerStackProbe::kSigCap, "overflow: the table stays at kSigCap");
    check(s.overflow[2] == 6, "overflow: only the distinct-past-cap calls overflow");
    {
        std::lock_guard<std::mutex> lock(p.mutex_);
        check(p.targets_[2].sigs[0].count == 2,
              "overflow: the surviving first signature still aggregates");
    }
}

// Exercise the production lifecycle record through its callback pointers.
// This checks the adapter, C ABI state guard and probe transitions together;
// the hook stub records attach/detach without installing executable patches.
void caseLifecycleAdapter() {
    auto& p = schedulerStackProbe;
    const auto& ops = plugins::diagnostics::schedulerLifecycleOps();
    void* const state = edvrDiagnosticsSchedulerState();
    void* const wrong = reinterpret_cast<void*>(uintptr_t(1));
    check(state == &p && state == ops.state,
          "lifecycle: one stable opaque process-lifetime identity");
    check(ops.configure && ops.frame && ops.shutdown,
          "lifecycle: actual configure, frame and shutdown callbacks");
    check(ops.componentIndex == 0 &&
          ops.manifestIndex == plugins::kPluginDiagnostics,
          "lifecycle: diagnostics component zero");

    g_configEnabled = false;
    ops.configure(nullptr);
    ops.configure(&Config::get());
    check(!p.active() && !g_attachedProbe && g_attachCalls == 0,
          "lifecycle: disabled configure never arms a hook");
    ops.frame(state, 11);
    check(p.frame_ == 0, "lifecycle: unarmed frame has no clock stamp");
    edvrDiagnosticsSchedulerConfigure(nullptr, 1);
    edvrDiagnosticsSchedulerConfigure(wrong, 1);
    check(g_attachCalls == 0, "lifecycle: invalid state cannot arm");

    g_configEnabled = true;
    ops.configure(&Config::get());
    check(p.active() && g_attachedProbe == &p && g_attachCalls == 1,
          "lifecycle: enabled configure attaches once");
    check(g_logNotes == 1, "lifecycle: first arm logs once");
    check(p.frame_ == 0 && p.armedTickMs_ == p.lastReportTickMs_,
          "lifecycle: arm clears the frame and starts the report clock");
    const unsigned liveNotes = g_logNotes;
    ops.configure(&Config::get());
    check(g_attachCalls == 1 && g_logNotes == liveNotes && p.frame_ == 0,
          "lifecycle: repeated enabled poll is idempotent");
    ops.frame(wrong, 12);
    ops.frame(nullptr, 13);
    check(p.frame_ == 0, "lifecycle: wrong or null frame state is inert");
    ops.frame(state, 14);
    check(p.frame_ == 14 && p.lastReportTickMs_ >= p.armedTickMs_,
          "lifecycle: owned frame stamp follows arm and report clock");
    ops.frame(state, 15);
    check(p.frame_ == 15, "lifecycle: frame stamp follows callback order");
    p.imageBase_ = kBase;
    uint64_t stack[SchedulerStackProbe::kScanSlots]{};
    stack[0] = kLo + 0x100;
    p.noteEntry(0, reinterpret_cast<uintptr_t>(&stack[0]));
    check(p.summary().calls[0] == 1,
          "lifecycle: an armed observation reaches the real probe table");

    edvrDiagnosticsSchedulerShutdown(wrong);
    edvrDiagnosticsSchedulerShutdown(nullptr);
    check(p.active() && g_detachCalls == 0,
          "lifecycle: invalid state cannot disarm");
    g_configEnabled = false;
    ops.configure(&Config::get());
    check(!p.active() && !g_attachedProbe && g_detachCalls == 1 &&
          p.frame_ == 0 && p.summary().calls[0] == 0,
          "lifecycle: disable detaches and clears the probe");
    check(g_logNotes == 2, "lifecycle: first disable logs once");
    ops.configure(&Config::get());
    ops.frame(state, 16);
    p.noteEntry(0, reinterpret_cast<uintptr_t>(&stack[0]));
    check(g_detachCalls == 1 && p.frame_ == 0 &&
          p.summary().calls[0] == 0,
          "lifecycle: repeated disabled poll, frame and entry stay inert");

    g_configEnabled = true;
    ops.configure(&Config::get());
    check(p.active() && g_attachCalls == 2 && g_attachedProbe == &p,
          "lifecycle: configure after disarm reattaches same static state");
    check(g_logNotes == 3, "lifecycle: rearm logs once");
    ops.shutdown(state);
    check(!p.active() && !g_attachedProbe && g_detachCalls == 2 &&
          p.frame_ == 0, "lifecycle: shutdown detaches and clears");
    check(g_logNotes == 4 && edvrDiagnosticsSchedulerState() == state,
          "lifecycle: shutdown logs once and preserves opaque storage");
    ops.shutdown(state);
    ops.frame(state, 17);
    check(g_detachCalls == 2 && p.frame_ == 0,
          "lifecycle: repeated shutdown and later frame stay inert");
}

void caseLifecycleHookRefusal() {
    auto& p = schedulerStackProbe;
    const auto& ops = plugins::diagnostics::schedulerLifecycleOps();
    const unsigned attaches = g_attachCalls, notes = g_logNotes;
    g_configEnabled = true;
    g_attachResult = "identity_mismatch";
    ops.configure(&Config::get());
    check(!p.active() && !g_attachedProbe &&
          p.hookStatus() == SchedulerStackProbe::HookStatus::IdentityMismatch &&
          g_attachCalls == attaches + 1 && g_logNotes == notes + 1,
          "lifecycle: identity refusal stands down and logs once");
    ops.configure(&Config::get());
    check(g_attachCalls == attaches + 2 && g_logNotes == notes + 1,
          "lifecycle: failed attach retries quietly on the next poll");
    g_attachResult = "opcode_mismatch";
    ops.configure(&Config::get());
    check(p.hookStatus() == SchedulerStackProbe::HookStatus::OpcodeMismatch &&
          g_logNotes == notes + 2,
          "lifecycle: a different refusal reason logs once");
    g_attachResult = "installed";
    ops.configure(&Config::get());
    check(p.active() && g_attachedProbe == &p && g_logNotes == notes + 3,
          "lifecycle: successful later poll arms and logs");
    ops.shutdown(ops.state);
    check(!p.active() && !g_attachedProbe && g_logNotes == notes + 4,
          "lifecycle: shutdown after retry disarms and logs");
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    if (!std::wcscmp(argv[1], L"--dry-run")) {
        std::puts("scheduler_stack_probe_test: dry-run (no runtime, device or files)");
        return 0;
    }
    if (std::wcscmp(argv[1], L"--self-test")) return 2;
    caseCaptureCollectsInRangeWithGaps();
    caseCaptureGapBudget();
    caseCaptureCap();
    caseCaptureFaultKeepsPrefix();
    caseFnvGolden();
    caseNoteEntryAggregates();
    caseNoteEntryOverflow();
    caseLifecycleAdapter();
    caseLifecycleHookRefusal();
    std::fprintf(stderr, "scheduler_stack_probe_test: %u checks, %u failures\n",
                 checks, failures);
    return failures ? 1 : 0;
}
