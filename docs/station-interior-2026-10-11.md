# Station interior, the sun, and the hangar hull (2026-10-11)

Offline analysis of `EliteDangerous64.exe` build 332841 (PE stamp 1788384820, SizeOfImage 104894464, image base 0x140000000). RVAs are file RVAs. No C++ changed; no build; no flight. **Proven** means read from the exe bytes, from EDVR's decompiles in `analysis\decomp`, or from repo source. **Inferred** means the bytes do not close the point.

## Status

- **State:** analysis only. Branch `claude/station-interior-design` from `origin/claude/hangar-test-3` (998921da). Nothing fixed, nothing flown.
- **Interior system:** exists, as components, state names and a location enum. No renderer input from it was found. RenderDirLights (0x5AC9C0) reads no interior global, and no path from the interior state to the sun constants, the shadow views or the record masks was found.
- **Shadow admission:** proven end to end (P7). A record reaches a sun cascade only through per-view bits: the collection mask, a distance window, a planes test and an optional callback.
- **Why the hull is missing from the sun map:** not established. Four gates could drop it (G1 to G4, I1). The hull is drawn in the main view, so the failing gate is specific to the sun views. Ranking is inferred.
- **Culling:** the engine's occluder job (RenderOcclusionRasteriseJob, 0x27F8C60) has one caller chain, the spot-shadow setup (0x28295A0) inside the deferred frame. No main-view culling and no containment test were found. EDVR's own occluder design is refuted (`docs/design-occlusion-culling-2026-09-22.md`).
- **Ruled out:** a per-model cast-shadow flag (no string); a size clamp in the planes test (0x4F4E10 has no size term); a cascade-range cut for the hull (the cascades reach 4.5 to 6.5 km); VisibilityAnalysis as scene visibility (it is the sun and lens-flare test); InteriorLightsCSM as a sun suppressor (I4); the byte group 0x5E889C0-C3 as an interior writer (P10).
- **Open:** the writer of collection+0x48 (G1); the view constants of the sun views (G2); the storage of `player_location` (0x6031438 is only a registration object); the name-to-profile link for GameplayCSM and InteriorLightsCSM.
- **Next flight (no engine change):** extend EDVR's cull-gate probe (`src/d3d11/cull_gate_probe.h`, which already logs rec+0x208, rec+0x210, the pose, the sphere and the planes verdict) for the hangar. Add the collection's +0x48 mask, the sun-view bits, the view+0x68D flag, and a return hook on the callback thunk 0x288AC40. Decision table in D0.
- **Probe keys:** none added in this pass.
- **Build gate for any hook:** PE TimeDateStamp 1788384820 and SizeOfImage 104894464 (`src/common/cpu_sampler.h` lines 69-70, `src/d3d11/explorer_cam_core.h` lines 47-48, `src/common/hmd_camera_tag.h` lines 20-21).

## Proven

**P1. Interior components are created at station load.** `SpaceOrStationLoad` (0x1F4700) calls the factory for InnerDockLightingComponent (string 0x51A9928, function 0x12C9B70, call at 0x1F52E6) and for InternalSpacesManager (string 0x5213268, function 0x1B0BBC0, call at 0x1F67E6). 0x1B0BBC0 also names StationInteriorComponent (0x5213BC8) and StationInteriorHub (0x5213E48). PortalRotationComponent (0x4DDD538) is registered at 0x1F6A22 after a compare of a byte at 0x5EFEFA0, which is in .bss and so set at run time. The classes have no MSVC RTTI names, so their vtables cannot be found by class name.

**P2. Station lighting properties.** Function 0x12C8C54 registers named properties: PlayerCanSeeInside (0x51AA150), GuideMarkersVisible, LandingPadExtraLighting, OutfittingLightsOn, HangarLiftDoors__ and PadRetraction. It has no direct caller. These are light and door state, not render flags.

**P3. Location enum.** The blackboard block at 0x41D9BB0 registers `player_location` (variable object at .data 0x6031438) with the values inside_vacuum, outside_station, inside_station, inside_docking_bay, nearby_outpost, inside_docking_bay_outpost, inside_docking_bay_planet and unknown. The object is referenced only by three blackboard registration blocks (0x41D9CA5, 0x41DD486, 0x41E1EC2). Its value storage is not found.

