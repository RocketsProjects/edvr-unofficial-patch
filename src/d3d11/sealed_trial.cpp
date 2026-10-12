// The sealed-hangar trial (sealed_trial.h). The pure rules are in sealed_trial_core.h; the store detour and its gate are in store_hook.cpp; the
// view the sphere is centred on is resolved by view_source.cpp. This file holds the toggle, the radii and their ladders, the per-record decision
// in the store's observer (sphere mode, or the own-pad cell of named mode, Built v8.3b), the class cache, the kept-records report, and the lines.
#include "sealed_trial.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../common/config.h"
#include "../common/log.h"
#include "asset_census_core.h"
#include "kept_report_core.h"
#include "sealed_trial_core.h"
#include "store_hook.h"
#include "view_source.h"

namespace edvr {
namespace {

// The render context's layout (the same offsets as shadow_gate_core.h): views at +0x40, 0x6A0 bytes each; the view count at +0x1A940.
constexpr uintptr_t kCtxCount = 0x1A940;
constexpr uintptr_t kRecCentre = 0x240;     // float3: the world sphere centre
constexpr uintptr_t kRecMask = 0x208;       // u64: the view mask the store wrote
constexpr uintptr_t kRecAdmitted = 0x234;   // u8: the admitted flag the store wrote
constexpr uintptr_t kRecRadius = 0x280;     // float: the sphere radius
constexpr uintptr_t kRecChildren = 0x2A8;   // pointer: the child list, an array of 8-byte record pointers
constexpr uintptr_t kRecChildCount = 0x2B0; // u64: the child count
constexpr uintptr_t kBucketBase = 0x28;     // the bucket's base record
constexpr uintptr_t kRecName = 0x18;        // pointer: the record's node (node+0 is the text pointer)

constexpr size_t kObservedCap = 32768;       // records the observer keeps for the report in one frame
constexpr uint32_t kReportNodeCap = 40000;   // records the report walks at most
constexpr uint32_t kReportDepth = 6;         // child-list generations the report walks below an observed record
constexpr uint32_t kReportChildCap = 4096;   // children read from one child list
constexpr size_t kClassSlots = 4096;         // the class cache: one slot per node (lock-free, valid for one frame)
constexpr size_t kNameSlots = 512;           // the name cache for the builder probe and the report (under the lock)
constexpr uint32_t kViewTries = 64;

std::atomic<bool> g_on{false};
// The live sphere radius (diag.sealed_trial_radius_m, and the ladder while on). Changed on the render thread; read by the store's observer.
std::atomic<float> g_radius{sealed::kDefaultRadiusM};
// The record rule (diag.sealed_trial_mode; hotkey.sealed_trial_mode cycles it while on).
std::atomic<sealed::Mode> g_mode{sealed::Mode::kSphere};
sealed::BuilderPolicy g_builderPolicy = sealed::BuilderPolicy::kCount;   // read at install; skip is refused in builder_probe.cpp
// The window's counters (atomics: the store's observer adds to them from its threads).
std::atomic<uint64_t> g_culled{0}, g_kept{0}, g_keptUnknown{0}, g_sunSkipped{0}, g_evalNs{0}, g_faults{0}, g_torn{0};
std::atomic<uint64_t> g_builderAfterCull{0};
std::atomic<uint64_t> g_keptByCell{0}, g_keptExplicit{0}, g_culledContaining{0};
// Built v8.4: the centre measurements of the window (under the lock). rec+0x240 exactly zero; the pose unreadable (the fallback); and the distance
// between rec+0x240 and the pose centre for the records where both are readable and rec+0x240 is not zero (reserved at a toggle-on; a write past
// the reserve is not kept, so the hot path never allocates).
constexpr size_t kCentreDistCap = 8192;
uint64_t g_zeroCentre = 0, g_poseFallback = 0;
std::vector<float> g_centreDist;
// Built v8.5: the kind of the pointer at rec+0x290 for each record the store visits in the window (under the lock).
uint64_t g_ptrNull = 0, g_ptrImage = 0, g_ptrOther = 0, g_ptrUnread = 0;
std::atomic<uint32_t> g_frameStamp{0};       // advanced at every Present tick: the observer resolves the view once per frame
std::atomic<bool> g_cameraKnown{false};

// Built v8.1 (the crash fix): every piece of state the store observer and the frame's code share is guarded by this lock, which is recursive
// because the store re-enters the observer through its own recursion. It is never held across a call into the game.
std::recursive_mutex g_sealedMutex;

// The named mode's cell radii (diag.sealed_cell_small_m / _medium_m / _large_m; guesses 60 / 90 / 130 m) and the size last seen. Under the lock.
float g_cellRadius[3] = {sealed::kCellDefaultM[0], sealed::kCellDefaultM[1], sealed::kCellDefaultM[2]};
char g_lastSize = 'S';

// The class cache (lock-free): a slot per node, a word per slot (sealed_trial_core.h packs it). Written by any thread that runs the store.
std::atomic<uint64_t> g_classSlots[kClassSlots] = {};

// The frame's pad and explicit-part records (collected under the lock while named mode runs), and the frame's camera.
sealed::PadObs g_pads[sealed::kPadSlots];
size_t g_padCount = 0;
sealed::ExplicitObs g_explicits[sealed::kExplicitSlots];
size_t g_explicitCount = 0;
float g_frameCam[3] = {};
bool g_frameCamValid = false;

// The cell in force for the frame now being observed (promoted at the frame's first record from the previous frame's pads), and the explicit
// instances picked for it (the nearest instance of each part node to the cell centre, within 200 m).
sealed::OwnCell g_cellPrev;
uintptr_t g_explicitPrev[sealed::kExplicitNodes] = {};
size_t g_explicitPrevCount = 0;
uint32_t g_cellStamp = 0;                    // the frame stamp the cell was last promoted at
bool g_cellLogHas = false;                   // the last logged own-pad state (a change is logged once)
char g_cellLogName[64] = {};

uint64_t g_windowStartMs = 0;                // render thread only
uint32_t g_viewStamp = 0;                    // the frame stamp g_view was resolved in
bool g_viewReady = false;                    // g_view holds a resolution
uint32_t g_viewTries = 0;                    // resolutions tried in the frame (the rows may be chosen part-way through it)
view::Resolved g_view;                       // the view the rule's sphere is centred on, this frame
uintptr_t g_lastCtx = 0;                     // the render context the observer last read

// The kept-records report: arm (a press), collect (the next frame's observer calls), write (the Present after that).
enum RepPhase : int { kRepIdle = 0, kRepArmed = 1, kRepCollecting = 2 };
std::atomic<int> g_repPhase{kRepIdle};
std::string g_repTrigger;                    // under the lock
uint32_t g_repFrame = 0;                     // the frame collected (under the lock)
struct Observed {
    uintptr_t bucket;
    uintptr_t record;
};
std::vector<Observed> g_observed;            // reserved when collecting starts; the observer appends within its capacity only

bool readMem(void* dst, uintptr_t src, size_t n) noexcept {
    if (!src) return false;
    __try {
        std::memcpy(dst, reinterpret_cast<const void*>(src), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool writeMem(uintptr_t dst, const void* src, size_t n) noexcept {
    if (!dst) return false;
    __try {
        std::memcpy(reinterpret_cast<void*>(dst), src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <typename T>
bool readVal(T* dst, uintptr_t src) noexcept { return readMem(dst, src, sizeof(T)); }

double nanoseconds(const LARGE_INTEGER& a, const LARGE_INTEGER& b) noexcept {
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    if (f.QuadPart <= 0) return 0.0;
    return static_cast<double>(b.QuadPart - a.QuadPart) * 1.0e9 / static_cast<double>(f.QuadPart);
}

void resetWindow() noexcept {
    g_culled.store(0);
    g_kept.store(0);
    g_keptUnknown.store(0);
    g_sunSkipped.store(0);
    g_evalNs.store(0);
    g_faults.store(0);
    g_torn.store(0);
    g_builderAfterCull.store(0);
    g_keptByCell.store(0);
    g_keptExplicit.store(0);
    g_culledContaining.store(0);
    g_zeroCentre = 0;
    g_poseFallback = 0;
    g_ptrNull = g_ptrImage = g_ptrOther = g_ptrUnread = 0;
    g_centreDist.clear();
    if (g_centreDist.capacity() < kCentreDistCap) g_centreDist.reserve(kCentreDistCap);   // at a window's end, not on the store's hot path
}

// The window's statistics, with the cell in force at its latest frame. Called under the lock.
sealed::Window takeWindow() noexcept {
    sealed::Window w;
    w.culled = g_culled.load();
    w.kept = g_kept.load();
    w.keptUnknown = g_keptUnknown.load();
    w.sunSkipped = g_sunSkipped.load();
    w.evalNs = g_evalNs.load();
    w.faults = g_faults.load();
    w.torn = g_torn.load();
    w.builderAfterCull = g_builderAfterCull.load();
    w.keptByCell = g_keptByCell.load();
    w.keptExplicit = g_keptExplicit.load();
    w.culledContaining = g_culledContaining.load();
    w.zeroCentre = g_zeroCentre;
    w.poseFallback = g_poseFallback;
    w.ptrNull = g_ptrNull;
    w.ptrImage = g_ptrImage;
    w.ptrOther = g_ptrOther;
    w.ptrUnread = g_ptrUnread;
    w.distN = g_centreDist.size();
    if (!g_centreDist.empty()) {
        w.distP50 = sealed::percentileOf(g_centreDist, 0.50);
        w.distP95 = sealed::percentileOf(g_centreDist, 0.95);
        w.distMax = *std::max_element(g_centreDist.begin(), g_centreDist.end());
    }
    const bool named = g_mode.load(std::memory_order_relaxed) == sealed::Mode::kNamed;
    w.cellHas = named && g_cellPrev.has;
    if (w.cellHas) {
        w.cellSize = sealed::padSizeLetter(g_cellPrev.size);
        w.cellRadius = g_cellPrev.radius;
        std::snprintf(w.padName, sizeof(w.padName), "%s", g_cellPrev.padName);
        w.padDist = g_cellPrev.padDist;
    } else {
        std::snprintf(w.padReason, sizeof(w.padReason), "%s", named ? g_cellPrev.reason : "sphere mode (no cell)");
    }
    return w;
}

// The camera position the rule centres on: the pose from the view's rows, else the view 0 origin. False when neither is known.
// The rule itself is view::sphereCentre (view_source.h), shared with the asset census.
bool cameraOf(const view::Resolved& v, float out[3]) noexcept { return view::sphereCentre(v, out); }

// The label the trial's lines carry: the view source, and where the sphere centre came from (see cameraOf).
const char* viewLabel(const view::Resolved& v) noexcept {
    static char label[96];
    const bool rows = v.poseOk && (v.source == view::Source::kExplorerCam || !v.originOk);
    std::snprintf(label, sizeof(label), "%s; centre = %s", view::sourceName(v.source),
                  rows ? "rows" : (v.originOk ? "origin" : "unknown"));
    return label;
}

// Writes the window's line at the current radius and the view it ran against, then starts a new window. Called under the lock.
void noteWindow() {
    char line[960];
    sealed::formatWindow(line, sizeof(line), takeWindow(), g_radius.load(std::memory_order_relaxed), viewLabel(g_view),
                         g_mode.load(std::memory_order_relaxed));
    Log::get().note("%s", line);
    resetWindow();
}

// Resolves the view afresh (the toggle, the report): the render context is the last one the observer read, when there is one.
void resolveNow() noexcept {
    view::resolveView(g_lastCtx, &g_view);
    g_viewStamp = g_frameStamp.load(std::memory_order_relaxed);
    g_viewReady = true;
}

// The name of a record, copied out under the lock (no pointer into the cache escapes: the slot may be reused as soon as the lock is let go).
// False when no name can be read. Used by the builder probe, the geometry probe and the report.
bool cachedName(uintptr_t record, char* out, size_t cap) noexcept {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    uintptr_t node = 0;
    if (!record || !readVal(&node, record + kRecName) || !node) return false;
    std::lock_guard<std::recursive_mutex> lock(g_sealedMutex);
    struct NameSlot {
        uintptr_t node = 0;
        uint32_t stamp = 0;
        bool ok = false;
        char name[64] = {};
    };
    static NameSlot names[kNameSlots];
    const uint32_t stamp = g_frameStamp.load(std::memory_order_relaxed);
    NameSlot& slot = names[(node >> 4) % kNameSlots];
    if (slot.node != node || slot.stamp != stamp) {
        slot.node = node;
        slot.stamp = stamp;
        slot.ok = false;
        slot.name[0] = '\0';
        uintptr_t text = 0;
        uint8_t bytes[census::kNameBytes] = {};
        std::string name;
        if (readVal(&text, node) && text && readMem(bytes, text, sizeof(bytes)) &&
            (census::textName(bytes, sizeof(bytes), false, &name) || census::textName(bytes, sizeof(bytes), true, &name))) {
            std::snprintf(slot.name, sizeof(slot.name), "%s", name.c_str());
            slot.ok = true;
        }
    }
    if (!slot.ok) return false;
    std::snprintf(out, cap, "%s", slot.name);
    return true;
}

// --- Built v8.4: a record's centre. The builder's centre is computed from the pose it reads, *(rec+0x290): q = pose+0x10, t = pose+0x20, c =
// pose+0x80 (sealed_trial_core.h builderCentre; decomp_42B4420.txt:196-237). rec+0x240 is the fallback when the pose cannot be read (counted).
constexpr uintptr_t kRecPose = 0x290;     // pointer: the builder's rcx, the pose context
constexpr uintptr_t kPoseQuat = 0x10;     // float4: the quaternion x, y, z, w
constexpr uintptr_t kPoseT = 0x20;        // float4: the translation
constexpr uintptr_t kPoseCentre = 0x80;   // float4: the local centre c

struct CentreRead {
    sealed::CentreSource source = sealed::CentreSource::kNone;
    float world[3] = {};      // the centre the rule uses
    float pose[3] = {};       // the builder's centre (valid when poseOk)
    bool poseOk = false;
    float raw[3] = {};        // rec+0x240 (valid when rawOk)
    bool rawOk = false;
    bool rawZero = false;     // rec+0x240 is exactly the origin
    float dist = -1.0f;       // |raw - pose| when both are readable and raw is not zero; else -1
};

// The builder's centre from the pose (SEH-guarded reads only). False when the pose pointer or any of its fields cannot be read.
bool poseCentreOf(uintptr_t rec, float out[3]) noexcept {
    uintptr_t pose = 0;
    if (!readVal(&pose, rec + kRecPose) || !pose) return false;
    float q[4] = {}, t[4] = {}, c[4] = {}, w[4] = {};
    if (!readMem(q, pose + kPoseQuat, sizeof(q)) || !readMem(t, pose + kPoseT, sizeof(t)) ||
        !readMem(c, pose + kPoseCentre, sizeof(c)))
        return false;
    sealed::builderCentre(q, t, c, w);
    out[0] = w[0];
    out[1] = w[1];
    out[2] = w[2];
    return sealed::finite3(out);
}

CentreRead recordCentreRead(uintptr_t rec) noexcept {
    CentreRead r;
    r.poseOk = poseCentreOf(rec, r.pose);
    r.rawOk = readMem(r.raw, rec + kRecCentre, sizeof(r.raw)) && sealed::finite3(r.raw);
    r.rawZero = r.rawOk && sealed::isZeroCentre(r.raw);
    r.source = sealed::chooseCentre(r.poseOk, r.pose, r.rawOk, r.raw, r.world);
    if (r.poseOk && r.rawOk && !r.rawZero) r.dist = sealed::distance(r.pose, r.raw);
    return r;
}

// Built v8.5: the game image's end (its base plus SizeOfImage from the PE header), read once and cached. A failed read caches the base (every
// non-null pointer is then "other"), which the window line would show.
std::atomic<uintptr_t> g_imageEnd{0};
uintptr_t gameImageEnd() noexcept {
    const uintptr_t cached = g_imageEnd.load(std::memory_order_acquire);
    if (cached) return cached;
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    uint32_t lfanew = 0, size = 0;
    const uintptr_t end = (base && readVal(&lfanew, base + 0x3C) && readVal(&size, base + lfanew + 0x50)) ? base + size : base;
    g_imageEnd.store(end, std::memory_order_release);
    return end;
}

// Built v8.5: the pointer at rec+0x290 read raw, and its kind (sealed_trial_core.h PtrKind).
sealed::PtrKind ptr290Of(uintptr_t rec, uint64_t* raw) noexcept {
    uintptr_t p = 0;
    const bool ok = readVal(&p, rec + kRecPose);
    if (raw) *raw = static_cast<uint64_t>(p);
    static const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));   // the module does not move
    return sealed::ptrKindOf(ok, p, base, gameImageEnd());
}

// Built v8.5: counts one record's pointer kind in the window (under the lock).
static void notePtr(sealed::PtrKind k) noexcept {
    switch (k) {
        case sealed::PtrKind::kNull: ++g_ptrNull; break;
        case sealed::PtrKind::kImage: ++g_ptrImage; break;
        case sealed::PtrKind::kOther: ++g_ptrOther; break;
        default: ++g_ptrUnread; break;
    }
}

// The class of a node's name, read from game memory with no lock (the census's accept rules). The miss path of the class cache.
sealed::NameClass classifyNode(uintptr_t node) noexcept {
    uintptr_t text = 0;
    uint8_t bytes[census::kNameBytes] = {};
    if (!readVal(&text, node) || !text || !readMem(bytes, text, sizeof(bytes))) return sealed::NameClass::kUnnamed;
    std::string name;
    const bool ok = census::textName(bytes, sizeof(bytes), false, &name) || census::textName(bytes, sizeof(bytes), true, &name);
    return sealed::classifyName(ok, name.c_str());
}

// The class of a record's name and its node. Lock-free: one guarded read of the node pointer and one atomic load on a hit; the name is read only
// on a miss (once per node per frame).
sealed::NameClass classOf(uintptr_t record, uintptr_t* nodeOut) noexcept {
    *nodeOut = 0;
    uintptr_t node = 0;
    if (!record || !readVal(&node, record + kRecName) || !node) return sealed::NameClass::kUnnamed;
    *nodeOut = node;
    if (!sealed::slotFits(node)) return classifyNode(node);
    const uint32_t stamp = g_frameStamp.load(std::memory_order_relaxed);
    std::atomic<uint64_t>& slot = g_classSlots[(uint64_t(node) >> 3) & (kClassSlots - 1)];
    sealed::NameClass cls = sealed::NameClass::kNone;
    if (sealed::unpackSlot(slot.load(std::memory_order_acquire), node, stamp, &cls)) return cls;
    cls = classifyNode(node);
    slot.store(sealed::packSlot(node, cls, stamp), std::memory_order_release);
    return cls;
}

// Logs the own pad when it changes (a change is logged once). Called under the lock.
void logCellChange(const sealed::OwnCell& cell, uint32_t frame) {
    const bool named = g_mode.load(std::memory_order_relaxed) == sealed::Mode::kNamed;
    if (!named) return;
    if (cell.has == g_cellLogHas && (!cell.has || std::strcmp(cell.padName, g_cellLogName) == 0)) return;
    g_cellLogHas = cell.has;
    std::snprintf(g_cellLogName, sizeof(g_cellLogName), "%s", cell.has ? cell.padName : "");
    if (cell.has)
        Log::get().note("sealed cell: own pad %s (size %c, cell radius %.1f m, centre (%.1f,%.1f,%.1f), %.1f m from the camera, %s) at frame %u.",
                        cell.padName, sealed::padSizeLetter(cell.size), static_cast<double>(cell.radius),
                        static_cast<double>(cell.centre[0]), static_cast<double>(cell.centre[1]), static_cast<double>(cell.centre[2]),
                        static_cast<double>(cell.padDist), cell.padContains ? "its sphere contains the camera" : "nearest within the fallback range",
                        frame);
    else
        Log::get().note("sealed cell: no own pad at frame %u (%s); named mode is the sphere rule for those frames.", frame, cell.reason);
}

// Promotes the previous frame's pads into the cell for the frame now starting, and the explicit instances for that cell. Called under the lock
// at the frame's first record. The pads and explicit parts are the previous frame's; the camera is the previous frame's too.
void promoteFrame(uint32_t stamp, uint32_t frame) {
    sealed::OwnCell next;
    if (g_mode.load(std::memory_order_relaxed) == sealed::Mode::kNamed) {
        if (g_frameCamValid) {
            const float radii[3] = {g_cellRadius[0], g_cellRadius[1], g_cellRadius[2]};
            sealed::pickOwnPad(g_pads, g_padCount, g_frameCam, radii, &next);
        } else {
            std::snprintf(next.reason, sizeof(next.reason), "no camera for the previous frame");
        }
    } else {
        std::snprintf(next.reason, sizeof(next.reason), "sphere mode");
    }
    g_explicitPrevCount = next.has ? sealed::pickExplicit(g_explicits, g_explicitCount, next.centre, g_explicitPrev, sealed::kExplicitNodes) : 0;
    g_cellPrev = next;
    g_padCount = 0;
    g_explicitCount = 0;
    g_cellStamp = stamp;
    if (next.has) g_lastSize = sealed::padSizeLetter(next.size);
    logCellChange(next, frame);
}

// Writes the kept-records report for the frame collected. Walks each observed record's child lists (to a small depth), reads each record's sphere,
// mask and children, and classifies every kept record against the view the frame's rule used. Called under the lock.
void writeKeptReport() {
    char line[900];
    const float R = g_radius.load(std::memory_order_relaxed);
    float cam[3] = {};
    if (!cameraOf(g_view, cam)) {
        Log::get().note("sealed trial report: frame %u not written: neither the view's rows nor its origin could be read (view source = %s).",
                        g_repFrame, view::sourceName(g_view.source));
        return;
    }
    const uintptr_t imageBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    constexpr uint32_t kNone = 0xFFFFFFFFu;
    const bool named = g_mode.load(std::memory_order_relaxed) == sealed::Mode::kNamed;

    std::unordered_map<uintptr_t, uint32_t> index;         // record -> entry
    std::unordered_map<uintptr_t, uintptr_t> observedBucket;  // record -> the bucket the observer saw it in
    std::unordered_map<uintptr_t, uintptr_t> bucketBase;      // bucket -> its base record
    std::vector<kept::Entry> entries;
    std::vector<uint32_t> parentOf;
    std::vector<bool> own;
    entries.reserve(4096);
    parentOf.reserve(4096);
    own.reserve(4096);
    for (const Observed& o : g_observed) observedBucket.emplace(o.record, o.bucket);

    struct Work {
        uintptr_t rec;
        uint32_t parent;
        uint32_t depth;
        uintptr_t bucket;
    };
    std::vector<Work> stack;
    for (size_t i = g_observed.size(); i-- > 0;) stack.push_back(Work{g_observed[i].record, kNone, 0, g_observed[i].bucket});

    bool truncated = false;
    while (!stack.empty()) {
        const Work w = stack.back();
        stack.pop_back();
        if (index.count(w.rec)) continue;
        if (entries.size() >= kReportNodeCap) {
            truncated = true;
            break;
        }
        kept::Entry e;
        e.address = w.rec;
        e.bucket = w.bucket;
        if (!bucketBase.count(w.bucket)) {
            uintptr_t base = 0;
            readVal(&base, w.bucket + kBucketBase);
            bucketBase.emplace(w.bucket, base);
        }
        e.base = bucketBase[w.bucket];
        e.parent = w.parent == kNone ? 0 : entries[w.parent].address;
        // Built v8.4: the world centre (the builder's, from the pose) and both centres for the columns.
        const CentreRead cr = recordCentreRead(w.rec);
        const float* centre = cr.world;
        for (int c = 0; c < 3; ++c) e.poseCentre[c] = cr.pose[c];
        e.poseOk = cr.poseOk;
        e.rawZero = cr.rawZero;
        e.centreSource = static_cast<uint8_t>(cr.source);
        e.centreDist = cr.dist;
        {
            uint64_t raw = 0;
            e.ptr290Kind = static_cast<uint8_t>(ptr290Of(w.rec, &raw));
            e.ptr290 = raw;
        }
        float radius = 0.0f;
        uint64_t mask = 0;
        uint64_t children = 0;
        uintptr_t vt = 0;
        const bool readOk = cr.source != sealed::CentreSource::kNone && readVal(&radius, w.rec + kRecRadius) &&
                            readVal(&mask, w.rec + kRecMask) && readVal(&children, w.rec + kRecChildCount);
        readVal(&vt, w.rec);
        {
            char name[64];
            if (cachedName(w.rec, name, sizeof(name))) e.name = name;   // the census's rules; empty when unreadable
        }
        e.readable = readOk;
        e.radius = readOk ? radius : 0.0f;
        e.childCount = readOk ? children : 0;
        e.vtableRva = vt >= imageBase ? static_cast<uint64_t>(vt - imageBase) : static_cast<uint64_t>(vt);
        e.kept = readOk && mask != 0;
        e.judged = observedBucket.count(w.rec) != 0;
        bool ownMeets = false;
        if (readOk && std::isfinite(radius) && radius > 0.0f) {
            for (int c = 0; c < 3; ++c) e.rel[c] = centre[c] - cam[c];
            e.dist = std::sqrt(e.rel[0] * e.rel[0] + e.rel[1] * e.rel[1] + e.rel[2] * e.rel[2]);
            e.contains = e.dist <= radius;
            ownMeets = e.contains || e.dist <= radius + R;
        }
        const uint32_t self = static_cast<uint32_t>(entries.size());
        index.emplace(w.rec, self);
        entries.push_back(e);
        parentOf.push_back(w.parent);
        own.push_back(ownMeets);
        if (w.depth >= kReportDepth || !readOk) continue;
        uintptr_t list = 0;
        uint64_t count = children;
        readVal(&list, w.rec + kRecChildren);
        if (!list || count == 0) continue;
        if (count > kReportChildCap) count = kReportChildCap;
        std::vector<uintptr_t> kids;
        kids.reserve(static_cast<size_t>(count));
        for (uint64_t i = 0; i < count; ++i) {
            uintptr_t child = 0;
            if (readVal(&child, list + static_cast<uintptr_t>(i) * 8) && child) kids.push_back(child);
        }
        for (size_t k = kids.size(); k-- > 0;) stack.push_back(Work{kids[k], self, w.depth + 1, w.bucket});
    }

    // Why each record is kept: its own test first, else the nearest ancestor whose test meets the cell.
    for (uint32_t i = 0; i < entries.size(); ++i) {
        uint32_t levelsUp = 0;
        uint32_t up = parentOf[i];
        for (uint32_t level = 1; up != kNone && level <= kReportDepth + 1; ++level) {
            if (own[up]) {
                levelsUp = level;
                break;
            }
            up = parentOf[up];
        }
        entries[i].levelsUp = own[i] ? 0 : levelsUp;
        entries[i].why = kept::classify(own[i], levelsUp);
    }

    // Named mode (Built v8.3b): each kept record's label is its reason against the cell in force for the frame (cell, explicit, or no cell).
    uint32_t nCell = 0, nExplicit = 0, nSphereNoCell = 0, nUnreadable = 0, nOtherNamed = 0;
    if (named) {
        for (kept::Entry& e : entries) {
            if (!e.kept) continue;
            if (!e.readable) {
                e.label = "unjudged_unreadable";
                ++nUnreadable;
            } else if (!g_cellPrev.has) {
                e.label = "sphere_no_cell";
                ++nSphereNoCell;
            } else {
                const float c[3] = {e.rel[0] + cam[0], e.rel[1] + cam[1], e.rel[2] + cam[2]};
                if (sealed::distance(c, g_cellPrev.centre) <= g_cellPrev.radius) {
                    e.label = "cell";
                    ++nCell;
                } else if (sealed::inAddrSet(g_explicitPrev, g_explicitPrevCount, e.address)) {
                    e.label = "explicit";
                    ++nExplicit;
                } else {
                    e.label = "other_named";
                    ++nOtherNamed;
                }
            }
        }
    }

    // Built v8.4: the centre measurements over all walked records: rec+0x240 exactly zero, the pose-unreadable fallback, and the distance between
    // rec+0x240 and the pose centre where both are readable and rec+0x240 is not zero.
    uint32_t nRawZero = 0, nPoseFallback = 0;
    std::vector<float> centreDists;
    for (const kept::Entry& e : entries) {
        if (e.rawZero) ++nRawZero;
        if (e.centreSource == 1) ++nPoseFallback;
        if (e.centreDist >= 0.0f) centreDists.push_back(e.centreDist);
    }
    char centreTotals[320];
    std::snprintf(centreTotals, sizeof(centreTotals),
                  "raw_centre_zero %u, pose_fallback %u, centre_dist_m p50 %.2f p95 %.2f max %.2f (n %zu)", nRawZero, nPoseFallback,
                  static_cast<double>(sealed::percentileOf(centreDists, 0.50)), static_cast<double>(sealed::percentileOf(centreDists, 0.95)),
                  static_cast<double>(centreDists.empty() ? 0.0f : *std::max_element(centreDists.begin(), centreDists.end())),
                  centreDists.size());
    const kept::Stats st = kept::countKept(entries.data(), static_cast<uint32_t>(entries.size()));
    uint32_t byRadius[kept::kTop];
    uint32_t byBase[kept::kTop];
    const uint32_t nRadius = kept::topByRadius(entries.data(), static_cast<uint32_t>(entries.size()), byRadius);
    const uint32_t nBase = kept::topBases(entries.data(), static_cast<uint32_t>(entries.size()), byBase);

    SYSTEMTIME st2{};
    GetLocalTime(&st2);
    char stamp[24] = {};
    std::snprintf(stamp, sizeof(stamp), "%04u%02u%02u_%02u%02u%02u", static_cast<unsigned>(st2.wYear),
                  static_cast<unsigned>(st2.wMonth), static_cast<unsigned>(st2.wDay), static_cast<unsigned>(st2.wHour),
                  static_cast<unsigned>(st2.wMinute), static_cast<unsigned>(st2.wSecond));
    char csvName[64] = {};
    std::snprintf(csvName, sizeof(csvName), "sealed_kept_%s.csv", stamp);
    std::wstring path = Config::get().logDir() + L"\\sealed_kept_";
    for (const char* c = stamp; *c; ++c) path.push_back(static_cast<wchar_t>(*c));
    path += L".csv";

    const char* source = viewLabel(g_view);
    const bool forwardKnown = g_view.poseOk;
    char row[600];
    char totals[360];
    if (named)
        std::snprintf(totals, sizeof(totals), "named: cell %u, explicit %u, other_named %u, sphere_no_cell %u, unjudged_unreadable %u",
                      nCell, nExplicit, nOtherNamed, nSphereNoCell, nUnreadable);
    else
        std::snprintf(totals, sizeof(totals), "sphere: own %u, base %u, unjudged %u (sphere rule; labels are the why_kept column)",
                      st.keptOwn, st.keptBase, st.keptUnjudged);
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (f) {
        std::fprintf(f, "# view source = %s camera=(%.2f,%.2f,%.2f) forward=(%.4f,%.4f,%.4f) radius_m=%.1f frame=%u observed=%zu "
                        "nodes=%u kept=%u truncated=%d\n",
                     source, static_cast<double>(cam[0]), static_cast<double>(cam[1]), static_cast<double>(cam[2]),
                     static_cast<double>(forwardKnown ? g_view.pose.fwd[0] : 0.0f),
                     static_cast<double>(forwardKnown ? g_view.pose.fwd[1] : 0.0f),
                     static_cast<double>(forwardKnown ? g_view.pose.fwd[2] : 0.0f), static_cast<double>(R), g_repFrame,
                     g_observed.size(), static_cast<unsigned>(entries.size()), st.kept, truncated ? 1 : 0);
        std::fprintf(f, "# totals over all %u walked records: kept %u; %s\n", static_cast<unsigned>(entries.size()), st.kept, totals);
        std::fprintf(f, "# centres over all walked records (Built v8.4): %s\n", centreTotals);
        if (named)
            std::fprintf(f, "# named cell: size %c, radius %.1f m, pad %s, centre (%.2f,%.2f,%.2f); explicit instances kept: %u\n",
                         g_cellPrev.has ? sealed::padSizeLetter(g_cellPrev.size) : 'n', static_cast<double>(g_cellPrev.radius),
                         g_cellPrev.has ? g_cellPrev.padName : "none", static_cast<double>(g_cellPrev.centre[0]),
                         static_cast<double>(g_cellPrev.centre[1]), static_cast<double>(g_cellPrev.centre[2]),
                         static_cast<unsigned>(g_explicitPrevCount));
        std::fprintf(f, "%s\n", kept::csvHeader());
        for (uint32_t k = 0; k < nRadius; ++k)
            if (kept::formatRow(row, sizeof(row), "radius", k + 1, entries[byRadius[k]]) >= 0) std::fprintf(f, "%s\n", row);
        for (uint32_t k = 0; k < nBase; ++k)
            if (kept::formatRow(row, sizeof(row), "base", k + 1, entries[byBase[k]]) >= 0) std::fprintf(f, "%s\n", row);
        std::fclose(f);
    }

    std::snprintf(line, sizeof(line),
                  "sealed trial report: frame %u view source = %s camera=(%.2f,%.2f,%.2f) radius_m=%.1f: observed %zu records, "
                  "walked %u, kept %u; totals %s; centres %s; top %u by radius, top %u bases by child count (the csv holds those rows; its "
                  "header carries the full totals); csv=%s%s",
                  g_repFrame, source, static_cast<double>(cam[0]), static_cast<double>(cam[1]), static_cast<double>(cam[2]),
                  static_cast<double>(R), g_observed.size(), static_cast<unsigned>(entries.size()), st.kept, totals, centreTotals, nRadius, nBase,
                  csvName, truncated ? " (walk truncated)" : "");
    Log::get().note("%s", line);
    if (!f) Log::get().note("sealed trial report: could not open edvr_logs\\%s; the line above is complete.", csvName);
}

}  // namespace

void sealedTrialInstall(bool flatProfile) {
    (void)flatProfile;
    const std::string r = Config::get().getString("diag.sealed_trial_radius_m", "");
    float start = sealed::kDefaultRadiusM;
    if (!sealed::parseRadius(r.c_str(), &start))
        Log::get().note("sealed trial: diag.sealed_trial_radius_m = '%s' is not a number of metres; the radius is %.1f m.",
                        r.c_str(), static_cast<double>(start));
    g_radius.store(start, std::memory_order_relaxed);
    const std::string modeText = Config::get().getString("diag.sealed_trial_mode", "");
    sealed::Mode mode = sealed::Mode::kSphere;
    if (!sealed::parseMode(modeText.c_str(), &mode))
        Log::get().note("sealed trial: diag.sealed_trial_mode = '%s' is not sphere, named or all; the mode is sphere.", modeText.c_str());
    g_mode.store(mode, std::memory_order_relaxed);
    // Named mode's cell radii (Built v8.3b). The defaults are GUESSES, not measurements.
    const std::string smallText = Config::get().getString("diag.sealed_cell_small_m", "");
    const std::string mediumText = Config::get().getString("diag.sealed_cell_medium_m", "");
    const std::string largeText = Config::get().getString("diag.sealed_cell_large_m", "");
    if (!sealed::parseCellRadius(smallText.c_str(), sealed::kCellDefaultM[0], &g_cellRadius[0]))
        Log::get().note("sealed cell: diag.sealed_cell_small_m = '%s' is not a number of metres; the small cell radius is %.1f m (a guess).",
                        smallText.c_str(), static_cast<double>(g_cellRadius[0]));
    if (!sealed::parseCellRadius(mediumText.c_str(), sealed::kCellDefaultM[1], &g_cellRadius[1]))
        Log::get().note("sealed cell: diag.sealed_cell_medium_m = '%s' is not a number of metres; the medium cell radius is %.1f m (a guess).",
                        mediumText.c_str(), static_cast<double>(g_cellRadius[1]));
    if (!sealed::parseCellRadius(largeText.c_str(), sealed::kCellDefaultM[2], &g_cellRadius[2]))
        Log::get().note("sealed cell: diag.sealed_cell_large_m = '%s' is not a number of metres; the large cell radius is %.1f m (a guess).",
                        largeText.c_str(), static_cast<double>(g_cellRadius[2]));
    const std::string policyText = Config::get().getString("diag.sealed_trial_builder", "");
    sealed::BuilderPolicy policy = sealed::BuilderPolicy::kCount;
    if (!sealed::parseBuilderPolicy(policyText.c_str(), &policy))
        Log::get().note("sealed trial: diag.sealed_trial_builder = '%s' is not count or skip; the builder is counted only.",
                        policyText.c_str());
    g_builderPolicy = policy;
    if (policy == sealed::BuilderPolicy::kSkip)
        Log::get().note("sealed trial: diag.sealed_trial_builder = skip is REFUSED: the builder's side effects are not proven absent "
                        "(draw-command appends, the counter at pipeline+0x2A4, the bucket lookup); the builder is counted only.");
    Log::get().note("sealed trial: off. hotkey.sealed_trial toggles it; the sphere starts at %.1f m (diag.sealed_trial_radius_m); the "
                    "record rule is %s (diag.sealed_trial_mode; hotkey.sealed_trial_mode cycles it while on). Named mode's cell radii are "
                    "small %.1f, medium %.1f, large %.1f m (diag.sealed_cell_*_m; guesses until measured); hotkey.sealed_cell_larger and "
                    "hotkey.sealed_cell_smaller step the current size's radius while named mode is on. hotkey.sealed_trial_report writes "
                    "the kept-records report for one frame.", static_cast<double>(start), sealed::modeName(mode),
                    static_cast<double>(g_cellRadius[0]), static_cast<double>(g_cellRadius[1]), static_cast<double>(g_cellRadius[2]));
}

// The record's name (builder probe, geometry probe, report): a copy, no pointer into the cache.
bool sealedTrialRecordName(uintptr_t record, std::string* out) noexcept {
    char name[64];
    if (!cachedName(record, name, sizeof(name))) return false;
    *out = name;
    return true;
}

bool sealedTrialRecordNameInto(uintptr_t record, char* out, size_t cap) noexcept { return cachedName(record, out, cap); }

unsigned char sealedTrialRecordCentre(uintptr_t record, float out[3], bool* rawZero) noexcept {
    const CentreRead r = recordCentreRead(record);
    out[0] = r.world[0];
    out[1] = r.world[1];
    out[2] = r.world[2];
    if (rawZero) *rawZero = r.rawZero;
    return static_cast<unsigned char>(r.source);
}

void sealedTrialToggle(const char* trigger, uint32_t frame) {
    std::lock_guard<std::recursive_mutex> lock(g_sealedMutex);
    char line[480];
    const float radius = g_radius.load(std::memory_order_relaxed);
    if (g_on.load(std::memory_order_acquire)) {
        noteWindow();
        g_on.store(false, std::memory_order_release);
        storeHookRecompute();
        sealed::formatToggle(line, sizeof(line), false, frame, radius, nullptr, false, view::sourceName(g_view.source));
        Log::get().note("%s (trigger %s)", line, trigger);
        // Built v8.5: the toggle never changes the record rule; the mode is the last one set (install, or the mode key).
        Log::get().note("sealed trial: off; the record rule stays %s across the toggle (trigger %s)",
                        sealed::modeName(g_mode.load(std::memory_order_relaxed)), trigger);
        return;
    }
    g_on.store(true, std::memory_order_release);
    storeHookRecompute();
    if (std::strcmp(storeHookStatus(), "installed") != 0) {
        g_on.store(false, std::memory_order_release);
        storeHookRecompute();
        Log::get().note("sealed trial: refused (trigger %s): the admission store is not detoured (%s).", trigger,
                        storeHookStatus());
        return;
    }
    // The camera is read now, from the same view the rule uses, so the toggle line names it (it used to wait for the first store call).
    resolveNow();
    resetWindow();
    g_windowStartMs = GetTickCount64();
    float cam[3] = {};
    const bool known = cameraOf(g_view, cam);
    sealed::formatToggle(line, sizeof(line), true, frame, radius, cam, known, viewLabel(g_view));
    Log::get().note("%s (trigger %s)", line, trigger);
    if (g_view.originGapM >= 0.0f)
        Log::get().note("sealed trial: view source = %s; its rows are %.3f m from the view 0 origin (the frames agree when this is small).",
                        view::sourceName(g_view.source), static_cast<double>(g_view.originGapM));
    char modeLine[400];
    sealed::formatModeLine(modeLine, sizeof(modeLine), g_mode.load(std::memory_order_relaxed), frame, trigger);
    Log::get().note("%s", modeLine);
}

// TEMPORARY (named mode): cycles the record rule while the trial is on. The window of the old rule closes first, so each rule has its own line.
void sealedTrialModeStep(const char* trigger, uint32_t frame) {
    std::lock_guard<std::recursive_mutex> lock(g_sealedMutex);
    char line[400];
    if (!g_on.load(std::memory_order_acquire)) {
        Log::get().note("sealed trial: the mode key acts only while the trial is on (mode %s at frame %u); nothing changed (trigger %s).",
                        sealed::modeName(g_mode.load(std::memory_order_relaxed)), frame, trigger);
        return;
    }
    noteWindow();   // the old rule's window closes here
    const sealed::Mode next = sealed::nextMode(g_mode.load(std::memory_order_relaxed));
    g_mode.store(next, std::memory_order_release);
    if (next != sealed::Mode::kNamed) {   // the own pad's cell belongs to named mode only
        g_cellPrev = sealed::OwnCell{};
        std::snprintf(g_cellPrev.reason, sizeof(g_cellPrev.reason), "%s mode", sealed::modeName(next));
    }
    g_windowStartMs = GetTickCount64();
    sealed::formatModeLine(line, sizeof(line), next, frame, trigger);
    Log::get().note("%s", line);
}

// TEMPORARY (named mode, Built v8.3b): steps the CURRENT size's cell radius one rung of the ladder (dir -1 smaller, +1 larger). The current size is
// the size of the cell in force, or the size last seen. Logs each change with its size; off, or not in named mode, the key logs a refusal.
void sealedTrialCellStep(int dir, const char* trigger, uint32_t frame) {
    std::lock_guard<std::recursive_mutex> lock(g_sealedMutex);
    char line[320];
    const bool named = g_mode.load(std::memory_order_relaxed) == sealed::Mode::kNamed;
    if (!g_on.load(std::memory_order_acquire) || !named) {
        Log::get().note("named cell: the cell keys act only while the trial is on in named mode (trial %s, mode %s); nothing changed "
                        "(trigger %s).", g_on.load() ? "on" : "off", sealed::modeName(g_mode.load(std::memory_order_relaxed)), trigger);
        return;
    }
    const char size = g_cellPrev.has ? sealed::padSizeLetter(g_cellPrev.size) : g_lastSize;
    const int index = size == 'S' ? 0 : (size == 'M' ? 1 : 2);
    bool moved = false;
    const float before = g_cellRadius[index];
    const float after = sealed::stepCellRadius(before, dir, &moved);
    if (moved) {
        g_cellRadius[index] = after;
        if (g_cellPrev.has && sealed::padSizeLetter(g_cellPrev.size) == size) g_cellPrev.radius = after;
    }
    sealed::formatCellStep(line, sizeof(line), size, moved, moved ? after : before, dir, frame, trigger);
    Log::get().note("%s", line);
}

void sealedTrialStep(int dir, const char* trigger, uint32_t frame) {
    std::lock_guard<std::recursive_mutex> lock(g_sealedMutex);
    char line[320];
    const float before = g_radius.load(std::memory_order_relaxed);
    if (!g_on.load(std::memory_order_acquire)) {
        sealed::formatStepRefused(line, sizeof(line), before, frame);
        Log::get().note("%s (trigger %s)", line, trigger);
        return;
    }
    bool moved = false;
    const float after = sealed::stepRadius(before, dir, &moved);
    if (moved) {
        noteWindow();   // the old radius's window closes here
        g_radius.store(after, std::memory_order_relaxed);
        g_windowStartMs = GetTickCount64();
    }
    sealed::formatStep(line, sizeof(line), dir, moved, moved ? after : before, frame);
    Log::get().note("%s (trigger %s)", line, trigger);
}

void sealedTrialReportRequest(const char* trigger, uint32_t frame) {
    std::lock_guard<std::recursive_mutex> lock(g_sealedMutex);
    (void)frame;
    if (!g_on.load(std::memory_order_acquire)) {
        Log::get().note("sealed trial report: refused (trigger %s): the trial is off; press hotkey.sealed_trial first.", trigger);
        return;
    }
    int expected = kRepIdle;
    if (!g_repPhase.compare_exchange_strong(expected, kRepArmed, std::memory_order_acq_rel)) {
        Log::get().note("sealed trial report: refused (trigger %s): a report is already armed or collecting.", trigger);
        return;
    }
    g_repTrigger = trigger ? trigger : "";
    Log::get().note("sealed trial report: armed (trigger %s); the next frame's kept records are collected and written at the Present "
                    "after it.", g_repTrigger.c_str());
}

void sealedTrialFrame(uint32_t frame) {
    std::lock_guard<std::recursive_mutex> lock(g_sealedMutex);
    g_frameStamp.fetch_add(1, std::memory_order_relaxed);
    const int phase = g_repPhase.load(std::memory_order_acquire);
    if (phase == kRepArmed) {
        if (!g_on.load(std::memory_order_acquire)) {
            g_repPhase.store(kRepIdle, std::memory_order_release);
            Log::get().note("sealed trial report: cancelled (trigger %s): the trial went off before the frame.", g_repTrigger.c_str());
        } else {
            g_repFrame = frame + 1;
            g_observed.clear();
            g_observed.reserve(kObservedCap);
            g_repPhase.store(kRepCollecting, std::memory_order_release);
            Log::get().note("sealed trial report: collecting frame %u (trigger %s).", g_repFrame, g_repTrigger.c_str());
        }
    } else if (phase == kRepCollecting) {
        if (!g_on.load(std::memory_order_acquire))
            Log::get().note("sealed trial report: cancelled (trigger %s): the trial went off during the frame.", g_repTrigger.c_str());
        else
            writeKeptReport();
        g_observed.clear();
        g_repPhase.store(kRepIdle, std::memory_order_release);
    }
    if (!g_on.load(std::memory_order_acquire)) return;
    const uint64_t now = GetTickCount64();
    if (now - g_windowStartMs < sealed::kWindowMs) return;
    g_windowStartMs = now;
    noteWindow();
}

bool sealedTrialOn() noexcept { return g_on.load(std::memory_order_acquire); }

bool sealedTrialSkipsSunPair() noexcept { return g_on.load(std::memory_order_acquire); }

const char* sealedTrialModeText() noexcept {
    return g_on.load(std::memory_order_acquire) ? sealed::modeName(g_mode.load(std::memory_order_relaxed)) : "off";
}

void sealedTrialNoteSunSkip() noexcept { g_sunSkipped.fetch_add(1, std::memory_order_relaxed); }

void sealedTrialNoteBuilderCalls(uint64_t afterCull) noexcept {
    if (afterCull) g_builderAfterCull.fetch_add(afterCull, std::memory_order_relaxed);
}

namespace {
// The record's view mask and admitted flag through the guarded reads and writes, for the fault-safe cull (sealed_trial_core.h).
struct RecordFields {
    uintptr_t rec;
    bool readMask(uint64_t* out) noexcept { return readVal(out, rec + kRecMask); }
    bool readAdmitted(uint8_t* out) noexcept { return readVal(out, rec + kRecAdmitted); }
    bool writeMask(uint64_t v) noexcept { return writeMem(rec + kRecMask, &v, sizeof(v)); }
    bool writeAdmitted(uint8_t v) noexcept { return writeMem(rec + kRecAdmitted, &v, sizeof(v)); }
};
}  // namespace

// The fault-safe cull of one record, with its counters (called under the lock). containing: the record's sphere contains the camera (named mode).
static void cullCounted(uintptr_t record, bool containing) noexcept {
    RecordFields fields{record};
    sealed::CullField failed = sealed::CullField::kMaskRead;
    uint64_t value = 0;
    switch (sealed::cullRecordFaultSafe(fields, &failed, &value)) {
        case sealed::Cull::kCulled:
            g_culled.fetch_add(1, std::memory_order_relaxed);
            if (containing) g_culledContaining.fetch_add(1, std::memory_order_relaxed);
            break;
        case sealed::Cull::kUntouched:
        case sealed::Cull::kRestored:
            g_faults.fetch_add(1, std::memory_order_relaxed);   // the record is as the store wrote it: kept, not judged
            break;
        case sealed::Cull::kTorn:
            g_faults.fetch_add(1, std::memory_order_relaxed);
            g_torn.fetch_add(1, std::memory_order_relaxed);     // the one case the record may be left half culled
            break;
    }
}

// The rule for one record the store has written. Every read and write is validated; a failure leaves the record as the store wrote it and counts
// a fault. The view the sphere is centred on is resolved once per frame (the first record of the frame).
void sealedTrialAfterStore(uintptr_t bucket, uintptr_t record) noexcept {
    // The lock covers the view, the context, the cell, the pad and explicit lists and the observed list. It is never held across a call into the
    // game (this observer runs after the store's forward has returned); it is recursive for the store's own recursion.
    std::lock_guard<std::recursive_mutex> lock(g_sealedMutex);
    LARGE_INTEGER t0{}, t1{};
    QueryPerformanceCounter(&t0);
    uintptr_t ctx = 0;
    uint32_t count = 0;
    if (!bucket || !record || !readVal(&ctx, bucket) || !ctx || !readVal(&count, ctx + kCtxCount) || count == 0) {
        g_faults.fetch_add(1, std::memory_order_relaxed);
        QueryPerformanceCounter(&t1);
        g_evalNs.fetch_add(static_cast<uint64_t>(nanoseconds(t0, t1)), std::memory_order_relaxed);
        return;
    }
    // Built v8.5, mode all: every record the store visits is culled, with no view, sphere, name or pose test: the discriminator. The pointer kind at
    // rec+0x290 is still counted, so the window line shows what the visits look like.
    if (g_mode.load(std::memory_order_relaxed) == sealed::Mode::kAll) {
        uint64_t raw = 0;
        notePtr(ptr290Of(record, &raw));
        cullCounted(record, false);
        if (g_repPhase.load(std::memory_order_relaxed) == kRepCollecting && g_observed.size() < g_observed.capacity())
            g_observed.push_back(Observed{bucket, record});
        QueryPerformanceCounter(&t1);
        g_evalNs.fetch_add(static_cast<uint64_t>(nanoseconds(t0, t1)), std::memory_order_relaxed);
        return;
    }
    // The view is resolved at the frame's first record, and again (up to kViewTries times) while it has no pose: the temporal pass chooses
    // its rows part-way through a frame, so an early record can find none.
    const uint32_t stamp = g_frameStamp.load(std::memory_order_relaxed);
    if (g_viewStamp != stamp) {
        g_viewStamp = stamp;
        g_viewTries = 0;
        g_viewReady = false;   // the frame's first record resolves the view afresh
    }
    if (!g_viewReady || (!g_view.poseOk && g_viewTries < kViewTries)) {
        ++g_viewTries;
        view::resolveView(ctx, &g_view);
        g_viewReady = true;
    }
    g_lastCtx = ctx;
    float cam[3] = {};
    if (!cameraOf(g_view, cam)) {
        g_faults.fetch_add(1, std::memory_order_relaxed);
        QueryPerformanceCounter(&t1);
        g_evalNs.fetch_add(static_cast<uint64_t>(nanoseconds(t0, t1)), std::memory_order_relaxed);
        return;
    }
    g_cameraKnown.store(true, std::memory_order_relaxed);
    // The frame's first record: the previous frame's pads become this frame's cell (the camera used is the previous frame's, still in g_frameCam).
    if (g_cellStamp != stamp) promoteFrame(stamp, stamp);
    std::memcpy(g_frameCam, cam, sizeof(g_frameCam));
    g_frameCamValid = true;
    const float trialR = g_radius.load(std::memory_order_relaxed);
    const bool named = g_mode.load(std::memory_order_relaxed) == sealed::Mode::kNamed;

    // Built v8.4: the record's world centre is the builder's (from the pose), with rec+0x240 as the counted fallback; the radius is rec+0x280.
    const CentreRead cr = recordCentreRead(record);
    {
        uint64_t raw = 0;
        notePtr(ptr290Of(record, &raw));
    }
    if (cr.rawZero) ++g_zeroCentre;
    if (cr.source == sealed::CentreSource::kRaw) ++g_poseFallback;
    if (cr.dist >= 0.0f && g_centreDist.size() < kCentreDistCap) g_centreDist.push_back(cr.dist);
    const float* centre = cr.world;
    float radius = 0.0f;
    if (cr.source == sealed::CentreSource::kNone || !readVal(&radius, record + kRecRadius) || !std::isfinite(radius) || !(radius > 0.0f) ||
        !std::isfinite(centre[0]) || !std::isfinite(centre[1]) || !std::isfinite(centre[2])) {
        g_keptUnknown.fetch_add(1, std::memory_order_relaxed);
    } else {
        bool keep = false;
        sealed::KeepWhy why = sealed::KeepWhy::kNone;
        bool containing = false;
        bool sphereJudged = false;
        if (named) {
            uintptr_t node = 0;
            const sealed::NameClass cls = classOf(record, &node);
            if (cls == sealed::NameClass::kPad && g_padCount < sealed::kPadSlots) {
                sealed::PadObs& p = g_pads[g_padCount];
                char name[64] = {};
                if (cachedName(record, name, sizeof(name))) {
                    p.addr = record;
                    p.centre[0] = centre[0];
                    p.centre[1] = centre[1];
                    p.centre[2] = centre[2];
                    p.radius = radius;
                    p.size = sealed::padSizeOf(name);
                    std::snprintf(p.name, sizeof(p.name), "%s", name);
                    if (p.size != sealed::PadSize::kNone) ++g_padCount;
                }
            } else if (cls == sealed::NameClass::kExplicit && g_explicitCount < sealed::kExplicitSlots) {
                sealed::ExplicitObs& x = g_explicits[g_explicitCount++];
                x.addr = record;
                x.node = node;
                x.centre[0] = centre[0];
                x.centre[1] = centre[1];
                x.centre[2] = centre[2];
            }
            const sealed::NamedKeep v = sealed::keepNamedCell(centre, radius, cam, trialR, g_cellPrev, g_explicitPrev, g_explicitPrevCount,
                                                              record);
            keep = v.keep;
            why = v.why;
            containing = v.containing;
            sphereJudged = v.sphereJudged;
        } else {
            keep = sealed::keepRecord(centre, radius, cam, trialR);
            why = keep ? sealed::KeepWhy::kSphere : sealed::KeepWhy::kNone;
        }
        if (keep) {
            g_kept.fetch_add(1, std::memory_order_relaxed);
            if (why == sealed::KeepWhy::kCell && !sphereJudged) g_keptByCell.fetch_add(1, std::memory_order_relaxed);
            if (why == sealed::KeepWhy::kExplicit) g_keptExplicit.fetch_add(1, std::memory_order_relaxed);
        } else {
            cullCounted(record, named && g_cellPrev.has && containing);
        }
    }
    if (g_repPhase.load(std::memory_order_relaxed) == kRepCollecting && g_observed.size() < g_observed.capacity())
        g_observed.push_back(Observed{bucket, record});
    QueryPerformanceCounter(&t1);
    g_evalNs.fetch_add(static_cast<uint64_t>(nanoseconds(t0, t1)), std::memory_order_relaxed);
}

}  // namespace edvr
