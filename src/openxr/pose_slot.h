#pragma once

// The pose-slot diagnostic (docs\terrain-culling.md, 2026-10-10; TEMPORARY with advanced.cull_pose_timing). Always on, in both modes, cheap: no
// allocation, no lock, owner thread only (the same rule as the cached frame it measures against).
//
// WHAT IT MEASURES. Elite's game thread asks for "now" between frames and culls planet terrain with the pose it gets; the fix (head_pose_time.h)
// answers at the display time of the NEXT frame as far as it can guess it (`next`: the cached frame's display time plus one period; `slot`: the
// next display slot at or after now + the frame's lead). A frame the game misses is displayed a period or more later than that guess, and the
// pose the cull used is then older than the frame it culls for: a flash at the edge. The `pose gap:` line compares each answer with the CACHED
// frame. This compares it with the frame that really came next. For every call the fix handles (a return address inside the game's image and a
// prediction under 5 ms: answered at an instant of its own OR fallen back to now + prediction) it remembers the instant the call was located at
// (T), why it was not answered at an instant of its own if it was not, the calling thread and the orientation it returned. At the next
// WaitGetPoses (the next time the cached frame changes) every call made since the previous one is resolved against that frame: its display time D'
// and period P', and the orientation EDVR located for it (the pose that frame is drawn with).
//   delta = T - D'. match |delta| < P'/4 (the call was located at the slot that frame really took); short delta <= -P'/4 (the pose is older than
//   the frame it was drawn in: 1p within 1.5 periods, 2p+ beyond); long delta >= +P'/4 (newer than it).
//   angle = the angle between the orientation the call returned and the one located for D'.
// Calls that were not answered at an instant of their own (fallbacks) are their own class, by reason, and are ALSO split by their delta, so
// the line's totals add up: calls = match + short + long + fallback + unresolved. A call that could not be compared (the frame never came, the
// cache was dropped by a session or origin change, the pending store was full, the next frame has no usable period) is unresolved.
//
// One line per calling thread per 60 s window, next to the `pose gap:` line (which is unchanged), written from the WaitGetPoses publish point:
//   pose slot: tid T mode next|slot|arrival calls N match M short S (1p a, 2p+ b) long L (1p c, 2p+ d) fallback F (none e, stale f, period g, cross h,
//   cap i, refused j; delta match k, short l, long m) unresolved U; >1deg match .. short .. (1p .., 2p+ ..) long .. (1p .., 2p+ ..) fallback ..
//   (none .., stale .., period .., cross .., cap .., refused ..); >2deg ...; max X deg (class); late frames n; window S s
// `mode` is the one in force when the calls were made; a mode change closes the window at the change, so a line has one mode. `late frames` is the
// runtime's late-frame count (frame_boundary.h, display periods skipped) over the same window; `window` is its length in seconds (60, or less for a
// window cut short by a mode change or the end of the run: the tally's per-minute rates divide by it). A window with calls always writes its line, with
// zeros, so a missing line means the instrument never ran. `max` is the largest angle of any call that had one, with its class.
//
// Pure: the clock, the late-frame count and the sink are passed in, so tools\openxr_pose_test drives every case and holds
// tools\pose_slot_fixture.log to exactly what this writes (tools\edvr_log.py --tally pose reads it).
#include "head_pose_time.h"
#include "pose_gap.h"

#include <cstdint>
#include <cstdio>

