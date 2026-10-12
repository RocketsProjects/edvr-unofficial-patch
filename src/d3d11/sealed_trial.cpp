// The sealed-hangar trial (sealed_trial.h). The sphere rule is in sealed_trial_core.h; the store detour and its gate are in store_hook.cpp;
// the view the sphere is centred on is resolved by view_source.cpp. This file holds the toggle, the radius and its ladder, the per-record
// decision in the store's observer, the kept-records report, and the lines.
#include "sealed_trial.h"

#include <windows.h>

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

constexpr size_t kObservedCap = 32768;       // records the observer keeps for the report in one frame
constexpr uint32_t kReportNodeCap = 40000;   // records the report walks at most
constexpr uint32_t kReportDepth = 6;         // child-list generations the report walks below an observed record
constexpr uint32_t kReportChildCap = 4096;   // children read from one child list

std::atomic<bool> g_on{false};
// The live radius: the configured start, then the ladder's rungs while the trial is on. Changed on the render thread (the step keys);
// the store's observer reads it through the atomic.
std::atomic<float> g_radius{sealed::kDefaultRadiusM};
// TEMPORARY (named mode): the record rule. Changed on the render thread (the mode key); the store's observer reads it through the atomic.
std::atomic<sealed::Mode> g_mode{sealed::Mode::kSphere};
// TEMPORARY (diag.sealed_trial_builder): read at install. skip is refused in builder_probe.cpp (not proven safe), so it counts only.
sealed::BuilderPolicy g_builderPolicy = sealed::BuilderPolicy::kCount;
std::atomic<uint64_t> g_culled{0}, g_kept{0}, g_keptUnknown{0}, g_sunSkipped{0}, g_evalNs{0}, g_faults{0}, g_torn{0};
std::atomic<uint64_t> g_builderAfterCull{0};  // TEMPORARY (Built v8.1): builder calls cleared at entry, counted by the builder probe's tick
// Built v8.1 (the crash fix): every piece of state the store observer and the frame's code share (the view, the view stamp, the render context,
// the report's observed list, the name cache, the window clock and the report's trigger) is guarded by this lock. It is recursive because the
// store re-enters the observer through its own recursion; nothing takes it across a call into the game (forward() is never called under it).
std::recursive_mutex g_sealedMutex;
std::atomic<uint32_t> g_frameStamp{0};       // advanced at every Present tick: the observer resolves the view once per frame
std::atomic<bool> g_cameraKnown{false};
uint64_t g_windowStartMs = 0;                // render thread only
uint32_t g_viewStamp = 0;                    // render thread: the frame stamp g_view was resolved in
bool g_viewReady = false;                    // render thread: g_view holds a resolution
uint32_t g_viewTries = 0;                    // render thread: resolutions tried in the frame (the rows may be chosen part-way through it)
constexpr uint32_t kViewTries = 64;
view::Resolved g_view;                       // render thread: the view the rule's sphere is centred on, this frame
uintptr_t g_lastCtx = 0;                     // render thread: the render context the observer last read

// The kept-records report: arm (a press), collect (the next frame's observer calls), write (the Present after that).
enum RepPhase : int { kRepIdle = 0, kRepArmed = 1, kRepCollecting = 2 };
std::atomic<int> g_repPhase{kRepIdle};
std::string g_repTrigger;                    // render thread
uint32_t g_repFrame = 0;                     // the frame collected (render thread)
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
}

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
    return w;
}

// TEMPORARY (record names). A small direct-mapped cache per node pointer, valid for one frame: the store, the builder probe and the kept
// report ask for the same node many times in a frame. The text is read with the census's accept rules (asset_census_core.h textName).
struct NameSlot {
    uintptr_t node = 0;
    uint32_t stamp = 0;
    bool ok = false;
    char name[64] = {};
};
constexpr size_t kNameSlots = 512;
NameSlot g_nameCache[kNameSlots];

// The camera position the rule centres on: the pose from the view's rows, else the view 0 origin. False when neither is known.
// The centre the rule uses. In the explorer cam it is the free camera's rows (the view 0 origin may not be the free camera there). Seated and
// on foot it is the view 0 origin, which the flights have proven; the rows are the fallback where no origin is read. The frame gap between
// the two is logged at each toggle, so the flight can check that they agree.
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

