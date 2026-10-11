#pragma once
// The sealed-hangar trial's pure half (sealed_trial.cpp; TEMPORARY, the manual trial of docs/concourse-culling-2026-10-11.md).
// No Windows, no engine memory: the sphere rule that decides which records the store keeps, the radius clamp, the radius ladder
// that hotkey.sealed_trial_smaller and hotkey.sealed_trial_larger step through, and the text of the lines. The rig
// tools\sealed_trial_test compiles it alone.
//
// The rule, applied in the admission store (FUN_144312040) to each record it writes:
//   keep when the record's sphere CONTAINS the camera (the hull, the hangar) or INTERSECTS the sphere of radius R around
//   the camera; otherwise clear the record's view mask (rec+0x208) and its admitted flag (rec+0x234), in every view.
//   Sphere: centre rec+0x240 (world), radius rec+0x280. Camera: the main view's origin, render context + 0x40 + 0x540.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace edvr::sealed {

constexpr float kDefaultRadiusM = 80.0f;     // diag.sealed_trial_radius_m
constexpr float kMinRadiusM = 1.0f;
constexpr float kMaxRadiusM = 5000.0f;
constexpr uint64_t kWindowMs = 5000;         // the statistics line's period while the trial is on (every 5 s, so a radius step has its own line)

// The radius ladder, largest first. hotkey.sealed_trial_smaller and hotkey.sealed_trial_larger walk it one rung at a time.
constexpr uint32_t kStepCount = 7;
constexpr float kSteps[kStepCount] = {80.0f, 60.0f, 40.0f, 30.0f, 20.0f, 15.0f, 10.0f};

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

// The rule: keep a record that contains the camera or intersects the camera's sphere.
inline bool keepRecord(const float centre[3], float radius, const float camera[3], float R) noexcept {
    return containsCamera(centre, radius, camera) || intersectsCameraSphere(centre, radius, camera, R);
}

// The statistics of one window. evalNs is the time spent in the rule (the store's own time is not included).
struct Window {
    uint64_t culled = 0;
    uint64_t kept = 0;
    uint64_t keptUnknown = 0;     // a record whose sphere could not be read: kept, not judged
    uint64_t sunSkipped = 0;      // sun pair draws the trial skipped in the window
    uint64_t evalNs = 0;
    uint64_t faults = 0;          // reads or writes that failed (the record is left as it was)
    uint64_t torn = 0;            // a cull whose put-back failed too: the record may be left half culled (see cullRecordFaultSafe)
};

// "sealed trial: window 5 s culled=.. kept=.. kept_unjudged=.. sun_pairs_skipped=.. eval_ms=.. faults=.. torn=.. radius_m=..". The
// radius is the one the window ran at (a step closes the window of the old radius first).
// The view the rule's sphere was centred on is named at the end: "view source = explorer cam" (or eye rows, vscreen, origin).
inline int formatWindow(char* out, size_t cap, const Window& w, float radiusM, const char* viewSource) noexcept {
    return std::snprintf(out, cap,
                         "sealed trial: window %llu s culled=%llu kept=%llu kept_unjudged=%llu sun_pairs_skipped=%llu "
                         "eval_ms=%.3f faults=%llu torn=%llu radius_m=%.1f view source = %s",
                         static_cast<unsigned long long>(kWindowMs / 1000), static_cast<unsigned long long>(w.culled),
                         static_cast<unsigned long long>(w.kept), static_cast<unsigned long long>(w.keptUnknown),
                         static_cast<unsigned long long>(w.sunSkipped), static_cast<double>(w.evalNs) / 1.0e6,
                         static_cast<unsigned long long>(w.faults), static_cast<unsigned long long>(w.torn),
                         static_cast<double>(radiusM), viewSource);
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
