#pragma once
// The sealed-hangar trial's pure half (sealed_trial.cpp; TEMPORARY, the manual trial of docs/concourse-culling-2026-10-11.md).
// No Windows, no engine memory: the sphere rule that decides which records the store keeps, the radius clamp, and the text
// of the toggle line and the 10-second line. The rig tools\sealed_trial_test compiles it alone.
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
constexpr uint64_t kWindowMs = 10000;        // the statistics line's period

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
};

// "sealed trial: window 10 s culled=.. kept=.. kept_unjudged=.. sun_pairs_skipped=.. eval_ms=.. faults=.. radius_m=..".
inline int formatWindow(char* out, size_t cap, const Window& w, float radiusM) noexcept {
    return std::snprintf(out, cap,
                         "sealed trial: window %llu s culled=%llu kept=%llu kept_unjudged=%llu sun_pairs_skipped=%llu "
                         "eval_ms=%.3f faults=%llu radius_m=%.1f",
                         static_cast<unsigned long long>(kWindowMs / 1000), static_cast<unsigned long long>(w.culled),
                         static_cast<unsigned long long>(w.kept), static_cast<unsigned long long>(w.keptUnknown),
                         static_cast<unsigned long long>(w.sunSkipped), static_cast<double>(w.evalNs) / 1.0e6,
                         static_cast<unsigned long long>(w.faults), static_cast<double>(radiusM));
}

// "sealed trial: on at frame F (radius R m; camera (x,y,z) or unknown; the sun pair is skipped; records outside the sphere
// lose every view; records containing the camera are kept)" and the matching off line.
inline int formatToggle(char* out, size_t cap, bool on, uint32_t frame, float radiusM, const float camera[3],
                        bool cameraKnown) noexcept {
    if (!on) return std::snprintf(out, cap, "sealed trial: off at frame %u", frame);
    if (!cameraKnown)
        return std::snprintf(out, cap, "sealed trial: on at frame %u (radius %.1f m; camera unknown: nothing is culled "
                                       "until the view array is read)", frame, static_cast<double>(radiusM));
    return std::snprintf(out, cap,
                         "sealed trial: on at frame %u (radius %.1f m; camera (%.1f,%.1f,%.1f); the sun pair is skipped; "
                         "records outside the sphere lose every view; records containing the camera are kept)",
                         frame, static_cast<double>(radiusM), static_cast<double>(camera[0]),
                         static_cast<double>(camera[1]), static_cast<double>(camera[2]));
}

}  // namespace edvr::sealed