namespace edvr::openxr {

// Calls held between two frames (Elite makes about 4 a frame on its second thread; a long stall with the game thread still asking can exceed it,
// and the rest are counted unresolved), callers kept per window, and the window.
constexpr unsigned kPoseSlotPending = 256;
constexpr unsigned kPoseSlotThreads = 8;
constexpr uint64_t kPoseSlotWindowMs = 60000;

// How a resolved call is classed, in the order the counters keep them. The fallbacks are classes by reason: PoseAnswer NoFrame..Refused (1..6) map to
// kSlotFallbackFirst + (reason - 1).
enum PoseSlotClass : unsigned {
  kSlotMatch = 0, kSlotShort1, kSlotShort2, kSlotLong1, kSlotLong2,
  kSlotFallbackFirst, kSlotFallbackLast = kSlotFallbackFirst + 5,
  kSlotClasses
};

inline const char* poseSlotClassName(unsigned c) {
  static const char* const names[kSlotClasses] = {"match", "short-1p", "short-2p+", "long-1p", "long-2p+", "fallback-none", "fallback-stale",
                                                  "fallback-period", "fallback-cross", "fallback-cap", "fallback-refused"};
  return c < kSlotClasses ? names[c] : "?";
}

// delta = target - nextDisplay against the next frame's period. kind: 0 match, 1 short, 2 long; oneP: the size of a short or long miss is within 1.5
// periods. False when the call cannot be compared (no usable period, or the subtraction would overflow). The comparisons are exact integer ones:
// match is |delta| * 4 < period; 1p is |delta| * 2 < period * 3.
inline bool classifyPoseDelta(int64_t target, int64_t nextDisplay, int64_t period, unsigned* kind, bool* oneP) {
  if (period <= 0) return false;
  if ((nextDisplay > 0 && target < INT64_MIN + nextDisplay) || (nextDisplay < 0 && target > INT64_MAX + nextDisplay)) return false;
  const int64_t delta = target - nextDisplay;
  const uint64_t magnitude = delta < 0 ? uint64_t(0) - uint64_t(delta) : uint64_t(delta);
  const uint64_t p = uint64_t(period);
  const bool match = magnitude <= UINT64_MAX / 4 && magnitude * 4 < p;
  *kind = match ? 0u : (delta < 0 ? 1u : 2u);
  *oneP = magnitude <= UINT64_MAX / 2 && p <= UINT64_MAX / 3 && magnitude * 2 < p * 3;
  return true;
}

struct PoseSlotCounts {
  uint64_t calls = 0, unresolved = 0;
  uint64_t cls[kSlotClasses]{};
  uint64_t over1[kSlotClasses]{}, over2[kSlotClasses]{};   // the calls of each class whose angle was above 1 and above 2 degrees
  uint64_t fallbackDelta[3]{};                              // fallbacks by their delta: match, short, long
  double maxAngle = 0;
  unsigned maxClass = 0;
  bool maxKnown = false;
};

struct NoPoseSlotSink {
  void operator()(const char*) const {}
};

class PoseSlotStats {
 public:
  // The timing in force: what the next calls are made under (next, slot, arrival). Set from the publish point (setMode).
  PoseTiming mode() const { return mode_; }

  // One call the fix handled, located at `at` (the instant it asked the runtime for) and returned orientation `q` (x, y, z, w; null when the pose
  // it returned was not valid). `why` is Answered, or the reason it fell back to now + prediction.
  void noteCall(uint32_t thread, int64_t at, PoseAnswer why, const float* q) {
    PoseSlotCounts* key = keyFor(thread);
    if (!key) { ++droppedCalls_; return; }
    ++key->calls;
    if (count_ >= kPoseSlotPending) { ++key->unresolved; ++overflowed_; return; }
    Pending& p = pending_[count_++];
    p.thread = thread; p.at = at; p.why = why; p.qKnown = q != nullptr;
    if (q) for (int i = 0; i < 4; ++i) p.q[i] = q[i];
  }

  // The next frame is known: its display time, period and drawn orientation (null when its pose was not valid). Resolves every call made since the
  // previous frame against it. Called from the publish point BEFORE the cached frame is replaced and before the window is judged.
  void resolve(int64_t nextDisplay, int64_t nextPeriod, const float* nextQ) {
    for (unsigned i = 0; i < count_; ++i) {
      const Pending& p = pending_[i];
      PoseSlotCounts* key = keyFor(p.thread);
      if (!key) { ++droppedCalls_; continue; }
      unsigned kind = 0;
      bool oneP = false;
      if (nextDisplay <= 0 || !classifyPoseDelta(p.at, nextDisplay, nextPeriod, &kind, &oneP)) { ++key->unresolved; continue; }
      unsigned cls;
      if (p.why != PoseAnswer::Answered) {
        cls = kSlotFallbackFirst + (unsigned(p.why) - 1u);
        if (cls > kSlotFallbackLast) cls = kSlotFallbackLast;
        ++key->fallbackDelta[kind];
      } else {
        cls = kind == 0 ? kSlotMatch : kind == 1 ? (oneP ? kSlotShort1 : kSlotShort2) : (oneP ? kSlotLong1 : kSlotLong2);
      }
      ++key->cls[cls];
      if (p.qKnown && nextQ) {
        const double angle = quaternionAngleDegrees(p.q, nextQ);
        if (angle > 1.0) ++key->over1[cls];
        if (angle > 2.0) ++key->over2[cls];
        if (!key->maxKnown || angle > key->maxAngle) { key->maxKnown = true; key->maxAngle = angle; key->maxClass = cls; }
      }
    }
    count_ = 0;
  }