**P4. Docking states.** Inside_Hangar, Outside_Hangar and StateTeleport_Inside_Hangar are strings (0x51A85F0, 0x51A8600, 0x51A8648) with pointers in .data at 0x5E78C58 to 0x5E78C98. No code reference to the table base 0x5E78C40 was found.

**P5. The sun constants.** RenderDirLights (0x5AC9C0, 3492 bytes) binds c_directionalLightColour, c_directionalLightDirection, c_directionalLightShadowCollectMask, c_numDirectionalLights, c_directionalShadowActive, c_globalLightScale and others by name. Its only .data operands are at 0x5E70838 and 0x5E70848; neither is an interior global. The per-frame names (gHasDirectionalLights and so on, 0x5039410 onward) are name strings bound in 0x57B990, not storage. Their writers are not identified.

**P6. Shadow cascades are data-driven profiles.** `GraphicsConfiguration.xml` in the game root (line 241 onward) holds `DirectionalShadows_Low`, `_Medium`, `_High` and `_Ultra`. The Low, Medium and High sections list `Profile_Space_Cockpit`, `Profile_Space_Humanoid`, `Profile_Space_AsteroidField`, `Profile_Planet_Cockpit` and `Profile_Planet_Humanoid` and no station, hangar or interior profile (Ultra not checked). Examples of EndDistance (assumed metres): Medium Space_Cockpit 5, 289.9, 5000; Medium Space_Humanoid 3, 267.1, 4500; High Space_Cockpit 5, 80.8, 739.2, 6500. CullingBias FixedOffset is 3500 to 10000 and ExtentScale 3 to 7. GameplayCSM and InteriorLightsCSM are absent from the XML. In the function whose parts include 0x1442F48 to 0x14430BF, the two CSM names sit beside "Shutter Open" and "Shutter Closed" strings and are chosen by a virtual-call check.

