#pragma once
// The hatch geometry probe's pure half (hatch_geometry.cpp; TEMPORARY, Built v8.2, hotkey.hatch_probe). No Windows, no engine memory.
//
// Why: the docking object's state did not change when the hangar doors closed (Built v8 flight, the pilot's report). This probe asks the other
// question: do the hatch-related records themselves move? On one sampled frame in six, while the hatch probe is on, the store observer keeps the
// records whose name holds BlastShield, LandingPad, Lift, Hanger or Hangar (cap 64 per sampled frame). Each kept record's sphere centre
// (rec+0x240, PROVEN in sealed_trial_core.h) and its 12 floats at rec+0x190 (the layout NOT proven: logged raw) are tracked across sampled frames.
// A line is written when a record appears, when it has been absent for 30 sampled frames, or when its centre moves more than 0.05 m or a pose
// float moves more than 1e-3 since its last logged value.
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
constexpr uint32_t kLineCap = 600;        // "hatch geometry:" lines per session; past it, lines are counted as suppressed
constexpr float kMoveM = 0.05f;           // a centre moving more than this (metres) since its last logged value is a change
constexpr float kPoseEps = 1e-3f;         // a pose float moving more than this since its last logged value is a change
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

enum class Kind : uint8_t { kAppear = 0, kChange = 1, kDisappear = 2 };

inline const char* kindName(Kind k) noexcept {
    switch (k) {
        case Kind::kAppear: return "appeared";
        case Kind::kChange: return "changed";
        default: return "disappeared";
    }
}

// One event from a sampled frame. A disappearance has no sample (sample == kNoSample) and carries the last logged values.
struct Event {
    Kind kind = Kind::kAppear;
    uint32_t sample = kNoSample;
    uintptr_t addr = 0;
    char name[kNameLen] = {};
    float centre[3] = {};
    float centreDelta = 0.0f;     // change: metres from the last logged centre
    float poseDelta = 0.0f;       // change: the largest pose float delta from the last logged pose
    bool poseOk = false;
    float pose[kPoseFloats] = {};
    uint32_t missed = 0;          // disappearance: sampled frames absent
};

// Follows the records across sampled frames. Render thread only (no lock). Each track keeps the value it last logged; a change is measured
// against that value, so slow drift below the thresholds is not reported until it adds up.
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

    // Applies one sampled frame: appends its events, in sample order, then the disappearances.
    void apply(const Sample* s, size_t n, std::vector<Event>* out) {
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
                remember(*t, cur);
                out->push_back(makeEvent(Kind::kAppear, static_cast<uint32_t>(i), cur, 0.0f, 0.0f, 0));
                continue;
            }
            const float cd = distance3(cur.centre, t->centre);
            const bool bothPose = cur.poseOk && t->poseOk;
            const float pd = bothPose ? poseDelta(cur.pose, t->pose) : 0.0f;
            if (cd > kMoveM || (bothPose && pd > kPoseEps)) {
                out->push_back(makeEvent(Kind::kChange, static_cast<uint32_t>(i), cur, cd, pd, 0));
                remember(*t, cur);
            }
        }
        for (Track& t : tracks_) {
            if (!t.used || !t.present || t.seen) continue;
            if (++t.missed < kMissLimit) continue;
            t.present = false;
            Sample last;
            last.addr = t.addr;
            std::memcpy(last.name, t.name, sizeof(last.name));
            std::memcpy(last.centre, t.centre, sizeof(last.centre));
            std::memcpy(last.pose, t.pose, sizeof(last.pose));
            last.poseOk = t.poseOk;
            out->push_back(makeEvent(Kind::kDisappear, kNoSample, last, 0.0f, 0.0f, t.missed));
        }
    }

