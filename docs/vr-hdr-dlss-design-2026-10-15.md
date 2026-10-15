# VR HDR DLSS: the eye route's input before the tonemap (design, 2026-10-15)

## Status

- **State (2026-10-15): DESIGN ONLY, no code.** Read-only research against da018ff7 (main), `origin/claude/rim-jitter`,
  `origin/claude/flat-bloom-input` (a565d842), `origin/claude/dlss-exposure` and `origin/claude/panel-dataflow-census`. Awaiting
  Sean's review. Nothing here is built or flown.
- **Goal:** the VR cockpit eye route (temporal_pass, native_temporal) gives DLSS the game's HDR scene (R11G11B10F, 2016x2017
  per eye) instead of the 8-bit post-tonemap image it takes at the OpenVR Submit today (`hdr=0`, pre-exposure 1.0, no exposure
  texture, R8G8B8A8).
- **Why:** the Mandalay rim's dashed highlight crawls. It is a single row of shell pixels, clipped to 255 (Sean's 2026-10-15
  capture). Jitter, motion, the 10 m split, kind-4, history reset, sun glare and the upscale ratio are already ruled out
  (docs/dlss-blur-elements-2026-10-14.md on `claude/rim-jitter`). Hypothesis H1 below: the clip throws away the highlight's energy
  before DLSS can average it.
- **Placement decision (proposed): option B1, "HDR at the door, tone after DLSS".** At the game's tonemap draw (tonemap_admit.h
  already admits it per eye, EDHM variants included), copy that eye's HDR source (PS t1) and the exposure texel. At the Submit door,
  run the existing eye DLSS with that copy as colour, IsHDR, and the game's own exposure as the exposure texture. Then re-issue the
  game's own tonemap draw at output size (4032x4034) onto the DLSS output. The UI layer already re-issues this draw once per eye at
  door output size (ui_layer.h:34-48, flown under fix.ui_quality). Everything after the door stays where it is: UI resolve, UI-layer
  composite, RCAS, FSS heal, sky ring and menu.
- **Fallback: option B2,** the same with the capture moved before bloom and the bloom added back at output size. Take it only if
  stage 1 shows that the tone's input is not scene plus an additive bloom, or that DoF or blur are active in the cockpit.
- **Rejected:** C, flat-style in-place DLAA at H (it inherits flat's bloom brightening and gives up the 2x upscale); D, upscaling
  at H with the game's post at output size (the game's targets are sized for render size); E, transcribing the tonemap
  (cockpit-hud-layer-design-2026-09-27.md "Ruled out": it drifts from EDHM); F, LDR compensation (AGENTS.md).
- **Open hypotheses (stage 1 discriminates all of them in one flight):**
  - H1: the rim's dash pixels carry HDR radiance well past the tone's white point, so the LDR input loses energy. B1 fixes it only
    if H1 holds.
  - H2: the dashes are geometric. The shell's crease covers a different sub-pixel fraction each jitter phase, and an HDR average
    would crawl the same way. If H2 holds, B1 does not fix the rim.
  - H3: the crawl rides on bloom, so a bloomed H at the tone draw still crawls. This favours B2.
  - H4: the tone PS reads H with UV sampling, so a re-issue at 4032 is exact. The UI layer's re-issue is evidence for it, but not
    proof for a full-frame source.
- **Inheritance of the flat bloom brightening (risk 1):** B1 never writes H, so the game's bloom and exposure read the raw scene.
  That removes flat's hypotheses (a) and (c) by construction (section 107 on `claude/flat-bloom-input`). Its (B), HDR AA spreading
  edge energy, remains possible: stage 2 measures it.
- **Exposure (risk 2):** give DLSS the scalar the game's tone applies, from tone VS t0, as a 1x1 R32F per eye. exposure_fix has
  already copied eye 0's strip over eye 1's before eye 1's tone draw, so each eye's copy is the value its own tone used.
  AutoExposure is refused: section 106 measured it dimming flat and losing stars. Which strip texel is the multiplier is
  CONFLICTED (exposure_chain.h:8 against exposure_fix.cpp:238). Stage 1 settles it.
