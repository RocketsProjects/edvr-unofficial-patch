#pragma once
// The hatch geometry probe's pure half (hatch_geometry.cpp; TEMPORARY, Built v8.2, changed in v8.3, hotkey.hatch_probe). No Windows, no engine
// memory.
//
// Why: the docking object's state did not change when the hangar doors closed (Built v8 flight). This probe asks whether the hatch-related records
// themselves move. On one sampled frame in six, while the hatch probe is on, the store observer keeps the records whose name holds BlastShield,
// LandingPad, Lift, Hanger or Hangar (cap 64 a sample). Each kept record's sphere centre (rec+0x240, PROVEN in sealed_trial_core.h) and its 12
// floats at rec+0x190 (the layout NOT proven: logged raw) are followed across samples.
//
// v8.3 (the v8.2 flight burnt its 600 lines in 12 s): only records within 400 m of the camera are followed; a record's changes are reported at most
// once a second while it moves, and a "settled" line follows when it has not moved for 2 s, with the total movement since it appeared; the cap is
// 3000 lines a session. Each line carries the camera and the vector and distance to the nearest Hanger*/Hangar* record within 400 m. The
// positions are in the station's rotating frame (pads drift in x/y with the rotation), so the distance to the hangar record is the quantity to read.
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

namespace edvr::hatchgeo {

constexpr uint32_t kSampleEvery = 6;      // one frame in six is sampled
constexpr size_t kSampleCap = 64;         // records one sampled frame keeps; more are counted as drops
constexpr uint32_t kMissLimit = 30;       // sampled frames a record may be absent before it is reported gone
constexpr size_t kTrackCap = 256;         // records followed at once; a new one past this is counted as refused
constexpr uint32_t kLineCap = 3000;       // "hatch geometry:" lines per session; past it, lines are counted as suppressed
constexpr float kRangeM = 400.0f;         // records followed, and the hangar search, reach this far (metres)
constexpr float kMoveM = 0.05f;           // a centre this far from the last reported centre is a change
constexpr float kPoseEps = 1e-3f;         // a pose float this far from the last reported pose is a change
constexpr float kMoveStepM = 0.01f;       // between two samples, a centre this far apart means the record is moving
constexpr uint64_t kChangeIntervalMs = 1000;  // a moving record reports at most one change line per this interval
constexpr uint64_t kSettleMs = 2000;      // a record unmoved this long after moving reports its settled line
constexpr size_t kNameLen = 64;
constexpr size_t kPoseFloats = 12;
constexpr uint32_t kNoSample = 0xFFFFFFFFu;
constexpr size_t kNamesInLine = 12;       // names the first-sample line lists

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

// The records the probe follows: a name holding any of the five words.
inline bool isGeometryName(const char* name) noexcept {
    if (!name || !*name) return false;
    static const char* const kWords[] = {"BlastShield", "LandingPad", "Lift", "Hanger", "Hangar"};
    for (const char* w : kWords)
        if (containsNoCase(name, w)) return true;
    return false;
}

// The records a distance is measured to: names that begin Hanger or Hangar.
inline bool isHangerName(const char* name) noexcept {
    return name && (startsNoCase(name, "Hanger") || startsNoCase(name, "Hangar"));
}

// One kept record of a sampled frame.
struct Sample {
    uintptr_t addr = 0;
    char name[kNameLen] = {};
    float centre[3] = {};
    float pose[kPoseFloats] = {};
    bool poseOk = false;
};

inline float distance3(const float a[3], const float b[3]) noexcept {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// The largest absolute difference of the twelve pose floats (a stand-in for "the pose changed").
inline float poseDelta(const float a[kPoseFloats], const float b[kPoseFloats]) noexcept {
    float worst = 0.0f;
    for (size_t i = 0; i < kPoseFloats; ++i) {
        const float d = std::fabs(a[i] - b[i]);
        if (d > worst) worst = d;
    }
    return worst;
}

// Keeps the samples within radiusM of the camera, compacting them in place. Returns how many are kept.
inline size_t keepWithin(Sample* s, size_t n, const float camera[3], float radiusM) noexcept {
    size_t kept = 0;
    for (size_t i = 0; i < n; ++i) {
        if (distance3(s[i].centre, camera) <= radiusM) {
            if (kept != i) s[kept] = s[i];
            ++kept;
        }
    }
    return kept;
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

// One event from a sampled frame. A disappearance has no sample (sample == kNoSample) and carries the last centre and pose.
// centreDelta and poseDelta are measured from the centre and pose of the last line for this record; totalMove from where it first appeared.
struct Event {
    Kind kind = Kind::kAppear;
    uint32_t sample = kNoSample;
    uintptr_t addr = 0;
    char name[kNameLen] = {};
    float centre[3] = {};
    float centreDelta = 0.0f;
    float poseDelta = 0.0f;
    float totalMove = 0.0f;
    bool poseOk = false;
    float pose[kPoseFloats] = {};
    uint32_t missed = 0;          // disappearance: sampled frames absent
};

// Follows the records across sampled frames. Render thread only (no lock). Time is the caller's clock (milliseconds), so the rig can drive it.
// A record that moves reports a change at most once per kChangeIntervalMs, measured from the last line it had; once it has stopped for
// kSettleMs (and its centre or pose differs from its last line by more than the thresholds) it reports settled, with the movement since then.
class Tracker {
public:
    Tracker() { tracks_.assign(kTrackCap, Track{}); }

    // Forgets every track (a toggle-on): the next sampled frame's records all appear.
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

    // Applies one sampled frame at time nowMs: appends its events, in sample order, then the disappearances.
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
            if (!t->present) {
                t->present = true;
                t->start[0] = t->start[1] = t->start[2] = 0.0f;
                for (int k = 0; k < 3; ++k) t->start[k] = cur.centre[k];
                t->lastLineMs = nowMs;
                t->lastMoveMs = nowMs;
                t->pending = false;
                remember(*t, cur);
                out->push_back(makeEvent(Kind::kAppear, static_cast<uint32_t>(i), cur, *t, 0.0f, 0.0f, 0));
                continue;
            }
            const bool bothPose = cur.poseOk && t->prevPoseOk;
            const bool movedNow = distance3(cur.centre, t->prev) > kMoveStepM || (bothPose && poseDelta(cur.pose, t->prevPose) > kPoseEps);
            const float fromLine = distance3(cur.centre, t->line);
            const bool lineBothPose = cur.poseOk && t->linePoseOk;
            const float poseFromLine = lineBothPose ? poseDelta(cur.pose, t->linePose) : 0.0f;
            const bool differsFromLine = fromLine > kMoveM || (lineBothPose && poseFromLine > kPoseEps);
            if (movedNow) {
                t->lastMoveMs = nowMs;
                t->pending = true;
            }
            if (movedNow && differsFromLine && nowMs - t->lastLineMs >= kChangeIntervalMs) {
                out->push_back(makeEvent(Kind::kChange, static_cast<uint32_t>(i), cur, *t, fromLine, poseFromLine, 0));
                relineTo(*t, cur, nowMs);
            } else if (!movedNow && t->pending && nowMs - t->lastMoveMs >= kSettleMs) {
                t->pending = false;
                if (differsFromLine) {
                    out->push_back(makeEvent(Kind::kSettled, static_cast<uint32_t>(i), cur, *t, fromLine, poseFromLine, 0));
                    relineTo(*t, cur, nowMs);
                }
            }
            for (int k = 0; k < 3; ++k) t->prev[k] = cur.centre[k];
            std::memcpy(t->prevPose, cur.pose, sizeof(t->prevPose));
            t->prevPoseOk = cur.poseOk;
        }
        for (Track& t : tracks_) {
            if (!t.used || !t.present || t.seen) continue;
            if (++t.missed < kMissLimit) continue;
            t.present = false;
            Sample last;
            last.addr = t.addr;
            std::memcpy(last.name, t.name, sizeof(last.name));
            for (int k = 0; k < 3; ++k) last.centre[k] = t.prev[k];
            std::memcpy(last.pose, t.prevPose, sizeof(last.pose));
            last.poseOk = t.prevPoseOk;
            out->push_back(makeEvent(Kind::kDisappear, kNoSample, last, t, 0.0f, 0.0f, t.missed));
        }
    }

private:
    struct Track {
        uintptr_t addr = 0;
        char name[kNameLen] = {};
        float start[3] = {};         // where the record first appeared
        float prev[3] = {};          // its centre at the last sample
        float prevPose[kPoseFloats] = {};
        bool prevPoseOk = false;
        float line[3] = {};          // its centre at its last reported line
        float linePose[kPoseFloats] = {};
        bool linePoseOk = false;
        uint64_t lastLineMs = 0;
        uint64_t lastMoveMs = 0;
        bool pending = false;        // it moved since its last reported line
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

    static void remember(Track& t, const Sample& s) noexcept {
        std::memcpy(t.name, s.name, sizeof(t.name));
        for (int k = 0; k < 3; ++k) {
            t.prev[k] = s.centre[k];
            t.line[k] = s.centre[k];
        }
        std::memcpy(t.prevPose, s.pose, sizeof(t.prevPose));
        std::memcpy(t.linePose, s.pose, sizeof(t.linePose));
        t.prevPoseOk = s.poseOk;
        t.linePoseOk = s.poseOk;
    }

    // Records the sample as the record's last reported value at time nowMs.
    static void relineTo(Track& t, const Sample& s, uint64_t nowMs) noexcept {
        for (int k = 0; k < 3; ++k) t.line[k] = s.centre[k];
        std::memcpy(t.linePose, s.pose, sizeof(t.linePose));
        t.linePoseOk = s.poseOk;
        t.lastLineMs = nowMs;
        t.pending = false;
    }

    static Event makeEvent(Kind k, uint32_t sample, const Sample& s, const Track& t, float cd, float pd, uint32_t missed) noexcept {
        Event e;
        e.kind = k;
        e.sample = sample;
        e.addr = s.addr;
        std::memcpy(e.name, s.name, sizeof(e.name));
        std::memcpy(e.centre, s.centre, sizeof(e.centre));
        std::memcpy(e.pose, s.pose, sizeof(e.pose));
        e.poseOk = s.poseOk;
        e.centreDelta = cd;
        e.poseDelta = pd;
        e.totalMove = distance3(s.centre, t.start);
        e.missed = missed;
        return e;
    }

    std::vector<Track> tracks_;
    uint32_t refused_ = 0;
};

// The nearest Hanger*/Hangar* record to the point c within limitM, excluding the record skipAddr. vec is the vector from c to that record's
// centre; dist its length; name its name.
struct Nearest {
    bool has = false;
    float dist = 0.0f;
    float vec[3] = {};
    char name[kNameLen] = {};
};

inline Nearest nearestHanger(const float c[3], const Sample* s, size_t n, uintptr_t skipAddr, float limitM) noexcept {
    Nearest best;
    for (size_t i = 0; i < n; ++i) {
        if (s[i].addr == skipAddr || !isHangerName(s[i].name)) continue;
        const float d = distance3(c, s[i].centre);
        if (d > limitM || (best.has && d >= best.dist)) continue;
        best.has = true;
        best.dist = d;
        for (int k = 0; k < 3; ++k) best.vec[k] = s[i].centre[k] - c[k];
        std::memcpy(best.name, s[i].name, sizeof(best.name));
    }
    return best;
}

// A bounded collection of sampled records, shared between the store's threads and the frame tick. Mutex is any lock (std::mutex in the engine;
// the rig drives the same code with threads). The lock covers one container operation; nothing holds it across a call into the game.
struct SampleStorage {
    std::vector<Sample> items;
    uint64_t drops = 0;
    uintptr_t ctx = 0;           // the render context the store read from a bucket (for the camera)
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

// What a line adds to the event: the camera (when known), the nearest hangar record (when one is within range), the frame's status words.
struct LineContext {
    bool hasCamera = false;
    float camera[3] = {};
    uint64_t ms = 0;
    bool statusKnown = false;
    uint32_t flags = 0;
    uint32_t flags2 = 0;
};

// "hatch geometry: <appeared|changed|settled|disappeared> frame F t=T ms: rec 0x.. name=.. centre (x,y,z) camera (x,y,z) dist_cam D m; <the
// change: moved d m since its last line, total d m since it appeared, pose change p | the settle: the same | the disappearance>;
// nearest_hangar=<name> vector (vx,vy,vz) dist D m | none; pose=[12 floats] | unreadable; status flags=.. flags2=..".
inline int formatEvent(char* out, size_t cap, const Event& e, const LineContext& ctx, uint32_t frame, const Nearest& hangar) noexcept {
    char detail[200] = {};
    switch (e.kind) {
        case Kind::kChange:
            std::snprintf(detail, sizeof(detail), " moved %.3f m since its last line, total %.3f m since it appeared, largest pose change %.4f",
                          static_cast<double>(e.centreDelta), static_cast<double>(e.totalMove), static_cast<double>(e.poseDelta));
            break;
        case Kind::kSettled:
            std::snprintf(detail, sizeof(detail), " settled: moved %.3f m since its last line, total %.3f m since it appeared, largest pose "
                          "change %.4f", static_cast<double>(e.centreDelta), static_cast<double>(e.totalMove),
                          static_cast<double>(e.poseDelta));
            break;
        case Kind::kDisappear:
            std::snprintf(detail, sizeof(detail), " not seen for %u sampled frames (its last centre is shown)", e.missed);
            break;
        default:
            std::snprintf(detail, sizeof(detail), " appeared");
            break;
    }
    char camera[120] = "camera unknown";
    if (ctx.hasCamera) {
        const float dc = distance3(e.centre, ctx.camera);
        std::snprintf(camera, sizeof(camera), "camera (%.2f,%.2f,%.2f) dist_cam %.2f m", static_cast<double>(ctx.camera[0]),
                      static_cast<double>(ctx.camera[1]), static_cast<double>(ctx.camera[2]), static_cast<double>(dc));
    }
    char nearText[160] = "nearest_hangar=none";   // not "near": windows.h defines near as an empty macro
    if (hangar.has)
        std::snprintf(nearText, sizeof(nearText), "nearest_hangar=%s vector (%.2f,%.2f,%.2f) dist %.2f m", hangar.name,
                      static_cast<double>(hangar.vec[0]), static_cast<double>(hangar.vec[1]), static_cast<double>(hangar.vec[2]),
                      static_cast<double>(hangar.dist));
    char pose[160] = {};
    if (e.poseOk) {
        size_t at = static_cast<size_t>(std::snprintf(pose, sizeof(pose), " pose=["));
        for (size_t i = 0; i < kPoseFloats && at < sizeof(pose) - 16; ++i)
            at += static_cast<size_t>(std::snprintf(pose + at, sizeof(pose) - at, "%s%.3f", i ? " " : "", static_cast<double>(e.pose[i])));
        std::snprintf(pose + at, sizeof(pose) - at, "]");
    } else {
        std::snprintf(pose, sizeof(pose), " pose=unreadable");
    }
    char status[64];
    if (ctx.statusKnown) std::snprintf(status, sizeof(status), "flags=%u flags2=%u", ctx.flags, ctx.flags2);
    else std::snprintf(status, sizeof(status), "flags unknown");
    return std::snprintf(out, cap, "hatch geometry: %s frame %u t=%llu ms: rec 0x%llX name=%s centre (%.3f,%.3f,%.3f) %s;%s; %s;%s; status %s",
                         kindName(e.kind), frame, static_cast<unsigned long long>(ctx.ms), static_cast<unsigned long long>(e.addr), e.name,
                         static_cast<double>(e.centre[0]), static_cast<double>(e.centre[1]), static_cast<double>(e.centre[2]), camera,
                         detail, nearText, pose, status);
}

}  // namespace edvr::hatchgeo
