# Five elements that look indistinct under DLSS (2026-10-14)

## Status

- **State (2026-10-14, night):** the scene inspector (`docs\scene-inspector-2026-10-14.md`, `hotkey.scene_inspect`) flew seven captures on
  v0.20.0-rc.1-4-g66dfe94f and three more on test build B (v0.20.0-rc.1-14-geeb168e4, which has the advert and the railing instrument). `claude/blur-fixes`
  carries the inspector, the advert fix and the railing instrument. `claude/effect-history` (cut from it) adds option A, the effect history rule: BUILT,
  NOT FLOWN. The blend probe and its key stay until a flight confirms the inspector.
- **The five elements:** (1) the hologram over the landing pad; (2) the gantry railing while the pad moves; (3) the hologram adverts; (4) weapons fire;
  (5) the station "mail slot" force field. (The inspector's captures: cap1-2 railing, cap3 pad, cap4 force field, cap5 advert, cap6 weapons, cap7 another
  ship's engine flares.)
- **Environment:** DLSS preset K, 2016x2017 in, 4032x4034 out per eye, Elite supersampling 1.0, native OpenXR runtime, Frontier. K ignores the reactive mask
  (dlaa.cpp:711); what K honours is depth and motion, plus EDVR's post-DLSS UI resolve, which replaces covered pixels with the current raster.
- **Findings (PROVEN from the captures):**
  1. **The railing's 2 px "mismatch" was a tool bug, not a pass fault.** The decision texture's path 2 is the WORLD camera term (temporal_shader_source.h:910,
     `count15 ? 2 : 1`, count15 set only in the world branch), path 1 the head's. The inspector had them reversed, so a pixel that correctly took the world
     term read as 2 px off the ship term. Fixed in the tool; the blend probe's legend has the same error.
  2. **The railing is kind 3.** Its draws are keyed and substituted (DE545DC8EE4FBB87/E46E3E4832B2FDB0 rigid#3, EB5234DB6ADB491D/3434972DB5336AA4 rigid#0), yet
     its pixels read engine kind 3: the pool record has no marker from this frame and none within 64, so the camera term is used. Cap2: 153 of 256 box pixels
     (99 m); the 103 beside them, 115-125 m, are exact. The true error is likely small (about 0.2 px per m/s of elevator speed at 99 m); thin rails at a 2x
     upscale are sensitive to it.
  3. **0357BBB2DEE43C1F/81812EF97FB4A361, 8289669D93A18C1D/C6E6E419DA9F6FAD, A1B7CFCD0BE7493E/2DB678B6B558B604 are the deferred light volumes** (a 14-index
     box, an 84-index sphere or cone, smaller instanced volumes; gpu_pass_profile.cpp:68): lighting, not the force field or the flares. Never route them.
  4. **B75A6FF2CA9FA5D6/D56F859BE4781431 is a generic anchor billboard with a depth-occlusion sample** (flat doc line 1899), present at the view centre in
     caps 3, 5, 6 and 7 (7-9 overlapping sprites): a light halo or flare layer, not the pad hologram.
  5. **68DDDEF04D9894AF/06332CA168B6DA63 is a stretched, age-ramped lit particle billboard** (docs\shaders\particle-stretched-vs.asm), in caps 5 and 6: not
     weapon-specific.
  6. The force field in cap4 is most likely 124D7F3F649138D4/8085AE8DD1906CDC, an alpha quad over the G-buffer (rt0) above the hull, with no depth write; no
     draw in cap3 is identified as the pad hologram.
- **Built on `claude/blur-fixes`:**
  - *Railing, instrument only.* The inspector now reads back, per slot under the box, the pool record the pass indexed (rigid or skinned, STATIC or MOVED, the
    marker word, pose and previous pose), and prints the kinematic producers' appended-record totals per captured frame (emit FUN_144312E00, the second
    producer FUN_14369C9C0 which is counted and never stamped, the primary). One capture of the railing mid-cycle settles whether it is a mover no hook reaches.
  - *Adverts.* 46546443FD3C3F88 and 95D01BA609BF7500 joined the hologram depth pass's world markers (holo_families.h): their own depth, so the camera term
    gives them parallax. Mechanism: the pass stamps depth into the AA depth copy; it writes no mask.
  - *Decided against (superseded below for the particle pair):* the force field (a G-buffer albedo write), the light volumes, `B75A6FF2`; a post-DLSS take (loses occlusion).
- **Built on `claude/effect-history` (option A):** where a listed glow supplies >= 0.35 of a pixel's light (and > 0.002 luma), the motion shader writes the
  out-of-range vector (`hidden`), so DLSS starts that pixel's history afresh. The sentinel is what preset K honours: flown on the background-uncovered case
  (`backgroundHistoryHidden`) and for masked engine pixels (temporal_shader_source.h `hidden`; dlaa.cpp:711 says K ignores the mask, not the vector). List: effect_history_pairs.h (particle-billboard 68DDDEF0/06332CA1 alone since
  build D; the halo B75A6FF2 was REMOVED, see Ruled out; never the light volumes or the force field's quad). TEMPORARY key `advanced.effect_history = on|off` (default on, live)
  A/Bs the rule in one session; remove it when the arc closes. A draw a fix substitutes (verdict, UI layer, engine substitution) is never replicated, and counted. Each listed eye draw is replicated before the game's, into a private R11G11B10 target
  (own blend, the game's depth view tested, nothing written to the game's targets, state restored), per eye, found by the scene depth it tests against; the pass binds
  it at t5 and the game's HDR target at t16 (probe.w bit 32768). Census every 30 s: `effect history: last 30 s ...`. Inspector: `effect rule:` line,
  `HIDDEN(effect rule)`, magenta in path.png, CSV column `effect_hidden`. Tests: tools\effect_history_test (WARP, in build.bat).
- **Build B captures:** boxes missed. Advert: no 4654/95D0 draw (pass live: 117 world markers/frame). Pad: 1C506222/2519C905 (additive, depth-only) a candidate, NOT listed. Railing: kind 4 on 256/256 and 183 px.
- **Flat comparison (Sean, test build D, flat + DLAA native): pad holograms and railings are fine; both blur in VR (HMD Quality 0.5, 2016 -> 4032, K).**
  (2) The railing, from code: kind 4 = a marker whose depth is not the scene's. Flat's prep treats the same pixel differently (flat_mono_shader_source.h:142,
  flat_mono_resolve.cpp:972 "steady detail"): it REFUSES history unless last frame's depth confirms the camera term (`historyDepthMatches`, both directions, 1%).
  The VR mv shader gives kind 4 the camera term with no check; its `backgroundHistoryHidden` tests only a NEARER occluder that left, so a rail moving over
  farther background keeps the background's history. Supports (2) for the railing; a port of the check to kind 4 is the proposed fix, NOT BUILT. Holograms:
  nothing in flat's code treats them; (1) ratio and (3) placement (flat resolves HDR before post: flat_mono_shader_source.h route.x) stay open until Sean's HMD Quality 1.0 flight.
- **Kind-4 depth check (flat parity, NOT FLOWN):** a kind-4 pixel keeps its history only where last frame's depth at the camera term's position confirms the
  predicted depth within 1% (4 texels, either way); else hidden. Kinds 1, 2, 3, 5, 6 untouched. Expect on the railing: box stats `by the kind-4 depth check N` > 0,
  `HIDDEN(kind4 depth)`, orange in path.png; the 30 s `kind-4 depth check:` line tens of px per eye-frame with a moving pad, near none still (high on a still scene = refusing real surfaces).
- **Open:** a pad-hologram capture that names its draws; an advert capture with the box on it; weapons fire, flares (option A, NOT FLOWN); the force field.
- **Ruled out:** railing motion mismatch (tool label bug); the 2026-09-29 pad-into-layer rule (9122f31e); reactive bias or sharpening (docs/hologram-depth-2026-09-24.md);
  light-volume pairs as effects (finding 3); `B75A6FF2` as the pad hologram (4); FUN_14369C9C0 as the railing's writer (hooked, 0 calls since launch);
  B75A6FF2 as an effect-only pair (~7M px/draw, the sky; flight d3013197); 68DDDEF0 as a weapons-only pair (constant ~200k px/draw background particles; hiding them
  shimmered the stars, flight 40b6dbc0). Option A has no safe pair left.
- **Next flight:** rim: `fix.sun_glare = off` against vivid (does the rim shimmer persist?), then HMD Quality 1.0. Aim: press the inspector key once (a "+" shows at
  the head-forward point), then again within 3 s; the box is 48 px. Option A: `effect history:` census line, key `advanced.effect_history`.

## Triage

Read-only, against origin/main 7e44b86c and `claude/flight-2026-10-14`. Source cites are line numbers in those trees.

What EDVR gives DLSS today:

- **The bias-current-colour mask** is handed over at dlaa.cpp:704-711, built in temporal_pass.cpp from the UI coverage mask plus
  engine-masked pixels. Preset K ignores it, so the reactive setting is inert on K.
- **What marks it** (ui_depth.cpp, `g_reissueMaskSlot`): the interface composites, the holo material's markers, the flight HUD strokes.
  Selection is by shader hash after the draw ladder in vscreen.cpp, not by blend state. There is no generic blend-based selection.
- **Smoke** is the only world transparent family with its own treatment: a coverage replica writes depth where alpha is at least 0.08
  (`kSmokeFloor`), so the dense core has its own depth and motion. Each new family needs its own pixel-shader replica.
- **The generic hologram pass** takes ten cockpit families (radius-clipped to 10 m) and one world marker, the reticle.
- **The post-DLSS UI resolve** (ui_resolve.h) is the real reactive path for K; **the UI layer** (`fix.ui_quality`) draws admitted families
  after the upscale and already admits `94D5C556DFD6D705`.
- **Engine-record velocity** (`engine_velocity_families.h` `kFamilies`) gives exact motion to keyed pairs with certified previous poses.
  Weapon motion is the on-foot first-person weapon only, not ship weapon fire.

| # | Element | Category (INFERRED) | Test | Fix route if confirmed |
|---|---|---|---|---|
| 1 | Pad hologram over the pad | translucent or additive, no depth; maybe the shared sprite pair | does the draw reach the window; routed to the layer or left in the frame; depth and motion of its pixels | per-draw discriminator (instance shape or range), never the hash alone: the pair is shared with the glare, radar contacts and the hangar sprite |
| 2 | Gantry railing in motion | opaque mover on the camera term | grid `p` = 11 and `e` = 1 on the rail, or `p` = 1/2 | key the railing's VS/PS in `kFamilies` and confirm the record certifies; touches the optimized-station veto, and a bad key caused the 2026-09-29 plating crawl |
| 3 | Hologram adverts | animated texture on a world quad | same as 1, still versus moving | shared with 1 and 5 once the hash is known; may share `81216C77` with the cockpit material |
| 4 | Weapons fire | thin fast additive geometry, no velocity source | row present while firing at a dark target | per-family coverage or layer route; no per-projectile velocity exists |
| 5 | Mail slot force field | scrolling additive texture, like the drive smoke | row present; grid depth equal to the far background's | the smoke recipe (coverage replica, depth floor, mask) or the layer route |

Order: one instrument flight, then G1 starting with 1 and 3, then 5, then 4, then G2. Not recommended: a global transparent mask (K ignores
it), sharpening or contrast tweaks.

## The instrument: `hotkey.blend_probe`

Log only. It changes no draw, no persistent state and no pixel. Unarmed, each eye draw pays one relaxed atomic load, the temporal pass one
more per frame. Press it with the element at the centre of the view.

A press arms a capture; the next frame boundary starts 3 consecutive frames; the answers are read back over the following frames
(up to 120 boundaries) and the report is written.

**Rows.** An eye draw is a candidate when it writes colour and is blend-enabled or writes no depth. Eye 0's draws are tested (eye 1 is
counted and skipped; `eye=-1` means the pass's eye table could not tell, and then both eyes' draws appear). A candidate is tested by a
scissored duplicate of the draw (the look probe's method, from `claude/hangar-test-4` look_probe.cpp) that writes no colour, depth or
stencil, bracketed by an occlusion query; its `cov=` is the samples that pass the draw's own depth test and survive its shader, inside the
96x96 window at the viewport centre. A candidate with `cov=0` is not listed. Draws with a UAV or stream-output target cannot be duplicated
and are listed with `cov=?` and counted; indirect draws are counted, never tested. At most 200 rows a frame are listed, the last 200 in
issue order (later draws land on top); the rest are `truncated`.

Row columns (`blend probe row:`):

- `f` captured frame 0..2; `gf` the game frame; `#` the draw's place among the frame's eye draws; `eye`.
- `vs` `ps` the 16-hex shader hashes; then the draw kind (D, I, N, X), `n` vertex or index count, `inst` instances.
- `blend=[src,dst,op/srcA,dstA,opA wm=mask]` (or `off`), `depth=[test FUNC write 0|1]`, `rt=` format and size.
- `cov=` covered samples in the window, `?` when unmeasured.
- `verdict=` EDVR's draw verdict (`none`, `skip`, `particle`, `glare-steady`, ...).
- `ui=FAMILY/DECISION`: the UI layer's family for the draw (`-` if none) and what it decided: `taken`, `left:REASON`, or `none` when no
  decision ran. `left:redirect` is the VR world route re-issuing a draw it did not take.
- `holo=cockpit|world|-`: the generic hologram pass's list that matched.
- `mask=SLOT:mF/A`: the ui-depth coverage family reissued this draw into private depth and marked the reactive mask; F and A are the
  strength in 1/255 quanta for a floating and an attached stroke (ui_depth.cpp `floorBuffer`: the holo material and the flight HUD write 1
  and 2, the others 1). `SLOT:nomask` is coverage without the mask; `-` is neither.
- `kf=`: the pair's place in `kFamilies`: `rigid#N`, `skinned#N` (N the family index), `self-marking`, `vs-only#N` (the vertex shader is
  keyed, this pixel shader is not), or `-`. `eng=sub|plain`: the pool pixel shaders were substituted for this draw.

**Grid.** The temporal pass stages a 96x96 crop of eye 0's decision texture (physical motion, decision flags), depth copy, coverage mask
and bias mask after the motion dispatch, using its existing capture-only decision shader variant (the production motion output is
unchanged). The crop is read back at the next frame boundaries and sampled at 8x8 cell centres (pixels 6, 18 ... 90). Lines:
`blend probe grid legend:` (once), `blend probe grid f=N:` (window, the pass's camera term: delta rotation in degrees and translation,
which inputs were staged), eight `blend probe grid f=N row J:` lines of `[z mv p e c b]` cells, and `blend probe grid f=N summary:`.

The summary is the railing answer: the median motion over all cells, over camera-path cells (`p` 1 or 2) and over engine-path cells (`p`
11), the difference of the last two, and a histogram of engine kinds. A moving railing with `p` = 1/2 is on the camera term alone. One
with `p` = 11 and `e` = 1 has exact per-object motion. The pass's camera term is reported as rotation and translation rather than a
per-pixel value because the decision texture has no room for it (a second capture shader variant would be a bigger change).

Other lines: at install `blend probe: off; hotkey.blend_probe is bound to KEY` or `... is NOT bound -- add "blend_probe = <a free key>" under [hotkey] in edvr.ini to use it` (check it before flying); `armed (trigger ...)`; `collecting 3 frames from game frame N`; per frame
`blend probe: frame f=N (game frame G): E eye draws seen (...); C candidates ..., T tested, K cover the window, L rows listed, truncated X;
not tested: ...; M answers missing`; `blend probe: captured 3 frames, R rows (G of 3 motion grids staged)`. Refusals read
`blend probe: refused (REASON)`: not VR, a capture already running, no eye pass seen, occlusion queries unavailable, some queries
unanswered, no motion grid (with the pass's reason). Zero rows is therefore never silent: the frame line says whether no eye draw
arrived (a menu, no VR), none was a candidate, or none reached the window.

## Flight plan

1. Set `fix.particle_billboard = stock` in `edvr.ini` (the live ini is Sean's: edit it with the Edit tool, never a regex). The probe offers
   each draw before any verdict substitutes its shaders, so it would see the game's hashes either way; stock removes the one other
   difference, the billboards' orientation, from the picture being judged. Bind `hotkey.blend_probe` to a free bare key, for example F3 (avoid keys the game uses).
2. For each element in the order 1 to 5, look straight at it so it sits at the centre of the view, press the key once while still, and
   once more (after the report, a second or so later) while moving the head. Note the order; the log numbers the captures by time.
3. Element 1: stand in the station hangar with the pad under the hologram. Element 3: adverts inside a station, and the two rows outside a
   large station. Element 4: hold fire at a dark target and press during the burst. Element 5: at the mail slot.
4. Element 2: press during the pad's raise and again during its lower, with the railing at the centre of the view.
5. The `temporal_aa_debug = motion_source` view of the railing cannot be taken on this build (see Status). The grid's `p` and `e` columns
   are its replacement.
6. After the flight: `python tools\edvr_log.py --target steam --expect-build HEAD --version`, then read the `blend probe` lines.

## Reading the output

- The row to look for in G1 is a blend-enabled draw with `depth=[... write 0]` that reaches the window with `cov` in the hundreds or
  more. Its `vs`/`ps` name the family; `ui=` and `holo=` and `mask=` say which of EDVR's routes already touch it.
- A row with `ui=...left:...` and `mask=-` and `holo=-` is in the game's frame with nothing telling DLSS it is a UI-like effect.
- The grid's `z` over those cells: equal to the far wall's depth with the wall's motion means the effect inherits the background (G1's
  mechanism). The moving head presses should show the wall's motion `mv` on those cells, and it should differ from the still presses.
- G2: `p` 1 or 2 on the railing cells while the pad moves means the camera term alone.

## Journal

- **2026-10-14, build.** `blend_probe_core.h` holds the window, the candidate rule, the text and the grid indexing; `blend_probe.cpp`
  the device side. Hooks: vscreen.cpp `forwardWithVerdict` offers each eye draw after its routing is decided (and at the skip return),
  `drawGateSubscribed` includes the probe so the verdict ladder runs and the AA-off fast path is not taken while it collects,
  `vScreenFrameBoundary` ticks it; temporal_pass.cpp asks for the capture-only decision shader and offers the staged crop after eye 0's
  motion dispatch (and releases the 65 MB decision texture once no eye run or capture wants it); ui_depth.cpp `uiDepthDrawProbe` and
  ui_layer.cpp `uiLayerDecisionCount` expose per-draw state read-only. The full build is green with the rig in its gate; nothing was
  installed or flown. The probe is VR only; a flat press is refused with a reason.

- **2026-10-14, first inspector flight and the fixes.** Seven captures, read against the draws, the pass's shader and the earlier engine-motion arc.
  The railing's "mismatch" was the tool's reversed path labels. The railing's pixels are engine kind 3 (see finding 2). docs\kinematic-motion-injection-2026-09-19.md
  (2026-09-28, "061832 braces") met the same signature on the Coriolis station's crossbraces and added the primary producer (FUN_1442B4130) and the private-pool
  certificates; the 13:23-13:25 windows of this flight show the primary running (relay calls 11573-20266, joined, moving 0-1) and the emit's masked-for-gap
  counts in the tens of thousands, so the railing's records are neither: the open candidate is the third producer FUN_14369C9C0, hooked and counted by
  `directBracket` but, unlike producer 0, given no emit observer. That is a hypothesis; the instrument above names the producer in one capture.
  The adverts were added as world markers on the evidence that the two pairs cover the whole box over bare sky in the HDR target that shares the scene depth,
  with no route, and sit with the target sprite and the reticle in flat_projection_recipes.h's dp4 b0 4..7 group. Their log signature after the fix:
  `holo=world`, `HOLO_PASS`, a finite depth under the box. Decisions without code are in the Status block (findings 3-6).

- **2026-10-14, option A: the effect history rule.** The check first. History-hide is the out-of-range motion vector (2 x the frame size) the motion shader
  writes for `hidden` pixels (temporal_shader_source.h, the `hidden` bool and `written = hidden ? float2(size)*2 : motion`); decision flag bit 16 is the
  inspector's "NVIDIA history hidden". It is what the other hidden cases already use on K, and K ignores only the reactive mask. Cost per pixel: one vector
  select; this rule adds two texel loads on the eye-frames an effect drew (nothing otherwise) and a group atomic for the census. The replica costs the listed draw
  once more (the census prices it).
  Design: replica into a private R11G11B10 target (blend and shaders as the game's, the game's depth view with writes off, so a glow behind a wall counts for
  nothing), the pass compares it with the game's HDR target, `effectHideTest` in effect_history_core.h (one HLSL definition, spliced into the shader and compared
  with the C++ rule on 4000 cases in the rig). Threshold: the effect supplies at least 0.35 of the pixel's light, i.e. E >= 0.54 U: enough that its own motion
  decides the pixel; below it the scene dominates and its vector is right, and a faint halo around a lamp keeps DLSS's anti-aliasing. Floor 0.002 luma: a black
  pixel's ratio is noise. Named constants, no key.
  Found while building: the pass's eye table does not know the lit HDR target until late in the frame (962 of 1244 draws read eye=-1 in cap1), so the module
  keys each private target by the scene depth texture the draw tests against, which is also what the pass reads per eye. The pass unbinds its render targets before
  it reads (a resource still bound as a target reads as null; the rig's first run proved it), so t16 is readable there.
  Not verified offline: that the game's HDR target still holds the lit frame when the pass runs. The census says `COULD NOT BE READ` only for a bind failure; a
  wrong-content target would hide every pixel with any glow. The first flight's `pixels hidden` figure against `covered pixels` shows it (hidden near covered =
  the share test is not thinning).

- **2026-10-14, the kind-4 depth check.** Sean's flat/DLAA comparison (pad holograms and railings fine in flat, blurred in VR) led to the flat resolver's rule for a
  stale marker (flat_mono_shader_source.h:142 `engineBefore`; flat_mono_resolve.cpp "steady detail", `historyDepthMatches`): a pixel whose marker's depth is not the
  scene's takes the camera term only where last frame's depth confirms it, else its history is refused. The VR motion shader returned kind 4 with the camera
  term and no check (`backgroundHistoryHidden` only catches a nearer occluder that left). Ported as `staleMarkerDepthHidden` (temporal_shader_source.h), applied
  to `engineKind == 4u` pixels off the foreground and UI paths: the four ZP texels around the previous raster position (`p + motion - holoJitter.xy`), expected depth
  `knobs.x + knobs.z / zPred`, tolerance `max(1e-6, (expected - knobs.x) * 1%)`. A pixel it cannot judge (no previous depth, no predicted depth, off the image)
  is left as it was, where flat refuses: a deliberate difference, so the VR change cannot hide more than the flat rule would. The pure rule is stale_depth_core.h;
  the shader carries a literal copy and the rig fails if it differs (the tools that read the shader's raw literals cannot follow a macro). Counted in a group
  atomic to Stats[41], read with Stats[40] by effect_history.cpp's reader: the `kind-4 depth check:` line every 30 s, and `by the kind-4 depth check` in the
  inspector's box stats (decision value 65536, CSV column `kind4_hidden`). Tests: effect_history_test (the rule against its HLSL on 1024 cases; the production
  wrapper extracted from the shader and run over a real previous-depth texture: confirming keeps, non-confirming hides, three cannot-judge cases keep). No shader
  golden was re-pinned by this change; the stale-marker rule is new text, and the only pin touched is the terrain A/B rig's reading of the shader, if the build shows one.

- **2026-10-14, the halo pair removed from option A (flight d3013197, build E, VR, Pimax).** Sean reported shimmer with DLSS on, the skybox shimmering and the sun
  billboard appearing to ride his head (sun_glare = vivid). The 30 s census from 20:02 said `halo-billboard: 1.82-2.00 draws/frame, 5.6-7.2 million covered pixels per
  draw` and `pixels hidden: 10-14k per eye-frame`. ruled out: B75A6FF2/D56F859B as an effect-only pair, because it covers about 7M px per draw (the sky: stars, the sun
  billboard, glare elements) and hiding it made the skybox shimmer. My reading of the build B pad capture ("17 overlapping sprites at the centre, so a halo layer")
  was wrong: that capture's box coverage (4090 samples of 256) was the first sign of a sky-wide draw. The lesson is now in effect_history_pairs.h: a pair is
  judged by its covered pixels per draw before it is listed. Now: the list is the particle billboard alone; the TEMPORARY key `advanced.effect_history` (default on,
  live) A/Bs the rule in one session; and a draw a fix substitutes or takes (any non-none verdict: sun glare clamp/steady, particle, holo, ...; the UI layer; an
  engine-record substitution) is never replicated and is counted per pair (`N/frame left alone (a fix substitutes them)`). Checked against the sun glare: the replica
  runs only for a verdict-none draw, before the game's issue and through the real entry point (below every EDVR hook), so it cannot consume the glare wrapper's per-draw
  substitution; what it did on build E was hide history under the sun's pixels. Consequence, said plainly: with the default `fix.particle_billboard = steady` the
  particle pair's draws carry a verdict, so the rule is inert by default until a flight shows the count and a replica inside that wrapper is justified. The kind-4
  depth check read 2-99 px per eye-frame on that flight: not a plausible cause of sky shimmer, and not part of the new key.

- **2026-10-15, the particle pair removed in effect, and the cockpit rim capture (flight 40b6dbc0, build G).** With `advanced.effect_history = off` the skybox stars stopped
  shimmering; on, the census read 68DDDEF0/06332CA1 at 2.00 draws/frame, 180-224k covered pixels per draw, 6-7k pixels hidden per eye-frame, all the time: background
  particles (space dust, star sprites). ruled out: 68DDDEF0 as a weapons-only pair, because it is constant ~200k px/draw background particles; hiding them made the skybox stars
  shimmer. The state cannot tell the two apart: every captured draw of the pair (13:25 weapons fire, 13:26, and this 04:54 capture) has the same blend (SRC_ALPHA,ONE),
  depth (GREATER_EQUAL, no write), four textures (6x1 buffer, 128, 256, 128) and 8733-9339 instances of 12 indices (a pooled system, most instances dead), so a per-draw
  or per-state rule cannot separate bolts from dust; only emitted brightness and per-instance motion can. Rim capture (cap1, 04:54:04, key off): the rim is the cockpit shell
  BFE51414CC3024B4/DB79AE788E049DFD (#6 top at the centre, #10 and #11 partial) with EB5234DB/B7D50283 (#21), no verdict, no key, camera-row motion; 1344 box pixels path 1
  (head delta, 2.5 m), 960 path 2 (sky); engine kind none on all 2304, hidden 0, kind-4 0, effect 0. mv against expected: ship path within 0.031 px of expect_ship, sky within
  0.0003 px of expect_world; the layers differ by 0.025 px. So the rim is not a motion error, not the 10 m split (2.5 m), not kind-4. The one translucent draw over it that
  moves differently is #250, VS 94D5C556DFD6D705 (the sun-glare train, verdict glare-steady, additive, depth test off, 15 instances, 5 layers at the centre), also taken as
  `holo=cockpit` by the generic hologram pass, with no pixel on path 8.
