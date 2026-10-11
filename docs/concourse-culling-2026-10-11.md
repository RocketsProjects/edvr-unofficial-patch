# Concourse culling and sealed hangar (2026-10-11)

Offline analysis of `EliteDangerous64.exe` build 332841 (TimeDateStamp 1788384820, SizeOfImage 104894464, base 0x140000000; gate matched). RVAs are image RVAs. No C++ changed, no build, no flight. **Proven**: exe bytes, EDVR source, or key names from the user's own journal. **Inferred**: not closed by the bytes. The scratch tool `xr.py` (capstone and numpy xrefs) is not committed.

## Status

- **State:** analysis and design only. Branch `claude/concourse-cull-design` from `claude/station-interior-design` (76c08530). Nothing fixed, flown or merged.
- **Primary: sealed hangar.** Docked, pad lowered, doors closed. Submit only the hangar cell and suppress the sun. Blocked on storage of pad and door state (P9).
- **Secondary: closed panes.** Seated inside a station (doors open, or flying inside). Cull the SocialSpace (concourse) records and replace the bay windows with mirror-like opaque glass (stage 1, Sean's decision). Impostor is a later stage only.
- **Concourse:** the SocialSpace sub-area under StationInteriorHub, created at station load (P1).
- **Triggers now:** seated = Status Flags bits 24-26 or Flags2 bits 1-2 (EDVR `journalSeatedFromFlags`). Disembark = journal Disembark with SRV, Taxi and Multicrew false (P5). Status OnFoot lags about 6 s, so it is not a restore trigger.
- **Ruled out:** Status.json as the sealed signal; ToggleCockpitRender as a render switch; InteriorStateChangeActivity as a switch (P7).
- **Open:** pad and door storage (P9); cell radius; pane draw hashes and ownership; env slot numbers; player_location read path (P10).
- **Next flight, no engine change:** M1 record-attributed census, then M2 storage dump.
- **Probe keys:** none added. **Gate for any hook:** TimeDateStamp 1788384820, SizeOfImage 104894464.

## Proven

**P1. Concourse ownership.** Factory 0x1B0BBC0 (called by SpaceOrStationLoad 0x1F4700, which 0x1FD45B calls) creates InteriorSubLevelAttach (0x5212CF0), StationInteriorHub (0x5213E48), StationInteriorComponent (0x5213BC8), TurboLiftSystem (0x5211FF0), DockingArmBoardShipButton (0x52122E8) and SocialSpaceBroadcastSync (0x5213550). The sub-area builder is 0x1B17C2E ("Not ready to make interior sub-area level"), which calls 0x1B17F70. The name "Interiors" (0x5211CC8) goes to registrar 0xB7BD0. Concourse_Bar and five other Concourse_* shops (0x51E19B0-0x51E1A30) are named in table builder 0x17425F0. The lift header ToSocialSpace ties SocialSpace to the concourse (inferred).

**P2. The lift.** TurboLiftComponent, TurboLiftComponent::State, TurboLiftDoorBank, TurboLiftPassenger (0x52148F0) and TurboLiftTerminal are created by 0x1B0BBC0. No code reads these names.

**P3. Pad and door properties.** Function 0x12C8C54 (1365 bytes) registers PlayerCanSeeInside (0x51AA150), HangarLiftDoors__ (0x51AA1C0), OutfittingLightsOn (0x51AA1A8) and PadRetraction (0x51AA198) on station-lighting objects (this+0x240 for the first two). HangarLiftDoors__ is copied inline into a descriptor at +0x14 with a default byte at +0x24 (from 0x51AA1D0). That is a descriptor, not the live value (inferred). Function 0x12C93A0 registers LandingPad (0x51AA140) with PadRetraction and LandingPadExtraLighting.

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
  - **Draws:** the pane draws, found by M1 as instanced draws with stable VS and PS hashes. Exclude the cockpit canopy, which belongs to the ship record and keeps its own shader.
  - **Output:** col = tint * env(R) * F + base, alpha 1. R = reflect(-V, N). F = Schlick with F0 about 0.04, or c_envmapTextureFresnelLUT. Tint: a constant, slightly cool grey.
  - **Env:** gEnvmapTexture (cube) through EnvmapSampler, with the view-space R rotated by c_viewToEnvmapRotationT. For interior reflections, prefer g_Tex_ReflectionTexture (MirrorSampler) where a reflection volume covers the pane. Coverage of the concourse is an M1 item.
  - **Env slots:** not in the exe. At runtime, D3DReflect the bound game PS bytecode for the bind points of gEnvmapTexture and EnvmapSampler, and for the cbuffer offsets of c_cameraPosition and c_viewToEnvmapRotationT. At draw time read the bound SRV and sampler at those points (PSGetShaderResources, PSGetSamplers) into fixed slots of the replacement. If the pane's own PS does not sample the envmap, take the frame-global envmap SRV bound by another draw and cache it.
  - **View vector:** V = normalize(c_cameraPosition - P). P is the pane VS world position if its outputs carry one; otherwise the intersection of the pixel's view ray (from c_inverseViewProjectionMatrix) with the pane's plane. The second needs no VS change. c_cameraPosition comes from the frame's view constant buffer, through the existing cbWatch (draw_census.cpp).
  - **State:** blend off, depth write on, depth function GREATER_EQUAL (reversed-Z); restore everything else after the draw (planet_motion pattern).
  - **Cost:** unmeasured. The screenshot framing gives about 150k pixels across five panes, one cube sample and a few ALU each. CPU: about four state sets per pane draw per frame.
  - **Complexity:** medium: a census for hashes, a replacement PS through tools/temporal_shader_build, one constant buffer, state save and restore.
  - **Risk:** a pane that writes depth without motion vectors may ghost in the temporal pass. Check in flight.
- **Stage 2 (later, only if stage 1 fails):** an impostor per pane, a colour view of the concourse from each pane centre, rendered once per visit. The engine's view path is depth-only for shadows, so colour capture is new work. High complexity.

## Measurement

- **M1. Record-attributed census.** Tag draws with their record. Probe the first three calls of builder 0x42B4420 for the record argument; attribute draws by DrawIndexed return address (draw_census.cpp). Per record: triangles, visible pixels, collection, bounds, sealed-cell membership. Run (a) docked, pad lowered: triangle and draw share outside the cell; (b) flying inside a station: pane draws, hashes and env coverage.
- **M2. Storage and timing.** Dump the LandingPad object and the lighting object at +0x240 across one pad cycle and one door cycle. Find the bytes that change and log them per frame beside the dock-state name. Measure the release lead (state edge to first door motion) and the lag (journal Disembark line to first on-foot frame).
- **M3. Outposts.** Dock at an open-pad outpost. Log NoHangarPresent, the dock state and PadRetraction. The gate must stay false.
- **Flight tests:** sealed hangar with the cull on and off (draws, frame time); the door-open transition watched for pops with the release at the state edge; the pane look at 50-150 m against the screenshot framing; disembark restore against the Disembark journal line.

## Open items

Pad and door storage (M2). Cell radius and hangar record set (M1). Pane hashes and hull or concourse ownership (M1). Env slot numbers (runtime reflection). player_location read path (P10). Status flag names beyond the bits EDVR decodes.
