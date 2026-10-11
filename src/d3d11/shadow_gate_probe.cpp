// The shadow gate probe (shadow_gate_probe.h): the engine side. The pure half
// (layout, the window and builder transforms, the gate decode, the line and CSV
// formats) is in shadow_gate_core.h; this file hooks the engine, captures one
// frame and writes the report.
//
// Four detours and the builder bracket, all read-only:
//   * FUN_144331300 (the window): its first argument is the collection (bucket),
//     its third the bucket's first record; it writes the mask (out[4]) the builder
//     then receives. The observer reads the bucket before the forward and the
//     output and the base record's replicated window after it.
//   * FUN_14288A930, the callback body. FUN_14288AC40 (the callback itself) cannot
//     be CodeHooked: its first instruction is an SSE move. It is a pure forwarder
//     (broadcast [r9], call FUN_14288A930, add rsp, ret), so the callee's AL is the
//     callback's AL. The observer attributes a call to the builder only when the
//     callback was called from the builder's own call site (the return address
//     found through the callee's frame, pinned by kCallbackReturnRva).
//   * FUN_1405B2C80 (DirectionalShadows.Depth) and FUN_1405B3530 (MSM): the object
//     each receives first. Matched against the render context's view array, they
//     name the sun views directly; unmatched, the probe says so.
//   * FUN_142B4420 (the builder), through the gate probe's own observer slot
//     (kinematic_eval_hook.cpp): its inputs before the forward, its per-view G1 and
//     G3 (the planes test recomputed on the builder's own box, the same as
//     cull_gate_probe.cpp) and the callback sequence for G4.
//
// Every detour forwards its arguments and returns the original's result. The
// planes test is called as the builder calls it (pure). Nothing is written to
// engine memory. The probe is one frame long: relays and the builder gate are
// open only while a capture runs.
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
constexpr uint32_t kDepthCap = 256;
constexpr uint32_t kNoSlot = 0xFFFFFFFFu;
constexpr int kLogLineCap = 40;
constexpr uint64_t kDiagDelayMs = 20000;

struct BucketObs {
    uintptr_t bucket = 0, ctx = 0, base = 0, table = 0, baseRecord = 0;
    uint64_t count = 0, mask = 0, outMask = 0, replMask = 0;
    uint64_t nibbles[4] = {};
    uint32_t lod = 0, tableCount = 0, frame = 0;
    float t0 = 0, lodScale = 0;
    float v[sg::kMaxViews] = {};
    uint8_t valid = 0, baseOk = 0;
};

