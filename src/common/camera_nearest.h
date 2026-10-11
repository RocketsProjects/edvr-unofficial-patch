#pragma once
// The nearest write (fix.camera_nearest, temporary A/B; docs\camera-rows-carry-2026-09-25.md, the 2026-10-11 nearest entry).
//
// The legacy chooser (chooseCameraRows, src\d3d11\temporal_pass.cpp) takes the frame's camera rows from the continuous writes (within 3 deg of the
// last applied rows) by its own preference: the bound object's, then the latest. Flight 6 (edvr_gfx_20261011_141016) showed that the track is what
// matters: the drawn camera at frame f+1 equals the write picked at f exactly. The rare errors were writes 90 to 125 m off the track, and every guard
// rejection there was real motion of about 0.55 m a frame. So the last step is the write NEAREST the previous applied pick.
//
// Distance: translation in metres plus kMetresPerDeg for each degree of rotation. The weight: one degree of view rotation moves a scene point 2.9 m
// away by 5 cm, the depth of the cockpit and the hangar, so 0.05 m a degree counts a rotation as the translation it would look like at that depth.
//
// Re-anchor: when the nearest write is farther than max(kReanchorMinM, kReanchorStepMul * the previous step) from the previous pick AND the legacy
// pick is the same pose as last frame's (it has held two frames), the legacy pick is taken. That is a real cut: the galaxy map, a camera switch.
//
// Never hold: the rule only returns one of the frame's continuous writes or the legacy pick, never the previous rows. The identical-to-previous twin
// is not offered (the chooser excludes it), since it is last frame's rows again.
//
// With the key off the rule still runs and counts: its state follows the applied (legacy) rows, so its counts say what it would have done on the
// track the flight actually flew.
//
// Pure: no Windows, no globals. The chooser's use is in chooseCameraRows; the rig is tools\temporal_test.

#include "camera_ipd_pair.h"

namespace edvr::camnearest {

constexpr float kMetresPerDeg = 0.05f;      // the rotation term's weight: a degree counts as 5 cm of translation
constexpr float kReanchorMinM = 1.0f;       // a nearest write farther than this (or than kReanchorStepMul steps) can be a cut
constexpr float kReanchorStepMul = 3.0f;

// The state: the last applied pick and the step that brought it there, and the legacy pick of the last frame.
struct State {
    bool havePrev = false;
    float prev[12] = {};
    float step = 0.0f;          // the translation step, metres, from the pick before to prev (0 with none)
    bool haveLegacy = false;
    float legacy[12] = {};
};

// The distance between two poses: translation in metres plus kMetresPerDeg per degree of rotation.
inline float distance(const float a[12], const float b[12]) {
    const float dx = a[3] - b[3], dy = a[7] - b[7], dz = a[11] - b[11];
    float ra[9], rb[9];
    ipdpair::rot3Of34(a, ra);
    ipdpair::rot3Of34(b, rb);
    return std::sqrt(dx * dx + dy * dy + dz * dz) + kMetresPerDeg * ipdpair::angle3Deg(ra, rb);
}

// The legacy pick is the same pose as last frame's legacy pick (the backstop's samePose: 1 mm, 0.1 deg).
inline bool legacyAgrees(const State& s, const float legacy[12]) {
    return s.haveLegacy && ipdpair::samePose(legacy, s.legacy);
}

inline void noteLegacy(State& s, const float legacy[12]) {
    std::memcpy(s.legacy, legacy, sizeof(s.legacy));
    s.haveLegacy = true;
}

// The verdict for one frame. index: the offered candidate taken, or -1 for the legacy pick. dist: the nearest candidate's distance from the
// previous pick. reanchor: the legacy pick is taken because a cut was confirmed.
struct Verdict {
    int index = -1;
    float dist = 0.0f;
    bool reanchor = false;
};

// cands: the continuous writes in the chooser's preference order (ties go to the earlier). legacyAgreed: legacyAgrees for this frame's legacy pick.
// Without a previous pick the frame takes the legacy pick.
inline Verdict decide(const State& s, const float* const* cands, int n, bool legacyAgreed) {
    Verdict v;
    if (!s.havePrev) return v;
    int best = -1;
    float bestD = 0.0f;
    for (int k = 0; k < n; ++k) {
        if (!cands[k]) continue;
        const float d = distance(cands[k], s.prev);
        if (best < 0 || d < bestD) {
            best = k;
            bestD = d;
        }
    }
    if (best < 0) return v;
    v.dist = bestD;
    const float limit = std::max(kReanchorMinM, kReanchorStepMul * s.step);
    if (bestD > limit && legacyAgreed) {
        v.reanchor = true;
        return v;
    }
    v.index = best;
    return v;
}

// The candidate applied this frame: the verdict's with the key on, the legacy pick (-1) with it off.
inline int applied(bool keyOn, const Verdict& v) { return keyOn ? v.index : -1; }

// Records the rows applied this frame: the state the next frame measures from.
inline void commit(State& s, const float rows[12]) {
    if (s.havePrev) {
        const float dx = rows[3] - s.prev[3], dy = rows[7] - s.prev[7], dz = rows[11] - s.prev[11];
        s.step = std::sqrt(dx * dx + dy * dy + dz * dz);
    } else {
        s.step = 0.0f;
    }
    std::memcpy(s.prev, rows, sizeof(s.prev));
    s.havePrev = true;
}

// ---- the 30 s line -------------------------------------------------------------------------------------------------------------------------------------------

struct Stats {
    uint32_t frames = 0;        // the legacy picks the rule judged in the window
    uint32_t differs = 0;       // ...whose nearest write is not the legacy pick
    uint32_t reanchors = 0;     // ...where the legacy pick was taken as a cut
    float maxDistM = 0.0f;      // the largest distance of a nearest write from the previous pick, metres
};

// differs: the verdict's nearest write is not the legacy pick (the caller compares the rows' ring indices).
inline void note(Stats& s, const Verdict& v, bool differs) {
    ++s.frames;
    if (v.reanchor) ++s.reanchors;
    if (v.index >= 0) {
        if (differs) ++s.differs;
        if (v.dist > s.maxDistM) s.maxDistM = v.dist;
    }
}

inline void resetWindow(Stats& s) {
    s.frames = 0;
    s.differs = 0;
    s.reanchors = 0;
    s.maxDistM = 0.0f;
}

// "temporal camera nearest: frames=.. differs_from_legacy=.. reanchors=.. max_dist_m=.. key=on|off". Returns the length written, or 0 when the
// buffer is too small.
inline int formatReport(char* buf, size_t n, const Stats& s, bool keyOn) {
    const int len = std::snprintf(buf, n,
        "temporal camera nearest: frames=%u differs_from_legacy=%u reanchors=%u max_dist_m=%.3f key=%s",
        s.frames, s.differs, s.reanchors, static_cast<double>(s.maxDistM), keyOn ? "on" : "off");
    return (len <= 0 || static_cast<size_t>(len) >= n) ? 0 : len;
}

}  // namespace edvr::camnearest
