# Station interior, the sun, and the hangar hull (2026-10-11)

Offline analysis of `EliteDangerous64.exe` build 332841 (PE stamp 1788384820, SizeOfImage 104894464, image base 0x140000000). RVAs are file RVAs. No C++ changed; no build; no flight. **Proven** means read from the exe bytes, from EDVR's decompiles in `analysis\decomp`, or from repo source. **Inferred** means the bytes do not close the point.

## Status

- **State:** D0 v2 built on `claude/shadow-gate-probe` (v1 is `8b4ebbe6`). The sealed-hangar trial was flown at 80 m in the hangar (flight 7: nothing missing; the sun bleed gone). v4 (the radius steps and the 5 s windows, the last section) is built on `claude/hangar-test-5`: full build green; NOT installed for the step census yet. Nothing fixed.
- **Interior system:** exists, as components, state names and a location enum. No renderer input from it was found. RenderDirLights (0x5AC9C0) reads no interior global, and no path from the interior state to the sun constants, the shadow views or the record masks was found.
- **Shadow admission:** proven end to end (P7). A record reaches a sun cascade only through per-view bits: the collection mask, a distance window, a planes test and an optional callback.
- **Why the hull is missing from the sun map:** not established. Four gates could drop it (G1 to G4, I1). The hull is drawn in the main view, so the failing gate is specific to the sun views. Ranking is inferred.
- **Culling:** the engine's occluder job (RenderOcclusionRasteriseJob, 0x27F8C60) has one caller chain, the spot-shadow setup (0x28295A0) inside the deferred frame. No main-view culling and no containment test were found. EDVR's own occluder design is refuted (`docs/design-occlusion-culling-2026-09-22.md`).
- **Ruled out:** a per-model cast-shadow flag (no string); a size clamp in the planes test (0x4F4E10 has no size term); a cascade-range cut for the hull (the cascades reach 4.5 to 6.5 km); VisibilityAnalysis as scene visibility (it is the sun and lens-flare test); InteriorLightsCSM as a sun suppressor (I4); the byte group 0x5E889C0-C3 as an interior writer (P10).
- **Open:** the writer of collection+0x48 (G1); the view constants of the sun views (G2); the storage of `player_location` (0x6031438 is only a registration object); the name-to-profile link for GameplayCSM and InteriorLightsCSM.
- **Flight 5 (2026-10-11, `edvr_gfx_20261011_133053`, docked twice and 1 km out):** `DirectionalShadows.Depth` 0x5B2C80 was never called in the stamped frame, and MSM 0x5B3530 once. `sun=none`, so no depth or MSM argument matched a view, and the argument was never logged. The hull base (cand0) passes every record gate in MAIN and views 1, 2 and 4, and fails G2 in view 3 (views 2 to 4 look like a cascade triplet, bits 36 to 38). The siblings cand2 to cand7 were unreached. `vis_applies` is 0 everywhere, so G4 does not apply. The window replica disagreed with the engine's bit about 21% of the time.
- **Next flight (no fix):** the v4 plan in the last entry: the sealed trial stepped down the ladder with a census at each rung, then the approach flight. The v3 plan (the look probe, then F4 with the trial on) still stands, and so does the D0 v2 plan for the rest: docked, F4 twice (bright and dark), then F3 and a deliberate launch within the 35 s window, then 1 km out and F4 once, with Pause presses in flight. F4 and F3 are the keys Sean has bound (`hotkey.shadow_gate_probe` and `hotkey.state_dump`; check the live ini).
- **Probe keys (TEMPORARY; remove when the arc closes, quoting each):** `hotkey.shadow_gate_probe` (unbound by default) and `diag.shadow_gate_probe = off` in `[diag]`. Both are on the flat allow-list. v3 adds `hotkey.look_probe` and `hotkey.sealed_trial` (unbound by default) and `diag.sealed_trial_radius_m = 80`. v4 adds `hotkey.sealed_trial_smaller` and `hotkey.sealed_trial_larger` (unbound by default).
- **Corrections (from the decompiles, 2026-10-11):** the builder's `mask` is the window's output for the group's base record, not the collection mask; children inherit the base's window verdict through `rec+0x2A8`; 0x288AC40 cannot be CodeHooked (SSE prologue), its callee 0x288A930 can.
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
- **Flight:** docked, rotating hangar, hangar doors closed. Read the `cand=` line for the hull: its first failing gate in the sun views (the reading rule is in the 2026-10-11 D0 entry).

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

## 2026-10-11, D0 probe built (not installed, not flown)

Branch `claude/shadow-gate-probe` from `76c08530`. The probe is the measurement D0 asked for: `src/d3d11/shadow_gate_probe.cpp` (engine side), `src/d3d11/shadow_gate_core.h` (the pure half: layout, transforms, gate decode, line and CSV formats), and `tools/shadow_gate_probe_test` (rig, 101 checks). Full `build.bat` green. With the keys off, nothing is hooked.

