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
#include "journal_watch.h"
#include "kinematic_eval_hook.h"
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
constexpr size_t kKeptNamedRows = 4000;
constexpr float kCulledNearAxisM = 90.0f;    // Built v12.1 (60 m); v13.3: 90 m, so the elevators 63 to 72 m off the shaft are listed
constexpr size_t kCulledNearRows = 2000;      // Built v11: kept named records the report lists (nearest the capsule's axis first)
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
// Built v10: producer B's veto: the mode (diag.sealed_trial_producer) and the window's counts (under the lock).
sealed::ProducerMode g_producerMode = sealed::ProducerMode::kVeto;
uint64_t g_prodCalls = 0, g_prodVetoed = 0, g_prodKept = 0, g_prodStyle = 0, g_prodUnread = 0;
uint64_t g_worldSrc[5] = {};   // Built v11: the named rule's world centre by source (sealed_trial_core.h Window::worldSrc)
std::atomic<uint32_t> g_frameStamp{0};       // advanced at every Present tick: the observer resolves the view once per frame
std::atomic<bool> g_cameraKnown{false};

// Built v8.1 (the crash fix): every piece of state the store observer and the frame's code share is guarded by this lock, which is recursive
// because the store re-enters the observer through its own recursion. It is never held across a call into the game.
std::recursive_mutex g_sealedMutex;

// The named mode's cell radii (diag.sealed_cell_small_m / _medium_m / _large_m; guesses 60 / 90 / 130 m) and the size last seen. Under the lock.
float g_cellRadius[3] = {sealed::kCellDefaultM[0], sealed::kCellDefaultM[1], sealed::kCellDefaultM[2]};
// Built v11 (second addendum): the box's doubled axis (diag.sealed_cell_double: length, the default, or width) and its margin (diag.sealed_cell_margin_m,
// stepped by PAGEUP and PAGEDOWN in named mode).
bool g_doubleLength = true;
float g_margin = sealed::kMarginDefaultM;
// Built v12: the early rig veto's counts (the lifetime tally, the window's start and the drawlist probe's last take) and diag.sealed_trial_early
// (count, the default, or veto).
struct RigTally {
    uint64_t seen = 0, vetoed = 0, mixed = 0, unknown = 0, refresh = 0, matched = 0, unevaluated = 0, ns = 0;
    uint64_t assocSync = 0, assocJob = 0, assocNone = 0;   // Built v12.1: the store records by association path
};
RigTally g_rigTally, g_rigWinStart, g_rigTakeLast;
uint64_t g_trialFrames = 0, g_trialFramesWinStart = 0;   // the frame ticks (the per-frame cost) and the window's start
sealed::ProducerMode g_early = sealed::ProducerMode::kCount;
// Built v12 (flight fix b): the named cell is FROZEN at the named engage (trial on in named mode, or the mode key to named). g_heldCell is the cell
// held until the trial turns off or the mode changes. An engage that finds no cell within kEngageTries promotes stays the sphere rule (no radius-0
// fallback) until the next engage. The window counts the frames held and the fresh picks that differed (ignored, not used).
constexpr uint32_t kEngageTries = 90;
sealed::OwnCell g_heldCell;
bool g_heldValid = false;
bool g_engagePending = false;
uint32_t g_engageTries = 0;
uint64_t g_frozenFrames = 0, g_frozenSkipped = 0;
// Built v13 (fix 2): the own pad's height along its up axis e2 this frame, and the lowest seen this session (the freeze needs the pad lowered).
constexpr float kLoweredToleranceM = 15.0f;
float g_padAlong = 0.0f, g_padLowest = 0.0f;
bool g_padAlongValid = false, g_padLowestSet = false;
// Built v13 (fix 1): the own ship's type prefix, taken at engage from the record nearest the camera that contains it (a small sphere), and that
// record's name as last seen (the candidate; kept across frames, the nearest to the camera in each frame that has one).
char g_ownPrefix[32] = {};
char g_ownCandidateName[48] = {};
float g_ownCandidateDist = -1.0f;   // the candidate's distance from the camera, this frame (reset at each promote)
uint64_t g_ownExempt = 0;           // records kept by the own-ship exemption this window
char g_lastSize = 'S';

// Built v13.1 (a): the window's culled-name tallies. Containing: culled records whose sphere holds the camera (the culled_containing count, by
// name). Near box: culled records within the held box expanded by kTallyMarginM. coords: the box coordinates of the name's first record (its
// centre about the cell, when the cell is not a box).
struct NameTally {
    char name[48];
    uint32_t n;
    float coords[3];
    float radius;        // Built v13.2: the first record's sphere radius (metres)
    uint64_t children;   // Built v13.2: the first record's child count
};
constexpr size_t kTallySlots = 48;
constexpr size_t kBayNameSlots = 20;
NameTally g_tallyContain[kTallySlots] = {};
NameTally g_tallyNear[kTallySlots] = {};
uint32_t g_tallyContainUsed = 0, g_tallyNearUsed = 0;
uint64_t g_tallyContainOver = 0, g_tallyNearOver = 0;
// Built v13.1 (b): the bay rule. Records it keeps that the box alone would cull: this window, and since the engage (with the first names).
uint64_t g_bayKept = 0;
uint64_t g_bayKeptEngage = 0;
char g_bayNames[kBayNameSlots][48] = {};
uint32_t g_bayNameCount = 0;
uint32_t g_bayEngageFrame = 0;
char g_bayNeighbour[200] = {};   // the nearest other pad to the shaft, measured at the engage
// Built v13.1 (a): the automatic reports. Due 2 s after a named engage; 10 s after the player goes on foot while the trial is on.
bool g_autoEngagePending = false;
uint64_t g_autoEngageDueMs = 0;
uint64_t g_onFootSinceMs = 0;
bool g_onFootReported = false;
// Built v13.1 (c): the camera's origin (the view 0 origin the on-foot rule centres on), its motion per window, and the on-foot part of it.
bool g_camOriginValid = false;
float g_camOrigin[3] = {};
double g_camMovedM = 0.0;
uint64_t g_camFrames = 0;
uint64_t g_camOnFootFrames = 0;
double g_camOnFootMovedM = 0.0;

// Built v13.2 (a): the distance under the pad: the box's bottom and the bay's lower bound (diag.sealed_cell_below_m).
float g_belowM = sealed::kBelowDefaultM;
// Built v13.2 (a): the running minima of the position along the shaft (from the pad's centre) since the last engage: the own ship's records and
// the Door* records, each within the bay's lateral distance of the shaft.
float g_minOwnAlong = 0.0f, g_minOwnLat = 0.0f, g_minDoorAlong = 0.0f, g_minDoorLat = 0.0f;
bool g_minOwnSet = false, g_minDoorSet = false;
char g_minOwnName[48] = {}, g_minDoorName[48] = {};
uint64_t g_minFrames = 0;   // the frames the minima were taken over

// Built v13.2 (b): the own ship. Each frame the records within kOwnNearM of the camera (address, name, sphere) are listed; the previous frame's
// list is the source at an engage. The own record is re-read by address each frame: its live sphere keeps every record whose centre lies within
// it plus kOwnSphereMarginM.
constexpr float kOwnNearM = 40.0f;
constexpr float kOwnSphereMarginM = 5.0f;
constexpr size_t kNearSlots = 24;
struct NearRec {
    uintptr_t addr = 0;
    char name[48] = {};
    float centre[3] = {};
    float radius = 0.0f;
    float dist = 0.0f;
};
NearRec g_nearCur[kNearSlots], g_nearPrev[kNearSlots];
size_t g_nearCurN = 0, g_nearPrevN = 0;
uintptr_t g_ownRecAddr = 0;
char g_ownRecName[48] = {};
bool g_ownLiveValid = false;
float g_ownLiveCentre[3] = {};
float g_ownLiveRadius = 0.0f;
uint64_t g_ownSphereKeptFrame = 0, g_ownSphereKeptLast = 0, g_ownSphereKeptWin = 0;

// Built v13.3 (b): the held cell's validity. Frames the held pad record was absent from the store; frames another own pad candidate was the
// nearest while the held one was not picked. A drop clears the cell and the own record; the cell re-freezes from the next pad once it is lowered
// (the lowest-height tracking restarts when the picked pad record changes).
uint32_t g_heldAbsent = 0, g_otherPicked = 0;
bool g_refreezeWatch = false;
uintptr_t g_lowestPad = 0;
char g_droppedName[64] = {};
sealed::PadSize g_droppedSize = sealed::PadSize::kNone;

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

// Built v13.1: counts one culled record under its name, in a tally of at most kTallySlots names (the rest are counted as over).
void tallyNote(NameTally* t, uint32_t* used, uint64_t* over, const char* name, const float coords[3], float radius,
               uint64_t children) noexcept {
    for (uint32_t i = 0; i < *used; ++i) {
        if (std::strcmp(t[i].name, name) == 0) {
            ++t[i].n;
            return;
        }
    }
    if (*used >= kTallySlots) {
        ++*over;
        return;
    }
    NameTally& s = t[(*used)++];
    std::snprintf(s.name, sizeof(s.name), "%s", name);
    s.n = 1;
    for (int k = 0; k < 3; ++k) s.coords[k] = coords[k];
    s.radius = radius;
    s.children = children;
}

// The top three names by count as "name xN at (a, b, c)", then the distinct and overflow counts.
std::string tallyText(const NameTally* t, uint32_t used, uint64_t over) {
    std::string out = "[";
    bool taken[kTallySlots] = {};
    for (int k = 0; k < 3; ++k) {
        int best = -1;
        for (uint32_t i = 0; i < used; ++i) {
            if (!taken[i] && (best < 0 || t[i].n > t[best].n)) best = static_cast<int>(i);
        }
        if (best < 0) break;
        taken[best] = true;
        char part[160];
        std::snprintf(part, sizeof(part), "%s%s x%u at (%.1f, %.1f, %.1f) r=%.1f children=%llu", k ? ", " : "", t[best].name, t[best].n,
                      static_cast<double>(t[best].coords[0]), static_cast<double>(t[best].coords[1]), static_cast<double>(t[best].coords[2]),
                      static_cast<double>(t[best].radius), static_cast<unsigned long long>(t[best].children));
        out += part;
    }
    if (out == "[") out += "none";
    char tail[80];
    std::snprintf(tail, sizeof(tail), "] distinct=%u over=%llu", used, static_cast<unsigned long long>(over));
    out += tail;
    return out;
}

// Built v13.1 (a, b): the bay's kept records since the engage, with their first names, and the nearest other pad. Called by the automatic reports.
void logBay(const char* trigger, uint32_t frame) {
    std::string names;
    for (uint32_t i = 0; i < g_bayNameCount; ++i) {
        if (i) names += ", ";
        names += g_bayNames[i];
    }
    Log::get().note("sealed bay (%s, frame %u): %llu records kept since the engage at frame %u that the box alone would cull; first %u names: %s. "
                    "Nearest other pad to the shaft: %s.", trigger, frame, static_cast<unsigned long long>(g_bayKeptEngage), g_bayEngageFrame,
                    g_bayNameCount, names.empty() ? "none" : names.c_str(), g_bayNeighbour);
}

