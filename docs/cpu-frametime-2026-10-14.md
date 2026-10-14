# Hangar frame time is CPU-bound: where the CPU goes, per frame

## Status

*Opened 2026-10-14. Phase 1 (the instrument) is built; no engine patch is written. Update this block when the doc changes.*

- **State:** the station hangar frame on Sean's rig (SteamVR OpenXR / Pimax, 3840x3840 panels, eye 2016x2016, DLSS out 4032x4032) is CPU-bound. The 2026-10-14 GPU pass profile (build 7116598f) read 14.3 ms a frame with the GPU busy about 10 ms; about 5.4 ms is gaps between passes even with a 6 ms delay dispatched at frame start, so the GPU waits for submission.
- **Prior evidence, none of it from this build:** (a) 2026-10-11 sampler, hangar, Pimax Crystal Super: the render thread sat 44% in Elite's frame handshake (function 0x5d6d20, a condition-variable wait with a 10 ms timeout), waiting on another thread; EDVR was 6.8% of render-thread samples (about 0.75 ms a frame). (b) 2026-09-22 ETW, parked cockpit near a settlement: the render thread ran 15.1 ms a frame, 3.94 ms of it innermost in EDVR's d3d11.dll (draw submission through the proxy), 4.45 ms in the game exe. Different scenes, different builds, so (a) and (b) disagree about EDVR; this capture settles it per scene.
- **Changed since then:** a pose cache removed the job thread's owner-thread hops; the light grid cache saves about 0.6 ms a frame of render-thread CPU (cluster builder 0x58B150); Optimized stations culls the sealed hangar. Nothing has measured the hangar CPU since.
- **The instrument:** the multi-thread CPU sampler was NOT on main; it is ported from claude/render-cpu-test (9033ff1b: sampler v3, the owner thread, wait lead, owner-invoke) and extended with per-frame accounting. One press of `hotkey.profile_cpu` (or `diag.profile_cpu = on` once, 20 s after the first in-scene frame) samples ten seconds. `python tools\edvr_log.py --target <t> --cpu-profile` prints the ranked table.
- **Open hypotheses**, each with the line that decides it (all in the `cpu profile:` block; render = the thread that presents, producer = the busiest thread besides render and owner, inferred, not proven):
  - **H-producer, the job thread is the critical path.** Confirmed by: `producer ... busy_pct_of_frame` at or above 85, `thread=render wait_split handshake=` at or above 3 ms, and `critical ... producer=` the largest share of frames with a render sample. Refuted by: handshake wait high while the producer's busy_pct is under 60 (it is not working; see H-sync), or handshake wait under 1 ms.
  - **H-render, render-thread submission is the path.** Confirmed by: `thread=render per_frame busy_ms` within 1 ms of `frame_ms`, `critical ... render=` the largest share, the waits small. `busy_split` says where: `d3d_driver` is driver submission, `elite` the game's own draw code, `edvr_*` ours; `elite_fn` ranks the game functions. Refuted by: render waits at or above 25% of the frame.
  - **H-edvr, EDVR hooks sit on the critical path.** Confirmed by: render `edvr_stack_ms` at or above 2 ms with `critical render=` leading, and `edvr_fn` ranking our draw hook or the Present boundary. Not confirmed by `edvr_d3d11_ms` alone (that is time innermost in our code; `edvr_stack_ms` also counts the driver and game calls we sit above). Refuted by: `edvr_stack_ms` under 0.5 ms.
  - **H-sync, a lock or the handshake stalls with nobody working.** Confirmed by: render `lock=`, `event=` or `sleep=` of 1 ms or more, or handshake wait high with the producer under 60% busy; frames with a render wait of about 10 ms (the handshake timeout) in the `window` lines (`frame_ms` spikes with `crit=` sync or producer). Refuted by: those waits under 0.3 ms.
  - Also read: `critical ... xr=` (the runtime paces; not a CPU limit), `gpu=` (the render thread waits on the driver or fence: the GPU is the limit after all).
- **Ruled out:** nothing yet from this instrument. Carried over (verified in the 2026-10-10 doc): GPU-bound in the heavy windows 4-6 and 8-9; the owner rendezvous as a cost; periodic render-thread jobs as the cause of long cycles.
- **Next flight:** four legs, one press each, same session, same headset setting: (1) hangar sealed, Optimized stations on; (2) the same hangar, `fix.optimized_stations` off (restart the game or toggle live if the menu allows); (3) supercruise; (4) a busy station concourse on foot. Details below.
- **Developer keys this adds to the arc (all temporary, listed here per AGENTS.md):** `hotkey.profile_cpu`, `diag.profile_cpu` (unchanged names, extended output). When the arc closes, propose removing both, quoted, and ship the winner as behaviour.

## Flight plan

Before flying: `python tools\install_edvr.py --verify-only` and note the build. Set `profile_cpu` in the `[hotkey]` section of the live edvr.ini to a bare key Elite does not bind (it is empty by default, so nothing runs); Sean's edit, not the tool's. Each leg: settle for 20 s in the scene, do not move the head or open a menu, press the key once, stay for 12 s (ten seconds of samples, one second of thread choice first). One capture per leg.