**Three corrections from the decompiles (they change the D1 site).**
1. The builder's `mask` argument is the window's output for the group's base record (`out[4]`, the traversal's `local_48`), not the collection's mask. The comment in `src/d3d11/cull_gate_probe.h` that calls it the collection's active mask is wrong.
2. The window (FUN_144331300) runs once per collection, on the group's base record. The store (FUN_144312040) writes that mask into `rec+0x208` after a LOD-limit filter, then recurses over the base's child list (`rec+0x2A8`, count `+0x2B0`). A child's G2 is its base's G2.
3. FUN_14288AC40 cannot be CodeHooked: its first instruction is `movups` (`41 0F 10 01`), which `codeInstructionLength` refuses. It is a pure forwarder (broadcast `[r9]`, `call FUN_14288A930`, `add rsp,38h`, `ret`), so the probe hooks its callee and reads the same AL.

**Hooks** (build 332841; each installs only when its pinned bytes match the exe, and its own line says why otherwise).

| RVA | What | Pinned bytes | Arguments forwarded |
|---|---|---|---|
| 0x4331300 | window, per collection: (collection, out, base record) | `48 8B C4 53 57 48 83 EC 68` (5 bytes stolen) | rcx, rdx, r8; RAX returned |
| 0x288A930 | callback body, the callee of FUN_14288AC40 | `40 55 56 57 41 56 41 57` (6 bytes stolen) | rcx, rdx, r8, r9; AL returned |
| 0x5B2C80 | DirectionalShadows.Depth | `48 89 5C 24 10` (5 stolen) | rcx, rdx, r8, r9; rcx is the object the `%p` format prints |
| 0x5B3530 | MSM pass | `48 89 5C 24 20` (5 stolen) | rcx, rdx, r8, r9 |

Also read, not hooked: the planes test 0x4F4E10 (`41 0F 10 18 45 33 C9 0F`), which the probe calls on the builder's box as `cull_gate_probe.cpp` does; and the call sites 0x42B45D0 (`E8 6B 66 5D FE`) and 0x288AC56 (`E8 D5 FC FF FF`), which the install checks. The builder is observed through the gate probe's observer slot (`kinematicEvalSetGateProbeObservers`), taken only while the probe runs. The probe refuses to start while the cull probe is armed; the cull probe has no such check, so an eye run that arms it during a shadow capture would replace the probe's builder observer (the capture would then log no builder rows).

**G4 attribution.** At the callee's entry, the stack holds the return into the callback, and the callback's own return (into the builder) sits 0x40 above it. The probe counts a callee call as the builder's only when that return equals 0x42B45D5. Its verdict is matched to the builder's eligible views in the builder's test order: G1 passes, planes not -1, and `view+0x68D` bit 0 with the global set.

**Sun views and MAIN.** The sun views are the views whose address is the object the depth or MSM builder receives first. Not verified: that this object is a view. If none matches, the header says `sun=none` and the CSV keeps every view the bucket's mask holds, so the cascades can be identified from their origins and window constants. MAIN is view 0 by convention (not verified).

**Candidates.** The 8 largest records whose sphere (`rec+0x240`, radius `rec+0x280`) holds the camera, and the 8 largest within 2 km. The camera is `ctx+0x00`, the store's window origin (not verified against the view origins). They are read at the Present after the captured frame.

**Output.** `shadow gate probe:` lines in the graphics log: a header (frame, trigger, the sun list, the match counts, bucket and builder counts, `window_mismatch`, `callbacks`, `extra`); one `cand=` line per candidate (role base, child, orphan or nobucket, and the first failing gate for each sun view and for MAIN); and `done` (with `callee_calls` and `first_callee_return`, which must equal the builder site). `edvr_logs\shadow_gate_<stamp>.csv` has one row per candidate and view: G1 (bucket mask), the window bit with its replicated value and threshold, `rec+0x208` (final), whether the builder reached the record, its mask bit, the planes verdict, the visibility flag, G4, and `first_fail`. The gate order is G1, G2 (`noWindow` when no window was observed), LOD (the window passed and `rec+0x208` lost the bit), unreached, G3, G4.

**Flight (Sean; not run here).**
1. In the live `edvr.ini`, bind `hotkey.shadow_gate_probe` to a key Elite does not bind, and raise `log.max_mb` to 256. `python tools\install_edvr.py --target <target> --dry-run`, then the install, then `python tools\edvr_log.py --target <target> --expect-build HEAD --version`.
2. Dock at a rotating hangar. Press once in a bright phase, wait for the cycle to turn, and press once in a dark phase.
3. Launch, hold about 1 km outside the same station, and press once. Optional: an outpost pad.
4. Before reading any gate: `callee_calls` must be above zero and `first_callee_return` must equal the builder site (base + 0x42B45D5). `window_mismatch` must be 0. If `sun=none`, the depth builder does not take the view, and the view rows are the evidence.

