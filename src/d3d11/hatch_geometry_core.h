#pragma once
// The hatch geometry probe's pure half (hatch_geometry.cpp; TEMPORARY, hotkey.hatch_probe). No Windows, no engine memory.
//
// Built v8.2 and v8.3 followed each record's world centre. Built v13.8 (docs\concourse-culling-2026-10-11.md, "Built v13.8") follows the records in
// the OWN PAD's frame: the pad's centre is the origin and its up axis is axes[2], so a station rotation or an origin rebase moves a record and its pad
// alike and drops out. Each sampled frame the store keeps the records with a geometry name and every pad; the frame tick reads the own pad and each
// record's world centre and axes (worldOfRecord, poseOrientOf), expresses them in the pad's frame, and feeds the tracker (change and settled lines)
// and the lower-cycle watch (the pad's height above the hangar shell; the records that move after the pad settles at the bottom of a lower).
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

#include "sealed_trial_core.h"

namespace edvr::hatchgeo {

constexpr uint32_t kSampleEvery = 6;          // one frame in six is sampled
constexpr size_t kSampleCap = 128;            // records one sampled frame keeps; more are counted as drops
constexpr uint32_t kMissLimit = 30;           // sampled frames a record may be absent before it is reported gone
constexpr size_t kTrackCap = 256;             // records followed at once; a new one past this is counted as refused
constexpr uint32_t kLineCap = 4000;           // "hatch geometry:" record lines per session; past it, lines are counted as suppressed
constexpr uint32_t kPadLineCap = 3000;        // "hatch pad:" lines per session
constexpr size_t kWatchCap = 128;             // records the lower-cycle watch follows
constexpr size_t kSummaryCap = 24;            // records a lower-cycle summary lists
constexpr float kRangeM = 300.0f;             // no shaft: records within this of the camera are followed
constexpr float kLateralM = 80.0f;            // with a shaft: records within this lateral of it are followed
constexpr float kShellM = 150.0f;             // the hangar shell: the nearest Hanger*/Hangar* record within this of the pad
constexpr float kMoveM = 0.05f;               // a centre this far (pad-local) from the last line is a change
constexpr float kAxisDeg = 0.5f;              // axes this far (pad-local, degrees) from the last line are a change
constexpr float kMoveStepM = 0.01f;           // between two samples, a centre this far apart is motion (records and the pad alike)
constexpr float kAxisStepDeg = 0.05f;         // between two samples, axes this far apart are motion
constexpr float kLowerM = 0.5f;               // a pad that settles this far below its cycle's start (relative to the shell) is a lower
constexpr uint64_t kChangeIntervalMs = 500;   // a moving record reports at most one change line per this interval
constexpr uint64_t kSettleMs = 2000;          // still this long: the settle (the pad's, and each record's)
constexpr uint64_t kNoMoveCloseMs = 30000;    // a bottom where no record moved closes its summary this long after the settle
constexpr uint64_t kPadHeartbeatMs = 2000;    // the pad line is also written this often while the pad is still
constexpr size_t kNameLen = 64;
constexpr size_t kNamesInLine = 12;
constexpr uint32_t kNoSample = 0xFFFFFFFFu;
constexpr float kPi = 3.14159265358979f;

// Case-insensitive substring and prefix tests on ASCII text.
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

inline bool startsNoCase(const char* text, const char* word) noexcept {
    for (size_t k = 0; word[k]; ++k) {
        if (!text[k] || std::tolower(static_cast<unsigned char>(text[k])) != std::tolower(static_cast<unsigned char>(word[k]))) return false;
    }
    return true;
}

// The records followed by name (v13.8): a name holding any of these words.
inline bool isGeometryName(const char* name) noexcept {
    if (!name || !*name) return false;
    static const char* const kWords[] = {"BlastShield", "Hatch", "Shutter", "Door", "Collar", "Lid", "Cover", "Gate", "Hanger", "Hangar"};
    for (const char* w : kWords)
        if (containsNoCase(name, w)) return true;
    return false;
}

// What the store keeps: a geometry name, or a pad (the own pad is picked among the pads, and is followed whatever its name).
inline bool keepsName(const char* name) noexcept { return isGeometryName(name) || sealed::isPadName(name); }

// The records a shell distance is measured to: names that begin Hanger or Hangar.
inline bool isHangerName(const char* name) noexcept {
    return name && (startsNoCase(name, "Hanger") || startsNoCase(name, "Hangar"));
}

// The reference the lower-cycle summary reports against: the blast shield's base (the shaft's target, the nearest the pad's up line).
inline bool isBaseName(const char* name) noexcept { return name && startsNoCase(name, "BlastShieldBase"); }

inline float dot3(const float a[3], const float b[3]) noexcept { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline float norm3(const float a[3]) noexcept { return std::sqrt(dot3(a, a)); }
inline float distance3(const float a[3], const float b[3]) noexcept {
    const float d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    return norm3(d);
}

// The angle between two directions, in degrees. atan2 of the cross and dot products is exact for small angles, where acos is not.
inline float angleDeg(const float a[3], const float b[3]) noexcept {
    const float c[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    return std::atan2(norm3(c), dot3(a, b)) * (180.0f / kPi);
}

// The largest of the three axis angles between two frames (axes[k] is the k-th unit axis).
inline float axesDeg(const float a[3][3], const float b[3][3]) noexcept {
    float worst = 0.0f;
    for (int k = 0; k < 3; ++k) {
        const float d = angleDeg(a[k], b[k]);
        if (d > worst) worst = d;
    }
    return worst;
}

// The frame the probe measures in: the own pad's (origin = its centre, axes = its axes; sealed::pointToLocal), or the world when no own pad could
// be read this frame.
struct Frame {
    bool padFrame = false;
    float origin[3] = {};
    float axes[3][3] = {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
};

inline void toFrame(const Frame& f, const float p[3], float out[3]) noexcept {
    if (f.padFrame) {
        sealed::pointToLocal(p, f.origin, f.axes, out);
    } else {
        out[0] = p[0];
        out[1] = p[1];
        out[2] = p[2];
    }
}

inline void dirToFrame(const Frame& f, const float v[3], float out[3]) noexcept {
    if (f.padFrame) {
        sealed::dirToLocal(v, f.axes, out);
    } else {
        out[0] = v[0];
        out[1] = v[1];
        out[2] = v[2];
    }
}

inline void axesToFrame(const Frame& f, const float in[3][3], float out[3][3]) noexcept {
    for (int k = 0; k < 3; ++k) dirToFrame(f, in[k], out[k]);
}

// One record of a sampled frame. The world fields are the store's view (worldOfRecord, poseOrientOf); the frame fields are set by localize().
struct Sample {
    uintptr_t addr = 0;
    char name[kNameLen] = {};
    bool isPad = false;
    float world[3] = {};
    float radius = 0.0f;
    uint8_t centreSrc = 4;            // 0 producer B, 1 pose (builder), 2 store-style, 3 raw, 4 none
    float worldAxes[3][3] = {};
    uint8_t axesSrc = 4;              // 1 builder, 2 store-style, 4 none
    float centre[3] = {};             // in the frame
    float axes[3][3] = {};            // in the frame
    bool axesOk = false;
    float along = 0.0f;               // along the shaft (frame coordinates, from the frame's origin)
    float lateral = 0.0f;             // from the shaft line
};

inline const char* centreSourceName(uint8_t s) noexcept {
    switch (s) {
        case 0: return "producer-B";
        case 1: return "builder";
        case 2: return "store-style";
        case 3: return "raw";
        default: return "unreadable";
    }
}

inline const char* axesSourceName(uint8_t s) noexcept {
    switch (s) {
        case 1: return "builder";
        case 2: return "store-style";
        default: return "unreadable";
    }
}

// Sets s's frame fields: its centre and axes in the frame, and its position along the unit shaft u (through the frame's origin) and across it.
inline void localize(const Frame& f, const float u[3], Sample* s) noexcept {
    toFrame(f, s->world, s->centre);
    s->axesOk = s->axesSrc != 4;
    if (s->axesOk) axesToFrame(f, s->worldAxes, s->axes);
    const float a = dot3(s->centre, u);
    const float across[3] = {s->centre[0] - a * u[0], s->centre[1] - a * u[1], s->centre[2] - a * u[2]};
    s->along = a;
    s->lateral = norm3(across);
}

// The index of the nearest Hanger*/Hangar* sample to the point c (world), within limitM, skipping skipAddr. -1 when none.
inline int nearestHangerIndex(const float c[3], const Sample* s, size_t n, uintptr_t skipAddr, float limitM) noexcept {
    int best = -1;
    float bestD = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        if (s[i].addr == skipAddr || !isHangerName(s[i].name)) continue;
        const float d = distance3(c, s[i].world);
        if (d > limitM) continue;
        if (best < 0 || d < bestD) {
            best = static_cast<int>(i);
            bestD = d;
        }
    }
    return best;
}

// The own pad by the trial's rule (sealed::pickOwnPad over the sample's pads): the pad whose sphere holds the camera, else the nearest within 150 m.
// 0 when no pad qualifies.
inline uintptr_t pickOwnPad(const Sample* s, size_t n, const float camera[3]) {
    std::vector<sealed::PadObs> obs;
    obs.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        if (!s[i].isPad) continue;
        sealed::PadObs p;
        p.addr = s[i].addr;
        for (int k = 0; k < 3; ++k) p.centre[k] = s[i].world[k];
        p.radius = s[i].radius;
        p.size = sealed::padSizeOf(s[i].name);
        std::snprintf(p.name, sizeof(p.name), "%s", s[i].name);
        obs.push_back(p);
    }
    if (obs.empty()) return 0;
    const float radii[3] = {1.0f, 1.0f, 1.0f};   // the cell's radius is not used here
    sealed::OwnCell cell;
    sealed::pickOwnPad(obs.data(), obs.size(), camera, radii, &cell);
    return cell.has ? cell.pad : 0;
}

// "name xK, name xK, ... (+M more names)" for the first-sample line: the records of a sampled frame by name, most first, at most kNamesInLine.
inline int formatNameCounts(char* out, size_t cap, const Sample* s, size_t n) noexcept {
    struct Count {
        const char* name;
        uint32_t count;
    };
    Count counts[kSampleCap];
    size_t kinds = 0;
    for (size_t i = 0; i < n; ++i) {
        bool known = false;
        for (size_t k = 0; k < kinds; ++k)
            if (std::strcmp(counts[k].name, s[i].name) == 0) {
                ++counts[k].count;
                known = true;
                break;
            }
        if (!known && kinds < kSampleCap) counts[kinds++] = Count{s[i].name, 1};
    }
    std::stable_sort(counts, counts + kinds, [](const Count& a, const Count& b) { return a.count > b.count; });
    int at = 0;
    for (size_t k = 0; k < kinds && k < kNamesInLine; ++k) {
        const int m = std::snprintf(out + at, cap > static_cast<size_t>(at) ? cap - static_cast<size_t>(at) : 0, "%s%s x%u",
                                    k ? ", " : "", counts[k].name, counts[k].count);
        if (m < 0) break;
        at += m;
    }
    if (kinds > kNamesInLine) {
        const int m = std::snprintf(out + at, cap > static_cast<size_t>(at) ? cap - static_cast<size_t>(at) : 0, " (+%zu more names)",
                                    kinds - kNamesInLine);
        if (m > 0) at += m;
    }
    if (kinds == 0 && cap > 0) std::snprintf(out, cap, "none");
    return at;
}

// A pose for the change rules: a centre and axes in the frame.
struct Pose {
    float c[3] = {};
    float ax[3][3] = {};
    bool ok = false;   // the axes were read
};

inline Pose poseOf(const Sample& s) noexcept {
    Pose p;
    for (int k = 0; k < 3; ++k) p.c[k] = s.centre[k];
    for (int k = 0; k < 3; ++k)
        for (int j = 0; j < 3; ++j) p.ax[k][j] = s.axes[k][j];
    p.ok = s.axesOk;
    return p;
}

// Moved between two poses: the centre by more than centreM, or (when both axes were read) an axis by more than axisDeg.
inline bool poseMoved(const Pose& a, const Pose& b, float centreM, float axisDeg) noexcept {
    if (distance3(a.c, b.c) > centreM) return true;
    return a.ok && b.ok && axesDeg(a.ax, b.ax) > axisDeg;
}

enum class Kind : uint8_t { kAppear = 0, kChange = 1, kSettled = 2, kDisappear = 3 };

inline const char* kindName(Kind k) noexcept {
    switch (k) {
        case Kind::kAppear: return "appeared";
        case Kind::kChange: return "changed";
        case Kind::kSettled: return "settled";
        default: return "disappeared";
    }
}

// One event of a sampled frame. A disappearance carries the record's last sample (sample == kNoSample).
struct Event {
    Kind kind = Kind::kAppear;
    uint32_t sample = kNoSample;
    Sample s;
    float centreDelta = 0.0f;     // the frame centre moved this far since the record's last line (metres)
    float axesDelta = 0.0f;       // the largest axis rotation since its last line (degrees)
    float totalMove = 0.0f;       // the centre's distance from where the record first appeared (metres)
    uint32_t missed = 0;          // a disappearance: the sampled frames it was absent
};

// Follows the records across sampled frames, in the frame the samples are expressed in. Render thread only (no lock). Time is the caller's clock
// (milliseconds), so the rig can drive it. A change line: the centre more than kMoveM, or an axis more than kAxisDeg, from the record's last line, at
// most one per kChangeIntervalMs while it moves. A settled line: still for kSettleMs after a move, and different from its last line. A record
// absent for kMissLimit sampled frames is reported gone.
class Tracker {
public:
    Tracker() { tracks_.assign(kTrackCap, Track{}); }

    // Forgets every track (a new frame, or a toggle-on): the next sampled frame's records all appear.
    void clear() {
        tracks_.assign(kTrackCap, Track{});
        refused_ = 0;
    }

    uint32_t refused() const noexcept { return refused_; }

    size_t present() const noexcept {
        size_t n = 0;
        for (const Track& t : tracks_)
            if (t.used && t.present) ++n;
        return n;
    }

    // Applies one sampled frame at time nowMs: appends its events in sample order, then the disappearances.
    void apply(const Sample* s, size_t n, uint64_t nowMs, std::vector<Event>* out) {
        for (Track& t : tracks_) t.seen = false;
        for (size_t i = 0; i < n; ++i) {
            const Sample& cur = s[i];
            Track* t = find(cur.addr);
            if (!t) t = admit(cur.addr);
            if (!t) {
                ++refused_;
                continue;
            }
            t->seen = true;
            t->missed = 0;
            const Pose now1 = poseOf(cur);
            if (!t->present) {
                t->present = true;
                for (int k = 0; k < 3; ++k) t->start[k] = cur.centre[k];
                t->lastLineMs = nowMs;
                t->lastMoveMs = nowMs;
                t->pending = false;
                remember(*t, cur, now1);
                out->push_back(makeEvent(Kind::kAppear, static_cast<uint32_t>(i), cur, *t, 0.0f, 0.0f, 0));
                continue;
            }
            const bool movedNow = poseMoved(t->prev, now1, kMoveStepM, kAxisStepDeg);
            const float fromLine = distance3(now1.c, t->line.c);
            const float axFromLine = (now1.ok && t->line.ok) ? axesDeg(now1.ax, t->line.ax) : 0.0f;
            const bool differs = fromLine > kMoveM || axFromLine > kAxisDeg;
            if (movedNow) {
                t->lastMoveMs = nowMs;
                t->pending = true;
            }
            if (movedNow && differs && nowMs - t->lastLineMs >= kChangeIntervalMs) {
                out->push_back(makeEvent(Kind::kChange, static_cast<uint32_t>(i), cur, *t, fromLine, axFromLine, 0));
                t->line = now1;
                t->lastLineMs = nowMs;
            } else if (!movedNow && t->pending && nowMs - t->lastMoveMs >= kSettleMs) {
                t->pending = false;
                if (differs) {
                    out->push_back(makeEvent(Kind::kSettled, static_cast<uint32_t>(i), cur, *t, fromLine, axFromLine, 0));
                    t->line = now1;
                    t->lastLineMs = nowMs;
                }
            }
            t->prev = now1;
            t->last = cur;
        }
        for (Track& t : tracks_) {
            if (!t.used || !t.present || t.seen) continue;
            if (++t.missed < kMissLimit) continue;
            t.present = false;
            out->push_back(makeEvent(Kind::kDisappear, kNoSample, t.last, t, 0.0f, 0.0f, t.missed));
        }
    }

private:
    struct Track {
        uintptr_t addr = 0;
        float start[3] = {};    // where the record first appeared (frame centre)
        Pose prev;              // at the last sample
        Pose line;              // at the last reported line
        Sample last;            // the last sample (its name, and the disappearance's values)
        uint64_t lastLineMs = 0;
        uint64_t lastMoveMs = 0;
        bool pending = false;   // it moved since its last settled line
        bool used = false;
        bool present = false;
        bool seen = false;
        uint32_t missed = 0;
    };

    Track* find(uintptr_t addr) noexcept {
        for (Track& t : tracks_)
            if (t.used && t.addr == addr) return &t;
        return nullptr;
    }

    // A free track, else a track whose record is absent (its slot is reused); null when every track is present.
    Track* admit(uintptr_t addr) noexcept {
        Track* slot = nullptr;
        for (Track& t : tracks_)
            if (!t.used) {
                slot = &t;
                break;
            }
        if (!slot)
            for (Track& t : tracks_)
                if (!t.present) {
                    slot = &t;
                    break;
                }
        if (!slot) return nullptr;
        *slot = Track{};
        slot->used = true;
        slot->addr = addr;
        return slot;
    }

    static void remember(Track& t, const Sample& s, const Pose& p) noexcept {
        t.prev = p;
        t.line = p;
        t.last = s;
    }

    static Event makeEvent(Kind k, uint32_t sample, const Sample& s, const Track& t, float cd, float ad, uint32_t missed) noexcept {
        Event e;
        e.kind = k;
        e.sample = sample;
        e.s = s;
        e.centreDelta = cd;
        e.axesDelta = ad;
        e.totalMove = distance3(s.centre, t.start);
        e.missed = missed;
        return e;
    }

    std::vector<Track> tracks_;
    uint32_t refused_ = 0;
};

// A bounded collection of sampled records, shared between the store's threads and the frame tick. Mutex is any lock (std::mutex in the engine; the
// rig drives the same code with threads). The lock covers one container operation; nothing holds it across a call into the game.
struct SampleStorage {
    std::vector<Sample> items;
    uint64_t drops = 0;
    uintptr_t ctx = 0;   // the render context the store read from a bucket (for the camera)
    void reserve(size_t cap) {
        items.clear();
        items.reserve(cap);
        drops = 0;
        ctx = 0;
    }
};

template <class Mutex>
class SampleBox {
public:
    // Replaces the storage and returns the old one whole. `next` must already be reserved (the allocation is outside the lock).
    SampleStorage exchange(SampleStorage&& next) {
        std::lock_guard<Mutex> lock(mutex_);
        std::swap(storage_, next);
        return std::move(next);
    }

    // A record the store visited in the sampled frame. The same record twice is kept once; past the reserve it is a drop.
    void add(const Sample& s, uintptr_t ctx = 0) {
        std::lock_guard<Mutex> lock(mutex_);
        if (ctx) storage_.ctx = ctx;
        for (const Sample& k : storage_.items)
            if (k.addr == s.addr) return;
        if (storage_.items.size() < storage_.items.capacity()) storage_.items.push_back(s);
        else ++storage_.drops;
    }

private:
    Mutex mutex_;
    SampleStorage storage_;
};

// The lower cycle (v13.8). The pad's height above the hangar shell, along the shaft (relH), is read on each sampled frame. A cycle starts when
// the pad moves; it settles when the pad has been still for kSettleMs. A settle kLowerM or more below the cycle's start is a BOTTOM: from then the
// records of the frame are watched, and the summary (closed when the moved records have been still for kSettleMs, when the pad moves again, when
// the probe turns off, or kNoMoveCloseMs after the settle with no record moved) lists each record that moved after the settle, with its start and
// end pad-local centre and rotation and its times from the settle.
enum class PadPhase : uint8_t { kStill = 0, kMoving = 1, kBottom = 2 };

enum class PadNotice : uint8_t { kNone = 0, kCycleStart = 1, kBottomSettled = 2, kSettledNotLower = 3 };

inline const char* phaseName(PadPhase p) noexcept {
    switch (p) {
        case PadPhase::kMoving: return "moving";
        case PadPhase::kBottom: return "at the bottom";
        default: return "still";
    }
}

struct MovedRecord {
    uintptr_t addr = 0;
    char name[kNameLen] = {};
    float startC[3] = {};     // the pad-local centre before the first move after the settle
    float endC[3] = {};       // at the last sample
    float startAx[3][3] = {};
    float endAx[3][3] = {};
    bool axesOk = false;
    float rotationDeg = 0.0f; // the largest axis rotation from the start to the end
    float startAlong = 0.0f;
    float endAlong = 0.0f;
    float startS = 0.0f;      // seconds from the pad's settle to the record's first move
    float stopS = 0.0f;       // seconds from the pad's settle to its last move
};

struct Summary {
    char reason[64] = {};
    float startH = 0.0f;      // the pad's height above the shell at the cycle's start (metres)
    float settleH = 0.0f;     // at the settle
    float endH = 0.0f;        // at the close
    uint64_t settleMs = 0;
    uint64_t closeMs = 0;
    uint32_t watched = 0;
    uint32_t movedCount = 0;
    bool baseSeen = false;    // a BlastShieldBase* record was watched
    char baseName[kNameLen] = {};
    float baseAlong = 0.0f;   // its along-shaft height at the close (pad-local)
    MovedRecord moved[kSummaryCap] = {};
    size_t movedN = 0;
};

class BottomWatch {
public:
    PadPhase phase() const noexcept { return phase_; }
    // Whether the last update saw the pad move.
    bool padMoving() const noexcept { return stepLast_; }

    // One call per sampled frame. valid says whether relH (the pad's height above the shell) was read this frame; s and n are the frame's kept
    // records (the frame's own frame).
    PadNotice update(uint64_t now, bool valid, float relH, const Sample* s, size_t n) {
        const bool step = valid && haveH_ && std::fabs(relH - lastH_) > kMoveStepM;
        stepLast_ = step;
        PadNotice out = PadNotice::kNone;
        switch (phase_) {
            case PadPhase::kStill:
                if (step) {
                    begin(now);
                    out = PadNotice::kCycleStart;
                }
                break;
            case PadPhase::kMoving:
                if (step) {
                    lastPadMoveMs_ = now;
                } else if (valid && now - lastPadMoveMs_ >= kSettleMs) {
                    if (startH_ - relH >= kLowerM) {
                        phase_ = PadPhase::kBottom;
                        settleMs_ = now;
                        settleH_ = relH;
                        arm(s, n);
                        out = PadNotice::kBottomSettled;
                    } else {
                        phase_ = PadPhase::kStill;
                        out = PadNotice::kSettledNotLower;
                    }
                }
                break;
            case PadPhase::kBottom:
                observe(s, n, now);
                if (step) {
                    close(now, "the pad moved again", relH);
                    begin(now);
                    out = PadNotice::kCycleStart;
                } else if (const char* why = endReason(now)) {
                    close(now, why, relH);
                    phase_ = PadPhase::kStill;
                }
                break;
        }
        if (valid) {
            lastH_ = relH;
            haveH_ = true;
        }
        return out;
    }

    // Closes an open bottom (the frame changed, or the probe turned off), so its summary is ready.
    void reset(uint64_t now, const char* reason) {
        if (phase_ == PadPhase::kBottom) close(now, reason, lastH_);
        phase_ = PadPhase::kStill;
        haveH_ = false;
        stepLast_ = false;
        watched_.clear();
    }

    // The ready summary, once. False when none is ready.
    bool takeSummary(Summary* out) {
        if (!summaryReady_) return false;
        *out = summary_;
        summaryReady_ = false;
        return true;
    }

private:
    struct Watched {
        uintptr_t addr = 0;
        bool isBase = false;
        bool started = false;     // it has moved at all since the settle
        bool moved = false;       // it has moved beyond the change thresholds from where it started
        bool startAxOk = false;
        float startC[3] = {};
        float startAx[3][3] = {};
        float startAlong = 0.0f;
        uint64_t firstMoveMs = 0;
        uint64_t lastMoveMs = 0;
        Sample last;
    };

    void begin(uint64_t now) {
        phase_ = PadPhase::kMoving;
        startH_ = lastH_;
        lastPadMoveMs_ = now;
    }

    void arm(const Sample* s, size_t n) {
        watched_.clear();
        for (size_t i = 0; i < n && watched_.size() < kWatchCap; ++i) {
            if (s[i].isPad) continue;
            Watched w;
            w.addr = s[i].addr;
            w.isBase = isBaseName(s[i].name);
            w.last = s[i];
            watched_.push_back(w);
        }
    }

    void observe(const Sample* s, size_t n, uint64_t now) {
        for (Watched& w : watched_) {
            const Sample* cur = nullptr;
            for (size_t i = 0; i < n; ++i)
                if (s[i].addr == w.addr) {
                    cur = &s[i];
                    break;
                }
            if (!cur) continue;
            const Pose before = poseOf(w.last);
            const Pose after = poseOf(*cur);
            if (poseMoved(before, after, kMoveStepM, kAxisStepDeg)) {
                if (!w.started) {
                    w.started = true;
                    for (int k = 0; k < 3; ++k) w.startC[k] = before.c[k];
                    for (int k = 0; k < 3; ++k)
                        for (int j = 0; j < 3; ++j) w.startAx[k][j] = before.ax[k][j];
                    w.startAxOk = before.ok;
                    w.startAlong = w.last.along;
                    w.firstMoveMs = now;
                }
                w.lastMoveMs = now;
                if (!w.moved) {
                    Pose start;
                    for (int k = 0; k < 3; ++k) start.c[k] = w.startC[k];
                    for (int k = 0; k < 3; ++k)
                        for (int j = 0; j < 3; ++j) start.ax[k][j] = w.startAx[k][j];
                    start.ok = w.startAxOk;
                    w.moved = poseMoved(start, after, kMoveM, kAxisDeg);
                }
            }
            w.last = *cur;
        }
    }

    // The reason the open bottom should close now, or null.
    const char* endReason(uint64_t now) const {
        bool anyMoved = false;
        uint64_t lastMove = 0;
        for (const Watched& w : watched_)
            if (w.moved) {
                anyMoved = true;
                if (w.lastMoveMs > lastMove) lastMove = w.lastMoveMs;
            }
        if (anyMoved && now - lastMove >= kSettleMs) return "the moved records settled";
        if (!anyMoved && now - settleMs_ >= kNoMoveCloseMs) return "no record moved";
        return nullptr;
    }

    void close(uint64_t now, const char* reason, float endH) {
        summary_ = Summary{};
        std::snprintf(summary_.reason, sizeof(summary_.reason), "%s", reason);
        summary_.startH = startH_;
        summary_.settleH = settleH_;
        summary_.endH = endH;
        summary_.settleMs = settleMs_;
        summary_.closeMs = now;
        summary_.watched = static_cast<uint32_t>(watched_.size());
        for (const Watched& w : watched_) {
            if (w.isBase && !summary_.baseSeen) {
                summary_.baseSeen = true;
                std::snprintf(summary_.baseName, sizeof(summary_.baseName), "%s", w.last.name);
                summary_.baseAlong = w.last.along;
            }
            if (!w.moved) continue;
            ++summary_.movedCount;
            if (summary_.movedN >= kSummaryCap) continue;
            MovedRecord& r = summary_.moved[summary_.movedN++];
            r.addr = w.addr;
            std::snprintf(r.name, sizeof(r.name), "%s", w.last.name);
            for (int k = 0; k < 3; ++k) {
                r.startC[k] = w.startC[k];
                r.endC[k] = w.last.centre[k];
            }
            for (int k = 0; k < 3; ++k)
                for (int j = 0; j < 3; ++j) {
                    r.startAx[k][j] = w.startAx[k][j];
                    r.endAx[k][j] = w.last.axes[k][j];
                }
            r.axesOk = w.startAxOk && w.last.axesOk;
            r.rotationDeg = r.axesOk ? axesDeg(r.startAx, r.endAx) : 0.0f;
            r.startAlong = w.startAlong;
            r.endAlong = w.last.along;
            r.startS = static_cast<float>(static_cast<double>(w.firstMoveMs - settleMs_) / 1000.0);
            r.stopS = static_cast<float>(static_cast<double>(w.lastMoveMs - settleMs_) / 1000.0);
        }
        summaryReady_ = true;
        watched_.clear();
    }

    PadPhase phase_ = PadPhase::kStill;
    bool haveH_ = false;
    bool stepLast_ = false;
    float lastH_ = 0.0f;
    float startH_ = 0.0f;
    float settleH_ = 0.0f;
    uint64_t lastPadMoveMs_ = 0;
    uint64_t settleMs_ = 0;
    bool summaryReady_ = false;
    std::vector<Watched> watched_;
    Summary summary_;
};

// What a line adds to a record's event: the camera, the frame, the shaft, and the frame's status words.
struct LineContext {
    bool hasCamera = false;
    float camera[3] = {};
    uint64_t ms = 0;
    bool statusKnown = false;
    uint32_t flags = 0;
    uint32_t flags2 = 0;
    bool padFrame = false;
    char frameName[kNameLen] = {};
    uintptr_t frameAddr = 0;
    uintptr_t ownAddr = 0;
    bool hasShaft = false;
    bool held = false;
};

// The pad's line (every sampled frame while it moves, and every kPadHeartbeatMs while it is still).
struct PadLine {
    uint64_t ms = 0;
    bool padFrame = false;
    char padName[kNameLen] = {};
    uintptr_t padAddr = 0;
    bool shellOk = false;
    char shellName[kNameLen] = {};
    float shellDistM = 0.0f;
    float relH = 0.0f;
    float firstH = 0.0f;
    bool firstSet = false;
    PadPhase phase = PadPhase::kStill;
    bool moving = false;
    bool hasShaft = false;
    bool held = false;
};

// "hatch geometry: <appeared|changed|settled|disappeared> frame F t=T ms: rec 0x.. name=.. [own pad]; frame=<own pad NAME rec 0x..|world>; local
// centre (x,y,z) along A m lateral L m; local axes u=(..) v=(..) w=(..) (src) | unreadable; world centre (x,y,z) (src) camera (x,y,z) dist_cam D m;
// <the change, the settle, or the disappearance>; status flags=.. flags2=.. | flags unknown".
inline int formatEvent(char* out, size_t cap, const Event& e, const LineContext& ctx, uint32_t frame) noexcept {
    const Sample& s = e.s;
    char detail[200] = {};
    switch (e.kind) {
        case Kind::kChange:
            std::snprintf(detail, sizeof(detail), "moved %.3f m since its last line, rotated %.2f deg since its last line, total %.3f m since it "
                          "appeared", static_cast<double>(e.centreDelta), static_cast<double>(e.axesDelta), static_cast<double>(e.totalMove));
            break;
        case Kind::kSettled:
            std::snprintf(detail, sizeof(detail), "settled: moved %.3f m since its last line, rotated %.2f deg, total %.3f m since it appeared",
                          static_cast<double>(e.centreDelta), static_cast<double>(e.axesDelta), static_cast<double>(e.totalMove));
            break;
        case Kind::kDisappear:
            std::snprintf(detail, sizeof(detail), "not seen for %u sampled frames (its last values are shown)", e.missed);
            break;
        default:
            std::snprintf(detail, sizeof(detail), "appeared");
            break;
    }
    char axes[160] = "unreadable";
    if (s.axesOk) {
        std::snprintf(axes, sizeof(axes), "u=(%.3f,%.3f,%.3f) v=(%.3f,%.3f,%.3f) w=(%.3f,%.3f,%.3f) (%s)", static_cast<double>(s.axes[0][0]),
                      static_cast<double>(s.axes[0][1]), static_cast<double>(s.axes[0][2]), static_cast<double>(s.axes[1][0]),
                      static_cast<double>(s.axes[1][1]), static_cast<double>(s.axes[1][2]), static_cast<double>(s.axes[2][0]),
                      static_cast<double>(s.axes[2][1]), static_cast<double>(s.axes[2][2]), axesSourceName(s.axesSrc));
    }
    char frameText[200];
    if (ctx.padFrame)
        std::snprintf(frameText, sizeof(frameText), "frame=own pad %s rec 0x%llX", ctx.frameName, static_cast<unsigned long long>(ctx.frameAddr));
    else
        std::snprintf(frameText, sizeof(frameText), "frame=world (no own pad read)");
    char camera[120] = "camera unknown";
    if (ctx.hasCamera) {
        const float dc = distance3(s.world, ctx.camera);
        std::snprintf(camera, sizeof(camera), "camera (%.2f,%.2f,%.2f) dist_cam %.2f m", static_cast<double>(ctx.camera[0]),
                      static_cast<double>(ctx.camera[1]), static_cast<double>(ctx.camera[2]), static_cast<double>(dc));
    }
    char pose[80] = "unknown";
    if (ctx.hasShaft) std::snprintf(pose, sizeof(pose), "shaft %s", ctx.held ? "held cell" : "named pick");
    else std::snprintf(pose, sizeof(pose), "no shaft (camera %.0f m)", static_cast<double>(kRangeM));
    char status[64];
    if (ctx.statusKnown) std::snprintf(status, sizeof(status), "flags=%u flags2=%u", ctx.flags, ctx.flags2);
    else std::snprintf(status, sizeof(status), "flags unknown");
    return std::snprintf(out, cap,
                         "hatch geometry: %s frame %u t=%llu ms: rec 0x%llX name=%s%s; %s; local centre (%.3f,%.3f,%.3f) along %.3f m "
                         "lateral %.3f m; local axes %s; world centre (%.3f,%.3f,%.3f) from %s; %s; %s; detail: %s; status %s",
                         kindName(e.kind), frame, static_cast<unsigned long long>(ctx.ms), static_cast<unsigned long long>(s.addr), s.name,
                         (ctx.ownAddr && s.addr == ctx.ownAddr) ? " [own pad]" : "", frameText, static_cast<double>(s.centre[0]),
                         static_cast<double>(s.centre[1]), static_cast<double>(s.centre[2]), static_cast<double>(s.along),
                         static_cast<double>(s.lateral), axes, static_cast<double>(s.world[0]), static_cast<double>(s.world[1]),
                         static_cast<double>(s.world[2]), centreSourceName(s.centreSrc), pose, camera, detail, status);
}

// "hatch pad: t=T ms: frame ..: own pad NAME rec 0x.. (or none); shell NAME at D m; height above the shell H m (change since the first reading C m);
// phase P; moving|still; shaft ..".
inline int formatPadLine(char* out, size_t cap, const PadLine& p, uint32_t frame) noexcept {
    char pad[160] = "no own pad frame (the records are followed in the world)";
    if (p.padFrame) std::snprintf(pad, sizeof(pad), "own pad %s rec 0x%llX", p.padName, static_cast<unsigned long long>(p.padAddr));
    char shell[160] = "no hangar shell within range";
    if (p.shellOk) std::snprintf(shell, sizeof(shell), "shell %s at %.2f m", p.shellName, static_cast<double>(p.shellDistM));
    char height[120] = "height above the shell unknown";
    if (p.shellOk && p.firstSet)
        std::snprintf(height, sizeof(height), "height above the shell %.3f m (change since the first reading %+.3f m)",
                      static_cast<double>(p.relH), static_cast<double>(p.relH - p.firstH));
    return std::snprintf(out, cap, "hatch pad: frame %u t=%llu ms: %s; %s; %s; phase %s (%s); shaft %s", frame,
                         static_cast<unsigned long long>(p.ms), pad, shell, height, phaseName(p.phase), p.moving ? "moving" : "still",
                         p.hasShaft ? (p.held ? "held cell" : "named pick") : "none");
}

// The lower-cycle summary's header line.
inline int formatSummary(char* out, size_t cap, const Summary& m, uint32_t frame) noexcept {
    return std::snprintf(out, cap, "hatch geometry: lower cycle summary (frame %u): the pad settled at the bottom at t=%llu ms; its height above the "
                         "shell was %.3f m at the cycle's start, %.3f m at the settle and %.3f m at the close (a fall of %.3f m); %u records "
                         "watched, %u moved after the settle; closed: %s. Times are seconds after the settle; heights are along the shaft in the "
                         "pad's frame, relative to the shell.", frame, static_cast<unsigned long long>(m.settleMs),
                         static_cast<double>(m.startH), static_cast<double>(m.settleH), static_cast<double>(m.endH),
                         static_cast<double>(m.startH - m.settleH), m.watched, m.movedCount, m.reason);
}

// One record that moved after the settle.
inline int formatMoved(char* out, size_t cap, const MovedRecord& r, const Summary& m, uint32_t frame) noexcept {
    char base[160] = "";
    if (m.baseSeen)
        std::snprintf(base, sizeof(base), " (%s at along %.3f m: the end is %+.3f m from it)", m.baseName, static_cast<double>(m.baseAlong),
                      static_cast<double>(r.endAlong - m.baseAlong));
    char ax[120] = "axes unreadable";
    if (r.axesOk)
        std::snprintf(ax, sizeof(ax), "end w axis (%.3f,%.3f,%.3f)", static_cast<double>(r.endAx[2][0]), static_cast<double>(r.endAx[2][1]),
                      static_cast<double>(r.endAx[2][2]));
    return std::snprintf(out, cap, "hatch geometry: moved after the settle (frame %u): rec 0x%llX name=%s: start local centre (%.3f,%.3f,%.3f) "
                         "end (%.3f,%.3f,%.3f); rotated %.2f deg, %s; along start %.3f m end %.3f m%s; first move %+.2f s, last move %+.2f s "
                         "after the settle.", frame, static_cast<unsigned long long>(r.addr), r.name, static_cast<double>(r.startC[0]),
                         static_cast<double>(r.startC[1]), static_cast<double>(r.startC[2]), static_cast<double>(r.endC[0]),
                         static_cast<double>(r.endC[1]), static_cast<double>(r.endC[2]), static_cast<double>(r.rotationDeg), ax,
                         static_cast<double>(r.startAlong), static_cast<double>(r.endAlong), base, static_cast<double>(r.startS),
                         static_cast<double>(r.stopS));
}

}  // namespace edvr::hatchgeo
