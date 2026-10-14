// GPU busy time from the operating system (2026-10-14): the arithmetic, the board that carries the answer to the
// overlay, and the log text. No Windows calls here, so tools\aa_off_test holds every rule below without a GPU.
//
// WHY. The overlay's GPU figure was the frame GPU work span (first command to the door end). On a GPU with slack that
// span includes every wait for the CPU to submit; the pass profiler's 2026-10-14 delay run put the same frame at
// 11.6 ms with the waits and 5.0 ms without. fpsVR and Task Manager read busy time. D3D11 has no way to ask the GPU for
// it, and OpenXR has no GPU timing at all, so the answer comes from the kernel's own books: D3DKMTQueryStatistics
// reports, per process and per hardware node, the time that node spent running this process's work (src\d3d11\
// gpu_busy_os.cpp reads it once a second on its own thread).
//
// THE MATHS. Each reading carries, per node, the cumulative running time in 100 ns units (measured 2026-10-14 on an
// RTX 5090: 2.05 s of running time for 2.00 s of GPU work). Two readings give, per node, running time over wall time =
// that node's utilisation. The figure is the BUSIEST COUNTED NODE, not the sum. Counted nodes are the 3D engines
// (on NVIDIA and AMD that node runs graphics and the compute queue; copy, video and the vendor's other engines are
// logged but not counted): parallel engines overlap, so a sum would count the same wall time twice, and the maximum
// over engines is what Task Manager shows as a process's GPU. busy ms per frame = running time over the window /
// frames over the window = utilisation x the window's mean frame time. "Of the period" in the log line is that
// utilisation.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr {

constexpr unsigned kGpuBusyMaxNodes = 32;           // an RTX 5090 reports 17
constexpr uint64_t kGpuBusyUnitNs = 100;            // RunningTime's unit, measured
constexpr uint64_t kGpuBusyMinWallUs = 250000;      // a window shorter than this is noise: keep accumulating
constexpr uint64_t kGpuBusyMaxAgeMs = 3000;         // the board's figure goes stale after three sample periods
constexpr double kGpuBusyClampAt = 1.5;             // a node over 150% is garbage, not skew: the window is dropped

// DXGK_ENGINE_TYPE (d3dkmdt.h): 3D is 1. Only the 3D engines are counted.
inline bool gpuBusyEngineCounts(uint32_t engineType) { return engineType == 1; }
inline const char* gpuBusyEngineName(uint32_t t) {
    switch (t) {
        case 0: return "Other";
        case 1: return "3D";
        case 2: return "VideoDecode";
        case 3: return "VideoEncode";
        case 4: return "VideoProcessing";
        case 5: return "SceneAssembly";
        case 6: return "Copy";
        case 7: return "Overlay";
        case 8: return "Crypto";
        default: return "Engine";
    }
}

// Why there is no figure. Logged once, by name; the overlay shows "--" for any of them.
enum class GpuBusyFail : uint8_t {
    None = 0,
    NotStarted,      // no frame has opened the sampler yet
    NoProcs,         // gdi32 has no D3DKMTQueryStatistics (Wine, DXVK)
    NoAdapter,       // the game's adapter could not be opened by LUID
    NoStatistics,    // the adapter's statistics query was refused
    NoNodes,         // the adapter reports no readable node
    ReadFailed,      // the node reads failed three times running
    BadReadings      // the readings stayed unusable (counters going backwards, over 150%)
};
inline const char* gpuBusyFailText(GpuBusyFail f) {
    switch (f) {
        case GpuBusyFail::None: return "none";
        case GpuBusyFail::NotStarted: return "not started yet";
        case GpuBusyFail::NoProcs: return "D3DKMTQueryStatistics is not available on this system";
        case GpuBusyFail::NoAdapter: return "the game's adapter could not be opened by LUID";
        case GpuBusyFail::NoStatistics: return "the adapter's statistics query was refused";
        case GpuBusyFail::NoNodes: return "the adapter reports no readable node";
        case GpuBusyFail::ReadFailed: return "the per-process node reads failed three times running";
        case GpuBusyFail::BadReadings: return "the per-process node counters were unusable";
    }
    return "unknown";
}