- **Cost (risk 4), estimate, not measured:** the input bytes do not change (R11G11B10 and R8G8B8A8 are both 4 B/px). Added per
  eye: an H copy (16 MB), an R11G11B10 DLSS output at 4032x4034 (65 MB), and one full-screen tone re-issue at 16.3 MP. Expect
  +0.3 to 0.6 ms a frame for two eyes on Sean's 5090. Stage 2 measures it with the GPU pass profiler.
- **Temporary keys:** none yet. Stage 1 adds one hotkey and stage 2 one temporary A/B key (named in "Flight plan"). Both go when
  the arc closes, proposed and quoted as AGENTS.md Scope control requires.
- **Unverified premise:** Sean reports the rim is lit by the lighting quad 53211E8C/7EAC7196 and the sun pass 7E38A6AA/7CECABDE.
  No doc read here records that capture, and build-332753.md:252 lists 53211E8C as a ring-family hash. Stage 1's census names the
  draws that write H in the box.
- **Next flight:** stage 1 (observe only). Face the star with the rim still, then arm the inspector's HDR burst, then the census.
  Read the log with `python tools\edvr_log.py --target steam --expect-build HEAD`, after checking the log's runtime and
  headset_key line.
- **Ruled out (here):** nothing yet. For the rim, see the blur-elements doc's Status on `claude/rim-jitter`.

## The problem

DLSS averages jittered samples over time. A highlight narrower than an input pixel lands in a different pixel fraction each phase.
In linear HDR, the average of those phases is the highlight's true footprint energy, and the tone applied afterwards turns it into
a steady, slightly wide bright line. Today the tone and the 8-bit clip come first: every phase where the highlight touches a
pixel reads 255, whatever its radiance, and the average depends on how often each pixel is touched. That frequency varies along
the rim, which gives a dashed line that moves as the history re-weights. Flat sidesteps this, because its HDR route resolves H
before bloom and tone (flat_hdr_route.h). The VR cockpit route never sees H.

## The eye route today (cites against da018ff7)

- **The door is the OpenVR Submit.** The path runs openvr_compositor.cpp:138-145 to native_runtime_host.h:2040-2090 (capture,
  treat), then native_temporal.cpp:423/514, then `edvrTemporalAa` (temporal_pass.cpp:6111) and `temporalInner` (2867). The colour
  is the submitted 8-bit eye, cropped and copied into `e.dlColour` (4076-4097).
- **NGX:** `dlaaEvaluate` gets no hdr flag (temporal_pass.cpp:4289-4291). Its flags are MVLowRes | DepthInverted
  (hdr_backend_flags.h:27-29), with no IsHDR, no AutoExposure and no exposure texture, and pre-exposure and exposure scale are 1
  (dlaa.cpp:719-721). The flat path calls the same function with hdr=true and a fixed 1x1 R32F of 1.0 (dlaa.cpp:721-738). B1
  reuses that plumbing.
- **Depth and motion:** an mv CS writes `e.dlDepth` (R32F) and `e.dlMv` (R16G16, render size, unjittered) from the scene depth
  (temporal_pass.cpp:3089-3120, 4181-4184). These do not depend on where the colour comes from.
- **After DLSS:** the UI resolve (3862-3870) writes `e.dlSubmit`, and the sky ring reads `dlColourSrv` and `dlOutSrv` (4459-4463).
  The FSS heal reads the temporal output (native_runtime_host.h:2092-2110). RCAS and the UI-layer composite follow in
  native_sharpen.cpp:81-124.
- **The tonemap draw** (VS 2D78DC3FD2C0C543 / PS 99C21CEB7A699821, measured) is a 3-vertex full-screen triangle, unblended, with
  no DSV. VS t0 is the exposure (R32), PS t0 the 3D grading LUT, PS t1 the HDR source, and b2 holds at least 256 bytes; it writes
  the RGBA8 eye (cockpit-hud-layer-design-2026-09-27.md:68-83). tonemap_admit.h admits it and its variants structurally.
