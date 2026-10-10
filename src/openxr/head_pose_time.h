#pragma once

// Elite's "now" head pose, answered one display period after the drawn frame's display time (docs\terrain-culling.md, "What EDVR does now").
//
// THE DEFECT. Elite builds its planet-terrain culling camera from a head pose it asks IVRSystem::GetDeviceToAbsoluteTrackingPose for with a
// prediction of about 0 s, i.e. "now", while it DRAWS with the pose WaitGetPoses handed the render thread, located at the frame's
// predictedDisplayTime. Located at the wall clock, the culling pose sat 41-44 ms before the drawn one (flight 3, 2026-10-09) and was
// turned up to 4.1 degrees from it, in step with head speed (r = +0.98): the terrain tiles chosen for the leading edge of a head turn were
// the wrong ones, and the black squares were the gap.
//
// THE MODEL THAT FITS FLIGHTS 3 TO 5 (pilot-judged, not an observation of the culling camera's consumer). Elite's game thread calls this
// function directly BETWEEN frames, for work that feeds the NEXT frame, so the right pose is the one the next frame will be drawn with: the
// latest frame's display time plus that frame's display period. While a frame renders the game thread reads that frame's own latched
// WaitGetPoses pose, which is already right for what it computes then (the DSS scanner UI included). Located at the display time itself
// (flight 3's fix, `display`) the direct calls were one period short, and squares remained on rapid turns; forcing the latched readers
// onto the direct path as well (an engine patch, `display_direct` / `next_direct`) put them a period ahead, flashed black on a rapid turn
// and made the scanner UI follow the head. Answering the direct calls one period past the display time and touching nothing in the game
// (`next`) showed neither.
//
// THE FILTER. Which requests are Elite's own "now"? Not a build's return address (a game update would silently stop the fix): any call
// whose return address lies inside the game executable's mapped image and whose prediction is under 5 ms either way. A real prediction (a
// caller that wants the future), a caller outside the image (an overlay, a tool) and a call before any frame has been waited are answered
// as they always were, at the wall clock plus the prediction.
#include "exe_module.h"

#include <cmath>
#include <cstdint>
#include <cstdio>

