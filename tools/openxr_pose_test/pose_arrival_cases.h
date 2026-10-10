#pragma once
// The pose-arrival diagnostic (src/openxr/pose_arrival.h; docs\terrain-culling.md, 2026-10-10): the bucket of a call against the first frame published
// after it arrived (at the half-period boundaries, on both signs), each arrival class counted, timed and resolved (a Straddled or Other call at once against
// the frame cached when it was served, a Between or Render call at the next publish), the fallbacks and unresolved calls, the angle thresholds, the pending
// store and its overflow, the window and the mode split (three timings), the line (exact text), and tools\pose_arrival_fixture.log, which the log tool's
// --self-test reads, held here to exactly what the writer writes.
#include "../../src/openxr/pose_arrival.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace pose_arrival_cases {
using namespace edvr::openxr;

struct Lines {
  std::vector<std::string> v;
  void operator()(const char* s) { v.emplace_back(s); }
};

inline void yaw(double degrees, float q[4]) {
  const double h = 3.14159265358979323846 / 180.0 * 0.5;
  q[0] = 0; q[1] = float(std::sin(degrees * h)); q[2] = 0; q[3] = float(std::cos(degrees * h));
}

// tools\pose_arrival_fixture.log: the same 1700 frames at 72 Hz as tools\pose_slot_fixture.log (200 ms apart on the window clock, every tenth frame late, the head
// turning at 100 degrees a second, `next` for frames 0-599, `arrival` for 600-999 -- a window, then one cut short by the flip back -- and `next` again to the end,
// the last 100 frames written by the end-of-run flush), seen by the arrival instrument. Elite's game thread (24212) makes one call a frame: a Straddled one (served
// after the publish, 6 to 8 ms in the queue) on every fourth frame, a Between one (0.3 ms) on the next two, a Render one (0.1 ms) on the fourth. A second thread
// (7001) makes a Between call every frame, one in five a fallback. Under `next` a Between call reads 0p on an on-time frame and -1p- before a late one, a
// Straddled one +1p, a Render one 0p/-1p-; under `arrival` the Between calls read +1p (0p before a late frame, which the slot arithmetic cannot see coming)
// and the others as before. 100 deg/s times the period is 1.39 degrees: every +1p and -1p- call is above 1 degree.
inline std::string fixtureLog() {
  constexpr int64_t P = 13888889;
  constexpr double omega = 100.0;   // degrees a second
  PoseArrivalStats stats;
  Lines lines;
  uint64_t ms = 1000, late = 0;
  int64_t D = 1000000000;
  const auto yawAt = [&](int64_t t, float q[4]) { yaw(std::fmod(omega * double(t) / 1e9, 360.0), q); };
  stats.setMode(PoseTiming::Next, late, ms, lines);
  stats.flushIfDue(ms, late, lines);   // the first call only opens a window
  const int frames = 1700;
  for (int f = 0; f < frames; ++f) {
    const bool nextLate = (f % 10) == 9;
    const bool arrivalMode = stats.mode() == PoseTiming::Arrival;
    float q[4];
    // Calls served before the next publish (Between and Render): located relative to the cached frame D.
    const int64_t tBetween = D + (arrivalMode ? 2 : 1) * P, tRender = D + P;
    const int cls = f % 4;   // 0 Straddled, 1 and 2 Between, 3 Render for thread 24212
    if (cls == 1 || cls == 2) { yawAt(tBetween, q); stats.noteCall(24212, PoseArrival::Between, 0.3, tBetween, true, q, nullptr); }
    if (cls == 3) { yawAt(tRender, q); stats.noteCall(24212, PoseArrival::Render, 0.1, tRender, true, q, nullptr); }
    if (f % 5 == 0) { yawAt(D - P, q); stats.noteCall(7001, PoseArrival::Between, 0.4, D - P, false, q, nullptr); }
    else { yawAt(tBetween, q); stats.noteCall(7001, PoseArrival::Between, 0.4, tBetween, true, q, nullptr); }
    // The publish of the next frame: its display time, and the pose it is drawn with.
    const int64_t next = D + (nextLate ? 2 : 1) * P;
    if (nextLate) ++late;
    ms += 200;
    float drawn[4];
    yawAt(next, drawn);
    stats.resolve(next, P, drawn);
    D = next;
    // A call that arrived before the publish and was served after it: compared at once with the frame cached when it was served.
    if (cls == 0) {
      const int64_t t = D + P;
      yawAt(t, q);
      PoseArrivalFrame served;
      served.display = D; served.period = P; served.q = drawn;
      stats.noteCall(24212, PoseArrival::Straddled, 6.0 + double(f % 3), t, true, q, &served);
    }
    stats.setMode((f >= 599 && f < 999) ? PoseTiming::Arrival : PoseTiming::Next, late, ms, lines);
    stats.flushIfDue(ms, late, lines);
  }
  stats.flushNow(ms, late, lines);
  std::string out;
  for (const auto& l : lines.v) out += "[00:00:00.000] " + l + "\n";
  return out;
}

