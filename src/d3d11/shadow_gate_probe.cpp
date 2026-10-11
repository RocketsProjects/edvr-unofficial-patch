// The shadow gate probe (shadow_gate_probe.h), engine side, v2 (2026-10-11, after flight 5). The pure half is in
// shadow_gate_core.h.
//
// Two windows after a press, both read-only:
//   * One frame (the frame after the press): the window FUN_144331300 (its output mask is G2), the callback body
//     FUN_14288A930 (G4; FUN_14288AC40 cannot be hooked), the builder bracket FUN_142B4420 (G1 and G3 per view, its
//     inputs as the builder sees them) and the bucket and record reads. It is attached only for this frame.
//   * 600 frames (the cadence): the DirectionalShadows.Depth builder 0x5B2C80 and the MSM pass 0x5B3530, counted per
//     frame, with their four arguments recorded (the first 256 calls). Their arguments are matched against the render
//     context's view array (viewIndexOf), so each view says whether a shadow builder named it. The docking state is
//     sampled every frame (Status Flags bit 0 and the Location line's Docked flag).
//
// Every detour forwards its arguments and returns the original's result. The planes test is called as the builder
// calls it (pure). Nothing is written to engine memory.
#include "shadow_gate_probe.h"

#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../common/code_hook.h"
#include "../common/config.h"
#include "../common/frame_flag.h"
#include "../common/log.h"
#include "cull_gate_probe.h"
#include "journal_watch.h"
#include "kinematic_eval_hook.h"
#include "shadow_gate_core.h"

#ifndef EDVR_VERSION_STRING
#define EDVR_VERSION_STRING "unversioned test build"
#endif

namespace sg = edvr::shadowgate;