  // The frame the calls were made against is gone (a session, origin or geometry change dropped the cache): they will never be compared.
  void dropPending() {
    for (unsigned i = 0; i < count_; ++i) {
      PoseSlotCounts* key = keyFor(pending_[i].thread);
      if (key) ++key->unresolved; else ++droppedCalls_;
    }
    count_ = 0;
  }

  // From the publish point, once per frame, after resolve(): the mode for the calls to come. The first call sets it; a change closes the window
  // (writing its lines under the old mode) and opens the next at `nowMs`.
  template <class Sink>
  void setMode(PoseTiming mode, uint64_t lateTotal, uint64_t nowMs, Sink&& sink) {
    if (!modeKnown_) { modeKnown_ = true; mode_ = mode; return; }
    if (mode == mode_) return;
    write(lateTotal, nowMs, sink);
    mode_ = mode; startMs_ = nowMs; lateStart_ = lateTotal; started_ = true;
  }

  // From the publish point: write the window's lines once it is 60 s old. The first call only opens a window.
  template <class Sink>
  void flushIfDue(uint64_t nowMs, uint64_t lateTotal, Sink&& sink) {
    if (!started_) { started_ = true; startMs_ = nowMs; lateStart_ = lateTotal; return; }
    if (nowMs - startMs_ < kPoseSlotWindowMs) return;
    write(lateTotal, nowMs, sink);
    startMs_ = nowMs; lateStart_ = lateTotal;
  }

  // The window as it stands, now (the end of a run, a rig): calls still pending have no next frame and are counted unresolved.
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
  const PoseSlotCounts* countsFor(uint32_t thread) const {
    for (unsigned i = 0; i < used_; ++i) if (keys_[i].thread == thread) return &keys_[i].counts;
    return nullptr;
  }

