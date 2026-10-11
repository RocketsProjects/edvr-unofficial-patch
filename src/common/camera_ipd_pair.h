#pragma once
// The camera IPD pair (fix.camera_ipd_pair; docs\camera-rows-carry-2026-09-25.md, the 2026-10-11 IPD entry).
//
// A frame's camera-block writes include both eyes' views, and the legacy chooser cannot tell them from the probe faces and the auxiliary passes:
// they share the object, the thread, the coordinator and the projection (camera writes diagnostic, camera_writes.h). What does tell them apart is
// the runtime's eye-to-eye offset. A candidate D is an eye camera when the frame also holds a write P whose rotation is D's (or D's rotation times
// the runtime's relative cant, for a canted headset whose rows carry it) to within 0.3 deg, and whose translation is D's translation plus the
// offset of the other eye expressed in D's own axes, to within 2 mm.
//
// The reference eye is the RIGHT eye (eyeToHead[1]). Its partner is the left eye, at -IPD along its own sideways axis: the logged pairs (P = D -
// 0.0606 m * col0, col0 = the first column of D's rotation) are exactly that, with D the right eye. The offset is taken in the game's convention:
// Elite is given S*E*S of the runtime's eye-to-head (openvr_system.cpp GetEyeToHeadTransform, canted_display.h gameHandedness), so this does the same.
//
// The rule is ahead of the legacy chooser. With no pair the frame holds the previous pose for up to kHoldLimit frames, then the legacy chooser takes
// over (a real camera change: the galaxy map, the external camera, on foot), and its candidate is checked by the backstop: a jump over 3 cm that does
// not continue the previous step is rejected, and a jump that persists for kBackstopLimit frames is taken. The rule is parked and off by default
// (2026-10-11); the legacy chooser's backstop is camera_jump_guard.h, which reuses this header's departs and continuesStep tests.
//
// Pure: no Windows, no globals. The chooser is chooseCameraRows in src\d3d11\temporal_pass.cpp, and the rig is tools\temporal_test.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr::ipdpair {

constexpr float kPairMeters = 0.002f;     // a partner's translation against D's translation plus the offset
constexpr float kPairDeg = 0.3f;          // a partner's rotation against D's (or against the relative cant)
constexpr float kJumpMeters = 0.03f;      // the backstop: a candidate this far from the previous pose...
constexpr float kStepMeters = 0.01f;      // ...is taken only when it continues the previous step, to this plus kStepFraction of the step
constexpr float kStepFraction = 0.05f;
constexpr float kStepDeg = 0.5f;          // ...and its rotation continues the previous step to this many degrees
constexpr uint32_t kHoldLimit = 3;        // frames without a pair that hold the previous pose; the next one uses the legacy chooser
constexpr uint32_t kBackstopLimit = 3;    // backstop rejections in a row before a persisting jump is taken
constexpr float kIpdMinM = 0.02f;         // a runtime offset outside these bounds is not an IPD
constexpr float kIpdMaxM = 0.20f;
constexpr float kPi = 3.14159265358979323846f;
constexpr uint32_t kErrSamples = 4096;    // pair residuals kept per 30 s window (p50 and max over them)
inline constexpr float kIdentity3[9] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};

// ---- rotations and the runtime offset ----------------------------------------------------------------------------------------------------------------
// Row-major 3x4: element (r, c) at r*4 + c; the translation is column 3 (indices 3, 7, 11).

// The game's convention of a runtime eye-to-head transform: S*E*S with S = diag(1, 1, -1).
inline void gameEyeToHead(const float in[12], float out[12]) {
    std::memcpy(out, in, 12 * sizeof(float));
    out[2] = -in[2];
    out[6] = -in[6];
    out[8] = -in[8];
    out[9] = -in[9];
    out[11] = -in[11];
}

// The angle in degrees between two 3x3 rotations (row-major, stride 3): acos((trace(A^T B) - 1) / 2).
inline float angle3Deg(const float a[9], const float b[9]) {
    float tr = 0.0f;
    for (int i = 0; i < 9; ++i) tr += a[i] * b[i];
    float c = (tr - 1.0f) * 0.5f;
    if (c > 1.0f) c = 1.0f;
    if (c < -1.0f) c = -1.0f;
    return std::acos(c) * 180.0f / kPi;
}

// The 3x3 rotation of a row-major 3x4, stride 3 out.
inline void rot3Of34(const float m[12], float r[9]) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r[i * 3 + j] = m[i * 4 + j];
}

// A^T B for 3x3 row-major matrices.
inline void mulTransposeLeft3(const float a[9], const float b[9], float out[9]) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            out[i * 3 + j] = a[0 * 3 + i] * b[0 * 3 + j] + a[1 * 3 + i] * b[1 * 3 + j] + a[2 * 3 + i] * b[2 * 3 + j];
}

