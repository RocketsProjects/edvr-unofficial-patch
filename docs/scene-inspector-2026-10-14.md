# Scene inspector (2026-10-14)

## Status

- **State (2026-10-14):** BUILT, NOT FLOWN, on `claude/scene-inspector` (cut from origin/main 498e4b16, not merged). Sean's ask: a developer
  tool that, pointed at something not behaving under DLSS, dumps what is needed to fix it in one pass. First users: the five DLSS-blurred
  elements of `docs\dlss-blur-elements-2026-10-14.md` (pad hologram, gantry railing, hologram adverts, the mail-slot force field, weapons fire).
- **Key:** `hotkey.scene_inspect` (`[hotkey]`, unbound by default, a developer tool: kept, listed in edvr.ini's comments, NOT in the F8 menu).
  Read the result with `python tools\edvr_log.py --target steam --inspect --expect-build HEAD` (add `--full` for whole verdicts).
- **One press captures ONE frame of the left eye.** Where the box is: the pixel where the HEAD's forward ray lands in that eye's image
  (from the runtime's frustum and eye-to-head rotation, so asymmetric frusta and canted displays are right), 16x16 render pixels. No gaze source
  exists in this build, so it is head-forward; the box line says so and says how far that is from the image centre.
- **It writes:** log lines `scene inspect ...` (box, frame counts, per-target box, the stack visible at the centre pixel, then per draw a `draw`,
  `route` and `verdict` line, `occluded` lines for opaque draws a later opaque draw hid, `pixel centre`, `box stats`, `dlss`, `files`, `complete`);
  `edvr_logs\scene_inspect_<stamp>_capN.csv` (one row per involved draw) and `..._box.csv` (the 256 box pixels: depth, both motions, the camera-term
  expectation, path, masks); PNGs `..._colour_in`, `_depth`, `_motion`, `_path` (128 px crops of DLSS's inputs) and `_dlss_out`, `_final` (the same
  field of view, 256 px at a 2x upscale, clamped to the image).
- **Per draw:** order, vs/ps hashes, kind, counts, topology, class (opaque, alpha, additive, premult, multiply), blend, depth, stencil, cull, every
  render target's count, RT0 and depth format and size, the first four sampled textures; `box` and `ctr` (samples it wrote in the box and at the centre
  pixel); EDVR's decision (verdict, UI-layer family and decision, hologram list, coverage-mask slot and strength, engine-motion key `kf`, whether the
  pool shaders were substituted, and the motion source that implies); and a verdict from the rules.
- **At the box:** depth (raw and metres), the pass's motion, the motion DLSS was handed, the motion the camera term predicts (world and ship), the
  mismatch, the decision path and engine-record kind, hidden/UI flags, the coverage and bias masks, min/max over the box; DLSS preset, sizes,
  jitter, whether the history restarted and why, exposure (pre 1, scale 1, no texture on the VR path), whether a reactive mask was handed (and that
  K ignores it), and that no transparency/T&C mask is ever supplied.
- **How the draws are found:** every left-eye eye-sized draw is duplicated just before the game's own, scissored to the box and then to the centre
  pixel, writing nothing (colour mask 0, depth and stencil writes off), each under an occlusion query. The count is what the draw adds over the
  earlier draws. Status: `top` (visible at the centre pixel, or it missed the centre and no later opaque draw reached the box), `buried` (a later
  opaque draw wrote its centre pixel), `partial` (missed the centre, a later opaque draw wrote elsewhere in the box), `unmeasured`.
- **Verdict codes** (`scene_inspector_core.h verdictFor`): `TRANSLUCENT_NO_MASK` (blend, no depth write, no mask, no UI-layer take, no hologram
  pass), `TRANSLUCENT_DEPTH`, `BORROWS_MOTION` (drawn over the depth owner, inherits its depth and motion), `MV_MISMATCH` (camera path, motion off the
  camera term by over 0.5 px), `ENGINE_MOTION` (engine-record pixel; the object's motion against the world), `CAMERA_ONLY` (opaque depth owner on the
  camera term with no engine key: the railing shape), `NO_ROUTE`, `THIN`, `HISTORY_HIDDEN`, `UI_LAYER`, `MASKED`, `HOLO_PASS`, `OCCLUDED`,
  `PARTIAL`, `UNMEASURED`, `OK`. They fire independently; the first listed is the code.