**Reading the result.** Take the hull: the `cand=` line for the station's base (role `base`) or a child (role `child`) whose sphere contains the camera. Its first failing gate in the sun views, with MAIN admitting:
- G1: the collection's mask lacks the sun bits. Next: the writer of `collection+0x48`.
- G2 or noWindow: the group's window fails for the sun views. The group's base decides every child, so the fix is at the window (0x4331300) or in the view constants (`view+0x540` to `+0x560`).
- LOD: the window passed, but the store's LOD filter cleared `rec+0x208`. Next: the record's LOD limit, `(rec+0x18)+0x68`.
- unreached: the traversal's gates (`rec+0x234`, `+0x290`, `+0x298`) never sent the hull to the builder.
- G3: the planes test on the hull's box returns -1. Fix the box (the transformed bounds), not the test.
- G4: the callee vetoes. Read its branch in 0x288A930.

The D1 hook is the first gate that is 0 in the sun views and 1 in MAIN for the hull record.

**Risks.** The sun identity (the depth object) and MAIN (view 0) are unverified until the first flight. The camera (`ctx+0x00`) is unverified. The replicated window uses an exact `1/d`, where the engine uses `rcpps`/`rsqrtps`, so a value at the threshold can disagree; `window_mismatch` counts those. The detours go in at the first press, in VR and flat, as the existing probes' CodeHooks do. A refused detour leaves its column unknown, and its own line says why.

## 2026-10-11, D0 v2 (after flight 5)

Branch `claude/shadow-gate-probe`. Flight 5 (above) answered two things and left the sun views unnamed: the depth builder ran zero times in the stamped frame, and the MSM pass ran once, so the one-frame capture could not see the cascades. v2 changes the measurement, not the gates.

**What changed.**
1. **The cadence.** The depth builder (0x5B2C80) and the MSM pass (0x5B3530) are counted for 600 frames after a press, on their own gate. Each frame's count goes into a histogram line (0, 1, 2, 3, 4 or more calls) for each builder, and into `edvr_logs\shadow_cadence_<stamp>.csv` (`frame,dock_status,dock_location,depth_calls,msm_calls`). A dock line splits the frames into docked and outside, by Status Flags bit 0 or the Location line's Docked flag. Read: a builder that is never called in any window does not run in the hangar; a builder that runs on a frame pattern shows the pattern.
2. **The sun views.** Each shadow builder call's four arguments (rcx, rdx, r8, r9) are recorded for the first 256 calls, and the first 48 are logged as `shadow call n:` lines, each with the view its argument named. Every view of the target frame's context is then logged (`shadow sun view k: ptr= bits= k1= k2= origin= depth_hits= msm_hits= shadow=yes|no`). The depth builder's disassembly keeps rcx (the object its `%p` prints), r8 and rdx alive, and reads r9 as an int; all four are logged so the match does not depend on that reading. A view that a shadow builder named is `shadow=yes`. If no argument matches any view, every line says `shadow=no`, and the sun views are not identified by these builders.
3. **G2 is the engine's output only.** The window replica (`window_v`, `window_t0`, `window_repl`, `window_mismatch`) is gone. The window bit in the CSV is the output mask the engine wrote.
4. **The callee check** applies only when a row had `vis_applies`. Otherwise the done lines say `callee check: not applicable`.
5. **Rows are written at the cadence's end**, with the sun roles known. The candidate reads and gate values are still taken at the capture frame, so the record fields are the frame's own.

**Formats.** Log: the `shadow gate probe:` header (frame, trigger, build, ctx, views, camera, main, buckets, records, builders, dropped, callbacks, extra, csv); `cand=` lines (sun and main first failing gates); `shadow gate probe: callee check: ...`; `shadow cadence depth calls per frame:` and `shadow cadence MSM calls per frame:` histograms; `shadow cadence dock:`; `shadow sun view k:`; `shadow call n:`; `done`. CSV: `shadow_gate_<stamp>.csv`, 23 columns (`frame,candidate,kind,record,bucket,base,role,radius_m,dist_m,contains,view,view_role,view_bits,g1_bucket,window_bit,rec208_bit,builder_reached,builder_row,builder_mask_bit,planes,vis_applies,g4,first_fail`), and `shadow_cadence_<stamp>.csv`.

