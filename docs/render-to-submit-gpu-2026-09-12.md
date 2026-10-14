# Render-to-submit GPU checkpoint

## Status

- 2026-10-14, claude/overlay-gpu-busy: the overlay's primary GPU figure is GPU BUSY from the OS
  (D3DKMTQueryStatistics, per-process node running time), with the frame GPU work span as a labelled
  second figure ("span"). BUILT, NOT FLOWN. The last section of this doc says how it works. The span
  is an elapsed figure that includes GPU idle, so it is not comparable to fpsVR; busy is.
- Open: the next flight compares the `gpu busy (os):` log line with the GPU pass profiler's "frame
  busy" with the delay (about 5.0 ms in the hangar on 2026-10-14). They should agree within about a
  millisecond. If busy reads far above the pass profiler's figure, suspect the node choice (a copy
  or compute node counted) or the 100 ns unit.
- No temporary keys: the sampler has no setting; it is on in the VR profile.
- The sections below are history (the OpenVR proxy era) up to the 2026-10-14 pair at the end.

This follows the successful `cc3d882` Frontier timer-migration check. It adds
the local frame measurement to the current OpenVR proxy. It does not add an
OpenXR renderer or complete Phase 0 qualification.

The `0ac3095` headset gate failed: renewed shimmering was reported during head
movement. Frontier was restored to the exact `cc3d882` DLL pair. The cause
remains unresolved; the timing implementation is not qualified for promotion.
See the regression record below.

The rollback also shimmers and blurs during head movement. Both rollback logs
and installed DLLs match `cc3d882`, and the INI still matches the earlier run.
Ruled out: the new outer frame instrument as a necessary cause, because the
symptom persists without that implementation. The report affects scene geometry
and text, and switching AA off makes the moving image clearer.

The subsequent paired captures established a separate projection mismatch under
the active raw-only culling probe. Temporal reconstruction was using the
widened raw frustum while the game rendered through its original matrix. See
the [projection diagnosis and
correction](temporal-projection-probe-2026-09-12.md).

## What the value means

The start marker precedes the first covered call on the bound immediate context
after a successful owned pose wait. The end marker follows the second distinct
accepted eye's entire submit path, including the runtime Submit call and EDVR's
post-submit shadow copy. Theater, healing and shadow-resubmit paths share the
same wrapper. Classic withholding, rejected submits, malformed pairs, missing
eyes and new boundaries cannot produce a valid completed frame.

The two inner intervals are **submit paths including runtime work**. They are
not EDVR-only GPU cost. Existing EDVR pass/door readings remain unchanged;
complete accounting of EDVR work across every early path is still separate
work. Subtracting those existing readings from this span is not a game-only
cost measurement.

The span measures elapsed GPU-stream time, including bubbles and contention. It
excludes commands after the final marker and commands issued before the pose
boundary. The preceding Frontier trace contains both kinds of work. It must not
be presented as the whole GPU frame or expected to equal SteamVR's scene/total
timing definitions.

## Placement and ownership

Covered base-context paths are direct/instanced/indirect draws and DrawAuto,
dispatch/indirect dispatch, RTV/DSV/UAV clears, copies and structure counts,
updates, resolves, mip generation, ExecuteCommandList, Map/Unmap and query
Begin/End. ClearState also consumes an armed boundary. EDVR's frame-query
commands bypass re-entry and the draw census. State-only setters are not a
universal start boundary. Extended-context upload/copy/clear variants and calls
that bypass the hooks are not proven coverage; the label deliberately says
render-to-submit from the first **covered** command.

The four new base-context slots are checked against the installed Windows SDK's
C vtable. They preserve the existing LiveCopy table contract and the separate
exposure hooks. The draft incorrectly assigned DrawAuto to slot 16; review
corrected it to slot 38 before execution. The paired test also verifies that
slot 16 still forwards PS constant-buffer bindings.

A version-1 paired export carries CPU pose/submit events. It changes no shared
memory layout. The graphics receiver allocates monotonic sequences across
compositor rebinds. Pose publication is atomic and issues no GPU commands; the
actual context pointer, device and calling OS thread gate the query ring. The
bridge resolves the exact executable-side graphics DLL and holds a module
reference for each callback, without loading a runtime or retaining a dangling
function pointer.

