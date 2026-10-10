#pragma once
// Elite's "now" head pose, answered one display period after the drawn frame's display time (src/openxr/head_pose_time.h, native_runtime_host.h
// locateHeadFor, src/openxr/pose_gap.h; docs\terrain-culling.md): through the real hop to the owner thread, from a foreign thread, the instant a call
// is located at (display time + period), the figures the diagnostic takes from each locate, what the WaitGetPoses publish point keeps, and who is
// said to be answered.
// Then the cache's lifetime: it is dropped wherever the origin, the session or the geometry publication is invalidated (a seated reset, a
// session stop and restart, a failed reset, a fatal failure), it is not used once it has fallen more than one display period behind now, and a
// display-time locate the runtime refuses with XR_ERROR_TIME_INVALID is retried once at now + prediction. Every cell reads the instant the fake
// XR runtime was actually handed, against a clock the rig sets, so "now" is a number the cell chooses. The window arithmetic, the filter, the
// freshness rule and the line formats are held in tools\openxr_pose_test.
#include "launch_centre_cases.h"
#include "frame_end_overlap_cases.h"
#include <atomic>
#include <cmath>
#include <cstring>
#include <thread>

namespace edvr::openxr::test {
namespace head_pose_fixture {
// The host's head clock: the fake XR runtime's convert hands the counter back as the time, so this is "now" to the nanosecond.
inline LARGE_INTEGER counter{};
inline bool readClock(LARGE_INTEGER* value){*value=counter;return true;}
inline void setNow(XrTime now){counter.QuadPart=now;}
// A session that goes STOPPING and READY on cue, for the real service tick.
inline XrSessionState queued=XR_SESSION_STATE_UNKNOWN;
inline XrResult XRAPI_PTR pollQueued(XrInstance,XrEventDataBuffer* out) {
  if(queued==XR_SESSION_STATE_UNKNOWN)return XR_EVENT_UNAVAILABLE;
  XrEventDataSessionStateChanged event{XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED};
  event.session=launch_fixture::session();event.state=queued;queued=XR_SESSION_STATE_UNKNOWN;
  std::memcpy(out,&event,sizeof(event));return XR_SUCCESS;
}
}
template<class Check> void runHeadPoseCases(Check&& check) {
  using namespace launch_fixture;
  using namespace head_pose_fixture;
  const auto close = [](double a, double b, double eps) { return std::fabs(a - b) <= eps; };
  OwnerBuiltFixture f;
  check(f.route.bind() && f.owner.start() && f.owner.invoke([&] { f.build(); }) && f.initialized, "(the head-pose cases' host is built on its own owner thread)");
  if (!f.initialized) return;
  auto& h = *f.hostPtr;
  h.headClock = readClock;
  const DWORD me = GetCurrentThreadId();
  const XrTime display = 5000000000, period = 11111111;
  const XrTime lead = 42000000;   // flight 3: the game thread's "now" was about 42 ms before the drawn frame's display time
  const XrTime now = display - lead;
  setNow(now);
  char lines[8][352]{};
  unsigned lineCount = 0;
  const auto collect = [&](const char* line) { if (lineCount < 8) std::snprintf(lines[lineCount++], sizeof(lines[0]), "%s", line); };
  const double half = 3.14159265358979323846 / 180.0 * 0.5;
  const auto yawed = [&](double degrees) { f.fake.head.orientation = {0.0f, float(std::sin(degrees * half)), 0.0f, float(std::cos(degrees * half))}; };
  // The publish point: the frame the game now holds, kept for the pose calls to be located at and measured against.
  XrVector3f turn{0.0f, 0.5235988f, 0.0f};   // 30 degrees a second about y
  bool published = false;
  check(f.owner.invoke([&] {
    h.publishPoseGap(display, period, XrPosef{{0, 0, 0, 1}, {0, 0, 0}}, true, XR_SPACE_VELOCITY_ANGULAR_VALID_BIT, turn, 1000, collect);
    published = h.latestPoseFrame.valid;
  }) && published, "(a frame is published to the host)");
  bool kept = false;
  f.owner.invoke([&] {
    const auto& p = h.latestPoseFrame;
    kept = p.displayTime == display && p.period == period && p.orientationKnown && p.speedKnown && close(p.speedDegPerSec, 30.0, 1e-3) &&
           p.orientation[0] == 0 && p.orientation[3] == 1 && h.poseGap.waits() == 1;
  });
  check(kept, "the publish point keeps the frame's display time and period, its orientation, the head speed in degrees a second, and counts the wait");
  // What the WaitGetPoses publish point does, for a cell that needs another frame.
  const auto publish = [&](XrTime displayTime, XrDuration framePeriod, uint64_t atMs) {
    f.owner.invoke([&] { h.publishPoseGap(displayTime, framePeriod, XrPosef{{0, 0, 0, 1}, {0, 0, 0}}, true, 0, XrVector3f{0, 0, 0}, atMs, collect); });
  };
  vr::TrackedDevicePose_t pose{};
  const auto locateAs = [&](bool atDisplay, float prediction, uint32_t rva = 0x4E3881) {
    HeadCall call;call.thread = me;call.rva = rva;call.display = atDisplay;
    pose = {};
    return h.locateHeadFor(h.geometryGeneration, vr::TrackingUniverseSeated, prediction, call, pose);
  };
  const auto flushLine = [&](std::string& out) {
    lineCount = 0;
    f.owner.invoke([&] { h.poseGap.flushNow(0, collect); });
    out = lineCount ? lines[lineCount - 1] : "";
    return lineCount;
  };
  yawed(0.0);
  // ---- inside the game's image and asking for "now": the display time ----------------------------------------------------------------
  check(locateAs(true, 0.0f) && f.fake.lastTime == display + period && h.lastHeadTime == display + period && pose.bPoseIsValid,
        "a call flagged as Elite's \"now\" is located at exactly the latest frame's display time plus that frame's period");
  check(locateAs(true, 0.003f) && f.fake.lastTime == display + period && locateAs(true, -0.004f) && f.fake.lastTime == display + period,
        "...whatever small prediction Elite passed (the filter, not the host, decides what is \"now\")");
  std::string line;
  flushLine(line);
  char want[512];
  std::snprintf(want, sizeof(want), "pose gap: tid %lu calls 3 from exe+0x4E3881 prediction -0.3 ms target-minus-display mean 11.11 ms (min 11.11 max 11.11) angle-to-drawn mean 0.000 max 0.000 deg head 30.0 deg/s waitgetposes 1 failed 0", (unsigned long)me);
  check(line == want, "the diagnostic records the CALLER's thread (the hop to the owner does not lose it), the return RVA, the mean prediction, a gap of one period (+11.11 ms), an angle of 0 and the wait the publish point counted (the exact line)");
  // ---- everything else: today's answer ------------------------------------------------------------------------------------------------
  check(locateAs(false, 0.0f) && f.fake.lastTime == now && f.fake.lastTime != display + period, "a call that is not flagged (outside the image, or with a real prediction) is located on the wall clock as always, not at the display time plus a period");
  flushLine(line);
  check(line.find("target-minus-display mean -42.00 ms") != std::string::npos && line.find("fallback") == std::string::npos,
        "...and its gap is the true one (-42.00 ms), with no fallback (it was never asked for the display time)");
  check(locateAs(false, 0.25f) && f.fake.lastTime == now + 250000000,
        "...and the prediction it passed is still added: a quarter second after now");
  const double lateGap = double(f.fake.lastTime - display) / 1e6;
  flushLine(line);
  char wantGap[160];
  std::snprintf(wantGap, sizeof(wantGap), "target-minus-display mean %.2f ms (min %.2f max %.2f)", lateGap, lateGap, lateGap);
  check(line.find(wantGap) != std::string::npos, "...and its gap is the located instant less the display time, in milliseconds");
  // The angle: the pose handed back against the pose that frame was drawn with.
  yawed(10.0);
  check(locateAs(true, 0.0f), "(a head turned 10 degrees from the drawn frame)");
  flushLine(line);
  check(line.find("angle-to-drawn mean 10.000 max 10.000 deg") != std::string::npos, "the angle between the pose handed back and the pose drawn is 10.000 degrees");
  yawed(0.0);
  // ---- no frame to be answered at: today's answer, counted ----------------------------------------------------------------------------
  f.owner.invoke([&] { h.latestPoseFrame.valid = false; });
  check(locateAs(true, 0.0f, 0x777777) && f.fake.lastTime == now, "with no frame yet an Elite \"now\" call is located at now + prediction, as before");
  flushLine(line);
  check(line.find("target-minus-display n/a angle-to-drawn n/a head n/a") != std::string::npos && line.find(" fallback 1") != std::string::npos,
        "...and the line says there was nothing to measure against, and one fallback");
  f.owner.invoke([&] { h.latestPoseFrame.valid = true;h.latestPoseFrame.displayTime = 0; });
  check(locateAs(true, 0.0f, 0x777777) && f.fake.lastTime == now, "a display time that is not positive falls back too");
  flushLine(line);
  check(line.find(" fallback 1") != std::string::npos, "...counted as a fallback");
  f.owner.invoke([&] { h.latestPoseFrame.displayTime = -5; });
  check(locateAs(true, 0.0f, 0x777777) && f.fake.lastTime == now, "...a negative one too");
  flushLine(line);
  f.owner.invoke([&] { h.latestPoseFrame.displayTime = display; });
  // ---- who is said to be answered ------------------------------------------------------------------------------------------------------
  check(h.poseSightings.distinct() == 1, "(so far one Elite caller has been answered one period past the display time: the fallbacks (from another address) and the unflagged calls said nothing)");
  check(locateAs(true, 0.0f, 0x4E3881), "(the same caller again)");
  check(h.poseSightings.distinct() == 1, "the same return address is not said twice");
  check(locateAs(true, 0.0f, 0x1234567), "(another caller in the image)");
  check(h.poseSightings.distinct() == 2, "a further distinct return address is said once (a caller that appears after a game update)");
  flushLine(line);
  // ---- failures: the owner's busy gate, a retired generation, an origin the runtime does not locate, a locate the runtime refuses ---
  HeadCall call;call.thread = me;call.rva = 0x4E3881;call.display = true;
  vr::TrackedDevicePose_t none{};
  check(!h.locateHeadFor(h.geometryGeneration + 1, vr::TrackingUniverseSeated, 0.0f, call, none) &&
            !h.locateHeadFor(h.geometryGeneration, vr::TrackingUniverseStanding, 0.0f, call, none),
        "a stale generation and an origin the runtime does not locate return false");
  f.fake.locateResult = XR_ERROR_RUNTIME_FAILURE;
  check(!h.locateHeadFor(h.geometryGeneration, vr::TrackingUniverseSeated, 0.0f, call, none), "...so does a locate the runtime refuses");
  f.fake.locateResult = XR_SUCCESS;
  flushLine(line);
  check(line.find(" calls 3 ") != std::string::npos && line.find(" failed 3") != std::string::npos && line.find("target-minus-display n/a") != std::string::npos,
        "all three failures are counted in `failed` on the caller's line, the silent busy-gate path included, and left out of every mean");
  check(h.poseSightings.distinct() == 2, "(a failed locate says nothing of who is answered)");
  h.noteHeadCallFailed(call, 0.5f);
  flushLine(line);
  check(line.find(" calls 1 ") != std::string::npos && line.find(" failed 1") != std::string::npos && line.find("prediction 500.0 ms") != std::string::npos,
        "a pose call that never reached the owner (no live session, a bad origin) is counted too, with its prediction");
  // The old entry point is the plain call.
  check(h.locateHead(h.geometryGeneration, vr::TrackingUniverseSeated, 0.0f, none) && f.fake.lastTime == now, "locateHead (no caller known) is located at now + prediction, as it always was");
  flushLine(line);
  check(line.find(" tid 0 calls 1 from exe+0x0 ") != std::string::npos, "...and is recorded under thread 0, RVA 0");
  // The publish point writes the window when it is 60 s old, and only then.
  f.owner.invoke([&] { h.poseGap.flushNow(0, collect); });
  lineCount = 0;
  f.owner.invoke([&] {
    PoseGapStats::Sample s{};s.thread = 5;s.rva = 0x4E3881;s.located = true;
    h.poseGap.note(s);
    h.publishPoseGap(display, period, XrPosef{{0, 0, 0, 1}, {0, 0, 0}}, true, 0, XrVector3f{0, 0, 0}, 59999, collect);
  });
  check(lineCount == 0, "the publish point writes nothing for a window under 60 s old");
  publish(display, period, 60000 + 1);
  check(lineCount == 1 && std::string(lines[0]).rfind("pose gap: tid 5 calls 1 ", 0) == 0, "...and the window's line once it is 60 s old");

  // ====================================================================================================================================
  // The cache's lifetime. Each cell first proves the cache is live (a qualifying call is located at the display time), so that what follows is
  // the invalidation and not a cache that never worked. A call "before the next wait" is one with no publishPoseGap in between: the publish
  // point is what waitPoses calls with the frame it just got (the wait itself needs a real session binding no fixture has).
  // ====================================================================================================================================
  const auto answeredAt = [&](XrTime instant) { return locateAs(true, 0.0f) && f.fake.lastTime == instant && pose.bPoseIsValid; };
  // ---- the origin: a seated reset, and any other invalidateOrigin ----------------------------------------------------------------------
  {
    setNow(now);
    publish(display, period, 100000);
    check(answeredAt(display + period), "(before a reset the cached display time answers a qualifying call)");
    XrSpace before = XR_NULL_HANDLE, after = XR_NULL_HANDLE;
    bool reset = false, dropped = false;
    f.owner.invoke([&] { before = h.seated.space(); reset = h.resetSeated(h.geometryGeneration); after = h.seated.space(); dropped = !h.latestPoseFrame.valid; });
    check(reset && before != after && dropped, "a seated reset replaces the space and drops the cached frame");
    setNow(now);
    check(answeredAt(now) && f.fake.lastTime != display + period, "...and a qualifying call before the next wait is located at now + prediction, not at the old display time");
    flushLine(line);
    check(line.find(" fallback 1") != std::string::npos, "...counted as a fallback");
    const XrTime next = display + period;
    setNow(next - lead);
    publish(next, period, 100001);
    check(answeredAt(next + period), "...and after the next wait publishes a frame, the display time answers again");
    // The other callers of invalidateOrigin (a reference-space change in a wait or a loading frame; the session restart below).
    f.owner.invoke([&] { h.invalidateOrigin("reference_change"); dropped = !h.latestPoseFrame.valid; });
    check(dropped, "a reference-space change (invalidateOrigin) drops it too");
    setNow(next - lead);
    check(answeredAt(next - lead) && f.fake.lastTime != next + period, "...and the call after it is located at now + prediction");
  }
  // ---- the session: STOPPING, READY, start, through the real service tick -----------------------------------------------------------------
  {
    const XrTime restartDisplay = 7000000000LL;
    setNow(restartDisplay - lead);
    const Dispatch dispatch{pollQueued, beginSession, endSession, wait, beginFrame, endFrame};
    bool running = false, stopped = false, dropped = false, restarted = false, droppedAtRestart = false, connected = false;
    f.owner.invoke([&] {
      // A session that polls on cue, started afresh (the fixture's own poll only ever says READY once).
      h.state.abandonAfterOwnerDestruction();
      queued = XR_SESSION_STATE_READY;
      running = h.state.reset(dispatch, instance(), session(), XR_ENVIRONMENT_BLEND_MODE_OPAQUE) == XR_SUCCESS &&
                h.state.pollEvents() == XR_SUCCESS && h.state.startIfReady() == XR_SUCCESS;
      h.publishPoseGap(restartDisplay, period, XrPosef{{0, 0, 0, 1}, {0, 0, 0}}, true, 0, XrVector3f{0, 0, 0}, 200000, collect);
    });
    check(running && answeredAt(restartDisplay + period), "(a started session, with a frame cached)");
    f.owner.invoke([&] {
      queued = XR_SESSION_STATE_STOPPING;
      h.pumpEvents();
      stopped = !h.state.running();
      dropped = !h.latestPoseFrame.valid;
    });
    check(stopped && dropped, "the service tick that ends the session on STOPPING drops the cached frame");
    f.owner.invoke([&] {
      queued = XR_SESSION_STATE_READY;
      h.pumpEvents();
      restarted = h.state.running();
      droppedAtRestart = !h.latestPoseFrame.valid;
      connected = h.read().connected;
    });
    check(restarted && droppedAtRestart && connected, "...and READY restarts it (prepareSessionRestart) with no frame cached, the host still connected and on the same generation");
    setNow(restartDisplay - lead);
    check(answeredAt(restartDisplay - lead) && f.fake.lastTime != restartDisplay + period,
          "a qualifying call after the restart and before the next wait is located at now + prediction, not at the previous session's display time");
    const XrTime afterRestart = restartDisplay + 50 * period;
    setNow(afterRestart - lead);
    publish(afterRestart, period, 200001);
    check(answeredAt(afterRestart + period), "...and after the next wait publishes a frame, the display time answers again");
    // prepareSessionRestart on its own (the review's path): origin invalidation clears it even with the session still the same one.
    bool prepared = false;
    f.owner.invoke([&] {
      queued = XR_SESSION_STATE_STOPPING;
      h.pumpEvents();
      h.publishPoseGap(afterRestart, period, XrPosef{{0, 0, 0, 1}, {0, 0, 0}}, true, 0, XrVector3f{0, 0, 0}, 200002, collect);   // a stale publish into the stopped session
      prepared = !h.state.running() && h.prepareSessionRestart();
      dropped = !h.latestPoseFrame.valid;
      queued = XR_SESSION_STATE_READY;
      h.state.pollEvents();
      restarted = h.state.startIfReady() == XR_SUCCESS;
      h.serviceStopped = false;   // what the service tick does once it has restarted the session
    });
    check(prepared && dropped && restarted, "prepareSessionRestart itself drops the cache (a frame published into the stopped session is gone at the restart)");
    setNow(afterRestart - lead);
    check(answeredAt(afterRestart - lead) && f.fake.lastTime != afterRestart + period, "...and the first call of the new session is located at now + prediction");
    publish(afterRestart, period, 200003);
  }
  // ---- the cache is not used once it is stale ----------------------------------------------------------------------------------------------
  {
    const XrTime base = 9000000000LL;
    publish(base, period, 300000);
    setNow(base + period);
    check(answeredAt(base + period), "a display time exactly one period behind now is still good (the freshness test is on the cached frame, not on the target a period ahead of it)");
    setNow(base + period + 1);
    check(answeredAt(base + period + 1) && f.fake.lastTime != base + period, "...one nanosecond more and it is not: the call is located at now + prediction");
    flushLine(line);
    check(line.find(" fallback 1") != std::string::npos, "...counted as a fallback");
    setNow(base + 3 * period);
    check(answeredAt(base + 3 * period), "...and three periods behind it is not either");
    setNow(base - lead);
    check(answeredAt(base + period), "(back at flight 3's lead the same frame answers again: nothing was dropped, the cache was only not used)");
    publish(base, 0, 300001);   // a frame that reports no period: there is no target to form
    flushLine(line);
    check(answeredAt(base - lead), "with no period no target can be formed: the call is located at now + prediction");
    flushLine(line);
    check(line.find(" fallback 1") != std::string::npos, "...counted as a fallback");
    publish(base, -5, 300001);
    check(answeredAt(base - lead), "...and a negative period likewise");
    flushLine(line);
    setNow(base);
    publish(base, 1000000000000LL, 300002);   // a period that is not a display period: the tolerance is held to the 50 ms cap
    setNow(base + 50000000);
    check(answeredAt(base + 1000000000000LL), "a huge period is added as it stands to the target, and the tolerance is capped at 50 ms: good at 50 ms behind");
    setNow(base + 50000001);
    check(answeredAt(base + 50000001), "...and not at 50 ms and a nanosecond");
    // A stale display time never reaches the runtime, so there is nothing for it to refuse or to answer with old tracking.
    publish(base, period, 300003);
    setNow(base + 10 * period);
    const unsigned locatesBefore = f.fake.locates;
    check(answeredAt(base + 10 * period) && f.fake.locates == locatesBefore + 1, "a stale display time costs the runtime exactly one locate, at now");
  }
  // ---- a reference-space change between the display time and the target ------------------------------------------------------------------
  {
    const XrTime base = 12000000000LL;
    setNow(base - lead);
    publish(base, period, 350000);
    check(answeredAt(base + period), "(a fresh frame)");
    flushLine(line);
    XrEventDataReferenceSpaceChangePending event{XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING};
    event.session = session();event.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;event.changeTime = base + period / 2;event.poseValid = XR_FALSE;
    bool noted = false;
    f.owner.invoke([&] { noted = h.changes.note(event); });
    check(noted && answeredAt(base - lead) && f.fake.lastTime != base + period,
          "a change of the reference space pending between the display time and the target: the call is located at now + prediction, not across the change");
    flushLine(line);
    check(line.find(" fallback 1") != std::string::npos, "...counted as a fallback");
    f.owner.invoke([&] { h.changes.clear();noted = h.changes.begin(session()); });
    check(noted && answeredAt(base + period), "(the change gone, the target answers again)");
    flushLine(line);
  }
  // ---- the runtime refuses the display time --------------------------------------------------------------------------------------------
  {
    const XrTime base = 11000000000LL;
    setNow(base - lead);
    publish(base, period, 400000);
    check(answeredAt(base + period), "(a fresh frame)");
    const unsigned sightings = h.poseSightings.distinct();
    flushLine(line);
    const unsigned locatesBefore = f.fake.locates;
    const uint64_t refusalsBefore = h.displayTimeRefusals;
    f.fake.rejectTime = base + period;
    const bool answered = locateAs(true, 0.0f);
    f.fake.rejectTime = -1;
    check(answered && pose.bPoseIsValid && f.fake.lastTime == base - lead && f.fake.locates == locatesBefore + 2 && h.displayTimeRefusals == refusalsBefore + 1,
          "XR_ERROR_TIME_INVALID on the display-time locate: one retry at now + prediction, and a valid pose, not an invalid one");
    check(h.poseSightings.distinct() == sightings, "...and a retried call is not said to have been answered one period past the display time");
    flushLine(line);
    check(line.find(" fallback 1") != std::string::npos && line.find(" failed 0") != std::string::npos, "...it is counted as a fallback, not as a failure");
    // The retry carries the prediction the caller passed (a prediction inside the filter's window).
    f.fake.rejectTime = base + period;
    check(locateAs(true, 0.004f) && f.fake.lastTime == base - lead + 4000000, "...at now + the prediction Elite passed");
    f.fake.rejectTime = -1;
    // Only that error.
    f.fake.rejectTime = base + period;f.fake.rejectResult = XR_ERROR_RUNTIME_FAILURE;
    const unsigned locatesOther = f.fake.locates;
    const bool failed = locateAs(true, 0.0f);
    f.fake.rejectTime = -1;f.fake.rejectResult = XR_ERROR_TIME_INVALID;
    check(!failed && f.fake.locates == locatesOther + 1, "any other error is not retried: one locate, no pose");
    // The retry is once: if now + prediction is refused too, the call fails with it.
    f.fake.locateResult = XR_ERROR_TIME_INVALID;
    const unsigned locatesTwice = f.fake.locates;
    const bool bothRefused = locateAs(true, 0.0f);
    f.fake.locateResult = XR_SUCCESS;
    check(!bothRefused && f.fake.locates == locatesTwice + 2, "...and only once: when now + prediction is refused as well, there are two locates and no pose");
    // The unflagged path never retries anything.
    f.fake.rejectTime = base - lead;
    const unsigned locatesPlain = f.fake.locates;
    check(!locateAs(false, 0.0f) && f.fake.locates == locatesPlain + 1, "a call that was never asked for the display time is not retried (it has no second instant to try)");
    f.fake.rejectTime = -1;
  }
  // ---- a wait that fails ------------------------------------------------------------------------------------------------------------------
  {
    setNow(display - lead);
    publish(display, period, 440000);
    check(answeredAt(display + period), "(a frame, before a wait that fails)");
    // A wait that gets as far as the frame and then cannot take it (the frame-sequence counter has no room): the runtime's own fail path, with no
    // boundary failure behind it that would end the host and drop the cache by another road.
    CompositorRead polled{};
    vr::EVRCompositorError waited = vr::VRCompositorError_None;
    bool dropped = false, stillRunning = false;
    f.owner.invoke([&] {
      h.frameSequenceOffset = (std::numeric_limits<uint64_t>::max)();
      waited = h.waitPoses(h.compositorGeneration, polled);
      h.frameSequenceOffset = 0;
      dropped = !h.latestPoseFrame.valid;
      stillRunning = !h.serviceFailed && h.state.running();
    });
    check(waited != vr::VRCompositorError_None && stillRunning && dropped, "a WaitGetPoses that fails invalidates the geometry publication and drops the cached frame with it (the host is still up)");
  }
  // ====================================================================================================================================
  // The pose-slot answer (advanced.cull_pose_timing, TEMPORARY) and its instrument (pose_slot.h), through the host: the mode is taken at each publish
  // from the frame the graphics half described, the frame's lead from the clock at its wait, the instant from slotAnswerTime, and every call the fix
  // handles -- answered or fallen back -- is compared with the frame that comes next. The pure arithmetic and the line writer are held in
  // tools\openxr_pose_test; this is the wiring.
  // ====================================================================================================================================
  {
    const XrTime base = 13000000000LL;
    std::vector<std::string> slotLines;
    const auto slotSink = [&](const char* l) { slotLines.emplace_back(l); };
    // The publish point as WaitGetPoses reaches it: the frame's lead is its display time less the time its wait returned at, and the graphics half's frame
    // says whether the slot answer is on.
    const auto publishFrame = [&](XrTime displayTime, XrDuration framePeriod, uint64_t atMs, bool slotOn) {
      f.owner.invoke([&] {
        h.featureFrameKnown = true;
        h.featureFrame.cullPoseTiming = slotOn ? 1u : 0u;
        h.waitReturnedAt = displayTime - lead;
        h.publishPoseGap(displayTime, framePeriod, XrPosef{{0, 0, 0, 1}, {0, 0, 0}}, true, 0, XrVector3f{0, 0, 0}, atMs, collect, slotSink);
      });
    };
    const auto counts = [&]() -> PoseSlotCounts {
      PoseSlotCounts c{};
      f.owner.invoke([&] { if (const PoseSlotCounts* p = h.poseSlot.countsFor(me)) c = *p; });
      return c;
    };
    const auto slotOn = [&]() { bool on = false; f.owner.invoke([&] { on = h.poseSlot.mode() != PoseTiming::Next; }); return on; };
    f.owner.invoke([&] { h.poseSlot.flushNow(0, h.boundary.lateFrames(), slotSink); });
    slotLines.clear();
    // The clock at the wait: the same clock a pose call reads.
    setNow(777000000);
    XrTime waitReturned = 0;
    f.owner.invoke([&] { h.noteWaitReturned(); waitReturned = h.waitReturnedAt; });
    check(waitReturned == 777000000, "the time the frame wait returned at is read from the head clock (the one a pose call's \"now\" reads)");
    // ---- off (what ships): the instrument measures, the answer is `next` -----------------------------------------------------------------------------
    yawed(0.0);
    setNow(base - lead);
    publishFrame(base, period, 1000, false);
    XrDuration keptLead = 0;
    f.owner.invoke([&] { keptLead = h.latestPoseFrame.lead; });
    check(keptLead == lead && !slotOn(), "the publish point keeps the frame's lead (its display time less the time its wait returned: 42 ms) and, with the key off, the shipped mode");
    setNow(base - lead + 5000000);
    check(locateAs(true, 0.0f) && f.fake.lastTime == base + period, "off: a call just after the wait is located one period past the display time (`next`)");
    publishFrame(base + period, period, 1200, false);
    check(counts().calls == 1 && counts().cls[kSlotMatch] == 1 && counts().unresolved == 0, "...and when the next frame comes on time it is a match");
    const XrTime t1 = base + period;
    setNow(t1 - lead + 5000000);
    yawed(3.0);
    check(locateAs(true, 0.0f) && f.fake.lastTime == t1 + period, "off: a call just after the next wait is located one period past that frame");
    yawed(0.0);
    publishFrame(t1 + 2 * period, period, 1400, false);   // the display skipped a slot
    {
      const PoseSlotCounts c = counts();
      check(c.calls == 2 && c.cls[kSlotShort1] == 1 && c.over1[kSlotShort1] == 1 && c.over2[kSlotShort1] == 1 && c.maxKnown && close(c.maxAngle, 3.0, 1e-3) && c.maxClass == kSlotShort1,
            "...a frame the display skipped a slot for makes it a slot short (1p), and the 3 degrees the head turned are above 1 and above 2");
    }
    const XrTime t2 = t1 + 2 * period;
    setNow(t2 + 3 * period);
    check(locateAs(true, 0.0f) && f.fake.lastTime == t2 + 3 * period, "off: a cached frame three periods behind now is stale, and the call is located at now + prediction");
    const uint64_t pendingBefore = [&] { unsigned n = 0; f.owner.invoke([&] { n = h.poseSlot.pending(); }); return uint64_t(n); }();
    check(locateAs(false, 0.0f), "(a call the fix does not handle: outside the image or with a real prediction)");
    const uint64_t pendingAfter = [&] { unsigned n = 0; f.owner.invoke([&] { n = h.poseSlot.pending(); }); return uint64_t(n); }();
    check(pendingBefore == 1 && pendingAfter == 1, "...is not remembered: only the calls the fix handles are compared");
    // The graphics half turns the key on: the publish point resolves the stale call, closes the window under the old mode, then switches.
    publishFrame(t2 + 4 * period, period, 31000, true);
    {
      const std::string wantLine = "pose slot: tid " + std::to_string(me) + " mode next calls 3 match 1 short 1 (1p 1, 2p+ 0) long 0 (1p 0, 2p+ 0) fallback 1 (none 0, stale 1, period 0, cross 0, cap 0, refused 0; "
                               "delta match 0, short 1, long 0) unresolved 0; ";
      check(slotLines.size() == 1 && slotLines[0].rfind(wantLine, 0) == 0 && slotLines[0].find(">2deg match 0 short 1 (1p 1, 2p+ 0)") != std::string::npos &&
                slotLines[0].find("max 3.000 deg (short-1p); late frames 0; window 31.0 s") != std::string::npos,
            "the key turned on closes the window under the OLD mode: the three calls (a match, a slot short, a stale fallback that was also short by its delta), the 3 degrees, 31 s (the window opened at 0)");
    }
    check(slotOn(), "...and the mode is slot from the next call");
    // ---- on: the slot answer ----------------------------------------------------------------------------------------------------------------------
    const XrTime t3 = t2 + 4 * period;
    setNow(t3 - lead + 5000000);
    check(locateAs(true, 0.0f) && f.fake.lastTime == t3 + period, "on: steady flight is exactly `next` (k = 1)");
    publishFrame(t3 + period, period, 32000, true);
    const XrTime t4 = t3 + period;
    setNow(t4 - lead + period * 12 / 10);
    check(locateAs(true, 0.0f) && f.fake.lastTime == t4 + 2 * period, "on: a call 1.2 periods after the wait returned is located at the second slot (k = 2)");
    publishFrame(t4 + 2 * period, period, 33000, true);   // the display skipped a slot: the call was right
    const XrTime t5 = t4 + 2 * period;
    setNow(t5 + 3 * period);
    check(locateAs(true, 0.0f) && f.fake.lastTime == t5 + 7 * period, "on: a cached frame three periods behind now is answered at the 7th slot (3 periods and the 42 ms lead), not dropped to the wall clock");
    publishFrame(t5 + 7 * period, period, 34000, true);
    check(counts().calls == 3 && counts().cls[kSlotMatch] == 3 && counts().fallbackDelta[0] + counts().fallbackDelta[1] + counts().fallbackDelta[2] == 0,
          "...and all three matched the frame that came next, which under `next` would have been a slot short, a slot short and a fallback");
    const XrTime t6 = t5 + 7 * period;
    setNow(t6 + 400000000);
    check(locateAs(true, 0.0f) && f.fake.lastTime == t6 + 400000000, "on: 400 ms behind is past the cap: the call is located at now + prediction");
    publishFrame(t6 + 450000000, period, 35000, true);
    check(counts().cls[kSlotFallbackFirst + 4] == 1 && counts().fallbackDelta[1] == 1, "...a fallback for the cap, short by its delta");
    const XrTime t7 = t6 + 450000000;
    setNow(t7 - lead + 5000000);
    f.fake.rejectTime = t7 + period;
    check(locateAs(true, 0.0f) && f.fake.lastTime == t7 - lead + 5000000, "on: the runtime refuses the slot with XR_ERROR_TIME_INVALID: one retry at now + prediction");
    f.fake.rejectTime = -1;
    publishFrame(t7 + period, period, 36000, true);
    check(counts().cls[kSlotFallbackFirst + 5] == 1 && counts().calls == 5, "...counted as a refused fallback");
    // A frame the display never published after: the cache is dropped, the calls waiting for it are unresolved.
    setNow(t7 + period - lead + 5000000);
    check(locateAs(true, 0.0f), "(a call waiting for a next frame)");
    f.owner.invoke([&] { h.dropPoseFrame(); });
    {
      bool gone = false;
      f.owner.invoke([&] { gone = !h.latestPoseFrame.valid; });
      check(gone && counts().unresolved == 1 && counts().calls == 6, "dropping the cached frame (a session or origin change) leaves the waiting call unresolved");
    }
    // The window is written 60 s after the mode changed (31 s), by the publish point, with the late frames from the boundary.
    slotLines.clear();
    publishFrame(t7 + 3 * period, period, 91000, true);
    check(slotLines.size() == 1 && slotLines[0].find("mode slot calls 6 match 3 short 0 (1p 0, 2p+ 0) long 0 (1p 0, 2p+ 0) fallback 2 (none 0, stale 0, period 0, cross 0, cap 1, refused 1; "
                                                    "delta match 0, short 2, long 0) unresolved 1; ") != std::string::npos &&
                  slotLines[0].find("late frames 0; window 60.0 s") != std::string::npos,
          "at 60 s the window's line is written under `slot`, with the cap and the refused fallback, the call dropped with the cache unresolved, the window length");
    // ---- off again, and the host as the later cells expect it -------------------------------------------------------------------------------------------
    publishFrame(t7 + 4 * period, period, 92000, false);
    check(!slotOn(), "the key turned off again: the shipped mode");
    f.owner.invoke([&] { h.featureFrameKnown = false; h.featureFrame.cullPoseTiming = 0; h.waitReturnedAt = 0; });
    f.owner.invoke([&] { h.poseSlot.flushNow(0, h.boundary.lateFrames(), slotSink); h.poseArrival.flushNow(0, h.boundary.lateFrames(), slotSink); });
  }
  // ====================================================================================================================================
  // Arrival (advanced.cull_pose_timing = arrival, and the pose-arrival instrument, pose_arrival.h), through the host. A call is stamped when it ARRIVES, on
  // the calling thread, with the frames published so far and the last frame Elite began to submit; whether it is Between, Straddled, Render or Other is
  // told at service. A straddle is made as production makes one: the owner is held in a job (the frame wait) while the call is made from another thread
  // and queues behind it, the job publishes the next frame, and the call is served after it. `submitSeq` is set here directly (the real Submit entry is
  // held in frame_end_overlap_cases.h, where the fixture can submit); the clock is the cells' own, in nanoseconds.
  // ====================================================================================================================================
  {
    const XrTime base = 17000000000LL;
    const double savedHz = h.headClockHz;
    h.headClockHz = 1e9;
    std::vector<std::string> slotLines, arrivalLines;
    const auto slotSink = [&](const char* l) { slotLines.emplace_back(l); };
    const auto arrivalSink = [&](const char* l) { arrivalLines.emplace_back(l); };
    uint64_t atMs = 1000;
    // The publish point as WaitGetPoses reaches it, on the owner thread (also called from inside a held owner job).
    const auto publishNow = [&](XrTime displayTime, uint32_t word) {
      h.featureFrameKnown = true;
      h.featureFrame.cullPoseTiming = word;
      h.waitReturnedAt = displayTime - lead;
      atMs += 200;
      h.publishPoseGap(displayTime, period, XrPosef{{0, 0, 0, 1}, {0, 0, 0}}, true, 0, XrVector3f{0, 0, 0}, atMs, collect, slotSink, arrivalSink);
    };
    const auto publishFrame = [&](XrTime displayTime, uint32_t word) { f.owner.invoke([&] { publishNow(displayTime, word); }); };
    const auto counts = [&]() -> PoseArrivalCounts {
      PoseArrivalCounts c{};
      f.owner.invoke([&] { if (const PoseArrivalCounts* p = h.poseArrival.countsFor(me)) c = *p; });
      return c;
    };
    const auto markSubmitted = [&](bool submitted) {
      f.owner.invoke([&] {
        const uint64_t published = h.publishSeq.load();
        h.submitSeq.store(submitted ? published : published - 1);
      });
    };
    // A call that arrives at `arrivalAt`, queues behind a held owner job that publishes `newDisplay` and is served at `serviceAt`.
    const auto straddle = [&](XrTime arrivalAt, XrTime serviceAt, XrTime newDisplay, uint32_t word) {
      std::atomic<bool> started{false}, go{false};
      bool located = false;
      std::thread frameWait([&] {
        f.owner.invoke([&] {
          started.store(true);
          while (!go.load()) Sleep(1);
          publishNow(newDisplay, word);
        });
      });
      while (!started.load()) Sleep(1);
      setNow(arrivalAt);
      std::thread caller([&] { located = locateAs(true, 0.0f); });
      Sleep(150);                       // the call has been stamped and is queued behind the held job
      setNow(serviceAt);
      go.store(true);
      frameWait.join();
      caller.join();
      return located;
    };
    const auto isArrival = [&]() { bool on = false; f.owner.invoke([&] { on = h.poseArrival.mode() == PoseTiming::Arrival && h.poseSlot.mode() == PoseTiming::Arrival; }); return on; };
    const auto flushBoth = [&] {
      f.owner.invoke([&] { h.poseSlot.flushNow(0, h.boundary.lateFrames(), slotSink); h.poseArrival.flushNow(0, h.boundary.lateFrames(), arrivalSink); });
      arrivalLines.clear();
    };
    // ---- what a stamp holds ----------------------------------------------------------------------------------------------------------------------
    {
      h.publishSeq.store(41);h.submitSeq.store(40);setNow(123456789);
      HeadCall stamped;
      h.stampArrival(stamped);
      check(stamped.arrived && stamped.publishSeq == 41 && stamped.submitSeq == 40 && stamped.arrivalTicks == 123456789,
            "a stamp holds the frames published so far, the last frame Elite began to submit, and the head clock's counter, and says it was stamped");
      HeadCall bare;
      check(!bare.arrived && bare.publishSeq == 0 && bare.submitSeq == 0 && bare.arrivalTicks == 0, "(a call nobody stamped says so)");
      h.publishSeq.store(0);h.submitSeq.store(0);
    }
    flushBoth();
    // ---- next: a call that arrived between frames and was served before the publish ---------------------------------------------------------------
    yawed(0.0);
    publishFrame(base, 0);
    markSubmitted(true);
    setNow(base - lead + 5000000);
    check(locateAs(true, 0.0f) && f.fake.lastTime == base + period, "next, Between: a call after Elite's Submit, served before the next publish, is located at D + P");
    {
      const PoseArrivalCounts c = counts();
      check(c.cls[0] == 1 && c.queueN[0] == 1 && c.queueMax[0] == 0.0, "...and is counted Between with a queue of 0 ms");
    }
    // ---- a call that arrived while the frame renders (published, not yet submitted) ---------------------------------------------------------------
    const XrTime d1 = base + period;
    publishFrame(d1, 0);   // resolves the Between call: T = D + P = D' is 0p
    {
      const PoseArrivalCounts c = counts();
      check(c.bucket[0][kBucket0p] == 1, "(the next publish finds the Between call at 0p: it was located at exactly the frame that came)");
    }
    setNow(d1 - lead + 5000000);   // submitSeq is one behind publishSeq now: the frame has not been submitted
    check(locateAs(true, 0.0f) && f.fake.lastTime == d1 + period, "next, Render: a call after the publish and before Elite's Submit is located at D + P");
    {
      const PoseArrivalCounts c = counts();
      check(c.cls[2] == 1 && c.queueN[2] == 1, "...and is counted Render");
    }
    // ---- a straddle: Between at arrival, served after a publish ------------------------------------------------------------------------------------
    markSubmitted(true);
    const XrTime d2 = d1 + period;
    check(straddle(d1 - lead + 5000000, d1 - lead + 13000000, d2, 0) && f.fake.lastTime == d2 + period,
          "next, Straddled: a call that arrived after the Submit and queued across the publish of the next frame is located at D(next) + P, a period past the Between call");
    {
      const PoseArrivalCounts c = counts();
      check(c.cls[1] == 1 && c.queueN[1] == 1 && std::fabs(c.queueSum[1] - 8.0) < 1e-6 && c.bucket[1][kBucketPlus1p] == 1,
            "...counted Straddled with its 8 ms of queue, and read +1p against the first frame published after it arrived (the one cached when it was served)");
      check(c.bucket[2][kBucket0p] == 1, "(and the Render call read 0p against the publish that followed it)");
    }
    // ---- Other: Render at arrival, served after a publish -------------------------------------------------------------------------------------------
    const XrTime d3 = d2 + period;
    markSubmitted(false);
    check(straddle(d2 - lead + 5000000, d2 - lead + 9000000, d3, 0) && f.fake.lastTime == d3 + period, "next, Other: a call that arrived while the frame rendered and queued across a publish is located at D(next) + P");
    {
      const PoseArrivalCounts c = counts();
      check(c.cls[3] == 1 && c.bucket[3][kBucketPlus1p] == 1 && c.queueN[0] == 1 && c.queueN[2] == 1, "...counted Other and read +1p (Other is not timed)");
    }
    // ---- the owner-thread path: stamped where it is made, no queue -------------------------------------------------------------------------------------
    markSubmitted(true);
    bool ownerLocated = false;
    setNow(d3 - lead + 5000000);
    f.owner.invoke([&] { ownerLocated = locateAs(true, 0.0f); });
    check(ownerLocated && f.fake.lastTime == d3 + period, "a call made on the owner thread itself is stamped there and located the same way");
    {
      const PoseArrivalCounts c = counts();
      check(c.cls[0] == 2 && c.queueN[0] == 2 && c.queueMax[0] == 0.0, "...counted Between with no queue");
    }
    // ---- arrival: the same calls ---------------------------------------------------------------------------------------------------------------------
    const XrTime d4 = d3 + period;
    publishFrame(d4, 2);   // the key turned on to arrival: the `next` window is written under next; the owner-thread Between call is resolved first
    {
      const std::string wantHead = "pose arrival: tid " + std::to_string(me) + " mode next calls 5 between 2 straddled 1 render 1 other 1 fallback 0 unresolved 0; ";
      check(arrivalLines.size() == 1 && arrivalLines[0].rfind(wantHead, 0) == 0 && arrivalLines[0].find("straddled 8.00/8.00") != std::string::npos &&
                arrivalLines[0].find("between 0.00/0.00") != std::string::npos,
            "the key turned to arrival closes the window under next: five calls, two Between, one of each other class, the 8 ms straddle");
    }
    check(isArrival(), "...and the mode is arrival from the next call (both instruments)");
    markSubmitted(true);
    setNow(d4 - lead + 5000000);
    check(locateAs(true, 0.0f) && f.fake.lastTime == d4 + 2 * period, "ARRIVAL, Between: a call after the Submit, served before the next publish, is located at D + 2P: the first frame after it arrived, plus a period");
    const XrTime d5 = d4 + period;
    publishFrame(d5, 2);
    {
      const PoseArrivalCounts c = counts();
      check(c.cls[0] == 1 && c.bucket[0][kBucketPlus1p] == 1, "...and reads +1p against that frame: every between-frames call is answered a period past the first frame published after it arrived");
    }
    setNow(d5 - lead + 5000000);   // the frame is published and not submitted
    check(locateAs(true, 0.0f) && f.fake.lastTime == d5 + period, "ARRIVAL, Render: a call while the frame renders is located as next, at D + P");
    markSubmitted(true);
    const XrTime d6 = d5 + period;
    check(straddle(d5 - lead + 5000000, d5 - lead + 12000000, d6, 2) && f.fake.lastTime == d6 + period,
          "ARRIVAL, Straddled: a call that queued across the publish is located at D(next) + P, with no extra period (it already is a period past the first frame after it arrived)");
    {
      const PoseArrivalCounts c = counts();
      check(c.cls[1] == 1 && c.bucket[1][kBucketPlus1p] == 1 && c.bucket[2][kBucket0p] == 1, "...so Between and Straddled both read +1p under arrival and Render reads 0p");
    }
    const XrTime d7 = d6 + period;
    markSubmitted(false);
    check(straddle(d6 - lead + 5000000, d6 - lead + 7000000, d7, 2) && f.fake.lastTime == d7 + period, "ARRIVAL, Other: located at D(next) + P as well");
    // ---- a late frame under arrival: the slot arithmetic adds its period to the extra one ---------------------------------------------------------------
    markSubmitted(true);
    setNow(d7 - lead + period * 12 / 10);
    check(locateAs(true, 0.0f) && f.fake.lastTime == d7 + 3 * period, "ARRIVAL, Between, 1.2 periods after the wait (k = 2): located at D + 3P");
    // ---- back to next ---------------------------------------------------------------------------------------------------------------------------------
    arrivalLines.clear();
    publishFrame(d7 + 3 * period, 0);
    {
      const std::string wantHead = "pose arrival: tid " + std::to_string(me) + " mode arrival calls 5 between 2 straddled 1 render 1 other 1 fallback 0 unresolved 0; ";
      check(arrivalLines.size() == 1 && arrivalLines[0].rfind(wantHead, 0) == 0, "the key turned back to next closes the window under arrival: five calls");
    }
    check(!isArrival(), "...and the mode is next again");
    // ---- the cache dropped under a waiting call: unresolved ----------------------------------------------------------------------------------------
    markSubmitted(true);
    setNow(d7 + 3 * period - lead + 5000000);
    check(locateAs(true, 0.0f), "(a Between call waiting for the next publish)");
    f.owner.invoke([&] { h.dropPoseFrame(); });
    {
      unsigned waiting = 9;
      f.owner.invoke([&] { waiting = h.poseArrival.pending(); });
      const PoseArrivalCounts c = counts();
      check(waiting == 0 && c.unresolved == 1 && c.cls[0] == 1, "dropping the cached frame (a session or origin change) leaves the waiting call unresolved, in the arrival instrument as in the slot one");
    }
    f.owner.invoke([&] { h.featureFrameKnown = false; h.featureFrame.cullPoseTiming = 0; h.waitReturnedAt = 0; });
    flushBoth();
    h.headClockHz = savedHz;
  }
  // ---- the last two cells end the host's usefulness (a reset that cannot make its space, a fatal failure), so they come last ----------------------
  {
    setNow(display - lead);
    publish(display, period, 450000);
    check(answeredAt(display + period), "(a frame again)");
    f.fake.createResult = XR_ERROR_RUNTIME_FAILURE;
    bool refused = false, dropped = false;
    f.owner.invoke([&] { refused = !h.resetSeated(h.geometryGeneration); dropped = !h.latestPoseFrame.valid; });
    f.fake.createResult = XR_SUCCESS;
    check(refused && dropped, "a seated reset whose new space cannot be created has already invalidated the geometry publication, and drops the cached frame with it");
  }
  {
    setNow(display - lead);
    publish(display, period, 500000);
    bool dropped = false;
    f.owner.invoke([&] { h.publishFatalFailure(XR_ERROR_RUNTIME_FAILURE, "head_pose_cases"); dropped = !h.latestPoseFrame.valid; });
    check(dropped, "a fatal failure drops the cached frame");
  }
  f.owner.stop();
}
} // namespace edvr::openxr::test
