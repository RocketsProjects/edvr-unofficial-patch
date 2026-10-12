#pragma once
// The sealed-hangar trial's pure half (sealed_trial.cpp; TEMPORARY, the manual trial of docs/concourse-culling-2026-10-11.md). No Windows, no
// engine memory: the sphere rule (sphere mode), the own-pad cell rule (named mode, Built v8.3b), the radius clamps and ladders, the fault-safe
// cull, and the text of the lines. The rig tools\sealed_trial_test compiles it alone.
//
// Sphere mode (the default), applied in the admission store (FUN_144312040) to each record it writes: keep when the record's sphere CONTAINS the
// camera or INTERSECTS the sphere of radius R round the camera; otherwise clear the record's view mask (rec+0x208) and admitted flag (rec+0x234).
//   Sphere: centre rec+0x240 (world), radius rec+0x280. Camera: the main view's origin, render context + 0x40 + 0x540.
//
// Named mode (Built v8.3b). Each frame's own PAD (a LandingPad* record, size from its name) gives the cell: its centre, and a radius per size
// (diag.sealed_cell_*_m). A record is kept when its sphere CENTRE lies within the cell radius of the cell centre, or when it is the nearest
// instance of an explicit part name (Hanger/Hangar/BlastShield/*HangerSkirt) to the cell centre within 200 m. Everything else is culled, named or
// unnamed, including records whose sphere contains the camera. With no own pad, the frame is the sphere rule.
#include <cctype>
#include <cmath>
#include <cstddef>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace edvr::sealed {

// --- The record's centre (Built v8.4). rec+0x240 is NOT the world centre for many records: the v8.3 flight showed dozens of distinct drones and
// decals at exactly (0,0,0). The builder's own centre is the pose-derived one: pose = *(rec+0x290) (the builder's rcx), q = pose+0x10 (quaternion
// x,y,z,w), t = pose+0x20, c = pose+0x80 (float4), and the centre is the rotation of c by q, plus t (decomp_42B4420.txt:196-237). The radius is
// still rec+0x280 here; the builder's own test (FUN_1404F4E10) is a plane test of a box at this centre with the four floats at pose+0x90 as its
// extents, not a sphere at rec+0x240, and rec+0x280 is not referenced in that block.

// The builder's transformed centre: out = R(q) * c + t (the same expression as decomp_42B4420.txt:196-237). The identity quaternion gives c + t.
inline void builderCentre(const float q[4], const float t[4], const float c[4], float out[4]) noexcept {
    const float w2 = q[3] + q[3];
    const float dot = q[0] * c[0] + q[1] * c[1] + q[2] * c[2];
    const float ww = q[3] * q[3] + q[3] * q[3];
    out[0] = (c[2] * q[1] - c[1] * q[2]) * w2 + ww * c[0] + (-c[0]) + dot * (q[0] + q[0]) + t[0];
    out[1] = (c[0] * q[2] - c[2] * q[0]) * w2 + ww * c[1] + (-c[1]) + dot * (q[1] + q[1]) + t[1];
    out[2] = (c[1] * q[0] - c[0] * q[1]) * w2 + ww * c[2] + (-c[2]) + dot * (q[2] + q[2]) + t[2];
    out[3] = (c[3] * q[3] - c[3] * q[3]) * w2 + ww * c[3] + (-c[3]) + dot * w2 + t[3];
}

// Where a record's centre came from: the pose (the builder's), the raw rec+0x240 (the pose was unreadable: a counted fallback), or none.
enum class CentreSource : uint8_t { kPose = 0, kRaw = 1, kNone = 2 };

inline bool finite3(const float v[3]) noexcept { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }

// The record's centre from what was read: the pose centre when it is readable and finite; else rec+0x240 when that is; else none.
inline CentreSource chooseCentre(bool poseOk, const float pose[3], bool rawOk, const float raw[3], float out[3]) noexcept {
    if (poseOk && pose && finite3(pose)) {
        out[0] = pose[0];
        out[1] = pose[1];
        out[2] = pose[2];
        return CentreSource::kPose;
    }
    if (rawOk && raw && finite3(raw)) {
        out[0] = raw[0];
        out[1] = raw[1];
        out[2] = raw[2];
        return CentreSource::kRaw;
    }
    out[0] = out[1] = out[2] = 0.0f;
    return CentreSource::kNone;
}

// rec+0x240 is exactly the origin: the value the flight showed for dozens of distinct records.
inline bool isZeroCentre(const float c[3]) noexcept { return c[0] == 0.0f && c[1] == 0.0f && c[2] == 0.0f; }