- **Exposure:** three 252x252 downsamples, then CS B8E727E9, then CS F1FB2EEF (the 6x1 R32F strip), then the tone (exposure_chain.h:4-10).
  exposure_fix skips eye 1's dispatches and copies eye 0's u0-u4 over (exposure_fix.cpp:349-384, 808-843).

## The frame between H and the door

- The view feature list (ui-layer-2026-09-23.md on `claude/panel-dataflow-census`, 2082-2105, CONFIRMED in disassembly) runs
  ... Forward, Blur, Restore, DepthOfField, Bloom, HDR, AntiAliasing, Scaling, UI, PostGUIDepthPass, PostGUI, Cinema. INFERRED:
  list order is execution order, and the "HDR" node is the tonemap.
- Bloom is BELIEVED to be composited into H before the tone, since the tone has no separate bloom input
  (cockpit-hud-layer-design:81-83). No bloom hash is known in VR. DoF and blur are undocumented in VR.
- After the tone: the game's AA (EDVR's DLSS replaces it), Scaling, then the UI. fix.ui_quality already takes the post-tonemap
  composites (menus, the 2D screen, the loading screen) and the HDR HUD families out of the eye into per-eye layers at door output
  size (ui_layer.h:18-48). What else writes the eye between the tone and the Submit is NOT KNOWN. Stage 1's census answers it.

## Options

| | DLSS reads | Then runs, at size | Flat bloom brightening | Cost vs today | What moves | Verdict |
|---|---|---|---|---|---|---|
| A | 8-bit eye at Submit (today) | UI resolve, RCAS at 4032 | n/a | 0 | nothing | the rim crawls |
| **B1** | H copied at the tone draw (bloom in), at Submit | game tone re-issued at 4032 on the DLSS output, then today's chain | (a)(c) impossible; (B) to measure | +0.3-0.6 ms est. | colour source, exposure, a tone replay at the door | **proposed** |
| B2 | H copied at scene end (pre-bloom) | DLSS + upsampled bloom delta, then tone re-issue at 4032 | as B1 | B1 + a second copy and a delta pass | as B1, plus a per-eye scene-end trigger | fallback if H3, or the composite is not additive |
| C | H in place, DLAA at render size (flat parity) | game bloom, tone and post at 2016, then a 2016-to-4032 upscale at the door | inherited: bloom reads the resolved H | a second upscaler or the loss of the 2x | writes the game's H | rejected |
| D | H, upscaled to 4032 in place | game bloom, exposure and tone at 4032 | inherited | large | the game's target sizes | rejected: not ours to resize |
| E | H, with EDVR's own tone | EDVR tone HLSL | n/a | small | everything | ruled out (drifts from EDHM) |
| F | 8-bit, plus reactive, sharpen or clamp tweaks | | | | | ruled out (compensation) |

B1 against the VR world route: vr_world_route resolves H once a frame, for the flat world on foot, with AutoExposure and the
flat detector's single trigger (vr_world_route.cpp:261-325, 559-584). The cockpit has one H per eye and no gate, so B1 reuses
the eye route's own evaluate with hdr=true, not the world route.

## What moves under B1 (risk 3)

| Piece | Today | Under B1 |
|---|---|---|
| DLSS colour | `e.dlColour` = submitted 8-bit | a new per-eye HDR copy of tone PS t1; `e.dlColour` stays for the readers below |
| Prep / mv CS colour (t0) | submitted 8-bit | unchanged (TO CHECK what it reads colour for) |
| Sky ring | reads `dlColourSrv` and `dlOutSrv` | keeps the 8-bit `dlColourSrv`; `dlOutSrv` is the re-toned output (same space as today) |
| UI coverage mask, UI resolve | post-DLSS, 8-bit | unchanged; runs on the re-toned output |
| UI layer (8-bit and HDR HUD) | its own tone re-issue at the tone draw, composite after RCAS | unchanged; the HUD families never reach H while fix.ui_quality is on |
| HDR HUD depth seed, hologram depth pass | depth only, in the game frame | unchanged (depth path) |
| Motion vectors, depth copy, jitter | render size, at the door | unchanged; H and the 8-bit eye are the same jittered frame |
| Exposure for DLSS | none | 1x1 R32F per eye, copied at the tone draw |
| Tone | the game's, at 2016, into the eye | the game's at 2016 still runs (the UI resolve and readers need it); a replay at 4032 writes the DLSS output |
| Scene inspector | stages 8-bit colour | also stages the HDR copy and the exposure (stage 1) |
| Withheld frame / `temporal.skip` | no H involved | the H copy is keyed by eye and frame sequence; a mismatch falls back to today's path and is counted |