| Leg | Scene | Why |
|---|---|---|
| 1 | Hangar, door sealed, Optimized stations on | the case from the GPU pass profile; the baseline |
| 2 | Same hangar, Optimized stations off | what the cull is worth in CPU; separates H-render from H-producer if the producer's load moves with draw count |
| 3 | Supercruise | no station, no hangar: if the producer is busy here too, it is not the hangar's draws |
| 4 | Station concourse on foot, many NPCs | the other heavy case; the producer should differ (animation, crowds) |

After each leg: `python tools\edvr_log.py --target steam --expect-build HEAD --version` (exit 2 means a stale DLL, discard the log), then `python tools\edvr_log.py --target steam --cpu-profile`. The CSVs are `edvr_logs\cpu_profile_<stamp>.csv` (raw samples) and `edvr_logs\cpu_frames_<stamp>.csv` (one row per frame). Name a game function with `python tools\elite_hotspots.py --samples <csv> --thread <producer> --disasm 0xRVA`.

## What one capture prints

All lines start `cpu profile:` in the graphics log. Per thread (render, main, owner, up to four busiest others), after the sampler's own `fn`/`class` lines:

- `thread=T per_frame frames=N frame_ms=F busy_ms= wait_ms= cpu_ms= edvr_d3d11_ms= edvr_openvr_ms= edvr_stack_ms=`
- `thread=T wait_split frame_ms=F busy= handshake= xr= gpu= lock= event= sleep= samples=S` (ms per frame; they sum to `frame_ms`)
- `thread=T busy_split elite= d3d_driver= edvr_d3d11= edvr_openvr= vendor= system= other=` (where the running time sits, ms per frame)
- `thread=T edvr_fn R incl X excl Y ms_per_frame module+begin..end` (top 10 EDVR functions among running samples)
- `thread=T elite_fn R incl X excl Y ms_per_frame EliteDangerous64.exe+begin..end` (top 20 game functions among running samples; render and the producer only)

Then once: `producer thread=T start=... busy_ms= frame_ms= busy_pct_of_frame= cpu_ms=`, `critical frames=N producer= render= xr= gpu= sync= unknown=`, one `window frames=a-b n=30 frame_ms= render_busy= wait_handshake= wait_xr= wait_gpu= wait_sync= producer_busy= owner_busy= render_edvr= render_n= producer_n= crit=...` per 30 frames, and `frames_csv=<name> rows=N`.

How it decides. A sample is *waiting* when its leaf is in an OS wait stub (a named ntdll/kernelbase/win32u export that waits: found by the export table, no symbols); anything else is *running*, including a thread spinning in the driver. A waiting sample is named by its walked stack (eight frames), strongest first: Elite's handshake function (only on build 332841, PE stamp 1788384820, size 104894464), then EDVR's OpenXR runtime or another VR runtime (xr), then the graphics driver, D3D/DXGI or an upscaler (gpu), then the stub's kind (lock = park, event, sleep). A frame is the interval between two of the render thread's Presents. Its *critical path* is read from the render thread's samples in that interval: under 25% waiting, the render thread itself; otherwise the longest wait names it (handshake means the producer). This is a statistical reading with three to four render samples per frame, so trust the counts over the 30-frame windows and the whole capture, not one frame.

Limits, stated once. The producer is inferred, not proven: it is the busiest non-render, non-owner thread by share of running samples. The walk is eight frames deep, so a wait whose runtime or driver frame is deeper falls through to lock/event/sleep. A thread spinning in the driver counts as running. EDVR functions are named by module and `.pdata` range only; match them to source with the build's PDB.

Cost. Idle: one relaxed load in the Present hook and one branch in the frame tick; no thread, no allocation (32 KB of static zero pages for the Present stamps, untouched until a capture). Armed: a worker thread stops each sampled thread for tens of microseconds at about 1200 samples a second combined (the render thread and the likeliest producer twice a round), under a 5% stopped-share cap; the render thread loses about 1 to 3% of a frame for ten seconds, and the log gets about 400 lines.

A cross-check exists: `tools\cpu_profile.py` (WPR/ETW, needs an administrator token) gives the same threads by kernel events. Use it only if this instrument and the headset disagree.

## Candidate engine patch targets (static analysis only, UNMEASURED; phase 2 waits for the flight)

None of these is a recommendation. Each saving is an upper bound read off prior logs and holds only if the capture names the matching hypothesis.

| Candidate | Where | Expected saving (unmeasured) | Needs |
|---|---|---|---|
| Handshake wake: the wait has a 10 ms timeout; 2026-09-22 saw 66 DPC-ended 10 ms waits beside thousands of short ones | EliteDangerous64.exe+0x5d6d20 (and 0x5d6d7f) | up to the timeout-bound share of `handshake=`; zero if the producer is simply late | H-producer or H-sync; the `window` frames with ~10 ms waits |
| Draw submission through the proxy: per-draw chain 0x51bcab < 0x4c82f18 < 0x594ed5 < 0x58f2f4 | EDVR d3d11.dll hooks | the 2026-09-22 cockpit read 3.94 ms innermost; the hangar is unread | H-edvr: `edvr_stack_ms` and `edvr_fn` |
| Post-Present drain on the render thread: record drain, draw-item chain 0x4c81ca0 < 0x4c8227c | game code after Present | 2026-09-22: 3.4 ms running in R5c, 77% game code | H-render with `elite_fn` showing these RVAs |
| More of the cluster builder (0x58B150 chunks): the cache already covers it | light grid cache | about 0.6 ms a frame already taken | none; re-read `elite_fn` for new leaders after the cache |