Eight frame slots hold six timestamp queries each. A frame owns one shared
frequency scope; existing pass timers borrow it. A live standalone interval
causes the frame measurement to skip rather than open an overlapping disjoint
scope. Queries are polled without flushing or waiting, retain partial ready
results, and expire after two seconds. An uncertain End permanently stops the
instrument until owner teardown. Query/COM cleanup is explicit; process exit
does not run it from destructors.

Source frames use the existing Present counter, captured when rendering starts,
not a second independently incremented frame counter. Results retain that
identity and their pose sequence even when they complete later. The Monitor
uses a locked CPU snapshot, rejects stale/invalid values, and never replaces a
newer sample with an older one. A late duplicate or foreign-thread event can
invalidate an already closed pair; affected older pending samples are discarded
conservatively rather than risk publishing a rejected frame as valid.

## Monitor and configuration

`advanced.app_gpu_timing = on` enables the local queries by default in this
checkpoint. It is live and independent of `advanced.compositor_timing` and
`advanced.openvr_census`. Turning it off closes/discards the local ring on its
verified owner; enabling it waits for a new pose boundary. If every hook-using
feature was disabled at startup and its hook was never installed, enabling this
feature requires a restart. Turning queries off retains the small
boundary-bridge and hook checks; an enabled/disabled comparison does not
measure that fixed cost.

The Monitor adds a render-to-submit line below the existing 16 tiles, including
D3D11 source, original frame and result age. Existing SteamVR GPU total, scene,
compositor, drops/reprojection, graphs and attribution are unchanged. Only the
small FPS overlay may use a fresh local fallback when compositor GPU timing is
absent, explicitly labeled `submit gpu`. Invalid/pending/stale results are not
zero-millisecond measurements.

## Validation

The focused suite passed 599 checks, including command-derived nested frame and
existing-pass timestamps, both eye orders, preserved source frames, partial
readiness, all seven allocation positions with failure/null-success variants,
disjoint/zero-frequency/unexpected GetData results, uncertain closure, ring
pressure, standalone-scope rejection, wrong thread/context/device, missing and
duplicate eyes, rejected submit, out-of-order pose waits, and configuration
disable/re-enable, immediate readout invalidation after failed poses, and
CPU-only frame publication from a foreign Present thread. Actual WARP
clear/copy workloads also verify every output pixel outside the measured
interval. Counts include repeated resource checks, not 599 distinct scenarios.

The paired build passed in `build/render-submit-validation-3.log`, including
the existing UI, motion, shader-bytecode, native adapter, shared-clock,
LiveCopy, OpenVR ABI, config and Python gates. Five isolated actual-proxy modes
pass: ready/delayed/disabled census and local GPU timing on/off. The local
modes have census and compositor timing disabled, use the two shipping proxies
with a fake historical compositor and real WARP, verify PS constant-buffer
bindings, and check every copied pixel. The first valid result in the separate
paired run was sequence 1 / source frame 1, 0.0113 ms outer and 0.0004 ms per
submit path, on context `000001DAF9C1F9A8`, thread `15324`. These software
timings are not performance targets.

The independent-feature test initially found that the compositor install gate
omitted local timing. The gate now includes it; the graphics hook install gate
also includes it. Neither census nor another rendering fix is required to start
the instrument. The final foreign-Present/failed-pose changes additionally
passed the 599-check focused suite. The final full paired build passed in
`build/render-submit-final-validation.log`, including all five actual-proxy
modes and the 599-check suite.

The headset run below did not qualify this span. WARP results establish the
tested command ordering and resource correctness, not hardware timing accuracy,
overhead or visual equivalence with production TAA/DLSS. The paired frame test
had temporal rendering disabled; the separate temporal smoke did not drive
OpenVR pose boundaries. Neither exercised temporal rendering with the outer
frame scope active.

## Frontier gate

Use Pimax through SteamVR on the Frontier install. Preserve the tuned INI.
Check prompt intro, cockpit and on-foot rendering/tracking, F8 Monitor close
and reopen, and normal exit. The new Monitor line should settle to a measured
value or explicitly explain unavailability. Compare both installed build
identities in the new logs before interpreting any readings.

Inspect owner/sequence/frame/age and valid/invalid summaries, active LiveCopy
hooks and existing pass/feature counters. This first run is a functional gate.
Matched-frame SteamVR correlation and a controlled enabled/disabled overhead
comparison remain required before treating the local value as qualified. Other
runtime/headset configurations and complete lifecycle/command coverage remain
separate Phase 0 gates.