**Flight plan (Sean, 2026-10-11).** In a rotating hangar, docked and with the pad lowered and the doors closed: press F4 (`hotkey.shadow_gate_probe`) in a bright phase and again in a dark phase. Then press F3 (`hotkey.state_dump`, 3000 frames, about 35 s at 90 fps) and, within that window, deliberately launch so the pad rises and the doors open. Fly out about 1 km from the same station and press F4 once. In flight, inside and outside the station, press Pause a few times with `diag.camera_writes = on` (the jump guard's marks are on the TCWR lines; see the concourse doc's Built section). Outposts: the open-pad step is in the concourse doc.

**Risks.** The depth argument may be a cascade object rather than a view; the table then says `shadow=no` for every view, and the next step is the builder's disassembly beyond the first 0x40 bytes. MAIN (view 0) and the camera (`ctx+0x00`) are still unverified.

## 2026-10-11, v3: look probe and sealed-hangar trial (same branch; not installed, not flown)

Built on `claude/shadow-gate-probe` after v2; full build and rig green (`tools\look_probe_test` 29 checks, `tools\sealed_trial_test` 23 checks, `tools\location_scan_test` 17 checks). The mechanics are in the concourse doc's Built v3 section. This entry says what each probe is for in the station question.

- **The look probe (`hotkey.look_probe`)** answers the aiming question directly: which records lie on the camera's forward ray within 2 km, nearest first, and which draws cover the screen centre in that frame, with their shader hashes, counts, blend and depth state. Hypothesis: docked in the hangar, the hull's records are on the ray and its draws cover the centre. Refuted if no record of the hull's size is on the ray while the hull fills the view. The CSV is `look_probe_<stamp>.csv`; the summary is one `look probe:` line.
- **The sealed-hangar trial (`hotkey.sealed_trial`, `diag.sealed_trial_radius_m = 80`)** tests the sealed-hangar cull by hand. With the trial on, the sun pair is skipped and records outside the sphere lose their views. Hypothesis: the sun-pair draws that reach the hull come from records outside the sphere, so the trial removes the far contributors and the hull stays. Refuted if the hull leaves the main view, or if `sun_pairs_skipped` stays 0 while the sun pair is drawn.
- **Flight plan (v3, no engine change beyond the probes):** (1) docked, bright phase: `hotkey.look_probe` once, then read the record rows and the covering draws against the shadow gate CSV's candidates; (2) docked, dark phase: `hotkey.sealed_trial` on, watch the window lines for 20 s, then press it again to turn it off; (3) docked: `hotkey.shadow_gate_probe` (F4) with the trial on, to see whether the hull enters the sun map once the far records are gone; then the D0 v2 plan for F3 and the 1 km return.

The look probe's centre test shows the depth state before each draw, not what ends on screen. Read its covering draws as candidates; the later ones are the likelier visible ones.

## 2026-10-11, v4: the sealed trial's step-down census and the approach flight (branch `claude/hangar-test-5`; not installed, not flown)

Flight 7 (the sealed trial at 80 m, docked in the hangar) found nothing missing at 80 m, and the sun bleed was gone (concourse doc, Flight 7). The next question is the smallest radius at which the hangar still looks complete. The answer comes from a step-down: with the trial on, `hotkey.sealed_trial_smaller` moves the radius through 60, 40, 30, 20, 15 and 10 m, and F6 (the census, `hotkey.occlusion_census`) at each rung gives the draws, the triangles and the zero-sample share.

- **H-H (hull and hangar):** the hangar's own geometry stays complete down to some rung, because its records contain the camera or meet the sphere. The first element to go missing names the record that lies outside the sphere. Refuted by an element inside the hangar that disappears at a rung above which its record should still be kept.
- **H-S (sun bleed):** the sun pair is skipped at every rung, so the bleed stays gone. Refuted if the bleed comes back at a smaller rung: then a sun-pair draw is getting past the skip.
- **H-C (census):** draws and triangles fall at each smaller rung. Refuted if they do not fall; the rule is then not culling the far records, and the window's `culled` count says whether it runs.
- **The approach flight (separate):** set `diag.state_dump_frames = 6000` and press F3 (`hotkey.state_dump`) about 30 s before touchdown. The question is whether the pad and the doors are the `AAB8` object (P9): the `AAB8.key3C0=` text should change as the pad lowers. Refuted if it never changes while the pad moves. The docked value comes from the Location scanner (v3), which should follow the journal's Docked lines.
- **The concourse bay windows (separate):** stand in the concourse facing one bay window and press `hotkey.look_probe`. If the window's pane is among the records on the ray, and the covering draws are the pane's, the pane is a record the sphere rule can cull. That is the aim check for the closed-pane question (B).

The step table and the rung-by-rung results are in the concourse doc (Built v4). The look probe's centre test still shows the depth state before each draw, so its covering draws remain candidates, not the final picture.