  // The line for one caller. `late` is the runtime's late-frame count over the window, `seconds` the window's length (60 s, or less for a window
  // cut short by a mode change or the end of the run: the tally's per-minute rates divide by it).
  static const char* line(char (&buffer)[768], uint32_t thread, PoseTiming mode, const PoseSlotCounts& c, uint64_t late, double seconds) {
    const auto u = [](uint64_t v) { return static_cast<unsigned long long>(v); };
    uint64_t fallback = 0, fallbackOver1 = 0, fallbackOver2 = 0;
    for (unsigned i = kSlotFallbackFirst; i <= kSlotFallbackLast; ++i) { fallback += c.cls[i]; fallbackOver1 += c.over1[i]; fallbackOver2 += c.over2[i]; }
    char over[2][256];
    const uint64_t* const sets[2] = {c.over1, c.over2};
    const uint64_t totals[2] = {fallbackOver1, fallbackOver2};
    for (int s = 0; s < 2; ++s) {
      const uint64_t* v = sets[s];
      std::snprintf(over[s], sizeof(over[s]),
        "match %llu short %llu (1p %llu, 2p+ %llu) long %llu (1p %llu, 2p+ %llu) fallback %llu (none %llu, stale %llu, period %llu, cross %llu, cap %llu, refused %llu)",
        u(v[kSlotMatch]), u(v[kSlotShort1] + v[kSlotShort2]), u(v[kSlotShort1]), u(v[kSlotShort2]), u(v[kSlotLong1] + v[kSlotLong2]), u(v[kSlotLong1]), u(v[kSlotLong2]),
        u(totals[s]), u(v[kSlotFallbackFirst]), u(v[kSlotFallbackFirst + 1]), u(v[kSlotFallbackFirst + 2]), u(v[kSlotFallbackFirst + 3]),
        u(v[kSlotFallbackFirst + 4]), u(v[kSlotFallbackFirst + 5]));
    }
    char max[64];
    if (c.maxKnown) std::snprintf(max, sizeof(max), "max %.3f deg (%s)", c.maxAngle, poseSlotClassName(c.maxClass));
    else std::snprintf(max, sizeof(max), "max n/a");
    std::snprintf(buffer, sizeof(buffer),
      "pose slot: tid %lu mode %s calls %llu match %llu short %llu (1p %llu, 2p+ %llu) long %llu (1p %llu, 2p+ %llu) fallback %llu "
      "(none %llu, stale %llu, period %llu, cross %llu, cap %llu, refused %llu; delta match %llu, short %llu, long %llu) unresolved %llu; "
      ">1deg %s; >2deg %s; %s; late frames %llu; window %.1f s",
      static_cast<unsigned long>(thread), poseTimingName(mode), u(c.calls), u(c.cls[kSlotMatch]), u(c.cls[kSlotShort1] + c.cls[kSlotShort2]),
      u(c.cls[kSlotShort1]), u(c.cls[kSlotShort2]), u(c.cls[kSlotLong1] + c.cls[kSlotLong2]), u(c.cls[kSlotLong1]), u(c.cls[kSlotLong2]), u(fallback),
      u(c.cls[kSlotFallbackFirst]), u(c.cls[kSlotFallbackFirst + 1]), u(c.cls[kSlotFallbackFirst + 2]), u(c.cls[kSlotFallbackFirst + 3]),
      u(c.cls[kSlotFallbackFirst + 4]), u(c.cls[kSlotFallbackFirst + 5]), u(c.fallbackDelta[0]), u(c.fallbackDelta[1]), u(c.fallbackDelta[2]),
      u(c.unresolved), over[0], over[1], max, u(late), seconds);
    return buffer;
  }

 private:
  struct Key {
    uint32_t thread = 0;
    PoseSlotCounts counts;
  };
  struct Pending {
    int64_t at = 0;
    uint32_t thread = 0;
    PoseAnswer why = PoseAnswer::Answered;
    bool qKnown = false;
    float q[4]{0, 0, 0, 1};
  };
  PoseSlotCounts* keyFor(uint32_t thread) {
    for (unsigned i = 0; i < used_; ++i) if (keys_[i].thread == thread) return &keys_[i].counts;
    if (used_ >= kPoseSlotThreads) return nullptr;
    keys_[used_] = Key{};
    keys_[used_].thread = thread;
    return &keys_[used_++].counts;
  }
  template <class Sink>
  void write(uint64_t lateTotal, uint64_t nowMs, Sink&& sink) {
    const uint64_t late = lateTotal >= lateStart_ ? lateTotal - lateStart_ : 0;
    const double seconds = started_ && nowMs >= startMs_ ? double(nowMs - startMs_) / 1000.0 : 0.0;
    char buffer[768];
    for (unsigned i = 0; i < used_; ++i) sink(static_cast<const char*>(line(buffer, keys_[i].thread, mode_, keys_[i].counts, late, seconds)));
    if (droppedCalls_) {
      char more[128];
      std::snprintf(more, sizeof(more), "pose slot: more than %u callers in a window; %llu calls not counted", kPoseSlotThreads,
                    static_cast<unsigned long long>(droppedCalls_));
      sink(static_cast<const char*>(more));
    }
    used_ = 0;
    droppedCalls_ = 0;
  }
  Key keys_[kPoseSlotThreads]{};
  unsigned used_ = 0;
  Pending pending_[kPoseSlotPending]{};
  unsigned count_ = 0;
  uint64_t overflowed_ = 0, droppedCalls_ = 0, startMs_ = 0, lateStart_ = 0;
  bool started_ = false, modeKnown_ = false;
  PoseTiming mode_ = PoseTiming::Next;
};
}  // namespace edvr::openxr
