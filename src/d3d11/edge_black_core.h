// The edge-black watch, the pure part (docs\terrain-culling.md, 2026-10-10; TEMPORARY, an always-on instrument of this test build). No device, no log, no
// clock: the GPU half (edge_black.cpp) hands this one 64 x 64 grid of values per eye per frame, and every line it writes comes back through a sink, so
// tools\edge_black_test drives each case and holds tools\edge_black_fixture.log to exactly what this writes.
//
// THE QUESTION. Pilots see quick black flashes, square, at the outer corners of an eye over flat planet ground. Is the black IN the image Elite hands
// EDVR (a tile culled or generated late, in the game's own render), or is it added after EDVR submits (the runtime's reprojection, the compositor, the
// display)? This watches the game's own submitted texture, the stage the luma probe calls "game", before any EDVR pass touches it.
//
// WHAT IT KEEPS. Per eye, the last four frames' grids, each point as one of three states by the texture's own encoding of max(R, G, B):
//   black  below kBlackBelow (0.004: an 8-bit texture's code 0 or 1, 1/255 = 0.0039; a float texture's 0.004)
//   lit    above kLitAbove (0.03: code 8 or more of 255)
//   dim    between (neither; real dark scenery lives here, and it is what separates "black" from "merely dark")
// A point is TRANSIENT BLACK in frame f when it is black in f and lit before and after: lit, black, lit (one frame), or lit, black, black, lit (two).
// A flash a single frame long is what a missing tile would be; a hole that stays is a horizon or a dark object moving in, which is the control below.
// An EVENT is a frame-eye with at least kMinCluster transient-black points that touch (8-neighbourhood) in the grid: the largest such cluster is reported
// (points, bounding box in cells, centre as normalised x, y, a region label, and whether it touches the image border).
//
// LATENCY. Frame f's verdict needs frame f+2 (a two-frame run is only known to end then), so a frame is judged when the frame two after it arrives. A frame
// missing from the sequence (the sequence numbers are not consecutive) or a change of size or format breaks the history: frames before the break can no
// longer be judged, and are counted as lost, not guessed.
//
// CONTROL. "Black-start": a point black now, lit before and still black the frame after and not lit again after that (lit, black, black, not lit) -- a
// hole that opens and stays. A horizon, a dark object or a loading edge moving in makes these and no transients. A transient count with no black-start is a
// flash; both together is something moving.
#pragma once

#include "../common/temporal_math.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

