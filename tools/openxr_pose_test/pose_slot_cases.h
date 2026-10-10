#pragma once
// The pose-slot diagnostic (src/openxr/pose_slot.h; docs\terrain-culling.md, 2026-10-10): the classification of a call against the frame that came
// next (at the quarter-period and 1.5-period boundaries, on both signs), the reasons a call fell back, the angle thresholds, the pending store and
// its overflow, the unresolved counts, the window and the mode split, the line (exact text), and tools\pose_slot_fixture.log, which the log tool's
// --self-test reads, held here to exactly what the writer writes.
#include "../../src/openxr/pose_slot.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace pose_slot_cases {
using namespace edvr::openxr;

struct Lines {
  std::vector<std::string> v;
  void operator()(const char* s) { v.emplace_back(s); }
};

inline void yaw(double degrees, float q[4]) {
  const double h = 3.14159265358979323846 / 180.0 * 0.5;
  q[0] = 0; q[1] = float(std::sin(degrees * h)); q[2] = 0; q[3] = float(std::cos(degrees * h));
}

// tools\pose_slot_fixture.log: 1700 frames at 72 Hz (13.89 ms of display time each, 200 ms apart on the window clock, so 300 frames fill a 60 s window), the
// head turning at 100 degrees a second, every tenth frame late (the display skips a slot). Elite's game thread (24212) makes one call a frame; a second thread
// (7001) makes one a frame and every seventh is a fallback (stale under `next`, the cap under `slot`; one cross). Under `next` a call before a late frame is
// located one period past the display time and the late frame is shown two periods past it: a slot short (1.39 degrees); under `slot` the call is located at
// the slot the late frame took and matches. Frames 0-599 are `next` (two windows), 600-999 `slot` (a window, then one cut short by the flip back), 1000-1699
// `next` again (two windows, then the last 100 frames, 20 s, written by the end-of-run flush): seven windows, fourteen lines.
inline std::string fixtureLog() {
  constexpr int64_t P = 13888889;
  constexpr double omega = 100.0;   // degrees a second
  PoseSlotStats stats;
  Lines lines;
  uint64_t ms = 1000, late = 0;
  int64_t D = 1000000000;
  const auto yawAt = [&](int64_t t, float q[4]) { yaw(std::fmod(omega * double(t) / 1e9, 360.0), q); };
  stats.setMode(false, late, ms, lines);
  stats.flushIfDue(ms, late, lines);   // the first call only opens a window
  const int frames = 1700;
  for (int f = 0; f < frames; ++f) {
    const bool slotNow = f >= 599 && f < 999;   // the mode the NEXT frame's calls are made under
    const bool nextLate = (f % 10) == 9;
    const int64_t T1 = D + (stats.on() && nextLate ? 2 : 1) * P;
    float q[4];
    yawAt(T1, q);
    stats.noteCall(24212, T1, PoseAnswer::Answered, q);
    if (f % 7 == 3) {
      const int64_t now = D - P;   // the wall clock: older than any slot
      yawAt(now, q);
      stats.noteCall(7001, now, stats.on() ? PoseAnswer::Cap : PoseAnswer::Stale, q);
    } else if (f == 100) {
      yawAt(D, q);
      stats.noteCall(7001, D, PoseAnswer::Cross, q);
    } else {
      yawAt(T1, q);
      stats.noteCall(7001, T1, PoseAnswer::Answered, q);
    }
    const int64_t next = D + (nextLate ? 2 : 1) * P;
    if (nextLate) ++late;
    ms += 200;
    float drawn[4];
    yawAt(next, drawn);
    stats.resolve(next, P, drawn);
    D = next;
    stats.setMode(slotNow, late, ms, lines);
    stats.flushIfDue(ms, late, lines);
  }
  stats.flushNow(ms, late, lines);
  std::string out;
  for (const auto& l : lines.v) out += "[00:00:00.000] " + l + "\n";
  return out;
}

