# Render thread CPU, flight of 2026-10-10 (v0.19.0)

## Status

*Opened 2026-10-10. The analysis was made with no C++ changed. The wait-lead instrument (the one C++ change, native_wait_lead and native_long_cycle_1x) is on branch claude/render-cpu-waitlead; see the Journal. Update this block when this doc changes.*

- **State:** source read at HEAD d78d9d85 (v0.19.1 and 44 later commits). The flight is v0.19.0, build 6AC9BAC9 (`module_init` line). Between them, native_runtime_host.h (+22), device_hook.cpp (+35) and vscreen.cpp (+42) gained only a projection-notice hook, a terrain-cover poll, log-only SSAA lines, a space-dust skip (vscreen.cpp:2013) and flat-only census blocks. The submit, wait, frame-end and producer-copy code is unchanged. Line numbers below are HEAD's.
- **Flight:** AMD RX 9070 XT, SteamVR through EDVR's OpenXR runtime, 2604x2644 per eye, 90 Hz, AA off, station hangar. Logs: openxr pid 5252 (215541), gfx 215539. Not run through `tools\edvr_log.py --expect-build`; the build line was checked by hand.
- **Finding 1, render thread is work-bound in the heavy windows, not GPU-bound.** Windows 4-6 and 8-9: the render thread waits 0.1-0.8 ms a frame in xrWaitFrame, so the runtime is not holding frame starts. Cycle p50 11.1-12.1 ms against an 11.11 ms period. EDVR's own render-thread work is about 1.2 ms a frame (table A) and adds straight into the cycle.
- **Finding 2, misses are whole-period.** Cycle p95 is 21-22 ms in windows 4-9, which is two periods. A frame that runs over costs a full period, so a 1 ms cut moves the miss count non-linearly; the mean alone will mislead.
- **Finding 3, the next-wait "block" is the pacer.** next_wait_queue_delay p50 0.007 ms: the frame-end job always finishes before WaitGetPoses arrives. The 0.75 ms p50 is xrWaitFrame plus xrBeginFrame on the owner thread.
- **Finding 4, windows 1-3 are not EDVR-limited.** Game work there is 1.0-1.5 ms, the render thread idles about 8 ms a frame, and misses still occur. Their tail sits after Submit (Elite's Present and after), not in EDVR's ~1.2 ms.
- **Open hypotheses**, each with its discriminating signature:
  - H1, EDVR work tips the frame over. Signature: missed cycles have cycle minus next_wait above one period. Partly visible now; the 2x long-cycle threshold hides 1-2 period misses.
  - H2, OpenXR pacing starts the frame later. Signature: per wait return, lead = predictedDisplayTime minus the return time, shrinking as EDVR work grows. LOGGED on branch claude/render-cpu-waitlead (native_wait_lead), NOT FLOWN.
  - H3, GPU limits windows 3 and 7 (gpu_frametime 10.5 and 8.0 ms). Signature: late cycles that coincide with real-Present blocking. Not yet seen; post2wait p95 is 4.3-4.7 ms in those windows.
  - H4, producer AcquireSync costs about 0.2 ms uncontended (a fixed handshake, not a wait). Signature: a timestamp before and after the call, with an immediate return. Current p99 0.44 ms, so a real wait is unlikely. NOT SPLIT.
  - H5, most of the 0.36 ms draw-hook cost is EDVR reissues. Signature: first-call versus reissue split. NOT SPLIT.
- **Ruled out:**
  - ruled out: GPU-bound in windows 4-6 and 8-9, because gpu_frametime reads 3.8-5.2 ms there while the render thread waits 0.1-0.8 ms. NOT ruled out in windows 3 and 7 (10.5 and 8.0 ms). Correction to the request: the nine windows read 0.75 to 10.55 ms, not 5.1-5.2, and EDVR's producer copy is 0.056 ms p50 (native_producer_gpu window 9), not 0.04.
  - ruled out: periodic render-thread jobs as the cause of long cycles, because only 2 of 54 coincide (perf review, 2026-09-29).
  - ruled out: xrWaitFrame overlapping xrEndFrame, because both run on the owner thread in FIFO order (native_runtime_host.h:1361), and next_wait_queue_delay shows the end job already done.
  - ruled out: per-frame SRV creates, because scene_srv_creates=4 per 256-pair window and the SRVs are cached (openxr-submit-overlap-2026-09-15.md).
  - ruled out: a subscription gate as new work, because draw_gate.h (2026-09-22) already exists and vscreen.cpp:1960 reads it.
  - ruled out: the owner rendezvous as a cost, because it measured 10-60 us (perf review, 2026-09-14), and queue delay agrees here.
- **Next flight** (same hangar, one build from HEAD with `build.bat` run and tests passing):
  1. Built on branch `claude/render-cpu-waitlead` (SHA in the 2026-10-10 entry below, not merged, not installed): per 30 s window, `native_wait_lead` (lead of predictedDisplayTime over each game wait return, frame work to xrEndFrame, late frames split by cause) and `native_long_cycle_1x` (cycles between 1x+0.5 ms and 2x period, with the work split). Settles H2 against H1 by the rule in the entry below. Read both lines in one flight.
  2. The 1.0x census is done as item 1's `native_long_cycle_1x` line, not as a change to `native_long_cycle`, which keeps its 2x threshold.
  3. Separate timestamps around producer AcquireSync (H4) and first-call versus reissue draw time (H5), still not split.
  No cut until item 1 shows which side limits the frame.

## Evidence

Window 9 of openxr log (native_frame_cycle_phase, ms):

| Item | Mean | p50 | p95 | Thread | Code |
|---|---|---|---|---|---|
| cycle | 11.88 | 11.13 | 20.91 | render | window line |
| game_before_first_submit | 7.07 | 6.87 | 8.48 | render (Elite + draw hooks) | - |
| first_submit_roundtrip (A) | 0.37 | 0.36 | 0.50 | render, direct | native_runtime_host.h:1309-1334 |
| second_submit_roundtrip (A) | 0.30 | 0.28 | 0.41 | render, direct | same |
| post_second_submit_to_next_wait | 2.91 | 2.77 | 3.66 | render | native_runtime_host.h:2159-2165 |
| edvr_after_real_present (B) | 0.19 | 0.11 | 0.71 | render | device_hook.cpp:1722ff |
| after_single_present | 2.56 | 2.45 | 3.28 | mostly Elite | same |
| next_wait_roundtrip (C) | 1.23 | 0.75 | 8.32 | render parked | native_runtime_host.h:1006-1066 |
| next_wait_owner_body | 1.21 | 0.74 | 8.31 | owner 7296 | native_runtime_host.h:1021-1028, 1119 |
| next_wait_queue_delay | 0.007 | 0.007 | 0.010 | owner FIFO | owner_service.h:47 |
| frame_end_owner_body | 1.16 | 1.09 | 1.35 | owner 7296 | native_runtime_host.h:1421-1423 |

Submit sub-phases, 256-pair window (p50 / p95 / p99, ms): producer_dispatch 0.50/0.79/0.90; producer_acquire 0.23/0.37/0.44; producer_flush 0.11/0.20/0.26; consumer_acquire 0.09/0.15/0.19; consumer_flush 0.10/0.13/0.14; receive 0.22/0.30/0.38; xr_end_frame 0.71/0.95/1.10; wait_frame 0.62/8.39/9.57. Producer copy GPU (window 9): 0.056 / 0.082 / 0.124.

Draw hook CPU (gfx log, sampled one frame in 16): 0.00-0.06 ms a frame in the first five 1800-frame windows, then 0.35-0.39 ms a frame from the window ending at frame 12600. The cost is scene-dependent; it includes EDVR reissues and excludes forwarded game draws.

Per window (cycle and wait in ms, late = missed periods in the window):

| Window | Cycle p50 | Game p50 | Wait p50 | Late | gpu_frametime |
|---|---|---|---|---|---|
| 1 (partial, 1448 frames) | 11.13 | 1.08 | 7.96 | 609 | 0.75 |
| 2 | 11.12 | 1.41 | 7.73 | 175 | 0.76 |
| 3 | 11.13 | 1.49 | 7.59 | 150 | 10.55 |
| 4 | 11.92 | 7.64 | 0.15 | 495 | 4.23 |
| 5 | 12.14 | 7.75 | 0.12 | 525 | 4.12 |
| 6 | 11.15 | 7.26 | 0.30 | 277 | 3.80 |
| 7 | 11.13 | 6.99 | 0.63 | 244 | 8.03 |
| 8 | 11.16 | 7.29 | 0.32 | 311 | 5.20 |
| 9 | 11.13 | 6.87 | 0.75 | 171 | 5.09 |

Session: native_pacing_summary late_frames=3143 over about 22,050 overlapped frames. native_frame_end_overlap_summary: overlapped 22049, failures 0.

## Thread model

- Render thread (16516): game work; both submits (`native_submit_route,path=direct`, so the body runs on the caller); the post-Present hook; the WaitGetPoses caller hop.
- Owner thread (7296): the waitPoses body (xrWaitFrame and xrBeginFrame, session_state.h:124-133, frame_boundary.h:116 under Runtime pacing); the FIFO frame-end job (native_runtime_host.h:1361) that runs xrEndFrame, the consumer copy (shared_texture_transfer.cpp:307-311) and compose.
- The FramePacer thread (frame_pacer.h) runs only under Deferred (turbo) pacing. Here pacing=0 and kicks=0, so it is inert.

## Answers to the four questions

**1. What each item does.** The submit roundtrips run on the render thread. producerCopy (shared_texture_transfer.cpp:252-291) does AcquireSync (262-263), CopyResource (269), an explicit Flush (270-271), then ReleaseSync. The consumer half (receive 0.22 ms) runs on the owner inside finishPair. The next-wait roundtrip is not a join of xrEndFrame: the caller hops to the owner (native_runtime_host.h:1021), where the FIFO has already run the frame-end job, and the body then calls xrWaitFrame and xrBeginFrame. Overlap is between the frame-end job and Elite's post-Submit work, which is about 2.9 ms on the render thread. edvr_after_real_present is the frame boundary after the real Present (device_hook.cpp:1722, presentFrameBoundary), 0.19 ms mean. The draw hook is DrawClock (vscreen.cpp:4645-4667), which times one frame in 16 and one draw in 64 (perf_monitor.h:128-145), with the thunk at vscreen.cpp:4693.

**2. Cuts.** See the ranked list below.

**3. Pacing or overhead.** Windows 4-6 and 8-9 show wait near zero, so the runtime is not holding frames; the cycle equals the work, and EDVR's 1.2 ms adds directly. That favours H1 in those windows. Windows 1-3 hold the render thread for 8 ms a frame and still miss, with the tail after Submit, so pacing or Elite's Present explains them, not EDVR. The two cannot be separated from the present fields: the lead of predictedDisplayTime over the wait return (H2) is not logged. One flight with `wait_lead_ms` and the 1.0x work field answers it.

**4. Prior work.** The perf review (2026-09-14) rejected deleting flushes and removing xrWaitFrame; both stay rejected here. frame_end_overlap is on (2026-09-24, flight 124504 clean; 125717 on SteamVR, second-Submit park 0.16-0.19 ms). Its 2026-09-29 entries rule out the periodic jobs, EDVR's Present hook, AA and engine motion for the stalls. The gate and SRV items are already done (see Status).

## Ranked cuts

Savings are upper bounds from p50 phase times; none is measured. Risk is the change to the pixels or the sync protocol.

| Rank | Cut | Where | Est. ms/frame | Risk | Note |
|---|---|---|---|---|---|
| 1 | Move non-frame work (journal poll, menu, hotkeys) off the post-Present path | device_hook.cpp:1687-1800 | 0.05-0.10 of 0.19 | low-med | Only the mean is EDVR's; p95 0.71 |
| 2 | Sample the producer GPU timer instead of every copy | producer_gpu_timing.cpp:36-52; shared_texture_transfer.cpp:265-272 | under 0.02 per eye (unmeasured) | low | Costs GPU-timing density |
| 3 | Fewer EDVR reissues, or a leaner draw thunk | vscreen.cpp:4693; DrawClock 4645 | 0.1-0.2 of 0.36 | med-high | Needs H5 split first |
| 4 | Remove the producer Flush per eye | shared_texture_transfer.cpp:270-271 | about 0.2 (p50 0.11 x2) | high | Cross-device ordering before ReleaseSync; untested |
| 5 | Replace the producer keyed mutex with a fence | shared_texture_transfer.cpp:262-263 | up to 0.45 (0.23 x2) | high, large | Do H4 first; changes the consumer protocol |
| 6 | Join frame-end later, not before WaitFrame | native_runtime_host.h:1361 | about 0.01 now | low saving; med risk | Worth it only if post-Submit shrinks below 1.1 ms |

Not recommended:
- Batching the two eye copies: each eye's texture must be consumed before its Submit returns, so eye 1 cannot be deferred without a texture-lifetime contract that is not in evidence.
- Per-frame SRV creates: 4 per 256 pairs; nothing to cut.
- Subscription gate: already in place (draw_gate.h).

## Caveats

- The log is v0.19.0; the code is HEAD. The submit and wait logic is unchanged, but measure on HEAD before cutting.
- The 2x long-cycle threshold means misses of one or two periods are not logged per cycle. Their composition is inferred from window percentiles.
- gpu_frametime is per window and the nine windows disagree (0.75 to 10.55 ms), so the GPU cannot be ruled out in windows 3 and 7.

## Journal

### 2026-10-10: wait-lead instrument (branch claude/render-cpu-waitlead, not merged, not installed)

Added, always on, no config key. The arithmetic is src\openxr\wait_lead_stats.h (header only). The host hooks it in three places: the owner's game wait return in waitPoses (real begins only, so not a turbo-synthesized frame), the xrEndFrame call (endFrameReturned, background frames excluded), and reportFrameCycles, which drains it at each frame-cycle window and so shares the 30 s window with native_frame_cycle_window. Lead converts predictedDisplayTime with xrConvertTimeToWin32PerformanceCounterKHR, loaded optionally with the existing convertTime; a missing entry point leaves lead at na and late still counts. The arithmetic fallback the request allowed does not give a lead: the only clock-to-XrTime calibration the code has (estimateNow) is set at the same wait, so the lead would be zero. Not implemented.

Lines, one pair per 30 s frame-cycle window:

    native_wait_lead,window=N,lead_p5/p50/p95=a/b/c,work_p50/p95/p99=d/e/f,late=K,late_short_lead=A,late_long_work=B,period_ms=P,frames=F,dropped=D
    native_long_cycle_1x,window=N,period_ms=P,count=C,cycle_p50=X,work_p50=W,work_ge_period=G,work_lt_period=L

Definitions (also in the header comment):
- Per game frame N: lead = predictedDisplayTime (QPC us) minus the wait's return; work = the start of N's xrEndFrame call minus that return; cycle = the next game wait's return minus this one's.
- late: the next game wait's predicted time skipped a period past N's, round((pred_next - pred_N) / period) - 1 >= 1. Same arithmetic as the late_frames counter, attributed to the frame it is about. Known only at the next wait, so a frame is committed then. late=K counts frames; late_frames counts skipped periods, so the two differ when a frame skips two.
- late_short_lead: late and lead < period - W50, W50 the window's work p50. The runtime gave less than a period minus a typical frame's work.
- late_long_work: late and work > lead. A frame can be in both. A late frame with no converted lead counts in late only.
- native_long_cycle_1x: cycles with period + 0.5 ms < cycle < 2 periods. work_ge_period: that frame's work is at least a period. work_lt_period: its work is under a period, so the cycle grew after xrEndFrame or in the wait.
- Not counted: loading frames, synthesized deferred frames, failed waits. The lines come only from the caller-thread path; a direct owner call drains nothing (as with native_frame_cycle_window). frames=F is the committed count; dropped=D counts frames past the 8192 capacity (about 90 s at 90 Hz). na: no sample in the window.

How to read it. The thresholds are the rule above; the expected values are not known yet, so the first clean window (late=0) sets the baseline lead p50.
- H1, EDVR work tips the frame over: late_long_work takes most of the late frames, and most of the census is work_ge_period.
- H2, the runtime starts the frame late: late_short_lead takes most of the late frames, lead p50 sits well under a period in windows that miss, and most of the census is work_lt_period.
- Late frames with both splits at zero: the miss is not in this frame's own lead or work. Look after Present (edvr_after_real_present, after_single_present).

Verification on this branch: wait_lead_cases.h (24 checks: steady, a short-lead late frame, a long-lead late frame, jitter, the census split, pending and no-end, unconverted lead, capacity, empty window, and the line format parsed back) passes, and `tools\edvr_log.py --self-test` covers parse_wait_lead and parse_long_cycle_1x. Full build.bat green (receipt written). Cost, measured on the desk: 16 ns per QueryPerformanceCounter read, 28 ns per frame of bookkeeping including the window drain (about 0.05 ms per 30 s window to build the report). The per-frame runtime call, xrConvertTimeToWin32PerformanceCounterKHR, is not measured here; it needs a headset session.

Next flight: the same hangar, a build from this branch (or from main once merged, which is not done here), read native_wait_lead and native_long_cycle_1x per window alongside native_frame_cycle_phase. One flight with the build line checked (`tools\edvr_log.py --expect-build HEAD`) first.