// The runtime's offset between the eyes, from the two eye-to-head transforms as openvr_system hands them (eye 0 left, eye 1 right; located, before
// the game's S*E*S). o is the left eye's place in the RIGHT eye's own axes, in metres: o = R_right^T (t_left - t_right). For a headset with no cant
// and an IPD of 60.6 mm this is (-0.0606, 0, 0). cantDeg is the angle between the two eyes' rotations.
struct Offset {
    bool valid = false;
    float o[3] = {};
    float ipdM = 0.0f;
    float cantDeg = 0.0f;
    float rel[9] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};   // the relative rotation Rr^T Rl: the partner's relative cant
};
inline Offset offsetFromEyes(const float eyeToHead[2][12]) {
    Offset out;
    float left[12], right[12];
    gameEyeToHead(eyeToHead[0], left);
    gameEyeToHead(eyeToHead[1], right);
    float d[3];
    for (int i = 0; i < 3; ++i) d[i] = left[i * 4 + 3] - right[i * 4 + 3];
    for (int i = 0; i < 3; ++i) {
        // (R^T d)_i = sum_k R_ki d_k
        out.o[i] = right[0 * 4 + i] * d[0] + right[1 * 4 + i] * d[1] + right[2 * 4 + i] * d[2];
    }
    bool finite = true;
    for (int i = 0; i < 12; ++i) finite = finite && std::isfinite(left[i]) && std::isfinite(right[i]);
    out.ipdM = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    float rl[9], rr[9], cant[9];
    rot3Of34(left, rl);
    rot3Of34(right, rr);
    mulTransposeLeft3(rr, rl, cant);   // the relative rotation Rr^T Rl
    std::memcpy(out.rel, cant, sizeof(cant));
    out.cantDeg = angle3Deg(cant, kIdentity3);
    out.valid = finite && out.ipdM >= kIpdMinM && out.ipdM <= kIpdMaxM;
    return out;
}

// ---- the partner test ----------------------------------------------------------------------------------------------------------------------------------------

// The partner's translation that candidate D predicts: D's translation plus D's rotation applied to the offset.
inline void predictPartner(const float d[12], const float o[3], float out[3]) {
    for (int i = 0; i < 3; ++i) out[i] = d[i * 4 + 3] + d[i * 4 + 0] * o[0] + d[i * 4 + 1] * o[1] + d[i * 4 + 2] * o[2];
}

// P is D's partner: its translation is the predicted one to kPairMeters, and its rotation relative to D is the identity (both eyes share the rows'
// rotation, the logged case) or the runtime's relative cant (the rows carry it), each to kPairDeg. transErrM, when given, is the translation residual.
inline bool isPartner(const float d[12], const float p[12], const float o[3], const float cant[9], float* transErrM = nullptr) {
    float pred[3];
    predictPartner(d, o, pred);
    const float dx = p[3] - pred[0], dy = p[7] - pred[1], dz = p[11] - pred[2];
    const float err = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (transErrM) *transErrM = err;
    if (!(err <= kPairMeters)) return false;
    float rd[9], rp[9], rel[9];
    rot3Of34(d, rd);
    rot3Of34(p, rp);
    mulTransposeLeft3(rd, rp, rel);   // D^T P: the identity when the rows share a rotation
    return angle3Deg(rel, kIdentity3) <= kPairDeg || angle3Deg(rel, cant) <= kPairDeg;
}

// The scan's result. ref is the reference (right-eye) candidate, partner its pair; families counts the reference candidates that qualify.
struct Scan {
    int ref = -1;
    int partner = -1;
    int families = 0;
    float transErrM = 0.0f;
};

