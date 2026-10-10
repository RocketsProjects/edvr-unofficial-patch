#pragma once
// Elite's "now" head pose, answered one display period after the drawn frame's display time (src/openxr/head_pose_time.h;
// docs\terrain-culling.md): which calls it answers (a return address inside the game's image and a prediction under 5 ms either way, at its
// boundary and on both signs), which it leaves alone, the instant it forms (display time + period, with the freshness test on the cached frame),
// and the log lines that say whom it is answering, and when the cap is hit.
#include "../../src/openxr/head_pose_time.h"
#include "../../src/openxr/space_pose.h"

#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace head_pose_time_cases {
using namespace edvr::openxr;

template<class Check>
void runHeadPoseTimeCases(Check&& check) {
  // ---- what "now" is ---------------------------------------------------------------------------------------------------------------------
  {
    check(kNowPredictionLimitSeconds==0.005f,"the bound is 5 ms");
    check(isNowRequest(0.0f)&&isNowRequest(-0.0f)&&isNowRequest(0.001f)&&isNowRequest(0.0049f)&&isNowRequest(0.00499f)&&isNowRequest(0.004999f),
          "a prediction under 5 ms is a request for \"now\"");
    check(!isNowRequest(0.005f)&&!isNowRequest(0.0051f)&&!isNowRequest(0.011f)&&!isNowRequest(0.0111111f)&&!isNowRequest(0.1f)&&!isNowRequest(1.0f),
          "...exactly 5 ms is already a prediction, and so is anything over it (a frame period is 11.1 ms)");
    check(isNowRequest(-0.001f)&&isNowRequest(-0.0049f)&&isNowRequest(-0.004999f)&&!isNowRequest(-0.005f)&&!isNowRequest(-0.0051f)&&!isNowRequest(-0.011f)&&!isNowRequest(-0.25f),
          "...on both signs: -4.9 ms is \"now\", -5 ms and the quarter second Elite's older callers pass are not");
    check(!isNowRequest(std::numeric_limits<float>::quiet_NaN())&&!isNowRequest(std::numeric_limits<float>::infinity())&&!isNowRequest(-std::numeric_limits<float>::infinity()),
          "a NaN and an infinity are not \"now\"");
  }
  // ---- whose call it is --------------------------------------------------------------------------------------------------------------------
  {
    check(answeredAtDisplayTime(0x4E3881,0.0f)&&answeredAtDisplayTime(0,0.0f)&&answeredAtDisplayTime(0x6000000,0.003f)&&answeredAtDisplayTime(kFrameUnknown-1,-0.004f),
          "a return address inside the image with a prediction of about 0 is answered one period past the display time: the build's own RVA is not asked");
    check(!answeredAtDisplayTime(0x4E3881,0.005f)&&!answeredAtDisplayTime(0x4E3881,-0.005f)&&!answeredAtDisplayTime(0x4E3881,0.25f)&&!answeredAtDisplayTime(0x4E3881,-0.25f)&&
              !answeredAtDisplayTime(0x4E3881,0.011f),
          "...a real prediction from inside the image is not (a caller that wants the future)");
    check(!answeredAtDisplayTime(kFrameOutside,0.0f)&&!answeredAtDisplayTime(kFrameUnknown,0.0f)&&!answeredAtDisplayTime(kFrameOutside,0.25f),
          "...a caller outside the image, or one whose address was not captured, never is");
    // The image: where an address is inside it.
    const ExeModule module{0x140000000ull,0x1000,0x1234,0x1000};
    check(frameRva(module,0x140000000ull)==0&&frameRva(module,0x140000FFFull)==0xFFF&&frameRva(module,0x140001000ull)==kFrameOutside&&frameRva(module,0x13FFFFFFFull)==kFrameOutside&&
              frameRva(module,0)==kFrameUnknown&&frameRva(ExeModule{},0x140000000ull)==kFrameOutside,
          "an address is inside the image from its base to the last byte of its size; one past either end, and any address when the image is unknown, is outside; null is not captured");
    check(answeredAtDisplayTime(frameRva(module,0x140000800ull),0.0f)&&!answeredAtDisplayTime(frameRva(module,0x140001000ull),0.0f)&&!answeredAtDisplayTime(frameRva(module,0x13FFFFFFFull),0.0f),
          "...so the filter holds at the image's edges");
  }
  // ---- the instant ---------------------------------------------------------------------------------------------------------------------------
  {
    int64_t t=-7;
    check(displayTimeTarget(5000000000,&t)&&t==5000000000&&displayTimeTarget(1,&t)&&t==1&&displayTimeTarget(INT64_MAX,&t)&&t==INT64_MAX,"the instant is the latest frame's display time");
    t=-7;
    check(!displayTimeTarget(0,&t)&&!displayTimeTarget(-1,&t)&&!displayTimeTarget(INT64_MIN,&t)&&t==-7,"with no frame yet (a display time that is not positive) there is none, and the output is left alone");
  }
  // ---- the instant: display time + one period ------------------------------------------------------------------------------------------------
  {
    constexpr int64_t period = 11111111, display = 5000000000, lead = 42000000;
    const auto add = [](int64_t shown, int64_t p, int64_t& out) { return nextPredictionTime(shown, p, out); };
    const auto never = [](int64_t, int64_t) { return false; };
    int64_t at = -7;
    check(answerTime(display, period, display - lead, add, never, &at) && at == display + period,
          "the instant is the latest frame's display time plus that frame's period (flight 3's now was 42 ms before the display time)");
    at = -7;
    check(answerTime(display, 13888889, display - lead, add, never, &at) && at == display + 13888889 && answerTime(display, 1, display, add, never, &at) && at == display + 1,
          "...whatever the period (72 Hz, 1 ns)");
    // The freshness test is on the CACHED frame: one period behind now is still good (the target is then now), a nanosecond more is not.
    at = -7;
    check(answerTime(display, period, display + period, add, never, &at) && at == display + period,
          "a cached frame exactly one period behind now still answers (judged on the frame's own display time, not on the target a period ahead of it)");
    at = -7;
    check(!answerTime(display, period, display + period + 1, add, never, &at) && !answerTime(display, period, display + 3 * period, add, never, &at) && at == -7,
          "...one nanosecond more, or three periods, it does not: no target, and the output is left alone");
    check(answerTime(display, period, display + period, add, never, &at) && !answerTime(display, period, display + period + 1, add, never, &at),
          "(the boundary is exactly one period, inclusive)");
    check(answerTime(display, 10000000, display + 10000000, add, never, &at) && at == display + 10000000 && !answerTime(display, 10000000, display + 10000001, add, never, &at),
          "(and it moves with the frame's own period)");
    // No frame, no period, overflow.
    at = -7;
    check(!answerTime(0, period, 1, add, never, &at) && !answerTime(-5, period, 1, add, never, &at) && !answerTime(INT64_MIN, period, 1, add, never, &at) && at == -7,
          "no display time (not positive): no target");
    check(!answerTime(0, period, 0, add, never, &at) && !answerTime(-5, period, -10, add, never, &at) && !answerTime(-1, period, INT64_MIN, add, never, &at) && at == -7,
          "...even when now is not ahead of it (a display time that is not positive is no display time, whatever the clock says)");
    check(!answerTime(display, 0, display, add, never, &at) && !answerTime(display, -1, display, add, never, &at) && !answerTime(display, INT64_MIN, display, add, never, &at) && at == -7,
          "a frame that reports no period (zero or negative) has no target: the call is located at now + prediction, a counted fallback");
    check(!answerTime(INT64_MAX - 5, period, INT64_MAX - 5, add, never, &at) && at == -7 && answerTime(INT64_MAX - period, period, INT64_MAX - period, add, never, &at) && at == INT64_MAX,
          "a display time so late that one more period overflows has no target and does not wrap; one that just fits has");
    // A reference-space change between the display time and the target: a pose there would be in another origin.
    const auto crossing = [&](int64_t from, int64_t to) { return from < display + 5000000 && display + 5000000 <= to; };
    at = -7;
    check(!answerTime(display, period, display - lead, add, crossing, &at) && at == -7, "a reference change between the display time and the target: no target");
    int64_t seenFrom = 0, seenTo = 0;
    check(answerTime(display, period, display - lead, add, [&](int64_t from, int64_t to) { seenFrom = from; seenTo = to; return false; }, &at) && seenFrom == display && seenTo == display + period,
          "...asked about exactly the span from the display time to the target");
    int64_t addCalls = 0;
    answerTime(display, period, display + 2 * period, [&](int64_t, int64_t, int64_t&) { ++addCalls; return true; }, never, &at);
    answerTime(0, period, 1, [&](int64_t, int64_t, int64_t&) { ++addCalls; return true; }, never, &at);
    check(addCalls == 0, "(a stale or absent frame never gets as far as adding a period)");
  }
  // ---- how long a display time stays good ----------------------------------------------------------------------------------------------------
  {
    constexpr int64_t period = 11111111, display = 5000000000;
    check(kPoseFrameMaxToleranceNs == 50000000, "the cap on the tolerance is 50 ms");
    check(poseFrameTolerance(period) == period && poseFrameTolerance(1) == 1 && poseFrameTolerance(kPoseFrameMaxToleranceNs) == kPoseFrameMaxToleranceNs,
          "the tolerance is one display period, as the frame reports it");
    check(poseFrameTolerance(kPoseFrameMaxToleranceNs + 1) == kPoseFrameMaxToleranceNs && poseFrameTolerance(INT64_MAX) == kPoseFrameMaxToleranceNs,
          "...capped at 50 ms, so a period that is not a display period cannot stretch it");
    check(poseFrameTolerance(0) == 0 && poseFrameTolerance(-1) == 0 && poseFrameTolerance(INT64_MIN) == 0, "...and no period (zero, negative) is no tolerance");
    check(displayTimeFresh(display, period, display - 42000000) && displayTimeFresh(display, period, display - 1) && displayTimeFresh(display, period, display),
          "a display time ahead of now, or exactly now, is good (the real case: now is about 42 ms before it)");
    check(displayTimeFresh(display, period, display + 1) && displayTimeFresh(display, period, display + period),
          "...and stays good until it is one display period behind now, inclusive");
    check(!displayTimeFresh(display, period, display + period + 1) && !displayTimeFresh(display, period, display + 2 * period) &&
              !displayTimeFresh(display, period, display + 5000000000),
          "...one nanosecond past that, or a second later, it is not");
    check(displayTimeFresh(display, 0, display) && !displayTimeFresh(display, 0, display + 1) && !displayTimeFresh(display, -5, display + 1),
          "with no period the display time must not be behind now at all");
    check(displayTimeFresh(display, INT64_MAX, display + kPoseFrameMaxToleranceNs) && !displayTimeFresh(display, INT64_MAX, display + kPoseFrameMaxToleranceNs + 1),
          "a huge period is held to the 50 ms cap");
    check(!displayTimeFresh(0, period, 1) && !displayTimeFresh(-7, period, 1) && displayTimeFresh(1, period, 1 + period) && !displayTimeFresh(1, period, 2 + period),
          "a display time that is not positive is never judged good once now is ahead of it; the smallest positive one follows the same rule as any other");
    check(displayTimeFresh(INT64_MAX, period, INT64_MAX) && displayTimeFresh(INT64_MAX, period, INT64_MAX - 1) && !displayTimeFresh(INT64_MAX - 3 * period, period, INT64_MAX),
          "the comparison does not overflow at the ends of the range");
  }
  // ---- why a call was or was not answered ---------------------------------------------------------------------------------------------------
  {
    constexpr int64_t period = 11111111, display = 5000000000;
    const auto add = [](int64_t shown, int64_t p, int64_t& out) { return nextPredictionTime(shown, p, out); };
    const auto never = [](int64_t, int64_t) { return false; };
    const auto always = [](int64_t, int64_t) { return true; };
    int64_t at = -7;
    check(answerTimeWhy(display, period, display - 1000, add, never, &at) == PoseAnswer::Answered && at == display + period, "the reason for an answered call is Answered, and the instant is written");
    at = -7;
    check(answerTimeWhy(0, period, 1, add, never, &at) == PoseAnswer::NoFrame && answerTimeWhy(-5, period, 1, add, never, &at) == PoseAnswer::NoFrame && at == -7,
          "no display time (not positive): NoFrame, the output left alone");
    check(answerTimeWhy(display, period, display + period + 1, add, never, &at) == PoseAnswer::Stale && at == -7, "a cached frame more than one period behind now: Stale");
    check(answerTimeWhy(display, 0, display, add, never, &at) == PoseAnswer::Period && answerTimeWhy(display, -1, display - 5, add, never, &at) == PoseAnswer::Period &&
              answerTimeWhy(INT64_MAX - 5, period, INT64_MAX - 5, add, never, &at) == PoseAnswer::Period && at == -7,
          "no period that is positive, or a sum that would overflow: Period");
    check(answerTimeWhy(display, 0, display + 1, add, never, &at) == PoseAnswer::Stale,
          "(with no period the display time must not be behind now at all, and one that is reads Stale: the freshness test comes before the period is added, as it always did)");
    check(answerTimeWhy(display, period, display - 1000, add, always, &at) == PoseAnswer::Cross && at == -7, "a reference-space change in between: Cross");
    check(answerTime(display, period, display - 1000, add, never, &at) && !answerTime(display, period, display - 1000, add, always, &at) &&
              !answerTime(0, period, 1, add, never, &at) && !answerTime(display, period, display + 2 * period, add, never, &at),
          "the yes/no answerTime is exactly 'the reason is Answered' (the shipped behaviour does not move)");
    check(std::string(poseAnswerName(PoseAnswer::Answered)) == "answered" && std::string(poseAnswerName(PoseAnswer::NoFrame)) == "none" &&
              std::string(poseAnswerName(PoseAnswer::Stale)) == "stale" && std::string(poseAnswerName(PoseAnswer::Period)) == "period" &&
              std::string(poseAnswerName(PoseAnswer::Cross)) == "cross" && std::string(poseAnswerName(PoseAnswer::Cap)) == "cap" &&
              std::string(poseAnswerName(PoseAnswer::Refused)) == "refused" && kPoseAnswers == 7,
          "the reasons are named as the log line prints them");
  }
  // ---- the slot instant (advanced.cull_pose_timing) --------------------------------------------------------------------------------------------
  {
    constexpr int64_t period = 11111111, display = 5000000000, lead = 42000000;
    const int64_t waitReturned = display - lead;   // the wait that produced the frame returned this long before its display time
    const auto never = [](int64_t, int64_t) { return false; };
    const auto add = [](int64_t shown, int64_t p, int64_t& out) { return nextPredictionTime(shown, p, out); };
    int64_t at = -7, k = -7;
    check(kSlotCapNs == 250000000, "the cap is 250 ms");
    // Steady flight: the call is made within a period of the wait that returned the frame, so the next slot is one period past its display time.
    check(slotAnswerTime(display, period, lead, waitReturned, never, &at, &k) == PoseAnswer::Answered && at == display + period && k == 1,
          "steady flight, a call just after the wait returned: k = 1, one period past the display time");
    int64_t next = -7;
    check(answerTime(display, period, waitReturned, add, never, &next) && next == at, "...exactly what `next` answers");
    check(slotAnswerTime(display, period, lead, waitReturned + 5000000, never, &at, &k) == PoseAnswer::Answered && at == display + period && k == 1,
          "...5 ms later: still k = 1 (e = display + 5 ms)");
    check(slotAnswerTime(display, period, lead, waitReturned + period, never, &at, &k) == PoseAnswer::Answered && at == display + period && k == 1,
          "...a call exactly one period after the wait returned (e = display + period): k = 1, the slot e falls on");
    check(slotAnswerTime(display, period, lead, waitReturned + period + 1, never, &at, &k) == PoseAnswer::Answered && at == display + 2 * period && k == 2,
          "...one nanosecond later: the next slot is the second, k = 2");
    // e <= D: a call before the wait could have returned (or a lead longer than the gap): the next slot is still the first, never the display time itself.
    check(slotAnswerTime(display, period, lead, display - 3 * lead, never, &at, &k) == PoseAnswer::Answered && at == display + period && k == 1,
          "e at or before the display time: k = 1, never fewer");
    check(slotAnswerTime(display, period, lead, display - lead, never, &at, &k) == PoseAnswer::Answered && at == display + period && k == 1, "e exactly the display time: k = 1");
    // A late frame: the call comes more than a period after the wait that produced the cached frame.
    check(slotAnswerTime(display, period, lead, waitReturned + period * 12 / 10, never, &at, &k) == PoseAnswer::Answered && at == display + 2 * period && k == 2,
          "LATE BY 1.2 PERIODS: k = 2, two periods past the display time (`next` would be a period short)");
    check(slotAnswerTime(display, period, lead, waitReturned + period * 25 / 10, never, &at, &k) == PoseAnswer::Answered && at == display + 3 * period && k == 3,
          "LATE BY 2.5 PERIODS: k = 3");
    check(slotAnswerTime(display, period, lead, waitReturned + 2 * period, never, &at, &k) == PoseAnswer::Answered && at == display + 2 * period && k == 2 &&
              slotAnswerTime(display, period, lead, waitReturned + 2 * period + 1, never, &at, &k) == PoseAnswer::Answered && at == display + 3 * period && k == 3,
          "a whole number of periods late rounds to that slot, a nanosecond more to the next (ceil)");
    // Where the freshness guard of `next` would have dropped the call to the wall clock, the slot answer has a slot of its own.
    int64_t unused = -7;
    check(answerTimeWhy(display, period, waitReturned + 5 * period, add, never, &unused) == PoseAnswer::Stale &&
              slotAnswerTime(display, period, lead, display + 3 * period, never, &at, &k) == PoseAnswer::Answered && k == 7 && at == display + 7 * period,
          "A STALE FRAME: `next` falls back (Stale), the slot answer does not -- 3 periods past the display time plus the 42 ms lead is the 7th slot");
    // The cap: a stall or a loading screen.
    const int64_t most = 22;   // 250 ms / 11.11 ms
    check(slotAnswerTime(display, period, lead, display - lead + most * period, never, &at, &k) == PoseAnswer::Answered && k == most && at == display + most * period,
          "the 22nd slot (244 ms) is the last the cap lets through");
    at = -7;
    check(slotAnswerTime(display, period, lead, display - lead + most * period + 1, never, &at, &k) == PoseAnswer::Cap && at == -7,
          "...one nanosecond past it is the 23rd (256 ms): Cap, the call falls back to now + prediction, the output left alone");
    check(slotAnswerTime(display, 13888889, lead, display - lead + 17 * 13888889LL, never, &at, &k) == PoseAnswer::Answered && k == 17 &&
              slotAnswerTime(display, 13888889, lead, display - lead + 17 * 13888889LL + 1, never, &at, &k) == PoseAnswer::Cap,
          "...at 72 Hz the cap is 17 slots (250 / 13.89 rounded down)");
    check(slotAnswerTime(display, 300000000, lead, display - lead, never, &at, &k) == PoseAnswer::Answered && k == 1 && at == display + 300000000 &&
              slotAnswerTime(display, 300000000, lead, display - lead + 300000001, never, &at, &k) == PoseAnswer::Cap,
          "a 'period' longer than the cap still gets its first slot (k = 1 is `next`) and no second");
    // No frame, no period, no lead.
    at = -7;
    check(slotAnswerTime(0, period, lead, 1, never, &at, &k) == PoseAnswer::NoFrame && slotAnswerTime(-5, period, lead, 1, never, &at, &k) == PoseAnswer::NoFrame &&
              slotAnswerTime(INT64_MIN, period, lead, 1, never, &at, &k) == PoseAnswer::NoFrame && at == -7,
          "no display time (not positive): NoFrame");
    check(slotAnswerTime(display, 0, lead, display, never, &at, &k) == PoseAnswer::Period && slotAnswerTime(display, -1, lead, display, never, &at, &k) == PoseAnswer::Period &&
              slotAnswerTime(display, INT64_MIN, lead, display, never, &at, &k) == PoseAnswer::Period && at == -7,
          "a frame that reports no period (zero or negative): Period");
    check(slotAnswerTime(display, period, 0, display, never, &at, &k) == PoseAnswer::Period && slotAnswerTime(display, period, -1, display, never, &at, &k) == PoseAnswer::Period &&
              slotAnswerTime(display, period, INT64_MIN, display, never, &at, &k) == PoseAnswer::Period && at == -7,
          "a lead that is not known or not positive (zero, negative): Period -- the call is located at now + prediction");
    // Overflow.
    check(slotAnswerTime(display, period, INT64_MAX, 1, never, &at, &k) == PoseAnswer::Period && slotAnswerTime(display, period, INT64_MAX - 5, 6, never, &at, &k) == PoseAnswer::Period &&
              at == -7,
          "now + lead that would overflow: Period, no wrap");
    check(slotAnswerTime(INT64_MAX - 5, period, lead, INT64_MAX - 5, never, &at, &k) == PoseAnswer::Period && at == -7 &&
              slotAnswerTime(INT64_MAX - 5, period, lead, 1, never, &at, &k) == PoseAnswer::Period && at == -7 &&
              slotAnswerTime(INT64_MAX - period, period, lead, INT64_MAX - period - lead, never, &at, &k) == PoseAnswer::Answered && at == INT64_MAX && k == 1,
          "a display time so late that one period more overflows: Period, no wrap (with now far from it, so only the sum overflows, and with now beside it); one that just fits is answered");
    check(slotAnswerTime(1, period, lead, INT64_MAX - lead, never, &at, &k) == PoseAnswer::Cap,
          "a now so far past the display time that the count would be astronomical is the cap, not an overflow");
    // The reference-space change between the display time and the slot.
    at = -7;
    const auto crossing = [&](int64_t from, int64_t to) { return from < display + 5000000 && display + 5000000 <= to; };
    check(slotAnswerTime(display, period, lead, waitReturned, crossing, &at, &k) == PoseAnswer::Cross && at == -7, "a reference change between the display time and the slot: Cross");
    int64_t seenFrom = 0, seenTo = 0;
    check(slotAnswerTime(display, period, lead, waitReturned + 2 * period + 1, [&](int64_t from, int64_t to) { seenFrom = from; seenTo = to; return false; }, &at, &k) == PoseAnswer::Answered &&
              seenFrom == display && seenTo == display + 3 * period,
          "...asked about exactly the span from the display time to the slot the call lands on");
    // The optional count may be left out.
    check(slotAnswerTime(display, period, lead, waitReturned, never, &at) == PoseAnswer::Answered && at == display + period, "(the count is optional)");
    // The freshness guard is not applied in this mode, and a display time behind now alone is no reason to fall back.
    check(slotAnswerTime(display, period, lead, display + 10 * period, never, &at, &k) == PoseAnswer::Answered && k > 1, "a display time ten periods behind now is answered at a later slot");
  }
  // ---- when the call arrived: the four classes ------------------------------------------------------------------------------------------------
  {
    check(classifyPoseArrival(5, 5, 5) == PoseArrival::Between && classifyPoseArrival(0, 0, 0) == PoseArrival::Between && classifyPoseArrival(9, 5, 5) == PoseArrival::Between,
          "the latest frame was already submitted when the call arrived (submitSeq >= publishSeq) and nothing was published while it queued: Between");
    check(classifyPoseArrival(5, 5, 6) == PoseArrival::Straddled && classifyPoseArrival(5, 5, 9) == PoseArrival::Straddled,
          "...and a publish happened while it queued: Straddled (it is served against the frame published during its wait)");
    check(classifyPoseArrival(4, 5, 5) == PoseArrival::Render && classifyPoseArrival(0, 1, 1) == PoseArrival::Render,
          "published but not yet submitted when it arrived (submitSeq < publishSeq), nothing published while it queued: Render");
    check(classifyPoseArrival(4, 5, 6) == PoseArrival::Other && classifyPoseArrival(0, 1, 3) == PoseArrival::Other,
          "...and a straddled Render call is Other");
    check(std::string(poseArrivalName(PoseArrival::Between)) == "between" && std::string(poseArrivalName(PoseArrival::Straddled)) == "straddled" &&
              std::string(poseArrivalName(PoseArrival::Render)) == "render" && std::string(poseArrivalName(PoseArrival::Other)) == "other" && kPoseArrivals == 4,
          "the classes are named as the log line prints them");
    check(poseTimingFromWord(0) == PoseTiming::Next && poseTimingFromWord(1) == PoseTiming::Slot && poseTimingFromWord(2) == PoseTiming::Arrival &&
              poseTimingFromWord(3) == PoseTiming::Next && poseTimingFromWord(0xFFFFFFFFu) == PoseTiming::Next,
          "the word the graphics half hands over: 0 next, 1 slot, 2 arrival; anything else reads next");
    check(std::string(poseTimingName(PoseTiming::Next)) == "next" && std::string(poseTimingName(PoseTiming::Slot)) == "slot" && std::string(poseTimingName(PoseTiming::Arrival)) == "arrival",
          "the timings are named as the log lines print them");
  }
  // ---- the three timings, each class --------------------------------------------------------------------------------------------------------
  {
    constexpr int64_t period = 11111111, display = 5000000000, lead = 42000000;
    const int64_t waitReturned = display - lead;
    const auto add = [](int64_t shown, int64_t p, int64_t& out) { return nextPredictionTime(shown, p, out); };
    const auto never = [](int64_t, int64_t) { return false; };
    const PoseArrival classes[] = {PoseArrival::Between, PoseArrival::Straddled, PoseArrival::Render, PoseArrival::Other};
    const PoseTiming timings[] = {PoseTiming::Next, PoseTiming::Slot, PoseTiming::Arrival};
    // answer(): the reason, the instant and the periods past the display time for one how and class.
    struct Got { PoseAnswer why; int64_t at, n; };
    const auto answerWith = [&](PoseTiming how, PoseArrival arrival, int64_t now, int64_t d, int64_t p, int64_t l) {
      Got g{PoseAnswer::Refused, -7, -7};
      g.why = timingAnswerTime(how, arrival, d, p, l, now, add, never, &g.at, &g.n);
      return g;
    };
    const auto answer = [&](PoseTiming how, PoseArrival arrival, int64_t now) { return answerWith(how, arrival, now, display, period, lead); };
    // On time (a call just after the wait returned): next and slot answer D + P for every class; arrival adds a period for Between only.
    bool steady = true;
    for (PoseTiming how : timings)
      for (PoseArrival c : classes) {
        const Got g = answer(how, c, waitReturned + 5000000);
        const int64_t extra = (how == PoseTiming::Arrival && c == PoseArrival::Between) ? 1 : 0;
        steady = steady && g.why == PoseAnswer::Answered && g.at == display + (1 + extra) * period && (how == PoseTiming::Next || g.n == 1 + extra);
      }
    check(steady, "ON TIME: next and slot answer D + P for every class (k = 1); arrival answers D + 2P for Between and D + P for Straddled, Render and Other");
    // Late by 1.2 periods (k = 2).
    bool late = true;
    for (PoseTiming how : timings)
      for (PoseArrival c : classes) {
        const Got g = answer(how, c, waitReturned + period * 12 / 10);
        const int64_t k = how == PoseTiming::Next ? 1 : 2;
        const int64_t extra = (how == PoseTiming::Arrival && c == PoseArrival::Between) ? 1 : 0;
        late = late && g.why == PoseAnswer::Answered && g.at == display + (k + extra) * period;
      }
    check(late, "LATE BY 1.2 PERIODS: next still D + P (its guard is not yet stale), slot D + 2P, arrival D + 3P for Between and D + 2P for the rest");
    // Stale: next gives up (a counted fallback), slot and arrival go on.
    bool stale = true;
    for (PoseArrival c : classes) {
      stale = stale && answer(PoseTiming::Next, c, display + 3 * period).why == PoseAnswer::Stale;
      const Got s = answer(PoseTiming::Slot, c, display + 3 * period);
      const Got a = answer(PoseTiming::Arrival, c, display + 3 * period);
      stale = stale && s.why == PoseAnswer::Answered && s.at == display + 7 * period && a.why == PoseAnswer::Answered &&
              a.at == display + (c == PoseArrival::Between ? 8 : 7) * period;
    }
    check(stale, "A STALE FRAME (3 periods behind now): next falls back for every class; slot answers the 7th slot; arrival the 8th for Between, the 7th for the rest");
    // The cap is judged on k, before the extra period.
    bool cap = true;
    const int64_t nowAtMost = waitReturned + 22 * period;   // k = 22: the last slot the cap lets through
    for (PoseArrival c : classes) {
      const Got s = answer(PoseTiming::Slot, c, nowAtMost), s2 = answer(PoseTiming::Slot, c, nowAtMost + 1);
      const Got a = answer(PoseTiming::Arrival, c, nowAtMost), a2 = answer(PoseTiming::Arrival, c, nowAtMost + 1);
      cap = cap && s.why == PoseAnswer::Answered && s.at == display + 22 * period && s2.why == PoseAnswer::Cap && s2.at == -7 &&
            a.why == PoseAnswer::Answered && a.at == display + (c == PoseArrival::Between ? 23 : 22) * period && a2.why == PoseAnswer::Cap && a2.at == -7;
    }
    check(cap, "THE CAP: the 22nd slot is the last for slot and arrival, the 23rd is a Cap fallback for every class; arrival's extra period may go one past it (the cap is judged on k)");
    // Overflow on the extra period.
    const Got fits = answerWith(PoseTiming::Slot, PoseArrival::Between, 1, INT64_MAX - period, period, lead);
    const Got extraOverflows = answerWith(PoseTiming::Arrival, PoseArrival::Between, 1, INT64_MAX - period, period, lead);
    const Got renderFits = answerWith(PoseTiming::Arrival, PoseArrival::Render, 1, INT64_MAX - period, period, lead);
    check(fits.why == PoseAnswer::Answered && fits.at == INT64_MAX && extraOverflows.why == PoseAnswer::Period && extraOverflows.at == -7 &&
              renderFits.why == PoseAnswer::Answered && renderFits.at == INT64_MAX,
          "OVERFLOW ON THE EXTRA PERIOD: D + P just fits and is answered (slot, and arrival for a Render call); D + 2P for a Between call under arrival overflows: Period, no wrap");
    const int64_t huge = INT64_MAX / 2 + 1;
    const Got hugeSlot = answerWith(PoseTiming::Slot, PoseArrival::Between, 1, 1, huge, lead);
    const Got hugeArrival = answerWith(PoseTiming::Arrival, PoseArrival::Between, 1, 1, huge, lead);
    check(hugeSlot.why == PoseAnswer::Answered && hugeSlot.at == 1 + huge && hugeArrival.why == PoseAnswer::Period && hugeArrival.at == -7,
          "...and a period so long that two of them cannot be multiplied: Period");
    // The crossing check covers the final span.
    int64_t seenFrom = 0, seenTo = 0;
    const auto recording = [&](int64_t from, int64_t to) { seenFrom = from; seenTo = to; return false; };
    int64_t at = -7, n = -7;
    timingAnswerTime(PoseTiming::Arrival, PoseArrival::Between, display, period, lead, waitReturned + 5000000, add, recording, &at, &n);
    const bool spanBetween = seenFrom == display && seenTo == display + 2 * period;
    timingAnswerTime(PoseTiming::Arrival, PoseArrival::Render, display, period, lead, waitReturned + 5000000, add, recording, &at, &n);
    const bool spanRender = seenFrom == display && seenTo == display + period;
    timingAnswerTime(PoseTiming::Slot, PoseArrival::Between, display, period, lead, waitReturned + 5000000, add, recording, &at, &n);
    const bool spanSlot = seenFrom == display && seenTo == display + period;
    check(spanBetween && spanRender && spanSlot, "the crossing check is asked about exactly the span from D to the final instant: D + 2P for Between under arrival, D + P otherwise");
    const int64_t change = display + period + 1;   // a reference change between D + P and D + 2P
    const auto crossing = [&](int64_t from, int64_t to) { return from < change && change <= to; };
    at = -7;
    const PoseAnswer slotOk = timingAnswerTime(PoseTiming::Slot, PoseArrival::Between, display, period, lead, waitReturned + 5000000, add, crossing, &at, &n);
    const bool slotAt = at == display + period;
    at = -7;
    const PoseAnswer arrivalCross = timingAnswerTime(PoseTiming::Arrival, PoseArrival::Between, display, period, lead, waitReturned + 5000000, add, crossing, &at, &n);
    const bool arrivalAt = at == -7;
    const PoseAnswer renderOk = timingAnswerTime(PoseTiming::Arrival, PoseArrival::Render, display, period, lead, waitReturned + 5000000, add, crossing, &at, &n);
    check(slotOk == PoseAnswer::Answered && slotAt && arrivalCross == PoseAnswer::Cross && arrivalAt && renderOk == PoseAnswer::Answered,
          "a reference-space change between D + P and D + 2P: slot and a Render call are answered, a Between call under arrival (which reaches past it) is a Cross fallback");
    // No frame, no period, no lead.
    bool bare = true;
    for (PoseArrival c : classes) {
      bare = bare && answerWith(PoseTiming::Next, c, 1, 0, period, lead).why == PoseAnswer::NoFrame && answerWith(PoseTiming::Slot, c, 1, 0, period, lead).why == PoseAnswer::NoFrame &&
             answerWith(PoseTiming::Arrival, c, 1, 0, period, lead).why == PoseAnswer::NoFrame;
      bare = bare && answerWith(PoseTiming::Slot, c, display, display, 0, lead).why == PoseAnswer::Period && answerWith(PoseTiming::Arrival, c, display, display, 0, lead).why == PoseAnswer::Period;
      bare = bare && answerWith(PoseTiming::Slot, c, display, display, period, 0).why == PoseAnswer::Period && answerWith(PoseTiming::Arrival, c, display, display, period, 0).why == PoseAnswer::Period;
      bare = bare && answerWith(PoseTiming::Next, c, display - 1000, display, period, 0).why == PoseAnswer::Answered;   // next never needed the lead
    }
    check(bare, "no display time: NoFrame in every timing; no period, or no lead: Period for slot and arrival (the call is located at now + prediction); next never needed a lead");
  }
  // ---- the lines -----------------------------------------------------------------------------------------------------------------------------
  {
    HeadPoseSightings s;
    std::vector<std::string> lines;
    const auto sink=[&](const char* l){lines.emplace_back(l);};
    s.note(0x4E3881,sink);
    check(lines.size()==1&&lines[0]=="head pose: Elite's \"now\" requests are answered one display period after the drawn frame's display time (first from exe+0x4E3881)"&&s.distinct()==1,
          "the first caller answered writes the line (the exact text), with the return RVA");
    s.note(0x4E3881,sink);s.note(0x4E3881,sink);
    check(lines.size()==1,"...the same caller again writes nothing");
    s.note(0x2A03F51,sink);
    check(lines.size()==2&&lines[1]=="head pose: another Elite caller of \"now\" is answered one display period after the drawn frame's display time (exe+0x2A03F51)"&&s.distinct()==2,
          "a further distinct return address writes one more line, named (a caller that appears after a game update)");
    for(uint32_t i=0;i<6;++i)s.note(0x100+i,sink);
    check(lines.size()==8&&s.distinct()==8,"(six more distinct callers: eight lines in all so far)");
    s.note(0x200,sink);
    check(lines.size()==9&&lines[8]=="head pose: another Elite caller of \"now\" is answered one display period after the drawn frame's display time (exe+0x200)"&&s.distinct()==9,
          "the eighth FURTHER caller (the ninth in all) is still named");
    s.note(0x300,sink);
    check(lines.size()==10&&lines[9]=="head pose: more than 8 further callers of \"now\"; the rest are not logged"&&s.distinct()==9,
          "the next distinct caller finds the cap: one line says the rest are not logged, and it is not remembered");
    s.note(0x301,sink);s.note(0x300,sink);s.note(0x4E3881,sink);s.note(0x200,sink);
    check(lines.size()==10,"...which is said once; later callers, new or old, write nothing");
  }
}
}  // namespace head_pose_time_cases