struct BuilderObs {
    uintptr_t record = 0, pose = 0, ctx = 0;
    uint64_t mask = 0, recMask = 0, bitOk = 0, visApplies = 0;
    float centre[4] = {}, extents[4] = {};
    int8_t planes[sg::kMaxViews] = {};   // -1 outside, 0 straddle, 1 inside, 2 not evaluated
    int8_t g4[sg::kMaxViews] = {};       // 0 vetoed, 1 passed, -1 not evaluated
    uint8_t eligible[sg::kMaxViews] = {};// the views whose callback the builder makes, in its order
    uint8_t eligibleCount = 0, centreChecked = 0, centreMatch = 0, viewCount = 0, valid = 0;
    uint32_t frame = 0, extra = 0;
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
// Relay machinery, mirrored from kinematic_eval_hook.cpp (kept as a copy, as that
// file's note asks: its flight-proven relays are not touched).
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
__declspec(noinline) uintptr_t __fastcall shadowCalleeObserved(uintptr_t ctx, uintptr_t dd, uintptr_t centre,
                                                               uintptr_t extent) noexcept;
__declspec(noinline) uintptr_t __fastcall shadowDepthObserved(uintptr_t a, uintptr_t b, uintptr_t c,
                                                              uintptr_t d) noexcept;
__declspec(noinline) uintptr_t __fastcall shadowMsmObserved(uintptr_t a, uintptr_t b, uintptr_t c,
                                                            uintptr_t d) noexcept;
__declspec(noinline) uint32_t __fastcall shadowBuilderObserved(uintptr_t pose, uintptr_t ctx, uintptr_t mask,
                                                               uintptr_t nibbles) noexcept;

alignas(8) std::atomic<uintptr_t> g_shadowGate{0};
static_assert(decltype(g_shadowGate)::is_always_lock_free, "The relay reads the aligned atomic cell directly.");

ShadowHook g_windowHook{"shadow-gate-window", sg::kWindowRva, reinterpret_cast<void*>(&shadowWindowObserved),
                        sg::kWindowPin, sizeof(sg::kWindowPin)};
ShadowHook g_calleeHook{"shadow-gate-callee", sg::kCalleeRva, reinterpret_cast<void*>(&shadowCalleeObserved),
                        sg::kCalleePin, sizeof(sg::kCalleePin)};
ShadowHook g_depthHook{"shadow-gate-depth", sg::kDepthRva, reinterpret_cast<void*>(&shadowDepthObserved),
                       sg::kDepthPin, sizeof(sg::kDepthPin)};
ShadowHook g_msmHook{"shadow-gate-msm", sg::kMsmRva, reinterpret_cast<void*>(&shadowMsmObserved),
                     sg::kMsmPin, sizeof(sg::kMsmPin)};

// Capture state. The render thread writes the target and the flags; the observers
// read them and fill preallocated rows through atomic reservations.
std::atomic<bool> g_armed{false};
std::atomic<uint32_t> g_stamp{0};          // the frame the work now submitted belongs to
uint32_t g_target = 0;                     // the frame being captured
std::atomic<uint32_t> g_bucketNext{0}, g_builderNext{0}, g_depthNext{0}, g_msmNext{0};
std::atomic<uint32_t> g_bucketDropped{0}, g_builderDropped{0}, g_callbacks{0}, g_callbackExtra{0};
std::atomic<uint32_t> g_windowMismatch{0};
std::atomic<uint32_t> g_calleeCalls{0};
std::atomic<uintptr_t> g_calleeFirstReturn{0};   // the return address of the callback's call to the callee, first call
std::vector<BucketObs> g_bucketRows;
std::vector<BuilderObs> g_builderRows;
uintptr_t g_depthObjects[kDepthCap] = {};
uintptr_t g_msmObjects[kDepthCap] = {};
uintptr_t g_base = 0;
uint8_t g_visGlobal = 0;
bool g_planesOk = false;
bool g_calleeSiteOk = false;

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
    readVal(&b.table, collection + sg::kBucketTable);
    readVal(&b.base, collection + sg::kBucketBase);
    readVal(&b.count, collection + sg::kBucketCount);
    if (b.count > (1u << 20)) b.count = 1u << 20;
    readVal(&b.mask, collection + sg::kBucketMask);
    return slot;
}

// The window's post-forward read: its output, and the base record's window
// replicated from the same inputs (values for the log; the engine's verdict is
// the output's bits).
void notePost(uint32_t slot, uintptr_t collection, uintptr_t out, uintptr_t record) noexcept {
    BucketObs& b = g_bucketRows[slot];
    (void)collection;
    uint64_t words[6] = {};
    if (!readMem(words, out, sizeof(words))) return;
    // The engine's own output is the verdict; it stands even when the replicated
    // values below cannot be read.
    b.outMask = words[4];
    for (int i = 0; i < 4; ++i) b.nibbles[i] = words[i];
    b.lod = static_cast<uint32_t>(words[5]);
    b.baseRecord = record;
    b.baseOk = 1;
    b.valid = 1;
    uintptr_t q = 0, m = 0;
    float qf[16] = {}, mf[16] = {}, local[3] = {};
    float radius = 0;
    readVal(&q, record + sg::kRecPosePtr);
    if (q) readVal(&m, q + 0x50);
    const bool poseOk = q && m && readMem(qf, q, 15 * sizeof(float)) && readMem(mf, m, 15 * sizeof(float));
    readMem(local, record + sg::kRecLocal, sizeof(local));
    readVal(&radius, record + sg::kRecRadius);
    readVal(&b.lodScale, b.ctx + sg::kCtxLodScale);
    if (b.table) {
        readVal(&b.t0, b.table);
        readVal(&b.tableCount, b.table + sg::kTableCount);
    }
    uint64_t views = 0;
    readVal(&views, b.ctx + sg::kCtxCount);
    const uint32_t nViews = views > sg::kMaxViews ? sg::kMaxViews : static_cast<uint32_t>(views);
    if (!poseOk || !b.table || !b.ctx) return;
    const sg::Vec3 centre = sg::windowCentre(qf, mf, local);
    uint32_t mismatch = 0;
    for (uint32_t v = 0; v < nViews; ++v) {
        const uintptr_t view = b.ctx + sg::kCtxViews + uintptr_t(v) * sg::kViewBytes;
        float geo[9] = {};   // origin at geo[0..2], k1 at geo[4] (+0x550), k2 at geo[8] (+0x560)
        uint64_t bits = 0;
        if (!readMem(geo, view + sg::kViewOrigin, sizeof(geo)) || !readVal(&bits, view + sg::kViewBits)) continue;
        const float value = sg::windowValue(centre, geo, radius, geo[4], geo[8], b.lodScale, nullptr);
        b.v[v] = value;
        const bool repl = (b.mask & bits) != 0 && sg::windowPasses(b.t0, value);
        if (repl) b.replMask |= 1ull << v;
        if (((b.outMask & bits) != 0) != repl) ++mismatch;
    }
    if (mismatch) g_windowMismatch.fetch_add(mismatch, std::memory_order_relaxed);
}