- **Cost:** unarmed, one relaxed atomic load per eye draw, per clear, per indirect draw, per eye-0 temporal frame (twice), per frame boundary, plus
  the key poll; no allocation, no hook installed at press time. Section "Cost" has the per-site list. The capture frame hitches (two extra
  draws per eye-0 draw, up to 6000 draws, plus staging copies and file writes).
- **Tested:** `tools\scene_inspector_test` (in build.bat): the box mapping (symmetric, asymmetric, canted, gazing), owner classification, the
  camera-term maths, every verdict, line lengths at their worst, the CSVs, the PNG writer (decoded again), and on a WARP device with the production
  module linked in: every state the duplicate touches is restored (rasterizer, scissor, blend + factor + mask, depth-stencil + reference; shaders,
  layout, topology, targets, viewports, constant buffers untouched), the target's pixels are byte-identical to a control run, counts are exact, the
  debug layer reports no error. `edvr_log.py --inspect` has its own self-test, in the build gate.
- **Targets and the DLSS inputs (2026-10-14, after the first flight):** each render target is labelled from the temporal pass's word: `DLSS colour input`
  (the texture the game submits), `DLSS depth target` (shares the scene depth the pass reads), both, or `intermediate pass, not a DLSS input`. Verdicts
  apply only to draws in the first three; the others get `INTERMEDIATE` and no route, stack or occluded line, and are listed after the DLSS ones. The
  depth owner is the last draw that wrote depth at the centre into the scene depth, whichever colour target it wrote (Elite's opaque pass and its
  effects write different colour targets over one depth). The PNG crops carry a 1-pixel magenta ring just outside the box.
- **Path numbers, corrected:** in the decision texture path 2 is the WORLD camera term (pixels beyond the ship split, and the far plane) and path 1 the
  head's delta (temporal_shader_source.h: `decisionPath = count15 ? 2 : 1`, count15 set only in the world branch). The first build had them the other
  way round, which made every distant camera-path pixel read as a 2 px mismatch against the ship term. `blend_probe_core.h`'s legend has the same error.
- **Limits:** VR only (flat: refused, said; the flat runtime would need its own capture, not cheap). Left eye only. Depth-only draws (no colour
  target) are not seen. NVIDIA DLSS only for the pixel inputs (FSR/XeSS: refused with the reason; the draw half still reports). A draw on a UAV or
  stream-output target, and indirect draws, are `unmeasured`/counted, never duplicated. Clears of a target between draws are honoured
  (ClearRenderTargetView only; DiscardView and clears by other routes are not). A half-resolution target's box is 16 of ITS pixels.
- **Environment a finding depends on:** the native OpenXR runtime (any), DLSS preset and render size from the `dlss` line, the box line's canting.
- **Next:** one flight, the five captures below, then `--inspect`. Each capture names the draw(s) and the rule; the fix is then written from
  that, no second diagnostic flight. `hotkey.blend_probe` and its rig stay until this one has flown (the inspector is its successor and does
  not touch it).
- **Ruled out / not proposed:** a global transparent mask (K ignores it), sharpening or contrast tweaks (see the blur doc).

## Flight checklist

Set `hotkey.scene_inspect` in the live `edvr.ini` (Edit tool, not a regex) to a free bare key, for example `F4`; the launch line
`scene inspect: off; hotkey.scene_inspect is bound to F4` confirms it. Install with `python tools\install_edvr.py --target steam`, confirm with
`--verify-only`, and check the first log with `--version --expect-build HEAD`. For each capture look straight at the element so it sits at the
centre of your view (the box is where your head points, not where your eyes are), hold still, press once, wait for the hitch, wait a second,
then press again while turning your head. Write down the order: the log numbers captures `cap=1, 2, ...`.