## Installed checkpoint

Commit `0ac3095` (`v0.15.1-54-g0ac3095`) was rebuilt from a clean working tree.
`build/frontier-render-submit-clean-build.log` passed the full suite, including
599 shared/frame timing checks and all five actual-proxy modes. The isolated
production WARP smoke passed in `build/render-submit-clean-smoke.log`,
including black void, temporal history/cuts, supersample and sharpening checks.
Its log confirms the clean version, LiveCopy and a completed existing
shared-clock pass sample. NGX/foveation runtime checks remain unavailable on
WARP.

Both DLLs were installed and independently verified in Frontier at 08:22 local
on 2026-09-12. Steam was not changed and the game was not launched.

| Payload | Linked stamp | SHA-256 |
| --- | --- | --- |
| `d3d11.dll` | `6AA55F6D` | `168425BDAEE2DA1EF174A1069E3D539E100F5CD685F8072FC010E423ED969FAA` |
| `openvr_api.dll` | `6AA55F75` | `1DBDCB030D962E5E312393138DD54714BCCADAA0BB89897B54A9D19ED97A4D4B` |

The tuned root INI remained byte-identical, SHA-256
`A32D631834E5C1EEEFAA03367750A7A4211652869EB4976900DC11CF530EC1A9`. There was
no separate Openvr/win64 INI before or after installation. Previous DLLs were
retained with suffix `.pre-0ac3095-20260912-082225.bak`. The later
installation-record commit does not change that DLL version. The following
flight used `0ac3095`; the rollback below supersedes this installation.

## Head-movement regression and rollback

Renewed shimmering was reported during head movement after this flight. This is
a failed visual gate, even though the instrument produced valid samples. Do not
describe it as a proven motion-vector defect or a proven query side effect yet.

Both graphics and VR logs were independently verified against `0ac3095` with
the sanctioned log tool. The installer's verification mode also confirmed both
installed DLLs matched the checkpoint. Detailed flight evidence and the
rollback receipt are retained locally.

Observed evidence:

- LiveCopy is active. Periodic timing summaries report valid samples and no
  invalid samples on the expected owner. This refutes owner rejection or an
  inactive instrument as the explanation for this run; it does not establish
  visual correctness.
- DLSS, scene depth, camera rows and motion capture are active. Native TAA
  statistics cannot validate NVIDIA's history or the submitted motion vectors.
- Ruled out: a newly appearing projection-order warning, because both this
  verified run and the earlier normal `cc3d882` run contain it. The warning is
  an aggregate count, not proof of which projection each rendered eye consumed.
- Game CreateShaderResourceView failures are also present, with subsequent
  rendering observed. Their relationship to the reported shimmer is unknown;
  these are not evidence of failed timing queries.

Read-only review found no branch where timing lease failure or pending results
skip temporal rendering or motion capture. In `temporal_pass.cpp`, timing and
statistics completion control diagnostic-slot retirement; in
`celestial_motion.cpp`, the sampled timer result controls only its matching
End. The new context hooks forward the existing commands and the four added
slots match the SDK. This rules out those direct code couplings, not an
indirect scheduling or driver interaction.

The rollback did not restore visual quality. The next discrimination uses the
existing paired eye capture on that baseline: one still-head sample and one
sample during a slow head turn, in the same stationary cockpit scene with DLSS
enabled. Paired capture is already the default; no new build or configuration
edit is needed. The Instruments action "Dump both eyes as seen" collects the
raw and treated sequences before the later EDVR menu overlay, plus motion
metadata and first-frame shader inputs.

Check history flags and reset reasons, camera continuity, depth coverage and
the actual motion texture against displacement measured from raw frames. The
first-frame motion texture must be aligned to its capture frame; later frames
have metadata, not a separate captured motion texture. Compare input/output
crop coordinates at their recorded resolutions. These observations distinguish
history resets, incorrect reprojection and reconstruction blur; aggregate
projection counts cannot do so. No rendering-code fix has been made on the
current evidence.

The existing hardware smoke also passed using the restored graphics DLL and the
same DLSS runtime in an isolated fixture. NVIDIA's history accumulated across
evaluations, the motion probe ran successfully, and the shipped motion/jitter
pairing had the lowest error among the tested conventions. Those synthetic
results do not establish which projection or pose the game actually consumed.
They do not rule out game-driven resets or mismatched camera/depth inputs
during the reported head movement.

