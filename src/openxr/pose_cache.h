#pragma once

// Elite's display-time head pose, answered from the owner's publication instead of an owner hop (native_pose_cache;
// docs\render-thread-cpu-2026-10-10.md, the 2026-10-11 owner-hop entry).
//
// WHY. Elite's job thread calls GetDeviceToAbsoluteTrackingPose five to nine times a frame. Each call ran on the owner, so a call that
// arrived while the owner sat in xrWaitFrame (8.6-9.2 ms) queued behind it. A display call's answer is a function of the latest frame's
// display time and period alone (head_pose_time.h answerTime), so the owner can publish it once per frame for any thread to read.
//
// THE TWO SLOTS. A call made between waits is answered at display + period: the hop's answer after the last publish. A call made DURING a
// wait is answered by the hop after that wait, at the NEW frame's display + period, and the owner does not know that display time until
// xrWaitFrame returns. So each publication locates two instants: `now` (display + 1 period, for calls between waits) and `next`
// (display + 2 periods, for a call made while the next wait is in progress; the owner sets `waiting` at the wait's entry and the next
// publish clears it). `next` equals the hop's answer exactly when the runtime's next display time is one period after this one. A late or
// skipped frame moves the runtime's next display time by k periods, and the in-wait answer is then early by k periods. The window line's
// target_delta is that error per frame (cached minus hop), measured on the owner from the next publication; nothing is hopped for it.
//
// KEYS AND FRESHNESS. A publication answers only a caller that presents the geometry generation and the origin generation it was made
// under (the origin generation is read through the thread-safe compositor read), and only while its display time is fresh at the caller's
// clock (displayTimeFresh, the hop's own rule). The caller estimates "now" as the runtime's now at publish plus the cache clock's elapsed
// time, so it makes no runtime call. Session end, origin or reference change, geometry change and fatal failure all reach dropPoseFrame,
// which invalidates the publication. Anything the cache cannot answer goes to the hop unchanged, and each such display call is counted.

#include "head_pose_time.h"
#include "pose_gap.h"
#include "space_pose.h"
#include "geometry_snapshot.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <vector>

namespace edvr::openxr {

// Why a display call took the owner hop instead of the publication.
enum class PoseCacheMiss : unsigned { Unpublished = 0, Keys, Stale, Slot, NewCaller, Count };

inline const char* poseCacheMissName(PoseCacheMiss m) {
  switch (m) {
    case PoseCacheMiss::Unpublished: return "unpublished";
    case PoseCacheMiss::Keys: return "keys";
    case PoseCacheMiss::Stale: return "stale";
    case PoseCacheMiss::Slot: return "slot";
    case PoseCacheMiss::NewCaller: return "new_caller";
    default: return "other";
  }
}

// The pose the hop hands back for a located head: the valid bits, the tracking result and the matrix (the tail of
// NativeRuntimeHost::locateHeadOwned). The publication uses it, so the two cannot disagree about the figures.
inline bool headPoseFrom(const XrSpaceLocation& head, vr::TrackedDevicePose_t& pose) {
  pose = vr::TrackedDevicePose_t{};
  pose.bDeviceIsConnected = true;
  pose.mDeviceToAbsoluteTracking.m[0][0] = pose.mDeviceToAbsoluteTracking.m[1][1] = pose.mDeviceToAbsoluteTracking.m[2][2] = 1;
  constexpr auto valid = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
  pose.bPoseIsValid = (head.locationFlags & valid) == valid;
  pose.eTrackingResult = pose.bPoseIsValid ? vr::TrackingResult_Running_OK : vr::TrackingResult_Running_OutOfRange;
  if (pose.bPoseIsValid) {
    double matrix[4][4];
    detail::rigid(head.pose, matrix);
    if (!detail::narrow(matrix, pose.mDeviceToAbsoluteTracking)) return false;
  }
  return true;
}

// One located instant: what a display call made under the publication's keys is answered with.
struct PoseCacheSlot {
  bool valid = false;
  XrTime target = 0;
  float orientation[4] = {0, 0, 0, 1};
  vr::TrackedDevicePose_t pose{};
};

// What a publication was made under. The session and the reference generation are not keys a caller can read (they are owner state);
// the owner drops the publication whenever either changes, through dropPoseFrame.
struct PoseCacheKeys {
  uint64_t geometry = 0, origin = 0;
  bool operator==(const PoseCacheKeys& o) const { return geometry == o.geometry && origin == o.origin; }
};

struct PoseCacheRecord {
  bool valid = false;      // false: nothing to answer with
  bool waiting = false;    // a waitPoses is in progress: a call takes `next`, not `now`
  PoseCacheKeys keys{};
  XrTime display = 0;      // the latest frame's display time and period, as publishPoseGap recorded them
  XrDuration period = 0;
  bool frameOrientationKnown = false;
  float frameOrientation[4] = {0, 0, 0, 1};
  bool speedKnown = false;
  double speedDegPerSec = 0;
  XrTime nowAtPublish = 0; // the runtime's now at the publish, and the cache clock's reading then (microseconds)
  uint64_t publishedUs = 0;
  PoseCacheSlot now;       // display + 1 period: the hop's answer for a call made between waits
  PoseCacheSlot next;      // display + 2 periods: the hop's answer for a call made during the next wait, if that display is one period on
};

// The caller's answer, and what the pose-gap line needs from it.
struct PoseCacheAnswer {
  vr::TrackedDevicePose_t pose{};
  XrTime target = 0, display = 0;
  bool inWait = false;
  bool frameOrientationKnown = false, speedKnown = false;
  float frameOrientation[4] = {0, 0, 0, 1};
  float returnedOrientation[4] = {0, 0, 0, 1};
  double speedDegPerSec = 0;
};

// One window of the cache's counters (native_pose_cache). Percentiles are nearest-rank over the window's samples, capped.
struct PoseCacheReport {
  uint64_t hits = 0, hitsInWait = 0, misses = 0, publishes = 0;
  uint64_t reasons[unsigned(PoseCacheMiss::Count)] = {};
  bool ageValid = false, targetValid = false, angleValid = false;
  double ageP50Ms = 0, ageP95Ms = 0, targetP50Ms = 0, targetP95Ms = 0, angleP50Deg = 0, angleP95Deg = 0;
};

class PoseCache final {
 public:
  using Clock = uint64_t (*)();
  static constexpr unsigned kSeenCapacity = 16;
  static constexpr size_t kSampleCapacity = 4096;