1. **Pad hologram** (station hangar, ship on the pad, hologram over the pad): look at the hologram's brightest line. Expect a blended draw with no
   depth write in the stack over the pad surface. Read: its vs/ps (candidate `B75A6FF2CA9FA5D6` / `D56F859BE4781431`), `route` (is the UI layer
   or the hologram pass already taking it), and `TRANSLUCENT_NO_MASK` with `BORROWS_MOTION` (it takes the pad's depth and motion). Then again
   with the pad (and ship) moving.
2. **Gantry railing** (pad raising, then lowering): the railing at the centre. Expect an opaque depth owner on path 1 or 2 with `kf=-`:
   `CAMERA_ONLY`. If the pixel is path 11, read `engine_kind` and `ENGINE_MOTION`. One press per direction.
3. **Mail-slot force field** (station entrance): look at the field. Expect a scrolling additive or alpha draw; read whether its box count is the
   whole box or a thin set (`THIN`) and what it borrows from.
4. **Hologram advert** (inside a station, then outside a large one): the advert's quad. Same reading as 1; note whether the advert's vs/ps match
   the pad's (the blur doc suspects a shared material).
5. **Weapons fire** (hold fire at a dark target, press during the burst, with the beam at the centre): expect thin, fast additive geometry:
   `THIN`, no velocity source (`motion=camera-row`). A box of `empty` means the beam missed the centre; fire again.

After the flight: `python tools\edvr_log.py --target steam --expect-build HEAD --inspect` prints the latest capture; earlier ones are in the log
(`--grep "scene inspect"`) and in the CSVs. The state line names how far a capture got (NOT BOUND, NEVER PRESSED, ARMED NEVER COLLECTED,
COLLECTED NO REPORT, NO EYE PASS, EMPTY BOX, COMPLETE); only COMPLETE and EMPTY BOX mean the capture ran, and only EMPTY BOX means nothing
was in the box.

## Design

- **Why a duplicate under an occlusion query, and why before the game's draw.** The suggested alternatives (re-issue after the draw into a
  scratch target; a per-draw ID in a UAV) both change what the game's pipeline sees or need a pixel shader of our own, which breaks alpha-tested
  and discarding shaders. A duplicate with the draw's own shaders, state and bindings, a write mask of zero and a scissor changes nothing it
  reads or writes, and the occlusion count is the number of samples that survive the draw's own depth test AND its shader. Running it just before
  the game's draw makes the count "what this draw adds over the earlier draws", which is what the ownership classification needs. The blend probe
  proved the technique's hook points (vscreen.cpp `forwardWithVerdict`), so the inspector is offered the same draws by the same call.
- **Why two queries.** The box count says how much of the box a draw covers; the one-pixel count is exact. Ownership is decided at that pixel: a
  draw that wrote it and a later opaque draw that wrote it means buried, for certain. Elsewhere in the box it can only be bounded, hence
  `partial`.
- **Why per render target.** A post pass writing another target cannot hide a scene draw. Stacks and status are per RT0 resource; the "scene
  target" (the one with a depth view and most draws in the box) is the one the pixel rules and the depth owner apply to.
- **Why the pass's textures and not the draws' target for the pixel.** DLSS sees colour, depth and motion as the temporal pass builds them
  (`e.dlColour`, `e.dlDepth`, `e.dlMv`), not the game's HDR target. The capture-only decision shader variant the blend probe added gives the path and
  flags per pixel; the inspector asks the pass for the same, and stages 128x128 crops of all six inputs plus the two outputs in the pass's own
  frame.
- **The cross-check.** The camera-term motion at a pixel is recomputed on the CPU from the pass's constant buffer (rows, translation, frusta,
  depth encoding) exactly as `fetchHistoryT` does, for the world path and the ship path. A camera-path pixel must match; an engine-path pixel's
  difference is the object's own motion.
