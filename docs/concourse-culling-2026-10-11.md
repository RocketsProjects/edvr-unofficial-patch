# Concourse culling and sealed hangar (2026-10-11)

Offline analysis of `EliteDangerous64.exe` build 332841 (TimeDateStamp 1788384820, SizeOfImage 104894464, base 0x140000000; gate matched). RVAs are image RVAs. No C++ changed, no build, no flight. **Proven**: exe bytes, EDVR source, or key names from the user's own journal. **Inferred**: not closed by the bytes. The scratch tool `xr.py` (capstone and numpy xrefs) is not committed.

## Status

- **State:** analysis and design. Flight 7 flew the sealed-hangar trial at 80 m in the docked hangar: nothing missing (see Measurement). Built v4 on `claude/hangar-test-5` (from `claude/hangar-test-4`, which carries the camera rule): the radius steps, the 5 s windows and the ladder; full build green; the step census and the approach flight are not flown yet. Nothing fixed. M1 is blocked on draw identity (see Measurement).
- **Primary: sealed hangar.** Docked, pad lowered, doors closed. Submit only the hangar cell and suppress the sun. Blocked on storage of pad and door state (P9).
- **Secondary: closed panes.** Seated inside a station (doors open, or flying inside). Cull the SocialSpace (concourse) records and replace the bay windows with mirror-like opaque glass (stage 1, Sean's decision). Impostor is a later stage only.
- **Concourse:** the SocialSpace sub-area under StationInteriorHub, created at station load (P1).
- **Triggers now:** seated = Status Flags bits 24-26 or Flags2 bits 1-2 (EDVR `journalSeatedFromFlags`). Disembark = journal Disembark with SRV, Taxi and Multicrew false (P5). Status OnFoot lags about 6 s, so it is not a restore trigger.
- **Ruled out:** Status.json as the sealed signal; ToggleCockpitRender as a render switch; InteriorStateChangeActivity as a switch (P7).
- **Open:** pad and door storage (P9); cell radius; pane draw hashes and ownership; env slot numbers; player_location read path (P10).
- **Next flight, no engine change:** the Built v4 plan in Measurement: docked, step the trial down the ladder one rung at a time with a census (F6) at each rung, and note the smallest radius where everything still looks complete; then the approach state dump (F3, `diag.state_dump_frames = 6000`) and the look probe at the concourse bay windows. The Built v3 and v2 plans still stand for the rest (the open-pad outpost, M3, is in v2). The M1 draw link is the open item.
- **Probe keys (TEMPORARY):** `hotkey.state_dump`, `hotkey.look_probe`, `hotkey.sealed_trial`, `hotkey.sealed_trial_smaller` and `hotkey.sealed_trial_larger` (all unbound by default), `diag.state_dump = off`, `diag.state_dump_frames = 3000` (60 to 6000; 6000 for the approach dump), and `diag.sealed_trial_radius_m = 80`. **Gate for any hook:** TimeDateStamp 1788384820, SizeOfImage 104894464.

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

## Open items

Pad and door storage (M2). Cell radius and hangar record set (M1). Pane hashes and hull or concourse ownership (M1). Env slot numbers (runtime reflection). player_location read path (P10). Status flag names beyond the bits EDVR decodes.