namespace edvr {
namespace edgeblack {

constexpr int kGrid = 64;
constexpr int kPoints = kGrid * kGrid;
constexpr float kBlackBelow = 0.004f;
constexpr float kLitAbove = 0.03f;
constexpr int kMinCluster = 3;
constexpr unsigned kEventsListed = 60;
constexpr uint64_t kWindowMs = 60000;
constexpr int kRegions = 7;
constexpr unsigned kReadbackDepth = 4;   // frames of readback the GPU half keeps in flight (edge_black.cpp)

enum State : uint8_t { Dim = 0, Black = 1, Lit = 2 };

// A NaN is dim: it is not a black the watch can stand behind, and not a lit one.
inline State classify(float v) {
    if (!(v == v)) return Dim;
    return v < kBlackBelow ? Black : (v > kLitAbove ? Lit : Dim);
}

inline const char* regionName(int r) {
    static const char* const names[kRegions] = {"outer-bottom", "outer-middle", "outer-top", "inner-bottom", "inner-middle", "inner-top", "centre"};
    return r >= 0 && r < kRegions ? names[r] : "?";
}

// The head's turning speed between two frames from the runtime's two poses (device to world, row-major 3x4) and the time between them. NaN when there is no
// such speed to say: no previous pose, a gap of under half a millisecond (the same frame twice) or over half a second (a pause, a load).
inline float headSpeedDegPerSec(const float* prev34, const float* now34, double seconds) {
    if (!prev34 || !now34 || !(seconds > 0.0005) || seconds > 0.5) return std::numeric_limits<float>::quiet_NaN();
    float delta[9];
    temporalHeadDelta(prev34, now34, delta);
    return static_cast<float>(temporalRotationAngleDeg(delta) / seconds);
}

struct WallTime {
    unsigned hour = 0, minute = 0, second = 0, milli = 0;
};

struct FrameInfo {
    uint64_t seq = 0;
    WallTime wall;
    float headDegPerSec = std::numeric_limits<float>::quiet_NaN();   // NaN: no head pose was known for this frame
};

struct Cluster {
    int points = 0;
    int minX = 0, minY = 0, maxX = 0, maxY = 0;
    float cx = 0.0f, cy = 0.0f;   // the centre of the cluster's cells, normalised to the image (0,0 top left)
    bool border = false;          // some cell of it is on the grid's edge
};

// The region label of a cluster's centre in eye `eye`: eye 0's outer (temporal) side is x < 0.5, eye 1's is x > 0.5. Top, middle and bottom are thirds of the
// image. "centre" is the middle third within 0.1 of the vertical midline, where outer and inner are not a fair thing to say. Order: outer-bottom,
// outer-middle, outer-top, inner-bottom, inner-middle, inner-top, centre.
inline int regionOf(int eye, float cx, float cy) {
    const bool middle = cy >= 1.0f / 3.0f && cy < 2.0f / 3.0f;
    if (middle && std::fabs(cx - 0.5f) < 0.1f) return 6;
    const float outward = eye == 0 ? 0.5f - cx : cx - 0.5f;
    const int row = cy < 1.0f / 3.0f ? 2 : (cy < 2.0f / 3.0f ? 1 : 0);
    return outward > 0.0f ? row : 3 + row;
}

// The clusters of set cells in a 64 x 64 flag array (8-neighbourhood). Returns how many have at least `minPoints` cells, and the largest of all of them (the
// first found on a tie, reading the grid top to bottom, left to right) in *largest. No allocation: the stack is one entry a cell at most.
inline int findClusters(const uint8_t* flags, int minPoints, Cluster* largest) {
    uint8_t seen[kPoints];
    uint16_t stack[kPoints];
    std::memset(seen, 0, sizeof(seen));
    Cluster best;
    int count = 0;
    for (int start = 0; start < kPoints; ++start) {
        if (!flags[start] || seen[start]) continue;
        Cluster c;
        c.minX = kGrid; c.minY = kGrid; c.maxX = -1; c.maxY = -1;
        double sumX = 0.0, sumY = 0.0;
        int sp = 0;
        stack[sp++] = static_cast<uint16_t>(start);
        seen[start] = 1;
        while (sp > 0) {
            const int i = stack[--sp];
            const int x = i % kGrid, y = i / kGrid;
            ++c.points;
            sumX += x + 0.5;
            sumY += y + 0.5;
            if (x < c.minX) c.minX = x;
            if (x > c.maxX) c.maxX = x;
            if (y < c.minY) c.minY = y;
            if (y > c.maxY) c.maxY = y;
            if (x == 0 || y == 0 || x == kGrid - 1 || y == kGrid - 1) c.border = true;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    if (!dx && !dy) continue;
                    const int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= kGrid || ny >= kGrid) continue;
                    const int j = ny * kGrid + nx;
                    if (flags[j] && !seen[j]) { seen[j] = 1; stack[sp++] = static_cast<uint16_t>(j); }
                }
            }
        }
        c.cx = static_cast<float>(sumX / c.points / kGrid);
        c.cy = static_cast<float>(sumY / c.points / kGrid);
        if (c.points >= minPoints) ++count;
        if (c.points > best.points) best = c;
    }
    if (largest) *largest = best;
    return count;
}

// One eye's last four frames. push() adds a frame's grid and returns the verdict on the frame two before it, once there are four.
class EyeHistory {
public:
    struct Verdict {
        bool evaluated = false;
        FrameInfo info;                 // the frame judged
        unsigned transient = 0;         // its transient-black points
        unsigned blackStart = 0;        // its black-start points
        int clusters = 0;               // clusters of at least kMinCluster transient points
        Cluster largest;                // the largest cluster of transient points (points 0 when there are none)
    };