// The p-quantile (nearest rank, p in [0,1]) of the values; 0 when there are none. The vector is taken by value (sorted here).
inline float percentileOf(std::vector<float> v, double p) noexcept {
    if (v.empty()) return 0.0f;
    std::sort(v.begin(), v.end());
    double rank = std::ceil(p * static_cast<double>(v.size()));
    if (rank < 1.0) rank = 1.0;
    if (rank > static_cast<double>(v.size())) rank = static_cast<double>(v.size());
    return v[static_cast<size_t>(rank) - 1];
}

constexpr float kDefaultRadiusM = 80.0f;     // diag.sealed_trial_radius_m
constexpr float kMinRadiusM = 1.0f;
constexpr float kMaxRadiusM = 5000.0f;
constexpr uint64_t kWindowMs = 5000;         // the statistics line's period while the trial is on (every 5 s, so a radius step has its own line)

// The radius ladder, largest first. hotkey.sealed_trial_smaller and hotkey.sealed_trial_larger walk it one rung at a time (sphere radius).
constexpr uint32_t kStepCount = 7;
constexpr float kSteps[kStepCount] = {80.0f, 60.0f, 40.0f, 30.0f, 20.0f, 15.0f, 10.0f};

// Named mode's cell (Built v8.3b). The radii are GUESSES (60 / 90 / 130 m for small, medium, large) until measured on the flight. The ladder
// is what hotkey.sealed_cell_larger and hotkey.sealed_cell_smaller step the CURRENT size's radius through, ascending.
constexpr float kCellDefaultM[3] = {60.0f, 90.0f, 130.0f};
constexpr float kCellMinM = 10.0f;
constexpr float kCellMaxM = 400.0f;
constexpr size_t kCellLadderCount = 10;
constexpr float kCellLadder[kCellLadderCount] = {30.0f, 40.0f, 50.0f, 60.0f, 70.0f, 80.0f, 100.0f, 120.0f, 150.0f, 200.0f};
constexpr float kPadFallbackM = 150.0f;      // no pad sphere contains the camera: the nearest pad centre within this
constexpr float kExplicitM = 200.0f;         // an explicit part's nearest instance to the cell centre must be within this
constexpr size_t kPadSlots = 32;             // pad records a frame keeps for the pick
constexpr size_t kExplicitSlots = 128;       // explicit-part records a frame keeps for the nearest-instance pick
constexpr size_t kExplicitNodes = 64;        // distinct part nodes the nearest-instance pick tracks

// Parses diag.sealed_trial_radius_m: a number of metres, clamped to [kMinRadiusM, kMaxRadiusM]. Empty or not a number
// gives the default and returns false, so the caller can say so once.
inline bool parseRadius(const char* text, float* out) noexcept {
    if (!text || !*text) {
        *out = kDefaultRadiusM;
        return true;
    }
    char* end = nullptr;
    const double v = std::strtod(text, &end);
    if (end == text || *end != '\0' || !std::isfinite(v)) {
        *out = kDefaultRadiusM;
        return false;
    }
    float r = static_cast<float>(v);
    if (r < kMinRadiusM) r = kMinRadiusM;
    if (r > kMaxRadiusM) r = kMaxRadiusM;
    *out = r;
    return true;
}

// Parses diag.sealed_cell_small_m (and the medium and large keys): a number of metres in [kCellMinM, kCellMaxM]; empty or invalid gives the
// given default and returns false.
inline bool parseCellRadius(const char* text, float defaultM, float* out) noexcept {
    if (!text || !*text) {
        *out = defaultM;
        return true;
    }
    char* end = nullptr;
    const double v = std::strtod(text, &end);
    if (end == text || *end != '\0' || !std::isfinite(v)) {
        *out = defaultM;
        return false;
    }
    float r = static_cast<float>(v);
    if (r < kCellMinM) r = kCellMinM;
    if (r > kCellMaxM) r = kCellMaxM;
    *out = r;
    return true;
}