namespace edvr::openxr {

// A prediction this close to zero is a request for "now". The bound is exclusive: exactly 5 ms is a prediction.
constexpr float kNowPredictionLimitSeconds = 0.005f;

inline bool isNowRequest(float predictionSeconds) {
  return std::fabs(predictionSeconds) < kNowPredictionLimitSeconds;   // (a NaN is false: the caller refuses it before this)
}

// Why a call handled by the fix was or was not answered at an instant of its own. Answered is the success; the rest are the counted fallbacks
// (the call is located at now + prediction instead), and the names are what the `pose slot:` line prints: NoFrame "none" (no frame cached yet,
// or a clock that cannot be read), Stale (the cached frame is more than one period behind now: `next` only), Period (no usable period or lead,
// or the sum would overflow), Cross (a reference-space change between the display time and the instant), Cap (more than kSlotCapNs ahead:
// `slot` only), Refused (the runtime refused the instant with XR_ERROR_TIME_INVALID and the call was retried at now + prediction).
enum class PoseAnswer : uint8_t { Answered = 0, NoFrame, Stale, Period, Cross, Cap, Refused };
constexpr unsigned kPoseAnswers = 7;

inline const char* poseAnswerName(PoseAnswer a) {
  switch (a) {
    case PoseAnswer::Answered: return "answered";
    case PoseAnswer::NoFrame: return "none";
    case PoseAnswer::Stale: return "stale";
    case PoseAnswer::Period: return "period";
    case PoseAnswer::Cross: return "cross";
    case PoseAnswer::Cap: return "cap";
    case PoseAnswer::Refused: return "refused";
  }
  return "?";
}

// Is this call one the display time answers? `returnRva` is the caller's return address as an RVA in the game executable (frameRva):
// a value below kFrameUnknown is inside the image.
inline bool answeredAtDisplayTime(uint32_t returnRva, float predictionSeconds) {
  return returnRva < kFrameUnknown && isNowRequest(predictionSeconds);
}

// The latest frame's predictedDisplayTime, or false when there is none yet (no frame waited, a display time that is not positive) and the call
// falls back to now + prediction. (The instant a call is located at is this plus the frame's period: answerTime below.)
inline bool displayTimeTarget(int64_t latestDisplayTime, int64_t* out) {
  if (latestDisplayTime <= 0) return false;
  *out = latestDisplayTime;
  return true;
}

// HOW LONG A DISPLAY TIME STAYS GOOD. The latest frame's display time is the instant of the frame being drawn, and a real one is always
// ahead of now (flight 3 measured now about 42 ms before it). It stops being usable when it is behind now by more than one display period:
// by then the display has moved on to a later frame, so the instant belongs to a frame that is no longer the one being drawn. That
// is what a session that has stopped, a recenter, a loading screen where WaitGetPoses does not run, or a frame that was never replaced
// look like to this cache, and a runtime may answer such a time with stale tracking or reject it. The tolerance is the frame's own period
// (its predictedDisplayPeriod), capped at 50 ms so a period that is not a display period cannot stretch it; with no period it is 0, and the
// display time must not be behind now at all.
constexpr int64_t kPoseFrameMaxToleranceNs = 50000000;

inline int64_t poseFrameTolerance(int64_t periodNs) {
  if (periodNs <= 0) return 0;
  return periodNs > kPoseFrameMaxToleranceNs ? kPoseFrameMaxToleranceNs : periodNs;
}

// Is `displayTime` (positive: displayTimeTarget has accepted it) still good at `now`? Both are runtime times in ns.
inline bool displayTimeFresh(int64_t displayTime, int64_t periodNs, int64_t now) {
  if (displayTime >= now) return true;                               // ahead of now, or exactly now: the normal case
  if (displayTime <= 0 || now <= 0) return false;                    // not a time this comparison can judge; the caller falls back
  return now - displayTime <= poseFrameTolerance(periodNs);          // both positive, so the difference cannot overflow
}

// THE INSTANT. One display period after the cached frame's display time (`addPeriod(display, period, out)` is space_pose.h nextPredictionTime,
// the arithmetic the game-pose array of WaitGetPoses already uses). The cached frame must still be fresh (displayTimeFresh), judged on the
// CACHED frame's display time, never on the target a period ahead of it. Returns false, and the call is located at now + prediction (counted
// as a fallback), when there is no usable display time, when the cached frame is no longer fresh, when the period cannot be added (no
// period, or the sum would overflow), or when a reference-space change falls between the display time and the target
// (`crosses(from, to)`, reference_changes.h): a pose across it would be in another origin.
template <class AddPeriod, class Crosses>
inline PoseAnswer answerTimeWhy(int64_t latestDisplayTime, int64_t periodNs, int64_t now, AddPeriod&& addPeriod, Crosses&& crosses, int64_t* out) {
  int64_t display = 0;
  if (!displayTimeTarget(latestDisplayTime, &display)) return PoseAnswer::NoFrame;
  if (!displayTimeFresh(display, periodNs, now)) return PoseAnswer::Stale;
  int64_t target = 0;
  if (!addPeriod(display, periodNs, target)) return PoseAnswer::Period;
  if (crosses(display, target)) return PoseAnswer::Cross;
  *out = target;
  return PoseAnswer::Answered;
}

// The same, as the yes/no the host has always asked (the reason is what the pose-slot diagnostic counts: pose_slot.h).
template <class AddPeriod, class Crosses>
inline bool answerTime(int64_t latestDisplayTime, int64_t periodNs, int64_t now, AddPeriod&& addPeriod, Crosses&& crosses, int64_t* out) {
  return answerTimeWhy(latestDisplayTime, periodNs, now, addPeriod, crosses, out) == PoseAnswer::Answered;
}

// THE SLOT INSTANT (TEMPORARY: advanced.cull_pose_timing = slot; docs\terrain-culling.md, 2026-10-10). `next` answers one period past the cached frame's
// display time, which is the next frame's display time only while that frame is on time. When the game misses a display slot the next frame is
// shown at D + 2P or later, so `next` is a period short for it, and once D is a period behind now the freshness guard drops the call to now +
// prediction, 40 ms short. This answers at the display slot the NEXT frame will really be shown at, as far as it can be told from the cached frame:
//   L  the frame's lead: its display time minus the runtime time at which the xrWaitFrame that produced it returned (how far ahead of the wait
//      the compositor schedules a frame; the next frame's lead is taken to be the same);
//   e  = now + L, when a frame whose wait returns now would be shown;
//   k  = the smallest whole number of periods past D that reaches e, and never fewer than one: the next slot at or after e.
// In steady flight now is within a period of the wait that returned D, so e is at most D + P and k = 1: exactly `next`. A late or stale frame
// puts now a period or more past that wait and k grows with it. The freshness guard is not applied (k replaces it); a gap longer than
// kSlotCapNs (a stall, a loading screen) is a counted fallback (Cap), the same as the stale fallback it replaces. No display time, no usable
// period or lead, an overflow, or a reference-space change between D and the instant (`crosses(from, to)`, reference_changes.h) fall back
// to now + prediction as well. `extraPeriods` (0 for `slot`; 1 for `arrival` on a call that arrived between frames) is added to k AFTER the cap
// is judged on k, with its own overflow check, and the crossing check covers the final span. `periodsOut`, when given, receives k + extra on success.
constexpr int64_t kSlotCapNs = 250000000;

template <class Crosses>
inline PoseAnswer slotAnswerTime(int64_t latestDisplayTime, int64_t periodNs, int64_t leadNs, int64_t now, Crosses&& crosses, int64_t* out,
                                 int64_t* periodsOut = nullptr, int64_t extraPeriods = 0) {
  int64_t display = 0;
  if (!displayTimeTarget(latestDisplayTime, &display)) return PoseAnswer::NoFrame;
  if (periodNs <= 0 || leadNs <= 0) return PoseAnswer::Period;
  if (now > 0 && leadNs > INT64_MAX - now) return PoseAnswer::Period;
  const int64_t expected = now + leadNs;
  int64_t k = 1;
  if (expected > display) {
    const int64_t ahead = expected - display;                       // display > 0 and expected > display: no overflow
    k = ahead / periodNs + (ahead % periodNs != 0 ? 1 : 0);
    if (k < 1) k = 1;
  }
  int64_t most = kSlotCapNs / periodNs;                             // (a period past the cap still gets its one slot: k = 1 is `next`)
  if (most < 1) most = 1;
  if (k > most) return PoseAnswer::Cap;
  if (extraPeriods < 0) extraPeriods = 0;
  const int64_t n = k + extraPeriods;                               // (k <= most, extra is 0 or 1 in use: no overflow here)
  if (periodNs > INT64_MAX / n) return PoseAnswer::Period;          // (a period past the cap has k = 1 and may be huge: the extra one can overflow)
  const int64_t step = n * periodNs;
  if (step > INT64_MAX - display) return PoseAnswer::Period;
  const int64_t target = display + step;
  if (crosses(display, target)) return PoseAnswer::Cross;
  *out = target;
  if (periodsOut) *periodsOut = n;
  return PoseAnswer::Answered;
}

// WHICH INSTANT, AND FOR WHICH CALL (TEMPORARY: advanced.cull_pose_timing = next | slot | arrival; docs\terrain-culling.md, 2026-10-10).
// Elite's pose call is served on the owner thread, which may be blocked in the frame wait for the next frame when the call arrives. A call that
// arrives between Elite's Submit of frame N and the publish of frame N+1 and is served before that publish is answered relative to N; the same call
// served after it is answered relative to N+1 -- a period apart for the same place in Elite's pipeline. `PoseArrival` says which it was, from two
// sequence numbers read when the call ARRIVED (publishSeq: frames published so far; submitSeq: the publishSeq of the last frame Elite began to
// submit) and the publishSeq when it was SERVED:
//   Between    submitSeq >= publishSeq at arrival (the latest frame was already submitted), and no publish happened while it queued.
//   Straddled  the same, but a publish did happen while it queued: it is served against the frame published during its wait.
//   Render     submitSeq < publishSeq at arrival (published, not yet submitted), no publish while it queued.
//   Other      a Render call that straddled a publish.
enum class PoseArrival : uint8_t { Between = 0, Straddled, Render, Other };
constexpr unsigned kPoseArrivals = 4;

inline const char* poseArrivalName(PoseArrival a) {
  switch (a) {
    case PoseArrival::Between: return "between";
    case PoseArrival::Straddled: return "straddled";
    case PoseArrival::Render: return "render";
    case PoseArrival::Other: return "other";
  }
  return "?";
}

inline PoseArrival classifyPoseArrival(uint64_t submitSeqAtArrival, uint64_t publishSeqAtArrival, uint64_t publishSeqAtService) {
  const bool submitted = submitSeqAtArrival >= publishSeqAtArrival;
  const bool straddled = publishSeqAtService != publishSeqAtArrival;
  if (submitted) return straddled ? PoseArrival::Straddled : PoseArrival::Between;
  return straddled ? PoseArrival::Other : PoseArrival::Render;
}

// The three timings: `next` (shipped; D + P), `slot` (the next display slot at or after now + lead), `arrival` (slot, plus one period for a call that
// arrived between frames and was NOT served after a publish, so that every between-frames call is answered at D(first frame published after it
// arrived) + P whether or not it queued behind the wait; Straddled, Render and Other calls get the slot target unchanged, Render being `next`).
enum class PoseTiming : uint8_t { Next = 0, Slot = 1, Arrival = 2 };

inline const char* poseTimingName(PoseTiming m) {
  switch (m) {
    case PoseTiming::Next: return "next";
    case PoseTiming::Slot: return "slot";
    case PoseTiming::Arrival: return "arrival";
  }
  return "?";
}
// The word the graphics half hands the runtime (EdvrNativeFrameOutput::cullPoseTiming): anything past 2 reads as `next`.
inline PoseTiming poseTimingFromWord(uint32_t word) { return word == 1 ? PoseTiming::Slot : word == 2 ? PoseTiming::Arrival : PoseTiming::Next; }

// The instant for one call under one timing. `addPeriod(display, period, out)` and `crosses(from, to)` as answerTime's; `next` ignores the arrival
// class and the lead and judges freshness on the cached frame (answerTimeWhy). `periodsOut` receives how many periods past D the instant is
// (slot and arrival only).
template <class AddPeriod, class Crosses>
inline PoseAnswer timingAnswerTime(PoseTiming mode, PoseArrival arrival, int64_t latestDisplayTime, int64_t periodNs, int64_t leadNs, int64_t now,
                                   AddPeriod&& addPeriod, Crosses&& crosses, int64_t* out, int64_t* periodsOut = nullptr) {
  if (mode == PoseTiming::Slot) return slotAnswerTime(latestDisplayTime, periodNs, leadNs, now, crosses, out, periodsOut, 0);
  if (mode == PoseTiming::Arrival)
    return slotAnswerTime(latestDisplayTime, periodNs, leadNs, now, crosses, out, periodsOut, arrival == PoseArrival::Between ? 1 : 0);
  return answerTimeWhy(latestDisplayTime, periodNs, now, addPeriod, crosses, out);
}

// The log lines that say the fix is acting, and for whom. The first caller to qualify writes one line; each further DISTINCT return RVA that
// qualifies writes one more, up to kFurtherCap, so a caller that turns up after a game update is named. One more line says the cap was reached.
//   head pose: Elite's "now" requests are answered one display period after the drawn frame's display time (first from exe+0x...)
//   head pose: another Elite caller of "now" is answered one display period after the drawn frame's display time (exe+0x...)
//   head pose: more than 8 further callers of "now"; the rest are not logged
class HeadPoseSightings {
 public:
  static constexpr unsigned kFurtherCap = 8;
  template <class Sink>
  void note(uint32_t returnRva, Sink&& sink) {
    for (unsigned i = 0; i < count_; ++i)
      if (seen_[i] == returnRva) return;
    char line[192];
    if (count_ == 0) {
      std::snprintf(line, sizeof(line), "head pose: Elite's \"now\" requests are answered one display period after the drawn frame's display time (first from exe+0x%X)", returnRva);
    } else if (count_ <= kFurtherCap) {
      std::snprintf(line, sizeof(line), "head pose: another Elite caller of \"now\" is answered one display period after the drawn frame's display time (exe+0x%X)", returnRva);
    } else {
      if (!capSaid_) {
        capSaid_ = true;
        std::snprintf(line, sizeof(line), "head pose: more than %u further callers of \"now\"; the rest are not logged", kFurtherCap);
        sink(static_cast<const char*>(line));
      }
      return;
    }
    seen_[count_++] = returnRva;
    sink(static_cast<const char*>(line));
  }
  unsigned distinct() const { return count_; }

 private:
  uint32_t seen_[kFurtherCap + 1]{};
  unsigned count_ = 0;
  bool capSaid_ = false;
};

}  // namespace edvr::openxr
