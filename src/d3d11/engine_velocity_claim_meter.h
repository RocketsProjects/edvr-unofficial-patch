// The claim meter: what the primary-copy claims look like while they are live. MEASUREMENT ONLY (docs\flat-cz-fps-2026-10-11.md, round 5).
//
// WHY. The 09:17:15 flat flight of 2026-10-16 (edvr_gfx_20261016_090725.log) entered a regime with no scene change: emissions 0 -> 332 ->
// 15,273 in six minutes, 100% of merges and about half of the clears on the claimed (locked) path, the render thread's engine-motion hooks
// 0.14 -> 1.06 ms a frame. The log says a claim is made once a frame or so and is consumed (joined == emissions, active claims 0 at every
// sample), so the claims are legitimate and short-lived, but live for most of the job phase, where the millions of merge and clear calls are.
// Whether a lock-free claim-set pre-check (publish the 1-3 claimed addresses as a seqlock snapshot; walk without the lock; lock only on a hit)
// is safe and worth building rests on four numbers this meter takes: how much of the time the claim set is non-empty, how big it is at claimed
// calls, how long a claim lives, and how often a claimed call actually touches a claim.
//
// WHAT IT COSTS. Relaxed atomics, bumped under primaryCopy::g_mutex by the code that already holds it (a flip of the published flag, an
// emission, a claim's end, a claimed merge or clear). No per-call log line. The clock is read on a flip, an emission and a claim's end, about
// once a frame each, never on the lock-free exits.
//
// DECISION RULE (for the flight): if the zero-hit share of claimed calls is about 100% and the claim set is 3 or fewer at nearly every
// claimed call, the lock-free claim-set pre-check is cleared. A claim set that grows past 4, or a zero-hit share well under 100%, says it is
// not (the hits would take the lock anyway) and the design needs another look.
#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr {
namespace claimmeter {

constexpr unsigned kLifeBuckets = 48;   // power-of-two buckets of clock ticks: bucket b holds [2^b, 2^(b+1)), bucket 0 holds 0 and 1
constexpr unsigned kSizeBuckets = 4;    // claim-set size 1 / 2 / 3-4 / 5+
constexpr unsigned kSampleMax = 8;      // the entity line: the first emissions after a quiet period
constexpr unsigned kQuietSeconds = 10;  // a quiet period: this long without an emission
enum class End : unsigned { Consumed = 0, Cleared, Dropped, Count };
inline constexpr const char* kEndNames[static_cast<unsigned>(End::Count)] = {"consumed by the native copy", "cleared by a dictionary clear",
                                                                              "dropped at a frame boundary, a reset or a merge"};

inline int64_t qpcNow() noexcept {
    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
inline int64_t qpcFrequency() noexcept {
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    return f.QuadPart;
}
inline int64_t (*g_clock)() noexcept = &qpcNow;   // the rig substitutes a scripted clock
inline int64_t g_quietTicks = 0;                  // 0: kQuietSeconds of the QPC frequency, taken at first use; the rig sets it

// ---- the counters -------------------------------------------------------------------------------------------------
inline std::atomic<int64_t> g_liveSince{0}, g_liveTicks{0}, g_windowStart{0};
inline std::atomic<uint64_t> g_flips{0}, g_emissions{0}, g_bursts{0};
inline std::atomic<uint64_t> g_life[static_cast<unsigned>(End::Count)][kLifeBuckets];
inline std::atomic<uint64_t> g_mergeSize[kSizeBuckets], g_clearSize[kSizeBuckets];
inline std::atomic<uint64_t> g_mergeHits{0}, g_clearHits{0};

inline unsigned sizeBucket(size_t n) noexcept { return n <= 1 ? 0u : n == 2 ? 1u : n <= 4 ? 2u : 3u; }
inline unsigned lifeBucket(int64_t ticks) noexcept {
    if (ticks <= 1) return 0;
    unsigned b = 0;
    for (uint64_t v = static_cast<uint64_t>(ticks); v > 1; v >>= 1) ++b;
    return b < kLifeBuckets ? b : kLifeBuckets - 1;
}
inline void noteLife(End how, int64_t ticks) noexcept {
    g_life[static_cast<unsigned>(how)][lifeBucket(ticks)].fetch_add(1, std::memory_order_relaxed);
}
// The published emissions flag changed (ClaimLock::publish, under the mutex).
inline void liveFlip(bool live) noexcept {
    const int64_t now = g_clock();
    if (live) {
        g_liveSince.store(now, std::memory_order_relaxed);
        g_flips.fetch_add(1, std::memory_order_relaxed);
    } else {
        const int64_t since = g_liveSince.load(std::memory_order_relaxed);
        if (since && now > since) g_liveTicks.fetch_add(now - since, std::memory_order_relaxed);
        g_liveSince.store(0, std::memory_order_relaxed);
    }
}
// A claimed merge (the set was non-empty under the lock) and whether it touched a claim (staged a transfer or erased one).
inline void noteMerge(size_t claims, bool hit) noexcept {
    g_mergeSize[sizeBucket(claims)].fetch_add(1, std::memory_order_relaxed);
    if (hit) g_mergeHits.fetch_add(1, std::memory_order_relaxed);
}
// A claimed clear walk, and whether it cleared a claim.
inline void noteClear(size_t claims, bool hit) noexcept {
    g_clearSize[sizeBucket(claims)].fetch_add(1, std::memory_order_relaxed);
    if (hit) g_clearHits.fetch_add(1, std::memory_order_relaxed);
}

// ---- the entity samples: the first emissions after a quiet period (under the mutex) ----------------------------------------
struct Sample {
    uint64_t source = 0;
    uint32_t frame = 0;
    int64_t at = 0;                 // ticks after the burst's first emission
    uint32_t words[9] = {};         // native[0], native[1], the pose words native[2..6], the marker, native[77]
    int32_t slot = -1;              // the pool slot the native copy consumed it into, once it has
};
inline Sample g_samples[kSampleMax];
inline unsigned g_sampleCount = 0, g_burstLeft = 0;
inline int64_t g_lastEmissionAt = 0, g_burstStart = 0, g_burstQuiet = 0;
inline void noteEmission(uint64_t source, const uint32_t* native, uint32_t marker, uint32_t frame, int64_t now) noexcept {
    g_emissions.fetch_add(1, std::memory_order_relaxed);
    if (!g_quietTicks) g_quietTicks = qpcFrequency() * kQuietSeconds;
    const bool quiet = g_lastEmissionAt == 0 || now - g_lastEmissionAt >= g_quietTicks;
    g_lastEmissionAt = now;
    if (quiet) {   // a new burst: the samples of the one before it are replaced
        g_bursts.fetch_add(1, std::memory_order_relaxed);
        g_burstLeft = kSampleMax;
        g_sampleCount = 0;
        g_burstStart = now;
        g_burstQuiet = 0;
    }
    if (!g_burstLeft || g_sampleCount >= kSampleMax) return;
    Sample& s = g_samples[g_sampleCount++];
    --g_burstLeft;
    s.source = source;
    s.frame = frame;
    s.at = now - g_burstStart;
    s.slot = -1;
    if (native) {
        s.words[0] = native[0];
        s.words[1] = native[1];
        for (unsigned i = 0; i < 5; ++i) s.words[2 + i] = native[2 + i];
        s.words[7] = marker;
        s.words[8] = native[77];
    }
}
// The native copy consumed the claim of `source` into pool slot `slot`.
inline void noteSlot(uint64_t source, uint32_t slot) noexcept {
    for (unsigned i = 0; i < g_sampleCount; ++i)
        if (g_samples[i].source == source && g_samples[i].slot < 0) { g_samples[i].slot = static_cast<int32_t>(slot); return; }
}

// ---- one window ------------------------------------------------------------------------------------------------------
struct Window {
    int64_t windowTicks = 0, liveTicks = 0;
    uint64_t flips = 0, emissions = 0, bursts = 0;
    uint64_t life[static_cast<unsigned>(End::Count)][kLifeBuckets] = {};
    uint64_t mergeSize[kSizeBuckets] = {}, clearSize[kSizeBuckets] = {};
    uint64_t mergeHits = 0, clearHits = 0;
    Sample samples[kSampleMax];
    unsigned sampleCount = 0;
    uint64_t mergeClaimed() const noexcept { uint64_t n = 0; for (uint64_t v : mergeSize) n += v; return n; }
    uint64_t clearClaimed() const noexcept { uint64_t n = 0; for (uint64_t v : clearSize) n += v; return n; }
    uint64_t lifeCount(End how) const noexcept { uint64_t n = 0; for (uint64_t v : life[static_cast<unsigned>(how)]) n += v; return n; }
    bool empty() const noexcept {
        return !emissions && !flips && !liveTicks && !mergeClaimed() && !clearClaimed() && !lifeCount(End::Consumed) && !lifeCount(End::Cleared) &&
               !lifeCount(End::Dropped);
    }
};
// Takes and empties the window. Under primaryCopy::g_mutex (the samples). `liveNow`: the published emissions flag, so a claim set live across the
// window's end is counted up to `now` and the next window starts there.
inline Window take(int64_t now, bool liveNow) noexcept {
    Window w;
    const int64_t start = g_windowStart.exchange(now, std::memory_order_relaxed);
    w.windowTicks = start ? now - start : 0;
    w.liveTicks = g_liveTicks.exchange(0, std::memory_order_relaxed);
    if (liveNow) {
        const int64_t since = g_liveSince.load(std::memory_order_relaxed);
        if (since && now > since) w.liveTicks += now - since;
        g_liveSince.store(now, std::memory_order_relaxed);
    }
    w.flips = g_flips.exchange(0, std::memory_order_relaxed);
    w.emissions = g_emissions.exchange(0, std::memory_order_relaxed);
    w.bursts = g_bursts.exchange(0, std::memory_order_relaxed);
    for (unsigned h = 0; h < static_cast<unsigned>(End::Count); ++h)
        for (unsigned b = 0; b < kLifeBuckets; ++b) w.life[h][b] = g_life[h][b].exchange(0, std::memory_order_relaxed);
    for (unsigned i = 0; i < kSizeBuckets; ++i) {
        w.mergeSize[i] = g_mergeSize[i].exchange(0, std::memory_order_relaxed);
        w.clearSize[i] = g_clearSize[i].exchange(0, std::memory_order_relaxed);
    }
    w.mergeHits = g_mergeHits.exchange(0, std::memory_order_relaxed);
    w.clearHits = g_clearHits.exchange(0, std::memory_order_relaxed);
    w.sampleCount = g_sampleCount;
    for (unsigned i = 0; i < g_sampleCount; ++i) w.samples[i] = g_samples[i];
    g_sampleCount = 0;   // the burst in progress goes on filling a fresh array (g_burstLeft is kept)
    return w;
}
inline void resetForRig() noexcept {
    g_liveSince = 0; g_liveTicks = 0; g_windowStart = 0;
    g_flips = 0; g_emissions = 0; g_bursts = 0;
    for (auto& row : g_life) for (auto& v : row) v = 0;
    for (auto& v : g_mergeSize) v = 0;
    for (auto& v : g_clearSize) v = 0;
    g_mergeHits = 0; g_clearHits = 0;
    g_sampleCount = 0; g_burstLeft = 0; g_lastEmissionAt = 0; g_burstStart = 0; g_burstQuiet = 0;
    g_quietTicks = 0;
}

// ---- the text ------------------------------------------------------------------------------------------------------------
constexpr size_t kClaimLineBytes = 1100;
// A percentile of one end's lifetime histogram, as the UPPER edge of its power-of-two bucket in microseconds (so within a factor of two, never
// below the truth); "-" for an end that did not happen.
inline void percentileText(const uint64_t (&hist)[kLifeBuckets], double fraction, double perUs, char (&out)[24]) noexcept {
    uint64_t total = 0;
    for (uint64_t v : hist) total += v;
    if (!total) { std::snprintf(out, sizeof(out), "-"); return; }
    uint64_t want = static_cast<uint64_t>(fraction * static_cast<double>(total) + 0.999999);
    if (want < 1) want = 1;
    uint64_t seen = 0;
    for (unsigned b = 0; b < kLifeBuckets; ++b) {
        seen += hist[b];
        if (seen >= want) {
            const double ticks = static_cast<double>(uint64_t(2) << b);   // 2^(b+1)
            std::snprintf(out, sizeof(out), "%.0f", perUs > 0 ? ticks / perUs : ticks);
            return;
        }
    }
    std::snprintf(out, sizeof(out), "-");
}
// engine motion: primary copy claims (30 s): ... The "no claims this window" form when nothing happened.
inline void formatClaimLine(const Window& w, double perUs, double seconds, double frames, char (&out)[kClaimLineBytes]) noexcept {
    size_t used = 0;
    auto put = [&](const char* format, auto... args) {
        if (used + 1 >= kClaimLineBytes) return;
        const int n = std::snprintf(out + used, kClaimLineBytes - used, format, args...);
        if (n > 0) used = (used + static_cast<size_t>(n) < kClaimLineBytes) ? used + static_cast<size_t>(n) : kClaimLineBytes - 1;
    };
    if (w.empty()) {
        put("engine motion: primary copy claims (%.0f s): no claims this window (0 emissions, the claim set never live, 0 claimed merges, 0 claimed clears).", seconds);
        out[used] = 0;
        return;
    }
    const double live = w.windowTicks > 0 ? 100.0 * static_cast<double>(w.liveTicks) / static_cast<double>(w.windowTicks) : 0.0;
    char perFlip[24] = "-", perEmission[24] = "-";
    if (frames > 0) {
        std::snprintf(perFlip, sizeof(perFlip), "%.2f", static_cast<double>(w.flips) / frames);
        std::snprintf(perEmission, sizeof(perEmission), "%.2f", static_cast<double>(w.emissions) / frames);
    }
    put("engine motion: primary copy claims (%.0f s): the claim set was non-empty %.1f%% of the window (%llu times it became so, %s a frame); "
        "%llu emissions (%s a frame); claim lifetime in us, the upper edge of a power-of-two bucket:",
        seconds, live, static_cast<unsigned long long>(w.flips), perFlip, static_cast<unsigned long long>(w.emissions), perEmission);
    for (unsigned h = 0; h < static_cast<unsigned>(End::Count); ++h) {
        char p50[24], p95[24];
        percentileText(w.life[h], 0.50, perUs, p50);
        percentileText(w.life[h], 0.95, perUs, p95);
        put("%s %s p50 %s p95 %s (n %llu)", h ? ";" : "", kEndNames[h], p50, p95, static_cast<unsigned long long>(w.lifeCount(static_cast<End>(h))));
    }
    const uint64_t merges = w.mergeClaimed(), clears = w.clearClaimed();
    put("; claim-set size at claimed merges 1/2/3-4/5+ %llu/%llu/%llu/%llu, at claimed clears %llu/%llu/%llu/%llu",
        static_cast<unsigned long long>(w.mergeSize[0]), static_cast<unsigned long long>(w.mergeSize[1]), static_cast<unsigned long long>(w.mergeSize[2]),
        static_cast<unsigned long long>(w.mergeSize[3]), static_cast<unsigned long long>(w.clearSize[0]), static_cast<unsigned long long>(w.clearSize[1]),
        static_cast<unsigned long long>(w.clearSize[2]), static_cast<unsigned long long>(w.clearSize[3]));
    char mergeShare[24] = "-", clearShare[24] = "-", zero[24] = "-";
    if (merges) std::snprintf(mergeShare, sizeof(mergeShare), "%.1f%%", 100.0 * static_cast<double>(w.mergeHits) / static_cast<double>(merges));
    if (clears) std::snprintf(clearShare, sizeof(clearShare), "%.1f%%", 100.0 * static_cast<double>(w.clearHits) / static_cast<double>(clears));
    if (merges + clears)
        std::snprintf(zero, sizeof(zero), "%.2f%%", 100.0 - 100.0 * static_cast<double>(w.mergeHits + w.clearHits) / static_cast<double>(merges + clears));
    put("; claimed merges that touched a claim %llu of %llu (%s), claimed clears that cleared one %llu of %llu (%s); zero-hit share of claimed calls %s",
        static_cast<unsigned long long>(w.mergeHits), static_cast<unsigned long long>(merges), mergeShare, static_cast<unsigned long long>(w.clearHits),
        static_cast<unsigned long long>(clears), clearShare, zero);
    put(".");
    out[used] = 0;
}
// engine motion: primary copy claim entities (30 s): the first emissions after a quiet period.
inline void formatEntityLine(const Window& w, double perUs, double seconds, char (&out)[kClaimLineBytes]) noexcept {
    size_t used = 0;
    auto put = [&](const char* format, auto... args) {
        if (used + 1 >= kClaimLineBytes) return;
        const int n = std::snprintf(out + used, kClaimLineBytes - used, format, args...);
        if (n > 0) used = (used + static_cast<size_t>(n) < kClaimLineBytes) ? used + static_cast<size_t>(n) : kClaimLineBytes - 1;
    };
    if (!w.bursts || !w.sampleCount) {
        put("engine motion: primary copy claim entities (%.0f s): no claim began after a quiet period this window (a quiet period is %u s without an emission).",
            seconds, kQuietSeconds);
        out[used] = 0;
        return;
    }
    put("engine motion: primary copy claim entities (%.0f s): %llu burst%s after %u s quiet this window, the last one's first %u emissions "
        "(frame, ms after the first, source address, pool slot it was copied into (? = not yet), native words 0 1, pose words 2-6, marker, word 77):",
        seconds, static_cast<unsigned long long>(w.bursts), w.bursts == 1 ? "" : "s", kQuietSeconds, w.sampleCount);
    for (unsigned i = 0; i < w.sampleCount; ++i) {
        const Sample& s = w.samples[i];
        char slot[16] = "?";
        if (s.slot >= 0) std::snprintf(slot, sizeof(slot), "%d", s.slot);
        put("%s #%u f=%u +%.1f ms src=0x%llX slot=%s w=%08X,%08X pose=%08X,%08X,%08X,%08X,%08X m=%08X w77=%08X", i ? ";" : "", i + 1, s.frame,
            perUs > 0 ? static_cast<double>(s.at) / perUs / 1000.0 : 0.0, static_cast<unsigned long long>(s.source), slot, s.words[0], s.words[1],
            s.words[2], s.words[3], s.words[4], s.words[5], s.words[6], s.words[7], s.words[8]);
    }
    put(".");
    out[used] = 0;
}

}  // namespace claimmeter
}  // namespace edvr
