# Concourse culling and sealed hangar (2026-10-11)

Offline analysis of `EliteDangerous64.exe` build 332841 (TimeDateStamp 1788384820, SizeOfImage 104894464, base 0x140000000; gate matched). RVAs are image RVAs. No C++ changed, no build, no flight. **Proven**: exe bytes, EDVR source, or key names from the user's own journal. **Inferred**: not closed by the bytes. The scratch tool `xr.py` (capstone and numpy xrefs) is not committed.

## Status

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