// One window between two readings.
struct GpuBusyWindow {
    bool valid = false;
    double busyMsPerFrame = 0.0;    // the busiest counted node's running time over the window / frames
    double utilization = 0.0;       // that node's running time / wall time, 0..1
    unsigned busiestNode = 0;
    double wallMs = 0.0;
    double frames = 0.0;
    bool clamped = false;           // a node read a little over 100% and was held at 100%
    unsigned nodeCount = 0;
    double nodeUtil[kGpuBusyMaxNodes] = {};   // every node, counted or not
};

// Two readings to a window. Feed it a reading about once a second; it keeps the previous one.
class GpuBusyAccumulator {
public:
    enum class Result { Baseline, Window, Dropped };

    void reset() { have_ = false; n_ = 0; }

    // wallUs: a monotonic clock in microseconds. frames: the render loop's frame count, monotonic. running[i]: node i's
    // cumulative running time in 100 ns units. usable[i]: node i was readable this time. counted[i]: it counts.
    Result feed(uint64_t wallUs, uint64_t frames, const uint64_t* running, const bool* usable, const bool* counted, unsigned n,
                GpuBusyWindow* out) {
        if (out) *out = GpuBusyWindow{};
        if (!running || !usable || !counted || n == 0 || n > kGpuBusyMaxNodes) { reset(); return Result::Dropped; }
        if (!have_ || n != n_) { store(wallUs, frames, running, n); return Result::Baseline; }
        if (wallUs < wallUs_ || frames < frames_) { store(wallUs, frames, running, n); return Result::Dropped; }
        const uint64_t dWall = wallUs - wallUs_;
        if (dWall < kGpuBusyMinWallUs) return Result::Dropped;   // too soon: the baseline stays, the next reading spans more
        const uint64_t dFrames = frames - frames_;
        GpuBusyWindow w;
        w.nodeCount = n;
        w.wallMs = static_cast<double>(dWall) / 1000.0;
        w.frames = static_cast<double>(dFrames);
        bool sane = dFrames > 0;   // a window with no frame has no per-frame figure (a pause, a loading screen)
        double best = -1.0;
        unsigned bestNode = 0;
        for (unsigned i = 0; i < n && sane; ++i) {
            if (!usable[i]) continue;
            if (running[i] < run_[i]) { sane = false; break; }   // a counter that went backwards: a reset, start over
            const double ns = static_cast<double>(running[i] - run_[i]) * static_cast<double>(kGpuBusyUnitNs);
            double util = ns / (static_cast<double>(dWall) * 1000.0);
            if (util > kGpuBusyClampAt) { sane = false; break; }
            if (util > 1.0) { util = 1.0; if (counted[i]) w.clamped = true; }
            w.nodeUtil[i] = util;
            if (counted[i] && util > best) { best = util; bestNode = i; }
        }
        store(wallUs, frames, running, n);
        if (!sane || best < 0.0) return Result::Dropped;
        w.valid = true;
        w.utilization = best;
        w.busiestNode = bestNode;
        w.busyMsPerFrame = best * w.wallMs / w.frames;
        if (out) *out = w;
        return Result::Window;
    }

private:
    void store(uint64_t wallUs, uint64_t frames, const uint64_t* running, unsigned n) {
        wallUs_ = wallUs;
        frames_ = frames;
        n_ = n;
        for (unsigned i = 0; i < n; ++i) run_[i] = running[i];
        have_ = true;
    }
    bool have_ = false;
    unsigned n_ = 0;
    uint64_t wallUs_ = 0, frames_ = 0;
    uint64_t run_[kGpuBusyMaxNodes] = {};
};

