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
  - *Decided against, from the evidence:* the force field (a G-buffer draw: the pass compares its light contribution with the lit frame, which an albedo write
    breaks), the light volumes, `B75A6FF2`, and the particle pair (no velocity source, and a post-DLSS take would lose scene occlusion).
- **Built on `claude/effect-history` (option A):** where a listed glow supplies >= 0.35 of a pixel's light (and > 0.002 luma), the motion shader writes the
  out-of-range vector (`hidden`), so DLSS starts that pixel's history afresh. The sentinel is what preset K honours: flown on the background-uncovered case
  (`backgroundHistoryHidden`) and for masked engine pixels (temporal_shader_source.h `hidden`; dlaa.cpp:711 says K ignores the mask, not the vector). List: effect_history_pairs.h (particle-billboard 68DDDEF0/06332CA1, halo-billboard
  B75A6FF2/D56F859B; never the light volumes or the force field's G-buffer quad). Each listed eye draw is replicated before the game's, into a private R11G11B10 target
  (own blend, the game's depth view tested, nothing written to the game's targets, state restored), per eye, found by the scene depth it tests against; the pass binds
  it at t5 and the game's HDR target at t16 (probe.w bit 32768). Census every 30 s: `effect history: last 30 s ...`. No key. Inspector: `effect rule:` line,
  `HIDDEN(effect rule)`, magenta in path.png, CSV column `effect_hidden`. Tests: tools\effect_history_test (WARP, in build.bat).
- **Build B captures (14:55-14:56, Frontier):** all three boxes missed (head-forward, 180 px right of centre). Advert: no 4654/95D0 draw, no `holo=` route, but 117
  world-marker draws/frame in the census (pass live). Pad (cap2): 17 overlapping B75A6FF2 sprites (listed) plus 1C506222/2519C905 (additive, 384 indices, depth-only:
  a hologram-volume candidate, NOT listed). Railing: kind 4 on 256 of 256 box pixels (cap3, slot 354 MOVED) and 183 (cap2, slot 749; slot 829 exact, same pose delta).
- **Flat comparison (Sean, test build D, flat + DLAA native): pad holograms and railings are fine; both blur in VR (HMD Quality 0.5, 2016 -> 4032, K).**
  (2) The railing, from code: kind 4 = a marker whose depth is not the scene's. Flat's prep treats the same pixel differently (flat_mono_shader_source.h:142,
  flat_mono_resolve.cpp:972 "steady detail"): it REFUSES history unless last frame's depth confirms the camera term (`historyDepthMatches`, both directions, 1%).
  The VR mv shader gives kind 4 the camera term with no check; its `backgroundHistoryHidden` tests only a NEARER occluder that left, so a rail moving over
  farther background keeps the background's history. Supports (2) for the railing; a port of the check to kind 4 is the proposed fix, NOT BUILT. Holograms:
  nothing in flat's code treats them; (1) ratio and (3) placement (flat resolves HDR before post: flat_mono_shader_source.h route.x) stay open until Sean's HMD Quality 1.0 flight.
- **Open:** a pad-hologram capture that names its draws; an advert capture with the box on it; the railing's owning draw (kind 4: which draw wrote the depth the
  marker lost to); weapons fire, flares (option A covers the two sprite pairs, NOT FLOWN); the force field.
- **Ruled out:** railing motion mismatch (tool label bug, finding 1); the 2026-09-29 pad-into-layer rule as wired in 9122f31e; reactive bias or sharpening as
  a fix (docs/hologram-depth-2026-09-24.md "Ruled out"); the light-volume pairs as effects (finding 3); `B75A6FF2` as the pad hologram (finding 4); the second
  producer FUN_14369C9C0 as the railing's writer, because its hook (kinematic-build-14369c9c0) was installed and counted 0 calls since launch in all three captures.
- **Next flight:** `effect history:` census line (hidden px per eye-frame, covered px per draw, GPU ms per replica, `HDR target ... readable`); on a glow
  capture the box stats' `by the effect rule N` and the `effect rule:` line. A census saying the HDR target COULD NOT BE READ means the game's HDR target could not be bound
  or was not the eye's size at the pass. Aim with the reticle: press the inspector key once (a "+" shows at the head-forward point), put it on the element,
  press again within 3 s; the box is now 48 px (docs\scene-inspector-2026-10-14.md, journal).

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