template <class Check>
void runPoseArrivalCases(Check&& check) {
  const auto close = [](double a, double b, double eps) { return std::fabs(a - b) <= eps; };
  constexpr int64_t P = 11111112;       // divisible by 8: half a period is 5555556, one and a half periods 16666668
  constexpr int64_t D = 7000000000;
  // ---- the bucket of a call against the first frame published after it arrived ---------------------------------------------------------------
  {
    unsigned b = 9;
    const auto at = [&](int64_t delta, unsigned want) { b = 9; return poseArrivalBucketOf(D + delta, D, P, &b) && b == want; };
    check(at(0, kBucket0p), "delta 0 is 0p");
    check(at(5555555, kBucket0p) && at(-5555555, kBucket0p) && at(-5555556, kBucket0p), "within half a period either way is 0p (up to exactly -P/2 below)");
    check(at(5555556, kBucketPlus1p) && at(P, kBucketPlus1p) && at(16666667, kBucketPlus1p), "from exactly half a period late up to 1.5 periods less a nanosecond is +1p");
    check(at(16666668, kBucketPlus2p) && at(2 * P, kBucketPlus2p) && at(9 * P, kBucketPlus2p), "from 1.5 periods late on is +2p+");
    check(at(-5555557, kBucketMinus1p) && at(-P, kBucketMinus1p) && at(-2 * P, kBucketMinus1p) && at(-7 * P, kBucketMinus1p), "more than half a period early is -1p-");
    check(!poseArrivalBucketOf(D, D, 0, &b) && !poseArrivalBucketOf(D, D, -1, &b) && !poseArrivalBucketOf(D, D, INT64_MAX, &b),
          "no usable period (zero, negative, or so long that the arithmetic could not hold it) cannot be told");
    check(!poseArrivalBucketOf(INT64_MIN, 5, P, &b) && !poseArrivalBucketOf(INT64_MAX, -5, P, &b), "...nor can a subtraction that would overflow");
    check(poseArrivalBucketOf(INT64_MAX, 1, P, &b) && b == kBucketPlus2p && poseArrivalBucketOf(1, INT64_MAX, P, &b) && b == kBucketMinus1p,
          "...and the largest differences that do not are bucketed without wrapping");
    check(std::string(poseArrivalBucketName(kBucket0p)) == "0p" && std::string(poseArrivalBucketName(kBucketPlus1p)) == "+1p" &&
              std::string(poseArrivalBucketName(kBucketPlus2p)) == "+2p+" && std::string(poseArrivalBucketName(kBucketMinus1p)) == "-1p-",
          "the buckets are named as the line prints them");
  }
  // ---- one window, call by call ---------------------------------------------------------------------------------------------------------------
  {
    PoseArrivalStats s;
    Lines lines;
    float q[4];
    const int64_t next = D + P;
    float drawn[4];
    yaw(0.0, drawn);
    PoseArrivalFrame served;
    served.display = next; served.period = P; served.q = drawn;
    const auto call = [&](uint32_t tid, PoseArrival arrival, double queue, int64_t delta, bool answered, double angle, const PoseArrivalFrame* frame) {
      yaw(angle, q);
      s.noteCall(tid, arrival, queue, next + delta, answered, q, frame);
    };
    s.setMode(PoseTiming::Next, 0, 1000, lines);
    s.flushIfDue(1000, 0, lines);
    call(1, PoseArrival::Between, 0.5, 0, true, 0.5, nullptr);                           // 0p, below 1 degree
    call(1, PoseArrival::Between, 2.5, P, true, 1.5, nullptr);                           // +1p, above 1
    call(1, PoseArrival::Straddled, 8.0, P, true, 3.0, &served);                         // +1p at once, above 1 and 2
    call(1, PoseArrival::Render, 0.1, 0, true, 0.1, nullptr);                            // 0p
    call(1, PoseArrival::Other, 5.0, 0, true, 2.5, &served);  // 0p at once, above 1 and 2 degrees; Other is not timed
    call(1, PoseArrival::Between, -1.0, 0, false, 4.0, nullptr);  // a fallback whose queue time was not read: counted, not bucketed, no angle
    call(1, PoseArrival::Between, 2.0, -3 * P, true, 4.0, nullptr);                      // -1p-, above 1 and 2
    call(1, PoseArrival::Straddled, 12.0, 0, true, 0.0, nullptr);                        // served with no frame cached: unresolved
    call(1, PoseArrival::Render, 0.3, 2 * P, true, 0.2, nullptr);                        // +2p+
    s.noteCall(1, PoseArrival::Between, 3.0, next, true, nullptr, nullptr);              // 0p, the pose was not valid: no angle
    check(s.pending() == 6, "six Between and Render calls wait for the next publish; the straddled ones were compared at once");
    float nextQ[4];
    yaw(0.0, nextQ);
    s.resolve(next, P, nextQ);
    check(s.pending() == 0, "the next publish resolves every one of them");
    const PoseArrivalCounts* c = s.countsFor(1);
    check(c && c->calls == 10 && c->unresolved == 1 && c->fallback == 1, "10 calls, one fallback, one unresolved");
    check(c && c->cls[0] == 5 && c->cls[1] == 2 && c->cls[2] == 2 && c->cls[3] == 1, "between 5, straddled 2, render 2, other 1");
    check(c && c->bucket[0][kBucket0p] == 2 && c->bucket[0][kBucketPlus1p] == 1 && c->bucket[0][kBucketPlus2p] == 0 && c->bucket[0][kBucketMinus1p] == 1 &&
              c->bucket[1][kBucketPlus1p] == 1 && c->bucket[2][kBucket0p] == 1 && c->bucket[2][kBucketPlus2p] == 1 && c->bucket[3][kBucket0p] == 1,
          "between: 0p 2, +1p 1, -1p- 1; straddled +1p 1; render 0p 1 and +2p+ 1; other 0p 1");
    check(c && c->over1[0] == 2 && c->over2[0] == 1 && c->over1[1] == 1 && c->over2[1] == 1 && c->over1[2] == 0 && c->over1[3] == 1 && c->over2[3] == 1,
          "the angle thresholds per class: between 2 above 1 and 1 above 2, straddled 1 and 1, render none, other 1 and 1 (the fallback's 4 degrees is left out)");
    check(c && c->maxKnown && close(c->maxAngle, 4.0, 1e-3) && c->maxClass == 0, "the largest angle is 4.0 degrees, of a Between call");
    check(c && c->queueN[0] == 4 && close(c->queueSum[0], 8.0, 1e-9) && close(c->queueMax[0], 3.0, 1e-9) && c->queueN[1] == 2 && close(c->queueSum[1], 20.0, 1e-9) &&
              close(c->queueMax[1], 12.0, 1e-9) && c->queueN[2] == 2 && close(c->queueSum[2], 0.4, 1e-9) && close(c->queueMax[2], 0.3, 1e-9),
          "the queue times: between 4 calls (mean 2, max 3; the one whose time was not read is left out), straddled 2 (mean 10, max 12), render 2 (mean 0.2, max 0.3); Other is not timed");
    uint64_t buckets = 0;
    if (c) for (unsigned a = 0; a < kPoseArrivals; ++a) for (unsigned k = 0; k < kBuckets; ++k) buckets += c->bucket[a][k];
    check(c && buckets + c->fallback + c->unresolved == c->calls, "the buckets of all classes + fallback + unresolved = calls: the totals add up");
    s.flushNow(2000, 3, lines);
    check(lines.v.size() == 1, "one line for the one caller");
    check(!lines.v.empty() && lines.v[0] ==
              "pose arrival: tid 1 mode next calls 10 between 5 straddled 2 render 2 other 1 fallback 1 unresolved 1; "
              "queue ms between 2.00/3.00, straddled 10.00/12.00, render 0.20/0.30; vs first frame after arrival: "
              "between (0p 2, +1p 1, +2p+ 0, -1p- 1), straddled (0p 0, +1p 1, +2p+ 0, -1p- 0), render (0p 1, +1p 0, +2p+ 1, -1p- 0), other (0p 1, +1p 0, +2p+ 0, -1p- 0); "
              ">1deg between 2 straddled 1 render 0 other 1; >2deg between 1 straddled 1 render 0 other 1; max 4.000 deg (between); late frames 3; window 1.0 s",
          "the line, exact: the classes, the queue times, the buckets per class, the angle counts, the largest angle and its class, the late frames, the window");
  }
  // ---- unresolved: a dropped cache, a next frame with no display time or period, a call waiting at the end --------------------------------------
  {
    PoseArrivalStats s;
    Lines lines;
    float q[4];
    yaw(0.0, q);
    s.noteCall(5, PoseArrival::Between, 0.2, 100, true, q, nullptr);
    s.noteCall(5, PoseArrival::Render, 0.2, 200, true, q, nullptr);
    s.dropPending();
    check(s.pending() == 0 && s.countsFor(5)->calls == 2 && s.countsFor(5)->unresolved == 2, "a dropped cache leaves its waiting calls unresolved");
    s.noteCall(5, PoseArrival::Between, 0.2, D, true, q, nullptr);
    s.resolve(D, 0, q);
    s.noteCall(5, PoseArrival::Between, 0.2, D, true, q, nullptr);
    s.resolve(0, P, q);
    check(s.countsFor(5)->unresolved == 4, "a next frame with no usable period, or no display time, leaves its calls unresolved");
    PoseArrivalFrame badPeriod;
    badPeriod.display = D; badPeriod.period = 0; badPeriod.q = q;
    s.noteCall(5, PoseArrival::Straddled, 0.2, D, true, q, &badPeriod);
    check(s.countsFor(5)->unresolved == 5, "a straddled call whose cached frame has no period too");
    s.noteCall(5, PoseArrival::Between, 0.2, D, true, q, nullptr);
    s.flushNow(1, 0, lines);
    check(lines.v.size() == 1 && lines.v[0].find(" calls 6 between 4 straddled 1 render 1 other 0 fallback 0 unresolved 6; ") != std::string::npos &&
              lines.v[0].find("max n/a; late frames 0; window 0.0 s") != std::string::npos,
          "a call still waiting at the end of the run is unresolved in the last line; a window with no angle reads max n/a");
  }
  // ---- the pending store -----------------------------------------------------------------------------------------------------------------------
  {
    PoseArrivalStats s;
    float q[4];
    yaw(0.0, q);
    for (unsigned i = 0; i < kPoseArrivalPending + 3; ++i) s.noteCall(9, PoseArrival::Between, 0.1, D + P, true, q, nullptr);
    check(s.pending() == kPoseArrivalPending && s.overflowed() == 3 && s.countsFor(9)->calls == kPoseArrivalPending + 3 && s.countsFor(9)->unresolved == 3,
          "the pending store holds 256 calls; three more are counted, as calls and as unresolved, and counted as overflowed");
    s.resolve(D + P, P, q);
    check(s.pending() == 0 && s.countsFor(9)->bucket[0][kBucket0p] == kPoseArrivalPending, "...and the 256 held are resolved");
  }
  // ---- the callers kept per window ---------------------------------------------------------------------------------------------------------------
  {
    PoseArrivalStats s;
    Lines lines;
    float q[4];
    yaw(0.0, q);
    for (uint32_t t = 100; t < 100 + kPoseArrivalThreads; ++t) s.noteCall(t, PoseArrival::Render, 0.1, D, true, q, nullptr);
    s.noteCall(999, PoseArrival::Render, 0.1, D, true, q, nullptr);
    s.noteCall(998, PoseArrival::Straddled, 0.1, D, true, q, nullptr);
    check(s.threads() == kPoseArrivalThreads && s.droppedCalls() == 2 && s.pending() == kPoseArrivalThreads, "eight callers are kept; a ninth and tenth are counted, not stored");
    s.resolve(D, P, q);
    s.flushNow(1, 0, lines);
    check(lines.v.size() == kPoseArrivalThreads + 1 && lines.v.back() == "pose arrival: more than 8 callers in a window; 2 calls not counted", "...and said in a line after the callers (the exact text)");
  }
  // ---- the window, the late frames and the mode split (three timings) ---------------------------------------------------------------------------------
  {
    PoseArrivalStats s;
    Lines lines;
    float q[4];
    yaw(0.0, q);
    check(s.mode() == PoseTiming::Next, "the instrument starts in the shipped mode");
    s.setMode(PoseTiming::Next, 5, 1000, lines);
    s.flushIfDue(1000, 5, lines);
    s.noteCall(1, PoseArrival::Between, 0.2, D, true, q, nullptr);
    s.resolve(D, P, q);
    s.flushIfDue(60999, 9, lines);
    check(lines.v.empty(), "a window under 60 s old writes nothing");
    s.flushIfDue(61000, 12, lines);
    check(lines.v.size() == 1 && lines.v[0].find("tid 1 mode next calls 1 between 1 ") != std::string::npos && lines.v[0].find("late frames 7; window 60.0 s") != std::string::npos,
          "...and at 60 s it writes its line, with the late frames since the window opened (12 - 5)");
    s.flushIfDue(121000, 20, lines);
    check(lines.v.size() == 1, "a window with no calls writes nothing");
    s.noteCall(1, PoseArrival::Between, 0.2, D, true, q, nullptr);
    s.resolve(D, P, q);
    s.setMode(PoseTiming::Arrival, 25, 130000, lines);
    check(s.mode() == PoseTiming::Arrival && lines.v.size() == 2 && lines.v[1].find("tid 1 mode next calls 1 ") != std::string::npos && lines.v[1].find("late frames 5; window 9.0 s") != std::string::npos,
          "a mode change closes the window at the change: its line is written under the OLD mode, with the late frames up to the change (25 - 20) and the 9 s it lasted");
    s.noteCall(1, PoseArrival::Between, 0.2, D, true, q, nullptr);
    s.resolve(D, P, q);
    s.setMode(PoseTiming::Slot, 28, 140000, lines);
    check(lines.v.size() == 3 && lines.v[2].find("tid 1 mode arrival calls 1 ") != std::string::npos, "...and the next one is written under `arrival`, closed by the change to `slot`");
    s.setMode(PoseTiming::Slot, 40, 150000, lines);
    check(lines.v.size() == 3, "setting the mode it is in changes nothing");
  }
  // ---- the lines of the scripted flight ------------------------------------------------------------------------------------------------------
  {
    const std::string built = fixtureLog();
    std::string fixture;
    FILE* f = nullptr;
    if (fopen_s(&f, "tools/pose_arrival_fixture.log", "rb") == 0 && f) {
      char buffer[4096];
      size_t n;
      while ((n = std::fread(buffer, 1, sizeof(buffer), f)) > 0) fixture.append(buffer, n);
      std::fclose(f);
    }
    std::string normalised;
    for (char ch : fixture) if (ch != '\r') normalised += ch;
    check(!fixture.empty(), "tools/pose_arrival_fixture.log is readable from the repo root");
    check(normalised == built, "the log tool's fixture file is what the aggregator writes for the scripted flight, byte for byte (python tools\\edvr_log.py --tally pose reads it; regenerate with --print-arrival-fixture)");
    size_t lineCount = 0;
    for (char ch : built) if (ch == '\n') ++lineCount;
    check(lineCount == 14 && built.find(" mode arrival ") != std::string::npos && built.find(" mode next ") != std::string::npos,
          "...and it carries 14 lines in both modes: two callers over seven windows (the fourth cut short by the flip back, the last by the end of the run)");
  }
}
}  // namespace pose_arrival_cases