  // The cache clock, in microseconds, monotonic. Set before the owner starts (the host sets the real one; a rig substitutes its own).
  void setClock(Clock clock) { clock_ = clock; }
  uint64_t clockUs() const { return clock_(); }

  // Caller thread. True when the call is answered from the publication. Otherwise `why` names what sends it to the owner hop, and the
  // call is counted. A caller from an RVA the cache has not seen yet goes to the hop once, so the owner's pose-sighting line still names it.
  bool answer(uint64_t geometry, uint64_t origin, uint32_t rva, PoseCacheAnswer& out, PoseCacheMiss& why) {
    std::lock_guard<std::mutex> lock(mutex_);
    const uint64_t nowUs = clock_();
    why = PoseCacheMiss::Unpublished;
    if (!rec_.valid) return missLocked(why);
    if (!(rec_.keys == PoseCacheKeys{geometry, origin})) { why = PoseCacheMiss::Keys; return missLocked(why); }
    const uint64_t elapsedUs = nowUs >= rec_.publishedUs ? nowUs - rec_.publishedUs : 0;
    const XrTime nowAt = rec_.nowAtPublish + XrTime(elapsedUs) * 1000;
    if (!displayTimeFresh(rec_.display, rec_.period, nowAt)) { why = PoseCacheMiss::Stale; return missLocked(why); }
    const PoseCacheSlot& slot = rec_.waiting ? rec_.next : rec_.now;
    if (!slot.valid) { why = PoseCacheMiss::Slot; return missLocked(why); }
    if (!seenLocked(rva)) { why = PoseCacheMiss::NewCaller; return missLocked(why); }
    out = PoseCacheAnswer{};
    out.pose = slot.pose;
    out.target = slot.target;
    out.display = rec_.display;
    out.inWait = rec_.waiting;
    out.frameOrientationKnown = rec_.frameOrientationKnown;
    for (unsigned i = 0; i < 4; ++i) {
      out.frameOrientation[i] = rec_.frameOrientation[i];
      out.returnedOrientation[i] = slot.orientation[i];
    }
    out.speedKnown = rec_.speedKnown;
    out.speedDegPerSec = rec_.speedDegPerSec;
    ++hits_;
    if (rec_.waiting) ++hitsInWait_;
    push(ageUs_, int64_t(elapsedUs));
    return true;
  }

  // Owner, waitPoses entry: a call made from now until the next publish is answered from `next`.
  void beginWait() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (rec_.valid) rec_.waiting = true;
  }

  // Owner: no publication. Every display call takes the hop until the next publish.
  void invalidate() {
    std::lock_guard<std::mutex> lock(mutex_);
    rec_ = PoseCacheRecord{};
  }