- **Why head-forward.** No gaze source exists in this tree (the eye-tracked foveation shipped off and left no reader). `headDirectionToImage`
  takes any head-space direction, so a gaze can replace the default without touching the rest.
- **Why not extend `blend_probe` in place.** It is a temporary instrument with a temporary key that is to be removed; the inspector is kept. They
  share `blend_probe_core.h` (state names, decision-flag decoder). When the blend probe goes, those helpers move into `scene_inspector_core.h`.
  The two are offered draws by one function and do not interfere.
- **Frame timing.** The temporal pass may run before or after the boundary that closes the draw frame; an inputs offer is taken only after a
  captured eye-0 draw has been seen, so the previous frame's offer is never mistaken for this one, and the first offer after that is this frame's.

## Cost

Unarmed (the state of every session that does not press the key):

| Site | What runs |
|---|---|
| `forwardWithVerdict` (every owner draw into an eye-sized target) | `blendProbeWantsDraws() \|\| sceneInspectWantsDraws()`: one more relaxed load |
| `hookedClearRtv` | one relaxed load (a few clears a frame) |
| indirect draws | one relaxed load, after the blend probe's |
| `drawGateSubscribed` | one relaxed load, once a frame |
| temporal pass, eye 0 | `sceneInspectInputsWanted()`: two relaxed loads a frame |
| frame boundary | `sceneInspectFrameBoundary()`: one acquire load |
| key poll | `Hotkey::pressed()` on an unbound key is a flag test |
| `dlaa.cpp` | named constants only; `dlaaEvalFacts` runs at capture |

No allocation, no hook, no state object, no query is created until a press. The rig times the idle entry points and prints them (WARP rig run,
2026-10-14, 20 million calls each): `sceneInspectWantsDraws` 0.79 ns, `sceneInspectInputsWanted` 0.83 ns, `sceneInspectFrameBoundary` 1.76 ns a
call; it fails above 100 ns and checks the idle module writes no log line. At a few thousand eye draws a frame the added load is about 3 microseconds a frame (a figure from a loop of calls, not from a flight).
The press itself: two draws per left-eye draw, two queries per draw, six staging copies, a few ms of PNG
and CSV writing; expect one long frame (the report is written at the boundary that sees the answers, up to 90 boundaries later).

## What the log shows if the capture never ran

Every state has its own words, so "nothing in the box" can only mean that. Install line at launch (`bound to X` or `NOT bound`); `armed cap=N`
at the press; `collecting cap=N` at the next boundary; then `report cap=N begin` and `complete cap=N`. Armed without collecting: no frame
boundary followed. Collecting without a report: the game ended or the tick faulted. A report with `refused (no eye pass seen ...)`: the frame
had no eye draw, which the line says is NOT an empty box. A report with `scene inspect empty: cap=N nothing wrote a sample inside the box (T
eye draws were tested ...) The capture RAN`: the box was empty. `edvr_log.py --inspect` names the state. `tools\scene_inspector_test` pins each of
these (S7).

## Journal

- **2026-10-14, build.** Pure core, device module, rig, hooks (vscreen.cpp draw offer, clears and ticks; temporal_pass.cpp inputs and outputs;
  native_temporal.cpp `nativeTemporalEyeToHead`; dlaa.cpp `dlaaEvalFacts`; device_hook.cpp key and environment), `edvr_log.py --inspect`, build.bat
  rig and compile lines, edvr.ini comment. Nothing installed or flown.
- **2026-10-14, first flight (v0.20.0-rc.1-4-g66dfe94f, Frontier).** Seven captures. The inspector worked as built; two tool faults surfaced and were fixed
  (the path labels above, and verdicts on intermediate lighting and bloom targets). Railing (cap 1, 2): the railing's draws are keyed and substituted
  (`DE545DC8EE4FBB87`/`E46E3E4832B2FDB0` rigid#3, `EB5234DB6ADB491D`/`3434972DB5336AA4` rigid#0) but their pixels read engine kind 3, a pool record with
  no marker from this frame or the last 64, so the pass falls back to the world camera term. See the coordinator report for the proposed fix.
