# Render thread CPU, flight of 2026-10-10 (v0.19.0)

## Status

*Opened 2026-10-10. The analysis was made with no C++ changed. The wait-lead instrument (the one C++ change, native_wait_lead and native_long_cycle_1x) is on branch claude/render-cpu-waitlead; see the Journal. Update this block when this doc changes.*

- **State:** source read at HEAD d78d9d85 (v0.19.1 and 44 later commits). The flight is v0.19.0, build 6AC9BAC9 (`module_init` line). Between them, native_runtime_host.h (+22), device_hook.cpp (+35) and vscreen.cpp (+42) gained only a projection-notice hook, a terrain-cover poll, log-only SSAA lines, a space-dust skip (vscreen.cpp:2013) and flat-only census blocks. The submit, wait, frame-end and producer-copy code is unchanged. Line numbers below are HEAD's.
- **Flight:** AMD RX 9070 XT, SteamVR through EDVR's OpenXR runtime, 2604x2644 per eye, 90 Hz, AA off, station hangar. Logs: openxr pid 5252 (215541), gfx 215539. Not run through `tools\edvr_log.py --expect-build`; the build line was checked by hand.
- **Finding 1, render thread is work-bound in the heavy windows, not GPU-bound.** Windows 4-6 and 8-9: the render thread waits 0.1-0.8 ms a frame in xrWaitFrame, so the runtime is not holding frame starts. Cycle p50 11.1-12.1 ms against an 11.11 ms period. EDVR's own render-thread work is about 1.2 ms a frame (table A) and adds straight into the cycle.
- **Finding 2, misses are whole-period.** Cycle p95 is 21-22 ms in windows 4-9, which is two periods. A frame that runs over costs a full period, so a 1 ms cut moves the miss count non-linearly; the mean alone will mislead.
- **Finding 3, the next-wait "block" is the pacer.** next_wait_queue_delay p50 0.007 ms: the frame-end job always finishes before WaitGetPoses arrives. The 0.75 ms p50 is xrWaitFrame plus xrBeginFrame on the owner thread.
- **Finding 4, windows 1-3 are not EDVR-limited.** Game work there is 1.0-1.5 ms, the render thread idles about 8 ms a frame, and misses still occur. Their tail sits after Submit (Elite's Present and after), not in EDVR's ~1.2 ms.
- **Finding 5, 2026-10-11 flight (Frontier, v0.19.1-7-gf096d5b6, Pimax Crystal Super, DLSS, NVIDIA, 77-87 fps in the hangar).** The process CPU profile (branch claude/render-cpu-profiler) sampled 9953 render-thread stops at 995 Hz, 1.37% stopped. 44% of them sit in ntdll+0x164a14 under KERNELBASE+0x22a20, called from EliteDangerous64.exe+0x5d6d7f: function 0x5d6d20 (chain 0x5d40e0, 0x5d6750, 0x54f154, 0x55cb20, the thread entry). The function locks, then waits on a condition variable with a 10 ms timeout while a global generation counter still equals the thread's last-seen value. That is a frame handshake: the render thread waits for another thread (not yet identified) to publish the next frame. EDVR's own exclusive share on the render thread is 6.76%, about 0.75 ms a frame at 86 fps.
- **Finding 6, wait-lead (same flight, the build's wait_lead_ms instrument).** In the hangar windows lead_p50 is 16.5-18.2 ms, work_p50 5.3-7.2 ms. In the worst windows the late frames split evenly: 17 with a short lead (17-18 ms), 17 with long work. SteamVR's gpu_frametime reads 8.5-10.7 ms there. What the large lead implies for the pace is not settled by these figures alone; the split is the open question.
- **Hypothesis H6 (new), the pace is set by Elite's producer thread, not by the render thread.** The render thread has slack; a second thread publishes the frame and the render thread waits for it. **Discriminator:** the producer thread's CPU near 100% of the frame period, while the render thread's `waits for producer` share is high. A producer near idle with a high wait share refutes it; a producer near 100% with the render thread's wait share low refutes it the other way.
- **EDVR's render-thread overhead is demoted.** At 6.76% of the render thread's samples it is about 0.75 ms a frame; it does not set the pace on this flight. H1 below is kept only as the mechanism that would cost frames if the producer were ever EDVR's.
- **Open hypotheses**, each with its discriminating signature:
  - H1, EDVR work tips the frame over. Signature: missed cycles have cycle minus next_wait above one period. Demoted by Finding 5; the 2x long-cycle threshold still hides 1-2 period misses (native_long_cycle_1x now counts them).
  - H2, OpenXR pacing starts the frame later. Signature: per wait return, lead = predictedDisplayTime minus the return time, shrinking as EDVR work grows. LOGGED (native_wait_lead) and flown once on Sean's rig (Finding 6): the late frames split evenly, so it is not settled.
  - H3, GPU limits windows 3 and 7 (gpu_frametime 10.5 and 8.0 ms). Signature: late cycles that coincide with real-Present blocking. Not yet seen; post2wait p95 is 4.3-4.7 ms in those windows.
  - H4, producer AcquireSync costs about 0.2 ms uncontended (a fixed handshake, not a wait). Signature: a timestamp before and after the call, with an immediate return. Current p99 0.44 ms, so a real wait is unlikely. NOT SPLIT.
  - H5, most of the 0.36 ms draw-hook cost is EDVR reissues. Signature: first-call versus reissue split. NOT SPLIT.
  - H7, Elite's job thread blocks on owner round-trips. The 2026-10-11 CPU profile (cpu_profile_20261011_052742.csv, v0.19.1-11-geb9ee3df) puts GetDeviceToAbsoluteTrackingPose (locateHeadFor) at 2.5 ms and PollNextEvent at 0.95 ms a frame, inside OwnerService::invoke; the owner serves one request at a time, so these queue behind waitPoses. Signature: queue_ms of locateHead and pollEvent is large and behind_wait is high on CPU-bound frames. If queue is near zero and run carries the time, H7 is refuted for that kind. Instrument ON BRANCH claude/render-cpu-ownerhop (native_owner_invoke), NOT FLOWN. Research and the design are in the 2026-10-11 entry.
- **Ruled out:**
  - ruled out: GPU-bound in windows 4-6 and 8-9, because gpu_frametime reads 3.8-5.2 ms there while the render thread waits 0.1-0.8 ms. NOT ruled out in windows 3 and 7 (10.5 and 8.0 ms). Correction to the request: the nine windows read 0.75 to 10.55 ms, not 5.1-5.2, and EDVR's producer copy is 0.056 ms p50 (native_producer_gpu window 9), not 0.04.
  - ruled out: periodic render-thread jobs as the cause of long cycles, because only 2 of 54 coincide (perf review, 2026-09-29).
  - ruled out: xrWaitFrame overlapping xrEndFrame, because both run on the owner thread in FIFO order (native_runtime_host.h:1361), and next_wait_queue_delay shows the end job already done.
  - ruled out: per-frame SRV creates, because scene_srv_creates=4 per 256-pair window and the SRVs are cached (openxr-submit-overlap-2026-09-15.md).
  - ruled out: a subscription gate as new work, because draw_gate.h (2026-09-22) already exists and vscreen.cpp:1960 reads it.
  - ruled out: the owner rendezvous as a cost, because it measured 10-60 us (perf review, 2026-09-14), and queue delay agrees here.
- **Next flight** (same hangar, the test branch claude/render-cpu-test: profiler v2 plus the wait-lead lines):
  1. Read the producer thread's CPU from the multi-thread report: `thread=main` (or the busy `t<tid>`) `cpu_pct` near 100% of the frame period tests H6.
  2. Read `thread=render waits for producer: ms_per_frame` against the frame period, on the gated build only (EliteDangerous64.exe stamp 1788384820, image 104894464).
  3. Read `native_wait_lead` and `native_long_cycle_1x` for the same windows.
  4. Keep H4 and H5 split timestamps; they remain unsplit.
  No cut until items 1 and 2 show which thread sets the pace.

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

### 2026-10-11 -- the Frontier flight of the process profile, and the producer wait

Flight: Frontier, v0.19.1-7-gf096d5b6 (branch origin/claude/render-cpu-test, the profiler merged with the wait-lead and hotspot branches). Log `edvr_gfx_20261011_045120.log`, CSV `cpu_profile_20261011_045750.csv`. 9953 render-thread samples at 995 Hz, 1.37% of wall stopped.

What the samples show. 44% of the render thread's samples sit at ntdll+0x164a14 under KERNELBASE+0x22a20, called from EliteDangerous64.exe+0x5d6d7f. The caller chain is 0x5d6d20 (the function, 0x5d6d20..0x5d6de2), 0x5d40e0, 0x5d6750, 0x54f154, 0x55cb20, the thread entry. Disassembled at 0x5d6d20: it locks the object at rdi, then calls a condition-variable sleep (rcx = rdi+8, rdx = rdi, a 10 ms timeout) while the global generation counter at RVA 0x5d6d5f+7+0x595103a still equals the thread's last-seen value at [rcx+8]. The other branch clears a flag with lock cmpxchg, increments that counter and wakes. So this is a frame handshake: the render thread waits for another thread to publish the next frame. EDVR's own exclusive share on the render thread is 6.76%, about 0.75 ms a frame at 86 fps.

Wait-lead, from the same rig (Pimax Crystal Super, DLSS, NVIDIA). In the hangar windows lead_p50 is 16.5-18.2 ms and work_p50 is 5.3-7.2 ms. In the worst windows the late frames split 17 with a short lead (17-18 ms) against 17 with long work, at 77-87 fps. SteamVR's gpu_frametime reads 8.5-10.7 ms there.

What this changes. The pace is set by the thread that publishes the frame, not by EDVR's render-thread work, which is demoted above. Hypothesis H6 (Status): the producer thread runs near 100% of the frame period while the render thread's wait share is high. The profiler v2 measures both: the producer's CPU (`thread=main` or the busy `t<tid>` lines) and the render thread's `waits for producer` line, which reports ms per frame over the window's Present count, on the gated build only (stamp 1788384820, image 104894464, the build 332841 gate of ui_sizing_math.h).

Instrument, profiler v2. The window samples the render thread (Present), the main thread (the earliest-created), and the four others with the most CPU over a one-second pre-window, round-robin at 1200 Hz combined, under the same 5% stopped-share cap. Each thread has its own lines: `cpu profile: thread=<label> role=... cpu_ms=... cpu_pct=...` (GetThreadTimes over the window), its top 20 functions (inclusive and exclusive), and its classes. The CSV has `thread` as its first column; `elite_hotspots.py --thread` filters it.

Not yet known: which thread is the producer. The 0x5d6d20 handshake names a counterpart, but the flight did not record it; the multi-thread report does.

### 2026-10-11: owner hops for locateHead and pollEvent (research, and the owner-invoke instrument)

Branch claude/render-cpu-ownerhop, cut from origin/claude/render-cpu-test (eb9ee3df). The hops are unchanged; this entry adds the instrument and the research the coordinator asked for, on the hypothesis H7 in Status.

**Instrument, always on, no config key.** `native_owner_invoke,window=N,kind=K,calls=..,per_frame=..,queue_p50/p95=..,run_p50/p95=..,behind_wait=..,dropped=..`, one line per kind per frame-cycle window (src/openxr/owner_invoke_stats.h, formatOwnerInvokeLine; the owner records the times in OwnerService::invoke and run). Kinds: waitPoses (the xrWaitFrame hop), locateHead (locateHeadFor), pollEvent (pollEvent), submit (the synchronous handoff), other. queue = the owner starts the job minus the caller's enqueue; run = the job body. behind_wait counts requests that arrived while the owner was inside a waitPoses job. per_frame = calls divided by the window's valid cycles. Counted: requests that ran (not rejected, cancelled, or run directly on the owner thread). Not counted: the overlapped frame-end jobs (OwnerService::submit has no caller wait). Clock: the same QPC microseconds as the other lines; three reads per hop. The sample is taken on the caller after its wait, under a mutex, and the window is drained at the frame-cycle report. Read it with H7 below.

**(a) Is an off-owner call legal in OpenXR 1.0?** Read through WebFetch from the Khronos registry (specification/registry/xr.xml, the command entries; the externsync values are my reading of that fetched rendering, not a byte-for-byte copy). xrPollEvent: `instance` and `eventData`, no externsync. xrLocateSpace: `space` and `baseSpace`, no externsync. xrLocateViews: `session` and the output parameters, no externsync. xrWaitFrame: `session` is externally synchronized, by its implicitexternsyncparams block ("the pname:session parameter by any other flink:xrWaitFrame call"). The default rule (specification/sources/chapters/fundamentals.adoc, Threading Behavior, as fetched): "certain parameters, or components of parameters are defined to be externally synchronized", and the caller "must guarantee that no more than one thread is using such a parameter at a given time." So a locate or a poll from another thread is legal under the spec when it does not overlap another call on the same externally synchronized parameter; the locate calls do not share `session` with xrWaitFrame. The man page for xrPollEvent says it "returns immediately regardless of whether an event was available" (fetched). Not verified: the full Threading Behavior section (the spec page was over WebFetch's size limit) and the man-page threading blocks (the fetched text had none). Not in the spec, so unknown: whether VDXR, SteamVR or Pimax tolerate xrLocateSpace concurrent with xrWaitFrame. Untested here.

**(b) A documented runtime quirk that forced the hop?** None found. The docs give EDVR's own reasons. docs/openxr-owner-service-2026-09-12.md: "Handle-using System methods now dispatch synchronously to the owner", and "Cached publications continue to use their own short locks and bypass this queue". That is a serialization policy, not a vendor finding. The handle gate (docs/openxr-frame-boundary-2026-09-12.md; locateHeadOwned's comment: "Handle-using calls fail promptly while another operation is in flight or shutdown has been requested") is the other reason. The pollEvent hop's comment (src/openxr/native_runtime_host.h:197): "pollEvent is also marshalled to the owner, so no cross-thread event queue is needed". That chose to avoid a queue; it is partly stale, because ResetEvents (src/openxr/reset_events.h) is already a mutex-guarded cross-thread queue, used by the reset path. The ABI census (docs/openxr-flight-2026-09-11.md) saw System calls on three Elite threads and concluded "System snapshots must therefore be safe for concurrent readers". No doc records a runtime that failed when called off the owner.

**(c) Does the display-time logic need owner state?** It needs owner state, not the owner thread as such. locateHeadFor's "now" answer (the cull-guard design: the game's "now" pose answered at the drawn frame's display time plus one period) reads: latestPoseFrame (src/openxr/native_runtime_host.h, the frame WaitGetPoses last gave the game; "Written and read on the owner thread only"); boundary.estimateNow() (the FrameBoundary's last real wait, owner-only); changes.crosses (ReferenceChanges); and the owner-written head-locate state (lastHeadTime, lastHeadResult, the failure counters, displayTimeRefusals). The arithmetic itself, poseTargetFor and answerTime (native_runtime_host.h ~2333; head_pose_time.h), is pure over those inputs. The lease matters most: gate.tryEnter is held by the owner across the frame loop, so a direct caller-thread locate would be refused promptly in exactly the frames Elite asks.

**Design.**
1. locateHead, recommended: publish a cached pose for the one instant the cull-guard answer uses in steady state. At each game wait the owner already locates at predictedDisplayTime (locateGeometry); one more xrLocateSpace at display + period, stored with (origin generation, reference generation, display time, orientation, speed), in a mutex-guarded publication like poses.read(). locateHeadFor on the caller returns it when the call is a display call, the publication is fresh and the same generation, and the instant matches; any other call (prediction calls, stale, refused) takes the owner hop unchanged. The pose-gap sample still records the caller's thread and RVA. Cost: one extra owner locate a frame, unmeasured here.
2. locateHead, alternative: a direct xrLocateSpace on the caller under a read-only lease that waitFrame does not block. Spec-legal per the registry, but runtime safety against a concurrent xrWaitFrame is vendor-defined and untested; the gate is exclusive by design; shutdown would have to wait for in-flight caller locates before destroying the space. Not first.
3. pollEvent: the owner fills a bounded event ring at each wait and idle pump; PollNextEvent drains it under a mutex (the ResetEvents pattern). Owner-only state to move: quitEventPending (make it guarded), the geometry-generation check (a guarded copy), resetPolls (atomic). Drop policy when the ring is full must be explicit.

**Risks.** (1) The ownerThread checks in locateHeadOwned and the pose-gap thread bookkeeping assume the owner. (2) Vendor thread safety off the owner is untested on VDXR, SteamVR and Pimax, whatever the spec allows. (3) Staleness: a cached pose must be dropped wherever dropPoseFrame is called (origin, geometry, session loss), and on a reference change. (4) Lifetime: the publication must not outlive the session; retire it with the session. (5) Removing 3.4 ms of caller block may not move the frame if the time moves into the owner: the owner's run time is the next thing to see. (6) The extra owner locate lengthens waitPoses, which other callers queue behind.

**Verification plan.** H7 stands if, on CPU-bound frames, locateHead and pollEvent show queue p50 in ms and behind_wait close to calls, with cycle misses in the same windows (native_wait_lead, native_frame_cycle_phase). H7 is refuted for a kind whose queue is near zero, where run carries the 2.5 ms or 0.95 ms; then the cost is inside the owner's body and the next question is what that body does. Rig: owner_invoke_cases.h (16 checks, a fake owner clock, a real waitPoses job holding the owner, a pollEvent queued behind it); edvr_log.py self-test covers parse_owner_invoke.

**Verification and cost (this branch).** Full `build.bat` green, receipt written. The owner rig (owner_invoke_cases.h) passes 16 checks, three runs with the same result; no new compiler warnings in the changed files. `tools\edvr_log.py --self-test` ends "self-test: ok" with parse_owner_invoke covered. Desk cost, /O2: the accounting itself is 13 ns a hop; the host adds three QueryPerformanceCounter reads a hop (about 16 ns each, 48 ns), so about 60 ns a hop. The window drain takes about 0.18 ms for 2700 samples. The owner's own time is not measured: the clock is read only when the owner was given stats, and only for requests that run. Not measured here: how much the queue shrinks in a flight, which needs the CPU profile again.