// Scans n candidates (rows[i] points at 12 floats; seq[i] is its clock). A candidate is a reference when a partner exists. Several references
// take the one nearest the previous chosen pose (its translation; the latest clock when there is no previous pose). inPair[i] (may be null) is set
// to 1 for every candidate in a family, as reference or partner.
inline Scan scanPairs(const float* const* rows, const uint32_t* seq, int n, const float o[3], const float cant[9], const float* prev,
                      uint8_t* inPair) {
    Scan s;
    if (inPair)
        for (int i = 0; i < n; ++i) inPair[i] = 0;
    float bestDist = 0.0f;
    uint32_t bestSeq = 0;
    bool haveBest = false;
    for (int i = 0; i < n; ++i) {
        const float* d = rows[i];
        float pred[3];
        predictPartner(d, o, pred);
        int found = -1;
        float err = 0.0f;
        for (int j = 0; j < n && found < 0; ++j) {
            if (j == i) continue;
            const float* p = rows[j];
            // The translation test is the cheap one; the rotation follows only for a near translation.
            if (!(std::fabs(p[3] - pred[0]) <= kPairMeters && std::fabs(p[7] - pred[1]) <= kPairMeters &&
                  std::fabs(p[11] - pred[2]) <= kPairMeters))
                continue;
            if (isPartner(d, p, o, cant, &err)) found = j;
        }
        if (found < 0) continue;
        ++s.families;
        if (inPair) {
            inPair[i] = 1;
            inPair[found] = 1;
        }
        float dist = 0.0f;
        if (prev) {
            const float dx = d[3] - prev[3], dy = d[7] - prev[7], dz = d[11] - prev[11];
            dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        }
        bool better = !haveBest;
        if (haveBest) {
            if (prev) better = dist < bestDist || (dist == bestDist && seq[i] > bestSeq);
            else better = seq[i] > bestSeq;
        }
        if (better) {
            s.ref = i;
            s.partner = found;
            s.transErrM = err;
            bestDist = dist;
            bestSeq = seq[i];
            haveBest = true;
        }
    }
    return s;
}

// The same pose: translations within 1 mm and rotations within 0.1 deg.
inline bool samePose(const float a[12], const float b[12]) {
    const float dx = a[3] - b[3], dy = a[7] - b[7], dz = a[11] - b[11];
    if (!(std::sqrt(dx * dx + dy * dy + dz * dz) <= 0.001f)) return false;
    float ra[9], rb[9];
    rot3Of34(a, ra);
    rot3Of34(b, rb);
    return angle3Deg(ra, rb) <= 0.1f;
}

// ---- the backstop --------------------------------------------------------------------------------------------------------------------------------------------

// The backstop's first test: the candidate's translation is more than kJumpMeters from the previous chosen pose.
inline bool departs(const float cand[12], const float prev[12]) {
    const float dx = cand[3] - prev[3], dy = cand[7] - prev[7], dz = cand[11] - prev[11];
    return std::sqrt(dx * dx + dy * dy + dz * dz) > kJumpMeters;
}

// The backstop's exception: rotation and translation both continue the previous step, a constant-velocity prediction from the last two chosen
// poses (before is the one chosen before prev; null when there is none, which predicts no motion).
inline bool continuesStep(const float cand[12], const float prev[12], const float before[12]) {
    float pred[12];
    float stepLen = 0.0f;
    if (before) {
        float m[9], pr[9], pb[9];
        rot3Of34(before, pb);
        rot3Of34(prev, pr);
        mulTransposeLeft3(pb, pr, m);   // before^T prev: the last step, in the body's axes
        float prot[9];
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) prot[i * 3 + j] = pr[i * 3 + 0] * m[0 * 3 + j] + pr[i * 3 + 1] * m[1 * 3 + j] + pr[i * 3 + 2] * m[2 * 3 + j];
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) pred[i * 4 + j] = prot[i * 3 + j];
            pred[i * 4 + 3] = 2.0f * prev[i * 4 + 3] - before[i * 4 + 3];
        }
        const float sx = prev[3] - before[3], sy = prev[7] - before[7], sz = prev[11] - before[11];
        stepLen = std::sqrt(sx * sx + sy * sy + sz * sz);
    } else {
        std::memcpy(pred, prev, sizeof(pred));
    }
    const float dx = cand[3] - pred[3], dy = cand[7] - pred[7], dz = cand[11] - pred[11];
    const float tol = kStepMeters + kStepFraction * stepLen;
    if (!(std::sqrt(dx * dx + dy * dy + dz * dz) <= tol)) return false;
    float rc[9], rp[9];
    rot3Of34(cand, rc);
    rot3Of34(pred, rp);
    return angle3Deg(rc, rp) <= kStepDeg;
}

// ---- the chain: hold, legacy fallback and the backstop, frame by frame ------------------------------------------------------------------------------------

enum class Pick : uint8_t { None, Rule, Legacy, Hold };

// The chain's state: the last two chosen poses, the hold and backstop runs, and the episode count.
struct Chain {
    bool havePose = false;
    bool havePrev = false;
    float pose[12] = {};
    float before[12] = {};
    uint32_t holdRun = 0;
    uint32_t backRun = 0;
    uint32_t episodes = 0;
    bool inEpisode = false;
};

struct Verdict {
    Pick pick = Pick::None;
    bool episodeStart = false;   // the legacy chooser takes over: the first frame of an episode
    bool episodeEnd = false;     // a pair came back, ending an episode
    bool backstop = false;       // the legacy candidate was rejected this frame (Pick::Hold)
    uint32_t episode = 0;        // the episode's number, when episodeStart
};