// Built v13.1 (b, c): at a named engage, the bay's rule is stated and the nearest other pad's lateral distance to the shaft is measured from the
// pads the store saw last frame (the own pad excluded). Also arms the automatic report due 2 s on.
void beginBay(const sealed::OwnCell& c, uint32_t frame) {
    g_bayKeptEngage = 0;
    g_bayNameCount = 0;
    g_bayEngageFrame = frame;
    g_autoEngagePending = true;
    g_autoEngageDueMs = GetTickCount64() + 2000;
    float u[3] = {};
    if (!c.capsule || !sealed::unitBetween(c.capA, c.capB, u)) {
        std::snprintf(g_bayNeighbour, sizeof(g_bayNeighbour), "not measured (no shaft at the engage)");
    } else {
        float best = -1.0f;
        uint32_t others = 0;
        char bestName[64] = {};
        for (size_t i = 0; i < g_padCount; ++i) {
            const sealed::PadObs& p = g_pads[i];
            if (p.addr == c.pad) continue;
            // Built v13.2: the own pad's second platform (the same size, within 30 m of its centre) is not a neighbour (it moves with the pad).
            if (p.size == c.size && sealed::distance(p.centre, c.centre) < 30.0f) continue;
            ++others;
            const float lat = sealed::lateralToAxis(p.centre, c.capA, u);
            if (best < 0.0f || lat < best) {
                best = lat;
                std::snprintf(bestName, sizeof(bestName), "%s", p.name);
            }
        }
        if (best < 0.0f)
            std::snprintf(g_bayNeighbour, sizeof(g_bayNeighbour), "none seen (0 other pads in last frame's records)");
        else
            std::snprintf(g_bayNeighbour, sizeof(g_bayNeighbour), "%s at %.1f m lateral of the shaft (%u other pads seen)", bestName,
                          static_cast<double>(best), others);
    }
    Log::get().note("sealed bay (engage frame %u): keeps records named Hanger, Hangar, Lobby, BlastShield, Skirt, LandingPad, Lift or Gate within %.0f m "
                    "lateral of the shaft, from %.0f m below the pad to %.0f m past the target (target %.1f m from the pad; diag.sealed_cell_below_m). "
                    "Nearest other pad: %s.", frame, static_cast<double>(sealed::kBayLateralM), static_cast<double>(g_belowM),
                    static_cast<double>(sealed::kBayPastM), static_cast<double>(c.capLen), g_bayNeighbour);
    // Built v13.2 (a): the minima over the frames since the previous engage, so the next flight can size the below distance. Own ship: the records
    // with the type prefix; doors: the Door* records. Both within the bay's lateral distance of the shaft.
    if (g_minFrames > 0)
        Log::get().note("sealed bay minima (since the previous engage, %llu frames): own ship %s at along %.1f m (lateral %.1f m); doors %s at "
                        "along %.1f m (lateral %.1f m). Along is up the shaft from the pad's centre: negative is below the pad.",
                        static_cast<unsigned long long>(g_minFrames), g_minOwnSet ? g_minOwnName : "none",
                        static_cast<double>(g_minOwnSet ? g_minOwnAlong : 0.0f), static_cast<double>(g_minOwnSet ? g_minOwnLat : 0.0f),
                        g_minDoorSet ? g_minDoorName : "none", static_cast<double>(g_minDoorSet ? g_minDoorAlong : 0.0f),
                        static_cast<double>(g_minDoorSet ? g_minDoorLat : 0.0f));
    g_minOwnSet = g_minDoorSet = false;
    g_minFrames = 0;
}