**P7. Shadow admission chain.** Decompiles: `decomp_4320340`, `decomp_4331300`, `decomp_42B4420`, `decomp_4312040` (EDVR's, same build).
- Traversal, 0x4320340 and 0x4321940. Per record (stride 0x2F0): the pose pointer at rec+0x290 is non-null, and the bytes at rec+0x234 and rec+0x298 are non-zero. For each set bit of the view mask rec+0x208, the LOD nibble at rec+0x210 is compared with a limit at (rec+0x18)+0x6A. The item then goes to the builder.
- Per-item view mask, 0x4331300 (8 parts). The first argument is the collection struct; the third is the record pointer plVar10[-3], whose pose and sphere are read. Whether this runs per record or once per array is not settled. Each view k is at render context +0x40 + k*0x6A0, with the count at ctx+0x1A940. View bits are m = view+0x570. A view is active if collection+0x48 & m is non-zero. A distance window follows, with the record centre from its pose and the radius at rec+0x280, measured from the view origin (view+0x540). Passing views OR their m into the output mask.
- Store, 0x4312040 (writes rec+0x208; reads rec+0x240 and rec+0x280).
- Builder, 0x42B4420 (7 parts). Per view: test (view+0x570 & mask) at 0x42B456D; planes call `call 0x4F4E10` at 0x42B4595, skipped when it returns -1; if DAT_145EA3399 is set and view+0x68D bit 0 is set (test at 0x42B45A8), the callback 0x288AC40 at 0x42B45D0; then view+0x570 is ORed into the output at 0x42B45DE.
- Planes, 0x4F4E10 (a leaf; no .pdata). Box (centre and absolute extents) against each plane (array at view+0x30, count word at +0x44). The caller skips on -1.
- Gates on disk: DAT_145EA3398 = 1 and DAT_145EA3399 = 1; no RIP writer. Callback body 0x288A930 (5 parts) reads .data gates 0x5E82B00 to 0x5E82B03, all 1 on disk, and a .bss byte at 0x5F9941D (run-time).
- Second per-(record, view) gate, 0x430EFE0 (EDVR's kinematic-hook target). It also calls the callback. EDVR's notes say its verdict feeds only the type-2 item path, so the static hull is probably not on it (I5).
- Not a sun bit. A sun cascade is an ordinary view whose view+0x570 bit is set in the sun collection's active mask. The bit values are not yet named.

**P8. Sun view creation.** The depth builder 0x5B2C80 (`DirectionalShadows.Depth.%p`) submits through 0x6C2110, which calls 0x6C0680 and does not touch view+0x570 or rec+0x208. The MSM pass is 0x5B3530. Spot-shadow views come from 0x28295A0 (`SpotShadowView`). The writers of view+0x68D are 0x2E7FB80 (store at 0x2E81948), 0x3596D20 (store at 0x3597805) and 0x35E6A40 (store at 0x35E6ACC, called from 0x35FE6B0). None is yet named as a sun view.

**P9. Occlusion and portal strings.** 0x27F8C60 (RenderOcclusionRasteriseJob, RenderOcclusionQueue, OccluderDepthPrepassBatch, OccluderDebugBatch) has one caller, 0x28295A0, which has one caller, 0x284E2C8 in the deferred frame 0x284CB70. OcclusionOutput (0x36426A0) binds planet and atmosphere constants. VisibilityAnalysis (0x36D50E0) holds SunSphereVisibilityAnalysis, SunVisibilityAnalysisMask and SunLensFlareVisibilityAnalysis. CellResolution and PartitionDistances (0x28555A0) are the cascade-settings parser. cellId, motherlodes and asteroids (0x1C04940, 0x203F8F0) are asteroid streaming. PortalGeometryInstanceComponent (0x28E70A0) has no visibility consumer found.

**P10. Byte group.** 0x5E889C0 to C3 are read by RIP-relative compares only (0x35F8A46, 0x35A7C0E, 0x35EB74A, 0x35A80F9, 0x360092A). The reader at 0x35EB720 is StarLightMgr. No RIP-relative writer was found.

## Inferred

- **I1. The hull fails a sun-view gate.** The gates in the order the code applies them:
  - G1: collection+0x48 lacks the sun-view bits, so the hull's collection is not sun-active.
  - G2: the distance window in 0x4331300 fails for the sun views. The sign convention is inferred, not checked.
  - G3: the planes test returns -1 for the hull's box, for example because the transformed bounds of a rotating model are stale.
  - G4: the callback (0x288A930) vetoes the hull for views with view+0x68D bit 0.
  The probe in D0 separates them.
- **I2. Cascades run, the hull is absent.** The hangar walls are inside the sun cascade range, and ship shadows reach them. That fits a missing caster for the hull, not a missing cascade.
- **I3. The interior state does not reach the sun in this build.** The components (P1) and the location enum (P3) describe presentation and state; no renderer reader was found. The location value is a candidate engine inside signal only once its storage is found.
- **I4. InteriorLightsCSM is a cascade-target name chosen by a shutter check.** Whether choosing it changes the sun's cascades is not shown, because the profile mapping is unread.
- **I5. The static hull is not on the 0x430EFE0 gate.** Taken from EDVR's notes about the type-2 path.

## Dead ends

- Interior flag into the sun pass (D2 below): no reader found; location storage unknown (P3).
- Per-model cast-shadow flag or size cut: no string; no size term in the planes test (P7).
- Cascade range cut: the last cascade reaches 4.5 to 6.5 km (P6).
- Engine occluder job for main views: reached only through the spot-shadow setup (P9).
- EDVR-side occluder culling: refuted (`docs/design-occlusion-culling-2026-09-22.md`).

## Design, ranked

The probe in D0 selects the gate. Each fix below is then chosen by that gate. None is coded.

### D0. Measure the failing gate (no engine change; next flight)
- **What:** reuse EDVR's cull-gate probe. For the hangar, log per builder call: the hull record (rec+0x280 near the station radius), collection+0x48, the sun-view bits (view+0x570 of views with view+0x68D bit 0 set), the planes verdict, and the callback verdict from a return hook on 0x288AC40 (bytes `48 83 EC 38 41 0F 10 01 4C 8D 4C 24 20 0F C6 C0`). The probe never calls the callback, so the hook reads its return value only.
- **Decision:**
  - collection+0x48 lacks the sun bit: G1. Find the writer of collection+0x48 next.
  - Window fails for the sun views: G2. Adjust the per-view window constants at view+0x540 to +0x560 for those views.
  - Planes return -1: G3. Fix the transformed bounds of the record, not the test.
  - Callback returns 0: G4. Read the branch in 0x288A930 for that record.
- **Cost:** a compare and a log line per builder call, in the hangar window only.
- **Flight:** docked, rotating hangar, hangar doors closed. Read the window line for the hull in each sun view.

### D1. Admit the hull to the sun cascades (root cause; after D0)
- **What:** change only the gate that D0 names, and only for sun views, so that a record whose box or sphere contains the camera is admitted. The walls then sit in the hull's shadow, which is the physical result.
- **Hook sites by gate** (file RVAs; prologue bytes for a CodeHook pin):
  - G3, the planes call at 0x42B4595 (`E8 76 08 24 FC`); the relay returns "inside" for sun views when the box contains the camera.
  - G4, the callback thunk 0x288AC40 (`48 83 EC 38 41 0F 10 01 4C 8D 4C 24 20 0F C6 C0`).
  - G2, the window constants at view+0x540 to +0x560 of the sun views, set in the view setup (writers not yet named).
  - G1, the writer of collection+0x48, not yet identified.
  - Builder prologue 0x42B4420 (`4C 8B DC 55 56 41 54 41 57 49 8D AB 08 FC FF FF`); window 0x4331300 (`48 8B C4 53 57 48 83 EC 68 48 89 68 10 48 8B FA`); store 0x4312040 (`40 55 53 56 57 41 56 41 57 48 8D AC`); planes 0x4F4E10 (`41 0F 10 18 45 33 C9 0F`).
- **Outposts and surface ports:** the change applies to sun views only, and only to records the failing gate rejected. Outpost hulls then cast shadows as they should. Open pads stay sunlit where the sun crosses the open side. The flight checks an outpost pad.
- **Per-frame cost:** one compare per record per sun view. The sun map gains the hull's draws; the count is unmeasured.
- **Gate:** the PE pair in Status; refuse to install on a mismatch.
- **Flight test:** toggle a temporary probe key (recorded in the arc's Status when added). Docked and rotating: with the hull cast, the walls stop cycling, ship shadows persist, the outside view and an outpost pad are unchanged, and the sun-pass draw count rises only by the hull's draws.

### D2. Attenuate the sun while inside (fallback; needs an engine signal)
- **Hook:** RenderDirLights entry 0x5AC9C0 (`48 89 5C 24 20 55 56 57 41 54 41 55 41 56 41 57`). Scale c_globalLightScale to 0 when `player_location` is inside_docking_bay. Outpost and planet values are separate names (P3), so they are not affected.
- **Blocked:** the storage of `player_location` is not found (P3). Until it is, the only inside signal available is Status.json's docked flag, which is the docked or station-type rule Sean ruled out.
- **Cost:** one byte compare per directional draw.
- **Flight test:** the skip toggle; with the constant at 0, the cycle stops; the outside view and outpost pads are unchanged.

### D3. Cull exterior geometry when inside (not recommended now)
- No main-view occluder path exists (P9). The census measures hidden draws (68 to 91 percent of scene draws in the hangar), which is a cost, not the visible bug. A hull occluder would reuse the refuted EDVR design. Park it.

## Method

- Index `interior\build_index.py` (capstone over 253,443 .pdata functions) and query helper `interior\q.py`; scratch files, not committed. Validated: the RenderDirLights caller (0x284E159) and the slot 0x52E94E8 match `docs/hangar-sunlight-2026-10-11.md`.
- Open limits: the window's sign convention (I1 G2), the storage of the per-frame directional globals, and the CSM profile mapping.