    // How many frames were dropped from the history by a break in the sequence (0 when the frame follows the last one).
    unsigned push(const float* values, const FrameInfo& info, Verdict* verdict) {
        *verdict = Verdict();
        unsigned dropped = 0;
        if (n_ > 0 && info.seq != slot(0).info.seq + 1) dropped = reset();
        newest_ = (newest_ + 1) % 4;
        Slot& d = slots_[newest_];
        d.info = info;
        for (int i = 0; i < kPoints; ++i) {
            d.state[i] = static_cast<uint8_t>(classify(values[i]));
            d.transient[i] = 0;
        }
        if (n_ < 4) ++n_;
        if (n_ >= 3) {
            Slot& c = slot(1);
            const Slot& b = slot(2);
            for (int i = 0; i < kPoints; ++i)
                if (b.state[i] == Lit && c.state[i] == Black && d.state[i] == Lit) c.transient[i] = 1;   // lit, black, lit
        }
        if (n_ >= 4) {
            Slot& c = slot(1);
            Slot& b = slot(2);
            const Slot& a = slot(3);
            for (int i = 0; i < kPoints; ++i)
                if (a.state[i] == Lit && b.state[i] == Black && c.state[i] == Black && d.state[i] == Lit) {   // lit, black, black, lit
                    b.transient[i] = 1;
                    c.transient[i] = 1;
                }
            verdict->evaluated = true;
            verdict->info = b.info;
            unsigned transient = 0, start = 0;
            for (int i = 0; i < kPoints; ++i) {
                transient += b.transient[i];
                if (a.state[i] == Lit && b.state[i] == Black && c.state[i] == Black && d.state[i] != Lit) ++start;
            }
            verdict->transient = transient;
            verdict->blackStart = start;
            if (transient) verdict->clusters = findClusters(b.transient, kMinCluster, &verdict->largest);
        }
        return dropped;
    }

    // Forget the history. Returns how many frames it held that were never judged and now never will be: the newest two when it held four (the one two
    // back was judged by the push that made it four), every frame it held when it held fewer (the oldest of three has nothing lit before it to be judged by).
    unsigned reset() {
        const unsigned lost = n_ >= 4 ? 2u : n_;
        n_ = 0;
        return lost;
    }

    unsigned held() const { return n_; }

private:
    struct Slot {
        FrameInfo info;
        uint8_t state[kPoints];
        uint8_t transient[kPoints];
    };
    Slot& slot(unsigned back) { return slots_[(newest_ + 4 - back) % 4]; }
    Slot slots_[4];
    unsigned newest_ = 0;
    unsigned n_ = 0;
};

// What a window (or the whole run) has counted.
struct Counts {
    uint64_t frames = 0, eyes = 0, events = 0, transient = 0, blackStart = 0, lost = 0;
    uint64_t region[kRegions] = {};
    unsigned largest = 0;
};

// The one line that says the watch is running, written when the first eye is sampled (the format is only known then): the stage, the grid, the metric, the
// texture's own encoding, the thresholds, the rule, the readback depth and what the log will carry.
inline void formatStart(char (&line)[640], const char* format, const char* encoding) {
    std::snprintf(line, sizeof(line),
                  "edge black: watching stage game (the texture Elite submits, before any EDVR pass): a %dx%d grid per eye each frame, the largest of R, G, B at each point "
                  "through a plain view of %s (%s, the texture's own encoding); black below %.3f, lit above %.2f; an event is %d or more touching transient-black points "
                  "(lit, black, lit; or lit, black, black, lit); readback %u frames deep; the first %u events are listed, a summary every %u s.",
                  kGrid, kGrid, format, encoding, static_cast<double>(kBlackBelow), static_cast<double>(kLitAbove), kMinCluster, kReadbackDepth, kEventsListed,
                  static_cast<unsigned>(kWindowMs / 1000));
}

inline void formatEvent(char (&line)[384], int eye, const FrameInfo& info, const Cluster& c, int region, int clusters) {
    char head[64];
    if (info.headDegPerSec == info.headDegPerSec) std::snprintf(head, sizeof(head), "%.0f deg/s", static_cast<double>(info.headDegPerSec));
    else std::snprintf(head, sizeof(head), "unknown (no head pose known to the graphics half)");
    char extra[32] = "";
    if (clusters > 1) std::snprintf(extra, sizeof(extra), " clusters=%d", clusters);
    std::snprintf(line, sizeof(line), "edge black: event eye=%d frame=%llu t=%02u:%02u:%02u.%03u points=%d cells=%dx%d centre=%.2f,%.2f region=%s border=%d head=%s%s",
                  eye, static_cast<unsigned long long>(info.seq), info.wall.hour, info.wall.minute, info.wall.second, info.wall.milli, c.points,
                  c.maxX - c.minX + 1, c.maxY - c.minY + 1, static_cast<double>(c.cx), static_cast<double>(c.cy), regionName(region), c.border ? 1 : 0, head, extra);
}

