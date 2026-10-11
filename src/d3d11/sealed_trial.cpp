// The sealed-hangar trial (sealed_trial.h). The sphere rule is in sealed_trial_core.h; the store detour and its gate are in
// store_hook.cpp. This file holds the toggle, the radius and its ladder, the per-record decision in the store's observer, and the
// lines.
#include "sealed_trial.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "../common/config.h"
#include "../common/log.h"
#include "sealed_trial_core.h"
#include "store_hook.h"

namespace edvr {
namespace {

// The render context's layout (the same offsets as shadow_gate_core.h): views at +0x40, 0x6A0 bytes each; the view count
// at +0x1A940; the main view (view 0) origin at +0x540 of its own struct, so the camera is at ctx + 0x40 + 0x540.
constexpr uintptr_t kCtxCount = 0x1A940;
constexpr uintptr_t kCameraOff = 0x40 + 0x540;
constexpr uintptr_t kRecCentre = 0x240;     // float3: the world sphere centre
constexpr uintptr_t kRecMask = 0x208;       // u64: the view mask the store wrote
constexpr uintptr_t kRecAdmitted = 0x234;   // u8: the admitted flag the store wrote
constexpr uintptr_t kRecRadius = 0x280;     // float: the sphere radius

std::atomic<bool> g_on{false};
// The live radius: the configured start, then the ladder's rungs while the trial is on. Changed on the render thread (the step
// keys); the store's observer reads it through the atomic.
std::atomic<float> g_radius{sealed::kDefaultRadiusM};
std::atomic<uint64_t> g_culled{0}, g_kept{0}, g_keptUnknown{0}, g_sunSkipped{0}, g_evalNs{0}, g_faults{0};
std::atomic<float> g_camX{0.0f}, g_camY{0.0f}, g_camZ{0.0f};
std::atomic<bool> g_cameraKnown{false};
uint64_t g_windowStartMs = 0;                        // render thread only

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
}

sealed::Window takeWindow() noexcept {
    sealed::Window w;
    w.culled = g_culled.load();
    w.kept = g_kept.load();
    w.keptUnknown = g_keptUnknown.load();
    w.sunSkipped = g_sunSkipped.load();
    w.evalNs = g_evalNs.load();
    w.faults = g_faults.load();
    return w;
}

// Writes the window's line at the current radius, then starts a new window.
void noteWindow() {
    char line[400];
    sealed::formatWindow(line, sizeof(line), takeWindow(), g_radius.load(std::memory_order_relaxed));
    Log::get().note("%s", line);
    resetWindow();
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
    Log::get().note("sealed trial: off. hotkey.sealed_trial toggles it; the sphere starts at %.1f m (diag.sealed_trial_radius_m). "
                    "While on, hotkey.sealed_trial_smaller and hotkey.sealed_trial_larger step it through 80, 60, 40, 30, 20, 15 "
                    "and 10 m. Records outside the sphere lose their views in the admission store, and the sun pair is skipped.",
                    static_cast<double>(start));
}

void sealedTrialToggle(const char* trigger, uint32_t frame) {
    char line[400];
    const float radius = g_radius.load(std::memory_order_relaxed);
    if (g_on.load(std::memory_order_acquire)) {
        noteWindow();
        g_on.store(false, std::memory_order_release);
        storeHookRecompute();
        sealed::formatToggle(line, sizeof(line), false, frame, radius, nullptr, false);
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
    resetWindow();
    g_windowStartMs = GetTickCount64();
    const float cam[3] = {g_camX.load(), g_camY.load(), g_camZ.load()};
    sealed::formatToggle(line, sizeof(line), true, frame, radius, cam, g_cameraKnown.load());
    Log::get().note("%s (trigger %s)", line, trigger);
}

// One rung while the trial is on. A rung that moves closes the window of the old radius first, so the new radius starts a window
// of its own; a clamp changes nothing and says so. Off, the key logs a refusal.
void sealedTrialStep(int dir, const char* trigger, uint32_t frame) {
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

void sealedTrialFrame(uint32_t frame) {
    (void)frame;
    if (!g_on.load(std::memory_order_acquire)) return;
    const uint64_t now = GetTickCount64();
    if (now - g_windowStartMs < sealed::kWindowMs) return;
    g_windowStartMs = now;
    noteWindow();
}

bool sealedTrialOn() noexcept { return g_on.load(std::memory_order_acquire); }

bool sealedTrialSkipsSunPair() noexcept { return g_on.load(std::memory_order_acquire); }

void sealedTrialNoteSunSkip() noexcept { g_sunSkipped.fetch_add(1, std::memory_order_relaxed); }

// The rule for one record the store has written. Every read and write is validated; a failure leaves the record as the
// store wrote it and counts a fault.
void sealedTrialAfterStore(uintptr_t bucket, uintptr_t record) noexcept {
    LARGE_INTEGER t0{}, t1{};
    QueryPerformanceCounter(&t0);
    uintptr_t ctx = 0;
    uint32_t count = 0;
    float cam[3] = {};
    if (!bucket || !record || !readVal(&ctx, bucket) || !ctx || !readVal(&count, ctx + kCtxCount) || count == 0 ||
        !readMem(cam, ctx + kCameraOff, sizeof(cam))) {
        g_faults.fetch_add(1, std::memory_order_relaxed);
        QueryPerformanceCounter(&t1);
        g_evalNs.fetch_add(static_cast<uint64_t>(nanoseconds(t0, t1)), std::memory_order_relaxed);
        return;
    }
    g_camX.store(cam[0], std::memory_order_relaxed);
    g_camY.store(cam[1], std::memory_order_relaxed);
    g_camZ.store(cam[2], std::memory_order_relaxed);
    g_cameraKnown.store(true, std::memory_order_relaxed);
    const float trialR = g_radius.load(std::memory_order_relaxed);

    float centre[3] = {};
    float radius = 0.0f;
    if (!readMem(centre, record + kRecCentre, sizeof(centre)) || !readVal(&radius, record + kRecRadius) ||
        !std::isfinite(radius) || !(radius > 0.0f) || !std::isfinite(centre[0]) || !std::isfinite(centre[1]) ||
        !std::isfinite(centre[2])) {
        g_keptUnknown.fetch_add(1, std::memory_order_relaxed);
    } else if (sealed::keepRecord(centre, radius, cam, trialR)) {
        g_kept.fetch_add(1, std::memory_order_relaxed);
    } else {
        const uint64_t none = 0;
        const uint8_t notAdmitted = 0;
        if (writeMem(record + kRecMask, &none, sizeof(none)) && writeMem(record + kRecAdmitted, &notAdmitted, 1))
            g_culled.fetch_add(1, std::memory_order_relaxed);
        else
            g_faults.fetch_add(1, std::memory_order_relaxed);
    }
    QueryPerformanceCounter(&t1);
    g_evalNs.fetch_add(static_cast<uint64_t>(nanoseconds(t0, t1)), std::memory_order_relaxed);
}

}  // namespace edvr