inline void commitPose(Chain& c, const float* rows) {
    if (c.havePose) {
        std::memcpy(c.before, c.pose, sizeof(c.pose));
        c.havePrev = true;
    }
    std::memcpy(c.pose, rows, sizeof(c.pose));
    c.havePose = true;
}

// One frame. ruleOn: the runtime offset is valid (the rule can run). ruleRows: the rule's reference rows, or null when no pair qualifies.
// legacyRows: the legacy chooser's pick after the headset tag, or null. Rule and Legacy commit their rows; Hold keeps the previous pose.
inline Verdict decideFrame(Chain& c, bool ruleOn, const float* ruleRows, const float* legacyRows) {
    Verdict v;
    if (ruleOn && ruleRows) {
        v.pick = Pick::Rule;
        if (c.inEpisode) {
            c.inEpisode = false;
            v.episodeEnd = true;
        }
        c.holdRun = 0;
        c.backRun = 0;
        commitPose(c, ruleRows);
        return v;
    }
    if (ruleOn && c.havePose && c.holdRun < kHoldLimit) {
        ++c.holdRun;
        v.pick = Pick::Hold;
        return v;
    }
    if (ruleOn && c.havePose && !c.inEpisode) {
        c.inEpisode = true;
        v.episodeStart = true;
        v.episode = ++c.episodes;
    }
    if (legacyRows && c.havePose && departs(legacyRows, c.pose) &&
        !continuesStep(legacyRows, c.pose, c.havePrev ? c.before : nullptr) && c.backRun < kBackstopLimit) {
        ++c.backRun;
        v.backstop = true;
        v.pick = Pick::Hold;
        return v;
    }
    c.backRun = 0;
    if (legacyRows) {
        v.pick = Pick::Legacy;
        commitPose(c, legacyRows);
    }
    return v;
}

// The pick the chooser applies: the chain's with the key on; the legacy chooser's with it off, where the chain only counts.
inline Pick applyPick(bool keyOn, Pick chainPick) { return keyOn ? chainPick : Pick::Legacy; }

// ---- the 30 s line -----------------------------------------------------------------------------------------------------------------------------------------

struct Stats {
    uint32_t frames = 0;             // the chooser's frames in the window
    uint32_t paired = 0;             // frames with a pair family
    uint32_t held = 0;               // frames held
    uint32_t legacyFallback = 0;     // frames with no pair that the legacy chooser took
    uint32_t episodes = 0;           // legacy episodes started
    uint32_t disagree = 0;           // frames whose rule pose differs from the legacy pick's
    uint32_t backstop = 0;           // legacy candidates the backstop rejected
    bool ipdKnown = false;
    float ipdM = 0.0f;
    uint32_t errN = 0;
    float errMax = 0.0f;
    float errs[kErrSamples] = {};
};

// The window's counts start over; the runtime's IPD (the last one seen) is state and stays.
inline void resetWindow(Stats& s) {
    s.frames = 0;
    s.paired = 0;
    s.held = 0;
    s.legacyFallback = 0;
    s.episodes = 0;
    s.disagree = 0;
    s.backstop = 0;
    s.errN = 0;
    s.errMax = 0.0f;
}

// "temporal ipd pair: frames=.. paired=.. held=.. legacy_fallback=.. episodes=.. disagree_with_legacy=.. ipd_mm=.. pair_err_mm p50/max=.. key=on|off
//  backstop=..". Sorts the residual samples in place (the window ends with the line). Returns the length written, or 0 when the buffer is too small.
inline int formatReport(char* buf, size_t n, Stats& s, bool keyOn) {
    float p50 = 0.0f;
    if (s.errN > 0) {
        const uint32_t k = s.errN / 2;
        std::nth_element(s.errs, s.errs + k, s.errs + s.errN);
        p50 = s.errs[k];
    }
    char ipd[24];
    if (s.ipdKnown) std::snprintf(ipd, sizeof(ipd), "%.2f", static_cast<double>(s.ipdM * 1000.0f));
    else std::snprintf(ipd, sizeof(ipd), "n/a");
    const int len = std::snprintf(buf, n,
        "temporal ipd pair: frames=%u paired=%u held=%u legacy_fallback=%u episodes=%u disagree_with_legacy=%u ipd_mm=%s "
        "pair_err_mm p50/max=%.3f/%.3f key=%s backstop=%u",
        s.frames, s.paired, s.held, s.legacyFallback, s.episodes, s.disagree, ipd,
        static_cast<double>(p50 * 1000.0f), static_cast<double>(s.errMax * 1000.0f), keyOn ? "on" : "off", s.backstop);
    return (len <= 0 || static_cast<size_t>(len) >= n) ? 0 : len;
}

}  // namespace edvr::ipdpair
