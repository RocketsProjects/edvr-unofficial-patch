# Render thread CPU, flight of 2026-10-10 (v0.19.0)

## Status

*Opened 2026-10-10. Analysis only; no C++ changed. Update this block when this doc changes.*

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
  - H1, EDVR work tips the frame over. Signature: missed cycles have cycle minus next_wait above one period. Demoted by Finding 5; the 2x long-cycle threshold still hides 1-2 period misses.
  - H2, OpenXR pacing starts the frame later. Signature: per wait return, lead = predictedDisplayTime minus the return time, shrinking as EDVR work grows. NOT LOGGED.
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
- **Next flight** (same hangar, the profiler v2 build, branch claude/render-cpu-profiler):
  1. Read the producer thread's CPU from the multi-thread report: `thread=main` (or the busy `t<tid>`) `cpu_pct` near 100% of the frame period tests H6.
  2. Read `thread=render waits for producer: ms_per_frame` against the frame period, on the gated build only (EliteDangerous64.exe stamp 1788384820, image 104894464).
  3. Keep H4 and H5 split timestamps; they remain unsplit.
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

## 2026-10-11 -- the Frontier flight of the process profile, and the producer wait

Flight: Frontier, v0.19.1-7-gf096d5b6 (branch origin/claude/render-cpu-test, the profiler merged with the wait-lead and hotspot branches). Log `edvr_gfx_20261011_045120.log`, CSV `cpu_profile_20261011_045750.csv`. 9953 render-thread samples at 995 Hz, 1.37% of wall stopped.

What the samples show. 44% of the render thread's samples sit at ntdll+0x164a14 under KERNELBASE+0x22a20, called from EliteDangerous64.exe+0x5d6d7f. The caller chain is 0x5d6d20 (the function, 0x5d6d20..0x5d6de2), 0x5d40e0, 0x5d6750, 0x54f154, 0x55cb20, the thread entry. Disassembled at 0x5d6d20: it locks the object at rdi, then calls a condition-variable sleep (rcx = rdi+8, rdx = rdi, a 10 ms timeout) while the global generation counter at RVA 0x5d6d5f+7+0x595103a still equals the thread's last-seen value at [rcx+8]. The other branch clears a flag with lock cmpxchg, increments that counter and wakes. So this is a frame handshake: the render thread waits for another thread to publish the next frame. EDVR's own exclusive share on the render thread is 6.76%, about 0.75 ms a frame at 86 fps.

Wait-lead, from the same rig (Pimax Crystal Super, DLSS, NVIDIA). In the hangar windows lead_p50 is 16.5-18.2 ms and work_p50 is 5.3-7.2 ms. In the worst windows the late frames split 17 with a short lead (17-18 ms) against 17 with long work, at 77-87 fps. SteamVR's gpu_frametime reads 8.5-10.7 ms there.

What this changes. The pace is set by the thread that publishes the frame, not by EDVR's render-thread work, which is demoted above. Hypothesis H6 (Status): the producer thread runs near 100% of the frame period while the render thread's wait share is high. The profiler v2 measures both: the producer's CPU (`thread=main` or the busy `t<tid>` lines) and the render thread's `waits for producer` line, which reports ms per frame over the window's Present count, on the gated build only (stamp 1788384820, image 104894464, the build 332841 gate of ui_sizing_math.h).

Instrument, profiler v2. The window samples the render thread (Present), the main thread (the earliest-created), and the four others with the most CPU over a one-second pre-window, round-robin at 1200 Hz combined, under the same 5% stopped-share cap. Each thread has its own lines: `cpu profile: thread=<label> role=... cpu_ms=... cpu_pct=...` (GetThreadTimes over the window), its top 20 functions (inclusive and exclusive), and its classes. The CSV has `thread` as its first column; `elite_hotspots.py --thread` filters it.

Not yet known: which thread is the producer. The 0x5d6d20 handshake names a counterpart, but the flight did not record it; the multi-thread report does.