With fix.ui_quality off, the HDR HUD is in H, so B1 hands it to DLSS in HDR. The coverage mask and UI resolve still cover it, as
they do today.

## Risks 1, 2, 4, 5

1. **Flat bloom brightening.** Flat's report: with DLAA and bloom on, stars blow out, the strut halos and planets wash out
   (section 107). Its hypothesis A, DLSS overshoot, was refuted by flight 8459abf4; B (HDR AA spreads edge energy) and D (preset)
   are untested, and the bloom probe has never flown. In flat, bloom reads the resolved H. In B1, bloom reads the raw H and DLSS
   only sees its result, so the surviving question is B. Stage 2 measures it directly: frame mean luma, p99 and star max of the
   re-toned HDR output against today's 8-bit DLSS output, on the same frame.
2. **Exposure.** NGX scales the input by the exposure texture before its internal tone compression. The texture should hold what
   the game's tone multiplies by (to confirm against the NGX header on the build machine). Fixed 1.0, flat's shipped choice, is
   the fallback if stage 1 finds the strip is not a plain multiplier. Pre-exposure stays 1.0, because nothing scales H before
   EDVR reads it.
3. See "What moves".
4. **Cost.** The flat fit gives eye DLSS at 0.093 ms per output MP, about 1.5 ms per eye at 16.3 MP (design-flat-temporal-aa:5782-5788).
   The input format does not change the byte count. What is new: the H copy (about 0.03 ms), the tone replay (a LUT fetch per
   output pixel, about 0.1-0.2 ms per eye, estimated), and any IsHDR overhead inside NGX (unknown). VRAM: about +160 MB for two
   eyes.
5. **Bloom, DoF and blur.** Under B1 they all run before the capture, on the raw H at render size, exactly as today. DLSS sees
   their output, as it does now. Bloom is low frequency and upscales cleanly. Under B2, bloom is added after DLSS as an upsampled
   delta. That is exact only if the composite is additive, which stage 1 checks.

## Flight plan (instrument first)

### Stage 1: observe only, one build, one flight

Built on a branch cut from `claude/rim-jitter` (it carries the inspector). TEMPORARY hotkey `hotkey.hdr_inspect`, unbound by
default, logs only, and the frame is untouched.

- **I1, HDR box at the tone draw.** In the tone draw's hook (tonemap_admit), per eye, `CopySubresourceRegion` the inspector's
  48x48 box from PS t1 into staging, plus all six strip texels from VS t0, and Map them with DO_NOT_WAIT (the inspector's
  `stageOne`, scene_inspector.cpp:752-800; the R11G11B10 decode from luma_probe.cpp:49-61). Pair it with the 8-bit box the
  inspector already stages at the door. Log per box: H max, p99 and mean per channel; the pixels with any LDR channel at 255; and
  for those, `clip ratio` = H x g over the radiance where the fitted tone reaches 254. Also write a per-pixel CSV (H rgb, LDR rgb,
  strip[0..5], eye, frame).
- **I2, burst.** Repeat I1 for 16 consecutive frames (two Halton-8 cycles), head still. An offline script (a new tool with
  `--self-test`, added to the build gate) fits a monotone tone curve from the paired samples. It then compares, along the rim row,
  the dash contrast (max/min) of `mean_t(LDR)` (today's order) against `tone(mean_t(H x g))` (B1's order).
