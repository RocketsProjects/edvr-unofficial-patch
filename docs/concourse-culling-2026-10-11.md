# Concourse culling and sealed hangar (2026-10-11)

Offline analysis of `EliteDangerous64.exe` build 332841 (TimeDateStamp 1788384820, SizeOfImage 104894464, base 0x140000000; gate matched). RVAs are image RVAs. No C++ changed, no build, no flight. **Proven**: exe bytes, EDVR source, or key names from the user's own journal. **Inferred**: not closed by the bytes. The scratch tool `xr.py` (capstone and numpy xrefs) is not committed.

## Status

- **Built v13.9 (`claude/hangar-test-9`, commit 02abe9e3, DLL v0.19.1-115-g02abe9e3; full build green (hatch_geometry_test 71 checks, sealed_trial_test 232 checks), `--dll-only` promotion green, pushed; not installed, not flown):** fixes the two trial bugs the v13.8 medium flight (edvr_gfx_20261012_183650.log) exposed, plus the probe's bay and shell. (1) The own pad is sticky: a near-tie within 5 m goes to the pad whose up line is nearest the camera laterally, then to the smaller name; the held pad stays while its record is seen, and another replaces it only after 30 unseen frames. (2) The reference shell is chosen once at the freeze (HangerSmall/Medium/Large matching the pad, never Gate, Ramp or Skirt) and re-read by address; if it disappears the release stands down (logged), it does not switch. (3) The probe logs each record's bay (inside or outside the held cell's box). See "Built v13.9".
- **Built v13.8 (`claude/hangar-test-9`, commit a4fd9f81, DLL v0.19.1-113-ga4fd9f81; full build green (hatch_geometry_test 67 checks, sealed_trial_test 232 checks), `--dll-only` promotion green, pushed; not installed, not flown):** the hatch probe (hotkey.hatch_probe, F7) expresses each record in the own pad's frame (centre at the origin, up = axis 2), reads world centre and axes with worldOfRecord and poseOrientOf, logs change and settled lines only, logs a pad line (height above the nearest Hanger/Hangar), and writes a lower-cycle summary of the records that move after the pad settles at the bottom. See "Built v13.8".
- **Built v13.7 (`claude/hangar-test-9`, commit 9a8e40e1, DLL v0.19.1-111-g9a8e40e1; full build green, sealed_trial_test 232 checks, gpu_pass_profile_test 72 checks; not installed, not flown):** the held cell is frozen in the held PAD's own frame and rebuilt from the pad's live pose before every judgement (the v13.6 medium's doors fell out after a 34 m frame shift); the pad's motion along the shaft against the hangar shell releases the cell (more than 5 m); the window line carries the frame shift and the shell offset. The GPU pass profiler addendum, in the same build: compute shader hashes per pass (top_cs_dispatches), depth-only maps off the eye size are shadow, a hud_overlay class from the game's HUD families, and medians beside the means. See "Built v13.7".
- **Built v13.6 (`claude/hangar-test-9`, commit 101708de, DLL v0.19.1-109-g101708de; full build green, gpu_pass_profile_test 54 checks, gpu_disjoint_clock_test 192 checks, sealed_trial_test 222 checks, config contract 170 keys; not installed, not flown):** while a GPU pass profile capture is armed or running, the application-frame timing and the census timers yield the shared clock (no new key): an application frame open across the profiler's boundary is ended there, the frames after are not opened (counted, logged once per capture), and the profiler's frames open. See "Built v13.6" and the profiler doc's "The yield (v13.6)".
- **Built v13.5 (`claude/hangar-test-9`, merge commit 9e910077, DLL v0.19.1-107-g9e910077; the gpu-pass-profile branch (0cdf493c, on origin/main 2d993d2b) merged in; full build green, sealed_trial_test 222 checks, gpu_pass_profile_test 46 checks; not installed, not flown):** the merge changes no behaviour. The profiler and the trial share the DisjointClock only through the census and the application-frame timing; the trial and the drawlist probe take no GPU timing. See "Built v13.5" and the profiler doc's "Interaction".
- **Built v13.4 (`claude/hangar-test-9`, commit 3a6ea80a, DLL v0.19.1-101-g3a6ea80a; full build green, sealed_trial_test 222 checks, not installed, not flown):** the floor is per pad size (small 85, medium 120, large 145 m; diag.sealed_cell_below_small_m / _medium_m / _large_m; diag.sealed_cell_below_m overrides all three when set), from the measured lift doors (small -72, medium -104, large -126 m) plus margin; the engage logs the deepest lift or door record within 60 m lateral from the previous frame; each engage (and so each re-freeze) logs the pads of the held size with their centres and axes. See "Built v13.4".
- **Built v13.3 (`claude/hangar-test-9`, commit c5815b6f, DLL v0.19.1-99-gc5815b6f; full build green, sealed_trial_test 218 checks; FLOWN 2026-10-12 (edvr_gfx_20261012_154838): small and large bays fully correct, the re-freeze on ship swaps worked, the medium's back elevator section (TurboLiftDoor at -103 to -104 m) missed by the 85 m floor; not installed):** the below distance defaults to 85 m (measured: the small hangar's lift doors at -72 m); the held cell drops after 30 frames of its pad record being absent, or of another own pad being nearest, and re-freezes from the next pad once lowered (lowest height per pad record); an unreadable centre is kept in named mode (a zero raw centre was being read as the origin and culled); the culled tops are their own "sealed culled top:" line; the culled-near-axis list reaches 90 m. See "Built v13.3".
- **Built v13.2 (`claude/hangar-test-9`, commit 4e826a7c, DLL v0.19.1-97-g4e826a7c; full build and dll-only promotion green, pushed; FLOWN 2026-10-12 with 85 m (logs edvr_gfx_20261012_143721 and _145059): the small hangar fully correct; the medium and large hangars judged against a stale cell, see v13.3; not installed):** the box bottom and the bay's lower bound at 60 m under the pad (diag.sealed_cell_below_m; the flight's on-foot culled parts sat 11 to 15 m below a 10 m bottom); the own record is named as the type prefix without its underscore, and every record whose centre lies within its live sphere plus 5 m is kept; the engage logs the minima along the shaft of the own ship's and the Door* records; the neighbour excludes the own pad's second platform; the culled-name tallies print each record's radius and child count. HangerLarge and Floor00 are not identified by the data available here. See "Built v13.2".
- **Built v13.1 (`claude/hangar-test-9`, commit 2a7cfdf3, DLL v0.19.1-95-g2a7cfdf3; full build and dll-only promotion green, pushed; FLOWN 2026-10-12 (edvr_gfx_20261012_141*.log): the collar is back on foot with a slight gap; on foot the own ship and the bay's clutter are still missing, see v13.2):** automatic kept and culled-near-axis reports (2 s after a named engage; 10 s on foot), the window's top culled names, the bay rule (the own hangar's parts kept within 60 m of the shaft), the live camera line (on foot the source is the view 0 origin; its motion is now logged), the rig decision with no timer and one probe per rig per frame (early_ms now times the frame tick only), and the axis log at the engage only. See "Built v13.1".
- **Built v13 (`claude/hangar-test-9`, commit 8ffe2995, DLL v0.19.1-94-g8ffe2995; full build and dll-only promotion green, pushed; FLOWN 2026-10-12, log edvr_gfx_20261012_131749.log: the named engage repeats, the own ship is kept, the early veto matches every record via the job path, faults 0; on foot the bay still loses parts, see v13.1):** the own ship is never culled in the sphere or named modes (only mode all culls it); the named cell freezes only from a lowered pad (within 15 m of the lowest height seen); the shaft target is the BlastShieldBase nearest the pad's e2 line, both distances logged and rig-checked; path D (FUN_14434D470) is a count-only hook with no veto; the render-thread CPU profiler is already on this branch. Which path draws engine trails and lights is NOT established. See "Built v13".
- **Built v12.1 (`claude/hangar-test-9`, same branch; built and pushed, not installed, not flown):** the box's axis is the pad-to-BlastShieldBase direction (margin default 10 m; the pad's e2 angle is logged); the early veto's association is by the rig's thread-local evaluation (synchronous path) and the store batch's collection (job path, bucket[2] - 0x300); the verdict is precomputed once per frame, so a decision is one lookup; culled-near-axis named records are listed in the report. Match rates are for the flight. See "Built v12.1".
- **Built v12 (`claude/hangar-test-9`, commit df9980c0, DLL v0.19.1-91-gdf9980c0; full build and dll-only promotion green, pushed, not installed, not flown):** the early rig veto at 0x431AFE0's entry (Elite's own off-screen branch, performed in place of the body; a rig is vetoed when its records' bound lies wholly outside the named cell; `diag.sealed_trial_early` count or veto, `hotkey.sealed_trial_early` toggles it live). The named cell now stands on the pad's up axis and is frozen at the named engage (v11 flight fixes). Rig identity: the record's +0x2B8 is the rig link; the match rate is logged, not yet read. See "Built v12".
- **Built v11 (`claude/hangar-test-8`, commit 3aa231ca, DLL v0.19.1-87-g3aa231ca; full build and dll-only promotion green, pushed, not installed, not flown):** named mode's cell is an ORIENTED BOX along the shaft (the pad's own orientation and extent; wiki pad sizes, doubled on `diag.sealed_cell_double` (length, a guess), plus `diag.sealed_cell_margin_m`), with the v11 capsule as its fallback. One world centre for every named position. PAGEUP/PAGEDOWN step the margin (-10 to 40 m). Path C is counted in the F11 probe; path D is not hooked and not measured. See "Built v11".
- **Early veto (RE, 2026-10-12, `claude/early-veto-re`, offline, nothing built, installed or flown):** the earliest safe skip is the rig eval 0x431AFE0 (RVA 0x431AFE0, already hooked as `kinematic-rig-eval`). The veto runs the engine's own off-screen branch (three field writes, PROVEN from bytes) and skips for that rig the view loop, the batch dispatch, the store (pose update, sphere, LOD), producer B, the builder and the sub-object integrator. Open before a build: the rig's name and position reads (candidates rig+0x20, +0x50, +0x190, unproven) and the caller of the rig slot (not located). See "Early veto (RE, 2026-10-12)".
- **Built v10 (`claude/hangar-test-8`, commit 6d348fe5, DLL v0.19.1-85-g6d348fe5; full build and dll-only promotion green, pushed, not installed, not flown):** the trial's veto moves to producer B's entry (FUN_144312E00, pre-forward, on the existing producer-0 bracket). Veto form: SKIP the forward (every write B makes to shared state is in its append branch; the store ignores its return; decomp_4312E00.txt, Built v10). The decision uses the store's rule on B's world sphere (storeWorldSphere, decomp_4312040.txt lines 212-262). `diag.sealed_trial_producer = veto` (default) or `count` (the A/B control). Window: producer_b_calls, producer_b_vetoed, producer_b_kept, world_sphere_store_style, world_sphere_unreadable. Flight: raised pad and sealed hangar, off, sphere, named, all, with F11 and F6; expect the screen to empty under all.
- **Built v9 (`claude/hangar-test-8`, commit 7cb314d5, DLL v0.19.1-83-g7cb314d5; full build and dll-only promotion green, pushed, FLOWN 2026-10-12: H-PERSIST REFUTED, see Built v9; the bulk path is producer B):** the draw-list probe, `hotkey.drawlist_probe` (TEMPORARY, unbound by default, flat allow-list, commented in the edvr.ini template). Collects three consecutive frames of path A (builder) and path B (producer FUN_144312E00) appends, the pool copy, merge and clear calls, and the substituted pool draws per family, with carried candidates per frame; writes `edvr_logs\drawlist_<stamp>.csv`. The per-draw join is NOT live (the CPU has no draw-to-pool-key map). Tests H-PERSIST: see "Built v9". Flight: raised pad, same view, probe off, then sphere, then all; then the sealed hangar; one capture with a ship in view.
- **Built v8.5 (`claude/hangar-test-8`, commit 0d55bf4d, DLL v0.19.1-79-g0d55bf4d; full build and dll-only promotion green, pushed, not installed, not flown):** diagnosis of the v8.4 flights (see "Built v8", v8.5). Clear audit: only `sealed_trial.cpp` writes rec+0x208 and rec+0x234, and the write lands before the record loop reads them; unchanged since 7fed41c8. New: `diag.sealed_trial_mode = all` (EVERY visited record loses its flags, no test; the discriminator); F2 keeps the mode (code never resets it); the F9 summary names the live builder path and the bracket's gate; the window and report carry the kind of the pointer at rec+0x290. Next flight: mode all with the census (F6) and a screen capture, then the F9 summary line.
- **Built v8.4 (`claude/hangar-test-8`, commit 0be37478, DLL v0.19.1-77-g0be37478; full build and dll-only promotion green, pushed, not installed, not flown):** the record's centre is the builder's pose-derived centre (from `*(rec+0x290)`), with `rec+0x240` as a counted fallback. INFERRED until flown: `rec+0x240` reads exactly the origin for many distinct records, so the pose centre is the culling centre. Next flight: the trial in named mode, reading `zero_centre=`, `pose_fallback=` and `centre_dist_m` (see "Built v8", v8.4).
- **Built v8 (`claude/hangar-test-8`, commit 7a466ee3, DLL v0.19.1-67-g7a466ee3; full build and dll-only promotion green, not installed, not flown):** three TEMPORARY instruments (see "Built v8"): the builder probe on 0x42B4420, the hatch-state probe on the docking object O, and a named record rule for the sealed trial. The builder's arguments are settled (six, the return unused); skip is REFUSED because its side effects are not proven absent.
- **State:** analysis and design. Built v7 (`claude/asset-census`, full build green, not installed, not flown) proves the record's identity chain to its fields (C1 to C5, see Built v7) and adds a one-frame name census, `hotkey.asset_census`, that groups the records by that chain. M1 (the draw side) is still open. Flight 7 flew the sealed-hangar trial at 80 m in the docked hangar: nothing missing (see Measurement). Built v6 on `claude/hangar-test-7` (see Built v6): the box is ruled out; the cull is fault-safe and the window line counts `torn=`; the step census and the approach flight are not flown yet. Nothing fixed. M1 is blocked on draw identity (see Measurement).
- **Primary: sealed hangar.** Docked, pad lowered, doors closed. Submit only the hangar cell and suppress the sun. Blocked on storage of pad and door state (P9).
- **Secondary: closed panes.** Seated inside a station (doors open, or flying inside). Cull the SocialSpace (concourse) records and replace the bay windows with mirror-like opaque glass (stage 1, Sean's decision). Impostor is a later stage only.
- **Concourse:** the SocialSpace sub-area under StationInteriorHub, created at station load (P1).
- **Triggers now:** seated = Status Flags bits 24-26 or Flags2 bits 1-2 (EDVR `journalSeatedFromFlags`). Disembark = journal Disembark with SRV, Taxi and Multicrew false (P5). Status OnFoot lags about 6 s, so it is not a restore trigger.
- **Ruled out:** Status.json as the sealed signal; ToggleCockpitRender as a render switch; InteriorStateChangeActivity as a switch (P7).
- **Hangar state (RE 2026-10-12, offline):** the four docking readers are virtual methods of one class O, reached through vtable 0x51A9F00 at O+0x58, which is the AAB8 object (`this`). Its state byte is this+0x3C8 (2 on the readers' docked path), its state name is a typed value at this+0x340, and its owner is at this-8 (occupancy count at owner+0x10). EDVR's reader detours already receive `this`. PadRetraction and HangarLiftDoors__ storage is still not located, and no link from a render record to O is found. Next flight: the state dump over a launch cycle, with owner+0x10 added (see "Hangar door state (RE, 2026-10-12)").
- **Open:** pad and door storage (P9); cell radius; pane draw hashes and ownership; env slot numbers; player_location read path (P10).
- **Next flight, no engine change:** the Built v7 census in three captures (see Built v7): (a) docked, hatch closed, seated; (b) on foot in the bay; (c) on foot in the concourse. Then the Built v5 plan (with the `torn=` check of Built v6). Older steps, still open: the Built v4 plan in Measurement: docked, step the trial down the ladder one rung at a time with a census (F6) at each rung, and note the smallest radius where everything still looks complete; then the approach state dump (F3, `diag.state_dump_frames = 6000`) and the look probe at the concourse bay windows. The Built v3 and v2 plans still stand for the rest (the open-pad outpost, M3, is in v2). The M1 draw link is the open item.
- **Probe keys (TEMPORARY, Built v8):** `hotkey.builder_probe` (arms one frame of builder calls), `hotkey.hatch_probe` (toggles the hatch-state lines), `hotkey.sealed_trial_mode` (cycles sphere and named while the trial is on), `diag.sealed_trial_mode = sphere` (or named), `diag.sealed_trial_builder = count` (skip is refused). All unbound or default off; all on the flat allow-list; all in the template commented. Suggested bare keys: F9 (builder probe), F11 (hatch probe), INSERT (trial mode).
- **Probe keys (TEMPORARY):** `hotkey.asset_census` (Built v7; unbound by default; on the flat allow-list), `hotkey.state_dump`, `hotkey.look_probe`, `hotkey.sealed_trial`, `hotkey.sealed_trial_smaller` and `hotkey.sealed_trial_larger` (all unbound by default), `diag.state_dump = off`, `diag.state_dump_frames = 3000` (60 to 6000; 6000 for the approach dump), and `diag.sealed_trial_radius_m = 80`. **Gate for any hook:** TimeDateStamp 1788384820, SizeOfImage 104894464.
- **Dynamic actors (RE, 2026-10-12, offline, no build):** the store is not the only admission. Both record loops (0x4320370 and 0x4321A13) test rec+0x234 and call the one builder 0x42B4420 (two direct call sites, PROVEN). The trial clears inside the store call, before those loops, so H2 (mask re-OR'd after the store) is RULED OUT. Open: H1 (a submitter outside both loops), H3 (other records), H5 (array members the store never visits keep stale flags, new). H4 (containment keeps near-camera actors) is PARTLY PROVEN and explains the on-foot list. Hook candidates: the store (already hooked) and the builder entry. Decide with the one-flight probe in the section "Dynamic actors (RE, 2026-10-12)". Nothing built.
- **Draw paths (RE, 2026-10-12, offline, no build; section "Draw paths (RE, 2026-10-12)"):** two appenders of 0x150 pool records. Path B = FUN_144312E00, called INSIDE the store (rigid movers: ships, drones), so the trial's post-store clear never reaches it. Path A = builder FUN_1442B4420 (instance-bearing records, admitted in the traversal and the single-entry path by rec+0x234/0x208/0x290/0x298). Leaf records (rec+0x290 = 0, rec+0x240 never written) cannot reach the builder. Veto points: builder bracket 0x42B4420 (A), producer entry 0x4312E00 (B). The .rdata/.pdata "callers" are range records, not calls. Next flight: the probe in "Draw paths", not built.

## Proven

**P1. Concourse ownership.** Factory 0x1B0BBC0 (called by SpaceOrStationLoad 0x1F4700, which 0x1FD45B calls) creates InteriorSubLevelAttach (0x5212CF0), StationInteriorHub (0x5213E48), StationInteriorComponent (0x5213BC8), TurboLiftSystem (0x5211FF0), DockingArmBoardShipButton (0x52122E8) and SocialSpaceBroadcastSync (0x5213550). The sub-area builder is 0x1B17C2E ("Not ready to make interior sub-area level"), which calls 0x1B17F70. The name "Interiors" (0x5211CC8) goes to registrar 0xB7BD0. Concourse_Bar and five other Concourse_* shops (0x51E19B0-0x51E1A30) are named in table builder 0x17425F0. The lift header ToSocialSpace ties SocialSpace to the concourse (inferred).

**P2. The lift.** TurboLiftComponent, TurboLiftComponent::State, TurboLiftDoorBank, TurboLiftPassenger (0x52148F0) and TurboLiftTerminal are created by 0x1B0BBC0. No code reads these names.

**P3. Pad and door properties.** Function 0x12C8C54 (1365 bytes) registers PlayerCanSeeInside (0x51AA150), HangarLiftDoors__ (0x51AA1C0), OutfittingLightsOn (0x51AA1A8) and PadRetraction (0x51AA198) on the station-lighting component: PlayerCanSeeInside and OutfittingLightsOn are on the object at this+0x240; PadRetraction and HangarLiftDoors__ are on other objects in the same function (owners unconfirmed). HangarLiftDoors__ is copied inline into a descriptor at +0x14 with a default byte at +0x24 (from 0x51AA1D0). That is a descriptor, not the live value (inferred). Function 0x12C93A0 registers LandingPad (0x51AA140) with PadRetraction and LandingPadExtraLighting.

**P4. Inner dock components.** Factory 0x12C9B70 (called from SpaceOrStationLoad 0x1F52E6 in the earlier pass) creates InnerDockComponent, InnerDockLightingComponent (0x51A9928), InnerDockNavigationComponent, DockComponent, LandingPad and LandingPlatform (0x51AA5E0).

**P5. Seated, on foot, disembark.** EDVR decodes Status.json: Flags 24-26 and Flags2 1-2 mean seated (journal_watch.h:139-148); Flags2 bit 0 is on foot (journal_watch.h:136-137). EDVR counts journal Disembark and Embark (journal_watch.cpp:352-355). Keys on the user's own journal Disembark and Embark lines (names only): SRV, Taxi, Multicrew, OnStation, OnPlanet, ID, StarSystem, Body, BodyID. Engine emitters: 0x24D602C writes "Disembark" (sites 0x24D608E and 0x24D60CE; codex variants at 0x5196F70-0x5196FF0), and 0x24D610A writes "Embark". Neither has a direct caller or a stored pointer, so the engine-side trigger is unresolved. explorer_cam_core.h:766-768 records the 6 s Status lag.

**P6. Docking states.** Inside_Hangar, Outside_Hangar and StateTeleport_Inside_Hangar (0x51A85F0, 0x51A8600, 0x51A8648) have .data pointers at 0x5E78C58-0x5E78C98. Readers 0x12C3C20, 0x12CAF80, 0x12CADE0 and 0x12CB320 call a virtual getter at [obj+0x88], then helper 0x140A21560 (probably a string compare; unconfirmed). 0x12C6F1F formats "StateTeleport_%s". NoHangarPresent (0x51AA2A8) is written by 0x12C0F11, the state for a pad without a hangar.

**P7. Existing toggles.** ToggleCockpitRender (0x5162320) is an input action name in the binding table of 0xECCF70; no handler found. InteriorStateChangeActivity (body 0x2853A99) waits for render-thread initialisation and logs. PlayerCanSeeInside has no reader found. No function that hides the concourse was found.

**P8. Glass and reflection names.** The exe has no window or glass material name; the "glass" strings are the UI frosted layer. Reflection and environment names: gEnvmapTexture (0x5039750, referenced in 0x579E60), gDiffuseEnvmapTexture (0x5039760), gEnvmapSplitSumLUTTexture (0x5039778), EnvmapSampler (0x5039968), MirrorSampler (0x5039908), c_envmapTextureFresnelLUT (0x50B97F8), c_viewToEnvmapRotationT (0x50B98C0), g_Tex_ReflectionTexture (0x5038AE0), g_enableReflectionVolumes (0x5038878), g_numReflectionVolumes (0x503A020, referenced in 0x57A730). View: c_cameraPosition (0x50B9760), c_cameraForward (0x50B9778), c_inverseViewProjectionMatrix (0x52E9AC0), c_viewportSize (0x51108F8). The exe holds no register slot numbers.

**P9. Pad and door state: not located.** Names, registrations and docking-state readers are proven (P3, P4, P6). The bytes holding PadRetraction, HangarLiftDoors__ and PlayerCanSeeInside were not found.

**P10. player_location.** The values inside_vacuum (0x557A158), outside_station, inside_station, inside_docking_bay (0x557A188), nearby_outpost, inside_docking_bay_outpost (0x557A1B0) and inside_docking_bay_planet (0x557A1D0) are referenced only by registration 0x41D9BB0 (called from 0x40DFAC0). The variable object 0x6031438 is passed to 0x41DD472 (67 bytes) and 0x41E1EA7 (96 bytes). Each calls helper 0x802500, then an indirect call through blackboard vtable slot +0x58; in 0x41E1EA7 the object is [this+0xE0]. A set or compare pair; direction unproven. Storage not found.

**P11. Prologue bytes (this build).** Builder 0x42B4420 `4C 8B DC 55 56 41 54 41 57 49 8D AB 08 FC FF FF`; store 0x4312040 `40 55 53 56 57 41 56 41 57 48 8D AC 24 68 FE FF`; traversal 0x4320340 `4C 8B DC 41 54 41 55 48 81 EC 98 00 00 00 48 8B`; RenderDirLights 0x5AC9C0 `48 89 5C 24 20 55 56 57 41 54 41 55 41 56 41 57`.

**P12. EDVR shader-swap pattern.** vscreen.cpp:3063 wraps the game's PSSetShader (hookedPSSetShader). particle_fix.cpp matches draws by bound VS and PS hash (boundVsHashFast, boundPsHashFast; binding_shadow.h). Replacements come from shaderSwapCreatePs (build-time bytecode, shader_swap.h:59) or shaderSwapCompilePs (runtime, shader_swap.h:97). Wrappers save and restore the state they change (planet_motion.h:62-68). src has no environment-map sampling today.

**P13. Census.** The census keys draws by VS and PS hash, instance count, depth write and pass, and reports occlusion samples (visible pixels) and triangles. The depth function is GREATER_EQUAL, so the engine is reversed-Z. In occlusion_census_20261011_115510.csv (a docked capture), 424 rows have 4 or more instances. The largest by triangles is 120 draws of 4 instances (DE545DC8EE4FBB87 / E46E3E4832B2FDB0, write, colour); the next is 168 draws of 6. Not pane evidence.

**P14. Screenshot (Sean, flying inside a station).** A diagonal row of four angled bay windows, a fifth angled window at the row's end, and a box-like module beside them. Each shows a lit interior (furniture, machinery, people). A pane is about 150-200 px wide on the 2000 px frame. HUD: "LEAVE STATION 3:51". No exe string matches "leave station".

## Inferred

- **I1.** The concourse records form their own collection under InteriorSubLevelAttach.
- **I2.** The bay windows are hull records set into the central structure, so the concourse cull leaves them. M1 must confirm.
- **I3.** The string from the [obj+0x88] getter is the docking state (P6).
- **I4.** The doors open after the pad starts moving, so a release at the pad's first change precedes the gap. Unmeasured.

## Dead ends

- Status.json as the sealed signal: Flags do not separate a lowered pad from a raised one.
- An interior visibility toggle: none found (P7).
- Helper 0x802500: called from thousands of sites; not a state reader.
- Box cull shape (an oriented box for the sealed trial): ruled out, because the planes test 0x4F4E10 has no box input (Built v6).

## Design

### A. Primary: sealed hangar

- **Gate:** dock state Inside_Hangar (P6) and NoHangarPresent false. Once P9 is found, add PadRetraction lowered and HangarLiftDoors__ closed. Until then, the state name and NoHangarPresent.
- **Release, no pop:** release on the first edge away from Inside_Hangar, or the first change of PadRetraction, whichever comes first. If I4 holds, the cull ends before the doors show a gap. If no earlier edge exists, the release is on the first door frame and one frame may show a gap. Lead unmeasured (M2).
- **Cell:** a record is inside when its bounding sphere (radius rec+0x280, pose rec+0x290) lies within R of the LandingPad centre. R comes from M1. Inside: the player's ship, other ships, NPCs, vehicles and the hangar walls. Everything else is outside.
- **Hook:** the store 0x4312040 writes the view mask rec+0x208. In sealed frames, clear the view bits of outside records after the store. The traversal (0x4320340, 0x4321940) then skips them in the main and shadow views. Whether the store runs once per record per frame is an M1 probe. Fallback: the per-view builder 0x42B4420, which costs more.
- **Sun:** RenderDirLights 0x5AC9C0 binds c_numDirectionalLights by name (station-interior doc P5). In sealed frames bind 0. Point and spot lights remain.
- **Cost:** one sphere test per record per frame, and one skip per culled record. Unmeasured.
- **Outposts:** an open-pad outpost reports NoHangarPresent, so the gate stays false (M3).

### B. Secondary: closed panes while seated

- **Cull:** while seated (journalSeatedFromFlags), clear the view bits of SocialSpace records (the InteriorSubLevelAttach and StationInteriorHub subtree) in the same store hook. Restore on the Disembark trigger. The concourse is masked, never unloaded, so a restore takes effect at the next traversal. No pop if it precedes the first on-foot frame (M2).
- **Scope:** apply the pane look only inside a station, so exterior views keep their look. Scope on dock state or player_location once found; until then, on the journal Undocked event.
- **Pane look, stage 1 (mirror-like, opaque, depth written):**
  - **Draws:** every glass draw of the four bay windows, the fifth window and the box module, found by M1 as instanced draws with stable VS and PS hashes. M1 lists each one, so none is missed. Exclude the cockpit canopy, which belongs to the ship record and keeps its own shader.
  - **Output:** col = tint * env(R) * F + base, alpha 1. R = reflect(-V, N). F = Schlick with F0 about 0.04, or c_envmapTextureFresnelLUT. Tint: a constant, slightly cool grey.
  - **Env:** gEnvmapTexture (cube) through EnvmapSampler, with the view-space R rotated by c_viewToEnvmapRotationT. For interior reflections, prefer g_Tex_ReflectionTexture (MirrorSampler) where a reflection volume covers the pane. Coverage of the concourse is an M1 item.
  - **Env slots:** not in the exe. At runtime, D3DReflect the bound game PS bytecode for the bind points of gEnvmapTexture and EnvmapSampler, and for the cbuffer offsets of c_cameraPosition and c_viewToEnvmapRotationT. At draw time read the bound SRV and sampler at those points (PSGetShaderResources, PSGetSamplers) into fixed slots of the replacement. If the pane's own PS does not sample the envmap, take the frame-global envmap SRV bound by another draw and cache it.
  - **View vector:** V = normalize(c_cameraPosition - P). P is the pane VS world position if its outputs carry one; otherwise the intersection of the pixel's view ray (from c_inverseViewProjectionMatrix) with the pane's plane. The second needs no VS change. c_cameraPosition comes from the frame's view constant buffer, through the existing cbWatch (draw_census.cpp).
  - **State:** blend off, depth write on, depth function GREATER_EQUAL (reversed-Z); restore everything else after the draw (planet_motion pattern).
  - **Cost:** unmeasured. The screenshot framing gives about 150k pixels across five panes, one cube sample and a few ALU each. CPU: about four state sets per pane draw per frame.
  - **Complexity:** medium: a census for hashes, a replacement PS through tools/temporal_shader_build, one constant buffer, state save and restore.
  - **Risks:** a pane that writes depth without motion vectors may ghost in the temporal pass; check in flight. At 50-300 m a mirror pane can read darker than the lit interior in Sean's screenshot. If it does, add a warm emissive term (stage 1b) before considering the impostor.
- **Stage 2 (later, only if stage 1 fails):** an impostor per pane, a colour view of the concourse from each pane centre, rendered once per visit. The engine's view path is depth-only for shadows, so colour capture is new work. High complexity.

## Measurement

- **M1. Record-attributed census.** Tag draws with their record. Probe the first three calls of builder 0x42B4420 for the record argument; attribute draws by DrawIndexed return address (draw_census.cpp). Per record: triangles, visible pixels, collection, bounds, sealed-cell membership. Run (a) docked, pad lowered: triangle and draw share outside the cell; (b) flying inside a station: pane draws, hashes and env coverage.
- **M2. Storage and timing.** Dump the LandingPad object and the lighting object at +0x240 across one pad cycle and one door cycle. Find the bytes that change and log them per frame beside the dock-state name. Measure the release lead (state edge to first door motion) and the lag (journal Disembark line to first on-foot frame).
- **M3. Outposts.** Dock at an open-pad outpost. Log NoHangarPresent, the dock state and PadRetraction. The gate must stay false.
- **Flight tests:** sealed hangar with the cull on and off (draws, frame time); the door-open transition watched for pops with the release at the state edge; the pane look at 50-150 m against the screenshot framing; disembark restore against the Disembark journal line.

### Built, 2026-10-11 (branch `claude/shadow-gate-probe`; build green; not installed, not flown)

**M1, blocked: no draw-side record identity.** The record side is known: `primaryIdentity` (`src/d3d11/engine_velocity_emit.h:413`) takes `record = lod - 0x210` from the builder's nibble argument and reads `context = rec+0x290` and `node = rec+0x18`. The draw side has no record: the primary copy scatters every native rigid record into one private pool (`engine_velocity_primary_copy.h`, `apply`), keyed by target address (`g_emissions`), so a draw is an offset into that pool and not a record. Two routes in the spec do not hold. The DrawIndexed return address is not attributed anywhere (`draw_census*` has no return-address code), and a return address is shared by every record that issues that draw. The thread-local-around-the-builder route does not hold either: the builder emits items and draws nothing. Next step, unverified: in the census, map a draw's bound vertex buffer and its start index to the pool target and from there to the emission (`g_emissions`). That needs one capture showing that the rigid draws bind the private pool and at what offsets.

**M2, built: `hotkey.state_dump` and `diag.state_dump` (`src/d3d11/state_dump_probe.cpp`, `state_dump_core.h`, rig `tools/state_dump_test`).** Four CodeHook detours on the docking-state readers, with the pinned six bytes `40 53 48 83 EC 20` (push rbx; sub rsp,20h, the same in all four) and six stolen:

| RVA | Reader | Forwarded | Returned |
|---|---|---|---|
| 0x12C3C20 | calls `[obj+0x88]` on rcx | rcx | RAX (AL code) |
| 0x12CAF80 | same shape | rcx | RAX |
| 0x12CADE0 | same shape | rcx | RAX |
| 0x12CB320 | same shape | rcx | RAX |

The probe never calls the getters. Per owned frame, at the Present tick (under its own `BoundaryTick`), it records: the journal's cumulative Docked, Undocked, Disembark and Embark counts (`journalDocks`/`journalUndocks` are new in `journal_watch.cpp`; the other two exist); the Status.json Flags and Flags2 words (`journalRawStatus`); for each reader, its AL code, its call count and its object; and for each distinct object the readers name (up to four tracks), the bytes that changed in its first 0x400 bytes, in the 0x40 bytes at +0x340, and in the 0x100 bytes behind the pointer at +0x240. Every read is SEH-guarded. Output: `edvr_logs\state_dump_<stamp>.csv` (one row per frame, 23 columns, the changes as `A<obj>.obj+0xOFF:WW>NN`), and `state dump:` lines for each event (a counter moved, a reader's code changed, a new object, bytes changed) and a done line. The capture is 600 frames.

What the flight must settle: which reader object is the LandingPad and which holds the lighting properties (the +0x240 region); what the reader codes mean (the readers branch on the getter's bool, and the codes are the observed AL values); and whether NoHangarPresent appears in any reader code (P6 says it is written by 0x12C0F11, which the readers do not call).

**M3, flight step at an open-pad outpost (no new code).** Dock at an outpost whose pad is open. Press `hotkey.state_dump`, then raise the pad and lower it within the 600 frames. In the dump, read the reader codes and objects, the Flags and Flags2 words, the journal Docked event, and the pad fields (the +0x240 region and the bytes that change with the raise and lower). NoHangarPresent is not one of the four reader codes. Observing it directly needs the writer 0x12C0F11's function entry, which is not yet identified; until then the step reports the codes the readers return, and the gate in A stays false only if the reader codes say so.

### Built v2, 2026-10-11 (after flight 5; branch `claude/shadow-gate-probe`; not installed, not flown)

**What flight 5 showed for M2.** The 600-frame dump (about 7 s) was too short to catch a pad or door movement. Reader 2 (object `0x254C4D94AE8`) ran only during the door-cycle dump. The session started docked (`Location` carried `Docked:true`), and the journal had no Docked event. `undocks` read 2 against one Undocked line.

**The undock count, explained and fixed.** Two causes in `journal_watch.cpp`, both counting a token twice. (1) The chunk carry: the last 32 bytes of each read are stitched in front of the next read and scanned again, so an event token in that tail counted twice. (2) A reopen (a file switch, or a fault) replays a provable journal from the top (lines 579 to 585). Both now count a token once, by its absolute byte position per journal file name (`countOnce`). Disembark and Embark counts are affected by the same double count, so they now deduplicate too; that changes their numbers (reported with the build).

**The dock state.** The Location line's `"Docked":true|false` is read from the same read buffer (`noteLocation`) and published through `journalLocationDocked`. The state dump's CSV carries `loc_known`, `loc_docked` (from Location) and `status_docked` (Status Flags bit 0). A change in either is logged as a `state dump:` line, beside the Docked and Undocked counts.

**The capture.** `diag.state_dump_frames` sets the length (default 3000, clamped to 60 to 6000; a value that is not a whole number takes the default and says so). Each reader object is tracked (up to eight objects): `+0x000` to `+0x3FF` (which holds `+0x18` and `+0x30`), `+0x300` to `+0x37F` (which holds `+0x330` to `+0x345`), and the 0x100 bytes behind `*(+0x240)`. Up to 48 changed bytes are listed per region per frame, up from 16.

**The jump guard's marks (TCWR, `diag.camera_writes`).** `pick=guard` is set on the write the guard applied in place of the legacy pick (key on). `rejected=1` is set on the legacy pick the guard rejected (either key), so a Pause dump shows the competing writes and the flip the guard masks. The TCWR line gains a `rejected=0|1` field after `pair=`; `guard` is a new pick name; lines written before the field read as not rejected. The marks use the write's own frame clock.

**The flight plan (Sean, 2026-10-11).**
1. Docked, pad lowered, doors closed: F4 (`hotkey.shadow_gate_probe`) in a bright phase, then F4 again in a dark phase.
2. F3 (`hotkey.state_dump`, 3000 frames, about 35 s at 90 fps). Within that window, deliberately launch: the pad rises and the doors open. The dump's per-frame rows give the lead.
3. Fly out about 1 km from the same station, and press F4 once.
4. In flight, inside and outside the station, press Pause a few times with `diag.camera_writes = on`.
5. Outposts (M3): dock at an outpost whose pad is open, press F3, raise and lower the pad within the window, and read the reader codes, the Location and Status docked values, the Docked event, and the pad object's changed bytes. NoHangarPresent is not one of the four reader codes (P6); observing it needs the writer 0x12C0F11's entry, which is not identified yet.

**Format changes in this build.** The state dump CSV has 26 columns (`loc_known`, `loc_docked` and `status_docked` sit before `flags_hex`). The shadow gate CSV has 23 columns (the window replica columns are gone), and a shadow cadence CSV is new. TCWR lines carry `rejected=` and the `guard` pick.

### Built v3, 2026-10-11 (after v2; branch `claude/shadow-gate-probe`; full build green; not installed, not flown)

**The journal's Location line (fix for the flight-6 "reads 0 while docked").** The Docked key of a `Location` line sits about fifty bytes after its event token: `"event":"Location"`, then `"DistFromStarLS":<number>`, then `"Docked":true|false` (checked against the user's own journal's key order, 2026-10-11). The v2 reader searched one read's bytes for the key, and its carry was 32 bytes, so a read that ended inside the gap lost the value. `src\d3d11\journal_location_core.h` is a byte-stream scanner that keeps its state across reads and drops the bytes an earlier read gave it; `journal_watch.cpp` feeds every byte of the file once. `tools\location_scan_test` checks every split of a one-line journal, one-byte reads, a replay from the top, overlapping re-reads and the refusals (17 checks).

**The AAB8 object (state dump).** The first reader object whose address ends in `0xAAB8` is pinned for the capture. It is tracked on every frame whether or not the readers name it (a general track is freed when they stop naming its object), and each row's `changes` column carries `AAB8.key3C0=<32 hex>`: the sixteen bytes at `+0x3C0` to `+0x3D0`. Changes anywhere in the object are still listed by the byte diff. `diag.state_dump_frames` keeps its meaning (default 3000, 60 to 6000).

**The sealed-hangar trial (`hotkey.sealed_trial`; `diag.sealed_trial_radius_m = 80`).** Off by default; a press toggles it from the next frame. While on, each record the admission store writes (0x4312040; the detour is installed only while something wants it, `src\d3d11\store_hook.cpp`) is judged by a sphere rule: kept when its sphere (`rec+0x240` centre, `rec+0x280` radius) contains the camera, or meets the sphere of the given radius around the camera; otherwise its view mask (`rec+0x208`) and admitted flag (`rec+0x234`) are cleared, so the traversal skips it. The camera is the main view's origin (render context +0x40 + 0x540). The sun pair (VS `7E38A6AA1269C901` with PS `7CECABDE34FFBE9E`) is skipped in every frame while the trial is on. A line every 10 s: `sealed trial: window 10 s culled=.. kept=.. kept_unjudged=.. sun_pairs_skipped=.. eval_ms=.. faults=.. radius_m=..`. Off costs nothing: the store gate is closed and the sun skip tests one atomic. Each read and write is validated; a failed read leaves the record as the store wrote it and counts a fault. `tools\sealed_trial_test` checks the radius parse, the rule and the lines (23 checks).

Not proven: the store writes a record's children before its observer runs, so the children of a culled record keep the masks the store gave them. Whether the traversal reaches them through a cleared parent is the question the window's `culled` count and the frame's draw count can answer.

**The look probe (`hotkey.look_probe`).** A press arms one capture. The next Present starts collecting, and the frame after that is recorded: (a) every record the store writes whose sphere the camera's forward ray meets within 2 km, nearest first, with its address, radius, entry distance, bucket, base-or-child role, child count, and the vtable RVAs of the record and its node; (b) every draw of the frame. Each draw with a plain colour target (no UAV, no stream output) is duplicated under a one-pixel scissor at the centre of the first viewport, with its own depth test against the depth buffer as it stands before the draw and no colour or depth writes; an occlusion query counts its visible samples. Up to 4096 draws per frame are tested; the queries are read back over up to 120 Present ticks. The forward is the near plane's direction taken from the far plane of the main view's frustum: the displacement from the eye onto the far plane, so the answer does not depend on the engine's normal convention. Output: `edvr_logs\look_probe_<stamp>.csv` (24 columns; record rows, then the last 64 covering draws in issue order) and one `look probe:` line, plus a note of the eye-to-plane distances. `tools\look_probe_test` checks the ray test, the forward recovered under both conventions and rotated, the refusals, and the 24-field rows (29 checks).

Limits: each tested draw costs one extra draw for that frame (a hitch). The centre test is the depth state before the draw, so it says what would pass the depth test there, not what ends on screen; the later covering draws are the likelier visible ones. The census wrappers (`vscreen.cpp`) now offer each draw before their own bracket, and their install is reference-counted so the census and the look probe can share it. Flat: the draw wrappers may not see the flat draw path (not flown).

**Format changes.** Look probe CSV: new, 24 columns. State dump: the `changes` column gains `AAB8.key3C0=`. Journal: none; `loc_docked` now reads the scanner.

**The flight plan (v3).**
1. Docked in the hangar, bright phase: press `hotkey.look_probe` once. The log shows `look probe: collecting frame N`, `frame N+1 collected`, the summary line and the CSV. Hypothesis L1: the hull's records are on the ray while docked, nearest first. Refuted if no large record is on the ray while the hull fills the view. Hypothesis L2: the covering draws include the hull's draws, identified by their VS and PS hashes against the shadow gate CSV's candidates. Refuted if none of the covering draws matches those hashes.
2. Same place, dark phase: press `hotkey.sealed_trial`. The log shows `sealed trial: on at frame N (radius 80.0 m; camera (...))`, then a `sealed trial: window` line within 10 s. Hypothesis S1: the hangar stays on screen with the trial on, because the hull's records contain the camera or meet the sphere. Refuted if the hangar disappears. Hypothesis S2: the far station records and their sun-pair draws are gone, so `sun_pairs_skipped` and `culled` are non-zero. Refuted if `sun_pairs_skipped` stays 0 or `faults` is non-zero. Press again to turn it off: `sealed trial: off at frame N`.
3. F3 (`hotkey.state_dump`) as in step 2 of the v2 plan. Read `loc_docked` on the rows around each dock and undock (it now comes from the scanner; it should follow the journal's Docked lines), and the `AAB8.key3C0=` text: it should change when the pad or the doors move. Refuted if it never changes while the pad moves.

### Flight 7: the sealed trial at 80 m, docked in the hangar, 2026-10-11

Log `edvr_gfx_20261011_150437.log`, build v0.19.1-57-g59f1d160 (`claude/hangar-test-4`: the v3 commit `06571406` plus the camera rule). Sean: at 80 m nothing was missing; the hangar looked good, just darker, as expected; the sun bleed was gone. The window: culled about 1.8M per 10 s, kept about 54k, the rule about 0.16 ms a frame, faults 0. Census off, then on: draws 5372 to 3940, triangles 5.34M to 4.74M, zero-sample draws 78% to 70.5%.

The flight shows that the rule keeps the hangar at 80 m and that the far records, and the sun-pair draws they feed, go with it. It does not show how far the radius can come down before something visible is lost. That is the next flight.

### Built v4, 2026-10-11 (branch `claude/hangar-test-5`; full build green; not installed, not flown)

**The radius steps.** `hotkey.sealed_trial_smaller` and `hotkey.sealed_trial_larger` (unbound by default) step the trial's radius one rung while the trial is on, through 80, 60, 40, 30, 20, 15 and 10 m. A step closes the window of the old radius first, so each rung has its own line. Each change is logged as `sealed trial: radius -> R m at frame N`. A press at an end of the ladder changes nothing and says so (the smallest step is 10 m, the largest 80 m). With the trial off, the keys log a refusal. A starting radius that is off the ladder steps onto it: 50 m smaller gives 40 m, larger gives 60 m.

**The windows.** One line every 5 s while the trial is on (was 10 s). Every window line carries `radius_m`.

Nothing else changed: the rule, the sun skip, the store hook and the camera rule. The camera chooser is untouched. `tools\sealed_trial_test` checks the ladder, a step down from every rung and up from every rung, the clamps at 80 m and 10 m, the walks past both ends, the off-ladder starts, the radius parse clamp at 1 and 5000 m, and the lines (46 checks).

**The flight plan (v4).**
1. Docked in the hangar, bright phase. The start radius is the default, 80 m. Press `hotkey.sealed_trial` to turn the trial on. Look around the hangar: the ceiling, the walls, the lift, the ship, the props. Press F6 (`hotkey.occlusion_census`) for a census at this rung.
2. Press `hotkey.sealed_trial_smaller` to step to 60 m. The 80 m window closes on that press. Wait for the rung's first window line, look around again, and press F6. Repeat at 40, 30, 20, 15 and 10 m.
3. At each rung, record in the table below whether everything still looks complete, and what went missing if it does not. The answer is the smallest radius where everything still looks complete.
4. Separate flight, approach: set `diag.state_dump_frames = 6000` (the cap). Press F3 (`hotkey.state_dump`) about 30 s before touchdown. Read `loc_docked` on the rows around the dock, and the `AAB8.key3C0=` text across the pad and door movement.
5. Separate stop, concourse: stand in the concourse facing a bay window and press `hotkey.look_probe`. Read the record rows on the ray (is the window's pane among them?) and the covering draws (what paints the centre of the window).

**Hypotheses and what would refute them.**
- R1: the hangar's own geometry (the ship, the props, the lift) stays complete down to some rung, because its records contain the camera or meet the sphere. Refuted if something inside the hangar goes missing at a rung above which its record should still be kept. That record is then outside the sphere, and the look probe can name it.
- R2: draws and triangles fall at each smaller rung without a visible loss. Refuted if the census does not fall; the rule is then not culling the far records, and the window's `culled` count says whether it runs at all.
- S1 (approach): the `AAB8.key3C0=` text changes when the pad lowers. Refuted if it never changes while the pad moves.

**Rung table (fill in on the flight).**

| rung (m) | census (draws, triangles, zero-sample %) | complete? | what went missing |
|---|---|---|---|
| 80 (flight 7, census on) | 3940, 4.74M, 70.5% | yes | nothing |
| 60 | | | |
| 40 | | | |
| 30 | | | |
| 20 | | | |
| 15 | | | |
| 10 | | | |

### Flight 8: the look probe on foot, and the sphere's children, 2026-10-11

Log `edvr_gfx_20261011_160819.log`, build v0.19.1-58-gd30df4c4 (`claude/hangar-test-5`). The sealed trial's ladder works: from 80 m down to 15 m the culled-to-kept ratio does not change, and only at 10 m does kept drop by about nine times. Seated in the docked hangar at 10 m, Sean saw nothing missing. On foot, looking out of the hangar opening, the walls of the station beyond were culled (space shows through), but far small props (lockers, banners, gantries, cables, well beyond 10 m) were kept. The look probe did not aim: `forward=unknown (no opposite near and far planes in the main view; records not aimed)`, 2623 records scanned, none on the ray; 34 centre draws listed (`look_probe_20261011_161210.csv`).

Two things follow. The frustum route cannot aim: the main view has no opposite near and far pair, and the capture was on foot, where Elite renders the flat on-foot camera into the 2D panel (3712 by 2088) rather than the eye targets. So the eye rows do not apply on foot. The props are kept while their own spheres are far away, which suggests they inherit their base's verdict through the child list (`rec+0x2A8`), as the station doc's decompile notes say.

### Built v5, 2026-10-11 (branch `claude/hangar-test-6`; not installed, not flown)

**The view the aim comes from.** The look probe and the sealed trial resolve the frame's view once (`src\d3d11\view_source.cpp`, `view_source_core.h`). The explorer cam and the seated view read the rows the temporal pass chose (`temporalPassChosenRows`). Those are the game's view-to-world rows `[R | c]` (temporal_math.h states it): the camera's place is `c` = (rows[3], rows[7], rows[11]), the axes are the columns of R, and the forward is column 2, the view's +Z (DirectX forward; inferred, to be checked in flight). An earlier draft read the rows as world-to-view; the source comment corrects that, and the rig now builds the rows the right way. On foot without the explorer cam, no pose is read: the chosen rows are not the on-foot camera, and the on-foot camera's rows are not read yet, so the source is `origin` (the render context's view 0 origin) and the look probe falls back to the frustum, which has no far plane on foot. The frustum route is a fallback only, with a sanity check (the near plane 1 cm to 50 m away, the far plane 20 m to 1000 km). Every line names its source: `view source = explorer cam`, `eye rows`, `origin`, `frustum fallback` (`vscreen` is reserved). The sealed trial's centre is the explorer cam's rows in the explorer cam, and the view 0 origin otherwise, which the flights proved. Each toggle logs the gap between the rows and the origin, so the flight shows whether the two frames agree: seated they should differ by a few centimetres (one eye against the other). The toggle line reads the view at the toggle, so `camera unknown` no longer shows at the toggle frame.

**The kept-records report (`hotkey.sealed_trial_report`; unbound by default).** While the trial is on, a press captures one frame. The records the store observed are walked through their child lists (to a depth of six), and every kept record is classified: `own` (its own sphere contains the camera or meets the cell), `base` (its own sphere does not, but an ancestor's does; `levels_up` counts the generations), or `unjudged` (no sphere on the way up qualifies). `judged` says whether the observer saw the record in the frame. The report writes `edvr_logs\sealed_kept_<stamp>.csv`: a `#` line naming the view it was evaluated against (source, camera, forward, radius), the header, the top 40 kept records by radius, and the top 40 kept base records (records with children) by child count. The log line gives the counts, including how many kept children were kept only because their base was.

**Format changes.** The window and toggle lines end with `view source = ...` and name the centre (`rows` or `origin`); the look probe line names its view after the trigger. The report CSV has 17 columns.

**What is and is not established (research, 2026-10-11).** Established: the row convention above; the explorer cam's session flag is one atomic (`explorerCamSessionActive`); the free camera's pose goes into the game's FreeCameraActivity object (activity + 0x3B0 local, + 0x70 world), with no public accessor; the eye rows follow the commander's body while walking (measured). Not established: the on-foot camera's rows. The screen draws' `VS b1` holds a view-projection at floats 1080 to 1095 and an origin at 1100 to 1102, but the float-932 block is not shown for VR on foot. Also not established: whether view 0 is the flat camera on foot or in Explorer Cam; whether the rows' `c`, the view 0 origin and the records' centres share one frame (the frames check is logged, not yet read); the sign of the forward against a known world direction. The on-foot source needs a capture of the screen camera's rows (the engine velocity source already holds b1's rows 270 to 275, privately) and a flight to confirm the layout.

**The flight plan (v5): Explorer Cam, stereo through the eye path.** The code as read places the camera at the commander's head, relative-locked, on foot only, with the camera keys blocked (`explorer_cam_core.h`, `docs/explorer-cam.md`). The redesign doc's free placement is not what the code does, so this flight tests the head-locked camera. A camera placed away from the seat is a separate question to settle in the code.
1. Turn Explorer Cam on. Read the look probe's lines: `view source = explorer cam`, and the rows' distance from the view 0 origin. The rows follow the commander's body while walking (measured in an earlier dump), not the seat.
2. Stand in the concourse facing a bay window and press `hotkey.look_probe`. The window's pane should be among the records on the ray, and the covering draws should be the pane's.
3. Press `hotkey.sealed_trial` (80 m), then `hotkey.sealed_trial_report`. Read the `#` line (which view the report used), the `own`, `base` and `unjudged` counts, and how many children were kept only because their base was.
4. Read the top base records. A base with many children that is kept by `base` (not `own`) explains the far props.

Hypotheses and refutations:
- **H-P (props):** the far props are kept because their base or group, whose sphere reaches the camera, is kept, and the children inherit it. Supported if children kept only by their base are a large share of the kept children and the props' `parent_address` is a kept base. Refuted if those children are few, and the props are kept as `unjudged`.
- **H-V (view):** the explorer cam's rows follow the free camera. Refuted if the look probe's note says they do not, or if the view source is `eye rows` while Explorer Cam is on.

### Built v6, 2026-10-11 (branch `claude/hangar-test-7`; not installed, not flown)

**The box is ruled out.** The planes test `0x4F4E10` (build 332841, gate matched) takes a point (`[rdx]`, four floats) and a radius (lane 0 of the sixteen bytes at `[r8]`). It tests both against the plane array of its first argument (`[rcx+0x30]`, count `[rcx+0x44]`; the builder passes its view object as rcx) with dot products and plus or minus the radius, and it reads no extents. So the engine's test has no oriented box to read. The builder (0x42B4420, the call at 0x42B4595) copies the point from `rbp-0x30` and the radius from `rbp-0x20`. Where those locals are loaded from is the research's pose-context reading, not re-verified here. The research also reports that a record's sphere is the union of its own sphere and its children's. That is not re-verified here either.

**The shape key is not in this build.** The `sealed_trial_shape` wiring (the key, its hotkey and the ini default `box`) was never committed: `sealedTrialShapeToggle` was declared and called but never defined, so the tree would not link. A box for the trial would be EDVR's own construction, a box from the children's spheres, not the engine's test, and it needs the children's spheres in the observer. Whether to build that is a choice for Sean.

**Faults.** The store's observer runs the cull through `cullRecordFaultSafe`: it reads the record's mask and admitted flag, writes both, and puts the mask back if the admitted write fails. A failed read or write leaves the record as the store wrote it (kept, counted in `faults`). A put-back that also fails is `torn`: the one case where the record may be left half culled (mask zero, admitted flag still set). The window line reads `faults=N torn=N`.

**Next flight.** The Built v5 plan, with one check added: `torn=` on every window line reads 0. A nonzero `torn` is the first thing to report, with its window line.

### Built v7, 2026-10-11 (branch `claude/asset-census`; full build green; not installed, not flown)

**The record's identity chain (build 332841).** Proven: read from `analysis\decomp` (decomp_4312040, decomp_4330194), with the line numbers. Inferred: the bytes do not close the point.

- **C1. Proven: the identity writer.** `FUN_144330194` (decomp_4330194, lines 36 to 81) walks the record array (its pointer at object+0x280, its count at object+0x298, stride 0x2F0). For each record whose source node SRC is non-zero (rec+0x2B8), it copies SRC+0xF0 to rec+0x250, the model M to rec+0x258, SRC+0x1A0 to rec+0x260, and a u32 combined hash to rec+0x268 (lines 64 to 76).
- **C2. Proven: the component and the model.** X = *(SRC+0x160) and M = *(X+0x470) (lines 40 to 42). M's dword at +0x08 holds a qword count in bits 17 to 27 (line 50); the qwords start at M+0x10 (line 52). The store reads the same SRC and X (decomp_4312040, lines 322 to 326) and tests X's byte at +0x4B against 2.
- **C3. Proven: the hash arithmetic.** mix(x) = low32(h >> 22) ^ low32(h), where h = wang(x): h = (x << 18) + ~x; h = (h >> 31 ^ h) * 21; h = (h >> 11 ^ h) * 65, in 64-bit arithmetic. The combined hash is mix(SRC+0xF0) ^ mix(SRC+0x1A0) ^ (the XOR of mix over M's list) (line 49 and lines 57 to 60). The rig checks this against an independent Python transcription of the same lines.
- **C4. Proven: the record's other fields.** The store reads the node at rec+0x18: the LOD limit is the ushort at node+0x68, and the bounds object P = *(node+0x08) holds the local sphere, centre at P+0x20 and radius at P+0x2C (decomp_4312040, lines 143 to 144 and 207 to 262). It reads the pose pointer at rec+0x190, the sphere at rec+0x240 and rec+0x280, the child list at +0x2A8 (count at +0x2B0), and a callback object at +0x2C0 (lines 129 to 130). The offsets are the decompile's `param_5` offsets times four.
- **C5. Proven: rec+0 is not a vtable.** The store tests the low dword of rec+0 against 0xFFFFFFFF and, when it matches, writes two u16 values there (decomp_4312040, lines 265 to 275). The sealed trial's kept report reads rec+0 as a vtable pointer (`readVal(&vt, w.rec)`, `sealed_trial.cpp`), which explains its garbage `vtable_rva`. The look probe reads its record and node vtables the same way (`look_probe.cpp`, `readVal(&vt, r.address)`), and those columns are unvalidated.

**Inferred (not closed by the bytes):**
- **I-a.** SRC+0xF0 and SRC+0x1A0 are content keys (asset or name hashes), so they recur across visits and stations. The census's `key1_is_pointer` flag (a value in the user address range) and the flight decide it.
- **I-b.** M+0x10 onward is the model's part or mesh key list. The count and the hash fit; the content is unread.
- **I-c.** X is the owning component. Its vtable RVA (`component_vt_rva`) identifies its class if the vtable check passes.
- **I-d.** A human-readable name, if the engine keeps one, sits behind a pointer in node, source, component or model. The census scans each object's first 0x100 bytes for pointers to text (ASCII or UTF-16LE, 4 to 48 characters). No such name is known for a station record. The exe does keep readable names for the avatar's parts: `kPartNameTableRva` (0x5E9C7D0) is a table of 8-byte pointers to C strings, one per AvatarModelComponent part, and EDVR checks it at run time (`explorer_cam.cpp`, `sehNameIs` and `checkPartNames`). That is a name route for the avatar only; no station table is found yet.

**The census (`hotkey.asset_census`, TEMPORARY).** A press arms one frame. Only while it is collecting does the store's observer run. It reads each record once (0x2F0 bytes), then the record's live chain through a per-source and per-model cache. Every hop is validated, and a hop that does not read refuses the chain; its row says which hop. At the Present after the frame it writes `edvr_logs\asset_census_<stamp>.csv`: a summary line, a hatch-state line, the header, and one row per group, keyed by (status, key1, key2, live hash). The columns: observations, records, buckets, children, the radius range, the nearest centre to the camera, `contains_camera`, `main_admitted` (the mask bit of view 0), `mask_zero`, `admitted_flag`, `block_agree` and `hash_agree` (the engine's copy and hash against the live chain), `key1_is_pointer`, models, model count and first qword, the vtable RVAs of model, component and source, `record_vtable`, names, and a sample record with its bucket base. Unknown values print as -1. The camera is `view::sphereCentre`, the sealed trial's rule (the explorer cam's rows in the explorer cam, else the view 0 origin, else the rows); the summary says which was used. The hatch state, in the CSV and the log: the four docking readers' calls in the frame and last codes (-1 when a reader was not called), the AAB8 object's quad at +0x3C8 (16 bytes, as words and floats), the Status `flags` and `flags2` words, `status_docked`, and `loc_docked`. The census arms the readers' observers for its own frame (`stateDumpCensusArm`). Until a reader has named an AAB8 object while armed, the line reads `aab8=none`.

**Changes.** New: `src\d3d11\asset_census_core.h` (pure: chain, hash, PE and vtable check, grouping, names, CSV), `src\d3d11\asset_census.cpp` and `.h` (the press, the collection, the write), `tools\asset_census_test` (93 checks). Changed: `state_dump_probe.cpp` and `.h` (the census arms the same observers; the gate stays open while either wants it; `stateDumpCensusArm` and `stateDumpCensusSnapshot`); `view_source.h` (`sphereCentre`, the sealed trial's rule moved here unchanged; `sealed_trial.cpp`'s `cameraOf` now calls it); `store_hook.cpp` (the observer call and the gate); `device_hook.cpp` (the key, the tick, the binding row); `common\runtime_profile.h` (the flat allow-list); `edvr.ini` (a commented template key); `build.bat` (the DLL source and `:rig_asset_census_test`).

**Build.** Full `build.bat` green on `claude/asset-census`, based on `7fed41c8`. Not installed, not flown. Flat is not tested.

**Flight plan (v7): three captures.** Each capture is one press and one CSV. Put a bare key on `hotkey.asset_census` in the live ini after the install, which this session does not do.
0. A first press in the cockpit, then about ten seconds of play. It arms the readers so that an AAB8 object can be named. Discard its CSV. If the next capture's hatch line still reads `aab8=none`, run `hotkey.state_dump` for 3000 frames (it pins AAB8), then read the census again.
a. Docked, pad lowered, hatch closed, seated in the cockpit.
b. On foot in the bay, at the same station and docking.
c. On foot in the concourse.
d. Optional: flying inside the station.

Then compare the groups. The bay set is what (a) and (b) share and (c) lacks. The on-foot set is what (b) and (c) share and (a) lacks. Repeat (a) on a second docking at the same station to test stability across visits: a content key recurs with the same key1, key2 and hash, and a pointer does not.

Hypotheses and what would refute them:
- **K1 (the hash chain, C3).** `hash_agree` equals `identified` in every group. Refuted if it is zero in every group: then M+0x10 is not the list the writer folds, and the chain is wrong at C2 or C3.
- **K2 (stable keys, I-a).** The same key1 and key2 appear for the same bay on the second docking. Refuted if they change while the bay does not, or if `key1_is_pointer` is 1 for every group.
- **K3 (rec+0, C5).** `record_vtable` is 0 in every group. Refuted if any record passes the vtable check; C5 would then be wrong for that record.
- **K4 (names, I-d).** A row with a name (`node+`, `source+`, `component+` or `model+`) is a name route. Refuted if no row in any capture has a name; the name is then not behind these objects, and I-d is closed.

## Open items

Pad and door storage (M2). Cell radius and hangar record set (M1). Pane hashes and hull or concourse ownership (M1). Env slot numbers (runtime reflection). player_location read path (P10). Status flag names beyond the bits EDVR decodes.

## Dynamic actors (RE, 2026-10-12)

Offline, build 332841 (gate matched). No C++ changed, no build, no flight. The helpers (`xr2.py`: capstone plus the pdata function table; `fieldrefs.py`) are in the session scratchpad and not committed. **PROVEN** means the bytes or a decompile line is cited. **INFERRED** means the bytes do not close the point.

### The admission chain

- **D1. PROVEN: two loops, one builder.** Traversal 0x4320340 (decomp_4320340 lines 40-108) walks up to 8 entries of 0x60 bytes, count at object+0x300. For each entry it calls the store 0x4312040 once for the group base (asm 0x43203F1; the fifth argument is [rdi-0x18], which is entry+0x28, the record array base), then walks the array (entry+0x28, count entry+0x30, stride 0x2F0) and calls builder 0x42B4420 (asm 0x43204F3). The single-entry form 0x4321940 (decomp_4321940 lines 37-94) does the same: store at 0x43219BB, builder call in the pdata-split body at 0x4321ADB (asm 0x4321A13 to 0x4321AE0). That form is fed from 0x431B11D to 0x431B20D: collection rbx = [rdi+0x348], record array rbx+0x280, count rbx+0x298, mask rsi, written to a stack entry (asm 0x431B178 to 0x431B20D, call 0x144321940 at 0x431B20D).
- **D2. PROVEN: the builder has two direct call sites.** A rel32 and rip-relative scan of .text, verified with capstone operands, finds exactly 0x43204F3 (in the traversal) and 0x4321ADB (in the single-entry body). Both come after the store for the same entry. Indirect (vtable) callers are not covered by this scan.
- **D3. PROVEN: the per-record admission test** (asm 0x4321A3D to 0x4321AAA; decomp_4320340 lines 64-101). Admit only if rec+0x290 is non-zero, byte rec+0x234 is non-zero, byte rec+0x298 is non-zero, and some set bit v of the 64-bit mask rec+0x208 has its 4-bit nibble (word rec+0x210 + (v>>4)*8, shift (v&15)*4) at most the ushort at [rec+0x218]+0x6A. The builder gets rcx = rec+0x290, rdx = context, r8 = window (from 0x4331300), r9 = rec+0x210, and [rsp+0x20] = callback rec+0x2C0 when global byte DAT_145ea3483 is non-zero. The record base is r9 - 0x210.
- **D4. PROVEN: the store.** 0x4312040 takes (entry, window, bounds, mask, record); the record is param_5 (decomp_4312040 lines 16-17). It recurses into the child list (rec+0x2A8, count rec+0x2B0; lines 342-353; recursive call at 0x4312650, asm 0x431263B). Per record it calls the callback at rec+0x2C0: vt+0x70 (line 280), vt+0x58 (line 281), and vt+0x80 when the low dword of rec+0 is 0xFFFFFFFF (lines 265-275).
- **D5. PROVEN: the trial runs inside the store call.** store_hook.cpp lines 106-115: storeObserved calls the original store, then sealedTrialAfterStore(bucket, record). That runs for the group base and for every recursive child call. sealed_trial.cpp lines 442-507 keep a record when sealed::keepRecord holds (sealed_trial_core.h lines 64-66: the camera is inside the sphere, or the sphere meets the 80 m sphere around the camera) or when the sphere is unreadable (keptUnknown). Otherwise cullRecordFaultSafe writes rec+0x208 and rec+0x234 (sealed_trial.cpp lines 430-437). Sphere: centre rec+0x240, radius rec+0x280 (constants lines 26-33).

### Hypotheses

- **H2 (mask re-OR'd after the store): REFUTED.** Ruled out: the record loop reads rec+0x234 and rec+0x208 right after the store of the same entry (D1, D3), with no writer in between, and the trial clears inside the store call (D5), before that read. A second entry reaching the same record calls the store and the trial again, so it is cleared again.
- **H1 (a second submitter that never goes through 0x4312040): OPEN; the two loops are not it.** Ruled out: a second builder caller that skips the store, because 0x4321ADB sits after the store at 0x43219BB (D2). Still open: a submitter reached by a virtual call (the traversal and the single-entry form have vtable-referenced callers at .rdata 0x5DDB604 to 0x5DDB614 and 0x5DD9F54 in Ghidra's caller list; not enumerated), or a builder with no caller found. The rel32 scan cannot find virtual or indirect callers. The skinned node string fRenderSkinningProcessorNode (0x5A6E170; lea at 0x10D8C54 in 0x10D8BD1 and 0x4C524A6 in 0x4C52320) and ShipRenderManager (0x5315870; lea at 0x2CC11D6 in 0x2CC11C0) are referenced only from functions that look like class registrations (name lea, constructor calls to 0x1401e3c80 and 0x1401fa180, vtable stores). INFERRED: neither is a per-frame submitter. The probe below decides it.
- **H3 (the visible ships are other records): OPEN.** Under D5 a cleared record cannot reach the builder in its own frame. So a visible ship drawn by the builder had its flag set after the trial, or was submitted by another path (H1). PROVEN from asset_census_20261012_045457.csv (trial on, 623 groups): 593 groups (2058 records) have main_admitted 0 and admitted_flag 0, including CobraMkV* parts at 790 m (for example group 117 onward, node+0 names). The census only names records the store visited, so it cannot say whether the visible hull is other records.
- **H4 (the trial keeps them: containment, unknown sphere, faults): PARTLY PROVEN.** keepRecord has no test on record type (D5). The same census keeps 11 groups (17 records). Six have no readable name: 416 (camera inside, 13.8 m, radius 38 m), 419 (24 m), 451 (33 m), 459 (21 m), 460 (31 m), and 457 (camera inside, 2,465 m). NPCs in the concourse, animated screens, posters and decals are within 80 m of an on-foot camera, so the trial keeps them by its own rule. INFERRED: this covers the on-foot list and flying ships within 80 m. It does not cover ships beyond about 80 m plus their radius, which the census shows cleared. Counters to read from the flight: keptUnknown, faults and torn in the window line.
- **H5 (new): stale flags on array members the store never visits.** PROVEN: the record loop covers every array member (count entry+0x30 and rbx+0x298), while the store visits only the group base and its child tree (D1, D4). INFERRED: a member outside that tree keeps the rec+0x234 and rec+0x208 values it last had, the trial never sees it, and the builder draws it each frame. That would explain drawn actors the census cannot name. OPEN. The probe below counts it.

### Hook candidates

- **Store 0x4312040 (already hooked).** Gates every record the store visits, children included, by sphere and not by name. Names are readable: rec+0x18 gives the node, node+0 the C string.
- **Builder 0x42B4420 entry (candidate for the H5 and H1 cases).** Every loop-admitted record passes it, with the record base at r9 - 0x210. Windows x64 convention: rcx = rec+0x290 (instance), rdx = context, r8 = window, r9 = rec+0x210, [rsp+0x20] = callback. Prologue `4C 8B DC 55 56 41 54 41 57 49 8D AB 08 FC FF FF` (decomp_42B4420 header; a CodeHook would need relocation). Confidence: high that it is a single admission-time point for both loops; low that it sees a submitter outside them.

### One-flight probe (designed, not built)

Read-only observers, no writes:
1. Builder entry: log the frame, record = r9 - 0x210, the name (node+0 C string), rec+0x208, rec+0x234, whether rec+0x2C0 is non-zero, the instance vtable RVA (rcx), and whether the store observer saw this record in the frame.
2. Store observer: per frame and per bucket, count the records passed to the store and compare with the bucket's count (bucket+0x30). Log the gap; for each unvisited member, log rec+0x234, rec+0x208 and the name.
3. Existing look probe on a visible hull: keep the VS hash and the bound vertex buffer, and log whether any builder call in that frame named the ship.

Reading the results:
- Builder calls for named actors whose store-visited flag was kept by containment: H4. Check the kept sphere distance against 80 m plus radius.
- Unvisited members with rec+0x234 = 1 and an actor name: H5 proven. The fix is a gate at the builder entry for those records.
- A visible ship with no builder call in its frame: H1 or H3. Read its VS hash. If it is one of the five known skinned VS (7B0D, 114A, D99A, 61AE, 8B58 in engine_velocity_families.h kSkinPairs), the skin path is the candidate.
- A zero gap in every frame and no actor-named builder call with flag 1 beyond the sphere: H1 or H3 with a submitter outside the loops.

Known VS in the repo: none of the 15 distinct VS in look_probe_20261012_051015.csv is in the known skinned set. AACFDCF2FB9AD809 is a family entry (engine_velocity_families.h line 30, PS CF534B32F491561A) with no skin flag, and it is not one of the five kSkinPairs (lines 70-76). INFERRED: the centre draws were not a known skinned NPC. This does not decide H1, because the list only holds known skinned VS.

### Not done

- Full reader and writer enumeration of rec+0x208 and rec+0x234. A displacement-only scan finds hundreds of unrelated structs with those offsets (the 0x234 scan printed well over 100 functions), so it needs a base-register anchor per site.
- The 0x4331300 per-item window and the builder body beyond its first arguments.
- Virtual callers of the traversal and the single-entry form.

## Hangar door state (RE, 2026-10-12)

Offline and read-only. Build 332841 (TimeDateStamp 1788384820, SizeOfImage 104894464, base 0x140000000), checked on the game-directory copy. Branch `claude/hangar-door-state` from `388289b3`. Nothing built, installed or flown. Scratch tools (not committed): a PE reader, a capstone linear sweep of `.text` (21.2 M instructions, rip-relative and direct call/jmp xrefs), and a `.pdata` function finder.

**Question.** Where is the docked / pad-lowered / hatch-closed value stored, and can EDVR reach it at render time?

**Answer in brief.** The four docking readers (0x12C3C20, 0x12CAF80, 0x12CADE0, 0x12CB320) are virtual methods of one class, called O here. Its name is not recoverable from the exe. The reader's `this` is the AAB8 object, which is O+0x58. The state byte is this+0x3C8. The docking name is a typed value at this+0x340. The owner is at this-8, and the owner's occupancy count is at owner+0x10. EDVR's reader detours already receive `this`. PadRetraction and HangarLiftDoors__ storage is not located. Byte offsets and reader logic are PROVEN from the exe bytes. The meaning of the values is INFERRED.

### Ruled out

- **RTTI route.** ruled out, because the exe has 19 `.?AV` type descriptors, all third-party (Wwise, MaskedOcclusion, std), and none is an Elite class.
- **Census vtable columns as a class key.** ruled out, because all 2583 rows of the four 20261012 census CSVs carry the same triple (model 0x50C7DE8, component 0x5596438, source 0x55967C0), hangar parts included, and record_vtable is 0 on every row. 0x50C7DE8 is not a vtable: the word before it is 0x6c and slots 2 and up are text.
- **Hangar part names as exe strings.** ruled out, because "HangerSmall", "BlastShield", "HangarComponent", "SmallHangerSkirt" and "LandingPadSmall" are not in the exe. They are asset data (node+0x0 names in the census).
- **Direct callers of the registrars and readers.** ruled out, because 0x12C8C54, 0x12C93A0, 0x12C0F11 and the four readers have no direct call xref. They are reached through vtable slots or the registry.
- **A +0x3C8 byte alone as the docked flag.** ruled out, because the reader return code also depends on the owner count, the owner flags and the name.
- **"Blast Shield Rotation Reset" and "Delay After Launch" as read by the four readers.** ruled out, because no rip-relative code reads 0x5E78C70 or 0x5E78C78. They may be indexed, and no reader uses them.

### PROVEN: the state-name table (0x5E78C40 to 0x5E78C98)

Pointers in .data to C strings:
0x5E78C40 State_Occupied; 0x5E78C48 State_LocalPlayerOccupied; 0x5E78C50 Ready for landing; 0x5E78C58 Inside_Hangar; 0x5E78C60 Outside_Hangar; 0x5E78C68 null; 0x5E78C70 Blast Shield Rotation Reset; 0x5E78C78 Delay After Launch; 0x5E78C80 Ready for landing; 0x5E78C88 StateTeleport_Inside_Hangar; 0x5E78C90 Outside_Hangar; 0x5E78C98 Inside_Hangar.

Code reads: 0x5E78C50 at 0x12C3C57 (reader 0x12C3C20) and 0x12CB336 (0x12CB320). 0x5E78C58 at 0x12C3C79, 0x12CAF96 and 0x12C6F95 (the "StateTeleport_%s" formatter 0x12C6F1F). 0x5E78C60 at 0x12C3C94, 0x12CADF6 and 0x12CB350.

### PROVEN: the object O

- **Vtables.** Constructor 0x12BA240 (2323 bytes, called from factory 0x12C0C70) stores vtable 0x51A9F00 at O+0x58 (0x12BA2F9, 0x12BA375) and vtable 0x51A9DD0 at O+0x0 (0x12BA34E). Deleting destructor 0x12BD010 is slot 0 of vtable 0x51A9DD0 and is reached through thunks 0x12BC304, 0x12BC310 and 0x12BC31C.
- **Owner.** The constructor stores its argument rdi at O+0x50 (0x12BA2D1). The readers reach it as [this-8].
- **Slots of vtable 0x51A9F00 (this = O+0x58).** +0x38 is 0x12CB310, which returns `this+0x3C8 == 2` (`cmp byte [rcx+0x3c8],2; sete al`). +0x88 is the getter 0x12C65A0. +0x90 is reader 0x12CAF80, +0x98 is 0x12CB320, +0xA0 is 0x12CADE0 and +0xC0 is 0x12C3C20. The census labels r0 to r3 are not yet mapped to these four addresses.
- **Getter (+0x88, 0x12C65A0).** `mov rax,[rcx-8]; cmp dword [rax+0x10],0; seta al`. It returns owner count > 0. The meaning of the count is INFERRED (occupancy of the pad).
- **State byte.** The constructor writes a word from bp at this+0x3C8 (0x12BAA02, `mov word [r14+0x420], bp`). Readers read the byte: 0x12C3C46 (`cmp byte [rbx+0x3c8], 2`), 0x12CB310 and 0x12CA510. Inside the class's virtual methods there is no writer. The writers outside the constructor are not found.
- **Name value.** this+0x340 holds a typed value. The readers call 0xA21560 (rcx = this+0x340, rdx = C string) for an equality test. 0xA21560 calls 0xA22140, which switches on a tag from 0x6495D0, and then reads the text through the value's vtable slot +0x10 (INFERRED from the instruction pattern). 0xA22380 checks that two accessors agree, vt+0x10 against vt+0x8 (INFERRED: a settled check).

### PROVEN: the four readers

Let N be true when owner count > 0, or owner+0x2C != 0, or owner+0x2F != 0. Name means the value at this+0x340.

- **0x12C3C20.** If not N: return 2 when this+0x3C8 == 2, else return 0. If N: name "Ready for landing" gives 0, "Inside_Hangar" gives 1, "Outside_Hangar" gives 2, and anything else gives 3.
- **0x12CAF80.** If owner count == 0: return 0. Otherwise return 1 when name == "Inside_Hangar" and the A22380 check holds, else 0.
- **0x12CB320 and 0x12CADE0.** Use "Ready for landing" and "Outside_Hangar" (see the table above). Not read in full.

### PROVEN: census rows (20261012 CSVs)

| capture | group | names | recs | dist min m | cam | admitted | mask0 |
|---|---|---|---|---|---|---|---|
| 045313 docked, seated | g1 | BlastShield / Door_ModularStyle | 48 | 1717 | 0 | 0 | 48 |
| 045313 docked, seated | g2 | BlastShieldBase / HangerSmall | 48 | 71.3 | 0 | 1 | 47 |
| 045313 docked, seated | g58 | SmallHangerSkirt / Agricultural_SmallHangar | 10 | 7.9 | 1 | 1 | 9 |
| 045313 docked, seated | g60 | HangerSmall / BlastShield | 10 | 25.3 | 1 | 1 | 9 |
| 045357 on foot in bay | g0 | BlastShield / Door_ModularStyle | 48 | 1.6 | 48 | 48 | 0 |
| 045357 on foot in bay | g1 | BlastShieldBase / HangerSmall | 48 | 131.9 | 0 | 0 | 48 |
| 045457 concourse | g1 | BlastShield_A / BlastShield_B | 48 | 790.5 | 0 | 0 | 48 |

The view mask of the blast-shield records follows the camera: masked from the seated view, admitted on foot in the bay. INFERRED: view bits are not a hatch signal.

### Access path

- **Render record to O.** Not found. Record to SRC (rec+0x2B8), then to X (SRC+0x160). X has the shared class (vtable 0x5596438) on every row. No pointer from X or the record to O was found.
- **Runtime path that exists.** The four readers are already detoured (`src\d3d11\state_dump_probe.cpp`), and the detour receives rcx = `this` = the AAB8 object. From `this`, the plain reads are: byte this+0x3C8, owner = [this-8], dword owner+0x10, bytes owner+0x2C and owner+0x2F. None of these needs a call, so they are safe inside the detour. The name at this+0x340 needs the A22140 decode before its text can be read. Until then, use the reader return codes as the name class.

### Value meanings (INFERRED, to be confirmed by the flight)

- this+0x3C8 = 2: docked, pad lowered. Evidence: the census's 02 02 02 02 while docked, and the reader's 2 branch.
- this+0x3C8 = 1: launch cue. Evidence: the earlier 01 01 00 00 at the launch cue.
- owner+0x10 > 0: a ship on the pad.
- Names: "Ready for landing", "Inside_Hangar" and "Outside_Hangar" are pad states. "Blast Shield Rotation Reset" and "Delay After Launch" are launch-sequence names. No code reads the last two yet.

### Not found

- **PadRetraction** (0x51AA198; also "PadRetractionCSM" at 0x5238598, lea at 0x1D7D5B0) and **HangarLiftDoors__** (0x51AA1C0). Both are registered by 0x12C8C54 with 16-byte inline names copied at 0x12C9165, and a default byte read at 0x12C9170 from 0x51AA1D0. Their storage is not located. The registrar and the readers are in the same module (0x12BA to 0x12CB), so O is the likely owner (INFERRED).
- **O's class name.** The registry table at 0x51AB2F0 pairs factory 0x12C0C70 (slot at 0x51AB338) with the descriptor vtable 0x51AB330. Registration 0x12B2415 (inside 0x12AF5B0) builds the descriptor from a runtime hash (`imul ..., 0x21`). The name object at 0x5EFD5C0 is zero in the file, so the name is not recoverable from the bytes.
- **Writers** of this+0x3C8 and this+0x340 outside the constructor.

### Probe (one flight, no new code beyond one logged field)

Use `hotkey.state_dump` (built: AAB8 pinned, +0x000 to +0x3FF tracked, the `AAB8.key3C0=` quad, reader codes per frame) with `diag.state_dump_frames = 6000`. The flight:

1. Docked and seated, pad lowered (Inside_Hangar), hatch closed. Press F3 and wait 10 s.
2. Take off: raise the pad, close the doors, launch. The dump keeps running. 6000 frames is about 100 s at 60 fps.

Per frame the dump already logs the reader codes and the `changes` column for AAB8. Add one field the dump does not yet track: owner dword +0x10, and owner bytes +0x2C and +0x2F.

Signatures:

- **S1 (this+0x3C8 is the docked enum).** Reads 2 while seated and docked, changes to 1 at the launch cue, before the pad moves. Refuted if it stays 2 through the pad raise and the launch.
- **S2 (owner+0x10 is the occupancy count).** Reads 1 when docked and 0 after undock. Refuted if it does not change on dock or undock.
- **S3 (names).** The reader codes move from 1 (Inside_Hangar) through 2 or 3 to 0 across the launch. Refuted if no reader code changes.
- **S4 (earlier trigger than geometry).** If this+0x3C8 changes before the LandingPad records move, the byte is an earlier trigger than geometry. Measure the lag in frames.
- **Control.** Repeat at a second station with the same pad type, to check that the value 2 is not station-specific.

Decision rule for the sealed cull (not yet decided): this+0x3C8 == 2, owner count > 0, and reader code 1, released on the first frame where the byte changes.

### Open

1. Find the writers of this+0x3C8 and of the name at this+0x340 in O's non-virtual methods and in the constructor's callers.
2. Decode A22140 and A21560 (variant tag and text) so EDVR can read the name safely.
3. Link O to the render records: find a field in X or SRC that points to O or to its owner.
4. Find the storage of PadRetraction and HangarLiftDoors__ (P9).

## Built v8 (2026-10-12, `claude/hangar-test-8`)

Commit `7a466ee3` (source; the branch also holds the merges of `claude/dynamic-actor-path` and `claude/hangar-door-state`). Full `build.bat` green (every gate, including `sealed_trial_test` at 80 checks and the new `builder_probe_test` at 48), then `build.bat --dll-only` green for the version stamp `v0.19.1-67-g7a466ee3`. Nothing installed, nothing flown. Every new key is TEMPORARY and default off; with no key pressed and the trial off, nothing is hooked.

Fix before the flight, commit `7a7f7e67` (DLL `v0.19.1-69-g7a7f7e67`, full build and dll-only green): the hatch probe clears its remembered objects on every toggle-on, the cap is 32 (was 8; the first refusal past it is logged once, with the distinct refused objects named), and each change line names the readers r0 to r3 that supplied the object.

v8.1 crash fix, commit `6063d2c8` (DLL `v0.19.1-71-g6063d2c8`, full build and dll-only green). Cause: in the flight with F2 on and docked, the store and builder detours inserted into `builder_probe.cpp`'s unordered sets with no lock, racing the frame tick (heap corruption, then an access violation in the hash insert inside the store's recursion). Fix: with only the trial on, the builder detour does no container work (an atomic count of calls whose rec+0x234 reads 0 at entry feeds `builder_after_cull=`, and `builder_unvisited=` leaves the window line); during the one F9 frame, all shared containers sit behind one collector lock that is never held across `forward()`, with reserves taken before the lock. The shared state of the sealed trial and the look probe is guarded by the same rule (the audit is in the commit message).

v8.2 geometry, commit `8bf70dc4` (DLL `v0.19.1-73-g8bf70dc4`, full build and dll-only green): while the hatch probe is on, one frame in six is sampled, and the store keeps the records named BlastShield, LandingPad, Lift, Hanger or Hangar (cap 64 a sample). A `hatch geometry:` line is written when one appears, changes (centre over 0.05 m, or a pose float over 1e-3, since its last logged value) or is absent for 30 samples. Centre is `rec+0x240` (proven); the pose at `rec+0x190` is logged as 12 raw floats because its layout is not proven (no evidence in the source or the docs).

v8.3, commit `5da8de9c` (DLL `v0.19.1-75-g5da8de9c`, full build and dll-only green; `builder_probe_test` 85, `hatch_geometry_test` 32, `sealed_trial_test` 102, config contract 159 keys). Four flight problems, and the named rule's refinement.

*Builder bracket (the v8.2 flight: "sixteen prologue bytes are not build 332841's", 13k lines).* The entry at 0x42B4420 already holds EDVR's own draw-item-builder bracket (`kinematic_eval_hook.cpp`, `bucketBuildObserved`), whose outer patch is a `jmp` (E9). Our CodeHook pin therefore refuses by design, and the refusal line now prints the bytes once. Findings: (1) *install condition:* the bracket is installed by `ensureInstalled()`, which the engine-motion consumers reach: `kinematicEvalEmitAttach` (engine_velocity.cpp, from `engineVelocityConfigure`, which temporal_pass.cpp calls with `g_temporalPassWantedFssChrome` in VR and `temporalModeEnabled` in flat), `kinematicEvalSchedulerAttach` (scheduler_stack_hook), `kinematicEvalGateProbeAttach` (object_probe, shadow_gate_probe) and `attachKinematicEvalHooks` (the kinematic probe). In a VR session with temporal AA on, it is installed. (2) *call sites:* it is an entry hook on the builder with no call-site filter, so it sees BOTH call sites (0x43204F3, return 0x43204F8; 0x4321ADB, return 0x4321AE0). The "verified primary call" filter (kinematic_eval_hook.cpp, the comment above `primaryBuildObserved`) belongs to the sub-builder 0x42B4130, not to this bracket. (3) *threading:* the bracket runs on the thread that calls the builder, takes no lock, and calls the observer before the forward. The observer uses the collector's lock; nothing is held across the forward. (4) *gate:* the bracket's relay is gated by `evalGate`, now open while an observer is attached (`builderWanted`). The new `kinematicEvalBuilderObserverAttach` attaches only when the bracket is already installed and never installs it. The path is chosen on a rising edge of the want and logged; the own detour is the fallback when the bracket is absent.

*Geometry (the v8.2 cap burnt in 12 s).* Samples are kept within 400 m of the camera; a moving record reports at most one line a second, and a `settled` line follows 2 s after it stops, with the move since its last line and the total since it appeared; each line names the camera, the distance to it, and the nearest Hanger/Hangar record's vector and distance within 400 m; the cap is 3000 lines a session. Positions are in the station's rotating frame, so the distance to the hangar record is the quantity to read.

*Named mode (costs 14x, and it kept too much).* The class cache is lock-free: one atomic word per node per frame (sealed_trial_core.h packs it), read from game memory only on a miss; the name lookup under the trial's lock is gone from the named path. The keep-list is replaced. The rule is the OWN PAD CELL: the nearest `LandingPad*` record whose sphere contains the camera (fallback: the nearest pad centre within 150 m; `GuideMarkers` excluded), its size from its name (Small, Medium, Large); the cell centre is the pad's centre; the cell radius is `diag.sealed_cell_small_m` / `_medium_m` / `_large_m`, **GUESSES 60 / 90 / 130 m** until measured. A record is kept when its CENTRE lies within the cell radius of the cell centre (not sphere overlap), or when it is the nearest instance of an explicit part (`Hanger*`, `Hangar*`, `BlastShield*`, `SmallHangerSkirt*`, `MediumHangerSkirt*`, `LargeHangerSkirt*`) to the cell centre within 200 m. Everything else is culled, named or unnamed, including records whose sphere contains the camera (`culled_containing=`). With no own pad, the frame is the sphere rule and the window line says why (`pad_reason=`). The cell is one frame behind: the pads of a frame choose the cell of the next. `hotkey.sealed_cell_larger` and `hotkey.sealed_cell_smaller` step the current size's radius through 30 40 50 60 70 80 100 120 150 200 m while named mode is on; the suggested bindings are PAGEUP and PAGEDOWN (unbound by default).

*What "unjudged" means in the code.* In the kept report, `kept::classify` labels a kept record `own` when its own sphere meets the trial sphere (contains the camera, or within R of it), `base` when an ancestor's sphere does, and `unjudged` otherwise: kept, but neither its own nor any ancestor's sphere explains it. In sphere mode the window's `kept_unjudged=` counts records whose centre or radius could not be read (kept, not judged), and `unreadable=` is that count. In v8.3 the report labels each kept record in named mode: `cell` (its centre within the cell), `explicit` (the nearest instance of a part), `other_named` (none of the above; should be rare), `sphere_no_cell` (no own pad this frame), or `unjudged_unreadable`. The kept report's CSV header carries the totals over all walked records, and the log line repeats them. The window's `kept_by_cell=` counts records kept by the centre test whose sphere does not meet the trial sphere, which is the named-mode analogue of "unjudged by name".

*Not done.* The eval cost of the named path was not measured on the rig: the rig cannot time game memory, so the class cache's hit path (one guarded read of the node pointer and one atomic load) is an estimate, and it still sits inside the trial's lock that sphere mode also takes. The pad's GuideMarkers exclusion and the cell radii are guesses to confirm on the flight. The report's labels assume the cell in force for the frame it observed.

v8.4, commit `0be37478` (DLL `v0.19.1-77-g0be37478`; full build green, then `build.bat --dll-only` green; `sealed_trial_test` 111, `builder_probe_test` 85, `hatch_geometry_test` 32, view source ok, config contract 159 keys). **INFERRED until flown.**

*Evidence (the v8.3 flight).* `rec+0x240` reads exactly the origin for dozens of records, and the rows show dx, dy, dz exactly equal to minus the camera. Four drones cannot share one point, so for those records `rec+0x240` is not the world centre. Hypothesis: the culling centre is the builder's own, computed from the pose it reads, `*(rec+0x290)`: q = pose+0x10, t = pose+0x20, c = pose+0x80, centre = R(q)·c + t (`analysis\decomp\decomp_42B4420.txt`, lines 196 to 237). Signature to read in the flight: `zero_centre=` counts the records whose `rec+0x240` is the origin; `centre_dist_m` p50, p95 and max give |`rec+0x240` − pose centre| over records where both read and `rec+0x240` is not zero. If the hypothesis holds, `zero_centre` is large, `pose_fallback=` is near 0, and the kept report's rows with `centre_source` pose have a centre that the raw column does not match.

*What changed.* `sealed_trial_core.h` has `builderCentre`, `chooseCentre` (pose, else `rec+0x240` counted as a fallback, else none), `isZeroCentre` and `percentileOf`. `sealed_trial.cpp` has one `recordCentreRead` for the store's sphere and named rules, the kept report and the window counters. The kept report gains `pose_centre_x/y/z`, `centre_source`, `raw_centre_zero` and `centre_dist_m` (24 columns). `hatch_geometry.cpp` uses the same centre. The window line gains `zero_centre=`, `pose_fallback=` and `centre_dist_m p50/p95/max`.

*The radius, from the decompile.* The builder does not reference `rec+0x280`. Its only inputs are the pose at its first argument (read at +0x10 to +0x9F), the context and the window. The four floats at pose+0x90 are passed, with the pose-derived centre, to `FUN_1404F4E10`, which tests a box at that centre against a plane set. So the builder's own extent is a box at pose+0x90, not a sphere radius (INFERRED from the call's shape; the callee's full body is not read). The trial's radius is still `rec+0x280`, and nothing in the decompile proves that `rec+0x280` is a sphere radius.

*Not done.* Nothing installed and nothing flown. The hypothesis is not confirmed; a flight reading the signature above decides it. The rig tests cover the arithmetic, the fallbacks and the percentiles, not the game's memory.

v8.5, commit `0d55bf4d` (DLL `v0.19.1-79-g0d55bf4d`; full build green: `sealed_trial_test` 121, `builder_probe_test` 85, `hatch_geometry_test` 32, `view_source_test` 45, config contract 159 keys; then `build.bat --dll-only` green). Nothing installed, nothing flown. The two v8.4 flight logs (`edvr_gfx_20261012_073419.log`, `edvr_gfx_20261012_074529.log`) are not on this machine, so the flight numbers below are the coordinator's reading, not re-read.

*1. The audit of the clear (PROVEN from the code).* rec+0x208 (u64 view mask) and rec+0x234 (u8 admitted flag) are written only by `sealed_trial.cpp`: `cullCounted` calls `sealed::cullRecordFaultSafe`, which writes the mask 0 and then the flag 0 through `RecordFields`. A grep of `src\d3d11` finds no other writer; `builder_probe`, `asset_census`, `cull_gate_probe`, `kinematic_eval_probe` and `shadow_gate_core` only read them. The write happens for each record the store writes, base and children. The store's observer (`storeObserved`, store_hook.cpp) runs after its forward returns, and the forward recurses through the children, so a child's clear lands inside its parent's forward and the parent's clear after it. The record loops (0x43203F1 and 0x43219BB) read the flags after the store returns. Compared with 7fed41c8, the write itself is identical: the same offsets, the same `writeMem`, the same fault-safe order. v8 added a recursive mutex held only while the observer runs (not across the forward), a named keep rule that only chooses which records are kept, and three observers after the trial's (census, builder, geometry), none of which writes those fields. No v8 change defers, copies out or drops the write. Answer: unchanged; the write lands before the loop reads the flags. It has no readback on the hot path, so the flight is the test.

The evidence the other way is item D. If the four builder calls per frame (HangarLobby, HangerSmall, LandingPadSmallA, SmallHangerSkirt) were taken with the trial on, the record loops already skip cleared records: the flags do control the builder loop. Then the census not falling (C) means the census measures draws the builder does not feed, or draws from a list built before the clear. `all` mode separates those cases.

*Ruled out (by code reading; the flight of `all` confirms or refutes each):*
- ruled out: a second writer of rec+0x208 or rec+0x234 undoes the clear, because the grep finds no other writer.
- ruled out: the clear is deferred, copied or lost in v8, because the write path matches 7fed41c8 and no v8 change runs between the observer and the loop's read.
- ruled out: F2 resets the mode, because the toggle never writes it: `g_mode` is set by `sealedTrialInstall` (startup, from diag.sealed_trial_mode) and by `sealedTrialModeStep` (the mode key). A reset the pilot saw came from a restart (the install re-reads the ini) or a mode press miscounted. v8.5 logs the rule at each toggle-off.
- ruled out: the bracket's relay gate closes during a collected frame, because `evalGate` stays open while `builderWanted` is set, and the want (`builderProbeWanted`: a frame armed or collecting, or the trial on) is true from the arm to the write. v8.5 logs the gate at the start and the write.
- explained: "builder hook not wanted yet" is the own detour's status (`g_builder.status`), which never installs when the bracket is the path. It said nothing about the probe's want.

*2. Mode `all` (`diag.sealed_trial_mode = all`; `hotkey.sealed_trial_mode` cycles sphere, named, all).* While the trial is on, every record the store visits is culled with no view, sphere, name or pose test, and the record's rec+0x290 kind is still counted. The window line says `mode=all`. Reading: if the screen does not go nearly empty and the census draws do not collapse, the store flags do not control the drawing, or our write does not land.

*3. F2 keeps the mode.* The toggle does not change `g_mode`; the toggle-off line now says the rule stays. The code already kept the mode across the toggle, so the reset the pilot saw is not explained by this code.

*4. The builder path and gate (item D).* The F9 summary names the live path (bracket, own detour, or refused), what the bracket said at the attach, and the bracket's relay gate at the frame's start and at the write. An arm asks for the path again unless the bracket is already the path, so a bracket installed after the first attach is picked up. Still open: why the v8.4 F9 frames had four calls with the trial on; the new summary line answers it.

*5. The rec+0x290 kind (item A).* The window line now counts, per store visit, whether the pointer at rec+0x290 is null, inside the game image, another non-null value, or unreadable (`ptr290 null= image= other= unread=`), and the kept report has `ptr_290` and `ptr_290_kind` columns. The v8.4 fallback reason ("pose unreadable") covered a null pointer, a failed read, and a readable pointer whose fields failed; v8.5 separates the first two. The pose fields behind a non-null pointer are not counted here.

*6. What the store uses to place a record (`analysis\decomp\decomp_4312040.txt`, FUN_144312040, the per-record function).* Read by hand from the decompile text (one reading, not cross-checked): the local centre and radius come from the node block, `*(*(rec+0x18)+8)`: the centre at +0x20 to +0x28 and the radius at +0x2C. The world centre is that local centre through the transform in the pose block at `*(rec+0x190)`, and a matrix at `*(pose+0x50)`. The store's sphere branch reads `rec+0x240` (centre) and `rec+0x280` (radius) when `rec+0x290` or `rec+0x2B0` is non-null. Children are the array at `rec+0x2A8` (count `rec+0x2B0`), each visited as its own record through the recursive call. INFERRED: a child is placed by its own node and pose block, not by the base's. The decompile drops the child argument of the recursive call, so this is read from the surrounding lines, not shown. The answer to "what does the store use for a child" is therefore: its own node block and its own rec+0x190 pose, and rec+0x240 only in the sphere branch.

*Not done.* No flight, no install. The mode `all` result, the F9 path line and the pointer kinds are the flight's to read. The child placement is INFERRED until a flight or a fuller decompile shows the recursive call's argument.

### Part 1: the builder 0x42B4420, arguments and return (PROVEN from the exe bytes, capstone 5.0.7)

- **Prologue** `4C 8B DC 55 56 41 54 41 57 49 8D AB 08 FC FF FF` matches. Both call sites 0x43204F3 and 0x4321ADB are `call 1442b4420`. The return addresses are 0x43204F8 and 0x4321AE0; `builder_probe_core.h` classes a call by them.
- **Correction to Dynamic actors D3 and "Hook candidates".** rcx is not the address rec+0x290. The caller loads the VALUE at rec+0x290 (`mov r11,[rbx+0x5C]`, with rbx = rec+0x234), so rcx is the instance pointer stored in the record. The probe therefore checks agreement by reading the memory at (r9 - 0x210) + 0x290 and comparing it with rcx. r9 = rec+0x210 (an address), so the record base r9 - 0x210 stands.
- **The six arguments at the call.** rcx = the value at rec+0x290; rdx = `*ctx` (the context's first qword); r8 = the window value `[rsp+0x60]`; r9 = rec+0x210; `[rsp+0x20]` = the callback rec+0x2C0 (or 0 when the global byte at RVA 0x5EA3483 is 0), always written; `[rsp+0x28]` = `[rdi-0x28]`, always written. Both stack slots are written on both paths of the `je`.
- **Argument count: six.** The builder reads rcx, rdx, r8 and the sixth argument (`[entry+0x30]`, read at 0x42B47F8 and 0x42B4D14). It never reads the fifth (`[entry+0x28]`). It does not read r9 as an incoming argument. No seventh argument is read. The observer therefore takes and forwards six arguments.
- **Return value: undefined.** One `ret` at 0x42B50B5. RAX there is left from the security-cookie call, not set by the builder. Both callers ignore RAX, and no argument is read back after either call. The callers reload rcx, rdx, r8 and r9 on each iteration.
- **Writes through the arguments: none directly.** The builder appends draw-command records (0x150-byte entries) to a per-pipeline command table (pointer slot at `[list+0xAA0+n*8]`, `inc [list+0x18]`), allocates list nodes through 140540DB0 when a bucket is full, and increments the dword at `[pipeline+0x2A4]`. It also calls 143696FA0 (a bucket lookup, not traced) and 817260 and 4C835D0 (not traced).
- **Skip verdict: NOT PROVEN SAFE, so skip is refused.** Caller-visible state is unchanged by a skip, but a skipped record loses its draw-command appends, and the counter at `[pipeline+0x2A4]` and the effects of 143696FA0 are not shown to stay consistent. The count runs in every case.
- **Why skip would change little anyway (INFERRED, to test in flight).** The admission loop tests rec+0x234 before it calls the builder (D3), and the trial clears rec+0x234 inside the store call (D5). A record the trial culled should therefore never reach the builder, so `builder_after_cull=` should read 0. A non-zero value refutes that and is a finding in itself.

### Part 1: the probe (`hotkey.builder_probe`, `builder_probe.cpp`)

- **Install.** The detour goes in on the first want: a press, or the sealed trial on. The pin is the 16 prologue bytes; a mismatch refuses the install and logs why. The relay gate is closed unless a frame is armed or collecting, or the trial is on.
- **Collection.** A press arms one frame (`builder probe: armed`). The next frame's builder calls are recorded with the entry state: the record, its name (rec+0x18 to node, node+0 to text, the census's accept rules), mask (+0x208), admitted flag (+0x234), sphere (+0x240, +0x280), distance from the frame's camera, the caller, and the rcx agreement. The store visits of that frame are recorded, and each call is classed by whether its record was visited (`store_seen`).
- **Writing.** At the Present after the collected frame: a `builder probe: frame F:` summary (calls, distinct records, calls whose record the store did not visit, agreement and disagreement, callers A/B/other), name lines of eight names each (top 20 by calls, with the unseen count), a line naming the builder hook's status, the store hook's status and the camera's source, and `edvr_logs\builder_probe_<stamp>.csv` (one row per name, then the calls, capped at 4000 rows).
- **Trial window.** While the sealed trial is on, each frame's calls are classed. The window line gains `builder_after_cull=` (calls for records the trial culled in that frame) and `builder_unvisited=` (calls for records the store did not visit in that frame), and `mode=`.
- **Reading a zero.** A zero summary with the hook installed means the builder was not called for any record that frame. A zero with the hook refused means nothing was recorded, and the refusal line says so before the summary.

### Part 2: the hatch-state probe (`hotkey.hatch_probe`, `state_dump_probe.cpp`, `hatch_probe_core.h`)

- **The AAB8 pin is removed.** It matched by address suffix and never fired. The reader detours now remember every distinct `this` they are called with (up to 32, see the fix line above), while the observers are armed (the hatch probe on, the census, or a dump). The state dump's general tracks already cover +0x3C0, so the key bytes are still in its CSV. The census's `# hatch state` line lists the remembered objects (four at most), not the pin.
- **On.** The four reader detours are installed if not yet in (their status is logged). The eight state-name table pointers at 0x5E78C40 are logged once (`hatch probe: state-name table at image+0x5E78C40`), so the +0x340 bytes can be compared with them offline.
- **Each frame.** For each remembered object: the dword at +0x3C8, the 16 bytes at +0x340, the owner (`[this-8]`), owner+0x10 (dword), and owner+0x2C and +0x2F (bytes). A line is written only when one of these changes: `hatch probe: object O frame F t=T ms: 3C8 a->b; 340 <hex>-><hex>; owner ..; owner_10 ..; owner_2C ..; owner_2F ..; status flags=.. flags2=..`. The first read of an object is the baseline. Lines are capped at 400 per session; the off line gives the count written and the count suppressed.
- **Off.** `hatch probe: off at frame F`, with the lines written and suppressed.

### Part 3: the named sealed-trial mode (`diag.sealed_trial_mode`, `hotkey.sealed_trial_mode`)

- **Sphere** (default): the sphere rule, as before. **Named:** a record with an unreadable sphere is kept as before (unjudged). Otherwise it is kept when its name holds Hanger, Hangar, BlastShield, LandingPad, Lift or Skirt (case-insensitive), or it has no readable name, or its radius is 100 m or less and its sphere meets the trial sphere. Everything else is culled, including a giant record that contains the camera.
- **Cycling.** `hotkey.sealed_trial_mode` cycles the rule while the trial is on. The window of the old rule closes first, so each rule has its own window line. Off, the key logs a refusal.
- **Names** are cached per node for the frame (direct-mapped, 512 slots), so the store does not allocate per record.
- **Kept report.** The name is the last column (18 fields, `(none)` when unreadable).
- **`diag.sealed_trial_builder = count|skip`.** skip logs its refusal at install and counts only.

### Keys added

`hotkey.builder_probe`, `hotkey.hatch_probe`, `hotkey.sealed_trial_mode`, `diag.sealed_trial_mode`, `diag.sealed_trial_builder`. All are on the flat allow-list in `runtime_profile.h` under a TEMPORARY comment, commented in the `edvr.ini` template with explanations, and listed in the Status block. The config contract passes (154 keys read, 154 documented).

### Flight plan (one session, with the trial and both probes)

1. Docked, seated, trial on in named mode (`hotkey.sealed_trial_mode`, then `hotkey.sealed_trial` at 80 m). Press `hotkey.builder_probe` and wait one frame. Read `builder_after_cull=` (expected 0; a non-zero is a finding) and the summary's unseen count.
2. Watch ships outside, then in the station, with a builder probe capture at each. Note which named actors are called with `store_seen=no`.
3. On foot in the concourse, with a builder probe capture.
4. Hatch probe on before docking, through a full launch and re-dock cycle (the pad raises, the doors close, launch, fly, dock, the pad lowers). Expect `3C8` to change before the LandingPad records move (S4), and the owner count to change on dock and undock (S2).

### Refutation criteria (H5 against H1 and H3)

- **H5 (stale flags on members the store never visits)** holds for an actor if a builder call names it with `store_seen=no` in a frame where the actor is drawn. It is refuted if every named actor call in visible-actor frames has `store_seen=yes`, and the unseen count is zero in those frames.
- **H1 (a submitter outside both loops)** is supported if an actor is drawn in a frame with no builder call naming it. The look probe's draw record for the frame gives the draw; the builder summary gives the calls. It is refuted if every drawn named actor has a builder call in its frame.
- **H3 (the visible ships are other records)** is supported if the calls naming a drawn ship come from records that no census row names. It is refuted if every drawn ship's calls name records already in the census.
- **H4 (containment keeps near-camera actors)** is checked by the named mode alone: if the named rule removes the on-foot list (the actors within 80 m that the sphere rule keeps), H4 is the mechanism.

### Not done

- The skip is not implemented, and its side effects are not traced (143696FA0, 140540DB0, 817260, 4C835D0).
- The hatch probe does not decode the name at +0x340 (A22140). The 16 bytes are logged raw; comparison with the table pointers is offline.
- No flight. No install. No merge to main.

Co-Authored-By: Claude Haiku 5.5 <noreply@anthropic.com>


## Draw paths (RE, 2026-10-12)

Offline, build 332841 (gate matched). No C++ changed, no build, no flight. Sources: `analysis\decomp\decomp_*.txt` (Ghidra; line numbers are the file's lines), the exe bytes (`analysis\EliteDangerous64.exe`, read with a PE reader in the scratchpad), and the docs cited. **PROVEN** = bytes or decompile lines cited. **INFERRED** = not closed by the bytes.

### Correction: the .rdata and .pdata "callers" are range records

- PROVEN: every .pdata or .rdata reference that Ghidra lists as a caller of a draw function is a 16-byte record of the form (leading dword, begin RVA, end RVA, unwind RVA). Examples: RVA 0x5DD54FC holds `042B4420 042B4478 05DD54C4` (begin 0x42B4420, end 0x42B4478); RVA 0x5DDB604 holds `04320340 04320370 05DDB5D4`; RVA 0x5DDA058 holds `04312040 0431207B 05DDA034`. They are the hot and cold ranges of split functions, not vtables or job tables.
- Consequence: the "vtable-referenced callers" in the Dynamic actors H1 entry have no support. Indirect callers through 8-byte function-pointer tables are NOT ruled out; no scan for those was run.

Direct call sites (PROVEN, from the Ghidra callers lists and the decompiles):
- 0x42B4420 (builder): 0x43204F3 in the traversal 0x4320340; 0x4321ADB in the single entry 0x4321940.
- 0x4312E00 (producer B): 0x43125DF in the store 0x4312040, and nowhere else.
- 0x4312040 (store): 0x43203F1 (traversal), 0x43219BB (single entry), 0x4312650 (recursion over the child list).
- 0x4320340 (traversal): called from an unnamed region at 0x1442DFAF3 (after FUN_1442DF9D0; no Ghidra function there).
- 0x4321940 (single entry): called from 0x431AFE0 only (decomp_431AFE0 line 124). The "0x431B20D" call in the Dynamic actors notes lies inside 0x431AFE0's range (0x431AFE0 + 699 = 0x431B29B), not in 0x431B2A0.
- 0x431AFE0 (rig eval): called at 0x1442DF47A in FUN_1442DF460 (31 bytes; decomp_42DF460). Its other dispatch is the collection's object vtable slot +0x50 (decomp_431AFE0 line 127), used when collection+8 is non-null.
- 0x42B4130 and 0x42B3FC0: builder only (decomp_42B4420 lines 369 and 512).
- 0x369C9C0 (producer C): 0x36A0F50 only, at four sites (decomp_36A0F50 lines 60, 74, 89, 98).
- 0x434D470 (producer D driver): from 0x434D3C4 (in 0x434D320), 0x434D421 (in 0x434D3E0), 0x434D460 (in 0x434D440).

### Q1: every appender

An appender writes a 0x150 pool record to a node from DAT_145EFDD30 and increments the per-pipeline draw count at [owner+0x2A4].

| Path | Function | Appends (PROVEN) | Reads rec+0x208 or rec+0x234? | What it iterates |
|---|---|---|---|---|
| B | FUN_144312E00 (hot split chunk 0x431305C) | yes: 4312E00 lines 179-250, key rec+0x250 (179), +0x2A4 at 250; chunk line 115 | no. Its mask is the store's per-view result (4312040:326-337) | one rig record, mask words up to 7 qwords |
| A | FUN_1442B4420 builder | yes: inline appends at 663-734 (owner = *(pauVar24[2]); +0x2A4 at 734), and through 0x42B4130 | no. Reads the instance pose (rcx), ctx (rdx), window (r8) | group members admitted by the traversal; sub-item list at instance+0x14 (0x58 stride, count +0x12), or parts of instance+0x28 |
| A' | FUN_1442B4130 primary emit | yes: lines 141 (alloc) and 203 (+0x2A4) | no | parts of instance+0x28, one per mask bit from 0x142817260 |
| C | FUN_14369C9C0 | yes: calls 140540DB0 and 143696FA0 (lines in decomp_369C9C0) | no | lists of a non-rig object (0xA0, 0x60, 0x70, 0x80 strides), gated by ctx masks +0x570, +0x580, +0x690 (36A0F50 lines 35-45) |
| D | FUN_14434D120 via FUN_14434D470 | yes: calls 140540DB0 and 143696FA0; INC per doc | no | one record per call, a packed 4-vector built from a transform |

Not appenders (PROVEN: no [owner+0x2A4] write in their decompile; they touch collection-level +0x2A0 lists and node allocation): 0x4316820, 0x4328B80, 0x431B2A0, 0x430A060, 0x431B4D5, 0x431F31F, 0x432A4E8, 0x43697140. INFERRED: list management. 0x4331300 is the group's per-view mask and window evaluator (it loops ctx+0x1A940 views with stride 0x6A0, decomp_4331300 line 112). It is not an appender.

Gates, not appenders: 0x431AFE0, 0x4320340, 0x4321940, 0x4312040, 0x430EFE0.

The main-view chain. PROVEN up to the dispatch, INFERRED after it:
1. FUN_1442DF460 calls 0x431AFE0 per rig. 0x431AFE0 returns at once unless rig+0x380 == 4 (line 39). It ORs view masks +0x570 for the views that pass FUN_142840840 and FUN_14432C520 (lines 44-76) into uVar9. It clears ctx+0x1A950 bits when collection+0x70 bit 7 is set (lines 80-87). It then either calls the collection's dispatch object vtable +0x50 (collection+8 non-null, line 127) or calls 0x4321940 synchronously (line 124).
2. INFERRED: the dispatch object runs the traversal 0x4320340 as a job. Its caller is not found.
3. 0x4320340 walks up to 8 groups (entries of 0x60 bytes at collection+0x40, count at +0x300). For each group it calls 0x4331300 (mask and window), then the store 0x4312040 on the group's record array base (entry+0x28). If the pipeline pointer is non-null it runs the admission loop over all members (count entry+0x30, stride 0x2F0) and calls the builder (lines 40-108).
4. The store 0x4312040, per record: LOD nibble filter, sphere test, per-view gate, producer B when the rig kind is 2, the pose update, and recursion over the child list (rec+0x2A8, count rec+0x2B0). Producer B runs inside the store (line 337).
5. The single entry 0x4321940 is item 3 for one record.

### Q2: which path draws what

- (a) Station static geometry: path A, the builder. PROVEN: the kinematic doc (capture 165433) finds builder parts behind 85.4% of static t33 records. FUN_1442B4420 made 252,659 calls and 3.82M items, against 430,886 items from FUN_144312E00 (engine-render-performance-2026-09-19 lines 586-596). INFERRED for named walls; the probe decides.
- (b) Ships, drones, turrets, rigid movers: path B, FUN_144312E00. PROVEN: the kinematic doc says moving pool records are rig records and "NOT the draw-item builder's records" (2 of 82 movers claimed by builder parts; kinematic doc lines 1683-1690). PROVEN: FUN_144312E00 copies the rig's +0x170/+0x178 and +0x17C/+0x180 into the record (4312E00 lines 66-76). INFERRED: the singleton items (max 3 per call) are ship-scale parts.
- (c) Characters (skinned): not path B. PROVEN: "Skinned records never reach the FUN_144312E00 emit hook (log tainted 0)" (kinematic doc line 4067). INFERRED: they go through the builder (path A), through either the instance+0x28 branch or the sub-item branch. The probe does not separate these yet.
- (d) Decals, screens, billboards: NOT FOUND. Producer D is the only candidate outside the rig path. Its callers (0x1435A0720, 0x143AE9130, 0x143AF1240, 0x144714FE0, 0x1430656F0) lie outside the draw cluster and were not traced.

### Q3: where the visibility decision is made

Path B (producer 0):
- The store computes each record's world sphere inline (4312040 lines 212-262). Centre comes from the node's bounds block: *(node+0x08)+0x20 (xyz), radius *(node+0x08)+0x2C. It is transformed by A = *(rec+0x190) and B = *(A+0x50) (PROVEN offsets; B's role INFERRED as the base transform). The radius is the raw node radius, not scaled by the instance (PROVEN: line 262 copies it unchanged).
- Per view, FUN_14430EFE0 (store line 304) tests the sphere against the view: centre minus view origin (view+0x540 to +0x54C), a distance threshold from view+0x550 and +0x560, and the view mask view+0x570 (decomp_430EFE0 lines 76-99, 125-160). Its struct argument, by PROVEN offsets: [2] = rec, [3] = pointer to the candidate mask (local_288), [5] = pointer to the world centre, [6] = pointer to the radius, [11] = pointer to a store-local byte.
- The surviving view bits (local_288) go to FUN_142817260 and then to producer B (4312040 lines 326-337). This gate does not change rec+0x208 or rec+0x234 (see Ruled out).

Path A (builder):
- Record level (traversal lines 64-86): rec+0x290 non-zero, rec+0x234 set, rec+0x298 set, and some set bit v of rec+0x208 whose LOD nibble (rec+0x210) is at most node+0x6A.
- Store level: LOD nibble filter against node+0x68 (4312040 lines 137-149), and FUN_144308B30 on the record's sphere (lines 156-165; decompile not read). Both write rec+0x208. rec+0x234 is set from the nibble-filtered mask before the sphere test (line 150).
- Builder level (42B4420 lines 194-275): the pose at rcx gives q (pose+0x10), centre (R(q)c + t, with c at pose+0x80), and extent (pose+0x90). For each view with (view+0x570 & window r8) non-zero, FUN_1404F4E10(view, centre, extent) must not return -1 (a box test; leaf, decompile not in the set, bytes at 0x4F4E10). Then, if DAT_145EA3399 and view+0x68D bit 0 are set, FUN_14288AC40 (visibility callback; bytes at 0x288AC40) must return true. Surviving views OR into local_4B8.
- Sub-item branch: FUN_1442B3FC0 per (sub-item, view) (lines 41-113): a distance test against view+0x540 and the thresholds view+0x550 and +0x560, using the pose centre, then FUN_1404F4E10.

Hook points (addresses and prologue bytes PROVEN from the exe):
- V-A: builder 0x42B4420. Prologue `4C 8B DC 55 56 41 54 41 57 49 8D AB 08 FC FF FF` (matches the Built v8.3 note). At entry: rcx = instance (the value at rec+0x290), rdx = ctx, r8 = window mask, r9 = rec+0x210 (so the record is r9 - 0x210), [rsp+0x20] = callback, [rsp+0x28] = f. Name: rec+0x18 gives the node, node+0 the C string. EDVR already hooks this address: kinematic_eval_hook.cpp bucketBuildObserved (about lines 538-560) calls an observer before the forward. That observer returns void and the forward is unconditional, so a veto needs a new return protocol (the bracket returns 0 before the forward when the observer asks for a skip). Confidence: high that it is the single point for every builder call from the traversal and the single-entry path, since one entry covers both direct sites. Medium-high that a skip before the forward is side-effect free: the builder's global writes are the appends after line 663.
- V-B: producer B, FUN_144312E00. Prologue `40 55 53 56 57 41 54 41 55 48 8D AC 24 38 FF FF`. At entry: rcx = rec (name rec+0x18 to node), rdx = owner (pipeline), r8 = mask array (7 qwords), r9 = tail. EDVR already has a direct-producer relay (kinematic_eval_probe.h line 45, kDirectBuildRvas 0x4312E00), which observes after the forward. A veto needs the observer to run before the forward and return skip. Side effects before the appends: the dictionary lookup FUN_143696FA0 (pure on a hit) and, on a miss, an empty node from FUN_140540DB0 (no draw effect). Confidence: high; its only caller is the store.
- Observer only, for the trial's sphere (not a veto): the store's per-view gate FUN_14430EFE0, prologue `40 53 55 56 57 41 54 41 56 41 57 48 83 EC 70 48`. On entry it gives the record and its true world sphere, which the trial should use in place of rec+0x240 and rec+0x280.
- Not recommended: a veto at the store entry 0x4312040 (prologue `40 55 53 56 57 41 56 41 57 48 8D AC 24 68 FE FF`). Zeroing its input mask also skips the pose update FUN_14433DB20 (store line 153), which the children and the builder depend on.

### Q4: the zero rec+0x240 and the unreadable rec+0x290

PROVEN (decomp_433DB20):
- Lines 298-305: rec+0x240 to +0x24C are written only when rec+0x2B0 (child count) or rec+0x290 (instance pointer) is non-zero. Otherwise they keep their old value.
- Lines 306-355: the instance pose is written (FUN_1442B6410(rec+0x290, ...)) only when rec+0x290 is non-zero.
- Lines 391-397: the no-child, no-instance case takes no centre.
- 4320340 lines 64-67 admit only records with rec+0x290 non-zero, so a record with rec+0x290 = 0 never reaches the builder.

So a record with no children and no instance has rec+0x240 = origin (never written), and *(rec+0x290) is a null read. That is the roughly 70% in the v8.3 flight.

INFERRED: these are leaf records of the rig family. They carry a rig link (rec+0x2B8), so they can reach producer B when the rig kind is 2. Their world transform is formed in the store from A and B (4312040 lines 239-261). They are not a different record type, and I found no evidence of instanced batches.

Consequences:
1. rec+0x240 is not the sphere centre for leaves, so the trial's origin-based decisions for them measure nothing about the record.
2. The v8.4 pose-derived hypothesis has no pose to read for these records.
3. These records cannot be builder draws. Producer B runs before the trial's post-store clear, so the trial never affects them.

### Ruled out

- ruled out: rdata and pdata references as vtable callers, because each is a range record (bytes above).
- ruled out: the post-store trial clear reaching producer B, because producer B is called inside the store (4312040 line 337) and the trial runs after the store returns (store_hook.cpp lines 106-115).
- ruled out: the store's per-view gate writing rec+0x234 or rec+0x208. Both derive from the LOD-filtered mask (4312040 lines 150 and 346); local_288 feeds only FUN_142817260 (lines 326-337). This confirms the correction in the Dynamic actors section.
- ruled out: leaf records as builder draws, because admission requires rec+0x290 non-zero (4320340 lines 64-67).
- ruled out for this capture: FUN_14369C9C0 as the station-geometry producer, because it made zero calls in 11.7k frames (engine-render-performance-2026-09-19 line 594). Not ruled out for other scenes.
- ruled out: 0x4316820, 0x4328B80, 0x431B2A0 and 0x430A060 as draw appenders (no [owner+0x2A4] write in their decompile).

### What the flight result implies (INFERRED)

The trial clears rec+0x208 and rec+0x234 after the store. That reaches path A only, and only for records the store visited. So:
- Producer B draws (ships, drones) are untouched by the trial.
- Most store-visited records are leaves that the builder never admits, so clearing them removes nothing that is drawn.
- Instance-bearing records (walls, walkers) are drawn by path A. They survive when they are admission-loop members that the store never visited. That is H5 (stale flags on members outside the group base's child tree). It is the leading explanation for "the draw count does not fall". The v8 counter builder_unvisited= tests it.

### One-flight probe (designed, not built)

Armed frame with the trial on: docked and in the concourse, plus one frame in open space with a ship in view.
1. Producer B pre-forward observer (new, read-only). Per call: rec, name, rig kind (rig+0x160 to +0x4B), popcount of the mask, owner+0x2A4 delta.
2. Builder bracket observer (exists since v8.3). Per call: return address (site), rec = r9 - 0x210, name, instance non-zero, instance+0x28 non-zero, instance+0x14 count, store_seen (v8), and the key of each appended record. Builder keys are the sub-item address +0x40 (42B4420:663), not rec+0x250.
3. Per group: entry+0x30 (count) against store-visited records. builder_unvisited= already logs this.
4. Draw side: the census row for each drawn record and its pool key. Producer B keys are rec+0x250 (4312E00:179). Map each key back to its rec.

Read-out:
- Station wall drawn, its rec in path A with store_seen = no: H5 supported.
- Station wall drawn, store_seen = yes and flag cleared: contradicts the clear. H1 or a missed input.
- Ship drawn via producer B: confirms the trial cannot reach ships, so a veto must sit at V-B.
- NPC drawn via path A with a named sub-branch: resolves the walker sub-branch. An NPC drawn with no A or B record: H1.
- Decal or screen drawn with no A or B record: producer D (14434D120). The probe should count it too.

### Recommended hook points

1. V-A: builder entry 0x42B4420 (existing bracket). Confidence high for path A coverage. The bracket needs a skip return.
2. V-B: producer entry 0x4312E00 (existing relay). Confidence high for path B (sole caller).
3. Trial sphere source: observer on 0x430EFE0 entry (no veto). Confidence high on the offsets. Caution: the radius is the unscaled node radius.

No key added, no config change, no install.

## Built v9 (2026-10-12, `claude/hangar-test-8`): the draw-list probe

Commit `7cb314d5` (DLL `v0.19.1-83-g7cb314d5`). The draw-path doc (`5dd8bd57`) was merged first (`fa3532b5`, doc only). Full `build.bat` green: `drawlist_probe_test` 38 checks, `sealed_trial_test` 121, `builder_probe_test` 85, config contract 160 keys; then `build.bat --dll-only` green. Nothing installed, nothing flown. The v8.5 flight log (`edvr_gfx_20261012_080922.log`) is not on this machine; its numbers are the coordinator's reading.

*Why.* In the v8.5 flight, `all` cleared about 916k records per 5 s with kept=0, yet less disappeared than with sphere or named. With the trial on, the builder ran about four times a frame while the walls still drew. Ships and drones come from producer B (FUN_144312E00), which runs inside the store before the trial's clear. The engine-motion logs show about ten producer-B calls a frame against 4,000 to 7,000 draw calls. **H-PERSIST (INFERRED):** each collection's draw list or pool persists across frames and is rebuilt only when something in the collection is admitted; clearing everything leaves the previous list drawing.

*What the probe records* (one window: three consecutive frames from the next frame tick, written at the tick after the third):
1. **Path A (builder)**, from the bracket observer, per call. The key is the instance pointer (rcx). The bracket's arguments do not carry the sub-item address (+0x40) the draw-path doc names, so that key is not used. Each call is taken as an append for the carried logic, since the builder's own append cannot be seen. One row per distinct key per frame.
2. **Path B (producer)**, from the emit observer, after the forward. `before` and `after` are owner+0x2A4, so the appended count is exact. The key is the rig record's +0x250, the key FUN_143696FA0 uses to pick the owner's list.
3. **Pool copy, merge and dictionary clear** calls per frame, with the raw collection pointer each observer receives (mapped base, destination, dictionary). The pointers are not joined across observers.
4. **Substituted pool draws per family**, from the engine-motion draw counter (owner thread). Exact for substituted draws only. The main-view scene draw total is NOT counted: the draw census counts only while its own capture is armed, and the occlusion census uses queries.
5. **Carried candidates**: a key appended in an earlier frame of the window and not in this one. A candidate is not a draw. The probe cannot say whether it was drawn.

*The join: how draws map to records.* **Not live.** A scene draw is not mapped to its pool record on the CPU. Engine motion's draw side writes each pixel's t33 slot to MRT6 in the substituted pixel shader, and the temporal pass reads it on the GPU (`engine_velocity.h`, the DRAW half). The CPU never reads which pool record a draw used. The "pool records joined" counter (`engine_velocity.cpp`, the summary line) is the emit's history join: the previous pose per record. It is not a draw join. So every scene draw's origin is **unattributed**, and the summary says so. Producer keys map back this way: B by rec+0x250 to the rig record, whose name is rec+0x18, node, +0; A by the instance (rcx = the value at rec+0x290) with rec = r9 - 0x210 for the name.

*What would appear if the probe never ran.* No `drawlist probe:` lines and no `drawlist_*.csv`. An armed line with no `collecting` line means the frame tick did not come (the probe runs from the builder probe's tick). A frame line that says `engine motion NOT live` means producer B, the copy, merge and clear taps, and the family draws are silent for that reason, not because nothing was drawn. A frame line that says `NOT the bracket path` means path A is zero for that reason.

*Trial-mode independence.* The taps read no trial state. The mode is written on each line for the reader only. The probe runs the same in off, sphere, named and all.

*Ruled out:*
- ruled out: a CPU-side draw-to-pool-record join exists in engine motion, because the draw side is per pixel on the GPU (`engine_velocity.h`, the DRAW half). A per-draw origin is therefore not reportable from this build without a new GPU readback.
- ruled out (by design, not measured): producer B's appends are affected by the trial's clear, because B runs inside the store before the clear (the draw-path doc, Ruled out). The probe's B counts are the control for that.

*Flight plan (one session, same view):*
1. Raised pad, same view, trial off: press `hotkey.drawlist_probe`, read the three frames.
2. Same view, trial on in sphere (`diag.sealed_trial_mode = sphere`, `hotkey.sealed_trial` at the same radius): press the probe.
3. Same view, trial on in `all` (`hotkey.sealed_trial_mode` until the mode says all): press the probe.
4. The same three in the sealed hangar (docked, doors closed), same view.
5. One capture with a ship in view. Its producer-B appends and its family row are the control.

*Refutation criteria for H-PERSIST.*
- **Confirmed:** in `all`, the substituted pool draws of the wall families stay large and about the same across the three frames, while their path A calls and path B appends drop to about zero, and the carried candidate count is large. The walls draw from a pool that nothing re-appends.
- **Refuted:** in `all`, the wall families' draws fall in step with their appends: every drawn wall was appended fresh in its frame.
- **Control:** ship families (producer B) should be unchanged under `all`, because B runs before the clear. That is not evidence for H-PERSIST; it is what the mechanism predicts.
- **Limit:** only substituted families are counted. A wall drawn outside a substituted family is not in the family rows at all. If the walls are not in a substituted family, the test says nothing about them, and the flight shows that as zero wall draws, not as a refutation. The family names in the rows say which families were substituted.

*Not done.* No flight, no install. Per-draw rows are not written (no join). The scene draw total is not counted. Path A appended counts are not exact (the builder's own append is not visible). A GPU readback of the pixel slots would give per-draw keys; that is a separate design.

*Flight result (2026-10-12, log `edvr_gfx_20261012_083704.log`, five drawlist CSVs; the coordinator's reading, the log is not on this machine).* Raised pad, same view: trial off, path A 162 calls a frame, producer B 2094 calls and about 820 to 1040 items appended a frame, substituted draws 2415, carried 0 to 12. Sphere: A 7, B 2098 calls and about 840 items, draws 2286. All: A 0, B 2099 calls and about 840 items, draws about 2205. Census scene draws 14262, 13455, 13006. Sealed hangar, sphere then all: B 1491 to 1559 calls and about 390 items, draws 829 to 662, census 6262 to 5452. The pilot saw a good part of the hangar bay vanish under all, so some bay geometry is path A; on the raised pad almost nothing changed.

*ruled out: H-PERSIST (persistent draw lists), because path B appends about 840 fresh items every frame and the carried candidates are 0 to 12.* The bulk path is producer B (FUN_144312E00, called inside the store before the trial's post-store clear).

## Built v10 (2026-10-12, `claude/hangar-test-8`): the veto at producer B

Commit `6d348fe5` (DLL `v0.19.1-85-g6d348fe5`). Full `build.bat` green: `sealed_trial_test` 128, `drawlist_probe_test` 38, `builder_probe_test` 85, config contract 161 keys; then `build.bat --dll-only` green. Nothing installed, nothing flown.

*Decompile evidence for the veto form.* `decomp_4312040.txt` lines 326 to 337: the store builds the per-view masks (`local_288` through `FUN_142817260`) and calls `FUN_144312e00(rec, owner = param_1[2], masks = &local_208, tail = param_1[3])` for rig records of kind 2 (rig+0x160, +0x4B equal to 2). The return value is not used: the next statement reloads `uVar34` from `local_290`. `decomp_4312040.txt` has no reference to `owner+0x2A4` at all. `decomp_4312E00.txt` (259 lines):
- Arguments: param_1 is the rig record, read only (+0x170, +0x178, +0x17C, +0x180, +0x250, +0x2B8, +0x2A0, +0x29C and the rig's tables). param_2 is the owner (the pipeline). param_3 is the seven-qword mask array, copied to a local (lines 116 to 125). param_4 is an optional pair copied to a local (lines 108 to 110). Return: void (line 257).
- Writes to shared state happen only in the append branch (lines 179 to 250, the loop over LODs with a nonzero mask): `FUN_143696fa0(owner+0x260, rig+0x250)` (the dictionary lookup), `FUN_140540db0` (a node, only when the list is empty or full), the 0x150-byte item copied into the node, the item's mask into the node's slot at `0x154 + count`, the node count, and `owner+0x2A4` (line 250).
- Everything else writes stack locals: `local_198` (from `FUN_144c835d0`, untraced, called on a local with 0), `local_178` (a 0x80-byte copy of the rig's sub-block at `*(rig+0x2B8)+0x160+0x478`), `local_1d8` (the folded mask), `uStack_17c`.

**Veto form: skip the forward.** Every write to shared state is inside the append branch, which only runs through the forward. Skipping it removes exactly the appends, the node allocations and the owner count that the veto is meant to remove, and leaves the store, the owner and the rig untouched. Forwarding with every mask zeroed would reach the same result (the loop never reaches the dictionary lookup when all masks are zero), but it still runs the untraced copy and call, so skip is simpler and cheaper. Two callees are untraced: `FUN_144c835d0` (on a stack object) and `FUN_14431fb20` (applied to the local mask copy, only when rig+0x2A0 is set). Both act on locals by their arguments (INFERRED), so skipping them changes nothing shared. `FUN_143696fa0` may insert a dictionary entry on a miss (INFERRED); a skip leaves that entry uncreated, which is equivalent to an empty list.

*What a skip leaves undone.* (a) The appends for the record: intended. (b) `owner+0x2A4`: not incremented for those items, which is consistent. (c) The rig's pose copy into the pool item (+0x170 to +0x180): the item is never written, so nothing. (d) The engine-motion emit history for the record: the post-forward observer is skipped, so the record gaps. The emit's own rules re-baseline a gap (`engine_velocity_emit.h`, history rules): the next uninterrupted frame is masked, not joined. (e) The emit census counters and `kinematicEvalProbe.noteDirectBuild` do not count the vetoed call. Nothing else.

*The world sphere.* `decomp_4312040.txt` lines 212 to 262, read by hand (one reading; cross-checked for the x component only). L is the node's bounds centre: the node block is `*(*(rec+0x18)+8)`, with the centre at +0x20, +0x24, +0x28 and the radius at +0x2C (unscaled). A is the pose block `*(rec+0x190)` (15 floats), and B is the matrix `*(A+0x50)` (16 floats). Then `u_j = sum_k A[4k+j] L_k + A[12+j]` and `v_j = sum_i B[4i+j] u_i + B[12+j]`; the centre is v. The decompile's world x is `sum_k L_k (M3 B3)[k][0] + (A12..14 . B column 0) + B12`, with M3 the 3x3 of A and B3 that of B, and regrouping gives the formula. `sealed_trial_core.h` `storeWorldSphere`; the rig checks the identity, translations, a swapped x and y, and a non-finite input.

*The decision* (`sealed_trial.cpp`, `producerDecide`). All culls every record. Named keeps the cell test against the previous frame's cell (`keepNamedCell`, the store's own function). Sphere keeps a record whose sphere meets the trial sphere (`keepRecord`). The camera is the frame's, from the store observer; before a frame's first store observer runs, the previous frame's camera is used, a one-frame lag for the first records of each frame. An unreadable record is kept and counted (`world_sphere_unreadable=`). In `all` mode the store observer culls unreadable records too, so the two differ for those.

*The hook* (`kinematic_eval_hook.cpp`, `directBracket`, producer 0). The veto is checked before the forward and before the emit's enter. It has its own want (`directVetoWanted`), which holds the relay gate open. It attaches through the shared install (`ensureInstalled`) and never sets the emit's want. Trial off: the veto pointer is null and the want is false, so B runs untouched.

*Window line.* `producer_b_calls=`, `producer_b_vetoed=` (in count mode, the decisions that would veto), `producer_b_kept=`, `world_sphere_store_style=`, `world_sphere_unreadable=`, `producer_mode=`. The toggle line says whether the veto is live; when it is not, these counts stay zero for that reason.

*Config.* `diag.sealed_trial_producer = veto` (default) or `count`. Allow-list and template updated.

*Flight plan* (raised pad, then the sealed hangar; F11 is the draw-list probe, F6 the occlusion census):
1. Raised pad, same view, trial off: press F11 (three frames) and F6.
2. Trial on in sphere (`hotkey.sealed_trial` at the same radius): F11, F6.
3. Trial on in named: F11, F6.
4. Trial on in all: F11, F6. Expect the screen to empty: structure and ships both gone, producer B appends falling to about zero, draws falling, and `producer_b_vetoed` close to `producer_b_calls`.
5. Repeat 1 to 4 in the sealed hangar, docked, doors closed.
6. One capture with a ship in view under sphere: the ship's family rows and `producer_b_vetoed`.
7. A/B control: `diag.sealed_trial_producer = count` in sphere. Producer B's appends must match trial off.

*How to read it.* The veto works if F11's producer-B appends fall in sphere, named and all against trial off, in step with `producer_b_vetoed`. If the screen does not empty under `all` with producer-B appends at zero, the remaining draws are path A (the builder, cleared by the post-store clear) or pools persisting from earlier frames; the v9 carried count says the second is unlikely, but recheck it. If `producer_b_calls` stays zero, the veto is not live: the toggle line gives the reason.

*Not done.* No flight, no install. The world-sphere formula is a hand reading of the decompile, checked for x only. The untraced callees (`FUN_144c835d0`, `FUN_14431fb20`) and the dictionary insertion on a miss are INFERRED.

## Built v11 (2026-10-12, `claude/hangar-test-8`): the named cell

Commit `3aa231ca` (DLL `v0.19.1-87-g3aa231ca`). Full `build.bat` green: `sealed_trial_test` 160, `view source test` 45, `drawlist_probe_test` 38, `builder_probe_test` 85, config contract 163 keys; then `build.bat --dll-only` green. Nothing installed, nothing flown. Flight log `edvr_gfx_20261012_104222.log` (the named-mode defects) is not on this machine; the defects are the coordinator's reading.

*The defects this answers.* In the sealed hangar, named mode culled the lift shaft and the hatch: a 60 m sphere round the lowered pad cannot hold a 140 m shaft. Its own pad sits at about (-2.4, 1.8, 6.8) in the hangar frame and the BlastShieldBase at (23.3, -40.4, -133.1). The cause was the pad's centre: the cell read the v8.4 centre, which is zero for leaf records, so the cell sat at the origin. Neighbouring pads' decks were kept because a 60 m sphere reaches them.

*One world centre (`worldOfRecord`, `sealed_trial.cpp`).* Every named position (own pad, explicit parts, the cell test, the kept report) reads the same centre, in order: the producer B cache (the veto's store-style sphere for the records B was called on this frame), the builder pose (path A), the store-style sphere (the node block through rec+0x190, `storeSphereOf`), and the raw rec+0x240 last. Sphere mode uses it too, so the sphere moves with the same centre.

*The capsule (first addendum).* The axis runs from the own pad's centre to the nearest BlastShieldBase* (else the nearest own HangerSmall, HangerMedium or HangerLarge within 400 m), extended 20 m past it. The cell test is the distance to that segment. Explicit picks are only the parts within 15 m of the axis (`pickExplicitSegment`). The axis is logged once per change (`sealed cell: capsule at frame ...`).

*The oriented box (second addendum).*
- Orientation: the pad record's own pose. The builder's quaternion (`R(q)`, from `builderCentre`) when the record has a pose at rec+0x290 (path A); else the store-style pose (rec+0x190 and its +0x50 matrix: local e_k maps to `axes[k][j] = sum_i B[4i+j] A[4k+i]`). The extent is the builder pose's box at pose+0x90, the only source of a length-versus-width test.
- Footprint: of the pad's three local axes, the one most along the shaft is the up axis. Of the other two, the one with the larger builder extent is the length axis. No extent means no box: the capsule stays and the log says why (`sealed box: ... not built: ...`).
- Half-extents: half the pad's wiki size, doubled on the chosen axis (`diag.sealed_cell_double = length` by default, a guess; `width` the other way), plus `diag.sealed_cell_margin_m` (default 5). Wiki sizes: small 66 x 55 m, medium 150 x 90, large 213 x 128.
- Height: from 10 m under the pad's centre up the shaft to 20 m past the target. The box centre is at (D + 10) / 2 along the shaft, D the pad-to-target distance; half-height (D + 30) / 2.
- Containment: a centre is in the box when its length, width and height coordinates are within the half-extents. The coordinates are in `boxCoords`.
- PAGEUP and PAGEDOWN step the margin through -10, -5, 0, 5, 10, 20, 40 m in named mode (`hotkey.sealed_cell_larger` and `hotkey.sealed_cell_smaller`). The v11 capsule-radius steps are gone from those keys. INSERT is unchanged.

*What is a guess.* The length axis (the doubled axis) is a guess. The small pad's default radius of 30 m is a guess (for the capsule). The margin's default of 5 m is a guess. The extent-based length choice is unverified: the log prints the builder extent next to the wiki size so the flight can check the match.

*The report and the window.* The kept report gains `axis_dist_m`, `axis_along_m`, `box_u_m`, `box_v_m` and `box_up_m` (31 columns; NaN when no box). The named rows list every kept named record, nearest the capsule axis first, capped at 4000, with the totals in a trailing comment line. The window line gains `box=on|off half_length_m= half_width_m= height_m= margin_m=` and the world-centre source counts.

*Path C and path D (measured or not).*
- **C = FUN_14369C9C0.** Evidence: the RE doc's appender table (path C appends through 140540DB0 and 143696FA0, `decomp_369C9C0.txt`); its only caller is 0x36A0F50 at four sites. It is already hooked: producer 1 of the direct-producer bracket (`kinematic_eval_hook.cpp`, `g_directEntries[1]`, "census reads only"). v11 counts its calls and appended items after the forward, through a function-pointer observer on that bracket (no new detour, no new link dependency). The F11 probe's frame line reports `path C ... calls, items, faults`.
- **D = FUN_14434D120, via FUN_14434D470.** Evidence: `decomp_434D120.txt` does the append: the owner dictionary lookup (`FUN_143696fa0(owner+0x260)`), a node allocation, the record write, and `owner+0x2A4` incremented (line 85). `decomp_434D470.txt` (798 bytes) builds the packed 4-vector from the object at `param_2` (its 0x10 to 0x38 floats, scaled) and calls D120 with `param_2 + 0x180`. Its callers are FUN_14434D320, FUN_14434D3E0 and FUN_14434D440 (the producer driver), whose callers lie outside the draw cluster and were not traced. **D is not hooked and not counted in this build.** A hook would be a new detour: not added without a flight that says it matters.
- **What the object is.** D's record has no rig key (the lookup takes only the owner's dictionary), and its source object is a transform block (`param_2`), not a rig record. Whether it has a readable world position is INFERRED: its translation would be the object's floats at 0x30 to 0x38, not yet read.

*Not decided: the survivors under `all`.* The lights, smoke and nebula-like volumes that survive `all` are not yet matched to a path. The measurement is: run F6 (the occlusion census) under `all` on the raised pad and in the bay, then read the census report's top shader pairs. Those pairs are the survivors, and each pair's draws show whether the survivors are path A (the builder, cleared by the post-store clear), path B (producer B, vetoed), path C (counted now) or path D (not counted). If C's count is zero under `all` and the survivors' pairs are not in the path A set, D is the candidate. No veto for C or D is added: the addendum says do not guess.

*The flight plan.* Raised pad and sealed hangar; off, sphere, named, all; F11 and F6 each.
1. Named, the box on the raised pad: read the `sealed box:` log lines (the axes, the extent, the wiki match) and the window's `box=on` half-extents. Check the box covers the pad's bay and not the neighbours.
2. Named, the box in the sealed hangar: the lift shaft and the hatch must be inside. Read the kept named rows nearest the axis: the neighbours show with their `box_u_m`, `box_v_m`, `box_up_m`.
3. PAGEUP and PAGEDOWN the margin in named mode; read the `named box: margin` lines and the window's `margin_m=`.
4. `all` with F11 and F6: path C's calls and items, and the F6 top shader pairs.
5. `diag.sealed_cell_double = width` in one run, to see which axis the bay is doubled on.

*Not done.* No flight, no install. D is not measured. The box's length axis, the margin default and the 30 m small default are guesses. The box is only as good as the pad's builder extent, which is present only on path A pads.

## Built v12 (2026-10-12, `claude/hangar-test-9`): the early rig veto, and the box on the up axis

Commit `df9980c0` (DLL `v0.19.1-91-gdf9980c0`), on a new worktree `hangar-test-9` from `origin/claude/hangar-test-8` (725897e9), with `origin/claude/early-veto-re` (b5c2cda3, doc only) merged (`ef68fd9b`). Full `build.bat` green: `sealed_trial_test` 181, `view source test` 45, `drawlist_probe_test` 38, `builder_probe_test` 85, config contract 165 keys; then `build.bat --dll-only` green. Nothing installed, nothing flown. The v11 flight log (`edvr_gfx_20261012_112936.log`) is not on this machine; its numbers are the coordinator's reading.

*The v11 flight, and what it changed.* Sealed hangar, named: almost perfect; only a structural collar below the hatch was culled. Raising the pad: the hangar flickered, and at the top everything but the blast shield and the lights was gone. The log: the box was built from the builder quaternion's axes (e0 (0.87, 0.50, 0) length, e1 (0.50, -0.87, 0) width, e2 (0, 0, -1) up), half-extents L 71.0, W 32.5, H 86.9, centred midway between the pad and BlastShieldBase; the capsule direction was 17 degrees off e2. Then "no own pad at frame 8618", and later "the cell is the pad's sphere of radius 0.0 m". Three fixes:
- **(a) The box stands on the pad's up axis.** `buildBox` takes the up axis (the footprint's up index, sign-flipped to point at the target). The box is centred laterally on the pad's centre, from 10 m under the pad to 20 m past the target's projection on that axis. The target's lateral offset from the up axis is recorded (`boxLateral`, in the box log). The BlastShieldBase's lateral offset is what cut the collar: a 17 degree lean is about 40 m lateral at the top, along e1, where the half-width is only 32.5 m.
- **(b) The cell is frozen at the named engage** (trial on in named mode, or the mode key to named). It is taken from the first promote whose pad gives a cell (radius above zero), and held until the trial is off or the mode changes. Fresh picks that differ (no pad, or a centre more than 0.5 m away) are counted (`frozen_frames`, `frozen_skipped`), not used. No cell within 90 promotes gives the sphere rule, logged once, and no radius-0 cell is ever used.
- **(c) Logged once per engage:** the held cell, its source (box, capsule or pad sphere), the pad's record and centre, the box centre and half-extents, the lateral offset, and the pad's frozen axes and extent (`sealed cell: HELD at the named engage`).

*The early rig veto (RE: "Early veto (RE, 2026-10-12)").*
- **Veto point and emulation.** `rigEvalObserved` (the existing hook on 0x431AFE0, `g_rigEvalEntry`, gated by evalGate, which the veto's own want holds open) asks the trial before its forward. A veto performs Elite's own off-screen branch in place of the body, then returns 0 without the forward. **Evidence (exe bytes, capstone 5.0.7, and `decomp_431AFE0.txt` lines 78 to 103):** the collection is `*(rig+0x348)` (decomp line 78). The branch (`if (uVar9 == 0 || (collection+0x70 & 1) == 0)`) writes `[collection+0x629] = 0` (bytes `mov byte [rbx+0x629], r13b` at 0x431B228; r13 is cleared at 0x431B017 and not written again on that path, so the byte is 0), calls `0x434DD50` with `rcx = collection+0x300` (that routine is `mov dword [rcx+0x2A4], 0; ret`, bytes at 0x434DD50), and, when `[collection+0x2E0]` is non-null, calls `0x42B5760` with `rcx` = that pipe (`add rcx, 0x78; jmp 0x434DD50`, bytes at 0x42B5760). The return value of the emulated path is the value the decompile leaves in RAX after the last callee, which nothing reads (the rig eval's slot is void by the thunk; the caller was not located). The function returns 0.
- **Calling convention.** Microsoft x64, one argument in rcx: `void (__fastcall*)(uintptr_t)` through `base + 0x434DD50` and `base + 0x42B5760`. The emulation reads the collection and the pipe with SEH first and writes nothing if the read fails; the three steps run under one SEH frame, and a fault after the first write is counted as a fault and forwarded.
- **The rig decision** (`sealedTrialRigVeto`, only while the trial is on and in named or all mode; `count` decides and forwards). A rig's bound is the box of its records' world spheres in the last frame that had records (the box is kept for a vetoed rig, whose records the store no longer visits). Its bounding sphere has the box centre and the half-diagonal as radius. A rig is OUTSIDE when its sphere lies wholly outside the cell (`distanceToCell` above the radius, for the box, the capsule or the pad sphere); in all mode every bounded rig is outside. Unknown rigs (never seen with records, or no cell in named mode) and mixed rigs (intersecting the cell) are forwarded. Every 30th consecutive veto of a rig is a refresh: it is let through so its records are re-judged (`rigVerdict`, `kRigRefreshN`).
- **Rig identity (Q2 of the RE).** The store's own rig read is `rec+0x2B8` (decomp_4312040.txt: `*(param_5+0xae)`, with the kind read at `rig+0x160`), and producer B reads the same field (decomp_4312E00.txt line 66: `lVar9 = *(param_1+0x2b8)`, then `rig+0x170` for the pool position). So the record's rig link is the rig object, and the rig's position is `rig+0x170` (three floats, copied to the pool). Whether the object rigEvalObserved receives (`rcx` of 0x431AFE0) is the same pointer as `rec+0x2B8` is NOT yet checked: the window line's `rig_records_matched` and `rig_records_unevaluated` count, for each store record, whether its rig was decided this frame. Those counts are the match rate for the flight. The name is not decoded from the rig (`rig+0x20` and `rig+0x50` are unread); the first record's name is logged for up to eight rigs per session (`early rig veto: rig 0x... first seen with record 0x... (name)`).
- **Threading.** No thread-local association: the record's rig pointer links the record to the rig, and the table is under the trial's lock. The veto decision and the store observer both take that lock, and the decision never calls into the store. The thread that calls 0x431AFE0 is not located (the RE's L0 caller is unknown); the design does not depend on it.
- **Safety.** A vetoed rig's emulation fault forwards (`rig_emulate_faults`). The veto is attached with the trial and detached with it; the mode key or a trial-off sends everything through next frame (the decision returns forward outside named and all mode, and the attach is cleared when the trial turns off).

*Caveats (stated, not measured).* The emulation's saving is not measured. Skipping a rig skips its sub-object integrators (0x42B53F0, which the engine also skips off-screen; on unseal they resume without catch-up, so a spinning sub-part may pop, INFERRED from the RE). The rig's bound comes from its records' node radii, which the store uses as the sphere: a rig with a large part that the store culls per record (a record whose own sphere is small) can be vetoed while a part of it is near the cell. The refresh every 30 vetoes bounds how long that can last. The rig position `rig+0x170` is an engine position and is not compared with the store's spheres here.

*Flight plan (sealed hangar, named mode; each setting 75 s; F6 and F11 captured in each).*
1. `diag.sealed_trial_early = count`, trial on in named: F11 and F6 (the decisions are counted, nothing is skipped). The window's `rigs_vetoed` is the number that would be skipped; `rig_records_matched` against `rig_records_unevaluated` is the identity match rate.
2. Toggle `hotkey.sealed_trial_early` to veto, live, in the same sitting: F11 and F6. Expect the frame time (the drawlist's rig line and the window's `early_ms`) to fall, the bay to stay complete (the box holds it), and the rigs outside the cell to stop appearing in F11 and F6.
3. Toggle back to count: F11 and F6, to confirm the bay returns (the rigs are re-judged).
4. Repeat 1 to 3 on the raised pad. Read the `sealed box:` and `sealed cell: HELD` lines: the held cell should not move when the pad is occluded or the own-pad picks fail (`frozen_skipped` counts those frames).
5. Mode all with the veto: every bounded rig is skipped, so expect the screen to empty except the rigs with no bound; read `rigs_unknown`.

*Not done.* No flight, no install. The emulation's saving, the identity match rate, the rig name, and the rig-position check are not measured. The C and D questions from v11 are unchanged: D is not hooked and not measured.

*The test of the veto.* `sealed_trial_test` (184 checks): the bound's cell distances, the verdicts including unknown and mixed, the refresh every 31st decision, the up-axis box with its lateral and flipped and tilted cases, the freeze, and the off-screen emulation's step order. The steps are `offScreenSteps` in `sealed_trial_core.h`, which the emulation runs itself (`kinematic_eval_hook.cpp`), so the rig checks the order the engine's bytes give. The emulation's effect on a live rig is not rig-testable: it needs the live rig eval.

## Built v12.1 (2026-10-12, `claude/hangar-test-9`): the axis, the association, the cost

The v12 flight (`edvr_gfx_20261012_120057.log`, build 6afae6e5; not on this machine, numbers are the coordinator's reading): the early veto never fired (`rig_records_matched=0`, every rig unknown, `rigs_vetoed=0`), and its decision cost 290 to 430 ms per 5 s window (about 0.8 ms per frame). The box with its axis on the pad's e2 culled the hatch as well as the collar. The pad travelled along the pad-to-BlastShieldBase line, not along its own e2.

- **(a) The box axis is pad to BlastShieldBase.** `buildBox` takes the unit vector from the pad's centre to the target as its axis W. The footprint's length axis is the pad's length axis made orthogonal to W (Gram-Schmidt); the width is W x length. The box is centred laterally on the pad, from 10 m under the pad to 20 m past the target along W. The angle between W and the pad's own e2 is logged. The margin default is 10 m. The freeze is unchanged.
- **(b) Culled near the axis.** The report gains a `culled_near_axis` list: the named records culled in named mode whose centre lies within 60 m of the axis, nearest first, with their names and box coordinates (capped at 2000).
- **(c) The association, without the record's +0x2B8.** Two paths, each counted (`assoc_sync`, `assoc_job`, `assoc_none` in the window line):
  - *Synchronous.* `rigEvalObserved` sets a thread-local `t_rigCurrent` to the rig around its forward. A store record observed on that thread while the forward runs belongs to that rig. This covers the single-entry path 0x4321940, which runs synchronously on the sync branch (decomp_431AFE0 line 124).
  - *Job path (the Submit at collection+8's vtable +0x50).* The store's bucket is the batch entry copied from the Submit's entry. `decomp_431AFE0.txt` lines 106 to 117 build that entry: `[0]` = ctx (local_98 = param_2), `[2]` = `collection+0x300` (local_88), `[5]` = the record array (local_70), `[8]` = `collection+0x2E0` (local_58). The store's bucket convention is the same (`*bucket` is the ctx, `bucket+0x28` the record base). So the rig is the one whose collection is `bucket[2] - 0x300`. The collection of each rig is read at its decision (`rig+0x348`, the branch's own collection) and recorded in a collection table. **INFERRED**: the batch's group entry keeps `[2]` unchanged when the Submit's entry is copied into the batch; the flight's `assoc_job` count tests it.
  - Neither path: the record is counted (`assoc_none`) and the veto has nothing to decide for its rig.
- **(d) Cost.** The frame tick (`sealedTrialFrame`, once per frame) computes every rig's verdict from its bound (the last frame's record box; the cell in force) and stores one byte per rig (`verdictCode`: 2 outside, 1 mixed, 0 unknown). A decision is then one hash lookup on the rig slot, a read of `rig+0x348`, and the counts. No record is scanned on the decision path, and the store observer's association costs one cache check per bucket per frame. The window line reports `early_ms_per_frame`. **Estimate, not measured**: the decision is a few hundred nanoseconds with its two timer reads, so about 0.1 ms per frame at 500 decisions; the target is under 0.05 ms. The flight measures it.

*Rig checks.* `sealed_trial_test` (188): the box on the pad-to-target axis (its centre along that axis, its length axis orthogonal to it, its half-height, the target below and along z, the target closer than 1 m), the angle to e2, the margin default (10 m), the verdict codes, the freeze, the rig rule, the refresh cadence, and the emulation's step order.

*Not done.* No flight, no install. The association's match rates (`assoc_sync`, `assoc_job`, `assoc_none`) and the per-frame cost are the flight's to read. If `assoc_job` is zero while the sync path is also zero, the veto is inert and costs one table read per rig decision, per the brief.
## Built v13 (2026-10-12, `claude/hangar-test-9`): own ship, lowered-pad freeze, shaft pick, path D count

Built, not installed, not flown. Full `build.bat` green: `sealed_trial_test` 197 checks, `drawlist_probe_test` 38, config contract 165 keys. The `cpu profile: failed` lines in the build log are the profiler's own negative self-test (`tools\cpu_profile.py` expects `failed` during `smoke_recording`), not a build failure.

*Fix 1, own ship.* Never culled in the sphere or named modes; only mode all culls it. Exempt: a record or rig whose sphere contains the camera with radius under 40 m (`kOwnShipRadiusM`), and records whose name carries the own ship's type prefix, taken at each named engage from the candidate nearest the camera among those containing it. Log: `own ship (frame N): type prefix 'X' from the record ...`. Window fields: `own_ship_kept`, `own_prefix`.

*Fix 2, freeze.* The named cell freezes only from a lowered pad: within 15 m (`kLoweredToleranceM`) of the lowest pad height seen this session, measured along the shaft. Otherwise the engage is refused and the sphere rule holds: `sealed cell: NOT frozen at the named engage (frame N): the own pad is X m above its lowest height seen this session (...)`.

*Fix 3, shaft axis.* The target is the BlastShieldBase nearest the line through the pad centre along the pad's e2: perpendicular distance decides, ties by along, and the target must lie ahead of the pad (along > 0) within `kCapsuleMaxM`. Log: `sealed axis (frame N): target X chosen on the pad's up line: P m perpendicular, A m along it; the nearest to the pad was Y at D m.` Both distances are logged. Rig checks in `sealed_trial_test` cover the on-line pick, the tie, and the below-pad case.

*Fix 4, render-thread CPU profiler.* Already on this branch (ancestor of HEAD, no merge). Keys: `hotkey.profile_cpu` (press) and `diag.profile_cpu` (arm). Output is written by `tools\cpu_profile.py`, with a state file at each stage: armed, recording started, recording stopped, or failed with the stage that failed.

*Fix 5, path D (FUN_14434D470), count only.* A CodeHook entry hook (`path-d-434d470`) with a gate. It attaches when the drawlist window arms and detaches when the window finishes. Every call forwards: no veto. Counts per frame: calls, readable and unreadable records (`+0x10..+0x40` read under SEH), calls by return site (0 = FUN_14434D320's call at 0x14434D3C4, 1 = FUN_14434D3E0's at 0x14434D421, 2 = FUN_14434D440's at 0x14434D460, 3 = everything else, including the unnamed caller at 0x1462B7FF4). The frame line prints `path D (FUN_14434D120 via D470): hook <status>; calls N (readable ..., unreadable ...; from D320 ..., D3E0 ..., D440 ..., other ...)`. The site buckets assume each xref address is a 5-byte call; that is not byte-checked in this build, so a `other` count that is not zero on a known site would point at the assumption.

*Effects: which path draws trails and lights (searched, not established).* FUN_14434D470 is void with five arguments. Its callers: FUN_14434D320 (listed in `analysis\decomp\decomp_434D320.txt`: FUN_144714fe0, FUN_1430656f0, FUN_141a14320, FUN_141a14970, FUN_141a14d80, FUN_141b71a40, FUN_141c01db0, FUN_141c04cd0), FUN_14434D3E0 (called from FUN_1435a0720), FUN_14434D440 (from FUN_143ae9130, FUN_143af1240, FUN_143af1520), and one unnamed site at 0x1462B7FF4. FUN_14434D120 is called only by D470. No decompile in `analysis\decomp` names a particle, trail or light appender for these paths (`grep -i particle|trail|glow|emitter|plume` over the D-series files returns nothing). The only trail-word hit in the set is `gStarGlowTrailDistortionTileRate` (1454ff888, one reference, from FUN_1437164c0): a tuning global, not an appender. So the answer is open. The flight's per-site counts separate D's callers; the callers that run in a trail-producing window (own ship boosting, the hangar or station approach) are the candidates to decompile next.

*Design for the effects flight (no veto in this build).* (1) A lowered pad in the sealed hangar, the own ship at rest and then boosting; F11 window in each. (2) Read `path D ... from D320 ..., D3E0 ..., D440 ..., other ...` per window: a site that rises with boost is the trail's appender. (3) If a site rises, decompile its caller (the site's named function) for the record's world position and emitter fields before any veto is designed. A veto there would need the record's readable world position, a per-frame decision like the rig veto, and a measured cost.

*Flight plan.* Install the built DLL on the raised-pad and sealed-hangar setup (the pilot's step; this build is not installed). Then: (a) lowered pad, named mode: expect `own ship (frame N): type prefix` and `sealed cell: frozen` lines, the own ship kept in the window (`own_ship_kept` above zero); (b) raised pad, named mode: expect the NOT-frozen refusal line and no freeze; (c) an F11 window in each: expect `sealed axis (frame N): target ...` with both distances, and the path D counts; (d) the profiler (`hotkey.profile_cpu`) on the same run for the render-thread share. Mode cycling is the sealed-trial mode key; the window is F11.

*Not done.* No flight and no install. The axis pick's on-line distances, the path D counts and the profiler output are the flight's to read. Path D's return-site buckets are unverified until a window shows whether `other` is zero for the known sites.

## Built v13.1 (2026-10-12, `claude/hangar-test-9`): the bay, the automatic reports, the camera, the decision cost

Built, not installed, not flown. Full `build.bat` green: `sealed_trial_test` 208 checks, 0 failures (new: `bayPartName`, `lateralToAxis`, `inBayVolume` at its limits, `unitBetween`, `boxCoordsOf`, `nearBox`); config contract 165 keys.

*(a) Automatic reports.* A named engage arms a report due 2 s on (`sealed trial report: armed (trigger auto: named engage + 2 s)`). On foot while the trial is on, one is armed 10 s after the player goes on foot, once per time on foot (`auto: on foot + 10 s`). Each writes the same CSV as `hotkey.sealed_trial_report`, culled_near_axis rows included; END still works. The window line adds `top_culled_containing=[name xN at (u, v, w), ...] distinct= over=` (culled records whose sphere holds the camera, by name) and `top_culled_near_box=` (culled records inside the held box expanded 20 m on each axis, by name). The coordinates are the box's own (u, v, w) when the cell is a box, else the offset from the pad's centre, both for the name's first record in the window.

*(b) The bay.* In named mode a record the box or capsule rule would cull is kept when its name contains Hanger, Hangar, Lobby, BlastShield, Skirt, LandingPad, Lift or Gate and its centre lies within 60 m lateral of the shaft, with the position along the shaft from -20 m (below the pad) to the target's distance plus 20 m. Camera-containing records are included. Each engage logs the rule and the neighbour: `sealed bay (engage frame N): keeps ... Nearest other pad: NAME at D m lateral of the shaft (K other pads seen).` The automatic reports log the count of records the box alone would cull that the bay kept, with the first 20 names: `sealed bay (named engage + 2 s, frame N): K records kept since the engage ...`. The window line has `bay_kept=`. Bay records are counted in `bay_kept`, not in `kept_by_cell`. The neighbour's lateral distance is measured from the pads the store saw last frame, so it is the flight's to read, not established here.

*(c) The camera on foot.* The source is the view 0 origin (`ctx + 0x540`), read through `view::sphereCentre`, because no Explorer Cam is on and the on-foot camera's rows are not read (`view_source.cpp`; this doc's "The view the aim comes from"). The window's `pad_dist` is NOT a live reading: it is `g_cellPrev.padDist`, the value frozen at the engage. The 17.4 m seen on foot is the freeze, not evidence that the camera is stale. The window now prints `camera_to_pad_m` (live: this frame's camera to the held pad's centre), `origin_moved_m` (the origin's summed motion over the window) and `on_foot_origin_moved_m` (the part of it on foot). If `on_foot_origin_moved_m` stays near zero while the commander walks, the origin does not follow the avatar. The fix then needs the on-foot camera's rows, which are not located yet (research, not a v13.1 change). This build does not change the camera source.

*(d) The decision cost.* The rig decision reads no timer, reads the rig's collection once per rig per frame, and finds the rig's slot through a one-entry cache (one probe per rig per frame). `early_ms` now times only the frame-tick refresh (`refreshRigVerdicts`); the window says `early_ms_scope=frame_tick_refresh_only`. The decisions themselves are not timed in this build, so the 0.087 to 0.157 ms per frame measured in v13 is not comparable with this build's `early_ms`. A per-decision cost needs a rig-timed benchmark, which is not built.

*The axis log.* `sealed axis` lines were logged on every change of the per-frame pick. They are now logged at the engage only, the frame the cell freezes from. The per-frame pick is not used while the cell is held (the store uses the held cell), so the wander was in the log, not in the culling. Why the un-held pick wanders (its pads and explicits come from each frame's records) is not investigated.

*Flight plan.* Install (the pilot's step). On foot in the bay with the trial on: expect the `sealed bay (engage ...)` line with the neighbour's lateral distance; `bay_kept` above zero; `top_culled_containing` with the unnamed culled record's name (the parts the bay now keeps should be gone from it); the `on foot + 10 s` report; and `on_foot_origin_moved_m` while walking. Then compare the on-foot report's culled_near_axis rows with the window tallies.

*Not done.* No flight, no install. The bay's neighbour measurement, the on-foot origin motion and the decision cost are the flight's to read.

## Built v13.2 (2026-10-12, `claude/hangar-test-9`): the box bottom at 60 m, the own record's sphere, the bay minima

Built, not installed, not flown. Full `build.bat` green: `sealed_trial_test` 215 checks, 0 failures (new: `parseBelowM`, the box with a 60 m bottom running -60 to 120 m along the shaft, `strippedPrefix`, `ownRecordMatch`, `ownSphereKeeps`); config contract 166 keys (`diag.sealed_cell_below_m` documented and allow-listed).

*Why.* The v13.1 flight's on-foot report had the own ship's parts (CobraMkV, CargoDoor_*, CobraRearGear*, LandingGear_CobraMkV_*, HardpointDoor_*, HeatSinkLauncherBase, Launcher, DoorA-D) at box-up -88 to -92 m, just below the box bottom at -10 m, so the box cut them. The bay's doorways sat at -44 to -45 m. The pad record's centre is about 12 to 15 m above the deck the ship stands on, so the box bottom needs to reach well below the pad's centre.

*(a) Below distance.* `diag.sealed_cell_below_m` (edvr.ini; default 60, a guess sized from the doorway at -45 m) is the distance under the pad's centre along the shaft at which the box bottom sits and below which the bay keeps nothing. The install logs the value. Minima: every engage logs the minima since the previous engage, along the shaft from the pad's centre (negative is below the pad): the own ship's records (names with the type prefix, or the own record's name, within 60 m lateral of the shaft) and the `Door*` records within 60 m lateral. These are the numbers the next flight uses to size the key. The line is `sealed bay minima (since the previous engage, N frames): ...`.

*(b) Own record, robust to movement.* At each named engage the own record is the record named as the type prefix without its underscore (`CobraMkV_` gives `CobraMkV`), taken from the previous frame's records within 40 m of the camera. A record containing the camera is preferred, then the nearest. The engage logs `sealed own ship (engage frame N): own record NAME, D m from the camera, radius R m (the camera is inside it / outside it)`. Each frame the own record is re-read by its address: its centre and radius are its current sphere, and its name must still match, otherwise it is dropped (the address was freed or reused) and the rule stands down until the next engage. Every record whose centre lies within that current sphere plus 5 m is kept in sphere and named modes (all mode still culls). The v13 prefix rule stays. Window fields: `own_record`, `own_record_live`, `own_record_radius_m`, `own_sphere_kept_last_frame`, `own_sphere_kept_window`.

*(c) Neighbour.* The own pad's second platform (same size, within 30 m of the pad's centre) is not a neighbour. The v13.1 report named `LandingPadSmallB` at 18.8 m as the nearest neighbour; with this rule it is excluded if it is the second platform.

*Culled-name tallies.* Each name now prints its first record's radius and child count: `name xN at (u, v, w) r=R children=C`. The `HangerLarge` (about 231 m, camera-containing) and `Floor00` (about -191 m) records were not identified here: their radius and child list are in the v13.1 report (`sealed_kept_20261012_141930.csv`) and in the v13.2 window line. The data are not on this machine, so the answer waits on that file. A hypothesis to test next flight, not established: a culled group record (containing the camera, so its own sphere is large) may take its children with it, which would explain a bay that loses parts far from the camera. The discriminator is the children count of the culled group against the bay parts missing from the report.

*(c, v13.1 flight) Camera.* No change: the on-foot source is still the view 0 origin, as the v13.1 finding stated.

*Not done.* No flight, no install. The below distance's value, the own-record rule's behaviour on foot, and the group-record hypothesis are the flight's to read.

## Built v13.3 (2026-10-12, `claude/hangar-test-9`): the default below distance, the cell's validity, unreadable records

Built, not installed, not flown. Full `build.bat` green: `sealed_trial_test` 218 checks, 0 failures (new: `rawCentreUsable`, `ownPadCandidate`, `heldCellDrops`, the default 85 m); config contract 166 keys.

*Why.* The v13.2 flights: with `diag.sealed_cell_below_m = 85` the small hangar was fully correct, the elevator doors included (TurboLiftDoor at -63 to -72 m). Then the pilot swapped ships into a medium hangar (Lynx Highliner: HangerMedium, LandingPadMediumA/B, MediumTransport01_*), another medium (LakonMiner_*), and a large (Type9_*, LandingPadLargeA, HangerLarge), without toggling F2. The trial stayed on with the cell frozen on the small pad's record, so every medium and large result was judged against a stale cell. Separately, at 15:00:36 thirty records with unreadable centres (VentFlap0-5, Nozzle0-2, BarrelFlaps0-2, BarrelClamp0-1, Barrel; `raw_centre_zero`) were culled, although named mode keeps them.

*(a) Default 85 m.* `kBelowDefaultM` and the ini template (`#sealed_cell_below_m = 85`) are 85, with the measurement noted in the template. Medium and large hangars are not measured at 85.

*(b) Held cell validity.* Each frame in named mode with a held cell, the held pad record is looked for among the store's pads this frame. An own pad candidate is a pad (any size) containing the camera or with its centre within 40 m of it (`ownPadCandidate`); the nearest candidate is the frame's pick. The held cell drops (`heldCellDrops`) when the held record is absent for 30 consecutive frames, or when another pick is the nearest for 30 consecutive frames while the held pad is not the pick. The drop logs `sealed cell: DROPPED (frame N): held pad NAME (size S) is not seen by the store / is not the nearest own pad for K frames; the nearest own pad is now NAME`, clears the cell and the own record, and sets a re-freeze watch. The sphere rule holds meanwhile. The watch re-freezes from the next pad once it is lowered (the engage path runs, and logs `sealed cell: RE-FROZEN (frame N) on pad NAME (size S), after the drop of NAME (size S)`). The lowest-height tracking is per picked pad record and restarts when the pick changes.

*Caveat in the rule as specified (for the coordinator to decide).* A newly picked pad's lowest height is its first reading, so `padLowered` passes on first sight: a pad that is raised when first seen would re-freeze at its raised height. The rule needs the pad seen lowered from a higher reading, or a longer observation, before the re-freeze. Not changed here; the flight will show whether the re-freeze comes on a raised pad.

*(c) Unreadable records.* `worldOfRecord` took the raw centre at rec+0x240 whenever it was finite, including exactly zero. An unreadable centre is then the origin with the record's radius: a fake sphere far from the camera, judged and culled. The raw centre is now usable only when finite, not the origin, with a positive finite radius (`rawCentreUsable`, rig-checked); otherwise the record has no sphere and named mode keeps it as unjudged (`kept_unreadable=` in the window line counts them; the existing `kept_unjudged` field counts the same records). Sphere mode keeps them too, since the same branch applies. Not confirmed on a flight: the next report's `raw_centre_zero` rows should be kept.

*(d) Culled tops.* The window line keeps its fields. The culled names move to a separate `sealed culled top:` line per window: `containing [name xN at (u, v, w) r=R children=C, ...] distinct= over=; near_box [...]`, with the names in full. The culled-near-axis list (the report) reaches 90 m from the shaft (`kCulledNearAxisM`), so the elevators 63 to 72 m off the shaft are listed.

*Flight plan.* Install (the pilot's step). Small hangar: the window's `kept_unreadable` count above zero with the raw-zero records kept in the report; the culled top lines with no elevator names. Swap to a medium without F2: expect `sealed cell: DROPPED` after 30 frames, the sphere rule, then `RE-FROZEN` on the medium pad once lowered (check that the first freeze is on a lowered pad). Read the `own_record` line on each ship.

*Not done.* No flight, no install. The re-freeze's lowered test on first sight (the caveat above) and the medium and large below distances are the flight's and the coordinator's.

## Built v13.4 (2026-10-12, `claude/hangar-test-9`): the floor per pad size, the deepest lift, the pads at each re-freeze

Built, not installed, not flown. Full `build.bat` green: `sealed_trial_test` 222 checks, 0 failures (new: the per-size floors 85, 120, 145; the override applied to every size; no size gives the small floor); config contract 169 keys.

*Why.* The v13.3 named flight: the small and large bays were fully correct and the re-freezes on ship swaps worked (DROPPED, then RE-FROZEN, through medium, small and large). The medium bay missed its back elevator section: its TurboLiftDoor records sat at along -102.8 and -103.9 m (lateral 27.8 and 33.3 m), 18 to 19 m below the single 85 m floor. The small bay's doors are at -61 to -72 m. So the floor depth is per size. The large bay's deepest door seen was -125.5 m, and the pilot saw that bay complete; whether that door belongs to the large bay is open, and the per-size minima and the pad log will show it.

*(a) Per-size floor.* `diag.sealed_cell_below_small_m` (85), `diag.sealed_cell_below_medium_m` (120), `diag.sealed_cell_below_large_m` (145), in metres under the pad's centre along the shaft. The box bottom and the bay's lower bound use the held pad's size (`belowForSize`, rig-checked). `diag.sealed_cell_below_m`, when set, overrides all three; the install logs which applies. The ini template states the measured lift doors (small -72, medium -104, large -126 m) and the margin each default adds.

*(b) Deepest lift at engage.* The engage logs `sealed bay deepest lift or door (engage frame N, the previous frame's records within 60 m lateral): NAME at along X m (lateral L m); the floor for this size is F m under the pad.` The record is the deepest name containing `Door` or `Lift` within 60 m lateral of the pick's shaft, from the previous frame, so one engage shows the depth without waiting for a minima window. The pick's shaft is used, so the line works while the cell is not held.

*(c) Pads at each re-freeze.* Each engage (every freeze and re-freeze) logs each pad record of the held pad's size that the store saw last frame: `sealed pad (engage frame N): held|other record 0x... NAME (size S) centre (x, y, z), D m from the camera; axes: e2 up (...), e0 (...), e1 (...)`. Two medium records with frames 90 degrees apart would show as two `sealed pad` lines with different axes; a second platform of one pad would share its neighbour's centre offset. No behaviour change.

*Caveat carried from v13.3.* The re-freeze's lowered test on first sight of a new pad still passes at once (its lowest height is its first reading). Not changed.

*Flight plan.* Install (the pilot's step). Medium bay: the engage line shows the deepest door near -104 m and no elevator is missing from the culled tops; the medium pads' `sealed pad` lines show whether the two LandingPadMediumA records are one pad's two platforms or two bays. Large bay: check whether the -125.5 m door is inside the large bay's floor (the 145 m floor keeps it).

*Not done.* No flight, no install. The medium and large floors are the flight's to confirm, and the large bay's -125.5 m door is open.

## Built v13.5 (2026-10-12, `claude/hangar-test-9`): the GPU pass profiler merged in

Built, not installed, not flown. The branch `origin/claude/gpu-pass-profile` (0cdf493c, on origin/main 2d993d2b) merged into `claude/hangar-test-9`; the merge brings main's three docs commits (c90b10d8, 1f171eef, 2d993d2b). Full `build.bat` green: `sealed_trial_test` 222 checks, 0 failures; `gpu_pass_profile_test` 46 checks, 0 failed; `occlusion_census_test` ok; config contract 170 keys. The `FAIL:` lines in the screen-motion rig are its mutation checks (each mutant must fail; the control passes).

*Conflicts resolved.* `build.bat`: the quiet rig list takes `gpu_pass_profile_test` and keeps `cpu_sampler_test`; both rig blocks (`rig_occlusion_census_test` and `rig_gpu_pass_profile_test`) are kept, in that order. `src/common/runtime_profile.h`: both key lists kept, the profiler's `hotkey.gpu_pass_profile` after the trial's keys. `src/d3d11/device_hook.cpp`: the trial's declarations (cpuProfileKeyText, sunSkipKeyText, shadowGateKeyText and the rest) kept, `gpuPassKeyText` appended to the key-text list; the profiler's `Hotkey gpuPassKey` and its `hotkey.gpu_pass_profile` row merged without conflict. Auto-merged without conflict: `edvr.ini` (the profiler's commented key), `vscreen.cpp`, `exposure_fix.cpp`, `gpu_census.cpp`, `gpu_disjoint_clock.h`, `gpu_timing.cpp`.

*Instrument interaction.* See the profiler doc's "Interaction" section. In short: the trial, the drawlist probe and the look probe take no GPU timing (QPC and counters only), so they cannot contend with the profiler. The census and the application-frame timing (`advanced.app_gpu_timing`, default on) share the one DisjointClock: a profiled frame that opens while one holds it fails with a logged reason, and their own leases are refused during a profiled frame. The profiler does not corrupt or stall the trial's measurements. Expected but not flown: with the application-frame timing on, profiled frames probably fail, because its segment is open at the profiler's frame boundary (read from the code). The remedy is a behaviour change and is not in this merge.

*Not done.* No flight, no install. The application-frame interaction is the flight's to confirm.

## Built v13.6 (2026-10-12, `claude/hangar-test-9`): the GPU pass profiler's yield

Built, not installed, not flown. Full `build.bat` green: `gpu_pass_profile_test` 54 checks (new: the yield's life across the press, the batch and the last close), `gpu_disjoint_clock_test` 192 checks (new: a frame open refuses a second frame and a profiler open; ending it releases the clock; a standalone scope then opens, closes and releases), `sealed_trial_test` 222 checks; config contract 170 keys (no new key).

*Why.* The v13.5 merge's interaction: with the default settings (advanced.app_gpu_timing on) the application-frame segment is open at the profiler's frame boundary, so a press would fail its frames. The coordinator's decision: fix it so a profiler press works with default settings, as a temporary diagnostic behaviour during a capture only.

*The yield.* From the press until the last profiled frame's scope is closed:
- `gpuTimingSetYield` (gpu_timing.cpp) is set at the press and cleared at the batch's end (the boundary after the last frame closes; also on every failure and at shutdown). The frame driver's begin and GpuTimer's begin refuse new leases while it is set, counted (`gpuTimingTakeYieldRefusals`).
- `gpuFrameCommand` (gpu_frame_timing.cpp) does not open an application frame while it is set; the skipped frame is counted once (`gpuFrameTakeYieldSkips`).
- `gpuFrameYieldBegin` (called by the profiler at each boundary while the yield is wanted) ends an application frame open across the boundary. It closes through the span ring to the clock's finishFrame, which clears the open scope, so the profiler's gpuTimingPassScopeOpen finds the clock free.
- The log at the batch's end: "app gpu timing: paused for the GPU pass profile, N frames; census timer leases refused: M."

*The census.* Its timers are refused the same way. A census timer lease is only open inside a census section, and no boundary runs inside a section, so the census does not hold the clock across a boundary. Not verified in a flight.

*Verification.* The clock's sequence is checked in gpu_disjoint_clock_test (the profiler's open condition scopeOpen() holds while the frame is open and a second frame is refused; finishFrame releases; a standalone scope opens after). The capture's life is checked in gpu_pass_profile_test. The D3D path is not modelled by a rig; it is reasoned in the profiler doc. The flight is the check: a profiled frame should open with app_gpu_timing on (no "not captured" line; the "paused" line counts the frames).

*Not done.* No flight, no install.

## Built v13.7 (2026-10-12, `claude/hangar-test-9`): the cell in the pad's frame, the release by the shell; the profiler addendum

Built, not installed, not flown. Full `build.bat` green: `sealed_trial_test` 232 checks (new: the pad-frame round trip; a shift of the frame moves the cell by exactly the shift; a pad turned 90 degrees turns its box; the release at 6 m, not at 4 or 5 m, and not by a shift of pad and shell alike); `gpu_pass_profile_test` 72 checks; `gpu_disjoint_clock_test` 192; config contract 170 keys.

*Why (the v13.6 medium).* The elevator doors fell out on foot. The engage lines showed the same held pad record at (10.02, 5.79, 2.33) at the cockpit engage and at (4.84, 2.80, 36.33) on foot: a static pad moved 34 m, and the box centre moved by the same offset. The frame the records are expressed in shifts when the player goes on foot (an origin rebase). A cell frozen in those world numbers is displaced after the shift, and the doors near its lower edge fell outside it.

*(a) The cell in the pad's frame.* At each named engage the held cell is localized to the held pad's frame: its centre and its three unit axes (poseOrientOf; axes[2] is up). Points (centre, capA, capB, the box centre) are written as coordinates along the axes from the centre; directions (the box axes) without the centre. `refreshHeldWorld` rebuilds the world cell every frame from the pad's live pose (its centre by worldOfRecord, its axes by poseOrientOf) before the frame's drop decisions and judgements (at the promote) and before the early-veto verdicts (at the frame tick). The explicit and bay keeps, the own-ship sphere and the culled-near-axis report read the same live cell. A pose that cannot be read keeps the cell as it was for that frame.

*(b) Release by the pad's own motion.* At the engage the hangar shell is the nearest named Hanger or Hangar part within 150 m (from the explicit parts the store saw last frame). Its live position is re-read each frame by address (the name must still match). The pad's height along the shaft relative to the shell (the pad minus the shell, dotted with up) is compared with its engage value: more than 5 m of change drops the cell (the sphere rule holds, as with any drop), and the cell re-freezes from the pad once it is lowered, as before. A frame shift that moves pad and shell alike changes nothing. No shell in range: the check stands down (logged).

*(c) Logs.* Once per engage: `sealed cell (engage frame N): pad-local box of ... in the pad's own frame ...` (the target, the box centre, the three axes, the half extents, the floor, all in the pad's frame), and `sealed shell (engage frame N): ...` (the shell's name, its distance, the pad's height above it). Every window: `held_frame=pad-local frame_shift_m=... shell=... shell_delta_m=... release_motion=...` on the window line. `frame_shift_m` is the held pad's live centre to its engage centre (the frame shift plus any real motion); it is the latest value, not a maximum.

*Caveats.* The pad's centre can come from different sources (worldOfRecord: producer B, pose, store-style, raw); if the source changes between frames the local frame jumps by the difference between them. The window's frame shift would show it. The shell is the nearest named Hanger or Hangar part, not a proven static record.

*The GPU pass profiler addendum (same build; `gpu_pass_profile_core.h`, `gpu_pass_profile.cpp`).*
- (1) Compute shaders: each dispatch notes its bound compute shader (bindingShaderHash(BindSlot::Cs)); the CSV has a new last column, top_cs_dispatches (hash(NAME):count, most dispatches first, hex). Named from skin_ledger.h only: APPLY_BIND_POSE_TRANSFORMS_CS and CLEAR_TRANSFORM_DATA_CS. Other compute shaders print as hex; the repo has no further catalogue of compute hashes.
- (2) A depth-only map that is not at the eye size is a shadow map (square or not, large or small). The 2048x1024 D16 pair and the 256x256 D16 map were other.
- (3) A new class hud_overlay: a colour target with at most four draws whose most-drawn vertex shader is one of the game's HUD families, read from the headers (EyeDrawSnapshot::kHud, kHoloGuiFxOffVs, kHoloTargetSphere). Matched by VS hash only (the pixel shader is not part of the match, so a HUD VS with another pixel shader still counts). Ahead of the eye-size rules, so its cost is out of fullscreen_at_eye.
- (4) The class lines and the top groups print the median of one pass's ms beside the mean.

*Not done.* No flight. The release threshold, the shell choice and the pad's frame source are the flight's to read; the window's frame_shift_m shows the rebase directly.

## Built v13.9 (2026-10-12, `claude/hangar-test-9`): sticky own pad, fixed shell, the bay

*The v13.8 medium flight (edvr_gfx_20261012_183650.log, medium bay only).* Two trial bugs muddied the probe's pad-local frame. (a) The own pad flipped. LandingPadMediumA and LandingPadMediumB are 40 m apart laterally with antiparallel axes; both spheres held the camera, and the pick tied (44.7 and 44.5 m). The own pad went A to B for about 180 frames mid-descent and rotated the box about 89 degrees: a whole-hangar blink. (b) The release shell switched. The "nearest Hanger/Hangar part" moved between MediumHangerSkirt, HangerMedium and HangarGate_Ramp1_1 as the pad moved; the cell was dropped and re-frozen every frame. Because the settles were judged against the flipping shell, no lower-cycle summary fired.

*Fix (a), sticky own pad (`sealed::pickOwnPadSticky`, `sealed::choosePadIndex`).* A near-tie is two candidates within `kPadTieM` (5 m). The tie goes to the pad whose up line passes nearest the camera laterally (both pads need an up axis: `PadObs.up`, read from the pad record's pose at the store), then to the smaller name ("...A" before "...B"). A held pad stays while its record is seen in the frame, whatever the camera does. A held pad that is not seen is replaced only after `kPadStickyFrames` (30) unseen frames; until then there is no pick. The trial resets the state at each toggle-on. The probe's own fallback pick (no trial pad) uses the same rule with its own state.

*Fix (b), fixed shell (`sealed::chooseShell`, `sealed::isShellName`).* The reference shell is chosen ONCE at the freeze: a named part that begins Hanger or Hangar and contains none of Gate, Ramp or Skirt, with a size word, matching the pad's size first (else any size), the nearest within 150 m. `refreshHeldWorld` re-reads it by address; if it is not read, the release check stands down and logs once (`sealed shell: the reference ... stands down`). It does not switch. The probe chooses its shell once per own pad (the frame key) the same way and re-reads it by address; a sample without it stands the heights down (logged once).

*Fix (c), the bay.* `sealedTrialBox` returns the held cell's box (or the named pick's), world coordinates. The probe expresses it in the frame and tests each record: inside the box's half extents (length, width, height) is `bay=in the held bay`, outside is `bay=outside the held bay`; no box (no named cell) is `bay=no box`.

*Lower-cycle summary.* The pad's settles are judged against the fixed shell, so the bottom is measured against one reference. A bottom still writes its `settled at the bottom` line at the settle (with the shell named by the pad line) and the full summary at close (2 s after the last moved record, the pad moving again, probe off, or 30 s with no record moved).

*Rig.* `tools\hatch_geometry_test` (71 checks; the v13.8 67 plus the shell choice, the near-tie with and without up axes, the held pad staying while seen, the 29/30 unseen-frame release, and the bay).

*Not done.* No flight. The next flight (small and large bays) should show `bay=` per record, one fixed shell name per bay in the pad line, and the own pad changing only after an unseen run.

## Built v13.8 (2026-10-12, `claude/hangar-test-9`): the hatch probe in the pad's frame, and the lower cycle

Commit `a4fd9f81` (DLL `v0.19.1-113-ga4fd9f81`). Full `build.bat` green (the dirty-tree stamp was `-112-g89528af1-dirty`, then the clean commit's `--dll-only` promotion, which passed with the receipt verified). `hatch_geometry_test` 67 checks, `sealed_trial_test` 232 checks. Nothing installed, nothing flown.

*Why.* The v8.2 probe read rec+0x240, which is zero for BlastShield, BlastShield_A and _B, and used world centres that a station rotation or an origin rebase moves. v10 gave every record a store-style centre; the hatch panels had not been logged with it. The sealed mode needs to know whether the hatch closes to the bottom lip of the shaft, for small, medium and large bays, and where it stops.

*What the probe logs (each sampled frame, one in six).*
1. The store keeps the records named BlastShield, Hatch, Shutter, Door, Collar, Lid, Cover, Gate, Hanger or Hangar, and every LandingPad (sample cap 128; was 64). Lift is no longer a name; Door covers TurboLiftDoor and HangarLiftDoors__.
2. The own pad: the trial's held cell pad, else its named pick, else the pad whose sphere holds the camera or the nearest within 150 m (sealed::pickOwnPad). Its world centre and axes are read at the tick (sealedTrialRecordWorld). With no readable own pad the world is the frame, and the pad line says so.
3. Each kept record: pad-local centre, along the shaft and across it (lateral), pad-local axes, and the world centre with its source (producer-B, builder, store-style, raw). Kept: the own pad, and each named record within 80 m lateral of the shaft (the trial's cell capsule, pad to target), or within 300 m of the camera when there is no shaft.
4. Change lines: the pad-local centre moves more than 0.05 m, or an axis more than 0.5 deg, from the record's last line; at most one per 500 ms while moving. A settled line comes 2 s after the last move, and only when the record differs from its last line; it carries the total move from where the record first appeared. A record absent for 30 sampled frames is reported gone. Record lines are capped at 4000 a session (`hatch geometry: record line cap` is logged once; past it, lines are counted).
5. Pad lines (their own cap, 3000): every sampled frame while the pad moves, every 2 s while it is still, and on each lower-cycle notice. Height above the shell is (pad - shell) . pad up; the shell is the nearest Hanger*/Hangar* within 150 m in the same sample. The change since the first reading of the frame is shown.
6. A new own pad (a frame change) restarts the tracker and the watch, logged once.
7. The docking-state +0x3C8 is not re-logged here: the hatch-state probe (state_dump_probe.cpp, same F7) already writes "3C8 old->new" on change, budget-limited by `g_hatchBudget`.

*Lower cycle.* A cycle starts when the pad moves and settles when it has been still for 2 s. A settle at least 0.5 m below the cycle's start (relative to the shell) is a BOTTOM. From the settle, the frame's records are watched. A record that moves beyond the change thresholds from its position at its first move after the settle is listed in the summary, with its pad-local start and end centre, its rotation (the largest axis angle from start to end), its along-shaft height at both ends, its first and last move in seconds after the settle, and its end height against BlastShieldBase along the shaft. The narrowest point of the shaft is not inferred beyond that reference. The summary closes 2 s after the last move of any listed record, when the pad moves again, when the probe turns off or the frame changes, or 30 s after the settle with no record moved. A rise, or a fall under 0.5 m, that settles is "not a lower": no summary.

*Deviations from the brief (accepted by the coordinator).* Lift dropped from the names. The shell is the nearest Hanger record in the sample, not the v13.7 explicit-part list. The shaft exists only while the trial is on (named mode); with the trial off the probe uses the 300 m camera filter and the pad-by-camera pick, so the flight runs with the trial on in named mode.

*Risks.* Pad-line volume is about 15 lines a second while the pad moves (its own cap). The producer-B cache is valid only while the trial is on. The store path takes the trial lock and then the box lock; the tick takes the box lock alone. No inversion was found, but the path has not been run under load.

*Rig.* `tools\hatch_geometry_test` (67 checks): the pad-local frame is unchanged by a rigid motion of the station (turn and rebase); the local point and direction in a turned pad; the angles of a half-degree turn and of a right angle; the along and lateral split; the tracker's appear, change interval (two change lines in a second of 0.1 m steps), 0.6 deg rotation change, settled and disappearance rules, and cap; the nearest hangar; the own-pad pick; the watch's lower, settle, not-lower, record start and end and times, rotation, pad-moved-again close, no-move close and turn-off close; the line texts; the sample box.

*Flight plan (not run).* F7 on, with the trial on in named mode (the cell supplies the shaft). Dock in a small bay, lower the pad, and sit until the hatch is fully closed; note the bay. Repeat in a medium bay and then a large bay, once each. Turn F7 off after each bay so the summary prints. For each: the pad line (phase, height above the shell, the fall); the lower-cycle summary (which records moved, their pad-local start and end centre, rotation, times after the settle, the BlastShieldBase reference); the first change line of each moving record. Confirm the log's version line names this build (`python tools\edvr_log.py --target frontier --expect-build HEAD`).

## Early veto (RE, 2026-10-12)

Offline, build 332841 (gate matched: TimeDateStamp 1788384820, SizeOfImage 104894464). No C++ changed, no build, no flight, nothing installed. Branch `claude/early-veto-re` from `claude/hangar-test-8` (b7e427a5). **PROVEN** = exe bytes (capstone 5.0.7 reader, scratchpad, not committed), a fresh Ghidra headless decompile of 18 callees (scratchpad `ev_decomp`, read-only project), or the decompiles in `analysis\decomp` and the EDVR source. **INFERRED** = not closed by those.

### Q1. The chain, and which level runs on a job thread

| Level | Site (VA, RVA) | Object at entry | Thread | Status |
|---|---|---|---|---|
| L0 | vtable 0x145591CB8, slot 0, thunk 0x1442DF460 | one rig; rdx = rig+0xd0 | caller unknown | PROVEN thunk bytes: `mov rax,r8; lea rcx,[rdx-0xd0]; xor r8d,r8d; test rdx,rdx; mov rdx,rax; cmove rcx,r8; mov r8,r9; jmp 0x14431AFE0`. The slot is the only xref (ghidra_thunk.log). The virtual call site is NOT located. A byte scan (`lea reg,[reg+0xd0]` followed by an indirect call) gave about 70 sites, nearly all rig-class constructors (`call 0x140529500/0x140529550` with rdx = obj+0xd0); none is the per-frame caller. |
| L1 | 0x14431AFE0 (RVA 0x431AFE0), the rig eval | rcx = rig, rdx = view context `ctx` (views at ctx+0x40, stride 0x6A0, count ctx+0x1A940) | the caller of L0 (INFERRED: the thread that submits) | PROVEN |
| L2 | `[collection+8]` vtable +0x50 (Submit of a 0x60-byte entry, `call [rax+0x50]` at 14431B21C), or `call 0x144321940` synchronously when collection+8 is null (14431B20D) | the entry: [0] ctx, [2] owner = collection+0x300, [5] record array, [6] count, [8] pipe = collection+0x2E0, [9] mask | caller thread on the sync path; queue otherwise | PROVEN call sites. The Submit class is UNKNOWN (its vtable is not decoded). |
| L3 | batch body 0x144320340 (RVA 0x4320340), run slot 0x1442DFAF0 (`mov rcx,rdx; jmp 0x144320340`), 8 entries | batch object: entries at +0x00 to +0x2FF (0x60 each), count at +0x300 | job thread (INFERRED from the run slot; EDVR's kinematic_eval_hook.cpp kJobRvas names it "render-data batch", job 1) | PROVEN layout and run slot; the threading is INFERRED |
| L3' | single body 0x144321940 (RVA 0x4321940), run slot 0x1442DF520 (`mov rcx,rdx; jmp 0x144321940`) | one entry | job thread, or the caller on the sync path | PROVEN |
| L4 | per group: 0x144331300 (mask and window), then store 0x144312040 on entry[5], then the admission loop over members (stride 0x2F0) | group | inside L3 or L3' | PROVEN (decompile 4320340 lines 40-108) |
| L5 | store 0x144312040 per record: LOD nibble filter, sphere 0x144308B30, per-view 0x14430EFE0, pose 0x14433DB20 (line 153), child recursion (rec+0x2A8, +0x2B0), producer B 0x144312E00 for kind 2 (line 337) | record | inside the job | PROVEN (draw-path doc, decompile 4312040) |
| L6 | builder 0x1442B4420 (path A); producer B 0x144312E00 (path B) | instance or rig record | inside the job | PROVEN |

The "8 store threads" of the flight: the bytes fix a batch of 8 entries (0x300 = 8 x 0x60, count at +0x300), not 8 threads. The thread count is INFERRED from the job system and not counted here. L0 to L2 run on the caller; L3 and L3' run where the job system runs them; L4 to L6 never run outside a job body (their call sites are 0x43203F1, 0x43219BB and the store's own recursion, PROVEN).

### Q2. Readable at entry, before this frame's pose update

- **L1 (0x431AFE0)**, PROVEN from the bytes: rig+0x380 (state, must be 4; first instruction, 14431AFEC), rig+0x348 (collection), rig+0x350 (a 64-bit KEY, not a position: FUN_142840840 compares it with the entries of view+0x530), rig+0x190 (pointer, non-null tested; the same offset as a record's pose matrix A), rig+0x188 (int, tested against -1), rig+0x3D1, +0x3D2, +0x3D5, +0x3D6 (flags).
- **Name at L1**: NOT decoded. EDVR's probe already reads rig+0x20 ("associated game-object interface") and rig+0x50 ("rig descriptor") (kinematic_eval_probe.cpp 435-436). The chain from those to a name is INFERRED and must be read first.
- **Position at L1**: INFERRED. A rig sphere can be built the way the store builds a record's (node bounds at node+0x08, centre +0x20, radius +0x2C, transformed by A = rig+0x190 and B = *(A+0x50)). The rig pose is not written in 0x431AFE0, and physics writes node matrices rather than records (kmi 19:11 note), so last frame's value is valid at entry.
- **Record (L5)**: name rec+0x18, then node+0 (draw-path doc, PROVEN). World sphere: the store's own computation (4312040 lines 212-262, EDVR `storeWorldSphere`). The pre-update fields rec+0x240 and +0x280 are not the centre for leaves (draw-path doc Q4, PROVEN).
- **Open**: whether the rig at L1 is the same object as the "rig record" whose +0x2B8 holds the kind (draw-path doc). Not established.

### Q3. What a skip leaves undone

**The key fact (PROVEN, bytes 14431B221 to 14431B240).** 0x431AFE0 already has a branch for "no view sees this rig" (uVar9 == 0, or collection+0x70 bit 0 clear, decomp 431AFE0 line ~130). It runs every frame for an off-screen rig, and its whole effect is three writes:
1. `[collection+0x629] = 0` (byte).
2. `call 0x14434DD50` with rcx = collection+0x300, which is `mov dword [rcx+0x2A4], 0` (owner count = collection+0x5A4).
3. if `[collection+0x2E0] != 0`: `call 0x1442B5760` with rcx = that pipe, which is `add rcx,0x78; jmp 0x14434DD50` (pipe+0x31C = 0).

Nothing else is written. The view tests are read-only: 0x142840840 reads view+0x530; 0x14432C520 calls 0x142853010, 0x142854840 and 0x142852F80, which read view+0x688 only (decompiled). The branch skips: the group mask (0x4331300), the batch dispatch, the store (LOD, sphere, per-view gate, pose 0x433DB20, children), producer B, the builder, the admission loops, and the sub-object integrator 0x42B53F0. That integrator is called only in the non-empty group path (decomp 4320340: `if (*plVar10 != 0) FUN_1442b53f0` sits in the else of `local_48 == 0`, PROVEN). So a veto that performs this branch is, by construction, the engine's own off-screen handling.

Concerns, one by one:
- **Pose updates** (0x433DB20 writes record+0x170 and +0x240 to +0x24C for visited records): skipped. Physics does not read record poses (kmi 19:11). EDVR's engine motion sees a gap and re-baselines it (draw-path doc, item d). Safe.
- **Animation (0x42B53F0)**: writes each sub-object's 4-float state, scaled by `*ctx+0x1A96C` (PROVEN read). Skipped exactly as the engine skips it off-screen. On unseal it resumes without catch-up, so a spinning sub-part may pop (INFERRED).
- **LOD state** (rec+0x208, +0x234): the store rewrites them before any read on the next visited frame (4312040 line 150 sets rec+0x234 before the sphere test). Safe.
- **Physics and collision**: the physics job 0x432B2A0 is gated on rig+0x380 == 4 and uses the node matrices, not records (PROVEN gate, decomp 432B2A0 line 107). Not touched.
- **Owner pool**: the drainer 0x434D790 returns at once when owner+0x2A4 == 0 (decomp line 26, PROVEN). The v9 flight found 0 to 12 carried keys a frame, so no draw list persists (flight numbers, reported by the coordinator). Safe.
- **Streaming and audio**: audio is unreachable (memory: Wwise linked statically). Streaming not traced: INFERRED none on this path.
- **0x629**: a byte scan of .text for displacement 0x629 gave 31 candidate hits. The instruction-level uses found are in 0x431AFE0 (the write at 14431B1FC and 14431B228, and the final read). The rest are misaligned bytes (INFERRED from a sample).

**Earliest safe level: L1 (the entry of 0x431AFE0).** Above it:
- L0 (the thunk) adds nothing: it is a pure jump (bytes above), and the existing hook already sees every call that reaches 0x431AFE0.
- The virtual caller is not located, so its safety is not known.
- The other slots of the rig interface are not free to skip (see below).

**One-frame resync.** On unseal, the next frame takes 0x431AFE0's normal branch and rebuilds everything: the store rewrites the flags before the admission reads, the pose is recomputed for every visited record, the owner counts are reset by 0x14434DB60 (`mov [rcx+0x2A4],0`, decomp 434DB60), and the batch is rebuilt. This needs the job to complete in that frame. Only the sub-object integrators keep their frozen state.

**Ruled out:**
- ruled out: the thunk 0x1442DF460 as the veto point, because it is a pure jump to 0x431AFE0 and the hook there already sees every call.
- ruled out: a veto above the virtual caller, because slot 1 (thunk 0x1442DF480, which calls 0x14431ABD0) runs for the same rig and the caller is not located.
- ruled out: skipping slot 1 (0x14431ABD0), because it drains the rig's pending list at rig+0x2D0 (count +0x2C8; the element callback returns 1 to remove, decomp 431ABD0 lines 43-70) and reads rig+0x3D1 (line 109). Not shown safe.
- ruled out: forcing the empty branch by writing rig+0x3D1 = 0, because 0x14431ABD0 reads that byte in its callbacks (decomp 431ABD0 lines 69 and 109). The veto writes the three fields of the branch instead.
- ruled out: zeroing the view masks (view+0x570), because every rig reads them.
- ruled out: L2 (Submit), because it sits after the L1 gate, so it saves less than a veto at L1, and its class is unknown.
- ruled out: L3 and L3' (the job bodies), because the work is already submitted by then, and writing owner counts from a job thread would add a thread discipline that the engine's own off-screen branch does not use.
- ruled out: L4 (per group), because the group mask exists only after the dispatch, so a per-group veto keeps the L2 and L3 costs.
- ruled out: the store entry 0x4312040 (draw-path doc), because zeroing its input skips the pose update the children and the builder need.
- ruled out: the physics job 0x432B2A0 for render cost, because it is the physics side (node writes), and its mean is about 7.6 us a call (kmi 2026-09-20 job timings).

### Q4. Recommended veto

- **Point**: 0x14431AFE0 (RVA 0x431AFE0), entry. It is EDVR's `kRigEvalRva`, already hooked by `g_rigEvalEntry` (kinematic_eval_hook.cpp 273-274) with `rigEvalObserved`, which calls `noteRigLink` and then forwards. No second detour: add a veto check in `rigEvalObserved` before the forward. A veto returns 0 and performs the three-field branch above. The return value is unused: the slot is `void` (decomp 42DF460 and the thunk `jmp`). `installOne(g_rigEvalEntry)` (line 805) gates it.
- **Calling convention**: Microsoft x64. rcx = rig, rdx = ctx (the thunk moves the original r8 into rdx), r8 unused by the body. Same shape as the existing `RigEvalFn(uintptr_t,uintptr_t)`.
- **Prologue, first 16 bytes (exe file bytes)**: `4C 8B DC 55 57 48 81 EC A8 00 00 00 83 B9 80 03`. The first 5 bytes are the CodeHook site; the bytes must be hashed from the file, as the existing hook does.
- **Identity and position**: not yet. The first step is a read-only probe of rig+0x20, rig+0x50, rig+0x190 and the pointers they reach, plus a check that rig:collection is 1:1 (the riglink dump has the collection per rig).
- **Expected saving (INFERRED)**: the veto removes all of that rig's non-candidate work: the view loop, the dispatch, the store per record, producer B, the builder and the integrators. The producer-B veto left a floor of 3.3 ms with everything vetoed (flight), and that floor is what this removes for the rigs that are culled. Not measured. The sealed trial's per-record decision (about 0.33 ms a frame) falls to one decision per rig.
- **Confidence**: high that the three-field branch is the engine's own off-screen behaviour (PROVEN from bytes). Medium on the saving. Low on identity and position (not read).

### Risks

1. Identity and position are unread. The rig at L1 may not be the object whose +0x2B8 holds the kind.
2. Rig-to-collection sharing. If two rigs share one collection, the veto's write to owner+0x2A4 on one rig's frame could race another rig's batch on another thread. The engine's own off-screen branch makes the same write, but more often. Check 1:1 first.
3. The caller of L0 and its thread are not known.
4. The Submit class is unknown. The veto runs before the append, so no half-built entry can exist (PROVEN from the branch order).
5. Unseal: the sub-object integrators resume without catch-up (a possible visible pop, INFERRED).
6. Engine-motion gaps: the emit re-baselines; the masked frames count against the flight's motion data.
7. Any new key is temporary (AGENTS: config values name the functionality). List it in Status and quote it for removal.

### Not done

No build, no install, no flight, no config key, no code. Not decoded: the Submit class, the caller of L0, the name and position reads, the 1:1 collection check. Scratch tools (not committed): `xre.py` (capstone reader), `find_rig_callers.py`, `find629.py`, `ev_scripts\DecompileEV.java` (Ghidra headless, read-only project) in the scratchpad.