__declspec(noinline) uintptr_t __fastcall shadowWindowObserved(uintptr_t collection, uintptr_t out,
                                                               uintptr_t record) noexcept {
    const auto forward = reinterpret_cast<WindowFn>(g_windowHook.forward.load(std::memory_order_acquire));
    if (!forward) return out;
    uint32_t slot = kNoSlot;
    const bool live = g_armed.load(std::memory_order_acquire) &&
                      g_stamp.load(std::memory_order_acquire) == g_target;
    if (live) slot = notePre(collection);
    const uintptr_t result = forward(collection, out, record);
    if (slot != kNoSlot) notePost(slot, collection, out, record);
    return result;
}

// The callback body's pre-forward read of the builder's centre, and the
// callee's verdict, attributed to the builder row by the return address.
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

using CalleeFn = uintptr_t(__fastcall*)(uintptr_t ctx, uintptr_t dd, uintptr_t centre, uintptr_t extent);

__declspec(noinline) uintptr_t __fastcall shadowCalleeObserved(uintptr_t a, uintptr_t b, uintptr_t c,
                                                               uintptr_t d) noexcept {
    const auto forward = reinterpret_cast<CalleeFn>(g_calleeHook.forward.load(std::memory_order_acquire));
    if (!forward) return 0;
    bool fromBuilder = false;
    const int32_t row = t_row;
    if (g_armed.load(std::memory_order_acquire)) {
        // This observer was entered by a jump, so the return address of the
        // callback's call to us is at our entry stack; the callback's own return
        // address (into the builder) sits 0x40 above it (FUN_14288AC40: sub rsp,38h
        // then call; the callee's frame is 0x38 + 8 below the callback's return).
        // The first such return is kept for the report, so a mismatch shows as its address.
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

void noteDepth(std::atomic<uint32_t>& next, uintptr_t* objects, uintptr_t a) noexcept {
    const uint32_t slot = next.fetch_add(1, std::memory_order_relaxed);
    if (slot < kDepthCap) objects[slot] = a;
}

__declspec(noinline) uintptr_t __fastcall shadowDepthObserved(uintptr_t a, uintptr_t b, uintptr_t c,
                                                              uintptr_t d) noexcept {
    const auto forward = reinterpret_cast<FourFn>(g_depthHook.forward.load(std::memory_order_acquire));
    if (!forward) return 0;
    if (g_armed.load(std::memory_order_acquire) && g_stamp.load(std::memory_order_acquire) == g_target)
        noteDepth(g_depthNext, g_depthObjects, a);
    return forward(a, b, c, d);
}

__declspec(noinline) uintptr_t __fastcall shadowMsmObserved(uintptr_t a, uintptr_t b, uintptr_t c,
                                                            uintptr_t d) noexcept {
    const auto forward = reinterpret_cast<FourFn>(g_msmHook.forward.load(std::memory_order_acquire));
    if (!forward) return 0;
    if (g_armed.load(std::memory_order_acquire) && g_stamp.load(std::memory_order_acquire) == g_target)
        noteDepth(g_msmNext, g_msmObjects, a);
    return forward(a, b, c, d);
}

// The builder's inputs before its forward, as cull_gate_probe.cpp's noteBuilder
// reads them: the record from the nibble pointer, the box from the pose context,
// and per view G1 (view bits & mask), the visibility flag, G3 (the planes test
// on the builder's box) and the callback's eligibility (G4 order).
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
    buildRelay(h.relay, &g_shadowGate, h.callback);
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

// Installs whatever can be installed. Returns the status line; the probe runs
// without a refused detour (its rows just say so).
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
// Diagnostic state (render thread only).
bool g_flat = false;
bool g_diagOn = false;
bool g_waitingForScene = false;
uint64_t g_requestAtMs = 0;
bool g_pending = false;
const char* g_pendingTrigger = "";

void startCapture(uint32_t target, const char* trigger) {
    if (cullGateProbe.armed()) {
        Log::get().note("shadow gate probe: refused (trigger %s): the cull gate probe owns the builder observers "
                        "(advanced.cull_gate_capture is armed in this eye run).", trigger);
        return;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!base) {
        Log::get().note("shadow gate probe: refused (trigger %s): the module base is unknown.", trigger);
        return;
    }
    g_base = base;
    const std::string detours = ensureDetours(base);
    kinematicEvalSetGateProbeObservers(nullptr, &shadowBuilderObserved, nullptr);
    const char* attach = kinematicEvalGateProbeAttach();
    if (std::strcmp(attach, "installed") != 0) {
        kinematicEvalSetGateProbeObservers(nullptr, nullptr, nullptr);
        Log::get().note("shadow gate probe: refused (trigger %s): the builder bracket did not attach (%s).", trigger,
                        attach);
        return;
    }
    if (g_bucketRows.size() != kBucketCap) g_bucketRows.assign(kBucketCap, BucketObs{});
    if (g_builderRows.size() != kBuilderCap) g_builderRows.assign(kBuilderCap, BuilderObs{});
    g_visGlobal = 0;
    readMem(&g_visGlobal, base + kVisGlobalRva, 1);
    g_bucketNext.store(0);
    g_builderNext.store(0);
    g_depthNext.store(0);
    g_msmNext.store(0);
    g_bucketDropped.store(0);
    g_builderDropped.store(0);
    g_callbacks.store(0);
    g_callbackExtra.store(0);
    g_windowMismatch.store(0);
    g_calleeCalls.store(0);
    g_calleeFirstReturn.store(0);
    g_target = target;
    g_armed.store(true, std::memory_order_release);
    g_shadowGate.store(1, std::memory_order_release);
    Log::get().note("shadow gate probe: armed for frame %u (trigger %s; builder bracket %s; detours: %s; "
                    "visibility global %u).", target, trigger, attach, detours.c_str(),
                    static_cast<unsigned>(g_visGlobal));
}

// A bucket's record range holds the record; the rows of the bucket are its own.
int bucketOf(const std::vector<BucketObs>& rows, uint32_t n, uintptr_t record) noexcept {
    for (uint32_t i = 0; i < n; ++i) {
        const BucketObs& b = rows[i];
        if (!b.valid || !b.base) continue;
        const uintptr_t end = b.base + uintptr_t(b.count) * sg::kRecStride;
        if (record >= b.base && record < end) return static_cast<int>(i);
    }
    return -1;
}

// True when `target` is `node` or one of its descendants through the child
// lists (rec+0x2A8 pointers, count at +0x2B0), to a small depth.
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

void writeReport(uint32_t frame) {
    const uint32_t nb = g_bucketNext.load() < kBucketCap ? g_bucketNext.load() : kBucketCap;
    const uint32_t nr = g_builderNext.load() < kBuilderCap ? g_builderNext.load() : kBuilderCap;
    const uint32_t nd = g_depthNext.load() < kDepthCap ? g_depthNext.load() : kDepthCap;
    const uint32_t nm = g_msmNext.load() < kDepthCap ? g_msmNext.load() : kDepthCap;

    // The camera and the view array come from the first bucket's context.
    uintptr_t ctx = 0;
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
    for (uint32_t v = 0; v < nViews; ++v) readVal(&viewBits[v], ctx + sg::kCtxViews + uintptr_t(v) * sg::kViewBytes + sg::kViewBits);

    // The sun views: the depth and MSM objects that are views of this context.
    uint64_t sunMask = 0;
    uint32_t depthMatched = 0, msmMatched = 0;
    for (uint32_t i = 0; i < nd; ++i) {
        const int k = sg::viewIndexOf(g_depthObjects[i], ctx, nViews);
        if (k >= 0) {
            sunMask |= 1ull << k;
            ++depthMatched;
        }
    }
    for (uint32_t i = 0; i < nm; ++i) {
        const int k = sg::viewIndexOf(g_msmObjects[i], ctx, nViews);
        if (k >= 0) {
            sunMask |= 1ull << k;
            ++msmMatched;
        }
    }
    std::string sunList;
    for (uint32_t v = 0; v < nViews; ++v)
        if (sunMask & (1ull << v)) sunList += (sunList.empty() ? "" : ",") + std::to_string(v);

    // Candidates: the largest-radius records whose sphere holds the camera, and
    // the largest-radius records within 2 km. Read now, the frame's own pose.
    sg::LargestRadius<sg::kCandidatesEach> containing, nearest;   // not "near": windows.h defines it empty
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

    char stamp[24] = {};
    SYSTEMTIME st{};
    GetLocalTime(&st);
    std::snprintf(stamp, sizeof(stamp), "%04u%02u%02u_%02u%02u%02u", static_cast<unsigned>(st.wYear),
                  static_cast<unsigned>(st.wMonth), static_cast<unsigned>(st.wDay), static_cast<unsigned>(st.wHour),
                  static_cast<unsigned>(st.wMinute), static_cast<unsigned>(st.wSecond));
    char csvName[64];
    std::snprintf(csvName, sizeof(csvName), "shadow_gate_%s.csv", stamp);
    std::wstring path = Config::get().logDir() + L"\\shadow_gate_";
    for (const char* c = stamp; *c; ++c) path.push_back(static_cast<wchar_t>(*c));
    path += L".csv";
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (f) std::fprintf(f, "%s\n", sg::csvHeader());
    else Log::get().note("shadow gate probe: could not open edvr_logs\\%s; the lines below are complete.", csvName);

    // Each candidate: rows for the sun views, the main view (0) and every view the
    // bucket's mask holds. Also the summary lines.
    uint32_t rows = 0;
    int linesLeft = kLogLineCap;
    uint32_t idx = 0;
    uint32_t builderMatched = 0;
    for (const Pick& p : picks) {
        const sg::Candidate& c = p.c;
        const int bi = bucketOf(g_bucketRows, nb, c.record);
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
        if (brow >= 0) ++builderMatched;
        std::string sunSummary, mainSummary;
        for (uint32_t v = 0; v < nViews; ++v) {
            const bool sun = (sunMask >> v) & 1ull;
            const bool main = v == 0;
            const bool g1 = bk && (bk->mask & viewBits[v]) != 0;
            if (!sun && !main && !g1) continue;
            sg::GateInput in;
            in.g1 = g1;
            in.windowKnown = bk && bk->baseOk;
            in.window = in.windowKnown && (bk->outMask & viewBits[v]) != 0;
            in.recMask = (recMask & viewBits[v]) != 0;
            if (brow >= 0) {
                const BuilderObs& br = g_builderRows[static_cast<uint32_t>(brow)];
                in.builderReached = true;
                in.planes = br.planes[v];
                in.visApplies = ((br.visApplies >> v) & 1ull) != 0;
                in.g4 = br.g4[v];
            }
            const sg::Gate gate = sg::firstFailingGate(in);
            sg::CsvRow row;
            row.candidate = idx;
            row.kind = p.contains ? "contains" : "near";
            row.record = c.record;
            row.bucket = c.bucket;
            row.base = bk ? bk->base : 0;
            row.role = role;
            row.radius = c.radius;
            row.dist = c.dist;
            row.contains = c.contains ? 1 : 0;
            row.view = v;
            row.viewRole = sun ? "sun" : (main ? "main" : "other");
            row.viewBits = viewBits[v];
            row.g1 = g1 ? 1 : 0;
            row.windowBit = in.windowKnown ? (in.window ? 1 : 0) : -1;
            row.windowV = bk ? bk->v[v] : 0;
            row.windowT0 = bk ? bk->t0 : 0;
            row.windowRepl = bk && bk->baseOk ? (((bk->replMask >> v) & 1ull) ? 1 : 0) : -1;
            row.recBit = in.recMask ? 1 : 0;
            row.builderReached = brow >= 0 ? 1 : 0;
            row.builderRow = brow;
            row.builderMaskBit = brow >= 0 ? ((g_builderRows[static_cast<uint32_t>(brow)].mask & viewBits[v]) ? 1 : 0) : -1;
            row.planes = in.planes;
            row.visApplies = in.visApplies ? 1 : 0;
            row.g4 = in.g4;
            row.gate = gate;
            if (f) {
                char line[512];
                sg::formatCsvRow(line, sizeof(line), frame, row);
                std::fprintf(f, "%s\n", line);
            }
            ++rows;
            const std::string item = "v" + std::to_string(v) + "=" + sg::gateName(gate);
            if (sun) sunSummary += (sunSummary.empty() ? "" : " ") + item;
            if (main) mainSummary += (mainSummary.empty() ? "" : " ") + item;
        }
        if (linesLeft > 0) {
            sg::CandidateLine cl;
            cl.index = idx;
            cl.kind = p.contains ? "contains" : "near";
            cl.record = c.record;
            cl.bucket = c.bucket;
            cl.role = role;
            cl.radius = c.radius;
            cl.dist = c.dist;
            cl.sun = sunSummary.c_str();
            cl.main = mainSummary.c_str();
            char line[512];
            sg::formatCandidateLine(line, sizeof(line), cl);
            Log::get().note("%s", line);
            --linesLeft;
        }
        ++idx;
    }
    if (f) std::fclose(f);

    sg::HeaderInfo h;
    h.frame = frame;
    h.trigger = g_pendingTrigger;
    h.build = EDVR_VERSION_STRING;
    h.ctx = ctx;
    h.views = nViews;
    h.camera[0] = camera[0];
    h.camera[1] = camera[1];
    h.camera[2] = camera[2];
    h.main = 0;
    h.sunList = sunList.c_str();
    h.depthObjects = nd;
    h.depthMatched = depthMatched;
    h.msmObjects = nm;
    h.msmMatched = msmMatched;
    h.buckets = nb;
    h.records = static_cast<uint32_t>(scanned > 0xFFFFFFFFull ? 0xFFFFFFFFu : scanned);
    h.builders = nr;
    h.buildersDropped = g_builderDropped.load();
    h.callbacks = g_callbacks.load();
    h.callbacksExtra = g_callbackExtra.load();
    h.windowMismatch = g_windowMismatch.load();
    h.csvName = csvName;
    char header[640];
    sg::formatHeaderLine(header, sizeof(header), h);
    Log::get().note("%s", header);
    Log::get().note("shadow gate probe: done frame=%u candidates=%u rows=%u builder_matched=%u bucket_drops=%u "
                    "callee_calls=%u first_callee_return=0x%llX (builder site ends 0x%llX; 0 = none seen). The sun list "
                    "above is the depth and MSM objects that were views of this context; unmatched objects mean these "
                    "builders do not take the view.",
                    frame, idx, rows, builderMatched, g_bucketDropped.load(), g_calleeCalls.load(),
                    static_cast<unsigned long long>(g_calleeFirstReturn.load()),
                    static_cast<unsigned long long>(g_base + sg::kCallbackReturnRva));
}

void finishCapture(uint32_t frame) {
    g_shadowGate.store(0, std::memory_order_release);
    g_armed.store(false, std::memory_order_release);
    kinematicEvalSetGateProbeObservers(nullptr, nullptr, nullptr);
    kinematicEvalGateProbeDetach();
    writeReport(frame);
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
            Log::get().note("shadow gate probe: diag.shadow_gate_probe = on: one capture of one frame, 20 s after the "
                            "first in-scene frame.");
    } else if (v != "off" && !v.empty()) {
        Log::get().note("shadow gate probe: diag.shadow_gate_probe = '%s' is not on or off; treated as off.", v.c_str());
    } else {
        Log::get().note("shadow gate probe: diag.shadow_gate_probe = off: nothing is hooked until hotkey.shadow_gate_probe "
                        "is pressed.");
    }
}

void shadowGateProbeRequest(const char* trigger) {
    if (g_armed.load(std::memory_order_acquire) || g_pending) {
        Log::get().note("shadow gate probe: a capture is already %s; the request (%s) is refused.",
                        g_pending ? "pending" : "running", trigger);
        return;
    }
    g_pending = true;
    g_pendingTrigger = trigger;
    Log::get().note("shadow gate probe: requested by %s; one frame is captured from the next frame.", trigger);
}

void shadowGateProbeFrame(uint32_t frame) {
    g_stamp.store(frame + 1, std::memory_order_release);
    if (g_waitingForScene) {
        if (!sceneArrived()) return;
        g_waitingForScene = false;
        g_requestAtMs = GetTickCount64() + kDiagDelayMs;
        Log::get().note("shadow gate probe: the first in-scene frame; the capture is requested in 20 s.");
    }
    if (g_requestAtMs && GetTickCount64() >= g_requestAtMs) {
        g_requestAtMs = 0;
        if (!g_armed.load(std::memory_order_acquire) && !g_pending) {
            g_pending = true;
            g_pendingTrigger = "diag.shadow_gate_probe = on";
        }
    }
    if (g_armed.load(std::memory_order_acquire)) {
        if (frame >= g_target) finishCapture(frame);
        return;
    }
    if (g_pending) {
        g_pending = false;
        startCapture(frame + 1, g_pendingTrigger);
    }
}

}  // namespace edvr