- **I3, census from H to the door.** On one armed frame per eye: every draw and dispatch from the last scene write to H to the
  Submit, with its shader pair, RTV format and size, whether it reads H (SRV identity), its blend, and whether ui_layer took it.
  It extends draw_census (`viewFmtOf`, draw_census.cpp:275-312), with the GPU pass profiler armed alongside for timings. This
  names the bloom composite, DoF or blur if active, the draws that write H in the rim box (the 53211E8C / 7E38A6AA premise), and
  every 8-bit write between the tone and the Submit that the UI layer does not take.
- **I4, exposure texel.** From I1's strip and box means, the texel whose value times mean(H) best predicts the fitted curve's
  input scale. Both eyes are logged, to confirm that eye 1 matches eye 0 after the share.
- **I5, pairing.** For 30 s, count the tone-draw sequence against the Submit sequence per eye (`hdr pairing: ...`). Any mismatch
  means B1 needs the keyed fallback.
- **I6, tone sampling.** Log the tone PS's t1 sampler binding, and whether its SRV read is Sample or Load (from the shader bytes,
  as the shader dumps do). This decides H4.

What the log shows if the instrument never ran: no `hdr inspect:` lines at all. A missed box reads `hdr inspect: box=miss`, never
zeros. A Map that is not ready by the next frame is counted as `pending`, and never logged as radiance 0.

Discriminating signatures:

| Hypothesis | Confirms | Refutes |
|---|---|---|
| H1 clip energy | most dash pixels at LDR 255 with clip ratio >= 2, and I2 HDR-order dash contrast at least 2x lower | dash pixels below the white point (ratio < 1.2), or I2 contrast unchanged |
| H2 geometric | I2 HDR-order contrast as high as LDR-order | as H1 confirms |
| H3 bloom carries it | I3 shows the bloom composite writing the rim box with a delta comparable to the dash | a negligible bloom delta in the box |
| H4 UV sampling | I6: Sample through a linear or point sampler, with UV from the triangle | Load with pixel coordinates (then the replay needs a viewport or coordinate fix) |
| exposure texel | one texel predicts the scale within 5% on both eyes | none does (use fixed 1.0) |

If H1 is refuted, B1 is not a rim fix. Write `ruled out: clip energy as the rim crawl, because ...` in the blur-elements doc and
stop here. The HDR route may still be worth having for other glints, but that becomes a separate decision for Sean.

### Stage 2: shadow B1, observe only (only if stage 1 confirms H1)

Each eye runs a second NGX feature (IsHDR, B1's inputs) into a private target, with the tone replayed at 4032, never presented,
for N frames on the same hotkey. It logs the frame mean luma, p99 and star max against today's output (risk 1), the GPU ms from
the profiler (risk 4), and a PNG pair of the rim box. Two extra features per eye cost VRAM only while armed.

### Stage 3: the move

B1 behind a TEMPORARY key `advanced.temporal_aa_hdr = on|off` (live, default off until flown, then on), A/B'd in one session on
the rim, the stars and a planet. When the arc closes, ship the winner as the behaviour and propose removing the key, quoted.

## Questions for Sean

1. B1 (tone replay at the door) or B2 (pre-bloom capture) as the target, if stage 1 cannot tell them apart?
2. Should stage 1 also capture one non-rim glint (a station light strip) to test "other thin bright glints"?
3. Which runtime and headset for stage 1: Pimax OpenXR or SteamVR (aapvr)? The rim capture was Frontier, native OpenXR, at
   2016x2017.

## Journal

- **2026-10-15, the design (read-only).** Three Haiku research passes: the eye-route door, the flat and world prior art, and the
  engine passes. Findings beyond the brief: the door is the Submit, not the game's AA node; the tone is a single full-screen draw
  with H, LUT and exposure as its only inputs; the UI layer already replays that draw at door output size; and section 107's
  probe has never flown. Its doc lives on `claude/flat-bloom-input`, not main.