// Built v13.1 (c): the camera's origin once per frame, its motion over the window, and the part of it on foot.
void noteCameraFrame() noexcept {
    if (!g_view.originOk) return;
    const bool foot = journalOnFootKnown() && journalOnFoot();
    if (g_camOriginValid) {
        const double m = static_cast<double>(sealed::distance(g_view.origin, g_camOrigin));
        g_camMovedM += m;
        if (foot) g_camOnFootMovedM += m;
    }
    for (int k = 0; k < 3; ++k) g_camOrigin[k] = g_view.origin[k];
    g_camOriginValid = true;
    ++g_camFrames;
    if (foot) ++g_camOnFootFrames;
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
    g_prodCalls = g_prodVetoed = g_prodKept = g_prodStyle = g_prodUnread = 0;
    for (auto& v : g_worldSrc) v = 0;
    g_rigWinStart = g_rigTally;   // Built v12: the window's rig counts start here
    g_trialFramesWinStart = g_trialFrames;
    g_frozenFrames = g_frozenSkipped = 0;
    g_ownExempt = 0;
    g_tallyContainUsed = g_tallyNearUsed = 0;
    g_tallyContainOver = g_tallyNearOver = 0;
    g_bayKept = 0;
    g_camFrames = g_camOnFootFrames = 0;
    g_camMovedM = g_camOnFootMovedM = 0.0;
    g_ownSphereKeptWin = 0;
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
    w.producerCalls = g_prodCalls;
    w.producerVetoed = g_prodVetoed;
    w.producerKept = g_prodKept;
    w.producerStyle = g_prodStyle;
    w.producerUnread = g_prodUnread;
    w.producerMode = sealed::producerModeName(g_producerMode);
    for (int k = 0; k < 5; ++k) w.worldSrc[k] = g_worldSrc[k];
    w.rigsSeen = g_rigTally.seen - g_rigWinStart.seen;
    w.rigsVetoed = g_rigTally.vetoed - g_rigWinStart.vetoed;
    w.rigsMixed = g_rigTally.mixed - g_rigWinStart.mixed;
    w.rigsUnknown = g_rigTally.unknown - g_rigWinStart.unknown;
    w.rigsRefresh = g_rigTally.refresh - g_rigWinStart.refresh;
    w.earlyMs = static_cast<double>(g_rigTally.ns - g_rigWinStart.ns) / 1.0e6;
    w.rigRecMatched = g_rigTally.matched - g_rigWinStart.matched;
    w.rigRecUnevaluated = g_rigTally.unevaluated - g_rigWinStart.unevaluated;
    w.rigEmuFaults = kinematicEvalRigEmulateFaults();
    w.assocSync = g_rigTally.assocSync - g_rigWinStart.assocSync;
    w.assocJob = g_rigTally.assocJob - g_rigWinStart.assocJob;
    w.assocNone = g_rigTally.assocNone - g_rigWinStart.assocNone;
    w.frames = g_trialFrames - g_trialFramesWinStart;
    w.ownKept = g_ownExempt;
    w.ownPrefix = g_ownPrefix;
    w.earlyMode = sealed::producerModeName(g_early);
    w.cellFrozen = g_heldValid;
    w.frozenFrames = g_frozenFrames;
    w.frozenSkipped = g_frozenSkipped;
    w.boxOn = g_cellPrev.box;
    for (int k = 0; k < 3; ++k) w.boxHalf[k] = g_cellPrev.box ? g_cellPrev.boxHalf[k] : 0.0f;
    w.boxMargin = g_margin;
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
    // Built v13.1: the top culled names, the bay, and the camera. camera_to_pad_m is LIVE (this frame's camera to the held pad's centre); pad_dist
    // above is the value frozen at the engage.
    const float camToPad = (g_frameCamValid && g_cellPrev.has) ? sealed::distance(g_frameCam, g_cellPrev.centre) : -1.0f;
    char camText[200];
    std::snprintf(camText, sizeof(camText), "camera_source=%s camera_to_pad_m=%.2f camera_frames=%llu origin_moved_m=%.2f on_foot_frames=%llu "
                  "on_foot_origin_moved_m=%.2f", view::sourceName(g_view.source), static_cast<double>(camToPad),
                  static_cast<unsigned long long>(g_camFrames), g_camMovedM, static_cast<unsigned long long>(g_camOnFootFrames), g_camOnFootMovedM);
    // Built v13.3 (d): the culled tops are their own line (noteWindow), not part of this one.
    const std::string tops = "containing " + tallyText(g_tallyContain, g_tallyContainUsed, g_tallyContainOver) + "; near_box " +
                             tallyText(g_tallyNear, g_tallyNearUsed, g_tallyNearOver);
    std::snprintf(w.culledTop, sizeof(w.culledTop), "%s", tops.c_str());
    std::string extra = "kept_unreadable=" + std::to_string(g_keptUnknown.load()) + " bay_kept=" + std::to_string(g_bayKept) + " " + camText +
                        " early_ms_scope=frame_tick_refresh_only (decisions untimed in v13.1) own_record=" +
                        (g_ownRecAddr && g_ownRecName[0] ? std::string(g_ownRecName) : std::string("none")) +
                        " own_record_live=" + (g_ownLiveValid ? "yes" : "no") + " own_record_radius_m=" +
                        std::to_string(g_ownLiveValid ? static_cast<double>(g_ownLiveRadius) : 0.0) +
                        " own_sphere_kept_last_frame=" + std::to_string(g_ownSphereKeptLast) +
                        " own_sphere_kept_window=" + std::to_string(g_ownSphereKeptWin) + " ";
    std::snprintf(w.extraText, sizeof(w.extraText), "%s", extra.c_str());
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
    char line[2600];
    const sealed::Window win = takeWindow();
    sealed::formatWindow(line, sizeof(line), win, g_radius.load(std::memory_order_relaxed), viewLabel(g_view),
                         g_mode.load(std::memory_order_relaxed));
    Log::get().note("%s", line);
    Log::get().note("sealed culled top: %s", win.culledTop);   // Built v13.3 (d): the names in full, on their own line
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

// --- Built v11: the named rule's world centre, one rule for every named position (own pad, explicit parts, the cell test). The order, per record:
// the producer B cache (the veto's store-style sphere, for the records B was called on this frame), the pose (builder, path A), the store-style
// sphere (the node block through rec+0x190; storeSphereOf), and the raw rec+0x240 as the last fallback.
namespace {
// The store-style world sphere (decomp_4312040.txt lines 212-262, sealed_trial_core.h storeWorldSphere). False when any read fails or the result
// is not finite. Reads only.
bool storeSphereOf(uintptr_t rec, float centre[3], float* radius) noexcept {
    constexpr uintptr_t kNodeOff = 0x18;       // rec+0x18: the node (census's name pointer)
    constexpr uintptr_t kBlockOff = 0x08;      // node+0x08: the bounds block
    constexpr uintptr_t kPoseOff = 0x190;      // rec+0x190: the pose block A
    constexpr uintptr_t kMatrixOff = 0x50;     // A+0x50: the matrix B
    float L[3] = {}, A[15] = {}, B[16] = {};
    float r = 0.0f;
    uintptr_t node = 0, block = 0, pose = 0, matrix = 0;
    bool ok = readVal(&node, rec + kNodeOff) && node && readVal(&block, node + kBlockOff) && block &&
              readMem(L, block + 0x20, sizeof(L)) && readVal(&r, block + 0x2C) && readVal(&pose, rec + kPoseOff) && pose &&
              readMem(A, pose, sizeof(A)) && readVal(&matrix, pose + kMatrixOff) && matrix && readMem(B, matrix, sizeof(B));
    ok = ok && std::isfinite(r) && r > 0.0f && sealed::storeWorldSphere(L, A, B, centre);
    if (ok) *radius = r;
    return ok;
}

// Built v11 (second addendum): a record's local axes in the world. The builder's quaternion (the pose at rec+0x290: q at +0x10, the extent at
// +0x90) when the record has one (path A); else the store-style pose (rec+0x190 and its +0x50 matrix; decomp_4312040.txt). src: 1 builder,
// 2 store-style, 4 neither (false). extOk: the builder extent was read (only the builder pose gives one).
bool poseOrientOf(uintptr_t rec, float axes[3][3], float ext[3], bool* extOk, uint8_t* src) noexcept {
    *extOk = false;
    *src = 4;
    uintptr_t pose = 0;
    float q[4] = {};
    if (readVal(&pose, rec + kRecPose) && pose && readMem(q, pose + 0x10, sizeof(q)) && std::isfinite(q[0]) && std::isfinite(q[1]) &&
        std::isfinite(q[2]) && std::isfinite(q[3])) {
        sealed::quatAxes(q, axes);
        *src = 1;
        if (readMem(ext, pose + 0x90, 3 * sizeof(float)) && std::isfinite(ext[0]) && std::isfinite(ext[1]) && std::isfinite(ext[2])) *extOk = true;
        return true;
    }
    uintptr_t pa = 0, matrix = 0;
    float A[15] = {}, B[16] = {};
    if (readVal(&pa, rec + 0x190) && pa && readMem(A, pa, sizeof(A)) && readVal(&matrix, pa + 0x50) && matrix && readMem(B, matrix, sizeof(B))) {
        sealed::storeAxes(A, B, axes);
        *src = 2;
        return true;
    }
    return false;
}

// The producer B cache: per record, the centre and radius B's veto decision used this frame. A generation per frame (the stamp moves at Present)
// makes a stale slot empty; under the lock.
constexpr size_t kProdSlots = 8192;
struct ProdSlot {
    uintptr_t rec = 0;
    uint32_t gen = 0;
    float c[3] = {};
    float r = 0.0f;
};
ProdSlot g_prodSlots[kProdSlots];
uint32_t g_prodGen = 1;          // the generation the slots are valid for (0 is never valid)
uint32_t g_prodStampSeen = 0;

void prodSync() noexcept {
    const uint32_t s = g_frameStamp.load(std::memory_order_relaxed);
    if (s != g_prodStampSeen) {
        g_prodStampSeen = s;
        ++g_prodGen;
    }
}

size_t prodIndex(uintptr_t rec) noexcept {
    return static_cast<size_t>((static_cast<uint64_t>(rec) >> 4) * 0x9E3779B97F4A7C15ull >> 51) & (kProdSlots - 1);
}

void prodPut(uintptr_t rec, const float c[3], float r) noexcept {
    size_t i = prodIndex(rec);
    for (size_t n = 0; n < kProdSlots; ++n, i = (i + 1) & (kProdSlots - 1)) {
        ProdSlot& s = g_prodSlots[i];
        if (s.gen != g_prodGen || s.rec == rec) {
            s.rec = rec;
            s.gen = g_prodGen;
            s.c[0] = c[0];
            s.c[1] = c[1];
            s.c[2] = c[2];
            s.r = r;
            return;
        }
    }
}

bool prodGet(uintptr_t rec, float c[3], float* r) noexcept {
    size_t i = prodIndex(rec);
    for (size_t n = 0; n < kProdSlots; ++n, i = (i + 1) & (kProdSlots - 1)) {
        const ProdSlot& s = g_prodSlots[i];
        if (s.gen != g_prodGen) return false;
        if (s.rec == rec) {
            c[0] = s.c[0];
            c[1] = s.c[1];
            c[2] = s.c[2];
            *r = s.r;
            return true;
        }
    }
    return false;
}

struct WorldRead {
    float world[3] = {};
    float radius = 0.0f;
    uint8_t source = 4;   // 0 producer B, 1 pose, 2 store-style, 3 raw fallback, 4 none
};

// The named rule's world centre and radius for a record (see the block comment above). Called under the lock.
WorldRead worldOfRecord(uintptr_t rec) noexcept {
    WorldRead w;
    prodSync();
    if (prodGet(rec, w.world, &w.radius)) {
        w.source = 0;
        return w;
    }
    float p[3] = {};
    if (poseCentreOf(rec, p)) {
        float r = 0.0f;
        if (readVal(&r, rec + kRecRadius) && std::isfinite(r) && r > 0.0f) {
            for (int k = 0; k < 3; ++k) w.world[k] = p[k];
            w.radius = r;
            w.source = 1;
        }
        return w;
    }
    if (storeSphereOf(rec, w.world, &w.radius)) {
        w.source = 2;
        return w;
    }
    float raw[3] = {};
    float r = 0.0f;
    // Built v13.3 (c): a zero raw centre is unreadable, not the origin: no sphere (the record is kept as unknown in named mode).
    if (readMem(raw, rec + kRecCentre, sizeof(raw)) && readVal(&r, rec + kRecRadius) && sealed::rawCentreUsable(raw, r)) {
        for (int k = 0; k < 3; ++k) w.world[k] = raw[k];
        w.radius = r;
        w.source = 3;
    }
    return w;
}
}  // namespace

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

// Built v11 (second addendum): the oriented box for the own pad, from the pad's own orientation. The length axis is the pad's footprint axis with
// the larger builder extent; the doubled axis, the wiki size and the margin give the half-extents. When the orientation or the extent cannot be
// read, the box is not built and the capsule (or the pad's sphere) stays the cell, with the reason in boxInfo.
void buildBoxFor(sealed::OwnCell& cell, const float target[3]) {
    cell.box = false;
    char why[240] = {};
    float axes[3][3] = {}, ext[3] = {};
    bool extOk = false;
    uint8_t src = 4;
    if (!cell.has || cell.size == sealed::PadSize::kNone) {
        std::snprintf(why, sizeof(why), "no sized pad");
    } else if (!poseOrientOf(cell.pad, axes, ext, &extOk, &src)) {
        std::snprintf(why, sizeof(why), "the pad's orientation is not readable (no builder pose, no store-style pose block)");
    } else if (!extOk) {
        std::snprintf(why, sizeof(why), "the pad has no builder extent (store-style orientation only), so its length and width cannot be told apart");
    } else {
        float u[3] = {target[0] - cell.centre[0], target[1] - cell.centre[1], target[2] - cell.centre[2]};
        const float ul = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
        int li = -1, wi = -1, ui = -1;
        if (!(ul >= 1.0f)) {
            std::snprintf(why, sizeof(why), "the shaft or the footprint could not be found");
        } else {
            for (float& v : u) v /= ul;
            if (!sealed::footprintAxes(axes, u, ext, &li, &wi, &ui)) {
                std::snprintf(why, sizeof(why), "the shaft or the footprint could not be found");
                std::snprintf(cell.boxInfo, sizeof(cell.boxInfo), "not built: %s", why);
                return;
            }
            const float* wiki = sealed::kPadWikiM[sealed::sizeIndex(cell.size)];
            const float halfLen = wiki[0] * 0.5f * (g_doubleLength ? 2.0f : 1.0f);
            const float halfWid = wiki[1] * 0.5f * (g_doubleLength ? 1.0f : 2.0f);
            if (!sealed::buildBox(cell.centre, target, axes[li], halfLen, halfWid, g_margin, g_belowM, &cell)) {
                std::snprintf(why, sizeof(why), "the box could not be built");
            } else {
                // v12.1: the axis is pad->target; the angle between it and the pad's own e2 is logged (the v12 flight's question).
                const float angleToE2 = sealed::angleDeg(cell.boxW, axes[2]);
                std::snprintf(cell.boxInfo, sizeof(cell.boxInfo),
                              "box axis = pad to BlastShieldBase (%.3f, %.3f, %.3f), %.1f deg from the pad's e2 (%.3f, %.3f, %.3f); "
                              "pad axes e0 (%.2f, %.2f, %.2f), e1 (%.2f, %.2f, %.2f), e2 (%.2f, %.2f, %.2f); length e%d (orthogonalised), width e%d; "
                              "builder extent (%.1f, %.1f, %.1f) m; wiki %.0f x %.0f m; doubled on the %s axis; source %s",
                              static_cast<double>(cell.boxW[0]), static_cast<double>(cell.boxW[1]), static_cast<double>(cell.boxW[2]),
                              static_cast<double>(angleToE2), static_cast<double>(axes[2][0]), static_cast<double>(axes[2][1]),
                              static_cast<double>(axes[2][2]), static_cast<double>(axes[0][0]), static_cast<double>(axes[0][1]),
                              static_cast<double>(axes[0][2]), static_cast<double>(axes[1][0]), static_cast<double>(axes[1][1]),
                              static_cast<double>(axes[1][2]), static_cast<double>(axes[2][0]), static_cast<double>(axes[2][1]),
                              static_cast<double>(axes[2][2]), li, wi, static_cast<double>(ext[0]), static_cast<double>(ext[1]),
                              static_cast<double>(ext[2]), static_cast<double>(wiki[0]), static_cast<double>(wiki[1]),
                              g_doubleLength ? "length" : "width", src == 1 ? "builder quaternion" : "store-style pose");
                return;
            }
        }
    }
    std::snprintf(cell.boxInfo, sizeof(cell.boxInfo), "not built: %s", why);
}

// Built v11 (second addendum): the box's log, once per change (the box appears or goes, its centre moves by more than 5 cm, or the reason changes).
void logBoxChange(const sealed::OwnCell& c, uint32_t frame) {
    static bool lastBox = false;
    static float lastCentre[3] = {};
    static char lastInfo[320] = {};
    bool changed = c.box != lastBox;
    if (c.box && lastBox) {
        for (int k = 0; k < 3; ++k)
            if (std::fabs(c.boxCentre[k] - lastCentre[k]) > 0.05f) changed = true;
    }
    if (!c.box && std::strcmp(c.boxInfo, lastInfo) != 0) changed = true;
    if (!changed) return;
    lastBox = c.box;
    std::memcpy(lastCentre, c.boxCentre, sizeof(lastCentre));
    std::snprintf(lastInfo, sizeof(lastInfo), "%s", c.box ? "" : c.boxInfo);
    if (!c.box) {
        if (c.boxInfo[0] != '\0')
            Log::get().note("sealed box: at frame %u: %s. The capsule (or the pad's sphere) is the cell.", frame, c.boxInfo);
        return;
    }
    Log::get().note("sealed box: at frame %u: %s. Box centre (%.1f, %.1f, %.1f); half-extents length %.1f, width %.1f, height %.1f m (margin %.0f m); "
                    "length axis (%.3f, %.3f, %.3f), width axis (%.3f, %.3f, %.3f), shaft (%.3f, %.3f, %.3f).", frame, c.boxInfo,
                    static_cast<double>(c.boxCentre[0]), static_cast<double>(c.boxCentre[1]), static_cast<double>(c.boxCentre[2]),
                    static_cast<double>(c.boxHalf[0]), static_cast<double>(c.boxHalf[1]), static_cast<double>(c.boxHalf[2]),
                    static_cast<double>(c.boxMargin), static_cast<double>(c.boxU[0]), static_cast<double>(c.boxU[1]),
                    static_cast<double>(c.boxU[2]), static_cast<double>(c.boxV[0]), static_cast<double>(c.boxV[1]),
                    static_cast<double>(c.boxV[2]), static_cast<double>(c.boxW[0]), static_cast<double>(c.boxW[1]),
                    static_cast<double>(c.boxW[2]));
}

// Built v11: the capsule's axis, logged once per change (the pad or the target moved by more than 5 cm, or the capsule came or went). Under the lock.
void logCapsuleChange(const sealed::OwnCell& c, uint32_t frame) {
    static bool lastCap = false;
    static float lastA[3] = {}, lastB[3] = {};
    bool changed = c.capsule != lastCap;
    if (c.capsule && lastCap) {
        for (int k = 0; k < 3; ++k)
            if (std::fabs(c.capA[k] - lastA[k]) > 0.05f || std::fabs(c.capB[k] - lastB[k]) > 0.05f) changed = true;
    }
    if (!changed) return;
    lastCap = c.capsule;
    for (int k = 0; k < 3; ++k) {
        lastA[k] = c.capA[k];
        lastB[k] = c.capB[k];
    }
    if (!c.capsule) {
        Log::get().note("sealed cell: no capsule at frame %u (no BlastShieldBase or own Hanger within %.0f m of the pad, or no pad); the cell is "
                        "the pad's sphere of radius %.1f m", frame, static_cast<double>(sealed::kCapsuleMaxM), static_cast<double>(c.radius));
        return;
    }
    const float dx = c.capB[0] - c.capA[0], dy = c.capB[1] - c.capA[1], dz = c.capB[2] - c.capA[2];
    const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
    const float inv = len > 0.0f ? 1.0f / len : 0.0f;
    Log::get().note("sealed cell: capsule at frame %u: axis from the pad (%.1f, %.1f, %.1f) to %s, extended %.0f m past it; length %.1f m, "
                    "direction (%.3f, %.3f, %.3f); radius %.1f m (the cell radius for the pad's size)", frame, static_cast<double>(c.centre[0]),
                    static_cast<double>(c.centre[1]), static_cast<double>(c.centre[2]), c.capTarget,
                    static_cast<double>(sealed::kCapsuleExtendM), static_cast<double>(c.capLen), static_cast<double>(dx * inv),
                    static_cast<double>(dy * inv), static_cast<double>(dz * inv), static_cast<double>(c.radius));
}

// Built v13 (fix 1): at each named engage, the own ship's type prefix from the candidate record (the one nearest the camera that contains it), logged.
void setOwnPrefixAtEngage(uint32_t frame) {
    sealed::ownPrefixOf(g_ownCandidateName, g_ownPrefix, sizeof(g_ownPrefix));
    if (g_ownPrefix[0] != '\0')
        Log::get().note("own ship (frame %u): type prefix '%s' from the record %s (nearest the camera among those containing it). It is not culled in "
                        "sphere or named mode; all mode still culls it.", frame, g_ownPrefix, g_ownCandidateName);
    else
        Log::get().note("own ship (frame %u): no candidate with an underscored name yet; the small-sphere exemption (radius under %.0f m, camera "
                        "inside) alone applies.", frame, static_cast<double>(sealed::kOwnShipRadiusM));
}

// Built v13 (fix 3): the shaft pick's log, once per change of the target: the target, its perpendicular and along distances from the pad's up line,
// and the nearest-to-the-pad BlastShieldBase's distance, so the two picks can be compared in the log.
void logAxisPick(const sealed::OwnCell& c, const float e2[3], bool haveAxes, float perp, float along, uint32_t frame) {
    static char lastTarget[48] = {};
    static float lastPerp = -1.0f;
    if (std::strcmp(lastTarget, c.capTarget) == 0 && std::fabs(lastPerp - perp) < 0.05f) return;
    std::snprintf(lastTarget, sizeof(lastTarget), "%s", c.capTarget);
    lastPerp = perp;
    float nearQ[3] = {};
    char nearN[48] = {};
    float nearD = -1.0f;
    if (sealed::pickCapsuleTarget(g_explicits, g_explicitCount, c.centre, nearQ, nearN, sizeof(nearN))) nearD = sealed::distance(nearQ, c.centre);
    if (haveAxes)
        Log::get().note("sealed axis (frame %u): target %s chosen on the pad's up line: %.1f m perpendicular, %.1f m along it; the nearest to the pad "
                        "was %s at %.1f m. Axis = pad to target, pad e2 (%.3f, %.3f, %.3f).", frame, c.capTarget, static_cast<double>(perp),
                        static_cast<double>(along), nearN, static_cast<double>(nearD), static_cast<double>(e2[0]), static_cast<double>(e2[1]),
                        static_cast<double>(e2[2]));
    else
        Log::get().note("sealed axis (frame %u): target %s, nearest to the pad (no pose, so no up line): %.1f m. ", frame, c.capTarget,
                        static_cast<double>(nearD));
}

// Built v12 (flight fix c): once per named engage, the held cell, its source, and the frozen pad (its record, its centre, its axes and extent).
void logEngage(const sealed::OwnCell& c, uint32_t frame) {
    if (c.box) {
        Log::get().note("sealed cell: HELD at the named engage (frame %u): source OWN BOX; pad record 0x%llX at (%.2f, %.2f, %.2f); box centre (%.2f, "
                        "%.2f, %.2f); half-extents length %.1f, width %.1f, height %.1f m (the box axis is the pad to BlastShieldBase). "
                        "Frozen pad: %s. Held until the trial is off or the mode changes.", frame, static_cast<unsigned long long>(c.pad),
                        static_cast<double>(c.centre[0]), static_cast<double>(c.centre[1]), static_cast<double>(c.centre[2]),
                        static_cast<double>(c.boxCentre[0]), static_cast<double>(c.boxCentre[1]), static_cast<double>(c.boxCentre[2]),
                        static_cast<double>(c.boxHalf[0]), static_cast<double>(c.boxHalf[1]), static_cast<double>(c.boxHalf[2]), c.boxInfo);
    } else if (c.capsule) {
        Log::get().note("sealed cell: HELD at the named engage (frame %u): source CAPSULE (no box: %s); pad record 0x%llX at (%.2f, %.2f, %.2f); "
                        "capsule radius %.1f m to %s. Held until the trial is off or the mode changes.", frame, c.boxInfo,
                        static_cast<unsigned long long>(c.pad), static_cast<double>(c.centre[0]), static_cast<double>(c.centre[1]),
                        static_cast<double>(c.centre[2]), static_cast<double>(c.radius), c.capTarget);
    } else {
        Log::get().note("sealed cell: HELD at the named engage (frame %u): source PAD SPHERE (no capsule, no box); pad record 0x%llX at (%.2f, %.2f, "
                        "%.2f), radius %.1f m. Held until the trial is off or the mode changes.", frame, static_cast<unsigned long long>(c.pad),
                        static_cast<double>(c.centre[0]), static_cast<double>(c.centre[1]), static_cast<double>(c.centre[2]),
                        static_cast<double>(c.radius));
    }
}

// Promotes the previous frame's pads into the cell for the frame now starting, and the explicit instances for that cell. Called under the lock
// at the frame's first record. The pads and explicit parts are the previous frame's; the camera is the previous frame's too.
// Built v13.2 (b): re-reads the own record by address (its centre and radius move with the pad). A record whose name no longer matches is dropped:
// the address was freed or reused, and the rule stands down until the next engage.
void readOwnLive() noexcept {
    g_ownLiveValid = false;
    if (!g_ownRecAddr) return;
    const WorldRead wr = worldOfRecord(g_ownRecAddr);
    char nm[48] = {};
    if (wr.source == 4 || !cachedName(g_ownRecAddr, nm, sizeof(nm)) || !sealed::ownRecordMatch(nm, g_ownRecName)) {
        g_ownRecAddr = 0;
        return;
    }
    if (!std::isfinite(wr.radius) || !(wr.radius > 0.0f)) return;
    std::memcpy(g_ownLiveCentre, wr.world, sizeof(g_ownLiveCentre));
    g_ownLiveRadius = wr.radius;
    g_ownLiveValid = true;
}

// Built v13.2 (b): at a named engage the own record is the one named as the stripped type prefix in the previous frame's near list: containing the
// camera preferred, then the nearest. Logged either way.
void pickOwnRecord(uint32_t frame) {
    g_ownRecAddr = 0;
    g_ownLiveValid = false;
    g_ownRecName[0] = '\0';
    char stripped[48] = {};
    if (!sealed::strippedPrefix(g_ownPrefix, stripped, sizeof(stripped))) {
        Log::get().note("sealed own ship (engage frame %u): no type prefix with an underscore; no own record, the small-sphere exemption alone applies.",
                        frame);
        return;
    }
    int best = -1;
    bool bestIn = false;
    for (size_t i = 0; i < g_nearPrevN; ++i) {
        const NearRec& r = g_nearPrev[i];
        if (!sealed::ownRecordMatch(r.name, stripped)) continue;
        const bool in = r.dist <= r.radius;
        if (best < 0 || (in && !bestIn) || (in == bestIn && r.dist < g_nearPrev[best].dist)) {
            best = static_cast<int>(i);
            bestIn = in;
        }
    }
    if (best < 0) {
        Log::get().note("sealed own ship (engage frame %u): no record named %s within %.0f m of the camera in the previous frame; the prefix rule alone "
                        "applies.", frame, stripped, static_cast<double>(kOwnNearM));
        return;
    }
    const NearRec& r = g_nearPrev[best];
    g_ownRecAddr = r.addr;
    std::snprintf(g_ownRecName, sizeof(g_ownRecName), "%s", r.name);
    readOwnLive();
    Log::get().note("sealed own ship (engage frame %u): own record %s, %.2f m from the camera, radius %.2f m (the camera %s it). Every frame, each record "
                    "whose centre lies within its current sphere plus %.0f m is kept; %s.", frame, g_ownRecName, static_cast<double>(r.dist),
                    static_cast<double>(r.radius), bestIn ? "is inside" : "is outside",
                    static_cast<double>(kOwnSphereMarginM), g_ownLiveValid ? "live sphere read" : "the live read failed; the rule stands down");
}

void promoteFrame(uint32_t stamp, uint32_t frame) {
    sealed::OwnCell next;
    g_ownCandidateDist = -1.0f;   // Built v13: the own-ship candidate is re-chosen from this frame's records
    // Built v13.2: the previous frame's near list is the engage's source; the own record's live sphere is re-read this frame; the per-frame keep
    // count rolls over; the minima count frames while a cell is in force.
    std::memcpy(g_nearPrev, g_nearCur, sizeof(g_nearPrev));
    g_nearPrevN = g_nearCurN;
    g_nearCurN = 0;
    g_ownSphereKeptLast = g_ownSphereKeptFrame;
    g_ownSphereKeptFrame = 0;
    readOwnLive();
    if (g_cellPrev.capsule) ++g_minFrames;
    if (g_mode.load(std::memory_order_relaxed) == sealed::Mode::kNamed) {
        if (g_frameCamValid) {
            const float radii[3] = {g_cellRadius[0], g_cellRadius[1], g_cellRadius[2]};
            sealed::pickOwnPad(g_pads, g_padCount, g_frameCam, radii, &next);
            // Built v11: the capsule. The axis runs from the pad's centre to the target, on 20 m past it. Built v13 (fix 3): the target is the
            // BlastShieldBase nearest the line through the pad's centre along its own up axis e2 (perpendicular distance, ties by the distance along
            // the axis), else the same for the own hangar parts, else (no pose to give an up axis) the nearest to the pad.
            if (next.has) {
                float e2[3] = {};
                float axes[3][3] = {};
                float ext[3] = {};
                bool extOk = false;
                uint8_t src = 4;
                const bool haveAxes = poseOrientOf(next.pad, axes, ext, &extOk, &src);
                g_padAlongValid = false;
                if (haveAxes) {
                    for (int k = 0; k < 3; ++k) e2[k] = axes[2][k];
                    // The pad's height along its up axis, tracked for the lowered-pad check at engage (fix 2).
                    g_padAlong = next.centre[0] * e2[0] + next.centre[1] * e2[1] + next.centre[2] * e2[2];
                    g_padAlongValid = true;
                    // Built v13.3 (b): the lowest height is tracked per picked pad record; it restarts when the pick changes to another record.
                    if (!g_padLowestSet || g_lowestPad != next.pad) {
                        g_padLowest = g_padAlong;
                        g_padLowestSet = true;
                        g_lowestPad = next.pad;
                    } else if (g_padAlong < g_padLowest) {
                        g_padLowest = g_padAlong;
                    }
                }
                float q[3] = {};
                char qn[48] = {};
                float perp = 0.0f, along = 0.0f;
                bool picked = false;
                if (haveAxes) {
                    picked = sealed::pickTargetOnLine(g_explicits, g_explicitCount, next.centre, e2, 0, q, qn, sizeof(qn), &perp, &along) ||
                             sealed::pickTargetOnLine(g_explicits, g_explicitCount, next.centre, e2, 1, q, qn, sizeof(qn), &perp, &along);
                }
                if (!picked) picked = sealed::pickCapsuleTarget(g_explicits, g_explicitCount, next.centre, q, qn, sizeof(qn));
                if (picked && sealed::buildCapsule(next.centre, q, sealed::kCapsuleExtendM, next.capA, next.capB, &next.capLen)) {
                    next.capsule = true;
                    std::snprintf(next.capTarget, sizeof(next.capTarget), "%s", qn);
                    // Built v13: both distances. Built v13.1: logged at the engage only, the frame the cell is frozen from (the per-frame pick wanders
                    // while the cell is not held, and the log would show that wander as if it were the cell).
                    if (g_engagePending) logAxisPick(next, e2, haveAxes, perp, along, frame);
                    buildBoxFor(next, q);   // Built v11 (second addendum): the box when the pad's orientation and extent are readable
                }
            }
        } else {
            std::snprintf(next.reason, sizeof(next.reason), "no camera for the previous frame");
        }
    } else {
        std::snprintf(next.reason, sizeof(next.reason), "sphere mode");
    }
    // Built v13.3 (b): the held cell is dropped when its pad record is absent from the store for kHeldLostFrames frames, or when another own pad
    // candidate (a pad containing the camera or within kOwnPadNearM of it, any size) is the nearest one for that many frames while the held pad is
    // not the one picked. Each drop is logged with the old and the new pad. The sphere rule holds until a pad is lowered; then the cell re-freezes.
    if (g_mode.load(std::memory_order_relaxed) == sealed::Mode::kNamed && g_heldValid) {
        bool heldSeen = false;
        const sealed::PadObs* best = nullptr;
        float bestD = 0.0f;
        for (size_t i = 0; i < g_padCount; ++i) {
            const sealed::PadObs& p = g_pads[i];
            if (p.addr == g_heldCell.pad) heldSeen = true;
            if (!g_frameCamValid) continue;
            const float d = sealed::distance(p.centre, g_frameCam);
            if (!sealed::ownPadCandidate(d, p.radius)) continue;
            if (!best || d < bestD) {
                best = &p;
                bestD = d;
            }
        }
        g_heldAbsent = heldSeen ? 0 : g_heldAbsent + 1;
        const bool otherPick = best != nullptr && best->addr != g_heldCell.pad;
        g_otherPicked = otherPick ? g_otherPicked + 1 : 0;
        if (sealed::heldCellDrops(g_heldAbsent, g_otherPicked, sealed::kHeldLostFrames)) {
            const bool absent = g_heldAbsent >= sealed::kHeldLostFrames;
            std::snprintf(g_droppedName, sizeof(g_droppedName), "%s", g_heldCell.padName);
            g_droppedSize = g_heldCell.size;
            Log::get().note("sealed cell: DROPPED (frame %u): held pad %s (size %c) %s for %u frames; the nearest own pad is now %s. Named mode is the "
                            "sphere rule until a pad is lowered; the cell re-freezes from it.", frame, g_heldCell.padName,
                            sealed::padSizeLetter(g_heldCell.size), absent ? "is not seen by the store" : "is not the nearest own pad",
                            absent ? g_heldAbsent : g_otherPicked, otherPick ? best->name : "none");
            g_heldValid = false;
            g_heldCell = sealed::OwnCell{};
            g_heldAbsent = 0;
            g_otherPicked = 0;
            g_refreezeWatch = true;
            g_ownRecAddr = 0;   // the own record belongs to the dropped ship; the next engage names the new one
            g_ownLiveValid = false;
        }
    }
    if (g_refreezeWatch && g_mode.load(std::memory_order_relaxed) == sealed::Mode::kNamed && next.has && next.radius > 0.0f && g_padAlongValid &&
        sealed::padLowered(g_padAlong, g_padLowest, kLoweredToleranceM)) {
        g_refreezeWatch = false;   // the new pad is lowered: the engage path below freezes the cell from it
        g_engagePending = true;
        g_engageTries = 0;
    }
    // Built v12 (flight fix b): the freeze. Named mode uses the cell held from its engage; a fresh pick never changes it. Pending (no cell yet) and
    // not-held named frames are the sphere rule. Outside named mode nothing is held.
    if (g_mode.load(std::memory_order_relaxed) != sealed::Mode::kNamed) {
        g_engagePending = false;
        g_heldValid = false;
        g_refreezeWatch = false;
    } else if (g_engagePending) {
        ++g_engageTries;
        if (next.has && next.radius > 0.0f) {
            // Built v13 (fix 2): the freeze is taken only from a LOWERED pad, within kLoweredToleranceM of its lowest height seen this session.
            // Otherwise the sphere rule holds and the log says why; re-engaging later (the mode key, or the trial) tries again.
            if (!g_padAlongValid) {
                g_engagePending = false;
                Log::get().note("sealed cell: NOT frozen at the named engage (frame %u): the own pad's height cannot be read (no pose). Named mode is the "
                                "sphere rule until the next engage.", frame);
            } else if (!sealed::padLowered(g_padAlong, g_padLowest, kLoweredToleranceM)) {
                g_engagePending = false;
                Log::get().note("sealed cell: NOT frozen at the named engage (frame %u): the own pad is %.1f m above its lowest height seen this session "
                                "(%.1f m; tolerance %.0f m). Named mode is the sphere rule; re-engage when the pad is lowered.", frame,
                                static_cast<double>(g_padAlong - g_padLowest), static_cast<double>(g_padLowest),
                                static_cast<double>(kLoweredToleranceM));
            } else {
                setOwnPrefixAtEngage(frame);   // Built v13 (fix 1): the own ship's type prefix, taken at each engage
                pickOwnRecord(frame);          // Built v13.2 (b): the own record, named as the prefix without its underscore
                g_heldCell = next;
                g_heldValid = true;
                g_engagePending = false;
                logEngage(g_heldCell, frame);
                if (g_droppedName[0] != '\0') {   // Built v13.3 (b): the re-freeze after a drop, named both ways
                    Log::get().note("sealed cell: RE-FROZEN (frame %u) on pad %s (size %c), after the drop of %s (size %c).", frame, g_heldCell.padName,
                                    sealed::padSizeLetter(g_heldCell.size), g_droppedName, sealed::padSizeLetter(g_droppedSize));
                    g_droppedName[0] = '\0';
                }
                beginBay(g_heldCell, frame);   // Built v13.1 (b, a): the bay rule and its neighbour measurement; the automatic report due 2 s on
            }
        } else if (g_engageTries >= kEngageTries) {
            g_engagePending = false;
            Log::get().note("sealed cell: no cell could be built at the named engage (%u promotes; last: %s). Named mode is the sphere rule until the "
                            "next engage; no radius-0 cell is used.", g_engageTries, next.reason);
        }
        if (!g_heldValid) next = sealed::OwnCell{};
    } else if (g_heldValid) {
        ++g_frozenFrames;
        if (sealed::cellDiffers(next, g_heldCell)) ++g_frozenSkipped;
        next = g_heldCell;
    } else {
        next = sealed::OwnCell{};
    }
    // Built v11 (addendum): with a capsule the explicit picks are the parts within kExplicitCapsuleM of its axis; without one, the v8.3b pick.
    if (next.has && next.capsule)
        g_explicitPrevCount = sealed::pickExplicitSegment(g_explicits, g_explicitCount, next.capA, next.capB, g_explicitPrev, sealed::kExplicitNodes);
    else
        g_explicitPrevCount = next.has ? sealed::pickExplicit(g_explicits, g_explicitCount, next.centre, g_explicitPrev, sealed::kExplicitNodes) : 0;
    g_cellPrev = next;
    g_padCount = 0;
    g_explicitCount = 0;
    g_cellStamp = stamp;
    if (next.has) g_lastSize = sealed::padSizeLetter(next.size);
    logCellChange(next, frame);
    logCapsuleChange(next, frame);
    logBoxChange(next, frame);
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
        // Built v11: the centre the report measures from is the named rule's position (worldOfRecord), the same rule the cell test uses.
        const WorldRead wr = worldOfRecord(w.rec);
        const float* centre = wr.world;
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
        float radius = wr.radius;
        uint64_t mask = 0;
        uint64_t children = 0;
        uintptr_t vt = 0;
        const bool readOk = wr.source != 4 && readVal(&mask, w.rec + kRecMask) && readVal(&children, w.rec + kRecChildCount);
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
                // Built v11: the cell test is the capsule's (the distance to the axis) when a capsule is in force, else the pad's sphere.
                const bool inCell = g_cellPrev.box                 ? sealed::inBox(c, g_cellPrev)
                                    : g_cellPrev.capsule           ? sealed::distToSegment(c, g_cellPrev.capA, g_cellPrev.capB) <= g_cellPrev.radius
                                                                   : sealed::distance(c, g_cellPrev.centre) <= g_cellPrev.radius;
                if (inCell) {
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
    // Built v11 (addendum): with a capsule in force, every readable entry's distance to the axis and its position along it, from the pad.
    if (g_cellPrev.capsule) {
        float u[3] = {g_cellPrev.capB[0] - g_cellPrev.capA[0], g_cellPrev.capB[1] - g_cellPrev.capA[1], g_cellPrev.capB[2] - g_cellPrev.capA[2]};
        const float ul = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
        if (ul > 0.0f)
            for (float& v : u) v /= ul;
        for (kept::Entry& e : entries) {
            if (!e.readable) continue;
            const float c[3] = {e.rel[0] + cam[0], e.rel[1] + cam[1], e.rel[2] + cam[2]};
            e.axisDist = sealed::distToSegment(c, g_cellPrev.capA, g_cellPrev.capB);
            e.axisAlong = (c[0] - g_cellPrev.capA[0]) * u[0] + (c[1] - g_cellPrev.capA[1]) * u[1] + (c[2] - g_cellPrev.capA[2]) * u[2];
            if (g_cellPrev.box) {   // Built v11 (second addendum): the record's place in the box
                float o[3] = {};
                sealed::boxCoords(c, g_cellPrev, o);
                e.boxU = o[0];
                e.boxV = o[1];
                e.boxUp = o[2];
            }
        }
    }
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
        // Built v11 (addendum): every kept named record, nearest the capsule's axis first (no capsule: by address), so a neighbouring bay shows up
        // by name and distance. Capped; the totals line says how many were written.
        if (named) {
            std::vector<uint32_t> keptNamed;
            for (uint32_t i = 0; i < entries.size(); ++i)
                if (entries[i].kept && entries[i].label) keptNamed.push_back(i);
            std::sort(keptNamed.begin(), keptNamed.end(), [&entries](uint32_t a, uint32_t b) {
                const float da = entries[a].axisDist >= 0.0f ? entries[a].axisDist : 1.0e9f;
                const float db = entries[b].axisDist >= 0.0f ? entries[b].axisDist : 1.0e9f;
                if (da != db) return da < db;
                return entries[a].address < entries[b].address;
            });
            const size_t written = keptNamed.size() < kKeptNamedRows ? keptNamed.size() : kKeptNamedRows;
            for (size_t k = 0; k < written; ++k)
                if (kept::formatRow(row, sizeof(row), "named", static_cast<uint32_t>(k + 1), entries[keptNamed[k]]) >= 0)
                    std::fprintf(f, "%s\n", row);
            std::fprintf(f, "# named kept rows: %zu written of %zu (cap %zu)\n", written, keptNamed.size(), kKeptNamedRows);
            // Built v12.1 (flight fix b): the named records CULLED in named mode whose centre lies within 60 m of the axis (the segment from the pad
            // to the target), nearest first, with names and box coordinates: the collar, named if it still goes. Capped.
            std::vector<uint32_t> culledNear;
            for (uint32_t i = 0; i < entries.size(); ++i) {
                const kept::Entry& e = entries[i];
                if (!e.kept && e.readable && e.axisDist >= 0.0f && e.axisDist <= kCulledNearAxisM) culledNear.push_back(i);
            }
            std::sort(culledNear.begin(), culledNear.end(), [&entries](uint32_t a, uint32_t b) {
                if (entries[a].axisDist != entries[b].axisDist) return entries[a].axisDist < entries[b].axisDist;
                return entries[a].address < entries[b].address;
            });
            const size_t culledWritten = culledNear.size() < kCulledNearRows ? culledNear.size() : kCulledNearRows;
            for (size_t k = 0; k < culledWritten; ++k)
                if (kept::formatRow(row, sizeof(row), "culled_near_axis", static_cast<uint32_t>(k + 1), entries[culledNear[k]]) >= 0)
                    std::fprintf(f, "%s\n", row);
            std::fprintf(f, "# culled near the axis (within %.0f m): %zu written of %zu (cap %zu)\n", static_cast<double>(kCulledNearAxisM),
                         culledWritten, culledNear.size(), kCulledNearRows);
        }
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
    // Built v10: producer B's veto mode. veto (the default) skips FUN_144312E00 for a record the rule culls; count forwards every call and counts
    // the decisions (the A/B control).
    const std::string producerText = Config::get().getString("diag.sealed_trial_producer", "");
    sealed::ProducerMode producer = sealed::ProducerMode::kVeto;
    if (!sealed::parseProducerMode(producerText.c_str(), &producer))
        Log::get().note("sealed trial: diag.sealed_trial_producer = '%s' is not veto or count; producer B is vetoed.", producerText.c_str());
    g_producerMode = producer;
    // Built v12: the early rig veto's mode (count is the default; hotkey.sealed_trial_early flips it live).
    const std::string earlyText = Config::get().getString("diag.sealed_trial_early", "");
    if (!sealed::parseProducerMode(earlyText.c_str(), &g_early)) {
        g_early = sealed::ProducerMode::kCount;
        Log::get().note("early rig veto: diag.sealed_trial_early = '%s' is not veto or count; the decisions are counted only.", earlyText.c_str());
    }
    // Built v11 (second addendum): the box's doubled axis (length, the default, or width) and its margin in metres.
    const std::string doubleText = Config::get().getString("diag.sealed_cell_double", "");
    if (doubleText == "width") {
        g_doubleLength = false;
    } else {
        g_doubleLength = true;
        if (!doubleText.empty() && doubleText != "length")
            Log::get().note("sealed box: diag.sealed_cell_double = '%s' is not length or width; the length axis is doubled.", doubleText.c_str());
    }
    const std::string belowText = Config::get().getString("diag.sealed_cell_below_m", "");
    if (!sealed::parseBelowM(belowText.c_str(), sealed::kBelowDefaultM, &g_belowM))
        Log::get().note("sealed box: diag.sealed_cell_below_m = '%s' is not a number of metres from 0 to 200; the distance is %.0f m.",
                        belowText.c_str(), static_cast<double>(sealed::kBelowDefaultM));
    Log::get().note("sealed box: the bottom is %.0f m under the pad's centre along the shaft (diag.sealed_cell_below_m; the bay keeps from the same "
                    "distance). Built v13.3: 85 m, measured from the small hangar's lift doors at -72 m.", static_cast<double>(g_belowM));
    const std::string marginText = Config::get().getString("diag.sealed_cell_margin_m", "");
    if (!sealed::parseMarginM(marginText.c_str(), sealed::kMarginDefaultM, &g_margin))
        Log::get().note("sealed box: diag.sealed_cell_margin_m = '%s' is not a number of metres; the margin is %.0f m.", marginText.c_str(),
                        static_cast<double>(sealed::kMarginDefaultM));
    Log::get().note("sealed box: the doubled axis is %s (diag.sealed_cell_double; a guess); margin %.0f m (diag.sealed_cell_margin_m; PAGEUP and "
                    "PAGEDOWN step it in named mode).", g_doubleLength ? "length" : "width", static_cast<double>(g_margin));
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
        // Built v10: the producer B veto leaves with the trial (its own want; the emit's want is untouched).
        kinematicEvalSetDirectVeto(nullptr);
        kinematicEvalDirectVetoDetach();
        // Built v12 (flight fix b): the frozen cell is released with the trial.
        g_engagePending = false;
        g_heldValid = false;
        // Built v12: the early rig veto leaves with the trial too.
        kinematicEvalSetRigVeto(nullptr);
        kinematicEvalRigVetoDetach();
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
    // Built v12 (flight fix b): the named cell engages with the trial (in named mode it is frozen from the first pad that gives a cell).
    g_heldValid = false;
    g_engageTries = 0;
    g_engagePending = g_mode.load(std::memory_order_relaxed) == sealed::Mode::kNamed;
    // Built v10: producer B's pre-forward veto on the emit bracket (kinematic_eval_hook.cpp). Its line says whether it applies: when the bracket
    // is not installed the veto is not live, and the window's producer_b_* counts stay zero for that reason.
    {
        const char* attach = kinematicEvalDirectVetoAttach();
        const bool live = std::strcmp(attach, "installed") == 0;
        kinematicEvalSetDirectVeto(live ? &sealedTrialProducerVeto : nullptr);
        Log::get().note("sealed trial: producer B veto (FUN_144312E00, pre-forward): %s; mode %s (diag.sealed_trial_producer)%s.",
                        live ? "live" : "NOT live", sealed::producerModeName(g_producerMode),
                        live ? "" : (std::string(": ") + attach).c_str());
    }
    // Built v12: the early rig veto on 0x431AFE0's entry (the decision runs only while the trial is on, in named or all mode).
    {
        const char* rigAttach = kinematicEvalRigVetoAttach();
        const bool rigLive = std::strcmp(rigAttach, "installed") == 0;
        kinematicEvalSetRigVeto(rigLive ? &sealedTrialRigVeto : nullptr);
        Log::get().note("sealed trial: early rig veto (0x431AFE0's entry): %s; diag.sealed_trial_early = %s%s.", rigLive ? "live" : "NOT live",
                        sealed::producerModeName(g_early), rigLive ? "" : (std::string(": ") + rigAttach).c_str());
    }
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
    // Built v12 (flight fix b): a mode change re-engages the named cell (to named) or releases it (from named).
    g_engagePending = next == sealed::Mode::kNamed;
    g_engageTries = 0;
    g_heldValid = false;
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
    // Built v11 (second addendum): PAGEUP and PAGEDOWN step the box's margin (-10, -5, 0, 5, 10, 20, 40 m); the next frame's box uses it.
    bool moved = false;
    const float before = g_margin;
    const float after = sealed::stepMarginRung(before, dir, &moved);
    if (moved) g_margin = after;
    if (moved)
        std::snprintf(line, sizeof(line), "named box: margin %.0f m (was %.0f m; ladder -10, -5, 0, 5, 10, 20, 40 m) at frame %u (trigger %s)",
                      static_cast<double>(after), static_cast<double>(before), frame, trigger);
    else
        std::snprintf(line, sizeof(line), "named box: margin is at the end of its ladder (%.0f m); nothing changed (trigger %s)",
                      static_cast<double>(before), trigger);
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

void refreshRigVerdicts() noexcept;   // Built v12.1: defined with the rig decision (below)

void sealedTrialFrame(uint32_t frame) {
    std::lock_guard<std::recursive_mutex> lock(g_sealedMutex);
    g_frameStamp.fetch_add(1, std::memory_order_relaxed);
    ++g_trialFrames;
    if (g_on.load(std::memory_order_acquire)) refreshRigVerdicts();   // Built v12.1: once per frame, the verdicts for the next frame's rig decisions
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
    // Built v13.1 (a): the automatic kept and culled-near-axis reports. Due 2 s after a named engage; and 10 s after the player goes on foot
    // while the trial is on (once per time on foot). Each writes the same report as hotkey.sealed_trial_report, and logs the bay.
    if (g_autoEngagePending && now >= g_autoEngageDueMs) {
        g_autoEngagePending = false;
        logBay("named engage + 2 s", frame);
        sealedTrialReportRequest("auto: named engage + 2 s", frame);
    }
    if (journalOnFootKnown() && journalOnFoot()) {
        if (g_onFootSinceMs == 0) g_onFootSinceMs = now;
        if (!g_onFootReported && now - g_onFootSinceMs >= 10000) {
            g_onFootReported = true;
            logBay("on foot + 10 s", frame);
            sealedTrialReportRequest("auto: on foot + 10 s", frame);
        }
    } else {
        g_onFootSinceMs = 0;
        g_onFootReported = false;
    }
    if (now - g_windowStartMs < sealed::kWindowMs) return;
    g_windowStartMs = now;
    noteWindow();
}

bool sealedTrialOn() noexcept { return g_on.load(std::memory_order_acquire); }

bool sealedTrialSkipsSunPair() noexcept { return g_on.load(std::memory_order_acquire); }

// Built v10: producer B's pre-forward decision (FUN_144312E00, called inside the store for each rig record with a mask; decomp_4312040.txt
// line 337, decomp_4312E00.txt). The record's world sphere is the store's own (node block: centre at +0x20, radius at +0x2C; pose block at
// rec+0x190; its +0x50 matrix; sealed_trial_core.h storeWorldSphere). The rule is the store observer's: all culls every record, named keeps the
// cell test against the previous frame's cell, sphere keeps a record whose sphere meets the trial sphere. A record whose sphere cannot be read
// is kept (counted). The camera is the frame's, from the store observer: before the first store observer of a frame, the previous frame's.
// True skips the forward (veto mode); count mode only counts the decision.
namespace {
bool producerDecide(uintptr_t rec) noexcept {
    float centre[3] = {};
    float radius = 0.0f;
    const bool ok = storeSphereOf(rec, centre, &radius);
    ++g_prodCalls;
    if (ok) {
        prodSync();
        prodPut(rec, centre, radius);   // Built v11: the named rule reads this record's centre from here (the same world centre)
    }
    if (!ok) {
        ++g_prodUnread;
        ++g_prodKept;
        return false;
    }
    ++g_prodStyle;
    if (!g_frameCamValid) {
        ++g_prodKept;
        return false;
    }
    const float trialR = g_radius.load(std::memory_order_relaxed);
    bool keep = false;
    const sealed::Mode mode = g_mode.load(std::memory_order_relaxed);
    if (mode == sealed::Mode::kAll) {
        keep = false;
    } else if (mode == sealed::Mode::kNamed) {
        keep = sealed::keepNamedCell(centre, radius, g_frameCam, trialR, g_cellPrev, g_explicitPrev, g_explicitPrevCount, rec).keep;
    } else {
        keep = sealed::keepRecord(centre, radius, g_frameCam, trialR);
    }
    // Built v13 (fix 1): the own ship is never vetoed in sphere or named mode (all mode vetoes it).
    if (!keep && mode != sealed::Mode::kAll) {
        char nm[48] = {};
        if (g_ownPrefix[0] != '\0') cachedName(rec, nm, sizeof(nm));
        if (sealed::ownShipExempt(sealed::containsCamera(centre, radius, g_frameCam), radius, nm, g_ownPrefix)) keep = true;
    }
    if (keep) {
        ++g_prodKept;
        return false;
    }
    ++g_prodVetoed;
    return g_producerMode == sealed::ProducerMode::kVeto;
}
}  // namespace

bool sealedTrialProducerVeto(uintptr_t rec, uintptr_t owner) noexcept {
    (void)owner;
    if (!g_on.load(std::memory_order_acquire) || !rec) return false;
    std::lock_guard<std::recursive_mutex> lock(g_sealedMutex);
    return producerDecide(rec);
}

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

// --- Built v12: the rig table. A rig (the object rigEvalObserved is called with) is a record's +0x2B8 (decomp_4312040.txt: the store reads the kind
// through rec+0x2B8; decomp_4312E00.txt reads rig+0x170 through it). Its bound is the box of its records' world spheres. The box of the last frame
// that had records is kept, so a vetoed rig (whose records the store no longer visits) keeps its bound. Under the trial's lock.
namespace {
constexpr size_t kRigSlots = 4096;
struct RigSlot {
    uintptr_t rig = 0;               // 0: an empty slot
    uint32_t evalStamp = 0;          // the frame stamp of the rig's last decision
    uint32_t curStamp = 0;           // the frame stamp the current box was accumulated in
    bool curValid = false;
    float curMn[3] = {}, curMx[3] = {};
    bool lastValid = false;          // the box of the last frame that had records (the one before curStamp)
    float lastMn[3] = {}, lastMx[3] = {};
    uint32_t vetoedRun = 0;          // vetoes since the rig was last let through
    bool named = false;
    char name[48] = {};              // the name of the rig's first record (for the own-ship prefix)
    uintptr_t coll = 0;              // the rig's collection (rig+0x348), as last read at a decision
    uint32_t collStamp = 0;          // Built v13.1: the frame stamp the collection was last read in (once per rig per frame)
    uint8_t verdict = 0;             // kVerdictUnknown / kVerdictMixed / kVerdictOutside, refreshed once per frame (refreshRigVerdicts)
};
constexpr uint8_t kVerdictUnknown = 0, kVerdictMixed = 1, kVerdictOutside = 2;
RigSlot g_rigSlots[kRigSlots];

// The collection table (Built v12.1): collection -> rig, for the job path's association. Open addressing, written at a decision when a rig's
// collection is read, read per bucket by the store's observer. Under the trial's lock.
constexpr size_t kCollSlots = 1024;
struct CollSlot {
    uintptr_t coll = 0;
    uintptr_t rig = 0;
};
CollSlot g_collSlots[kCollSlots];

size_t collIndex(uintptr_t coll) noexcept {
    return static_cast<size_t>((static_cast<uint64_t>(coll) >> 4) * 0x9E3779B97F4A7C15ull >> 54) & (kCollSlots - 1);
}

void collInsert(uintptr_t coll, uintptr_t rig) noexcept {
    size_t i = collIndex(coll);
    for (size_t n = 0; n < kCollSlots; ++n, i = (i + 1) & (kCollSlots - 1)) {
        CollSlot& s = g_collSlots[i];
        if (s.coll == coll || s.coll == 0) {
            s.coll = coll;
            s.rig = rig;
            return;
        }
    }
}

uintptr_t collFind(uintptr_t coll) noexcept {
    size_t i = collIndex(coll);
    for (size_t n = 0; n < kCollSlots; ++n, i = (i + 1) & (kCollSlots - 1)) {
        const CollSlot& s = g_collSlots[i];
        if (s.coll == coll) return s.rig;
        if (s.coll == 0) return 0;
    }
    return 0;
}

// The job path's one-entry cache per frame: the last store bucket and the rig it resolved to (a bucket's records come in a run).
uintptr_t g_rigBucketCache = 0, g_rigBucketRig = 0;
uint32_t g_rigBucketStamp = 0;
size_t g_rigSlotsUsed = 0;
uint64_t g_rigNamesLogged = 0;

size_t rigIndex(uintptr_t rig) noexcept {
    return static_cast<size_t>((static_cast<uint64_t>(rig) >> 4) * 0x9E3779B97F4A7C15ull >> 52) & (kRigSlots - 1);
}

RigSlot* rigFind(uintptr_t rig, bool insert) noexcept {
    size_t i = rigIndex(rig);
    for (size_t n = 0; n < kRigSlots; ++n, i = (i + 1) & (kRigSlots - 1)) {
        RigSlot& s = g_rigSlots[i];
        if (s.rig == rig) return &s;
        if (s.rig == 0) {
            if (!insert) return nullptr;
            s.rig = rig;
            ++g_rigSlotsUsed;
            return &s;
        }
    }
    return nullptr;
}

// The store's observer, for each record with a world sphere: the record's rig gets the record's box (this frame) and, the first time, the record's
// name is logged (eight names a session at most, so the identity can be checked by eye).
void rigNoteRecord(uintptr_t bucket, uintptr_t record, const float c[3], float r) noexcept {
    if (!(r > 0.0f) || !std::isfinite(r)) return;
    const uint32_t stamp = g_frameStamp.load(std::memory_order_relaxed);
    // The association (Built v12.1, flight fix c; the record's +0x2B8 is NOT used: the v12 flight showed it is not the evaluated object). Synchronous
    // path: the rig evaluation's forward is on this thread's stack, so the record belongs to that rig. Job path: the store's bucket is the batch
    // entry copied from the Submit's entry (decomp_431AFE0 lines 106 to 117: [0] ctx, [2] = collection+0x300, [5] record array), so the rig is the
    // one whose collection is bucket[2] - 0x300. One cache entry per bucket per frame; the lookup is per bucket, not per record.
    uintptr_t rig = kinematicEvalCurrentRig();
    if (rig) {
        ++g_rigTally.assocSync;
    } else {
        if (g_rigBucketStamp != stamp) {
            g_rigBucketStamp = stamp;
            g_rigBucketCache = 0;
            g_rigBucketRig = 0;
        }
        if (bucket && bucket == g_rigBucketCache) {
            rig = g_rigBucketRig;
        } else {
            uintptr_t owner = 0;
            rig = 0;
            if (bucket && readVal(&owner, bucket + 0x10) && owner > 0x300) rig = collFind(owner - 0x300);
            g_rigBucketCache = bucket;
            g_rigBucketRig = rig;
        }
        if (rig) ++g_rigTally.assocJob;
        else ++g_rigTally.assocNone;
    }
    if (!rig) return;
    RigSlot* s = rigFind(rig, true);
    if (!s) return;
    if (s->evalStamp == stamp) ++g_rigTally.matched;
    else ++g_rigTally.unevaluated;
    if (s->curStamp != stamp) {
        if (s->curValid) {
            for (int k = 0; k < 3; ++k) {
                s->lastMn[k] = s->curMn[k];
                s->lastMx[k] = s->curMx[k];
            }
            s->lastValid = true;
        }
        s->curStamp = stamp;
        s->curValid = false;
    }
    for (int k = 0; k < 3; ++k) {
        const float lo = c[k] - r, hi = c[k] + r;
        if (!s->curValid) {
            s->curMn[k] = lo;
            s->curMx[k] = hi;
        } else {
            if (lo < s->curMn[k]) s->curMn[k] = lo;
            if (hi > s->curMx[k]) s->curMx[k] = hi;
        }
    }
    s->curValid = true;
    if (!s->named) {
        s->named = true;
        char name[64] = {};
        if (!cachedName(record, name, sizeof(name))) std::snprintf(name, sizeof(name), "(none)");
        std::snprintf(s->name, sizeof(s->name), "%s", name);   // Built v13: the own-ship prefix test reads it
        if (g_rigNamesLogged < 8) {
            ++g_rigNamesLogged;
            Log::get().note("early rig veto: rig 0x%llX first seen with record 0x%llX (%s) at frame stamp %u.",
                            static_cast<unsigned long long>(rig), static_cast<unsigned long long>(record), name, stamp);
        }
    }
}
}  // namespace

// Built v12: the early rig veto's decision (kinematic_eval_hook.cpp rigEvalObserved calls it before the forward, only while the veto is attached).
// Only in named or all mode with the trial on. The bound is last frame's records (this frame's, if they were already noted, is the frame before).
// True vetoes; count mode counts the decision and forwards.
// Built v13.1 (d): the decision's cache: the last rig's slot and the frame it was found in (one probe per rig per frame, not per decision). No
// timer is read on the decision path (the frame tick times the refresh; the decisions' own time is not measured in this build).
uintptr_t g_rigCacheRig = 0;
RigSlot* g_rigCacheSlot = nullptr;
uint32_t g_rigCacheStamp = 0;

bool sealedTrialRigVeto(uintptr_t rig, uintptr_t ctx) noexcept {
    (void)ctx;
    if (!g_on.load(std::memory_order_acquire) || !rig) return false;
    const sealed::Mode mode = g_mode.load(std::memory_order_relaxed);
    if (mode == sealed::Mode::kSphere) return false;
    std::lock_guard<std::recursive_mutex> lock(g_sealedMutex);
    const uint32_t stamp = g_frameStamp.load(std::memory_order_relaxed);
    ++g_rigTally.seen;
    RigSlot* s = nullptr;
    if (rig == g_rigCacheRig && g_rigCacheStamp == stamp && g_rigCacheSlot && g_rigCacheSlot->rig == rig) {
        s = g_rigCacheSlot;
    } else {
        s = rigFind(rig, true);
        if (!s) {
            ++g_rigTally.unknown;
            return false;
        }
        g_rigCacheRig = rig;
        g_rigCacheStamp = stamp;
        g_rigCacheSlot = s;
    }
    s->evalStamp = stamp;
    // The rig's collection (rig+0x348, read with SEH), once per rig per frame: recorded so the store's job path can associate its batch entries
    // with this rig.
    if (s->collStamp != stamp) {
        s->collStamp = stamp;
        uintptr_t coll = 0;
        if (readVal(&coll, rig + 0x348) && coll && coll != s->coll) {
            s->coll = coll;
            collInsert(coll, rig);
        }
    }
    // One lookup: the verdict was computed once this frame (refreshRigVerdicts, on the frame tick). No record is scanned here.
    bool veto = false;
    switch (s->verdict) {
        case kVerdictOutside:
            if (s->vetoedRun >= sealed::kRigRefreshN) {
                ++g_rigTally.refresh;
                s->vetoedRun = 0;
            } else {
                ++g_rigTally.vetoed;
                ++s->vetoedRun;
                veto = g_early == sealed::ProducerMode::kVeto;
            }
            break;
        case kVerdictMixed:
            ++g_rigTally.mixed;
            s->vetoedRun = 0;
            break;
        default:
            ++g_rigTally.unknown;
            s->vetoedRun = 0;
            break;
    }
    return veto;
}

// Built v12.1 (flight fix d): the per-frame refresh of every rig's verdict, on the frame tick (render thread), from its bound: the box of its
// records' spheres in the last frame that had records. The cell is the one in force. The decision then reads one slot per rig.
void refreshRigVerdicts() noexcept {
    LARGE_INTEGER t0{}, t1{};
    QueryPerformanceCounter(&t0);
    std::lock_guard<std::recursive_mutex> lock(g_sealedMutex);
    const bool allMode = g_mode.load(std::memory_order_relaxed) == sealed::Mode::kAll;
    for (RigSlot& s : g_rigSlots) {
        if (s.rig == 0) continue;
        const float* mn = nullptr;
        const float* mx = nullptr;
        if (s.curValid) {
            mn = s.curMn;
            mx = s.curMx;
        } else if (s.lastValid) {
            mn = s.lastMn;
            mx = s.lastMx;
        }
        const bool bounded = mn != nullptr;
        float c[3] = {0.0f, 0.0f, 0.0f};
        float r = 0.0f;
        if (bounded) {
            float d2 = 0.0f;
            for (int k = 0; k < 3; ++k) {
                c[k] = 0.5f * (mn[k] + mx[k]);
                const float h = 0.5f * (mx[k] - mn[k]);
                d2 += h * h;
            }
            r = std::sqrt(d2);
        }
        const sealed::RigVerdict v = sealed::rigVerdict(bounded, c, r, g_cellPrev.has, g_cellPrev, allMode, 0);
        // Built v13 (fix 1): the own ship's rig is not vetoed in sphere or named mode: its bound holds the camera with a small radius, or its first
        // record's name has the own-ship prefix. (All mode vetoes it.)
        bool ownRig = false;
        if (!allMode && bounded && g_frameCamValid) ownRig = sealed::containsCamera(c, r, g_frameCam) && r < sealed::kOwnShipRadiusM;
        if (!allMode && !ownRig && g_ownPrefix[0] != '\0') ownRig = sealed::ownShipExempt(false, 0.0f, s.name, g_ownPrefix);
        s.verdict = ownRig ? kVerdictUnknown : sealed::verdictCode(v);   // 2 outside, 1 mixed, 0 unknown
    }
    QueryPerformanceCounter(&t1);
    g_rigTally.ns += static_cast<uint64_t>(nanoseconds(t0, t1));
}

// hotkey.sealed_trial_early: flips the early rig veto between count and veto, live, and logs it. Works with the trial off too (the decision runs
// only while the trial is on).
void sealedTrialEarlyToggle(const char* trigger, uint32_t frame) {
    std::lock_guard<std::recursive_mutex> lock(g_sealedMutex);
    g_early = g_early == sealed::ProducerMode::kVeto ? sealed::ProducerMode::kCount : sealed::ProducerMode::kVeto;
    Log::get().note("early rig veto: diag.sealed_trial_early = %s (live, frame %u, trigger %s): %s", sealed::producerModeName(g_early), frame,
                    trigger, g_early == sealed::ProducerMode::kVeto
                                 ? "rigs whose bound lies wholly outside the named cell are not forwarded (all mode: every bounded rig)"
                                 : "the decisions are counted; every rig is forwarded");
}

// The drawlist probe's take: the rig decisions since its last take (seen, vetoed, mixed, unknown, refresh, matched, unevaluated, early ms).
void sealedTrialRigTake(uint64_t out[8]) noexcept {
    std::lock_guard<std::recursive_mutex> lock(g_sealedMutex);
    out[0] = g_rigTally.seen - g_rigTakeLast.seen;
    out[1] = g_rigTally.vetoed - g_rigTakeLast.vetoed;
    out[2] = g_rigTally.mixed - g_rigTakeLast.mixed;
    out[3] = g_rigTally.unknown - g_rigTakeLast.unknown;
    out[4] = g_rigTally.refresh - g_rigTakeLast.refresh;
    out[5] = g_rigTally.matched - g_rigTakeLast.matched;
    out[6] = g_rigTally.unevaluated - g_rigTakeLast.unevaluated;
    out[7] = g_rigTally.ns - g_rigTakeLast.ns;
    g_rigTakeLast = g_rigTally;
}

// The fault-safe cull of one record, with its counters (called under the lock). containing: the record's sphere contains the camera (named mode).
// True when the record was culled (the caller tallies its name then).
static bool cullCounted(uintptr_t record, bool containing) noexcept {
    RecordFields fields{record};
    sealed::CullField failed = sealed::CullField::kMaskRead;
    uint64_t value = 0;
    switch (sealed::cullRecordFaultSafe(fields, &failed, &value)) {
        case sealed::Cull::kCulled:
            g_culled.fetch_add(1, std::memory_order_relaxed);
            if (containing) g_culledContaining.fetch_add(1, std::memory_order_relaxed);
            return true;
        case sealed::Cull::kUntouched:
        case sealed::Cull::kRestored:
            g_faults.fetch_add(1, std::memory_order_relaxed);   // the record is as the store wrote it: kept, not judged
            return false;
        case sealed::Cull::kTorn:
            g_faults.fetch_add(1, std::memory_order_relaxed);
            g_torn.fetch_add(1, std::memory_order_relaxed);     // the one case the record may be left half culled
            return false;
    }
    return false;
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
    if (g_cellStamp != stamp) {
        promoteFrame(stamp, stamp);
        noteCameraFrame();   // Built v13.1 (c): the camera's origin once per frame (the on-foot motion check)
    }
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
    // Built v11: the named rule's position is the world centre of worldOfRecord (the producer B cache, the pose, the store-style sphere, raw).
    const WorldRead wr = worldOfRecord(record);
    ++g_worldSrc[wr.source];
    if (wr.source != 4) rigNoteRecord(bucket, record, wr.world, wr.radius);   // Built v12: the record's rig gets its box (v12.1: by association)
    const float* centre = wr.world;
    const float radius = wr.radius;
    if (wr.source == 4 || !std::isfinite(radius) || !(radius > 0.0f) ||
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
                char xn[48] = {};
                if (cachedName(record, xn, sizeof(xn))) std::snprintf(x.name, sizeof(x.name), "%s", xn);
                else x.name[0] = '\0';
            }
            const sealed::NamedKeep v = sealed::keepNamedCell(centre, radius, cam, trialR, g_cellPrev, g_explicitPrev, g_explicitPrevCount,
                                                              record);
            keep = v.keep;
            why = v.why;
            containing = v.containing;
            sphereJudged = v.sphereJudged;
            // Built v13.1 (b): the own hangar's parts stay inside the bay volume, even where the box alone would cull them (whatever their size,
            // camera-containing ones included). The shaft is the held cell's; without a capsule there is no bay.
            // Built v13.2 (a): the minima along the shaft of the own ship's records and the Door* records within the bay's lateral distance.
            if (g_cellPrev.capsule) {
                float u[3] = {};
                float a = 0.0f;
                if (sealed::unitBetween(g_cellPrev.capA, g_cellPrev.capB, u)) {
                    const float lat = sealed::lateralToAxis(centre, g_cellPrev.capA, u, &a);
                    if (std::isfinite(lat) && lat <= sealed::kBayLateralM) {
                        char mn[48] = {};
                        if (cachedName(record, mn, sizeof(mn))) {
                            const bool ownNamed = (g_ownPrefix[0] != '\0' && std::strncmp(mn, g_ownPrefix, std::strlen(g_ownPrefix)) == 0) ||
                                                  (g_ownRecName[0] != '\0' && std::strstr(mn, g_ownRecName) != nullptr);
                            const bool door = std::strstr(mn, "Door") != nullptr;
                            if (ownNamed && (!g_minOwnSet || a < g_minOwnAlong)) {
                                g_minOwnSet = true;
                                g_minOwnAlong = a;
                                g_minOwnLat = lat;
                                std::snprintf(g_minOwnName, sizeof(g_minOwnName), "%s", mn);
                            }
                            if (door && (!g_minDoorSet || a < g_minDoorAlong)) {
                                g_minDoorSet = true;
                                g_minDoorAlong = a;
                                g_minDoorLat = lat;
                                std::snprintf(g_minDoorName, sizeof(g_minDoorName), "%s", mn);
                            }
                        }
                    }
                }
            }
            if (!keep && g_cellPrev.capsule) {
                float u[3] = {};
                if (sealed::unitBetween(g_cellPrev.capA, g_cellPrev.capB, u) &&
                    sealed::inBayVolume(centre, g_cellPrev.capA, u, g_cellPrev.capLen, g_belowM)) {
                    char bn[48] = {};
                    if (cachedName(record, bn, sizeof(bn)) && sealed::bayPartName(bn)) {
                        keep = true;   // why stays as the box rule's (kNone): the bay is counted in bay_kept, not in kept_by_cell
                        ++g_bayKept;
                        ++g_bayKeptEngage;
                        if (g_bayNameCount < kBayNameSlots) std::snprintf(g_bayNames[g_bayNameCount++], sizeof(g_bayNames[0]), "%s", bn);
                    }
                }
            }
        } else {
            keep = sealed::keepRecord(centre, radius, cam, trialR);
            why = keep ? sealed::KeepWhy::kSphere : sealed::KeepWhy::kNone;
        }
        // Built v13 (fix 1): the own ship is never culled in sphere or named mode (all mode culls it). A small sphere holding the camera is the own
        // ship's candidate (the nearest one per frame names it); the type prefix taken at engage keeps the records of that type.
        {
            const bool containsCam = sealed::containsCamera(centre, radius, cam);
            // Built v13.2 (b): the near list (the records within kOwnNearM of the camera, with names and spheres), the previous frame's source for
            // the engage's own-record pick.
            {
                const float dCam = sealed::distance(centre, cam);
                if (dCam <= kOwnNearM && g_nearCurN < kNearSlots) {
                    char nn[48] = {};
                    if (cachedName(record, nn, sizeof(nn))) {
                        NearRec& nr = g_nearCur[g_nearCurN++];
                        nr.addr = record;
                        std::snprintf(nr.name, sizeof(nr.name), "%s", nn);
                        for (int k = 0; k < 3; ++k) nr.centre[k] = centre[k];
                        nr.radius = radius;
                        nr.dist = dCam;
                    }
                }
            }
            if (containsCam && radius < sealed::kOwnShipRadiusM) {
                const float dCam = sealed::distance(centre, cam);
                if (g_ownCandidateDist < 0.0f || dCam < g_ownCandidateDist) {
                    char cn[48] = {};
                    if (cachedName(record, cn, sizeof(cn))) {
                        std::snprintf(g_ownCandidateName, sizeof(g_ownCandidateName), "%s", cn);
                        g_ownCandidateDist = dCam;
                    }
                }
            }
            if (!keep && g_mode.load(std::memory_order_relaxed) != sealed::Mode::kAll) {
                char nm[48] = {};
                if (g_ownPrefix[0] != '\0') cachedName(record, nm, sizeof(nm));
                if (sealed::ownShipExempt(containsCam, radius, nm, g_ownPrefix)) {
                    keep = true;
                    ++g_ownExempt;
                }
                // Built v13.2 (b): the own record's live sphere, re-read this frame: any record whose centre lies within it plus 5 m is kept.
                if (!keep && g_ownLiveValid && sealed::ownSphereKeeps(centre, g_ownLiveCentre, g_ownLiveRadius, kOwnSphereMarginM)) {
                    keep = true;
                    ++g_ownSphereKeptFrame;
                    ++g_ownSphereKeptWin;
                }
            }
        }
        if (keep) {
            g_kept.fetch_add(1, std::memory_order_relaxed);
            if (why == sealed::KeepWhy::kCell && !sphereJudged) g_keptByCell.fetch_add(1, std::memory_order_relaxed);
            if (why == sealed::KeepWhy::kExplicit) g_keptExplicit.fetch_add(1, std::memory_order_relaxed);
        } else {
            const bool culled = cullCounted(record, named && g_cellPrev.has && containing);
            // Built v13.1 (a): the culled names, for the window's top three: those containing the camera, and those just outside the held box.
            if (culled && named && g_cellPrev.has) {
                char tn[48] = {};
                // Coordinates: the box's own axes when the cell is a box; else the offset from the pad's centre.
                float x[3] = {centre[0] - g_cellPrev.centre[0], centre[1] - g_cellPrev.centre[1], centre[2] - g_cellPrev.centre[2]};
                uint64_t nch = 0;   // Built v13.2: the record's child count (with its radius, the HangerLarge/Floor00 question)
                readVal(&nch, record + kRecChildCount);
                if (containing) {
                    if (cachedName(record, tn, sizeof(tn))) {
                        if (g_cellPrev.box) sealed::boxCoordsOf(g_cellPrev, centre, x);
                        tallyNote(g_tallyContain, &g_tallyContainUsed, &g_tallyContainOver, tn, x, radius, nch);
                    }
                } else if (g_cellPrev.box && sealed::nearBox(g_cellPrev, centre) && cachedName(record, tn, sizeof(tn))) {
                    sealed::boxCoordsOf(g_cellPrev, centre, x);
                    tallyNote(g_tallyNear, &g_tallyNearUsed, &g_tallyNearOver, tn, x, radius, nch);
                }
            }
        }
    }
    if (g_repPhase.load(std::memory_order_relaxed) == kRepCollecting && g_observed.size() < g_observed.capacity())
        g_observed.push_back(Observed{bucket, record});
    QueryPerformanceCounter(&t1);
    g_evalNs.fetch_add(static_cast<uint64_t>(nanoseconds(t0, t1)), std::memory_order_relaxed);
}

}  // namespace edvr
