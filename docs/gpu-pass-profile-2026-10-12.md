## Status

State: BUILT, NOT FLOWN. Branch claude/gpu-pass-profile. A measurement only: no draw, binding or shader changes.
v13.5 (2026-10-12, claude/hangar-test-9): merged into the sealed-hangar trial's branch (merge commit 9e910077, DLL v0.19.1-107-g9e910077; see the concourse doc's v13.5 section). The merge changes no behaviour. The interaction with the other instruments on the same clock: see "Interaction" below. v13.6 (commit 101708de, DLL v0.19.1-109-g101708de): while a capture is armed or running, the application-frame timing and the census timers yield the shared clock, so a profiled frame can open with the default settings: see "The yield (v13.6)".
Key: hotkey.gpu_pass_profile (TEMPORARY, default unbound; flat allow-list in runtime_profile.h; commented in edvr.ini).
Question: where Elite's own GPU time goes each frame. Sean sees about 9 ms GPU in fpsVR in a station hangar whatever the view
direction. EDVR's census prices its own share (about 5.5 ms) and estimates the game's (about 5 ms); neither splits Elite's frame.

Open hypotheses (each has the signature that confirms or refutes it in the CSV):
- H1 probes: reflection-probe cube faces re-rendered on turns. Confirm: probe_cube rows with a run of six same-size square
  passes, ms per frame large and stable across view directions. Refute: probe_cube under about 0.3 ms.
- H2 shadows: sun and light shadow maps. Confirm: shadow rows; their count per frame (1 means once per frame, 2 means per eye).
- H3 per-eye duplication: scene_gbuffer and forward_at_eye rows come in pairs (one set per eye). Confirm: the pass count per
  frame is even and the two halves match in ms. Refute: a single eye-sized set per frame.
- H4 full-screen passes at eye size: fullscreen_at_eye total ms. Confirm if it is more than about 2 ms per frame.
- H5 post chains: post_smaller total ms and the sizes in its signatures (downsampled targets).
Ruled out: none yet.

Flight plan (one press each; the hangar trial is off for both):
1. Sealed hangar, seated: press the key once, let the four frames finish, then read the CSV and the "gpu pass profile:" lines.
   Expect the summary within a second or two of the press.
2. Same press on the raised pad, for comparison: the split should move with the scene, not with the stance.
A press during a capture is refused with a note. A capture arms at the next frame boundary, not mid-frame.

Build gates: tools\gpu_pass_profile_test --self-test (in build.bat). The DLL build compiles src\d3d11\gpu_pass_profile.cpp.
Still open: no flight yet; the overhead below is an estimate, not a measurement.

## What it does

A press arms a batch of four whole frames. At each frame boundary (vScreenFrameBoundary, once per Present) the frame in
progress is closed and the next one opened:

- Each frame has its own frequency scope, opened with gpu_timing.cpp's gpuTimingPassScopeOpen (a standalone scope on the
  shared disjoint clock). It refuses while any other scope is open, so the profiler never borrows a scope it did not open.