template <class Check>
void runPoseSlotCases(Check&& check) {
  const auto close = [](double a, double b, double eps) { return std::fabs(a - b) <= eps; };
  constexpr int64_t P = 11111112;       // divisible by 8: a quarter period is 2777778, one and a half periods 16666668
  constexpr int64_t D = 7000000000;
  // ---- the classification of a call against the frame that came next -------------------------------------------------------------------------
  {
    unsigned kind = 9;
    bool oneP = false;
    const auto at = [&](int64_t delta, unsigned wantKind, bool wantOneP) {
      kind = 9; oneP = false;
      return classifyPoseDelta(D + delta, D, P, &kind, &oneP) && kind == wantKind && (wantKind == 0 || oneP == wantOneP);
    };
    check(at(0, 0, false), "delta 0 is a match");
    check(at(2777777, 0, false) && at(-2777777, 0, false), "a quarter period less a nanosecond, either way, is a match");
    check(at(-2777778, 1, true), "exactly a quarter period short is short (delta <= -P/4), within one period class");
    check(at(2777778, 2, true), "exactly a quarter period long is long (delta >= +P/4)");
    check(at(-P, 1, true) && at(P, 2, true), "one period short or long is 1p");
    check(at(-16666667, 1, true) && at(16666667, 2, true), "one and a half periods less a nanosecond is still 1p");
    check(at(-16666668, 1, false) && at(16666668, 2, false), "one and a half periods is 2p+");
    check(at(-2 * P, 1, false) && at(2 * P, 2, false) && at(-5 * P, 1, false), "two periods or more is 2p+");
    check(at(-(P / 4) / 2, 0, false), "(an eighth of a period is a match)");
    check(!classifyPoseDelta(D, D, 0, &kind, &oneP) && !classifyPoseDelta(D, D, -1, &kind, &oneP), "a next frame with no usable period cannot be compared");
    check(!classifyPoseDelta(INT64_MIN, 5, P, &kind, &oneP) && !classifyPoseDelta(INT64_MAX, -5, P, &kind, &oneP), "...nor can a subtraction that would overflow");
    check(classifyPoseDelta(INT64_MAX, 1, P, &kind, &oneP) && kind == 2 && !oneP && classifyPoseDelta(1, INT64_MAX, P, &kind, &oneP) && kind == 1 && !oneP,
          "...and the largest differences that do not are classed without wrapping");
    check(classifyPoseDelta(D, D, 1, &kind, &oneP) && kind == 0, "a period of 1 ns: delta 0 matches");
  }
  // ---- one window, call by call --------------------------------------------------------------------------------------------------------------
  {
    PoseSlotStats s;
    Lines lines;
    float q0[4], q[4];
    yaw(0.0, q0);
    // A frame is published (D + P); the calls made before it are located at the instants below and returned the orientations below.
    const int64_t next = D + P;
    const auto call = [&](uint32_t tid, int64_t delta, PoseAnswer why, double angle) {
      yaw(angle, q);
      s.noteCall(tid, next + delta, why, q);
    };
    s.setMode(false, 0, 1000, lines);
    s.flushIfDue(1000, 0, lines);
    call(1, 0, PoseAnswer::Answered, 0.5);                                         // match, below 1 degree
    call(1, 2777777, PoseAnswer::Answered, 1.5);                                   // match, above 1
    call(1, -P, PoseAnswer::Answered, 2.5);                                        // short 1p, above 1 and 2
    call(1, -2 * P, PoseAnswer::Answered, 0.9999);                                 // short 2p+, just below 1
    call(1, P, PoseAnswer::Answered, 1.2);                                         // long 1p, above 1
    call(1, 3 * P, PoseAnswer::Answered, 3.0);                                     // long 2p+, above 1 and 2
    call(1, -3 * P - 5, PoseAnswer::Stale, 4.0);                                   // fallback stale, short by delta, above 1 and 2
    call(1, -P / 2, PoseAnswer::NoFrame, 1.1);                                     // fallback none, short by delta
    call(1, 0, PoseAnswer::Period, 0.2);                                           // fallback period, match by delta
    call(1, P, PoseAnswer::Cross, 0.0);                                            // fallback cross, long by delta
    call(1, -3 * P, PoseAnswer::Cap, 1.9999);                                      // fallback cap, short by delta, above 1 but not above 2
    call(1, -P, PoseAnswer::Refused, 2.0001);                                      // fallback refused, above 2
    s.noteCall(1, next, PoseAnswer::Answered, nullptr);                            // a match whose pose was not valid: counted, no angle
    check(s.pending() == 13, "thirteen calls are waiting for a next frame");
    yaw(0.0, q0);
    s.resolve(next, P, q0);
    check(s.pending() == 0, "the next frame resolves every one of them");
    const PoseSlotCounts* c = s.countsFor(1);
    check(c && c->calls == 13 && c->unresolved == 0, "13 calls, none unresolved");
    check(c && c->cls[kSlotMatch] == 3 && c->cls[kSlotShort1] == 1 && c->cls[kSlotShort2] == 1 && c->cls[kSlotLong1] == 1 && c->cls[kSlotLong2] == 1,
          "match 3 (one with no angle), short 1p 1, short 2p+ 1, long 1p 1, long 2p+ 1");
    check(c && c->cls[kSlotFallbackFirst] == 1 && c->cls[kSlotFallbackFirst + 1] == 1 && c->cls[kSlotFallbackFirst + 2] == 1 && c->cls[kSlotFallbackFirst + 3] == 1 &&
              c->cls[kSlotFallbackFirst + 4] == 1 && c->cls[kSlotFallbackFirst + 5] == 1,
          "one fallback of each reason: none, stale, period, cross, cap, refused");
    check(c && c->fallbackDelta[0] == 1 && c->fallbackDelta[1] == 4 && c->fallbackDelta[2] == 1,
          "...also classified by their delta: one match (period), four short (none, stale, cap, refused), one long (cross)");
    check(c && c->over1[kSlotMatch] == 1 && c->over2[kSlotMatch] == 0 && c->over1[kSlotShort1] == 1 && c->over2[kSlotShort1] == 1 && c->over1[kSlotShort2] == 0 &&
              c->over1[kSlotLong1] == 1 && c->over2[kSlotLong1] == 0 && c->over1[kSlotLong2] == 1 && c->over2[kSlotLong2] == 1,
          "the angle thresholds: above 1 degree and above 2 degrees, per class (0.9999 is below 1, 1.2 above 1 only)");
    check(c && c->over1[kSlotFallbackFirst + 1] == 1 && c->over2[kSlotFallbackFirst + 1] == 1 && c->over1[kSlotFallbackFirst] == 1 && c->over2[kSlotFallbackFirst] == 0 &&
              c->over1[kSlotFallbackFirst + 2] == 0 && c->over1[kSlotFallbackFirst + 3] == 0 && c->over1[kSlotFallbackFirst + 4] == 1 && c->over2[kSlotFallbackFirst + 4] == 0 &&
              c->over1[kSlotFallbackFirst + 5] == 1 && c->over2[kSlotFallbackFirst + 5] == 1,
          "...by fallback reason too: stale 4.0 above both, none 1.1 above 1, cap 1.9999 above 1 and not above 2, refused 2.0001 above both");
    check(c && c->maxKnown && close(c->maxAngle, 4.0, 1e-3) && c->maxClass == kSlotFallbackFirst + 1 && std::string(poseSlotClassName(c->maxClass)) == "fallback-stale",
          "the largest angle is 4.0 degrees, and its class is fallback-stale");
    check(c && c->calls == c->unresolved + [&] { uint64_t n = 0; for (unsigned i = 0; i < kSlotClasses; ++i) n += c->cls[i]; return n; }(),
          "calls = resolved classes + unresolved: the totals add up");
    s.flushNow(2000, 7, lines);
    check(lines.v.size() == 1, "one line for the one caller");
    check(!lines.v.empty() && lines.v[0] ==
              "pose slot: tid 1 mode next calls 13 match 3 short 2 (1p 1, 2p+ 1) long 2 (1p 1, 2p+ 1) fallback 6 (none 1, stale 1, period 1, cross 1, cap 1, refused 1; "
              "delta match 1, short 4, long 1) unresolved 0; >1deg match 1 short 1 (1p 1, 2p+ 0) long 2 (1p 1, 2p+ 1) fallback 4 (none 1, stale 1, period 0, cross 0, cap 1, refused 1); "
              ">2deg match 0 short 1 (1p 1, 2p+ 0) long 1 (1p 0, 2p+ 1) fallback 2 (none 0, stale 1, period 0, cross 0, cap 0, refused 1); "
              "max 4.000 deg (fallback-stale); late frames 7; window 1.0 s",
          "the line, exact: the classes, the fallbacks by reason and by delta, the >1 and >2 degree counts, the largest angle and its class, the late frames");
  }
  // ---- unresolved: a frame that never came, a dropped cache, an unusable next frame ------------------------------------------------------------
  {
    PoseSlotStats s;
    Lines lines;
    float q[4];
    yaw(0.0, q);
    s.noteCall(5, 100, PoseAnswer::Answered, q);
    s.noteCall(5, 200, PoseAnswer::NoFrame, q);
    s.dropPending();
    check(s.pending() == 0 && s.countsFor(5) && s.countsFor(5)->calls == 2 && s.countsFor(5)->unresolved == 2, "a dropped cache leaves its pending calls unresolved, none of them classed");
    s.noteCall(5, 300, PoseAnswer::Answered, q);
    s.resolve(D, 0, q);
    check(s.countsFor(5)->unresolved == 3 && s.countsFor(5)->cls[kSlotMatch] == 0, "a next frame with no usable period leaves its calls unresolved");
    s.noteCall(5, 300, PoseAnswer::Answered, q);
    s.resolve(0, P, q);
    check(s.countsFor(5)->unresolved == 4, "...and so does one with no display time");
    s.noteCall(5, D, PoseAnswer::Answered, q);
    s.flushNow(1, 0, lines);
    check(lines.v.size() == 1 && lines.v[0].find(" calls 5 ") != std::string::npos && lines.v[0].find(" unresolved 5;") != std::string::npos,
          "a call still waiting at the end of the run is unresolved in the last line (the end-of-run flush)");
    check(lines.v[0].find("max n/a;") != std::string::npos, "...a window with no angle at all reads max n/a");
  }
  // ---- the pending store ---------------------------------------------------------------------------------------------------------------------
  {
    PoseSlotStats s;
    float q[4];
    yaw(0.0, q);
    for (unsigned i = 0; i < kPoseSlotPending + 3; ++i) s.noteCall(9, D + P, PoseAnswer::Answered, q);
    check(s.pending() == kPoseSlotPending && s.overflowed() == 3 && s.countsFor(9)->calls == kPoseSlotPending + 3 && s.countsFor(9)->unresolved == 3,
          "the pending store holds 256 calls; three more are counted, as calls and as unresolved, and counted as overflowed");
    s.resolve(D + P, P, q);
    check(s.pending() == 0 && s.countsFor(9)->cls[kSlotMatch] == kPoseSlotPending && s.countsFor(9)->unresolved == 3, "...and the 256 held are resolved");
    s.noteCall(9, D + P, PoseAnswer::Answered, q);
    check(s.pending() == 1, "(the store is usable again)");
  }
  // ---- the callers kept per window -----------------------------------------------------------------------------------------------------------
  {
    PoseSlotStats s;
    Lines lines;
    float q[4];
    yaw(0.0, q);
    for (uint32_t t = 100; t < 100 + kPoseSlotThreads; ++t) s.noteCall(t, D, PoseAnswer::Answered, q);
    s.noteCall(999, D, PoseAnswer::Answered, q);
    s.noteCall(998, D, PoseAnswer::Answered, q);
    check(s.threads() == kPoseSlotThreads && s.droppedCalls() == 2 && s.pending() == kPoseSlotThreads, "eight callers are kept; a ninth and tenth are counted, not stored");
    s.resolve(D, P, q);
    s.flushNow(1, 0, lines);
    check(lines.v.size() == kPoseSlotThreads + 1 && lines.v.back() == "pose slot: more than 8 callers in a window; 2 calls not counted",
          "...and said in a line after the callers (the exact text)");
    check(s.threads() == 0 && s.droppedCalls() == 0, "(the window is empty after the flush)");
  }
  // ---- the window, the late frames and the mode split ----------------------------------------------------------------------------------------
  {
    PoseSlotStats s;
    Lines lines;
    float q[4];
    yaw(0.0, q);
    check(!s.on(), "the instrument starts in the shipped mode");
    s.setMode(false, 5, 1000, lines);
    s.flushIfDue(1000, 5, lines);
    s.noteCall(1, D, PoseAnswer::Answered, q);
    s.resolve(D, P, q);
    s.flushIfDue(60999, 9, lines);
    check(lines.v.empty(), "a window under 60 s old writes nothing");
    s.flushIfDue(61000, 12, lines);
    check(lines.v.size() == 1 && lines.v[0] ==
              "pose slot: tid 1 mode next calls 1 match 1 short 0 (1p 0, 2p+ 0) long 0 (1p 0, 2p+ 0) fallback 0 (none 0, stale 0, period 0, cross 0, cap 0, refused 0; "
              "delta match 0, short 0, long 0) unresolved 0; >1deg match 0 short 0 (1p 0, 2p+ 0) long 0 (1p 0, 2p+ 0) fallback 0 (none 0, stale 0, period 0, cross 0, cap 0, refused 0); "
              ">2deg match 0 short 0 (1p 0, 2p+ 0) long 0 (1p 0, 2p+ 0) fallback 0 (none 0, stale 0, period 0, cross 0, cap 0, refused 0); max 0.000 deg (match); late frames 7; window 60.0 s",
          "...and at 60 s it writes its line, with the late frames since the window opened (12 - 5); a window whose only call matched writes every count, zeros included");
    s.flushIfDue(121000, 20, lines);
    check(lines.v.size() == 1, "a window with no calls writes nothing");
    // The mode flips in the middle of a window.
    s.noteCall(1, D, PoseAnswer::Answered, q);
    s.noteCall(2, D, PoseAnswer::Answered, q);
    s.resolve(D, P, q);
    s.setMode(true, 25, 130000, lines);
    check(s.on() && lines.v.size() == 3 && lines.v[1].find("tid 1 mode next calls 1 ") != std::string::npos && lines.v[2].find("tid 2 mode next calls 1 ") != std::string::npos &&
              lines.v[1].find("late frames 5") != std::string::npos,
          "a mode change closes the window at the change: its lines are written under the OLD mode, with the late frames up to the change (25 - 20)");
    s.noteCall(1, D, PoseAnswer::Answered, q);
    s.resolve(D, P, q);
    s.flushIfDue(189999, 30, lines);
    check(lines.v.size() == 3, "the next window opened at the change, not at the old window's start");
    s.flushIfDue(190000, 31, lines);
    check(lines.v.size() == 4 && lines.v[3].find("tid 1 mode slot calls 1 ") != std::string::npos && lines.v[3].find("late frames 6") != std::string::npos,
          "...and is written under the new mode 60 s later (31 - 25)");
    s.setMode(true, 40, 200000, lines);
    check(lines.v.size() == 4, "setting the mode it is in changes nothing");
  }
  // ---- the lines of the scripted flight ------------------------------------------------------------------------------------------------------
  {
    const std::string built = fixtureLog();
    std::string fixture;
    FILE* f = nullptr;
    if (fopen_s(&f, "tools/pose_slot_fixture.log", "rb") == 0 && f) {
      char buffer[4096];
      size_t n;
      while ((n = std::fread(buffer, 1, sizeof(buffer), f)) > 0) fixture.append(buffer, n);
      std::fclose(f);
    }
    std::string normalised;
    for (char ch : fixture) if (ch != '\r') normalised += ch;
    check(!fixture.empty(), "tools/pose_slot_fixture.log is readable from the repo root");
    check(normalised == built, "the log tool's fixture file is what the aggregator writes for the scripted flight, byte for byte (python tools\\edvr_log.py --tally pose reads it; regenerate with --print-slot-fixture)");
    size_t lineCount = 0;
    for (char ch : built) if (ch == '\n') ++lineCount;
    check(lineCount == 14 && built.find(" mode slot ") != std::string::npos && built.find(" mode next ") != std::string::npos,
          "...and it carries 14 lines in both modes: two callers over seven windows (the fourth cut short by the flip back, the last by the end of the run)");
  }
}
}  // namespace pose_slot_cases