  // Owner, after a successful wait. Replaces the publication. The previous publication's `next` is compared with this frame's `now`
  // (the in-wait error, as the hop would have answered it), when both were made under the same keys.
  void publish(const PoseCacheRecord& fresh, uint64_t publishedUs) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (rec_.valid && rec_.next.valid && fresh.now.valid && rec_.keys == fresh.keys) {
      push(targetDeltaNs_, int64_t(rec_.next.target - fresh.now.target));
      push(angleDeg_, quaternionAngleDegrees(rec_.next.orientation, fresh.now.orientation));
    }
    rec_ = fresh;
    rec_.valid = fresh.now.valid;
    rec_.waiting = false;
    rec_.publishedUs = publishedUs;
    ++publishes_;
  }

  // Takes the window's counters and clears them.
  void takeWindow(PoseCacheReport& r) {
    std::lock_guard<std::mutex> lock(mutex_);
    r = PoseCacheReport{};
    r.hits = hits_; r.hitsInWait = hitsInWait_; r.publishes = publishes_;
    for (unsigned i = 0; i < unsigned(PoseCacheMiss::Count); ++i) { r.reasons[i] = reasons_[i]; r.misses += reasons_[i]; }
    if (!ageUs_.empty()) {
      r.ageValid = true;
      r.ageP50Ms = pick(ageUs_, 50) / 1000.0; r.ageP95Ms = pick(ageUs_, 95) / 1000.0;
    }
    if (!targetDeltaNs_.empty()) {
      r.targetValid = true;
      r.targetP50Ms = pick(targetDeltaNs_, 50) / 1e6; r.targetP95Ms = pick(targetDeltaNs_, 95) / 1e6;
    }
    if (!angleDeg_.empty()) {
      r.angleValid = true;
      r.angleP50Deg = pick(angleDeg_, 50); r.angleP95Deg = pick(angleDeg_, 95);
    }
    hits_ = hitsInWait_ = publishes_ = 0;
    for (auto& c : reasons_) c = 0;
    ageUs_.clear(); targetDeltaNs_.clear(); angleDeg_.clear();
  }

 private:
  bool missLocked(PoseCacheMiss why) {
    ++reasons_[unsigned(why)];
    return false;
  }
  // True for a caller already seen. A new one is recorded, so its next call can be answered; past capacity every new caller takes the hop.
  bool seenLocked(uint32_t rva) {
    for (unsigned i = 0; i < seenCount_; ++i) if (seen_[i] == rva) return true;
    if (seenCount_ < kSeenCapacity) seen_[seenCount_++] = rva;
    return false;
  }
  template <class T> static void push(std::vector<T>& v, T value) {
    if (v.size() < kSampleCapacity) v.push_back(value);
  }
  // Nearest rank, as OwnerInvokeStats: v[(n*pct+99)/100 - 1] after a sort.
  template <class T> static double pick(std::vector<T>& v, unsigned pct) {
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return double(v[(n * pct + 99) / 100 - 1]);
  }

  std::mutex mutex_;
  Clock clock_ = nullptr;
  PoseCacheRecord rec_;
  uint32_t seen_[kSeenCapacity]{};
  unsigned seenCount_ = 0;
  uint64_t hits_ = 0, hitsInWait_ = 0, publishes_ = 0;
  uint64_t reasons_[unsigned(PoseCacheMiss::Count)] = {};
  std::vector<int64_t> ageUs_, targetDeltaNs_;
  std::vector<double> angleDeg_;
};

// native_pose_cache,window=N,hits=H,hits_in_wait=W,misses=M,miss_reasons=unpublished:a/keys:b/stale:c/slot:d/new_caller:e,
//   age_p50/p95=x/y,target_delta_p50/p95=x/y,angle_delta_p50/p95=x/y,publishes=P,events_local=L,events_dropped=D
// ms, ms and degrees with three decimals; na where a series has no sample. target_delta is the cached in-wait target minus the hop's target
// (negative: the in-wait answer was earlier). Returns the length written, or 0 when the buffer is too small.
inline int formatPoseCacheLine(uint64_t window, const PoseCacheReport& r, uint64_t eventsLocal, uint64_t eventsDropped,
                               char* buf, size_t cap) {
  char age[24], target[24], angle[24];
  auto pair = [](char* dst, size_t n, bool valid, double a, double b) {
    if (valid) std::snprintf(dst, n, "%.3f/%.3f", a, b); else std::snprintf(dst, n, "na/na");
  };
  pair(age, sizeof age, r.ageValid, r.ageP50Ms, r.ageP95Ms);
  pair(target, sizeof target, r.targetValid, r.targetP50Ms, r.targetP95Ms);
  pair(angle, sizeof angle, r.angleValid, r.angleP50Deg, r.angleP95Deg);
  const int n = std::snprintf(buf, cap,
      "native_pose_cache,window=%llu,hits=%llu,hits_in_wait=%llu,misses=%llu,"
      "miss_reasons=unpublished:%llu/keys:%llu/stale:%llu/slot:%llu/new_caller:%llu,"
      "age_p50/p95=%s,target_delta_p50/p95=%s,angle_delta_p50/p95=%s,publishes=%llu,events_local=%llu,events_dropped=%llu\n",
      static_cast<unsigned long long>(window), static_cast<unsigned long long>(r.hits),
      static_cast<unsigned long long>(r.hitsInWait), static_cast<unsigned long long>(r.misses),
      static_cast<unsigned long long>(r.reasons[0]), static_cast<unsigned long long>(r.reasons[1]),
      static_cast<unsigned long long>(r.reasons[2]), static_cast<unsigned long long>(r.reasons[3]),
      static_cast<unsigned long long>(r.reasons[4]), age, target, angle,
      static_cast<unsigned long long>(r.publishes), static_cast<unsigned long long>(eventsLocal),
      static_cast<unsigned long long>(eventsDropped));
  return n > 0 && size_t(n) < cap ? n : 0;
}

} // namespace edvr::openxr
