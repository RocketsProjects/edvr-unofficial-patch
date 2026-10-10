#pragma once

// The pose-arrival diagnostic (docs\terrain-culling.md, 2026-10-10; TEMPORARY with advanced.cull_pose_timing). Always on, in all three timings, cheap:
// no allocation, no lock, owner thread only (the same rule as pose_slot.h, which it sits beside).
//
// WHAT IT MEASURES. Elite's game thread asks for "now" on another thread; the call is served on the owner thread, which may be blocked in the frame wait
// for the next frame when the call arrives, so the call is served AFTER that frame is published and answered relative to it. Whether a call is served
// before or after the next publish depends on what the owner was doing when Elite asked, not on anything in the pose: the same place in Elite's
// pipeline gets answers a period apart (D(N)+P if served before the publish of N+1, D(N+1)+P ~= D(N)+2P if after). The `pose slot:` line classifies a
// call against the next publish after it was SERVED, so it calls both "match" by construction. This classifies by what was true when it ARRIVED
// (head_pose_time.h PoseArrival: Between / Straddled / Render / Other, from publishSeq and submitSeq read on the calling thread) and compares the
// instant it was located at with the FIRST FRAME PUBLISHED AFTER IT ARRIVED: for a call that straddled a publish, the frame published during its queue
// (the one cached when it was served); for the others, the next publish (the same resolver as pose_slot.h).
//   delta = T - D(first frame after arrival), in periods of that frame, to the nearest: 0p, +1p, +2p+ (two or more late), -1p- (a period or more early).
//   angle = the angle between the orientation the call returned and the pose that frame is drawn with.
// Under `next` an unqueued Between call reads 0p and a Straddled one +1p; under `arrival` both read +1p (every between-frames call is answered a period
// past the first frame published after it arrived) and Render reads 0p. Calls that were not answered at an instant of their own (fallbacks) are
// counted in their class and in `fallback`, and left out of the buckets and the angles (they are located at now and say nothing of the timing).
// calls = between + straddled + render + other; the buckets of all classes + fallback + unresolved = calls.
//
// One line per calling thread per 60 s window of one timing, next to the `pose slot:` line, written from the WaitGetPoses publish point:
//   pose arrival: tid T mode next|slot|arrival calls N between B straddled S render R other O fallback F unresolved U; queue ms between m/x, straddled m/x,
//   render m/x; vs first frame after arrival: between (0p a, +1p b, +2p+ c, -1p- d), straddled (...), render (...), other (...); >1deg between x straddled y
//   render z other w; >2deg ...; max X deg (class); late frames n; window S s
// `queue ms` is the time between the call's arrival and its service (mean/max over the class; n/a when the clock could not be read).
//
// Pure: the clock, the late-frame count and the sink are passed in, so tools\openxr_pose_test drives every case and holds tools\pose_arrival_fixture.log
// to exactly what this writes (tools\edvr_log.py --tally pose reads it).
#include "head_pose_time.h"
#include "pose_gap.h"
#include "pose_slot.h"

#include <cstdint>
#include <cstdio>

namespace edvr::openxr {

constexpr unsigned kPoseArrivalPending = 256;
constexpr unsigned kPoseArrivalThreads = 8;

// How far from the first frame after arrival a call was located, in periods of that frame, to the nearest.
enum PoseArrivalBucket : unsigned { kBucket0p = 0, kBucketPlus1p, kBucketPlus2p, kBucketMinus1p, kBuckets };

inline const char* poseArrivalBucketName(unsigned b) {
  static const char* const names[kBuckets] = {"0p", "+1p", "+2p+", "-1p-"};
  return b < kBuckets ? names[b] : "?";
}

// delta = target - frameDisplay; the bucket is round(delta / period) to the nearest period (half a period rounds up): 0p for -P/2 <= delta < P/2, +1p for
// P/2 <= delta < 3P/2, +2p+ beyond, -1p- for anything at or below -P/2 - 1. False when it cannot be told (no usable period, or the subtraction would overflow).
inline bool poseArrivalBucketOf(int64_t target, int64_t frameDisplay, int64_t period, unsigned* bucket) {
  if (period <= 0 || period > INT64_MAX / 4) return false;
  if ((frameDisplay > 0 && target < INT64_MIN + frameDisplay) || (frameDisplay < 0 && target > INT64_MAX + frameDisplay)) return false;
  const int64_t delta = target - frameDisplay;
  if (delta > INT64_MAX / 4) { *bucket = kBucketPlus2p; return true; }
  if (delta < -(INT64_MAX / 4)) { *bucket = kBucketMinus1p; return true; }
  const int64_t num = 2 * delta + period, den = 2 * period;
  int64_t q = num / den;
  if (num % den != 0 && num < 0) --q;                              // floor
  *bucket = q == 0 ? kBucket0p : q == 1 ? kBucketPlus1p : q >= 2 ? kBucketPlus2p : kBucketMinus1p;
  return true;
}

struct PoseArrivalCounts {
  uint64_t calls = 0, fallback = 0, unresolved = 0;
  uint64_t cls[kPoseArrivals]{};
  uint64_t queueN[3]{};                       // between, straddled, render (other is not timed)
  double queueSum[3]{}, queueMax[3]{};        // ms
  uint64_t bucket[kPoseArrivals][kBuckets]{};
  uint64_t over1[kPoseArrivals]{}, over2[kPoseArrivals]{};   // answered calls of each class whose angle was above 1 and above 2 degrees
  double maxAngle = 0;
  unsigned maxClass = 0;
  bool maxKnown = false;
};

// The frame a call that straddled a publish is compared with at once: the one cached when it was served.
struct PoseArrivalFrame {
  int64_t display = 0, period = 0;
  const float* q = nullptr;                   // x, y, z, w; null when its pose was not valid
};

class PoseArrivalStats {
 public:
  PoseTiming mode() const { return mode_; }