private:
    struct Track {
        uintptr_t addr = 0;
        char name[kNameLen] = {};
        float centre[3] = {};        // the last logged centre
        float pose[kPoseFloats] = {};  // the last logged pose
        bool poseOk = false;
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
        std::memcpy(t.centre, s.centre, sizeof(t.centre));
        std::memcpy(t.pose, s.pose, sizeof(t.pose));
        t.poseOk = s.poseOk;
    }

    static Event makeEvent(Kind k, uint32_t sample, const Sample& s, float cd, float pd, uint32_t missed) noexcept {
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
        e.missed = missed;
        return e;
    }

    std::vector<Track> tracks_;
    uint32_t refused_ = 0;
};

// The nearest Hanger*/Hangar* record's centre to point c, excluding the record skipAddr. False when there is none.
inline bool nearestHanger(const float c[3], const Sample* s, size_t n, uintptr_t skipAddr, float* out) noexcept {
    bool found = false;
    float best = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        if (s[i].addr == skipAddr || !isHangerName(s[i].name)) continue;
        const float d = distance3(c, s[i].centre);
        if (!found || d < best) {
            best = d;
            found = true;
        }
    }
    if (found && out) *out = best;
    return found;
}

// A bounded collection of sampled records, shared between the store's threads and the frame tick. Mutex is any lock (std::mutex in the engine;
// the rig drives the same code with threads). The lock covers one container operation; nothing holds it across a call into the game.
struct SampleStorage {
    std::vector<Sample> items;
    uint64_t drops = 0;
    void reserve(size_t cap) {
        items.clear();
        items.reserve(cap);
        drops = 0;
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
    void add(const Sample& s) {
        std::lock_guard<Mutex> lock(mutex_);
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

// "hatch geometry: <appeared|changed|disappeared> frame F t=T ms: rec 0x.. name=.. centre (x,y,z) ..." The pose is the 12 raw floats, or
// "unreadable". The nearest Hanger*/Hangar* distance is named, or "none".
inline int formatEvent(char* out, size_t cap, const Event& e, bool hasNearest, float nearest, uint32_t frame, uint64_t ms,
                       bool statusKnown, uint32_t flags, uint32_t flags2) noexcept {
    char extra[160] = {};
    switch (e.kind) {
        case Kind::kChange:
            std::snprintf(extra, sizeof(extra), " moved %.3f m, largest pose float change %.4f", static_cast<double>(e.centreDelta),
                          static_cast<double>(e.poseDelta));
            break;
        case Kind::kDisappear:
            std::snprintf(extra, sizeof(extra), " not seen for %u sampled frames (last centre shown)", e.missed);
            break;
        default:
            break;
    }
    char pose[160] = {};
    if (e.poseOk) {
        size_t at = 0;
        at += static_cast<size_t>(std::snprintf(pose, sizeof(pose), " pose=["));
        for (size_t i = 0; i < kPoseFloats && at < sizeof(pose) - 16; ++i)
            at += static_cast<size_t>(std::snprintf(pose + at, sizeof(pose) - at, "%s%.3f", i ? " " : "", static_cast<double>(e.pose[i])));
        std::snprintf(pose + at, sizeof(pose) - at, "]");
    } else {
        std::snprintf(pose, sizeof(pose), " pose=unreadable");
    }
    char nearText[48];   // not "near": windows.h defines near as an empty macro
    if (hasNearest) std::snprintf(nearText, sizeof(nearText), "%.2f m", static_cast<double>(nearest));
    else std::snprintf(nearText, sizeof(nearText), "none");
    char status[64];
    if (statusKnown) std::snprintf(status, sizeof(status), "flags=%u flags2=%u", flags, flags2);
    else std::snprintf(status, sizeof(status), "flags unknown");
    return std::snprintf(out, cap,
                         "hatch geometry: %s frame %u t=%llu ms: rec 0x%llX name=%s centre (%.3f,%.3f,%.3f)%s; nearest_hangar=%s;%s; status %s",
                         kindName(e.kind), frame, static_cast<unsigned long long>(ms), static_cast<unsigned long long>(e.addr), e.name,
                         static_cast<double>(e.centre[0]), static_cast<double>(e.centre[1]), static_cast<double>(e.centre[2]), extra,
                         nearText, pose, status);
}

}  // namespace edvr::hatchgeo