inline float distance(const float a[3], const float b[3]) noexcept {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// The record's sphere contains the camera.
inline bool containsCamera(const float centre[3], float radius, const float camera[3]) noexcept {
    return distance(centre, camera) <= radius;
}

// The record's sphere intersects the sphere of radius R around the camera.
inline bool intersectsCameraSphere(const float centre[3], float radius, const float camera[3], float R) noexcept {
    return distance(centre, camera) <= radius + R;
}

// The sphere rule: keep a record that contains the camera or intersects the camera's sphere.
inline bool keepRecord(const float centre[3], float radius, const float camera[3], float R) noexcept {
    return containsCamera(centre, radius, camera) || intersectsCameraSphere(centre, radius, camera, R);
}

// The statistics of one window. evalNs is the time spent in the rule (the store's own time is not included).
struct Window {
    uint64_t culled = 0;
    uint64_t kept = 0;
    uint64_t keptUnknown = 0;     // a record whose centre or radius could not be read: kept, not judged (kept_unjudged)
    uint64_t sunSkipped = 0;      // sun pair draws the trial skipped in the window
    uint64_t evalNs = 0;
    uint64_t faults = 0;          // reads or writes that failed (the record is left as it was)
    uint64_t torn = 0;            // a cull whose put-back failed too: the record may be left half culled (see cullRecordFaultSafe)
    // TEMPORARY (builder probe, Built v8.1): builder calls whose record was cleared at the builder's entry (rec+0x234 reads 0).
    uint64_t builderAfterCull = 0;
    // Named mode (Built v8.3b): kept by the centre test while the sphere does not meet the trial sphere (kept_by_cell); kept as an explicit
    // part's nearest instance (kept_explicit); culled because the sphere contains the camera (culled_containing); and the cell in force for the
    // window's latest frame (cellHas false: the frame had no own pad, and padReason says why).
    uint64_t keptByCell = 0;
    uint64_t keptExplicit = 0;
    uint64_t culledContaining = 0;
    bool cellHas = false;
    char cellSize = 'n';          // 'S', 'M', 'L' or 'n' (none)
    float cellRadius = 0.0f;
    char padName[64] = {};
    float padDist = -1.0f;
    char padReason[80] = {};
    // Built v8.4: the centre measurements of the window. zeroCentre: records whose rec+0x240 was exactly the origin; poseFallback: records whose
    // pose could not be read (their centre is rec+0x240, counted); the distance between rec+0x240 and the pose centre over the records where both
    // were readable and rec+0x240 was not zero (distN of them; p50, p95 and max in metres).
    uint64_t zeroCentre = 0;
    uint64_t poseFallback = 0;
    uint64_t distN = 0;
    float distP50 = 0.0f;
    float distP95 = 0.0f;
    float distMax = 0.0f;
    // Built v8.5: the kind of the pointer at rec+0x290 for every record the store visited in the window: null, inside the game image, any other
    // non-null value, or unreadable. (The pose it points at is what the centre reads; decomp_4312040.txt places records from rec+0x190 instead.)
    uint64_t ptrNull = 0;
    uint64_t ptrImage = 0;
    uint64_t ptrOther = 0;
    uint64_t ptrUnread = 0;
};

// The trial's record rule (TEMPORARY, diag.sealed_trial_mode): sphere is the sphere rule alone; named is the own-pad cell rule.
// Built v8.5: all clears the view mask and admitted flag of EVERY record the store visits, with no sphere, name or pose test (the discriminator:
// it separates "the flags do not control the draws" from "our write does not land").
enum class Mode : uint8_t { kSphere = 0, kNamed = 1, kAll = 2 };

// Parses diag.sealed_trial_mode: "sphere", "named" or "all" (empty is sphere). False for any other text: the caller says so and keeps sphere.
inline bool parseMode(const char* text, Mode* out) noexcept {
    if (!text || !*text || std::strcmp(text, "sphere") == 0) {
        *out = Mode::kSphere;
        return true;
    }
    if (std::strcmp(text, "named") == 0) {
        *out = Mode::kNamed;
        return true;
    }
    if (std::strcmp(text, "all") == 0) {
        *out = Mode::kAll;
        return true;
    }
    *out = Mode::kSphere;
    return false;
}

inline const char* modeName(Mode m) noexcept {
    if (m == Mode::kNamed) return "named";
    if (m == Mode::kAll) return "all";
    return "sphere";
}

// The next mode: hotkey.sealed_trial_mode cycles sphere, named, all, sphere.
inline Mode nextMode(Mode m) noexcept {
    if (m == Mode::kSphere) return Mode::kNamed;
    if (m == Mode::kNamed) return Mode::kAll;
    return Mode::kSphere;
}

// TEMPORARY (diag.sealed_trial_builder): count leaves the builder calls alone and counts them; skip is refused (builder_probe.cpp).
enum class BuilderPolicy : uint8_t { kCount = 0, kSkip = 1 };

inline bool parseBuilderPolicy(const char* text, BuilderPolicy* out) noexcept {
    if (!text || !*text || std::strcmp(text, "count") == 0) {
        *out = BuilderPolicy::kCount;
        return true;
    }
    if (std::strcmp(text, "skip") == 0) {
        *out = BuilderPolicy::kSkip;
        return true;
    }
    *out = BuilderPolicy::kCount;
    return false;
}

inline const char* builderPolicyName(BuilderPolicy p) noexcept { return p == BuilderPolicy::kSkip ? "skip" : "count"; }

// Case-insensitive prefix test on ASCII text.
inline bool hasPrefixNoCase(const char* text, const char* word) noexcept {
    for (size_t k = 0; word[k]; ++k) {
        if (!text[k] || std::tolower(static_cast<unsigned char>(text[k])) != std::tolower(static_cast<unsigned char>(word[k]))) return false;
    }
    return true;
}

// Case-insensitive substring test on ASCII text.
inline bool containsNoCase(const char* text, const char* word) noexcept {
    const size_t n = std::strlen(word);
    for (size_t i = 0; text[i]; ++i) {
        size_t k = 0;
        while (k < n && text[i + k] && std::tolower(static_cast<unsigned char>(text[i + k])) ==
                                           std::tolower(static_cast<unsigned char>(word[k])))
            ++k;
        if (k == n) return true;
    }
    return false;
}

// The class of a record's name, as the lock-free cache stores it. kPad: a LandingPad* record (GuideMarkers excluded). kExplicit: a part whose
// centre is judged against the cell by its nearest instance (Hanger, Hangar, BlastShield, SmallHangerSkirt, MediumHangerSkirt, LargeHangerSkirt).
// kOther: any other readable name. kUnnamed: no readable name. kNone is the empty cache slot, never a verdict.
enum class NameClass : uint8_t { kNone = 0, kPad = 1, kExplicit = 2, kOther = 3, kUnnamed = 4 };

inline bool isPadName(const char* name) noexcept {
    return name && hasPrefixNoCase(name, "LandingPad") && !containsNoCase(name, "GuideMarkers");
}

inline bool isExplicitName(const char* name) noexcept {
    if (!name) return false;
    static const char* const kPrefixes[] = {"Hanger", "Hangar", "BlastShield", "SmallHangerSkirt", "MediumHangerSkirt", "LargeHangerSkirt"};
    for (const char* p : kPrefixes)
        if (hasPrefixNoCase(name, p)) return true;
    return false;
}

inline NameClass classifyName(bool readable, const char* name) noexcept {
    if (!readable || !name || !*name) return NameClass::kUnnamed;
    if (isPadName(name)) return NameClass::kPad;
    if (isExplicitName(name)) return NameClass::kExplicit;
    return NameClass::kOther;
}

// The own pad's size, from its name: LandingPadSmall*, LandingPadMedium*, LandingPadLarge* (GuideMarkers and other names: none).
enum class PadSize : uint8_t { kNone = 0, kSmall = 1, kMedium = 2, kLarge = 3 };

inline PadSize padSizeOf(const char* name) noexcept {
    if (!isPadName(name)) return PadSize::kNone;
    const char* rest = name + 10;   // past "LandingPad"
    if (hasPrefixNoCase(rest, "Small")) return PadSize::kSmall;
    if (hasPrefixNoCase(rest, "Medium")) return PadSize::kMedium;
    if (hasPrefixNoCase(rest, "Large")) return PadSize::kLarge;
    return PadSize::kNone;
}

inline char padSizeLetter(PadSize s) noexcept {
    switch (s) {
        case PadSize::kSmall: return 'S';
        case PadSize::kMedium: return 'M';
        case PadSize::kLarge: return 'L';
        default: return 'n';
    }
}

// One pad record of a frame, for the pick.
struct PadObs {
    uintptr_t addr = 0;
    float centre[3] = {};
    float radius = 0.0f;       // the pad's own sphere (rec+0x280)
    PadSize size = PadSize::kNone;
    char name[64] = {};
};

// The own cell of a frame. has false: no own pad this frame (named mode is the sphere rule for it) and reason says why.
struct OwnCell {
    bool has = false;
    PadSize size = PadSize::kNone;
    float centre[3] = {};      // the pad's centre
    float radius = 0.0f;       // the cell radius for the pad's size
    uintptr_t pad = 0;
    float padDist = -1.0f;     // the camera to the pad's centre
    bool padContains = false;  // the pad's own sphere contains the camera (else it was the fallback within kPadFallbackM)
    char padName[64] = {};
    char reason[80] = {};
};

// The index into radii[] (small, medium, large) for a size; -1 for none.
inline int sizeIndex(PadSize s) noexcept { return s == PadSize::kNone ? -1 : static_cast<int>(s) - 1; }

// The own pad: among the frame's pad records with a size, the one whose own sphere contains the camera, nearest centre first; else the nearest
// centre within kPadFallbackM. The cell's radius is radii[size]. With no candidate, has is false and reason says why.
inline void pickOwnPad(const PadObs* obs, size_t n, const float camera[3], const float radii[3], OwnCell* out) noexcept {
    *out = OwnCell{};
    if (n == 0) {
        std::snprintf(out->reason, sizeof(out->reason), "no LandingPad record this frame");
        return;
    }
    int best = -1;
    float bestD = 0.0f;
    bool bestContains = false;
    for (size_t i = 0; i < n; ++i) {
        if (obs[i].size == PadSize::kNone) continue;
        const float d = distance(obs[i].centre, camera);
        const bool contains = d <= obs[i].radius;
        if (!contains) continue;
        if (best < 0 || d < bestD) {
            best = static_cast<int>(i);
            bestD = d;
            bestContains = true;
        }
    }
    if (best < 0) {
        for (size_t i = 0; i < n; ++i) {
            if (obs[i].size == PadSize::kNone) continue;
            const float d = distance(obs[i].centre, camera);
            if (d > kPadFallbackM) continue;
            if (best < 0 || d < bestD) {
                best = static_cast<int>(i);
                bestD = d;
            }
        }
    }
    if (best < 0) {
        std::snprintf(out->reason, sizeof(out->reason), "no pad sphere contains the camera and none is within %.0f m", static_cast<double>(kPadFallbackM));
        return;
    }
    const PadObs& p = obs[best];
    out->has = true;
    out->size = p.size;
    for (int k = 0; k < 3; ++k) out->centre[k] = p.centre[k];
    out->radius = radii[sizeIndex(p.size)];
    out->pad = p.addr;
    out->padDist = bestD;
    out->padContains = bestContains;
    std::snprintf(out->padName, sizeof(out->padName), "%s", p.name);
}

// One explicit-part record of a frame: its node identifies the part name (records of one part share it).
struct ExplicitObs {
    uintptr_t addr = 0;
    uintptr_t node = 0;
    float centre[3] = {};
};

// For each part node, the nearest instance to the cell centre within kExplicitM. Writes their addresses to out (at most outCap); returns the count.
inline size_t pickExplicit(const ExplicitObs* obs, size_t n, const float cellCentre[3], uintptr_t* out, size_t outCap) noexcept {
    struct Best {
        uintptr_t node;
        uintptr_t addr;
        float d;
    };
    Best best[kExplicitNodes];
    size_t nb = 0;
    for (size_t i = 0; i < n; ++i) {
        const float d = distance(obs[i].centre, cellCentre);
        if (d > kExplicitM) continue;
        size_t k = 0;
        while (k < nb && best[k].node != obs[i].node) ++k;
        if (k < nb) {
            if (d < best[k].d) {
                best[k].addr = obs[i].addr;
                best[k].d = d;
            }
        } else if (nb < kExplicitNodes) {
            best[nb++] = Best{obs[i].node, obs[i].addr, d};
        }
    }
    size_t written = 0;
    for (size_t k = 0; k < nb && written < outCap; ++k) out[written++] = best[k].addr;
    return written;
}

// Why a record is kept in named mode.
enum class KeepWhy : uint8_t { kNone = 0, kCell = 1, kExplicit = 2, kSphere = 3 };

inline const char* keepWhyName(KeepWhy w) noexcept {
    switch (w) {
        case KeepWhy::kCell: return "cell";
        case KeepWhy::kExplicit: return "explicit";
        case KeepWhy::kSphere: return "sphere";
        default: return "culled";
    }
}

struct NamedKeep {
    bool keep = false;
    KeepWhy why = KeepWhy::kNone;
    bool containing = false;      // the record's sphere contains the camera
    bool sphereJudged = false;    // the sphere rule alone would keep it (so a cell keep is "by cell" only when this is false)
};

inline bool inAddrSet(const uintptr_t* set, size_t n, uintptr_t addr) noexcept {
    for (size_t i = 0; i < n; ++i)
        if (set[i] == addr) return true;
    return false;
}

// The named rule for one record (Built v8.3b). No own cell: the sphere rule for the frame. With one: kept when the record's centre lies within
// the cell radius of the cell centre (why cell), or when it is an explicit part's nearest instance (why explicit). Otherwise culled, including a
// record whose sphere contains the camera.
inline NamedKeep keepNamedCell(const float centre[3], float radius, const float camera[3], float R, const OwnCell& cell,
                               const uintptr_t* explicitSet, size_t explicitN, uintptr_t addr) noexcept {
    NamedKeep v;
    v.containing = containsCamera(centre, radius, camera);
    v.sphereJudged = keepRecord(centre, radius, camera, R);
    if (!cell.has) {
        v.keep = v.sphereJudged;
        v.why = v.keep ? KeepWhy::kSphere : KeepWhy::kNone;
        return v;
    }
    if (distance(centre, cell.centre) <= cell.radius) {
        v.keep = true;
        v.why = KeepWhy::kCell;
        return v;
    }
    if (inAddrSet(explicitSet, explicitN, addr)) {
        v.keep = true;
        v.why = KeepWhy::kExplicit;
    }
    return v;
}

// One rung of the cell ladder from radius r (dir negative: smaller, positive: larger). Off the ladder, a step lands on the nearest rung that way;
// at the end of the ladder r stays (the clamp). *moved says whether it changed.
inline float stepCellRadius(float r, int dir, bool* moved) noexcept {
    float out = r;
    if (dir < 0) {
        for (size_t i = kCellLadderCount; i-- > 0;) {
            if (kCellLadder[i] < r) {
                out = kCellLadder[i];
                break;
            }
        }
    } else if (dir > 0) {
        for (size_t i = 0; i < kCellLadderCount; ++i) {
            if (kCellLadder[i] > r) {
                out = kCellLadder[i];
                break;
            }
        }
    }
    if (moved) *moved = out != r;
    return out;
}

// "named cell: small radius -> 80.0 m at frame N (trigger T)"; the clamp line names the end of the ladder.
inline int formatCellStep(char* out, size_t cap, char sizeLetter, bool moved, float radiusM, int dir, uint32_t frame, const char* trigger) noexcept {
    const char* word = sizeLetter == 'S' ? "small" : (sizeLetter == 'M' ? "medium" : "large");
    if (moved)
        return std::snprintf(out, cap, "named cell: %s radius -> %.1f m at frame %u (trigger %s)", word, static_cast<double>(radiusM), frame,
                             trigger);
    return std::snprintf(out, cap, "named cell: %s radius stays %.1f m at frame %u: the %s step is %.1f m (trigger %s)", word,
                         static_cast<double>(radiusM), frame, dir < 0 ? "smallest" : "largest",
                         static_cast<double>(dir < 0 ? kCellLadder[0] : kCellLadder[kCellLadderCount - 1]), trigger);
}

// The class cache's slot (lock-free): node>>3 in bits 0..43, the class in bits 44..46 (never 0 for a stored slot), the frame's stamp in bits
// 47..63. A zero word is an empty slot. A node that is not 8-aligned or is out of the 47-bit user range is not cached.
constexpr uint64_t kSlotNodeMask = (uint64_t(1) << 44) - 1;
constexpr uint32_t kSlotStampMask = (1u << 17) - 1;

inline bool slotFits(uintptr_t node) noexcept { return (node & 7u) == 0 && (uint64_t(node) >> 3) <= kSlotNodeMask; }

inline uint64_t packSlot(uintptr_t node, NameClass cls, uint32_t stamp) noexcept {
    return (uint64_t(node) >> 3) | (uint64_t(static_cast<uint8_t>(cls)) << 44) | (uint64_t(stamp & kSlotStampMask) << 47);
}

// True when the slot holds this node's class in this frame; the class is written to *out.
inline bool unpackSlot(uint64_t v, uintptr_t node, uint32_t stamp, NameClass* out) noexcept {
    if (v == 0) return false;
    if ((v & kSlotNodeMask) != (uint64_t(node) >> 3)) return false;
    if (((v >> 47) & kSlotStampMask) != (stamp & kSlotStampMask)) return false;
    const uint32_t c = static_cast<uint32_t>((v >> 44) & 7u);
    if (c == 0) return false;
    *out = static_cast<NameClass>(c);
    return true;
}

// Built v8.5: the kind of a pointer read from a record (rec+0x290): null, inside the game image [imageBase, imageEnd), any other non-null
// value, or unreadable. Pure; the caller supplies the read result and the image bounds.
enum class PtrKind : uint8_t { kNull = 0, kImage = 1, kOther = 2, kUnread = 3 };

inline PtrKind ptrKindOf(bool readOk, uintptr_t value, uintptr_t imageBase, uintptr_t imageEnd) noexcept {
    if (!readOk) return PtrKind::kUnread;
    if (value == 0) return PtrKind::kNull;
    if (value >= imageBase && value < imageEnd) return PtrKind::kImage;
    return PtrKind::kOther;
}

inline const char* ptrKindName(PtrKind k) noexcept {
    switch (k) {
        case PtrKind::kNull: return "null";
        case PtrKind::kImage: return "image";
        case PtrKind::kOther: return "other";
        default: return "unreadable";
    }
}

// "sealed trial: mode = named at frame N (...)" and the sphere equivalent, when hotkey.sealed_trial_mode changes the rule while on.
inline int formatModeLine(char* out, size_t cap, Mode m, uint32_t frame, const char* trigger) noexcept {
    if (m == Mode::kAll)
        return std::snprintf(out, cap, "sealed trial: mode = all at frame %u (trigger %s): EVERY record the store visits loses its view mask and "
                                       "admitted flag, with no sphere, name or pose test (the discriminator; the screen and the census should "
                                       "collapse if the flags control the draws)", frame, trigger);
    if (m == Mode::kNamed)
        return std::snprintf(out, cap, "sealed trial: mode = named at frame %u (trigger %s): the own pad's cell (LandingPad small, medium or large; "
                                       "radius per size, diag.sealed_cell_*_m) keeps records whose centre lies within it, plus the nearest instance of "
                                       "each explicit part within 200 m; everything else is culled, including records whose sphere contains the "
                                       "camera; with no own pad the frame is the sphere rule", frame, trigger);
    return std::snprintf(out, cap, "sealed trial: mode = sphere at frame %u (trigger %s): the sphere rule alone", frame, trigger);
}

// "sealed trial: window 5 s culled=.. kept=.. kept_unjudged=.. sun_pairs_skipped=.. eval_ms=.. faults=.. torn=.. radius_m=.. builder_after_cull=..
// mode=.. size=.. cell_r=.. pad=.. pad_dist=.. pad_reason=.. kept_explicit=.. kept_by_cell=.. culled_containing=.. view source = ..". The radius is the
// one the window ran at (a step closes the window of the old radius first). The view the rule's sphere was centred on is named at the end.
inline int formatWindow(char* out, size_t cap, const Window& w, float radiusM, const char* viewSource, Mode mode = Mode::kSphere) noexcept {
    char cell[200];
    if (w.cellHas)
        std::snprintf(cell, sizeof(cell), "size=%c cell_r=%.1f pad=%s pad_dist=%.1f pad_reason=-", w.cellSize, static_cast<double>(w.cellRadius),
                      w.padName, static_cast<double>(w.padDist));
    else
        std::snprintf(cell, sizeof(cell), "size=none cell_r=0.0 pad=none pad_dist=-1.0 pad_reason=%s", w.padReason);
    return std::snprintf(out, cap,
                         "sealed trial: window %llu s culled=%llu kept=%llu kept_unjudged=%llu sun_pairs_skipped=%llu "
                         "eval_ms=%.3f faults=%llu torn=%llu radius_m=%.1f builder_after_cull=%llu mode=%s %s "
                         "kept_explicit=%llu kept_by_cell=%llu culled_containing=%llu zero_centre=%llu pose_fallback=%llu "
                         "centre_dist_m p50=%.2f p95=%.2f max=%.2f n=%llu ptr290 null=%llu image=%llu other=%llu unread=%llu "
                         "view source = %s",
                         static_cast<unsigned long long>(kWindowMs / 1000), static_cast<unsigned long long>(w.culled),
                         static_cast<unsigned long long>(w.kept), static_cast<unsigned long long>(w.keptUnknown),
                         static_cast<unsigned long long>(w.sunSkipped), static_cast<double>(w.evalNs) / 1.0e6,
                         static_cast<unsigned long long>(w.faults), static_cast<unsigned long long>(w.torn), static_cast<double>(radiusM),
                         static_cast<unsigned long long>(w.builderAfterCull), modeName(mode), cell,
                         static_cast<unsigned long long>(w.keptExplicit), static_cast<unsigned long long>(w.keptByCell),
                         static_cast<unsigned long long>(w.culledContaining), static_cast<unsigned long long>(w.zeroCentre),
                         static_cast<unsigned long long>(w.poseFallback), static_cast<double>(w.distP50),
                         static_cast<double>(w.distP95), static_cast<double>(w.distMax), static_cast<unsigned long long>(w.distN),
                         static_cast<unsigned long long>(w.ptrNull), static_cast<unsigned long long>(w.ptrImage),
                         static_cast<unsigned long long>(w.ptrOther), static_cast<unsigned long long>(w.ptrUnread), viewSource);
}

// "sealed trial: on at frame F (radius R m; camera (x,y,z) or unknown; the sun pair is skipped; records outside the sphere
// lose every view; records containing the camera are kept)" and the matching off line.
inline int formatToggle(char* out, size_t cap, bool on, uint32_t frame, float radiusM, const float camera[3],
                        bool cameraKnown, const char* viewSource) noexcept {
    if (!on) return std::snprintf(out, cap, "sealed trial: off at frame %u", frame);
    if (!cameraKnown)
        return std::snprintf(out, cap, "sealed trial: on at frame %u (radius %.1f m; view source = %s; camera unknown: nothing is "
                                       "culled until the view is read)", frame, static_cast<double>(radiusM), viewSource);
    return std::snprintf(out, cap,
                         "sealed trial: on at frame %u (radius %.1f m; view source = %s; camera (%.1f,%.1f,%.1f); the sun pair is "
                         "skipped; records outside the sphere lose every view; records containing the camera are kept)",
                         frame, static_cast<double>(radiusM), viewSource, static_cast<double>(camera[0]),
                         static_cast<double>(camera[1]), static_cast<double>(camera[2]));
}

// One rung from radius r, in direction dir (negative: smaller, positive: larger). Smaller takes the largest rung below r; larger
// takes the smallest rung above r. With no rung that way (r is at the end of the ladder), r stays: that is the clamp. A radius
// off the ladder steps onto it (50 m: smaller gives 40 m, larger gives 60 m). *moved says whether the radius changed.
inline float stepRadius(float r, int dir, bool* moved) noexcept {
    float out = r;
    if (dir < 0) {
        for (uint32_t i = 0; i < kStepCount; ++i) {          // descending: the first rung below r is the largest one
            if (kSteps[i] < r) {
                out = kSteps[i];
                break;
            }
        }
    } else if (dir > 0) {
        for (uint32_t i = kStepCount; i-- > 0;) {            // ascending: the first rung above r is the smallest one
            if (kSteps[i] > r) {
                out = kSteps[i];
                break;
            }
        }
    }
    if (moved) *moved = out != r;
    return out;
}

// "sealed trial: radius -> R m at frame N" when the radius moved. When it did not, the clamp line names the end of the ladder.
inline int formatStep(char* out, size_t cap, int dir, bool moved, float radiusM, uint32_t frame) noexcept {
    if (moved)
        return std::snprintf(out, cap, "sealed trial: radius -> %.1f m at frame %u", static_cast<double>(radiusM), frame);
    if (dir < 0)
        return std::snprintf(out, cap, "sealed trial: radius stays %.1f m at frame %u: the smallest step is %.1f m",
                             static_cast<double>(radiusM), frame, static_cast<double>(kSteps[kStepCount - 1]));
    return std::snprintf(out, cap, "sealed trial: radius stays %.1f m at frame %u: the largest step is %.1f m",
                         static_cast<double>(radiusM), frame, static_cast<double>(kSteps[0]));
}

// A step key pressed while the trial is off: nothing moves, and the line says so.
inline int formatStepRefused(char* out, size_t cap, float radiusM, uint32_t frame) noexcept {
    return std::snprintf(out, cap,
                         "sealed trial: the radius keys act only while the trial is on (radius %.1f m at frame %u); "
                         "nothing changed", static_cast<double>(radiusM), frame);
}

// The cull of one record, with its fault handling (sealed_trial.cpp; the rig tests it against a fake memory). The record's view mask and its
// admitted flag are read first and written only after both are read: a failed read touches nothing, a failed mask write touches nothing,
// and a failed admitted write puts the mask back, so a fault always leaves the record as the store wrote it (kept). A failed put-back is
// reported as torn: the one case where the record may be left half culled, counted and named.
enum class Cull : uint8_t { kCulled = 0, kUntouched = 1, kRestored = 2, kTorn = 3 };
enum class CullField : uint8_t { kMaskRead = 0, kAdmittedRead = 1, kMaskWrite = 2, kAdmittedWrite = 3, kRestoreWrite = 4 };

template <typename Mem>
inline Cull cullRecordFaultSafe(Mem& m, CullField* failed, uint64_t* value) noexcept {
    uint64_t oldMask = 0;
    uint8_t oldAdmitted = 0;
    if (!m.readMask(&oldMask)) {
        *failed = CullField::kMaskRead;
        *value = 0;
        return Cull::kUntouched;
    }
    if (!m.readAdmitted(&oldAdmitted)) {
        *failed = CullField::kAdmittedRead;
        *value = oldMask;
        return Cull::kUntouched;
    }
    if (!m.writeMask(0)) {
        *failed = CullField::kMaskWrite;
        *value = oldMask;
        return Cull::kUntouched;
    }
    if (!m.writeAdmitted(0)) {
        *failed = CullField::kAdmittedWrite;
        *value = oldAdmitted;
        if (m.writeMask(oldMask)) return Cull::kRestored;
        *failed = CullField::kRestoreWrite;
        *value = oldMask;
        return Cull::kTorn;
    }
    return Cull::kCulled;
}

}  // namespace edvr::sealed