  // One call the fix handled. `queueMs` is arrival to service (negative: not known). `at` is the instant it was located at, `answered` whether that was an
  // instant of its own, `q` the orientation it returned (null when the pose was not valid). A Straddled or Other call is compared with `served` (the frame
  // cached when it was served) at once; a Between or Render call waits for the next publish.
  void noteCall(uint32_t thread, PoseArrival arrival, double queueMs, int64_t at, bool answered, const float* q, const PoseArrivalFrame* served) {
    PoseArrivalCounts* key = keyFor(thread);
    if (!key) { ++droppedCalls_; return; }
    const unsigned c = unsigned(arrival);
    ++key->calls;
    ++key->cls[c];
    if (c < 3 && queueMs >= 0.0) { ++key->queueN[c]; key->queueSum[c] += queueMs; if (queueMs > key->queueMax[c]) key->queueMax[c] = queueMs; }
    if (!answered) { ++key->fallback; return; }
    if (arrival == PoseArrival::Straddled || arrival == PoseArrival::Other) {
      if (served && served->display > 0) resolveOne(*key, c, at, q, served->display, served->period, served->q);
      else ++key->unresolved;
      return;
    }
    if (count_ >= kPoseArrivalPending) { ++key->unresolved; ++overflowed_; return; }
    Pending& p = pending_[count_++];
    p.thread = thread; p.cls = c; p.at = at; p.qKnown = q != nullptr;
    if (q) for (int i = 0; i < 4; ++i) p.q[i] = q[i];
  }

  // The next frame is published: compare every Between and Render call made since the previous one with it. Called from the publish point BEFORE the
  // cached frame is replaced, with the same arguments as PoseSlotStats::resolve.
  void resolve(int64_t nextDisplay, int64_t nextPeriod, const float* nextQ) {
    for (unsigned i = 0; i < count_; ++i) {
      const Pending& p = pending_[i];
      PoseArrivalCounts* key = keyFor(p.thread);
      if (!key) { ++droppedCalls_; continue; }
      if (nextDisplay <= 0) { ++key->unresolved; continue; }
      resolveOne(*key, p.cls, p.at, p.qKnown ? p.q : nullptr, nextDisplay, nextPeriod, nextQ);
    }
    count_ = 0;
  }

  void dropPending() {
    for (unsigned i = 0; i < count_; ++i) {
      PoseArrivalCounts* key = keyFor(pending_[i].thread);
      if (key) ++key->unresolved; else ++droppedCalls_;
    }
    count_ = 0;
  }

  template <class Sink>
  void setMode(PoseTiming mode, uint64_t lateTotal, uint64_t nowMs, Sink&& sink) {
    if (!modeKnown_) { modeKnown_ = true; mode_ = mode; return; }
    if (mode == mode_) return;
    write(lateTotal, nowMs, sink);
    mode_ = mode; startMs_ = nowMs; lateStart_ = lateTotal; started_ = true;
  }

  template <class Sink>
  void flushIfDue(uint64_t nowMs, uint64_t lateTotal, Sink&& sink) {
    if (!started_) { started_ = true; startMs_ = nowMs; lateStart_ = lateTotal; return; }
    if (nowMs - startMs_ < kPoseSlotWindowMs) return;
    write(lateTotal, nowMs, sink);
    startMs_ = nowMs; lateStart_ = lateTotal;
  }

  template <class Sink>
  void flushNow(uint64_t nowMs, uint64_t lateTotal, Sink&& sink) {
    dropPending();
    write(lateTotal, nowMs, sink);
    startMs_ = nowMs; lateStart_ = lateTotal; started_ = true;
  }

  unsigned pending() const { return count_; }
  uint64_t overflowed() const { return overflowed_; }
  uint64_t droppedCalls() const { return droppedCalls_; }
  unsigned threads() const { return used_; }
  const PoseArrivalCounts* countsFor(uint32_t thread) const {
    for (unsigned i = 0; i < used_; ++i) if (keys_[i].thread == thread) return &keys_[i].counts;
    return nullptr;
  }