namespace edvr {
namespace {

// The data-global the builder tests before its callback (DAT_145ea3399).
constexpr uintptr_t kVisGlobalRva = 0x5EA3399u;
constexpr uint32_t kBucketCap = 1024;
constexpr uint32_t kBuilderCap = 4096;
constexpr uint32_t kCallCap = 256;
constexpr uint32_t kCadenceN = sg::kCadenceFrames;   // 600
constexpr uint32_t kNoSlot = 0xFFFFFFFFu;
constexpr int kCandidateLines = 16;
constexpr int kSunLines = 64;
constexpr int kCallLines = 48;
constexpr uint64_t kDiagDelayMs = 20000;

struct BucketObs {
    uintptr_t bucket = 0, ctx = 0, base = 0;
    uint64_t count = 0, mask = 0, outMask = 0;
    uint64_t nibbles[4] = {};
    uint32_t lod = 0, frame = 0;
    uint8_t valid = 0;
};

struct BuilderObs {
    uintptr_t record = 0, pose = 0, ctx = 0;
    uint64_t mask = 0, recMask = 0, bitOk = 0, visApplies = 0;
    float centre[4] = {}, extents[4] = {};
    int8_t planes[sg::kMaxViews] = {};    // -1 outside, 0 straddle, 1 inside, 2 not evaluated
    int8_t g4[sg::kMaxViews] = {};        // 0 vetoed, 1 passed, -1 not evaluated
    uint8_t eligible[sg::kMaxViews] = {}; // the views whose callback the builder makes, in its order
    uint8_t eligibleCount = 0, centreChecked = 0, centreMatch = 0, viewCount = 0, valid = 0;
    uint32_t frame = 0, extra = 0;
};

struct CallObs {
    uint32_t frame = 0;
    uint8_t which = 0;                    // 0 depth, 1 MSM
    uintptr_t a = 0, b = 0, c = 0, d = 0;
};

// One row of the capture, before the sun roles are known: the CSV row and the candidate it belongs to.
struct CapRow {
    sg::CsvRow row;
    uint64_t gateBits = 0;
};

struct CandMeta {
    uint32_t index = 0;
    const char* kind = "";
    uintptr_t record = 0, bucket = 0;
    const char* role = "";
    float radius = 0, dist = 0;
};

// The capture's header facts, kept until the cadence ends (the report is written once).
struct CapHeader {
    uint32_t frame = 0;
    const char* trigger = "";
    uintptr_t ctx = 0;
    uint32_t views = 0;
    float camera[3] = {};
    uint32_t buckets = 0, records = 0, builders = 0, buildersDropped = 0, callbacks = 0, callbacksExtra = 0;
    uint32_t visRows = 0;
    uint32_t calleeCalls = 0;
    uintptr_t firstCalleeReturn = 0;
    bool builderAttached = false;
    std::string detours;
    bool haveCapture = false;
};

// ---------------------------------------------------------------------------
// Engine memory, read under SEH. POD only: a wild pointer is a false, never a fault.
bool readMem(void* dst, uintptr_t src, size_t n) noexcept {
    if (!src) return false;
    __try {
        std::memcpy(dst, reinterpret_cast<const void*>(src), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <typename T>
bool readVal(T* dst, uintptr_t src) noexcept { return readMem(dst, src, sizeof(T)); }

// FUN_1404F4E10 (view, point, interval) -> -1 outside, 0 straddle, 1 inside. Pure.
using PlanesFn = uint64_t(__fastcall*)(uintptr_t view, const float* point, const float* interval);
bool callPlanes(uintptr_t fn, uintptr_t view, const float* point, const float* interval, int* out) noexcept {
    __try {
        *out = static_cast<int>(static_cast<int32_t>(static_cast<uint32_t>(
            reinterpret_cast<PlanesFn>(fn)(view, point, interval))));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---------------------------------------------------------------------------
// Relay machinery, mirrored from kinematic_eval_hook.cpp (kept as a copy, as that file's note asks). Each detour has its
// own gate cell, so the cadence detours stay open while the one-frame detours do not.
constexpr size_t kRelayBytes = 44, kOriginalLiteral = 36;

uint8_t* allocateRelay(uintptr_t target) noexcept {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const uintptr_t granularity = info.dwAllocationGranularity;
    const uintptr_t floor = reinterpret_cast<uintptr_t>(info.lpMinimumApplicationAddress);
    const uintptr_t ceiling = reinterpret_cast<uintptr_t>(info.lpMaximumApplicationAddress);
    const uintptr_t distance = uintptr_t(INT32_MAX) - 0x10000u;
    uintptr_t at = target > distance ? target - distance : floor;
    if (at < floor) at = floor;
    const uintptr_t limit = target > ceiling - distance ? ceiling : target + distance;
    while (at < limit) {
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(reinterpret_cast<void*>(at), &region, sizeof(region))) break;
        const uintptr_t start = reinterpret_cast<uintptr_t>(region.BaseAddress);
        if (region.RegionSize > UINTPTR_MAX - start) break;
        const uintptr_t end = start + region.RegionSize;
        if (region.State == MEM_FREE) {
            uintptr_t candidate = at > start ? at : start;
            if (candidate > UINTPTR_MAX - (granularity - 1)) break;
            candidate = (candidate + granularity - 1) & ~(granularity - 1);
            if (candidate < limit && candidate < end && end - candidate >= 4096) {
                auto* p = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(candidate), 4096,
                                                             MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
                if (p) return p;
            }
        }
        if (end <= at) break;
        at = end;
    }
    return nullptr;
}

void buildRelay(uint8_t* code, const void* gate, void* callback) noexcept {
    // mov rax,&gate; cmp qword ptr[rax],0; je original; jmp [callback]; original: jmp [trampoline].
    const uint8_t body[kRelayBytes] = {
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0x48, 0x83, 0x38, 0,
        0x74, 0x0E, 0xFF, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0xFF, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    std::memcpy(code, body, sizeof(body));
    const uintptr_t gateAddress = reinterpret_cast<uintptr_t>(gate);
    const uintptr_t callbackAddress = reinterpret_cast<uintptr_t>(callback);
    std::memcpy(code + 2, &gateAddress, 8);
    std::memcpy(code + 22, &callbackAddress, 8);
}

struct ShadowHook {
    const char* name;
    uintptr_t rva;
    void* callback;
    const uint8_t* pin;
    size_t pinBytes;
    const void* gate;
    std::atomic<uintptr_t> forward{0};
    CodeHook hook;
    uint8_t* relay = nullptr;
    std::atomic<bool> ready{false};
};

bool prepareRelay(void* trampoline, void* context) noexcept {
    auto& h = *static_cast<ShadowHook*>(context);
    const uintptr_t address = reinterpret_cast<uintptr_t>(trampoline);
    std::memcpy(h.relay + kOriginalLiteral, &address, 8);
    DWORD oldProtect = 0;
    if (!VirtualProtect(h.relay, 4096, PAGE_EXECUTE_READ, &oldProtect) ||
        !FlushInstructionCache(GetCurrentProcess(), h.relay, kRelayBytes))
        return false;
    h.forward.store(address, std::memory_order_release);
    return true;
}

// ---------------------------------------------------------------------------
// The observers. Declared first: the detour table below takes their addresses.
using WindowFn = uintptr_t(__fastcall*)(uintptr_t collection, uintptr_t out, uintptr_t record);
using FourFn = uintptr_t(__fastcall*)(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d);

__declspec(noinline) uintptr_t __fastcall shadowWindowObserved(uintptr_t collection, uintptr_t out,
                                                               uintptr_t record) noexcept;
__declspec(noinline) uintptr_t __fastcall shadowCalleeObserved(uintptr_t a, uintptr_t b, uintptr_t c,
                                                               uintptr_t d) noexcept;
__declspec(noinline) uintptr_t __fastcall shadowDepthObserved(uintptr_t a, uintptr_t b, uintptr_t c,
                                                              uintptr_t d) noexcept;
__declspec(noinline) uintptr_t __fastcall shadowMsmObserved(uintptr_t a, uintptr_t b, uintptr_t c,
                                                            uintptr_t d) noexcept;
__declspec(noinline) uint32_t __fastcall shadowBuilderObserved(uintptr_t pose, uintptr_t ctx, uintptr_t mask,
                                                               uintptr_t nibbles) noexcept;

alignas(8) std::atomic<uintptr_t> g_captureGate{0};   // the window and callee detours: open for the one frame
alignas(8) std::atomic<uintptr_t> g_cadenceGate{0};   // the depth and MSM detours: open for the cadence
static_assert(decltype(g_captureGate)::is_always_lock_free, "The relay reads the aligned atomic cell directly.");
static_assert(decltype(g_cadenceGate)::is_always_lock_free, "The relay reads the aligned atomic cell directly.");

ShadowHook g_windowHook{"shadow-gate-window", sg::kWindowRva, reinterpret_cast<void*>(&shadowWindowObserved),
                        sg::kWindowPin, sizeof(sg::kWindowPin), &g_captureGate};
ShadowHook g_calleeHook{"shadow-gate-callee", sg::kCalleeRva, reinterpret_cast<void*>(&shadowCalleeObserved),
                        sg::kCalleePin, sizeof(sg::kCalleePin), &g_captureGate};
ShadowHook g_depthHook{"shadow-gate-depth", sg::kDepthRva, reinterpret_cast<void*>(&shadowDepthObserved),
                       sg::kDepthPin, sizeof(sg::kDepthPin), &g_cadenceGate};
ShadowHook g_msmHook{"shadow-gate-msm", sg::kMsmRva, reinterpret_cast<void*>(&shadowMsmObserved),
                     sg::kMsmPin, sizeof(sg::kMsmPin), &g_cadenceGate};

// The one-frame capture and the cadence. The render thread writes the targets and flags; the observers read them and
// fill preallocated rows through atomic reservations.
std::atomic<bool> g_armed{false};          // the one-frame capture's target frame is live
std::atomic<bool> g_cadencing{false};      // the cadence window is live
std::atomic<uint32_t> g_stamp{0};          // the frame the work now submitted belongs to
uint32_t g_target = 0;                     // the one-frame capture's frame
uint32_t g_cadenceFirst = 0, g_cadenceLast = 0;
std::atomic<uintptr_t> g_targetCtx{0};     // the render context the target frame's window saw
std::atomic<uint32_t> g_bucketNext{0}, g_builderNext{0}, g_callNext{0};
std::atomic<uint32_t> g_bucketDropped{0}, g_builderDropped{0}, g_callbacks{0}, g_callbackExtra{0};
std::atomic<uint32_t> g_calleeCalls{0};
std::atomic<uintptr_t> g_calleeFirstReturn{0};   // the return address of the callback's call to the callee, first call
std::atomic<uint32_t> g_depthPerFrame[kCadenceN];
std::atomic<uint32_t> g_msmPerFrame[kCadenceN];
int8_t g_dockStatus[kCadenceN] = {};       // Status Flags bit 0: 1 docked, 0 not, -1 unknown (render thread only)
int8_t g_dockLocation[kCadenceN] = {};     // the Location line's Docked flag: 1, 0, -1 (render thread only)
std::vector<BucketObs> g_bucketRows;
std::vector<BuilderObs> g_builderRows;
std::vector<CallObs> g_calls;
std::vector<CapRow> g_capRows;
std::vector<CandMeta> g_candMeta;
CapHeader g_header;
uintptr_t g_base = 0;
uint8_t g_visGlobal = 0;
bool g_planesOk = false;
bool g_calleeSiteOk = false;
bool g_builderAttached = false;

thread_local int32_t t_row = -1;      // the builder row of this thread's builder call
thread_local uint32_t t_seq = 0;      // callbacks seen in that call

// The window's pre-forward read: the bucket's fields. Returns its row, or kNoSlot.
uint32_t notePre(uintptr_t collection) noexcept {
    const uint32_t slot = g_bucketNext.fetch_add(1, std::memory_order_relaxed);
    if (slot >= kBucketCap) {
        g_bucketDropped.fetch_add(1, std::memory_order_relaxed);
        return kNoSlot;
    }
    BucketObs& b = g_bucketRows[slot];
    b = BucketObs{};
    b.bucket = collection;
    b.frame = g_target;
    readVal(&b.ctx, collection + sg::kBucketCtx);
    readVal(&b.base, collection + sg::kBucketBase);
    readVal(&b.count, collection + sg::kBucketCount);
    if (b.count > (1u << 20)) b.count = 1u << 20;
    readVal(&b.mask, collection + sg::kBucketMask);
    if (b.ctx) g_targetCtx.store(b.ctx, std::memory_order_relaxed);
    return slot;
}

// The window's post-forward read: the engine's output. Its mask is G2 (the replica that used to sit here disagreed
// with the engine about a fifth of the time, flight 5; it is gone).
void notePost(uint32_t slot, uintptr_t out, uintptr_t record) noexcept {
    BucketObs& b = g_bucketRows[slot];
    (void)record;
    uint64_t words[6] = {};
    if (!readMem(words, out, sizeof(words))) return;
    b.outMask = words[4];
    for (int i = 0; i < 4; ++i) b.nibbles[i] = words[i];
    b.lod = static_cast<uint32_t>(words[5]);
    b.valid = 1;
}

__declspec(noinline) uintptr_t __fastcall shadowWindowObserved(uintptr_t collection, uintptr_t out,
                                                               uintptr_t record) noexcept {
    const auto forward = reinterpret_cast<WindowFn>(g_windowHook.forward.load(std::memory_order_acquire));
    if (!forward) return out;
    uint32_t slot = kNoSlot;
    const bool live = g_armed.load(std::memory_order_acquire) && g_stamp.load(std::memory_order_acquire) == g_target;
    if (live) slot = notePre(collection);
    const uintptr_t result = forward(collection, out, record);
    if (slot != kNoSlot) notePost(slot, out, record);
    return result;
}

// The callback body's pre-forward read of the builder's centre.
void noteCentre(int32_t row, uintptr_t centreAddress) noexcept {
    BuilderObs& b = g_builderRows[static_cast<uint32_t>(row)];
    float c[4] = {};
    if (!readMem(c, centreAddress, sizeof(c))) return;
    bool same = true;
    for (int i = 0; i < 3; ++i) {
        const float tol = 1e-2f * (1.0f + std::fabs(b.centre[i]));
        if (!(std::fabs(c[i] - b.centre[i]) <= tol)) same = false;
    }
    b.centreChecked = 1;
    b.centreMatch = same ? 1 : 0;
}

void noteVerdict(int32_t row, uintptr_t result) noexcept {
    BuilderObs& b = g_builderRows[static_cast<uint32_t>(row)];
    const uint32_t seq = t_seq++;
    g_callbacks.fetch_add(1, std::memory_order_relaxed);
    const bool passed = (result & 0xFFu) != 0;
    if (seq < b.eligibleCount) {
        b.g4[b.eligible[seq]] = passed ? 1 : 0;
    } else {
        ++b.extra;
        g_callbackExtra.fetch_add(1, std::memory_order_relaxed);
    }
}

using CalleeFn = uintptr_t(__fastcall*)(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d);

__declspec(noinline) uintptr_t __fastcall shadowCalleeObserved(uintptr_t a, uintptr_t b, uintptr_t c,
                                                               uintptr_t d) noexcept {
    const auto forward = reinterpret_cast<CalleeFn>(g_calleeHook.forward.load(std::memory_order_acquire));
    if (!forward) return 0;
    bool fromBuilder = false;
    const int32_t row = t_row;
    if (g_armed.load(std::memory_order_acquire)) {
        // This observer was entered by a jump, so the return address of the callback's call to us is at our entry
        // stack; the callback's own return address (into the builder) sits 0x40 above it (FUN_14288AC40: sub rsp,38h
        // then call). The first such return is kept for the report: the check is applicable only when vis applies.
        const uintptr_t entry = reinterpret_cast<uintptr_t>(_AddressOfReturnAddress());
        uintptr_t callerReturn = 0;
        readVal(&callerReturn, entry + 0x40);
        g_calleeCalls.fetch_add(1, std::memory_order_relaxed);
        uintptr_t none = 0;
        g_calleeFirstReturn.compare_exchange_strong(none, callerReturn ? callerReturn : 1, std::memory_order_relaxed);
        if (row >= 0 && callerReturn == g_base + sg::kCallbackReturnRva) {
            fromBuilder = true;
            noteCentre(row, c);
        }
    }
    const uintptr_t result = forward(a, b, c, d);
    if (fromBuilder) noteVerdict(row, result);
    return result;
}

// The shadow builders' arguments, recorded per frame of the cadence (their counts are per frame; the first calls keep
// their arguments).
void noteCall(uint8_t which, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d) noexcept {
    const uint32_t stamp = g_stamp.load(std::memory_order_acquire);
    if (!g_cadencing.load(std::memory_order_acquire) || stamp < g_cadenceFirst || stamp > g_cadenceLast) return;
    const uint32_t idx = stamp - g_cadenceFirst;
    if (which == 0) g_depthPerFrame[idx].fetch_add(1, std::memory_order_relaxed);
    else g_msmPerFrame[idx].fetch_add(1, std::memory_order_relaxed);
    const uint32_t slot = g_callNext.fetch_add(1, std::memory_order_relaxed);
    if (slot < kCallCap) {
        CallObs& c2 = g_calls[slot];
        c2.frame = stamp;
        c2.which = which;
        c2.a = a;
        c2.b = b;
        c2.c = c;
        c2.d = d;
    }
}

__declspec(noinline) uintptr_t __fastcall shadowDepthObserved(uintptr_t a, uintptr_t b, uintptr_t c,
                                                              uintptr_t d) noexcept {
    const auto forward = reinterpret_cast<FourFn>(g_depthHook.forward.load(std::memory_order_acquire));
    if (!forward) return 0;
    noteCall(0, a, b, c, d);
    return forward(a, b, c, d);
}

__declspec(noinline) uintptr_t __fastcall shadowMsmObserved(uintptr_t a, uintptr_t b, uintptr_t c,
                                                            uintptr_t d) noexcept {
    const auto forward = reinterpret_cast<FourFn>(g_msmHook.forward.load(std::memory_order_acquire));
    if (!forward) return 0;
    noteCall(1, a, b, c, d);
    return forward(a, b, c, d);
}

// The builder's inputs before its forward, as cull_gate_probe.cpp's noteBuilder reads them.
__declspec(noinline) uint32_t __fastcall shadowBuilderObserved(uintptr_t pose, uintptr_t ctx, uintptr_t mask,
                                                               uintptr_t nibbles) noexcept {
    t_row = -1;
    t_seq = 0;
    if (!g_armed.load(std::memory_order_acquire) || g_stamp.load(std::memory_order_acquire) != g_target)
        return kGateProbeNoRow;
    const uint32_t slot = g_builderNext.fetch_add(1, std::memory_order_relaxed);
    if (slot >= kBuilderCap) {
        g_builderDropped.fetch_add(1, std::memory_order_relaxed);
        return kGateProbeNoRow;
    }
    BuilderObs& b = g_builderRows[slot];
    b = BuilderObs{};
    b.frame = g_target;
    b.pose = pose;
    b.ctx = ctx;
    b.mask = mask;
    b.record = nibbles >= 0x210 ? nibbles - 0x210 : 0;
    readVal(&b.recMask, b.record + sg::kRecMask);
    float q[4] = {}, t[4] = {}, c[4] = {}, e[4] = {};
    const bool poseOk = readMem(q, pose + sg::kPoseQuat, 16) && readMem(t, pose + sg::kPoseT, 16) &&
                        readMem(c, pose + sg::kPoseCentre, 16) && readMem(e, pose + sg::kPoseExtents, 16);
    if (poseOk) {
        sg::builderCentre(q, t, c, b.centre);
        std::memcpy(b.extents, e, sizeof(e));
    }
    uint64_t count = 0;
    readVal(&count, ctx + sg::kCtxCount);
    const uint32_t nViews = count > sg::kMaxViews ? sg::kMaxViews : static_cast<uint32_t>(count);
    b.viewCount = static_cast<uint8_t>(nViews);
    for (uint32_t v = 0; v < sg::kMaxViews; ++v) {
        b.planes[v] = 2;
        b.g4[v] = -1;
    }
    for (uint32_t v = 0; v < nViews; ++v) {
        const uintptr_t view = ctx + sg::kCtxViews + uintptr_t(v) * sg::kViewBytes;
        uint64_t bits = 0;
        uint8_t flags = 0;
        if (!readVal(&bits, view + sg::kViewBits) || !readVal(&flags, view + sg::kViewVisFlag)) continue;
        if (!(bits & mask)) continue;
        b.bitOk |= 1ull << v;
        const bool vis = g_visGlobal != 0 && (flags & 1u) != 0;
        if (vis) b.visApplies |= 1ull << v;
        if (!poseOk || !g_planesOk) continue;
        int r = 2;
        if (!callPlanes(g_base + sg::kPlanesRva, view, b.centre, b.extents, &r)) continue;
        b.planes[v] = static_cast<int8_t>(r);
        if (r != -1 && vis) b.eligible[b.eligibleCount++] = static_cast<uint8_t>(v);
    }
    b.valid = 1;
    t_row = static_cast<int32_t>(slot);
    return slot;
}

// ---------------------------------------------------------------------------
// Installing the detours and the builder bracket.

const char* installDetour(ShadowHook& h, uintptr_t base) noexcept {
    if (h.ready.load(std::memory_order_acquire)) return "installed";
    uint8_t got[16] = {};
    if (!readMem(got, base + h.rva, h.pinBytes) || !sg::pinMatches(got, h.pin, h.pinBytes))
        return "prologue mismatch";
    h.relay = allocateRelay(base + h.rva);
    if (!h.relay) return "no relay within range";
    buildRelay(h.relay, h.gate, h.callback);
    if (!h.hook.install(reinterpret_cast<void*>(base + h.rva), h.relay, nullptr, h.name, &prepareRelay, &h)) {
        VirtualFree(h.relay, 0, MEM_RELEASE);
        h.relay = nullptr;
        return "CodeHook refused (its own line says why)";
    }
    h.ready.store(true, std::memory_order_release);
    return "installed";
}

bool sitePinned(uintptr_t address, const uint8_t* pin, size_t n) noexcept {
    uint8_t got[8] = {};
    return readMem(got, address, n) && sg::pinMatches(got, pin, n);
}

// Installs whatever can be installed. Returns the status line; the probe runs without a refused detour.
std::string ensureDetours(uintptr_t base) {
    std::string status;
    g_planesOk = sitePinned(base + sg::kPlanesRva, sg::kPlanesPin, sizeof(sg::kPlanesPin));
    g_calleeSiteOk = sitePinned(base + sg::kCalleeSiteRva, sg::kCalleeSitePin, sizeof(sg::kCalleeSitePin)) &&
                     sitePinned(base + sg::kCallbackSiteRva, sg::kCallbackSitePin, sizeof(sg::kCallbackSitePin));
    status += std::string("window=") + installDetour(g_windowHook, base);
    status += std::string(" callee=") + (g_calleeSiteOk ? installDetour(g_calleeHook, base)
                                                         : "call sites do not match");
    status += std::string(" depth=") + installDetour(g_depthHook, base);
    status += std::string(" msm=") + installDetour(g_msmHook, base);
    status += std::string(" planes=") + (g_planesOk ? "pinned" : "prologue mismatch");
    return status;
}

// ---------------------------------------------------------------------------
// The dock state, sampled every frame of the cadence (render thread).
void sampleDock(uint32_t idx) noexcept {
    const JournalRawStatus raw = journalRawStatus();
    g_dockStatus[idx] = raw.flagsKnown ? static_cast<int8_t>(raw.flags & 1u) : -1;
    bool docked = false;
    g_dockLocation[idx] = journalLocationDocked(&docked) ? static_cast<int8_t>(docked ? 1 : 0) : -1;
}

// ---------------------------------------------------------------------------
// Diagnostic state (render thread only).
bool g_flat = false;
bool g_diagOn = false;
bool g_waitingForScene = false;
uint64_t g_requestAtMs = 0;
bool g_pending = false;
const char* g_pendingTrigger = "";
bool g_capturing = false;     // a one-frame capture is in progress (the frame after the press)

int bucketOf(uint32_t n, uintptr_t record) noexcept {
    for (uint32_t i = 0; i < n; ++i) {
        const BucketObs& b = g_bucketRows[i];
        if (!b.valid || !b.base) continue;
        const uintptr_t end = b.base + uintptr_t(b.count) * sg::kRecStride;
        if (record >= b.base && record < end) return static_cast<int>(i);
    }
    return -1;
}

// True when `target` is `node` or one of its descendants through the child lists, to a small depth.
bool isInTree(uintptr_t node, uintptr_t target, int depth, int& budget) noexcept {
    if (node == target) return true;
    if (depth <= 0 || --budget < 0) return false;
    uintptr_t list = 0;
    uint64_t count = 0;
    readVal(&list, node + sg::kRecChildren);
    readVal(&count, node + sg::kRecChildCount);
    if (!list || count == 0 || count > 4096) return false;
    for (uint64_t i = 0; i < count && budget > 0; ++i) {
        uintptr_t child = 0;
        if (!readVal(&child, list + uintptr_t(i) * 8)) break;
        if (child && isInTree(child, target, depth - 1, budget)) return true;
    }
    return false;
}

// The one-frame capture, at the frame after its target: the candidates (the largest-radius records whose sphere holds
// the camera, and the largest within 2 km), each record's rows for every view, and the callee facts. Read now, the frame's
// own state; the report is written when the cadence ends.
void finishCapture(uint32_t frame) {
    g_armed.store(false, std::memory_order_release);
    g_capRows.clear();
    g_candMeta.clear();
    kinematicEvalSetGateProbeObservers(nullptr, nullptr, nullptr);
    if (g_builderAttached) {
        kinematicEvalGateProbeDetach();
        g_builderAttached = false;
    }
    g_header = CapHeader{};
    g_header.frame = frame;
    g_header.trigger = g_pendingTrigger;
    g_header.detours = std::string();
    g_header.haveCapture = true;

    const uint32_t nb = g_bucketNext.load() < kBucketCap ? g_bucketNext.load() : kBucketCap;
    const uint32_t nr = g_builderNext.load() < kBuilderCap ? g_builderNext.load() : kBuilderCap;
    uintptr_t ctx = g_targetCtx.load(std::memory_order_acquire);
    if (!ctx)
        for (uint32_t i = 0; i < nb && !ctx; ++i)
            if (g_bucketRows[i].valid) ctx = g_bucketRows[i].ctx;
    float camera[3] = {};
    uint64_t viewCount = 0;
    if (ctx) {
        readMem(camera, ctx + sg::kCtxOrigin, sizeof(camera));
        readVal(&viewCount, ctx + sg::kCtxCount);
    }
    const uint32_t nViews = viewCount > sg::kMaxViews ? sg::kMaxViews : static_cast<uint32_t>(viewCount);
    uint64_t viewBits[sg::kMaxViews] = {};
    for (uint32_t v = 0; v < nViews; ++v)
        readVal(&viewBits[v], ctx + sg::kCtxViews + uintptr_t(v) * sg::kViewBytes + sg::kViewBits);

    sg::LargestRadius<sg::kCandidatesEach> containing, nearest;
    uint64_t scanned = 0;
    for (uint32_t i = 0; i < nb; ++i) {
        const BucketObs& b = g_bucketRows[i];
        if (!b.valid || !b.base) continue;
        for (uint64_t j = 0; j < b.count; ++j) {
            const uintptr_t rec = b.base + uintptr_t(j) * sg::kRecStride;
            float world[3] = {};
            float radius = 0;
            if (!readMem(world, rec + sg::kRecWorld, sizeof(world)) || !readVal(&radius, rec + sg::kRecRadius)) continue;
            if (!(radius > 0.0f)) continue;
            ++scanned;
            const sg::Candidate c = sg::makeCandidate(rec, b.bucket, world, radius, camera);
            if (c.contains) containing.offer(c);
            if (c.dist < sg::kNearMetres) nearest.offer(c);
        }
    }
    struct Pick { sg::Candidate c; bool contains; };
    std::vector<Pick> picks;
    for (size_t i = 0; i < containing.size(); ++i) picks.push_back({containing[i], true});
    for (size_t i = 0; i < nearest.size(); ++i) {
        bool dup = false;
        for (const Pick& p : picks) dup = dup || p.c.record == nearest[i].record;
        if (!dup) picks.push_back({nearest[i], false});
    }

    uint32_t index = 0;
    for (const Pick& p : picks) {
        const sg::Candidate& c = p.c;
        const int bi = bucketOf(nb, c.record);
        const BucketObs* bk = bi >= 0 ? &g_bucketRows[static_cast<uint32_t>(bi)] : nullptr;
        const char* role = "nobucket";
        if (bk) {
            if (c.record == bk->base) {
                role = "base";
            } else {
                int budget = 256;
                role = isInTree(bk->base, c.record, 3, budget) ? "child" : "orphan";
            }
        }
        uint64_t recMask = 0;
        readVal(&recMask, c.record + sg::kRecMask);
        int brow = -1;
        for (uint32_t r = 0; r < nr; ++r)
            if (g_builderRows[r].valid && g_builderRows[r].record == c.record) {
                brow = static_cast<int>(r);
                break;
            }
        CandMeta meta;
        meta.index = index;
        meta.kind = p.contains ? "contains" : "near";
        meta.record = c.record;
        meta.bucket = c.bucket;
        meta.role = role;
        meta.radius = c.radius;
        meta.dist = c.dist;
        g_candMeta.push_back(meta);
        for (uint32_t v = 0; v < nViews; ++v) {
            const bool g1 = bk && (bk->mask & viewBits[v]) != 0;
            sg::GateInput in;
            in.g1 = g1;
            in.windowKnown = bk && bk->valid;
            in.window = in.windowKnown && (bk->outMask & viewBits[v]) != 0;
            in.recMask = (recMask & viewBits[v]) != 0;
            if (brow >= 0) {
                const BuilderObs& br = g_builderRows[static_cast<uint32_t>(brow)];
                in.builderReached = true;
                in.planes = br.planes[v];
                in.visApplies = ((br.visApplies >> v) & 1ull) != 0;
                in.g4 = br.g4[v];
            }
            CapRow cr;
            sg::CsvRow& row = cr.row;
            row.candidate = index;
            row.kind = meta.kind;
            row.record = c.record;
            row.bucket = c.bucket;
            row.base = bk ? bk->base : 0;
            row.role = role;
            row.radius = c.radius;
            row.dist = c.dist;
            row.contains = c.contains ? 1 : 0;
            row.view = v;
            row.viewRole = v == 0 ? "main" : "other";
            row.viewBits = viewBits[v];
            row.g1 = g1 ? 1 : 0;
            row.windowBit = in.windowKnown ? (in.window ? 1 : 0) : -1;
            row.recBit = in.recMask ? 1 : 0;
            row.builderReached = brow >= 0 ? 1 : 0;
            row.builderRow = brow;
            row.builderMaskBit = brow >= 0 ? ((g_builderRows[static_cast<uint32_t>(brow)].mask & viewBits[v]) ? 1 : 0) : -1;
            row.planes = in.planes;
            row.visApplies = in.visApplies ? 1 : 0;
            row.g4 = in.g4;
            row.gate = sg::firstFailingGate(in);
            g_capRows.push_back(cr);
        }
        ++index;
    }

    // The callee facts, and how many builder rows had a callback to make (the check's applicability).
    uint32_t visRows = 0;
    for (uint32_t r = 0; r < nr; ++r)
        if (g_builderRows[r].valid && g_builderRows[r].visApplies != 0) ++visRows;
    g_header.ctx = ctx;
    g_header.views = nViews;
    g_header.camera[0] = camera[0];
    g_header.camera[1] = camera[1];
    g_header.camera[2] = camera[2];
    g_header.buckets = nb;
    g_header.records = static_cast<uint32_t>(scanned > 0xFFFFFFFFull ? 0xFFFFFFFFu : scanned);
    g_header.builders = nr;
    g_header.buildersDropped = g_builderDropped.load();
    g_header.callbacks = g_callbacks.load();
    g_header.callbacksExtra = g_callbackExtra.load();
    g_header.visRows = visRows;
    g_header.calleeCalls = g_calleeCalls.load();
    g_header.firstCalleeReturn = g_calleeFirstReturn.load();
    g_header.builderAttached = true;
}

// The cadence's end: the histogram, the view table with the shadow builders' hits, the call lines, and the reports
// (the capture's candidates first). The CSVs are written beside the graphics log.
void finishCadence(uint32_t frame) {
    g_cadencing.store(false, std::memory_order_release);
    const uint32_t nCalls = g_callNext.load() < kCallCap ? g_callNext.load() : kCallCap;
    const uintptr_t ctx = g_targetCtx.load(std::memory_order_acquire);
    uint64_t viewCount = 0;
    if (ctx) readVal(&viewCount, ctx + sg::kCtxCount);
    const uint32_t nViews = viewCount > sg::kMaxViews ? sg::kMaxViews : static_cast<uint32_t>(viewCount);

    // Per-view hits: every recorded shadow builder argument that is one of this context's view addresses.
    uint32_t depthHits[sg::kMaxViews] = {}, msmHits[sg::kMaxViews] = {};
    uint64_t sunMask = 0;
    for (uint32_t i = 0; i < nCalls; ++i) {
        const CallObs& c = g_calls[i];
        const uintptr_t args[4] = {c.a, c.b, c.c, c.d};
        for (uintptr_t arg : args) {
            const int k = sg::viewIndexOf(arg, ctx, nViews);
            if (k < 0) continue;
            sunMask |= 1ull << k;
            if (c.which == 0) ++depthHits[k];
            else ++msmHits[k];
        }
    }
    // Histograms of the cadence, and the dock classes.
    uint32_t depthHist[5] = {}, msmHist[5] = {};
    uint32_t dockedFrames = 0, outsideFrames = 0, unknownFrames = 0;
    uint32_t dockedDepth = 0, dockedMsm = 0, outsideDepth = 0, outsideMsm = 0;
    for (uint32_t idx = 0; idx < kCadenceN; ++idx) {
        const uint32_t d = g_depthPerFrame[idx].load();
        const uint32_t m = g_msmPerFrame[idx].load();
        sg::addToHistogram(depthHist, d);
        sg::addToHistogram(msmHist, m);
        const bool docked = g_dockStatus[idx] == 1 || g_dockLocation[idx] == 1;
        const bool outside = !docked && (g_dockStatus[idx] == 0 || g_dockLocation[idx] == 0);
        if (docked) {
            ++dockedFrames;
            dockedDepth += d;
            dockedMsm += m;
        } else if (outside) {
            ++outsideFrames;
            outsideDepth += d;
            outsideMsm += m;
        } else {
            ++unknownFrames;
        }
    }

    // Stamp and file names.
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char stamp[24] = {};
    std::snprintf(stamp, sizeof(stamp), "%04u%02u%02u_%02u%02u%02u", static_cast<unsigned>(st.wYear),
                  static_cast<unsigned>(st.wMonth), static_cast<unsigned>(st.wDay), static_cast<unsigned>(st.wHour),
                  static_cast<unsigned>(st.wMinute), static_cast<unsigned>(st.wSecond));
    char csvName[64], cadName[64];
    std::snprintf(csvName, sizeof(csvName), "shadow_gate_%s.csv", stamp);
    std::snprintf(cadName, sizeof(cadName), "shadow_cadence_%s.csv", stamp);
    auto logPath = [&](const wchar_t* prefix) {
        std::wstring p = Config::get().logDir() + L"\\" + prefix;
        for (const char* c = stamp; *c; ++c) p.push_back(static_cast<wchar_t>(*c));
        p += L".csv";
        return p;
    };

    // The capture's rows: the sun roles are known now. Rows for the sun views, the main view and every view the
    // bucket's mask holds.
    std::string sunList;
    for (uint32_t v = 0; v < nViews; ++v)
        if (sunMask & (1ull << v)) sunList += (sunList.empty() ? "" : ",") + std::to_string(v);
    FILE* f = nullptr;
    if (g_header.haveCapture) {
        f = _wfopen(logPath(L"shadow_gate_").c_str(), L"wb");
        if (f) std::fprintf(f, "%s\n", sg::csvHeader());
    }
    int linesLeft = kCandidateLines;
    for (const CandMeta& meta : g_candMeta) {
        std::string sunSummary, mainSummary;
        for (const CapRow& cr : g_capRows) {
            if (cr.row.candidate != meta.index) continue;
            const uint32_t v = cr.row.view;
            const bool sun = v < nViews && ((sunMask >> v) & 1ull);
            const bool main = v == 0;
            if (!sun && !main && !cr.row.g1) continue;
            sg::CsvRow row = cr.row;
            row.viewRole = sun ? "sun" : (main ? "main" : "other");
            if (f) {
                char line[512];
                sg::formatCsvRow(line, sizeof(line), g_header.frame, row);
                std::fprintf(f, "%s\n", line);
            }
            const std::string item = "v" + std::to_string(v) + "=" + sg::gateName(row.gate);
            if (sun && sunSummary.size() < 80) sunSummary += (sunSummary.empty() ? "" : " ") + item;
            if (main) mainSummary += (mainSummary.empty() ? "" : " ") + item;
        }
        if (linesLeft > 0) {
            sg::CandidateLine cl;
            cl.index = meta.index;
            cl.kind = meta.kind;
            cl.record = meta.record;
            cl.bucket = meta.bucket;
            cl.role = meta.role;
            cl.radius = meta.radius;
            cl.dist = meta.dist;
            cl.sun = sunSummary.c_str();
            cl.main = mainSummary.c_str();
            char line[512];
            sg::formatCandidateLine(line, sizeof(line), cl);
            Log::get().note("%s", line);
            --linesLeft;
        }
    }
    if (f) std::fclose(f);

    // The capture's header, and the callee check (applicable only when some row had a callback to make).
    if (g_header.haveCapture) {
        sg::HeaderInfo h;
        h.frame = g_header.frame;
        h.trigger = g_header.trigger;
        h.build = EDVR_VERSION_STRING;
        h.ctx = g_header.ctx;
        h.views = g_header.views;
        h.camera[0] = g_header.camera[0];
        h.camera[1] = g_header.camera[1];
        h.camera[2] = g_header.camera[2];
        h.main = 0;
        h.buckets = g_header.buckets;
        h.records = g_header.records;
        h.builders = g_header.builders;
        h.buildersDropped = g_header.buildersDropped;
        h.callbacks = g_header.callbacks;
        h.callbacksExtra = g_header.callbacksExtra;
        h.csvName = csvName;
        char header[600];
        sg::formatHeaderLine(header, sizeof(header), h);
        Log::get().note("%s", header);
        char check[200];
        sg::formatCalleeCheck(check, sizeof(check), g_header.visRows, g_header.calleeCalls, g_header.firstCalleeReturn,
                              g_base + sg::kCallbackReturnRva);
        Log::get().note("shadow gate probe: %s; sun=%s (cadence below)", check,
                        sunList.empty() ? "none" : sunList.c_str());
    } else {
        Log::get().note("shadow gate probe: the capture frame had no state (the builder bracket refused or no bucket).");
    }

    // The cadence: the histograms and the dock classes.
    char line[256];
    sg::formatHistogram(line, sizeof(line), "shadow cadence depth calls per frame:", depthHist);
    Log::get().note("%s over %u frames (%u..%u)", line, kCadenceN, g_cadenceFirst, g_cadenceLast);
    sg::formatHistogram(line, sizeof(line), "shadow cadence MSM calls per frame:", msmHist);
    Log::get().note("%s", line);
    Log::get().note("shadow cadence dock: docked frames=%u (depth=%u msm=%u) outside frames=%u (depth=%u msm=%u) "
                    "unknown frames=%u", dockedFrames, dockedDepth, dockedMsm, outsideFrames, outsideDepth, outsideMsm,
                    unknownFrames);

    // The views: each one's pointer, mask bits and window constants, and whether a shadow builder named it.
    int viewLines = kSunLines;
    for (uint32_t k = 0; k < nViews && viewLines > 0; ++k, --viewLines) {
        const uintptr_t ptr = ctx + sg::kCtxViews + uintptr_t(k) * sg::kViewBytes;
        uint64_t bits = 0;
        float k1 = 0, k2 = 0, origin[3] = {};
        readVal(&bits, ptr + sg::kViewBits);
        readVal(&k1, ptr + sg::kViewK1);
        readVal(&k2, ptr + sg::kViewK2);
        readMem(origin, ptr + sg::kViewOrigin, sizeof(origin));
        char vl[400];
        sg::formatSunViewLine(vl, sizeof(vl), k, ptr, bits, k1, k2, origin, depthHits[k], msmHits[k]);
        Log::get().note("%s", vl);
    }
    // The recorded calls, with the view each argument named.
    int callLines = kCallLines;
    for (uint32_t i = 0; i < nCalls && callLines > 0; ++i, --callLines) {
        const CallObs& c = g_calls[i];
        int k = sg::viewIndexOf(c.a, ctx, nViews);
        if (k < 0) k = sg::viewIndexOf(c.b, ctx, nViews);
        if (k < 0) k = sg::viewIndexOf(c.c, ctx, nViews);
        if (k < 0) k = sg::viewIndexOf(c.d, ctx, nViews);
        char cl[300];
        sg::formatCallLine(cl, sizeof(cl), i, c.which ? "msm" : "depth", c.frame, c.a, c.b, c.c, c.d, k);
        Log::get().note("%s", cl);
    }

    // The cadence CSV: one row per frame.
    FILE* cf = _wfopen(logPath(L"shadow_cadence_").c_str(), L"wb");
    if (cf) {
        std::fprintf(cf, "%s\n", sg::cadenceHeader());
        for (uint32_t idx = 0; idx < kCadenceN; ++idx) {
            char row[160];
            sg::formatCadenceRow(row, sizeof(row), g_cadenceFirst + idx, g_dockStatus[idx], g_dockLocation[idx],
                                 g_depthPerFrame[idx].load(), g_msmPerFrame[idx].load());
            std::fprintf(cf, "%s\n", row);
        }
        std::fclose(cf);
    } else {
        Log::get().note("shadow gate probe: could not open edvr_logs\\%s; the lines above are complete.", cadName);
    }
    Log::get().note("shadow gate probe: done frame=%u cadence=%u..%u calls=%u (kept %u) csv=%s cadence_csv=%s",
                    frame, g_cadenceFirst, g_cadenceLast, g_callNext.load(), nCalls, csvName, cadName);
    g_capRows.clear();
    g_candMeta.clear();
}

// Starts the capture and the cadence at the frame `first`.
void startAll(uint32_t first, const char* trigger) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!base) {
        Log::get().note("shadow gate probe: refused (trigger %s): the module base is unknown.", trigger);
        return;
    }
    g_base = base;
    const std::string detours = ensureDetours(base);
    kinematicEvalSetGateProbeObservers(nullptr, &shadowBuilderObserved, nullptr);
    const char* attach = kinematicEvalGateProbeAttach();
    g_builderAttached = std::strcmp(attach, "installed") == 0;
    if (!g_builderAttached) kinematicEvalSetGateProbeObservers(nullptr, nullptr, nullptr);
    if (g_bucketRows.size() != kBucketCap) g_bucketRows.assign(kBucketCap, BucketObs{});
    if (g_builderRows.size() != kBuilderCap) g_builderRows.assign(kBuilderCap, BuilderObs{});
    if (g_calls.size() != kCallCap) g_calls.assign(kCallCap, CallObs{});
    g_visGlobal = 0;
    readMem(&g_visGlobal, base + kVisGlobalRva, 1);
    g_bucketNext.store(0);
    g_builderNext.store(0);
    g_callNext.store(0);
    g_bucketDropped.store(0);
    g_builderDropped.store(0);
    g_callbacks.store(0);
    g_callbackExtra.store(0);
    g_calleeCalls.store(0);
    g_calleeFirstReturn.store(0);
    g_targetCtx.store(0);
    for (uint32_t idx = 0; idx < kCadenceN; ++idx) {
        g_depthPerFrame[idx].store(0);
        g_msmPerFrame[idx].store(0);
        g_dockStatus[idx] = -1;
        g_dockLocation[idx] = -1;
    }
    g_target = first;
    g_cadenceFirst = first;
    g_cadenceLast = first + kCadenceN - 1;
    g_capturing = true;
    g_cadencing.store(true, std::memory_order_release);
    g_cadenceGate.store(1, std::memory_order_release);
    g_armed.store(true, std::memory_order_release);
    g_captureGate.store(1, std::memory_order_release);
    Log::get().note("shadow gate probe: armed for frame %u (trigger %s; the cadence runs to frame %u; builder bracket %s; "
                    "detours: %s; visibility global %u).", first, trigger, first + kCadenceN - 1, attach, detours.c_str(),
                    static_cast<unsigned>(g_visGlobal));
}

}  // namespace

void shadowGateProbeInstall(bool flatProfile) {
    g_flat = flatProfile;
    const std::string v = Config::get().getString("diag.shadow_gate_probe", "off");
    if (v == "on") {
        g_diagOn = true;
        g_waitingForScene = !flatProfile;
        if (flatProfile)
            Log::get().note("shadow gate probe: diag.shadow_gate_probe = on, but flat has no scene flag, so it waits and "
                            "never fires: bind hotkey.shadow_gate_probe there.");
        else
            Log::get().note("shadow gate probe: diag.shadow_gate_probe = on: one capture of one frame and a 600-frame "
                            "cadence, 20 s after the first in-scene frame.");
    } else if (v != "off" && !v.empty()) {
        Log::get().note("shadow gate probe: diag.shadow_gate_probe = '%s' is not on or off; treated as off.", v.c_str());
    } else {
        Log::get().note("shadow gate probe: diag.shadow_gate_probe = off: nothing is hooked until hotkey.shadow_gate_probe "
                        "is pressed.");
    }
}

void shadowGateProbeRequest(const char* trigger) {
    if (g_capturing || g_cadencing.load() || g_pending) {
        Log::get().note("shadow gate probe: a capture is already %s; the request (%s) is refused.",
                        g_pending ? "pending" : "running", trigger);
        return;
    }
    g_pending = true;
    g_pendingTrigger = trigger;
    Log::get().note("shadow gate probe: requested by %s; one frame is captured from the next frame, with the 600-frame "
                    "cadence.", trigger);
}

void shadowGateProbeFrame(uint32_t frame) {
    // The work that just finished is stamped `frame`.
    if (g_cadencing.load(std::memory_order_acquire) && frame >= g_cadenceFirst && frame <= g_cadenceLast)
        sampleDock(frame - g_cadenceFirst);
    if (g_capturing && frame >= g_target) {
        g_capturing = false;
        g_captureGate.store(0, std::memory_order_release);
        finishCapture(frame);
    }
    if (g_cadencing.load(std::memory_order_acquire) && frame >= g_cadenceLast) {
        g_cadenceGate.store(0, std::memory_order_release);
        finishCadence(frame);
    }
    // The work after this tick is stamped frame + 1.
    g_stamp.store(frame + 1, std::memory_order_release);
    if (g_waitingForScene) {
        if (!sceneArrived()) return;
        g_waitingForScene = false;
        g_requestAtMs = GetTickCount64() + kDiagDelayMs;
        Log::get().note("shadow gate probe: the first in-scene frame; the capture is requested in 20 s.");
    }
    if (g_requestAtMs && GetTickCount64() >= g_requestAtMs) {
        g_requestAtMs = 0;
        if (!g_capturing && !g_cadencing.load() && !g_pending) {
            g_pending = true;
            g_pendingTrigger = "diag.shadow_gate_probe = on";
        }
    }
    if (g_pending && !g_capturing && !g_cadencing.load()) {
        g_pending = false;
        startAll(frame + 1, g_pendingTrigger);
    }
}

}  // namespace edvr