- A TIMESTAMP query is issued on the owner context at every pass boundary:
  - an OMSetRenderTargets or OMSetRtvAndUav call (the bound target set changes);
  - ClearRenderTargetView and ClearDepthStencilView;
  - a Dispatch (exposure_fix.cpp's hook);
  - the begin and end of each EDVR census section (gpu_census.cpp; the Altered sections time Elite's own draw and are excluded).
- The time between two consecutive stamps is one segment. It is attributed to the targets bound when it began, and carries the
  draws, dispatches and clears issued inside it. A segment with none of those carries its time into the next one.
- Draws are counted at the draw thunks (Draw, DrawIndexed, DrawInstanced, DrawIndexedInstanced, DrawAuto, and the indirect pair).
  Their shader pairs (bindingShaderHash VS and PS) are tallied per segment; the top three by draw count are written.
- At the next boundaries the scope's result is polled, then the stamps are read with DONOTFLUSH. Nothing blocks.

Reused: the shared DisjointClock and its frame/standalone scope rules (gpu_disjoint_clock.h, gpu_timing.cpp); the census's
section brackets (gpu_census.h, gpuCensusBegin/End); the binding shadow (bindingGet, bindingShaderHash, the per-slot
resolve of views); the draw thunks' owner-context checks; the config's log directory and Log::note. Added: scopeOpen() on
DisjointClock, the pass-scope functions in gpu_timing, the core header, and the module.

## Classification rules (gpu_pass_profile_core.h, classifyPass; first rule that holds wins)

Let E be the eye size (eyeW x eyeH, the same size vscreen publishes), and C be the first colour target.
1. Inside an EDVR census section: edvr.
2. Dispatches and no draws: compute.
3. No draws: clear if it cleared, else other.
4. Depth only (no colour) and square with width at least 512, or with more than one slice: shadow.
5. Depth only and the depth target is E: scene_gbuffer (a depth prepass at the eye size).
6. C is square and a cube resource: probe_cube.
7. Two or more colour targets, C at E: scene_gbuffer.
8. One colour target at E: fullscreen_at_eye with one or two draws, otherwise forward_at_eye.
9. C smaller than E in area, with draws: post_smaller.
10. Otherwise: other.
Then, over the whole frame, any run of six or more consecutive single-colour, square, drawn-into passes of one size is
relabelled probe_cube (a cube map drawn face by face).

UI is NOT a class: no reliable target signature identifies it. Elite's UI draws that EDVR redirects are counted in the census
(AlteredUiLayer) and fall into the classes above here.

## Output

- edvr_logs\gpu_pass_profile_<YYYYMMDD-HHMMSS>.csv, one row per segment per frame. Columns: frame (0-3), pass (index within the
  frame), class, edvr (1 or 0), ms, draws, dispatches, clears, tris (estimate: count/3 x instances for triangle draws; 0 for the
  indirect and auto draws), colours, rt (WxH of C, or -), rt_fmt (DXGI format), slices, cube, dsv (WxH or -), dsv_fmt,
  top_vs_ps_draws (vs:ps:draws, up to three, hex).
- "gpu pass profile:" lines: "means over N frames: frame X ms = elite Y ms + edvr Z ms (unattributed U ms)"; one line per
  class with its ms and passes per frame; the top 15 groups by ms per frame (class, target signature, draws, passes); and an
  overflow line when a frame hit its stamp cap.
- Reading: elite plus edvr equals the frame by construction, so unattributed should be near zero. A nonzero unattributed value
  is time in a boundary-less stretch (CopyResource and the like are not boundaries; their time is in the segment they fall in).
- A CSV summariser is not written; the CSV is plain text and the numbers above are enough for the first read.

What the log shows when the capture does not run, so that "nothing" is not mistaken for "measured zero":
- No line after the press: the request was never taken at a boundary (check the hotkey line at start-up: "hotkey: GPU pass
  profile key bound"). A press during a capture says "refused".
- "this context does not own the GPU timing domain": the timing domain is not bound to this context.
- "frame N not captured: the shared GPU timing scope was already open": a census timer or the application-frame timing held
  the one scope. The batch stops there and the frames read so far are written.
- "frame N not captured: ..." with the disjoint reason (disjoint clock change, stale, driver failure): said with the scope's
  own reason. Disjoint frames are not zero; they are not read.
- "not ready after 240 boundaries": the GPU never reported the scope (about 2.7 s at 90 Hz).

Overhead (estimate, not measured): unarmed, one bool read per hook. Armed, each boundary costs one TIMESTAMP query insertion
on the owner context plus a bookkeeping step; a frame with N boundaries costs N insertions. Each census section and each
Draw adds its own bookkeeping, and the shape cache calls GetDesc and GetResource once per distinct view per capture. The
timestamp pool is 4 x 4096 queries, created on the first press (one hitch). Readback is one GetData per stamp, on a later
boundary. A measured figure needs the flight: compare the frame time in the batch with and without the press.

Risks:
- The app-frame GPU timing (gpu_frame_timing) yields its scope while a capture runs; its samples for those frames are dropped.
- Census timers borrow the profiler's scope while it is open, so their figures are not changed by it, but census lease budget
  (128 per record) is shared.
- A view address recycled within a capture would read a stale shape. Captures are four frames, so the risk is small; it is
  not checked.
- Boundaries merge the census's EDVR sections, so a Frame section that runs per draw adds a boundary per draw. The stamp cap
  (4096 per frame) is counted and said, not hidden.

## Interaction (v13.5, merge with the sealed-hangar trial)

The instruments share one DisjointClock (gpu_timing.cpp). A clock admits one open scope or frame lease at a time: a standalone scope (the profiler's per-frame scope, gpuTimingPassScopeOpen) is refused while any scope or frame lease is open (DisjointClock::scopeOpen), and a frame lease (GpuTimingFrameDriver::begin, the application-frame timing and the census spans) is refused while a standalone scope is open (startFrame).

- The sealed-hangar trial and the drawlist probe take no GPU timing. They count and read game memory with QPC and counters only, so they cannot contend with the profiler.
- The census (gpu_census.cpp, GpuIntervals over GpuTimer) and the application-frame timing (advanced.app_gpu_timing, default on in vscreen.cpp) do take the clock. A profiler frame that opens while either holds it fails with "the shared GPU timing scope was already open", and the frame is logged as failed. The profiler never corrupts or stalls them: its failure is local to its own frame.
- Conversely, a census or application-frame lease that opens during a profiled frame is refused for that interval.
- The v13.5 expectation (from the code): the profiler's boundary runs in the frame tick (device_hook.cpp, the vScreen rest), while the application-frame segment closes in hookedPresent after the real Present (gpuFramePresent), so a segment is open at the boundary and the profiled frame failed with that reason. v13.6 fixes this; see "The yield (v13.6)" below.

## The yield (v13.6)

While a capture is armed or running (from the press until the last profiled frame's scope is closed), the application-frame timing and the timers yield the shared clock:

- The press sets the yield flag (gpuTimingSetYield, gpu_timing.cpp). The flag is read by every new lease the frame driver and the timers would take: GpuTimingFrameDriver::begin, GpuTimer::beginImpl (standalone and borrowed). A refusal is counted and returns the same not-taken result a busy clock gives. The profiler's own scopes are not gated.
- gpuFrameCommand (gpu_frame_timing.cpp) returns before it opens an application frame while the flag is set. The frame is counted once (by its sequence). The rest of that frame's commands find no open frame, so its SubmitBegin records NoOpenFrame, as a frame that never began would.
- At the profiler's boundary, while the capture is armed or running, gpuFrameYieldBegin ends any application frame open across the boundary: an invalidate, which closes the open span through the ring (closeApplication, driver end, DisjointClock::finishFrame, which clears the open scope). Then the profiler's gpuTimingPassScopeOpen finds the clock free (scopeOpen() false) and opens its frame.
- The yield ends at the batch's end: the boundary after the last frame's close clears the flag (endYield) and logs once: "app gpu timing: paused for the GPU pass profile, N frames; census timer leases refused: M."
- Shutdown clears the flag.

Why this holds: an application frame opened before the press is closed at the first boundary by gpuFrameYieldBegin; every later frame is refused at its open, so no lease of theirs can be open when a profiled frame opens. The census's timers (GpuIntervals over GpuTimer) are refused the same way: a timer lease is only ever open inside a census section, and no boundary runs inside a section, so the census does not hold the clock across a boundary. If a census lease were open at a boundary, the profiled frame would fail with the existing reason and the flight would show it.

Verification: the clock behaviour is in tools\gpu_disjoint_clock_test (192 checks, including the sequence: frame open, scopeOpen holds and a second frame is refused; finishFrame releases; a standalone scope then opens, closes and releases). The capture's state is in tools\gpu_pass_profile_test (54 checks, including the yield's life across the press, the batch and the last close). The D3D path (the real query pool, the span ring and the hooked Present) is not modelled by any rig; its reasoning is above, and the flight is the check that a profiled frame opens with app_gpu_timing on (the "frame ... not captured" line should be absent, and the "app gpu timing: paused" line should count the frames).