// What the overlay and the tile read.
struct GpuBusyReading {
    bool have = false;              // a figure no older than kGpuBusyMaxAgeMs
    double busyMs = 0.0;            // per frame
    double percent = 0.0;           // of the frame period
    GpuBusyFail failure = GpuBusyFail::NotStarted;
};

// The worker publishes here, the overlay reads: one packed word and a time, so a reader never sees a half figure that
// matters (a torn pair of word and time is one sample old at worst).
class GpuBusyBoard {
public:
    void publish(const GpuBusyWindow& w, uint64_t nowMs) {
        if (!w.valid) return;
        double us = w.busyMsPerFrame * 1000.0;
        if (us < 0.0) us = 0.0;
        if (us > 4294967295.0) us = 4294967295.0;
        double pm = w.utilization * 1000.0;
        if (pm < 0.0) pm = 0.0;
        if (pm > 32767.0) pm = 32767.0;
        const uint64_t packed = (1ull << 63) | (static_cast<uint64_t>(pm + 0.5) << 32) | static_cast<uint64_t>(us + 0.5);
        packed_.store(packed, std::memory_order_relaxed);
        atMs_.store(nowMs, std::memory_order_relaxed);
        failure_.store(static_cast<uint8_t>(GpuBusyFail::None), std::memory_order_relaxed);
    }
    void fail(GpuBusyFail f) {
        packed_.store(0, std::memory_order_relaxed);
        failure_.store(static_cast<uint8_t>(f), std::memory_order_relaxed);
    }
    GpuBusyReading read(uint64_t nowMs) const {
        GpuBusyReading r;
        const uint64_t packed = packed_.load(std::memory_order_relaxed);
        const uint64_t at = atMs_.load(std::memory_order_relaxed);
        r.failure = static_cast<GpuBusyFail>(failure_.load(std::memory_order_relaxed));
        if (!(packed >> 63)) return r;
        if (nowMs < at || nowMs - at > kGpuBusyMaxAgeMs) {
            if (r.failure == GpuBusyFail::None) r.failure = GpuBusyFail::NotStarted;   // had one, now stale: no sample lately
            return r;
        }
        r.have = true;
        r.busyMs = static_cast<double>(packed & 0xFFFFFFFFull) / 1000.0;
        r.percent = static_cast<double>((packed >> 32) & 0x7FFFull) / 10.0;
        return r;
    }
    void reset() { packed_.store(0); atMs_.store(0); failure_.store(static_cast<uint8_t>(GpuBusyFail::NotStarted)); }

private:
    std::atomic<uint64_t> packed_{0};
    std::atomic<uint64_t> atMs_{0};
    std::atomic<uint8_t> failure_{static_cast<uint8_t>(GpuBusyFail::NotStarted)};
};

// The 30 s log line's accumulation: per-second windows, weighted so the mean is the true mean over the interval.
struct GpuBusyLogWindow {
    double busyMsSum = 0.0;         // sum of (busy ms per frame x frames)
    double frames = 0.0;
    double wallMs = 0.0;
    double busyTimeMs = 0.0;        // sum of (utilisation x wall) of the busiest counted node of each window
    unsigned windows = 0;
    unsigned nodeCount = 0;
    double nodeBusyMs[kGpuBusyMaxNodes] = {};   // per node, utilisation x wall summed

    void add(const GpuBusyWindow& w) {
        if (!w.valid) return;
        busyMsSum += w.busyMsPerFrame * w.frames;
        frames += w.frames;
        wallMs += w.wallMs;
        busyTimeMs += w.utilization * w.wallMs;
        ++windows;
        nodeCount = w.nodeCount;
        for (unsigned i = 0; i < w.nodeCount; ++i) nodeBusyMs[i] += w.nodeUtil[i] * w.wallMs;
    }
    double busyMsPerFrame() const { return frames > 0.0 ? busyMsSum / frames : 0.0; }
    double percent() const { return wallMs > 0.0 ? 100.0 * busyTimeMs / wallMs : 0.0; }
    double nodePercent(unsigned i) const { return wallMs > 0.0 && i < nodeCount ? 100.0 * nodeBusyMs[i] / wallMs : 0.0; }
};