  static const char* line(char (&buffer)[1024], uint32_t thread, PoseTiming mode, const PoseArrivalCounts& c, uint64_t late, double seconds) {
    const auto u = [](uint64_t v) { return static_cast<unsigned long long>(v); };
    char queue[3][40];
    for (int i = 0; i < 3; ++i) {
      if (c.queueN[i]) std::snprintf(queue[i], sizeof(queue[i]), "%.2f/%.2f", c.queueSum[i] / double(c.queueN[i]), c.queueMax[i]);
      else std::snprintf(queue[i], sizeof(queue[i]), "n/a");
    }
    char vs[kPoseArrivals][80];
    for (unsigned i = 0; i < kPoseArrivals; ++i)
      std::snprintf(vs[i], sizeof(vs[i]), "(0p %llu, +1p %llu, +2p+ %llu, -1p- %llu)", u(c.bucket[i][kBucket0p]), u(c.bucket[i][kBucketPlus1p]),
                    u(c.bucket[i][kBucketPlus2p]), u(c.bucket[i][kBucketMinus1p]));
    char max[64];
    if (c.maxKnown) std::snprintf(max, sizeof(max), "max %.3f deg (%s)", c.maxAngle, poseArrivalName(PoseArrival(c.maxClass)));
    else std::snprintf(max, sizeof(max), "max n/a");
    std::snprintf(buffer, sizeof(buffer),
      "pose arrival: tid %lu mode %s calls %llu between %llu straddled %llu render %llu other %llu fallback %llu unresolved %llu; "
      "queue ms between %s, straddled %s, render %s; vs first frame after arrival: between %s, straddled %s, render %s, other %s; "
      ">1deg between %llu straddled %llu render %llu other %llu; >2deg between %llu straddled %llu render %llu other %llu; %s; late frames %llu; window %.1f s",
      static_cast<unsigned long>(thread), poseTimingName(mode), u(c.calls), u(c.cls[0]), u(c.cls[1]), u(c.cls[2]), u(c.cls[3]), u(c.fallback), u(c.unresolved),
      queue[0], queue[1], queue[2], vs[0], vs[1], vs[2], vs[3], u(c.over1[0]), u(c.over1[1]), u(c.over1[2]), u(c.over1[3]), u(c.over2[0]), u(c.over2[1]),
      u(c.over2[2]), u(c.over2[3]), max, u(late), seconds);
    return buffer;
  }

 private:
  struct Key {
    uint32_t thread = 0;
    PoseArrivalCounts counts;
  };
  struct Pending {
    int64_t at = 0;
    uint32_t thread = 0;
    unsigned cls = 0;
    bool qKnown = false;
    float q[4]{0, 0, 0, 1};
  };
  PoseArrivalCounts* keyFor(uint32_t thread) {
    for (unsigned i = 0; i < used_; ++i) if (keys_[i].thread == thread) return &keys_[i].counts;
    if (used_ >= kPoseArrivalThreads) return nullptr;
    keys_[used_] = Key{};
    keys_[used_].thread = thread;
    return &keys_[used_++].counts;
  }
  static void resolveOne(PoseArrivalCounts& key, unsigned cls, int64_t at, const float* q, int64_t frameDisplay, int64_t framePeriod, const float* frameQ) {
    unsigned bucket = 0;
    if (!poseArrivalBucketOf(at, frameDisplay, framePeriod, &bucket)) { ++key.unresolved; return; }
    ++key.bucket[cls][bucket];
    if (q && frameQ) {
      const double angle = quaternionAngleDegrees(q, frameQ);
      if (angle > 1.0) ++key.over1[cls];
      if (angle > 2.0) ++key.over2[cls];
      if (!key.maxKnown || angle > key.maxAngle) { key.maxKnown = true; key.maxAngle = angle; key.maxClass = cls; }
    }
  }
  template <class Sink>
  void write(uint64_t lateTotal, uint64_t nowMs, Sink&& sink) {
    const uint64_t late = lateTotal >= lateStart_ ? lateTotal - lateStart_ : 0;
    const double seconds = started_ && nowMs >= startMs_ ? double(nowMs - startMs_) / 1000.0 : 0.0;
    char buffer[1024];
    for (unsigned i = 0; i < used_; ++i) sink(static_cast<const char*>(line(buffer, keys_[i].thread, mode_, keys_[i].counts, late, seconds)));
    if (droppedCalls_) {
      char more[128];
      std::snprintf(more, sizeof(more), "pose arrival: more than %u callers in a window; %llu calls not counted", kPoseArrivalThreads,
                    static_cast<unsigned long long>(droppedCalls_));
      sink(static_cast<const char*>(more));
    }
    used_ = 0;
    droppedCalls_ = 0;
  }
  Key keys_[kPoseArrivalThreads]{};
  unsigned used_ = 0;
  Pending pending_[kPoseArrivalPending]{};
  unsigned count_ = 0;
  uint64_t overflowed_ = 0, droppedCalls_ = 0, startMs_ = 0, lateStart_ = 0;
  bool started_ = false, modeKnown_ = false;
  PoseTiming mode_ = PoseTiming::Next;
};
}  // namespace edvr::openxr
