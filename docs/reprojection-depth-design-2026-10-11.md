# Reprojection depth: submitting per-eye depth with EDVR's projection layer

## Status

- **State:** DESIGN ONLY (2026-10-11). Nothing is built and nothing has flown.
  Branch `claude/reprojection-depth-design`.
- **Today:** EDVR never enables or submits `XR_KHR_composition_layer_depth`.
  It submits one colour-only projection layer (`frame_boundary.h:266-284`).
- **Main finding:** benefit is uneven across the runtimes flown (section 5).
  VDXR discards depth; PimaxXR forwards it; Sean's Pimax Play runtime is
  unverified; Meta ASW 2.0 uses it via LibOVR (OpenXR unverified); Valve
  recommends it; WMR documents depth reprojection.
- **Recommendation:** stage it. Stage 1 costs about 0.05-0.1 ms of GPU per eye
  and adds no new GPU pass on Elite's device. It sends the render-size R32
  depth that the temporal pass already writes (`e.dlDepth`, which already
  carries EDVR's UI depth), over the existing cross-device copy, into an XR
  depth swapchain.
- **Key (proposed, temporary while the arc is open):**
  `fix.reprojection_depth = auto|on|off`, default `off` until flown. Auto: on
  when the runtime advertises the extension, the runtime is not VDXR, and the
  temporal pass is producing depth this frame.
- **Open hypotheses:**
  - H1: Pimax Play's runtime advertises the extension.
  - H2: Smart Smoothing reads the depth.
  - H3: the depth reduces the warping of the cockpit frame and HUD under forced
    half-rate.
  - H4: on Oculus, depth at render size is refused or ignored, because LibOVR
    ASW 2.0 needs depth at colour size.
- **Ruled out:**
  - Virtual Desktop SSW as a beneficiary, because VDXR defines
    `IGNORE_DEPTH_SUBMISSION` with the comment "Virtual Desktop does not use
    the depth buffer for anything at this time"
    (`virtualdesktop-openxr/RuntimeConfiguration.h`, last changed 2024-05).
  - A VD flight can only show that depth does no harm.
- **Next flight:** a Stage 0+1 build on the Crystal Super (Pimax OpenXR) with
  Smart Smoothing forced on.
  - Read `reprojection_depth_caps` first: advertised or not, and the formats.
  - Then fly A/B with the key: strafe beside the canopy strut, with HUD text
    over stars.
- **Questions for Sean:**
  - Q1: is Stage 0+1 worth a flight, given that VD gains nothing?
  - Q2: can he force Smart Smoothing on, at a fixed half rate?
  - Q3: is a Quest Link (Oculus OpenXR) tester available? That is the best
    documented consumer.
  - Q4: should depth stay off while `fix.temporal_aa = off`?

## 1. Today's submission (openxr half)

- **Extensions** are enabled at `native_runtime_host.h:2686-2707` (API 1.0.0;
  `enabled[6]` is full and needs a slot). The extension is not core in 1.1.
- **Compose.** One sRGB colour swapchain per eye (`d3d11_stereo.cpp:190-211`).
  `renderCaptured` (`:241-365`) blits the eye with a linear sampler into a
  placement viewport (fov_trim's black border, `:288-299`), fades, and fills
  `layerViews_` with the whole image (`:361-365`). `finish()` rejects a non-null
  `layer.next` (`frame_boundary.h:276`); depth chains on each *view*, so that
  check does not block it.
- **Cross-device copy** (`separate_device=1` is mandatory,
  `native_local_config.h:70,82`): at Submit the producer copies Elite's eye into
  a keyed-mutex shared texture (`shared_texture_transfer.cpp:269`); the consumer
  copies it to a private texture on the runtime device (`:308`). Formats:
  RGBA8/BGRA8 only (`:26-43`). Under **frame_end_overlap**
  (`native_runtime_host.h:1353-1373`) the consumer copy, compose and xrEndFrame
  run on the owner, overlapping Elite's next frame.
- **Replay and background:** a withheld frame replays `previousPair` with old
  views (`:2224`); the loading skybox goes through `composeBackground` (`:2249`).
- **The eye image is EDVR's texture** while the temporal pass runs
  (`e.dlSubmit`, after the upscaler, the UI resolve and the UI layer;
  `temporal_pass.cpp:4127-4132`, `native_sharpen.cpp:81-124`).
- **Runtimes flown:** Pimax OpenXR (Pimax Play, `PiOpenXR_64.dll`, Sean's
  Crystal Super; `engine-render-performance-2026-09-19.md:1568-1573`); PimaxXR
  (`canted-projection.md:51`); SteamVR/OpenXR with an 8KX (`:1577`); Oculus
  OpenXR over Air Link and VDXR, both Quest 3
  (`openxr-implementation-status.md:14,92,156`); WMR OpenXR, user 5
  (`field-crashes-2026-09-30.md:12`).

## 2. Elite's depth

- **Format.** One target per eye at render size: `R32G8X24_TYPELESS` with a
  `D32_FLOAT_S8X24_UINT` view and bind flags 0x48. It is cleared to 0
  (`anti-aliasing.md:923-929`).
- **Convention.** Reversed-Z, infinite far, d = near/z, sky = 0, near 0.025 m
  (`design-flat-temporal-aa-2026-09-23.md:7427`, `engine-render-pipeline.md:310`,
  `canted-projection.md:276`, `depth_probe.cpp:48`). Elite *asks* for
  0.025..50000 and 0.1..1000 (`anti-aliasing.md:959-961`; logged as
  `projection_query`, `native_runtime_host.h:705`) but builds its own infinite
  projection. First-person and weapon draws use near 0.0675
  (`fixed_shader_source.h:634-635`), so the same raw value means 2.7x the
  distance.
- **Which target.** The probe picks the per-eye pair (`g_scenePick`,
  `depth_probe.cpp:582,677-720`); the temporal pass fetches it at Submit
  (`temporal_pass.cpp:2855-2893`).
- **Resolution.** The motion dispatch writes `e.dlDepth` (R32_FLOAT, render
  size) from the scene depth and the UI-depth copy (`temporal_pass.cpp:3871-3887`).
  Under DLSS/FSR the output is larger (for example 2016x1991 in, 4032x3982 out);
  under DLAA, or with no upscale, the sizes match (`:3730-3759`,
  `native_temporal.cpp:444-450`).
- **Final** at Submit, after Elite's eye render. Smoke, particles and alpha UI
  write no depth and carry the depth behind them: the usual reprojector
  artefact.
- **Gated:** the probe runs only with `fix.temporal_aa` (shipped `off`,
  `edvr.ini:376`) or `advanced.eye_depth_capture`.

## 3. UI and HUD

- **What the UI pixels carry today.** `ui_depth` reissues the supported HUD,
  hologram and interface composites depth-only into a *private* copy of the
  scene depth. Each eye's copy is seeded once a frame (`ui_depth.h:1-16`,
  `uiDepthTemporalDepth` `:230`). Interface-projection composites are converted
  to the scene's encoding.
  - So in `e.dlDepth`, holo panels and the flight HUD already sit at their own
    plane rather than at the stars behind them.
  - That is the right depth for reprojection. The HUD then moves with the
    cockpit.
- **What is not covered:**
  - (a) draws the classifier declines, which keep the scene depth behind them;
  - (b) the `fix.ui_quality` layer's own content: menus, 2D screens, loading
    screens and the cockpit-HUD families. These are composited after the
    upscaler (`uiLayerComposite`, `ui_layer.cpp:4578`). Their alpha is
    transmittance, held internally;
  - (c) EDVR's own menu (`menu.treat`, `native_runtime_host.h:1727-1737`);
  - (d) weapon pixels encoded at near 0.0675.
- **Answer.** Stage 1 submits `e.dlDepth` as it is: right for (a)-covered UI,
  wrong but bounded for the rest. Stage 2 fixes the rest:
  - (b): where the layer's transmittance is below 0.5, write the depth of the
    surface the layer is drawn on;
  - (c): omit depth while the menu shows;
  - (d): re-encode by the first-person mask.
- **Not a separate layer.** A depthless UI quad would need the UI flattened
  onto one plane; the cockpit HUD is not one plane, and it doubles compositor
  blending. (Meta computes quad/cylinder depth analytically, which would help
  only if EDVR's own menu became a quad.)

## 4. Spec facts (XR_KHR_composition_layer_depth, rev 6, ratified)

Quoted from the man page and the extension source
([man](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrCompositionLayerDepthInfoKHR.html),
[adoc](https://raw.githubusercontent.com/KhronosGroup/OpenXR-Docs/main/specification/sources/chapters/extensions/khr/khr_composition_layer_depth.adoc)):

- "add the XrCompositionLayerDepthInfoKHR to the next chain for all
  XrCompositionLayerProjectionView structures in the given layer". So it is all
  views or none, per layer.
- "The XR runtime may use this information to perform more accurate
  reprojections taking depth into account." A runtime is not obliged to use it.
- "minDepth and maxDepth are the window space depths that correspond to the
  near and far frustum planes, respectively." "minDepth must be less than
  maxDepth." Both are in [0, 1].
- "nearZ and farZ are the positive distances in meters to the near and far
  frustum planes, respectively." "nearZ and farZ must not be equal." Both are
  in "(0, +infinity]". "reversed depth ... can be achieved by making nearZ >
  farZ."
- "The resolution of the depth image need not match that of the corresponding
  color image."
- "The swapchain must have been created with a XrSwapchainCreateInfo::faceCount
  of 1." No format or usage flag is mandated. The formats come from
  `xrEnumerateSwapchainFormats`, with `XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT`.
- Every frame is not required. Per
  [XrReprojectionModeMSFT](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrReprojectionModeMSFT.html),
  a runtime uses "a runtime defined fallback reprojection mode" when depth is
  missing, and does not fail xrEndFrame.

**For Elite:** minDepth=0, maxDepth=1, farZ=0.025, and nearZ set to a *finite*
1e6 m rather than +infinity.
- Reason: PimaxXR converts nearZ/farZ to `Projection23 = farZ*nearZ/(nearZ-farZ)`
  (`pimax-openxr/frame.cpp`). With +infinity that is inf/inf, which is NaN.
- Cost: a finite 1e6 shifts d by 2.5e-8, below the float spacing of every
  distance under 1 km.

## 5. Per runtime

| Runtime | Advertises | Reprojection reads depth? | Source |
|---|---|---|---|
| VDXR (Virtual Desktop) | yes | **No.** `IGNORE_DEPTH_SUBMISSION`, overridable only by registry `quirk_use_depth` | VDXR source, `RuntimeConfiguration.h`, `session.cpp` (verified) |
| PimaxXR | yes | Passed on as `pvrLayerType_EyeFovDepth`. Whether Smart Smoothing uses it is unverified | `pimax-openxr/frame.cpp` (verified) |
| Pimax OpenXR (Pimax Play; Sean) | unverified | unverified (a Dec 2024 forum remark by mbucchia says it does) | closed source; Stage 0 logs it |
| Oculus OpenXR (Link) | yes (ChimeraX ticket, v63) | ASW 2.0 does through LibOVR: "Depth and color buffers must use matching resolutions", D32_FLOAT, no MSAA. Through OpenXR: unverified (forum reports of no effect and of block artefacts) | [ASW 2.0 guide](https://developers.meta.com/vr/blog/developer-guide-to-asw-20/) |
| SteamVR OpenXR | yes (since 1.16.3) | Valve: depth "will enable users to optionally run at higher refresh rates using improved reprojection techniques" (Steam Frame page). Desktop Motion Smoothing: unverified | [Steamworks perf criteria](https://partner.steamgames.com/doc/steamhardware/steamframe/compat/perf_criteria) |
| WMR OpenXR | yes | Documented per-pixel depth reprojection; "Always use XR_KHR_composition_layer_depth" (HoloLens-focused page) | [MS best practices](https://learn.microsoft.com/en-us/windows/mixed-reality/develop/native/openxr-best-practices) |

**Auto rule.** On when all of these hold:
- the extension is advertised;
- the runtime name is not VirtualDesktopXR (it would be wasted GPU);
- a depth source exists this frame.

Size rule: render size, except on runtimes named Oculus, where the depth is
upsampled to colour size (the LibOVR requirement).

## 6. Data path (fewest passes)

1. **Elite's device: no new pass.** `e.dlDepth` already exists. In `treatEye`,
   return it beside the colour. This needs a temporal table v2
   (`common\native_temporal.h`, today version 1) that adds the depth texture
   and its encoding (near, reversed, UI-merged), and a matching client in
   `src\openxr`.
2. **Producer copy.** It runs in the same executor job as the colour copy and
   inside Submit, so frame_end_overlap is unaffected. `dlDepth` is swapped with
   `zPrev` and rewritten next frame (`temporal_pass.cpp:4286`), so it must be
   copied there. Cost:
   - a second `SharedTextureTransfer` slot with an R32_FLOAT family added to
     `formatChoice`;
   - one CopyResource per eye at render size.
3. **Consumer.** Skip the private copy. Read the shared texture under the keyed
   mutex in one full-screen pass on the runtime device that writes `SV_Depth`
   into the XR depth swapchain's DSV. That one pass does the following:
   - converts to whatever depth format the runtime offers (D32_FLOAT preferred,
     then D24S8, then D16);
   - applies the same placement viewport as the colour and clears the trim
     border to 0 (far);
   - when colour size is required, upsamples with Gather and takes the nearest
     surface (max in reversed-Z). Never bilinear: that invents depths at edges.
4. **Chain.** Attach `XrCompositionLayerDepthInfoKHR`, with the depth swapchain's
   full-image subImage, on both views.
5. **Swapchains.** Create the depth swapchains at render size, or at colour size
   on Oculus. Recreate them when the size changes; HMD Quality rebuilds already
   recreate.

**GPU cost per eye** (bandwidth-bound; RTX 5090 at about 1.5 TB/s, an 8 GB
card at about 450 GB/s):

| Case | Bytes moved | 5090 | 450 GB/s card |
|---|---|---|---|
| Render size 2016x1991 (4.0 Mpx): copy 32 MB + pass 32 MB | 64 MB | ~0.04-0.06 ms | ~0.15 ms |
| Colour size 4032x3982 (16 Mpx), sent at render size and upsampled in the pass: 32 + 16 + 64 MB | 112 MB | ~0.08-0.1 ms | ~0.25 ms |
| Colour size sent whole (avoid) | ~256 MB | ~0.2 ms | ~0.6 ms |

CPU: one extra acquire, wait and release per eye on the owner thread, plus one
draw. The owner-thread cost should be measured in the existing
`native_submit_phases` window.

## 7. Failure modes and rules

| Case | Rule |
|---|---|
| Loading skybox (`composeBackground`) | Depth cleared to 0 (infinity). Exact for a skybox, and costs one clear |
| Fully black (fade = 1), transition withholds | Clear to 0; withholds follow the replay rule |
| Partial fade | Submit depth unchanged |
| Withheld frame replay (`previousPair`) | Stage 1: omit depth and count it. Stage 2: keep the depth in `exchangeBuffers` with the pair |
| Temporal pass off, stood down, maps (temporal AA off on all maps), FSS guard, flat | No source, so no depth. Counted by reason |
| EDVR menu visible | Omit (Stage 1) |
| On foot | Weapon near 0.0675 makes the weapon look 2.7x closer. Stage 2 re-encodes |
| Size change mid-flight (HMD Quality, DLSS mode) | Omit until the depth swapchain is rebuilt |
| frame_end_overlap | Unaffected: the producer copy is at Submit and the consumer runs on the owner |
| Pose cache (`claude/render-cpu-posecache`) | Unaffected: depth rides the same `frameViews` as colour |
| Depth not final (smoke, particles) | Inherent. Document it; no compensation |
| Canted / parallel projection | The same fov and pose as colour, so nothing new |

## 8. Instrumentation

- **Once a session, the openxr log:**
  `reprojection_depth_caps,runtime=,advertised=0|1,formats=<list>,chosen=,mode=auto|on|off,decision=on|off,reason=`.
  A silent log means the code never ran.
- **Window line:**
  `native_depth_submit,window=,pairs=,with_depth=,omitted=no_source/replay/menu/background/resize/off,size=WxH,colour=WxH,near=1e6,far=0.025,gpu_ms=p50/p95`.
  The GPU time comes from the existing observer phases.
- **Graphics log, once:** `reprojection depth: source dlDepth WxH, ui merged=1`.
- **Consumption** has no API. PimaxXR/VDXR ETW traces (`xrEndFrame_View`
  "Depth") prove only the hand-off; the proof is visual.
- **Visual test:** Smart Smoothing forced to half rate, parked, sway sideways
  10-20 cm. Watch the canopy strut against stars, HUD text and radar, and holo
  panels for wobble or tearing; key off, on, off. An optional one-shot dump of
  the submitted depth (the kept eye dump) checks its alignment with colour.

## 9. Work estimate

- **Stage 0+1 (about 2-3 days, one flight).** Enable the extension when
  advertised and log the caps; temporal table v2 exporting `dlDepth`; the depth
  transfer slot; the depth swapchains and SV_Depth pass; the chain, omission
  rules and key. Tests in `openxr_native_test` (fake runtime): the chain on both
  views or neither, the near/far/min/max values, the omission reasons, rebuild
  on resize. Fly on Sean's Crystal Super (Pimax OpenXR), and on a Quest Link
  tester if one exists. A VD flight would only show GPU spent for nothing; auto
  excludes VDXR.
- **Stage 2 (2-3 days).** UI-layer and menu depth, the weapon re-encode, replay
  depth, and a source when `fix.temporal_aa` is off (a probe-pick conversion
  pass from the D32S8 view).
- **Stage 3 (separate design).** `XR_EXT_frame_synthesis`: depth plus EDVR's
  existing motion vectors (`e.dlMv`), for runtimes that do motion-assisted
  synthesis (Valve's list).