The exact retained baseline backups were hash-checked, staged separately from
the current build outputs, installed through `tools/install_edvr.py`, and
verified again. Current INI bytes were preserved. Use `--expect-build cc3d882`
for the next flight and the staged rollback root when verifying the current
installation. The subsequent headset comparison confirmed that the rollback did
not recover visual quality.

The capture investigation subsequently established a raw-only culling-probe
projection mismatch; see the [evidence and
correction](temporal-projection-probe-2026-09-12.md). The corrected `f622cd2`
pair, including the current timing work, has replaced the rollback in Frontier.
Both DLL hashes match the clean build and the current INI bytes are preserved.
Use `--expect-build f622cd2` for the next flight. Visual recovery remains the
gate before further OpenXR implementation.


## 2026-10-14: the overlay's GPU time with the application clock off
## 2026-10-14: the overlay's GPU time with the application clock off

With `advanced.app_gpu_timing` off (its default since the AA-off cuts), the
overlay's GPU TIME tile read "--" permanently: its only source was the
application-render clock.

**Superseded in part, same day.** The first version timed one pair from frame
boundary to frame boundary. On a 90 Hz GPU with slack that reads the frame
period (about 11.1 ms) every frame, because it includes the idle wait for the
next frame. The work span below replaces it.

**What it measures.** The frame GPU work span: one timestamp pair a frame, from
the frame's first GPU command on the owner context to the end of EDVR's door
work, just before the real Present. Label: "frame GPU work span (first command
to last; includes GPU idle inside the frame)". The span includes GPU idle inside
the frame, not the idle between frames.

**Begin and end.**
- Arm: `gpuCensusFrame` (gpu_census.cpp) arms the span after it opens the frame
  scope, at the frame boundary in `presentFrameBoundary` (device_hook.cpp), which
  runs after the real Present.
- Begin: the first command through `gpuFrameCommandIfLive` (gpu_frame_timing.h),
  which every command thunk already calls. One relaxed load of the arm; the
  first command on the owner context begins the pair and disarms.
- End: `hookedPresent` (device_hook.cpp), just before `realPresent`, after EDVR's
  door work, via `gpuCensusFrameWorkEnd`.
- Ordering: the scope opens after one real Present, the pair begins at the next
  frame's first command, ends before the next real Present, and the next
  boundary (after that Present) closes the scope. So the pair lies in one scope.
  A fallback end at the census's close (`frameSpanEnd`) covers a pair the door
  end did not close.
- Not covered: the runtime's own treatment on its transfer thread (another
  context, not timed here).

**Overlay and history.** The GPU TIME tile shows the application-render mean when
that clock is on. Off, it shows the last completed work span, and the series
feeds `NativePerfHistory::noteFrameWork`, so the tile's window mean is the work
span's. "--" appears only before the first pair has completed. The local GPU
line reads "Frame GPU work span: N ms (...)". The native benchmark is not fed:
it joins GPU observations to CPU frames by native frame sequence, which a census
span does not carry.

**Cost.** Two timestamp queries a frame, plus one relaxed load in the command
gate, and a poll of pending pairs on the census's boundary. Leases: at most six
pairs in flight, two timestamps each, inside `kLeases` = 512 (static check in
aa_off_test). No standalone disjoint query: the pair is refused rather than
opening a scope of its own.

**Tests.** `aa_off_test`: the tile shows a number with the application clock off
and a completed span, and "--" only with no sample; the label; a short buffer; the
arm gate; the lease check; and a simulated 90 Hz slack frame in which the work
span is less than the frame period (the arithmetic only).
Not flown.

## 2026-10-14: the overlay's primary GPU figure is GPU busy, from the OS