// What the sampler knows about each node, for the log text.
struct GpuBusyNodeInfo {
    uint32_t id = 0;
    uint32_t engineType = 0;
    bool usable = false;
    bool counted = false;
    char name[24] = {};
};

inline const char* gpuBusyNodeLabel(const GpuBusyNodeInfo& n) {
    return n.name[0] ? n.name : gpuBusyEngineName(n.engineType);
}

// The 30 s line. `spanMs` < 0 means no span in the interval. The node list names every node that is counted or that
// ran at least 0.1% of the interval; `*` marks the busiest counted node, and a node of an uncounted engine is tagged
// "(not counted)". tools\edvr_log.py does not parse this line yet; the next flight reads it by eye against the pass
// profiler's "frame busy".
inline void formatGpuBusyLogLine(char* buf, size_t len, const GpuBusyLogWindow& w, const GpuBusyNodeInfo* nodes, unsigned nodeCount,
                                 unsigned busiestNode, double spanMs) {
    if (!buf || !len) return;
    buf[0] = 0;
    char span[32] = "-";
    if (spanMs >= 0.0) std::snprintf(span, sizeof(span), "%.3f", spanMs);
    char list[512] = "";
    size_t used = 0;
    for (unsigned i = 0; i < nodeCount && i < kGpuBusyMaxNodes && nodes; ++i) {
        const double pct = w.nodePercent(i);
        if (!nodes[i].counted && pct < 0.1) continue;
        const int k = std::snprintf(list + used, sizeof(list) - used, "%s%u:%s=%.1f%%%s%s", used ? "," : "", nodes[i].id,
                                    gpuBusyNodeLabel(nodes[i]), pct, nodes[i].counted && i == busiestNode ? "*" : "",
                                    nodes[i].counted ? "" : "(not counted)");
        if (k < 0 || static_cast<size_t>(k) >= sizeof(list) - used) break;
        used += static_cast<size_t>(k);
    }
    std::snprintf(buf, len, "gpu busy (os): %.3f ms/frame (%.1f%% of the period), span %s ms, source=d3dkmt nodes=%s",
                  w.busyMsPerFrame(), w.percent(), span, list[0] ? list : "-");
}

// The one-time line naming the adapter and nodes the sampler opened.
inline void formatGpuBusyStartLine(char* buf, size_t len, const char* adapter, uint32_t luidHigh, uint32_t luidLow,
                                   const GpuBusyNodeInfo* nodes, unsigned nodeCount, unsigned sampleMs) {
    if (!buf || !len) return;
    char list[512] = "";
    size_t used = 0;
    for (unsigned i = 0; i < nodeCount && i < kGpuBusyMaxNodes && nodes; ++i) {
        const int k = std::snprintf(list + used, sizeof(list) - used, "%s%u:%s%s%s", used ? "," : "", nodes[i].id,
                                    gpuBusyNodeLabel(nodes[i]), nodes[i].counted ? "*" : "", nodes[i].usable ? "" : "(unreadable)");
        if (k < 0 || static_cast<size_t>(k) >= sizeof(list) - used) break;
        used += static_cast<size_t>(k);
    }
    std::snprintf(buf, len,
                  "gpu busy (os): adapter \"%s\" luid=%08X:%08X, nodes=%s (* = counted: the 3D engines; busy = the busiest counted node), "
                  "sampled every %u ms, source=d3dkmt",
                  adapter ? adapter : "?", luidHigh, luidLow, list[0] ? list : "-", sampleMs);
}

inline void formatGpuBusyFailLine(char* buf, size_t len, GpuBusyFail f) {
    if (!buf || !len) return;
    std::snprintf(buf, len, "gpu busy (os): unavailable (%s); the overlay shows \"--\" for GPU busy, source=d3dkmt", gpuBusyFailText(f));
}

}  // namespace edvr