// The counts, as the summary and the closing line both say them: `head` is what comes before ("edge black: 60 s:", "edge black: closed -- totals for the run:").
inline void formatCounts(char (&line)[512], const char* head, const Counts& w) {
    const auto u = [](uint64_t v) { return static_cast<unsigned long long>(v); };
    std::snprintf(line, sizeof(line),
                  "%s frames %llu eyes-sampled %llu events %llu (outer-bottom %llu, outer-middle %llu, outer-top %llu, inner-bottom %llu, inner-middle %llu, "
                  "inner-top %llu, centre %llu) largest %u points; transient points total %llu; black-start points (black now, lit before, still black) %llu; samples lost %llu",
                  head, u(w.frames), u(w.eyes), u(w.events), u(w.region[0]), u(w.region[1]), u(w.region[2]), u(w.region[3]), u(w.region[4]), u(w.region[5]),
                  u(w.region[6]), w.largest, u(w.transient), u(w.blackStart), u(w.lost));
}

// The per-minute summary, written for every window the watch ran, events or none. `seconds` is the window's length (60, less for the last window of a run).
inline void formatSummary(char (&line)[512], unsigned seconds, const Counts& w) {
    char head[48];
    std::snprintf(head, sizeof(head), "edge black: %u s:", seconds);
    formatCounts(line, head, w);
}

// The whole watch: two eyes, the window, the event lines (the first kEventsListed), the totals. Owner: one thread (Elite's render thread).
class Watch {
public:
    // One eye's grid for one frame, read back. `nowMs` is a monotonic clock in milliseconds; every line goes to sink(const char*).
    template <class Sink>
    void ingest(int eye, const float* values, const FrameInfo& info, uint64_t nowMs, Sink&& sink) {
        if (eye < 0 || eye > 1) return;
        if (!open_) { open_ = true; windowStartMs_ = nowMs; }
        ++window_.eyes;
        if (!haveSeq_ || info.seq != lastSeq_) { ++window_.frames; lastSeq_ = info.seq; haveSeq_ = true; }
        EyeHistory::Verdict v;
        window_.lost += eyes_[eye].push(values, info, &v);
        if (v.evaluated) {
            window_.transient += v.transient;
            window_.blackStart += v.blackStart;
            if (v.clusters > 0) {
                const int region = regionOf(eye, v.largest.cx, v.largest.cy);
                ++window_.events;
                ++window_.region[region];
                if (static_cast<unsigned>(v.largest.points) > window_.largest) window_.largest = static_cast<unsigned>(v.largest.points);
                if (listed_ < kEventsListed) {
                    ++listed_;
                    char line[384];
                    formatEvent(line, eye, v.info, v.largest, region, v.clusters);
                    sink(static_cast<const char*>(line));
                    if (listed_ == kEventsListed)
                        sink("edge black: 60 events listed; further events are counted in the summaries only.");
                }
            }
        }
        if (nowMs - windowStartMs_ >= kWindowMs) writeWindow(nowMs, sink);
    }

    // The eye's size or format changed, or a readback was lost: its history is useless. The frames it held are counted lost.
    void resetEye(int eye) {
        if (eye < 0 || eye > 1) return;
        window_.lost += eyes_[eye].reset();
    }
    // Samples that never reached the watch (a readback ring that was full, one that never completed).
    void noteLost(uint64_t n) { window_.lost += n; }

    // The end of the run: the open window, then the totals.
    template <class Sink>
    void close(uint64_t nowMs, Sink&& sink) {
        if (open_) writeWindow(nowMs, sink);
        char line[512];
        formatCounts(line, "edge black: closed -- totals for the run:", total_);
        sink(static_cast<const char*>(line));
    }

    const Counts& window() const { return window_; }
    const Counts& total() const { return total_; }
    unsigned listed() const { return listed_; }
    bool open() const { return open_; }

private:
    template <class Sink>
    void writeWindow(uint64_t nowMs, Sink&& sink) {
        const uint64_t ms = nowMs >= windowStartMs_ ? nowMs - windowStartMs_ : 0;
        unsigned seconds = static_cast<unsigned>((ms + 500) / 1000);
        char line[512];
        formatSummary(line, seconds, window_);
        sink(static_cast<const char*>(line));
        total_.frames += window_.frames;
        total_.eyes += window_.eyes;
        total_.events += window_.events;
        total_.transient += window_.transient;
        total_.blackStart += window_.blackStart;
        total_.lost += window_.lost;
        for (int i = 0; i < kRegions; ++i) total_.region[i] += window_.region[i];
        if (window_.largest > total_.largest) total_.largest = window_.largest;
        window_ = Counts();
        windowStartMs_ = nowMs;
        open_ = false;
    }

    EyeHistory eyes_[2];
    Counts window_, total_;
    bool open_ = false, haveSeq_ = false;
    uint64_t windowStartMs_ = 0, lastSeq_ = 0;
    unsigned listed_ = 0;
};

}  // namespace edgeblack
}  // namespace edvr