// Writes the window's line at the current radius and the view it ran against, then starts a new window.
void noteWindow() {
    char line[480];
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

// Writes the kept-records report for the frame collected. Walks each observed record's child lists (to a small depth), reads each
// record's sphere, mask and children from memory, and classifies every kept record against the view the frame's rule used.
void writeKeptReport() {
    char line[640];
    const float R = g_radius.load(std::memory_order_relaxed);
    float cam[3] = {};
    if (!cameraOf(g_view, cam)) {
        Log::get().note("sealed trial report: frame %u not written: neither the view's rows nor its origin could be read (view source = %s).",
                        g_repFrame, view::sourceName(g_view.source));
        return;
    }
    const uintptr_t imageBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    constexpr uint32_t kNone = 0xFFFFFFFFu;

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
        float centre[3] = {};
        float radius = 0.0f;
        uint64_t mask = 0;
        uint64_t children = 0;
        uintptr_t vt = 0;
        const bool readOk = readMem(centre, w.rec + kRecCentre, sizeof(centre)) && readVal(&radius, w.rec + kRecRadius) &&
                            readVal(&mask, w.rec + kRecMask) && readVal(&children, w.rec + kRecChildCount);
        readVal(&vt, w.rec);
        {
            std::string name;
            if (sealedTrialRecordName(w.rec, &name)) e.name = name;   // the census's rules; empty when unreadable
        }
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
    char row[512];
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (f) {
        std::fprintf(f, "# view source = %s camera=(%.2f,%.2f,%.2f) forward=(%.4f,%.4f,%.4f) radius_m=%.1f frame=%u observed=%zu "
                        "nodes=%u kept=%u truncated=%d\n",
                     source, static_cast<double>(cam[0]), static_cast<double>(cam[1]), static_cast<double>(cam[2]),
                     static_cast<double>(forwardKnown ? g_view.pose.fwd[0] : 0.0f),
                     static_cast<double>(forwardKnown ? g_view.pose.fwd[1] : 0.0f),
                     static_cast<double>(forwardKnown ? g_view.pose.fwd[2] : 0.0f), static_cast<double>(R), g_repFrame,
                     g_observed.size(), static_cast<unsigned>(entries.size()), st.kept, truncated ? 1 : 0);
        std::fprintf(f, "%s\n", kept::csvHeader());
        for (uint32_t k = 0; k < nRadius; ++k)
            if (kept::formatRow(row, sizeof(row), "radius", k + 1, entries[byRadius[k]]) >= 0) std::fprintf(f, "%s\n", row);
        for (uint32_t k = 0; k < nBase; ++k)
            if (kept::formatRow(row, sizeof(row), "base", k + 1, entries[byBase[k]]) >= 0) std::fprintf(f, "%s\n", row);
        std::fclose(f);
    }

    std::snprintf(line, sizeof(line),
                  "sealed trial report: frame %u view source = %s camera=(%.2f,%.2f,%.2f) radius_m=%.1f: observed %zu records, "
                  "walked %u, kept %u (own %u, base %u, unjudged %u); children kept %u, of which %u were kept only because their "
                  "base was; top %u by radius, top %u bases by child count; csv=%s%s",
                  g_repFrame, source, static_cast<double>(cam[0]), static_cast<double>(cam[1]), static_cast<double>(cam[2]),
                  static_cast<double>(R), g_observed.size(), static_cast<unsigned>(entries.size()), st.kept, st.keptOwn,
                  st.keptBase, st.keptUnjudged, st.keptChildren, st.keptChildrenByBase, nRadius, nBase, csvName,
                  truncated ? " (walk truncated)" : "");
    Log::get().note("%s", line);
    if (!f) Log::get().note("sealed trial report: could not open edvr_logs\\%s; the line above is complete.", csvName);
}

// The name of a record, copied out under the trial's lock (Built v8.1: the slot may be reused by another thread the moment the lock is let go, so
// no pointer into the cache escapes). False when no name can be read. Allocation-free on a hit: the cache holds the text.
bool cachedName(uintptr_t record, char* out, size_t cap) noexcept {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    uintptr_t node = 0;
    if (!record || !readVal(&node, record + 0x18) || !node) return false;
    std::lock_guard<std::recursive_mutex> lock(g_sealedMutex);
    const uint32_t stamp = g_frameStamp.load(std::memory_order_relaxed);
    NameSlot& slot = g_nameCache[(node >> 4) % kNameSlots];
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
        Log::get().note("sealed trial: diag.sealed_trial_mode = '%s' is not sphere or named; the mode is sphere.", modeText.c_str());
    g_mode.store(mode, std::memory_order_relaxed);
    const std::string policyText = Config::get().getString("diag.sealed_trial_builder", "");
    sealed::BuilderPolicy policy = sealed::BuilderPolicy::kCount;
    if (!sealed::parseBuilderPolicy(policyText.c_str(), &policy))
        Log::get().note("sealed trial: diag.sealed_trial_builder = '%s' is not count or skip; the builder is counted only.",
                        policyText.c_str());
    g_builderPolicy = policy;
    // skip is refused: the builder's effects (draw-command appends, the counter at pipeline+0x2A4, the bucket lookup) are not proven absent,
    // so a skipped record could change the frame. The count runs either way (docs Built v8, the builder's disassembly).
    if (policy == sealed::BuilderPolicy::kSkip)
        Log::get().note("sealed trial: diag.sealed_trial_builder = skip is REFUSED: the builder's side effects are not proven absent "
                        "(draw-command appends, the counter at pipeline+0x2A4, the bucket lookup); the builder is counted only.");
    Log::get().note("sealed trial: off. hotkey.sealed_trial toggles it; the sphere starts at %.1f m (diag.sealed_trial_radius_m); the "
                    "record rule is %s (diag.sealed_trial_mode; hotkey.sealed_trial_mode cycles it while on). While on, "
                    "hotkey.sealed_trial_smaller and hotkey.sealed_trial_larger step it through 80, 60, 40, 30, 20, 15 and 10 m, and "
                    "hotkey.sealed_trial_report writes the kept-records report for one frame. Records outside the rule lose their views in "
                    "the admission store, and the sun pair is skipped.", static_cast<double>(start), sealed::modeName(mode));
}

// The record's name, cached per node for the frame. Needed only by named mode, the builder probe and the kept report; the store does not
// ask for it otherwise.
bool sealedTrialRecordName(uintptr_t record, std::string* out) noexcept {
    char name[64];
    if (!cachedName(record, name, sizeof(name))) return false;
    *out = name;
    return true;
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
    g_windowStartMs = GetTickCount64();
    sealed::formatModeLine(line, sizeof(line), next, frame, trigger);
    Log::get().note("%s", line);
}

// One rung while the trial is on. A rung that moves closes the window of the old radius first, so the new radius starts a window of its
// own; a clamp changes nothing and says so. Off, the key logs a refusal.
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

void sealedTrialNoteSunSkip() noexcept { g_sunSkipped.fetch_add(1, std::memory_order_relaxed); }

void sealedTrialNoteBuilderCalls(uint64_t afterCull) noexcept {
    if (afterCull) g_builderAfterCull.fetch_add(afterCull, std::memory_order_relaxed);
}

// The record's view mask and admitted flag through the guarded reads and writes, for the fault-safe cull (sealed_trial_core.h).
namespace {
struct RecordFields {
    uintptr_t rec;
    bool readMask(uint64_t* out) noexcept { return readVal(out, rec + kRecMask); }
    bool readAdmitted(uint8_t* out) noexcept { return readVal(out, rec + kRecAdmitted); }
    bool writeMask(uint64_t v) noexcept { return writeMem(rec + kRecMask, &v, sizeof(v)); }
    bool writeAdmitted(uint8_t v) noexcept { return writeMem(rec + kRecAdmitted, &v, sizeof(v)); }
};
// The keep decision for one record, for the mode on (sphere: the sphere rule; named: the name rule over the same sphere). Named mode reads
// the cached name without allocating.
bool keepByRule(uintptr_t record, const float centre[3], float radius, const float cam[3], float R) noexcept {
    const sealed::Mode m = g_mode.load(std::memory_order_relaxed);
    if (m == sealed::Mode::kSphere) return sealed::keepRecord(centre, radius, cam, R);
    char name[64];
    const bool hasName = cachedName(record, name, sizeof(name));
    return sealed::keepRecordNamed(hasName, name, centre, radius, cam, R);
}

}  // namespace

// The rule for one record the store has written. Every read and write is validated; a failure leaves the record as the store wrote it and
// counts a fault. The view the sphere is centred on is resolved once per frame (the first record of the frame).
void sealedTrialAfterStore(uintptr_t bucket, uintptr_t record) noexcept {
    // The lock covers the view, the render context, the observed list and the name cache. It is never held across a call into the game (this
    // observer runs after the store's forward has returned), and it is recursive for the store's own recursion.
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
    const float trialR = g_radius.load(std::memory_order_relaxed);

    float centre[3] = {};
    float radius = 0.0f;
    if (!readMem(centre, record + kRecCentre, sizeof(centre)) || !readVal(&radius, record + kRecRadius) ||
        !std::isfinite(radius) || !(radius > 0.0f) || !std::isfinite(centre[0]) || !std::isfinite(centre[1]) ||
        !std::isfinite(centre[2])) {
        g_keptUnknown.fetch_add(1, std::memory_order_relaxed);
    } else if (keepByRule(record, centre, radius, cam, trialR)) {
        g_kept.fetch_add(1, std::memory_order_relaxed);
    } else {
        RecordFields fields{record};
        sealed::CullField failed = sealed::CullField::kMaskRead;
        uint64_t value = 0;
        switch (sealed::cullRecordFaultSafe(fields, &failed, &value)) {
            case sealed::Cull::kCulled:
                g_culled.fetch_add(1, std::memory_order_relaxed);
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
    if (g_repPhase.load(std::memory_order_relaxed) == kRepCollecting && g_observed.size() < g_observed.capacity())
        g_observed.push_back(Observed{bucket, record});
    QueryPerformanceCounter(&t1);
    g_evalNs.fetch_add(static_cast<uint64_t>(nanoseconds(t0, t1)), std::memory_order_relaxed);
}

}  // namespace edvr
