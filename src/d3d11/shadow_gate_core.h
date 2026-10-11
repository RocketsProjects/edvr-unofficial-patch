#pragma once
// The shadow gate probe's pure half (shadow_gate_probe.cpp; TEMPORARY, the D0 diagnostic of
// docs/station-interior-2026-10-11.md, v2). No Windows, no engine memory: the build 332841 layout and RVAs, the
// pinned bytes, the builder's centre transform (as cull_gate_probe.cpp has it), the view match, the candidate choice,
// the first failing gate, and the text of every log and CSV line. The rig tools\shadow_gate_probe_test compiles it alone.
//
// v2 (2026-10-11, after flight 5): the window replica is gone (it disagreed with the engine's bit about a fifth of the
// time). G2 is the engine's own output mask. The sun views come from the depth and MSM builders' arguments, matched
// against the render context's view array, and reported with a 600-frame cadence of those calls.
//
// Gate order, as the engine applies it to one record and one view:
//   G1   the collection's mask holds the view's bits          (window, FUN_144331300)
//   G2   the window's output holds the view's bits            (the engine's verdict, not a replica)
//   LOD  the record's view mask keeps the bit after the store (store, FUN_144312040)
//   G3   the view's planes do not reject the transformed box  (builder, FUN_1404F4E10)
//   G4   the visibility callback does not veto                (builder, FUN_14288AC40 -> FUN_14288A930)
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr::shadowgate {

// Build 332841, the hash-verified exe (cpu_sampler.h pins the same stamp and size).
constexpr uint32_t kExpectedTimestamp = 1788384820u;
constexpr uint32_t kExpectedImageSize = 104894464u;

// RVAs, each read from the exe or its decompile.
constexpr uintptr_t kWindowRva = 0x4331300u;        // FUN_144331300(collection, out, record); RAX = out
constexpr uintptr_t kCalleeRva = 0x288A930u;        // FUN_14288A930: the callback body (its AL is the verdict)
constexpr uintptr_t kCalleeSiteRva = 0x288AC56u;    // inside FUN_14288AC40: call FUN_14288A930, add rsp,38h, ret
constexpr uintptr_t kCalleeReturnRva = 0x288AC5Bu;  // the return into FUN_14288AC40
constexpr uintptr_t kCallbackSiteRva = 0x42B45D0u;  // inside FUN_142B4420: call FUN_14288AC40, test al,al
constexpr uintptr_t kCallbackReturnRva = 0x42B45D5u;// the builder's return from that call
constexpr uintptr_t kDepthRva = 0x5B2C80u;          // DirectionalShadows.Depth builder (format "%p" of its first argument)
constexpr uintptr_t kMsmRva = 0x5B3530u;            // the MSM pass
constexpr uintptr_t kPlanesRva = 0x4F4E10u;         // FUN_1404F4E10(view, point, interval)

// Bytes read from the exe at those RVAs. A hook installs only when the target starts with them.
constexpr uint8_t kWindowPin[9] = {0x48, 0x8B, 0xC4, 0x53, 0x57, 0x48, 0x83, 0xEC, 0x68};
constexpr uint8_t kCalleePin[8] = {0x40, 0x55, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57};
constexpr uint8_t kDepthPin[5] = {0x48, 0x89, 0x5C, 0x24, 0x10};
constexpr uint8_t kMsmPin[5] = {0x48, 0x89, 0x5C, 0x24, 0x20};
constexpr uint8_t kPlanesPin[8] = {0x41, 0x0F, 0x10, 0x18, 0x45, 0x33, 0xC9, 0x0F};
constexpr uint8_t kCalleeSitePin[5] = {0xE8, 0xD5, 0xFC, 0xFF, 0xFF};
constexpr uint8_t kCallbackSitePin[5] = {0xE8, 0x6B, 0x66, 0x5D, 0xFE};

// Layout. Views: ctx+0x40 + k*0x6A0 for k < ctx+0x1A940 (capped at 64 by the engine).
constexpr uintptr_t kCtxViews = 0x40;
constexpr uintptr_t kCtxCount = 0x1A940;
constexpr uintptr_t kCtxOrigin = 0x00;      // float3: the camera in the records' world frame (unverified)
constexpr uintptr_t kViewBytes = 0x6A0;
constexpr uintptr_t kViewOrigin = 0x540;    // float3: the window's origin
constexpr uintptr_t kViewK1 = 0x550;        // float: window slope
constexpr uintptr_t kViewK2 = 0x560;        // float: window offset
constexpr uintptr_t kViewBits = 0x570;      // u64: the view's mask bits
constexpr uintptr_t kViewVisFlag = 0x68D;   // byte: bit 0 enables the callback
constexpr uint32_t kMaxViews = 64;
// Bucket (collection) fields, read from the window's first argument.
constexpr uintptr_t kBucketCtx = 0x00;      // render context
constexpr uintptr_t kBucketBase = 0x28;     // first record
constexpr uintptr_t kBucketCount = 0x30;    // record count
constexpr uintptr_t kBucketMask = 0x48;     // the collection's active view mask (G1)
// Record fields (stride 0x2F0).
constexpr uintptr_t kRecStride = 0x2F0;
constexpr uintptr_t kRecWorld = 0x240;      // float3: world bounding centre
constexpr uintptr_t kRecMask = 0x208;       // u64: final view mask (the store)
constexpr uintptr_t kRecRadius = 0x280;     // float: radius
constexpr uintptr_t kRecChildren = 0x2A8;   // pointer: child list; count u64 at +0x2B0
constexpr uintptr_t kRecChildCount = 0x2B0;
// Pose context (the builder's frame, as cull_gate_probe.cpp reads it).
constexpr uintptr_t kPoseQuat = 0x10, kPoseT = 0x20, kPoseCentre = 0x80, kPoseExtents = 0x90;

// The probe's own limits and thresholds.
constexpr float kNearMetres = 2000.0f;      // "within 2 km"
constexpr size_t kCandidatesEach = 8;       // K for each of the two lists
constexpr uint32_t kCadenceFrames = 600;    // the cadence window after a press, in frames

// True when `got` starts with the `n` bytes of `want`.
inline bool pinMatches(const uint8_t* got, const uint8_t* want, size_t n) noexcept {
    return got && want && std::memcmp(got, want, n) == 0;
}

// The builder's transformed centre (decomp_42B4420.txt:196-237), as cull_gate_probe.cpp builderCentre: q = pose+0x10
// (quaternion), t = pose+0x20, c = pose+0x80 (float4). The rig pins it against a hand-worked case.
inline void builderCentre(const float q[4], const float t[4], const float c[4], float out[4]) noexcept {
    const float w2 = q[3] + q[3];
    const float dot = q[0] * c[0] + q[1] * c[1] + q[2] * c[2];
    const float ww = q[3] * q[3] + q[3] * q[3];
    out[0] = (c[2] * q[1] - c[1] * q[2]) * w2 + ww * c[0] + (-c[0]) + dot * (q[0] + q[0]) + t[0];
    out[1] = (c[0] * q[2] - c[2] * q[0]) * w2 + ww * c[1] + (-c[1]) + dot * (q[1] + q[1]) + t[1];
    out[2] = (c[1] * q[0] - c[0] * q[1]) * w2 + ww * c[2] + (-c[2]) + dot * (q[2] + q[2]) + t[2];
    out[3] = (c[3] * q[3] - c[3] * q[3]) * w2 + ww * c[3] + (-c[3]) + dot * w2 + t[3];
}

// The view whose address is `object` (a shadow builder's argument), or -1.
inline int viewIndexOf(uintptr_t object, uintptr_t ctx, uint32_t count) noexcept {
    if (!object || !ctx) return -1;
    const uint32_t n = count < kMaxViews ? count : kMaxViews;
    for (uint32_t k = 0; k < n; ++k)
        if (object == ctx + kCtxViews + uintptr_t(k) * kViewBytes) return static_cast<int>(k);
    return -1;
}

// One record considered by the probe: its identity and bounding sphere.
struct Candidate {
    uintptr_t record = 0;
    uintptr_t bucket = 0;       // the collection it was found in
    float centre[3] = {};       // rec+0x240 (world)
    float radius = 0;           // rec+0x280
    float dist = 0;             // from the camera
    bool contains = false;      // the sphere holds the camera
};

// The camera test: distance from the camera to the centre, and whether the sphere contains it.
inline Candidate makeCandidate(uintptr_t record, uintptr_t bucket, const float centre[3], float radius,
                               const float camera[3]) noexcept {
    Candidate c;
    c.record = record;
    c.bucket = bucket;
    c.centre[0] = centre[0];
    c.centre[1] = centre[1];
    c.centre[2] = centre[2];
    c.radius = radius;
    const float dx = centre[0] - camera[0], dy = centre[1] - camera[1], dz = centre[2] - camera[2];
    c.dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    c.contains = c.dist <= radius;
    return c;
}

// Keeps the K largest-radius candidates offered, largest first. A tie keeps the earlier candidate.
template <size_t K>
class LargestRadius {
public:
    void offer(const Candidate& c) noexcept {
        if (n_ < K) {
            items_[n_++] = c;
        } else if (c.radius > items_[K - 1].radius) {
            items_[K - 1] = c;
        } else {
            return;
        }
        for (size_t i = n_ - 1; i > 0 && items_[i].radius > items_[i - 1].radius; --i) {
            const Candidate t = items_[i];
            items_[i] = items_[i - 1];
            items_[i - 1] = t;
        }
    }
    size_t size() const noexcept { return n_; }
    const Candidate& operator[](size_t i) const noexcept { return items_[i]; }

private:
    Candidate items_[K] = {};
    size_t n_ = 0;
};

// The first gate that stops a view from admitting a record, in engine order.
enum class Gate : uint8_t { None, G1, NoWindow, G2, Lod, Unreached, G3, G4 };

struct GateInput {
    bool g1 = false;            // the bucket mask holds the view's bits
    bool windowKnown = false;   // a window observation covers the record's bucket
    bool window = false;        // the engine's window output holds the view's bits
    bool recMask = false;       // rec+0x208 (final) holds the view's bits
    bool builderReached = false;
    int planes = 2;             // -1 outside, 0 straddle, 1 inside, 2 not evaluated
    bool visApplies = false;    // view+0x68D bit 0 and the global are set
    int g4 = -1;                // -1 not evaluated, 0 vetoed, 1 passed
};

inline Gate firstFailingGate(const GateInput& in) noexcept {
    if (!in.g1) return Gate::G1;
    if (!in.windowKnown) return Gate::NoWindow;
    if (!in.window) return Gate::G2;
    if (!in.recMask) return Gate::Lod;
    if (!in.builderReached) return Gate::Unreached;
    if (in.planes == -1) return Gate::G3;
    if (in.visApplies && in.g4 == 0) return Gate::G4;
    return Gate::None;
}

inline const char* gateName(Gate g) noexcept {
    switch (g) {
        case Gate::None: return "admit";
        case Gate::G1: return "G1";
        case Gate::NoWindow: return "noWindow";
        case Gate::G2: return "G2";
        case Gate::Lod: return "LOD";
        case Gate::Unreached: return "unreached";
        case Gate::G3: return "G3";
        case Gate::G4: return "G4";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// The text of the log lines and the CSV. Every output of the probe is built here.

struct CsvRow {
    uint32_t candidate = 0;
    const char* kind = "";          // "contains" or "near"
    uintptr_t record = 0, bucket = 0, base = 0;
    const char* role = "";          // base | child | orphan | nobucket
    float radius = 0, dist = 0;
    int contains = 0;
    uint32_t view = 0;
    const char* viewRole = "";      // sun | main | other
    uint64_t viewBits = 0;
    int g1 = 0;
    int windowBit = -1;             // the engine's window output bit; -1 when no window observation covers the bucket
    int recBit = 0;
    int builderReached = 0;
    int64_t builderRow = -1;
    int builderMaskBit = -1;
    int planes = 2;
    int visApplies = 0;
    int g4 = -1;
    Gate gate = Gate::None;
};

inline const char* csvHeader() noexcept {
    return "frame,candidate,kind,record,bucket,base,role,radius_m,dist_m,contains,view,view_role,view_bits,"
           "g1_bucket,window_bit,rec208_bit,builder_reached,builder_row,builder_mask_bit,planes,vis_applies,g4,first_fail";
}

inline int formatCsvRow(char* out, size_t cap, uint32_t frame, const CsvRow& r) noexcept {
    return std::snprintf(out, cap,
                         "%u,%u,%s,0x%llX,0x%llX,0x%llX,%s,%.2f,%.2f,%d,%u,%s,0x%llX,%d,%d,%d,%d,%lld,%d,%d,%d,%d,%s",
                         frame, r.candidate, r.kind, static_cast<unsigned long long>(r.record),
                         static_cast<unsigned long long>(r.bucket), static_cast<unsigned long long>(r.base), r.role,
                         static_cast<double>(r.radius), static_cast<double>(r.dist), r.contains, r.view, r.viewRole,
                         static_cast<unsigned long long>(r.viewBits), r.g1, r.windowBit, r.recBit, r.builderReached,
                         static_cast<long long>(r.builderRow), r.builderMaskBit, r.planes, r.visApplies, r.g4,
                         gateName(r.gate));
}

// The capture's summary line.
struct HeaderInfo {
    uint32_t frame = 0;
    const char* trigger = "";
    const char* build = "";
    uintptr_t ctx = 0;
    uint32_t views = 0;
    float camera[3] = {};
    int main = 0;
    uint32_t buckets = 0, records = 0, builders = 0, buildersDropped = 0, callbacks = 0, callbacksExtra = 0;
    const char* csvName = "";
};

inline int formatHeaderLine(char* out, size_t cap, const HeaderInfo& h) noexcept {
    return std::snprintf(out, cap,
                         "shadow gate probe: frame=%u trigger=%s build=%s ctx=0x%llX views=%u camera=(%.1f,%.1f,%.1f) "
                         "main=%d buckets=%u records=%u builders=%u dropped=%u callbacks=%u extra=%u csv=%s",
                         h.frame, h.trigger, h.build, static_cast<unsigned long long>(h.ctx), h.views,
                         static_cast<double>(h.camera[0]), static_cast<double>(h.camera[1]),
                         static_cast<double>(h.camera[2]), h.main, h.buckets, h.records, h.builders,
                         h.buildersDropped, h.callbacks, h.callbacksExtra, h.csvName);
}

// One candidate: its identity and the first failing gate for each sun view and for the main view.
struct CandidateLine {
    uint32_t index = 0;
    const char* kind = "";
    uintptr_t record = 0, bucket = 0;
    const char* role = "";
    float radius = 0, dist = 0;
    const char* sun = "";           // "v3=G3 v5=admit", at most eight
    const char* main = "";          // "v0=admit"
};

inline int formatCandidateLine(char* out, size_t cap, const CandidateLine& c) noexcept {
    return std::snprintf(out, cap,
                         "shadow gate probe: cand=%u %s rec=0x%llX bucket=0x%llX role=%s r=%.1f dist=%.1f "
                         "sun[%s] main[%s]",
                         c.index, c.kind, static_cast<unsigned long long>(c.record),
                         static_cast<unsigned long long>(c.bucket), c.role, static_cast<double>(c.radius),
                         static_cast<double>(c.dist), c.sun[0] ? c.sun : "-", c.main[0] ? c.main : "-");
}

// The callee check: applies only when the callback can be called (vis_applies in some row). Without that, the check
// is not applicable and says so.
inline int formatCalleeCheck(char* out, size_t cap, uint32_t visRows, uint32_t callees, uintptr_t firstReturn,
                             uintptr_t siteReturn) noexcept {
    if (visRows == 0)
        return std::snprintf(out, cap, "callee check: not applicable (vis_applies is 0 in every row; the callback is not called)");
    if (callees == 0)
        return std::snprintf(out, cap, "callee check: no callee call while vis applies (rows=%u)", visRows);
    return std::snprintf(out, cap, "callee check: first_callee_return=0x%llX builder site=0x%llX %s",
                         static_cast<unsigned long long>(firstReturn), static_cast<unsigned long long>(siteReturn),
                         firstReturn == siteReturn ? "match" : "MISMATCH");
}

// The cadence: depth and MSM calls per frame over the 600 frames after a press, as a histogram (0, 1, 2, 3, 4 or more).
inline int formatHistogram(char* out, size_t cap, const char* name, const uint32_t counts[5]) noexcept {
    return std::snprintf(out, cap, "%s 0=%u 1=%u 2=%u 3=%u 4+=%u", name, counts[0], counts[1], counts[2], counts[3],
                         counts[4]);
}

inline void addToHistogram(uint32_t counts[5], uint32_t perFrame) noexcept {
    ++counts[perFrame >= 4 ? 4 : perFrame];
}

inline const char* cadenceHeader() noexcept { return "frame,dock_status,dock_location,depth_calls,msm_calls"; }

// One frame of the cadence. dockStatus and dockLocation are 1 docked, 0 not, -1 unknown.
inline int formatCadenceRow(char* out, size_t cap, uint32_t frame, int dockStatus, int dockLocation, uint32_t depth,
                            uint32_t msm) noexcept {
    return std::snprintf(out, cap, "%u,%d,%d,%u,%u", frame, dockStatus, dockLocation, depth, msm);
}

// One view of the render context: its address, its mask bits and window constants, and how many recorded shadow
// builder calls named this address (depth and MSM).
inline int formatSunViewLine(char* out, size_t cap, uint32_t k, uintptr_t ptr, uint64_t bits, const float k1,
                             const float k2, const float origin[3], uint32_t depthHits, uint32_t msmHits) noexcept {
    return std::snprintf(out, cap,
                         "shadow sun view %u: ptr=0x%llX bits=0x%llX k1=%.6g k2=%.6g origin=(%.1f,%.1f,%.1f) "
                         "depth_hits=%u msm_hits=%u shadow=%s",
                         k, static_cast<unsigned long long>(ptr), static_cast<unsigned long long>(bits),
                         static_cast<double>(k1), static_cast<double>(k2), static_cast<double>(origin[0]),
                         static_cast<double>(origin[1]), static_cast<double>(origin[2]), depthHits, msmHits,
                         (depthHits || msmHits) ? "yes" : "no");
}

// One recorded shadow builder call: its frame, which builder, its four arguments, and the view whose address one of
// them is (-1 when none is).
inline int formatCallLine(char* out, size_t cap, uint32_t index, const char* which, uint32_t frame, uintptr_t a,
                          uintptr_t b, uintptr_t c, uintptr_t d, int view) noexcept {
    return std::snprintf(out, cap, "shadow call %u: %s frame=%u rcx=0x%llX rdx=0x%llX r8=0x%llX r9=0x%llX view=%d",
                         index, which, frame, static_cast<unsigned long long>(a), static_cast<unsigned long long>(b),
                         static_cast<unsigned long long>(c), static_cast<unsigned long long>(d), view);
}

}  // namespace edvr::shadowgate