**Why.** The work span above is first command to door end, so it includes every wait for the CPU
to submit. The 2026-10-14 pass profiler delay run (log 162220, Pimax Crystal Super, hangar) put
one frame at 11.6 ms with the waits and 5.0 ms busy with a 6 ms front-loaded delay. The overlay
read about 10 ms on a frame that kept the GPU busy about half the time. fpsVR's older comparison
(dlss-performance-review-2026-09-28.md, "Query median 12.761 ms includes submission gaps, not GPU
busy time") was the same effect. OpenXR has no GPU timing and D3D11 cannot ask the GPU for busy
time, so the figure comes from the kernel's books.

**What it reads.** `D3DKMTQueryStatistics` (gdi32.dll, loaded by name so a system without it
degrades to "--"): the adapter's node count, then per node the process's cumulative `RunningTime`.
The unit is 100 ns (measured here on an RTX 5090: 20,548,170 units for about 2.0 s of known GPU
work). Engine types and names come from `D3DKMTQueryAdapterInfo(KMTQAITYPE_NODEMETADATA)`. The
RTX 5090 reports 17 nodes: node 0 is the 3D engine (graphics and compute), 3/4/12-15 copy, 1/2
and 6-8 video, 5/9/10/11/16 other (Security, OFA, JPEG, VR).

**The figure.** For each reading pair: utilisation of a node = running time over wall time;
busy ms per frame = the busiest counted node's running time over the window / frames in the window
(= utilisation x the window's mean frame time). Counted nodes are the 3D engines only. The
maximum is used, not the sum: engines run in parallel, so a sum counts the same wall time twice,
and the maximum over engines is what Task Manager shows as a process's GPU. Copy, video and the
vendor engines are logged, not counted. Windows under 250 ms, with no frame, with a counter that
went backwards, or with a node over 150% are dropped; 100-150% is held at 100%.

**Where it runs.** `src\common\gpu_busy_os.h` holds the maths, the board and the text (no
Windows calls). `src\d3d11\gpu_busy_sampler.cpp` holds the kernel calls and a detached thread
that samples once a second; `gpuBusyFrame` (device_hook.cpp, beside `vramWatchTick`) costs one
relaxed add per frame and, once, reads the device's adapter LUID. The kernel is never asked on
the render thread. VR profile only. Cost: about 17 syscalls a second.

**What it cannot see.** This process's work on the game's adapter. The runtime's compositor work
(a different process) is not in it, and neither is a second adapter's. Where
`D3DKMTQueryStatistics` is absent (Wine, DXVK) the overlay shows "--". Pimax OpenXR is not
SteamVR, so fpsVR's per-app GPU time from the compositor is not available on that runtime;
this figure is the comparable one.

**Overlay and tile.** `chooseGpuFigures` (frame_span_tile.h) builds both figures once for the
fps line and the Monitor tile:

    90 fps   gpu busy 5.4 ms   span 9.7 ms   cpu 3.6 ms

With `advanced.app_gpu_timing` on, the second figure is the application-render mean, labelled
"app render" (also while it has no sample; the span is not read then). The Monitor page's GPU
TIME tile became GPU BUSY: the value is busy, the sub-line holds the labelled second figure
("span 9.7 ms (elapsed)"). "--" appears for busy only when the OS has no figure (the sub-line
says why).

**Log.** One start-up line and one line every 30 s from the sampler thread:

    gpu busy (os): adapter "NVIDIA GeForce RTX 5090" luid=00000000:00013628, nodes=0:3D*,... (* = counted: ...), sampled every 1000 ms, source=d3dkmt
    gpu busy (os): 5.412 ms/frame (48.7% of the period), span 9.703 ms, source=d3dkmt nodes=0:3D=48.7%*,4:Copy=0.4%(not counted)
    gpu busy (os): unavailable (<reason>); the overlay shows "--" for GPU busy, source=d3dkmt

"Of the period" is the utilisation of the busiest counted node (busy / the window's mean frame
time). The span is the mean of the census's work spans over the same 30 s.

**To validate in the next flight.** In a hangar at 90 Hz, compare the 30 s `gpu busy (os)` line
with the GPU pass profiler's "frame busy" under the delay (about 5.0 ms on 2026-10-14; the delay
removes the submission waits the span includes). Agreement within about a millisecond confirms
the node choice and the unit; busy should sit well under the span (the span read about 10 ms
that day). Also check the percentage against Task Manager's 3D engine for the game process.

**Tests.** `aa_off_test` (Cut E): the maths (utilisation, ms per frame, the 100 ns unit, max not
sum, short windows, no frames, counter reset, clamp, garbage, no counted node, node count
change, bad calls), the board (age, failure, saturation), the exact log text, and the tile and
fps line choices including the failure paths. The kernel path was checked by a throwaway
harness (a compute load of known length: 4 dispatches of 1.175 ms read 4.80 ms a frame) and
is otherwise not exercised by the rig.
