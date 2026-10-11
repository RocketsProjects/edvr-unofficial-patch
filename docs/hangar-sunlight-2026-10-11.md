# Hangar sunlight: where Elite applies its sun, and a fix

Offline analysis of `EliteDangerous64.exe` (build 332841, PE stamp 1788384820, image base 0x140000000). No C++ changed; no flight yet. **Proven** means read from the exe bytes, from the repo's captured shader bytecode, or from EDVR source. **Inferred** means the bytes do not close the point. RVAs are file RVAs; add 0x140000000 for the VA.

## Status

- **State:** static analysis done; design written; nothing implemented; no flight since the 2026-10-11 reproduction.
- **Bug:** docked in a station hangar, the walls and floor brighten and darken with the station's rotation; shadows of ships outside cross them.
- **Ruled out:** EDVR, by --suspend flat flight 2026-10-11.
- **H1, favourite:** the station hull is not admitted to the sun's shadow map. Ship shadows reach the hangar, so the map is populated; the hull records are not. Candidate mechanism: per-record view masks (P9). Open: which bit or plane excludes the hull.
- **H2:** a per-pixel bypass. The shadow resolve's flag byte says "no occlusion" on the walls (bit 2 clear, or bits 6-7 select a static term). Open: the flag byte on the walls.
- **H3:** the sun is not the source; ambient or IBL carries the star. Unlikely for the ship shadows (IBL casts none), not excluded for the brightness. Open: skip the directional draw and see whether the cycle stops.
- **Open leads:** the byte group at 0x5E889C0-C3 (P8), no writer found; whether Status.json Docked is set at surface ports (I3).
- **Fix order:** B (attenuate the directional pass constant while docked in a station), then A (light-object intensity), then C (admit the hull to the shadow views, the root-cause fix). Nothing implemented.
- **Next flight:** flat, Coriolis or Orbis hangar, two full cycles. Eye dump of the walls per phase (flag bits 2, 6, 7; shadow mask); draw census of the DirectionalShadows passes inside vs 1 km outside; bytes at 0x5E889C0-C3 at each phase and at the dock event; one skip of the directional draw; Status flags at each state. Decision table below.

## Proven

**P1. The sun's direct light is `RenderDirLights`, RVA 0x5AC9C0** (3492 B). Its marker "RenderDirectionalLights" is referenced at 0x5AD238. It binds these per-pass constants: `c_directionalLightColour` 0x50B9950, `c_directionalLightDirection` 0x50B9970, `c_directionalLightShadowCollectMask` 0x50B9990, `c_numDirectionalLights` 0x50B99B8, `c_directionalShadowActive` 0x50B99D0, `c_globalLightScale` 0x50B9A20, `c_shadowTransparancy` 0x50B9AC0, `c_directionalLightTransparency` 0x50B9AD8, plus `c_shadowCollectTexture`, `c_shadowTexture`, `c_albedoFlagsTexture`, `c_depthTexture`. No other function binds the five scalars by name (RIP-relative and pointer-table scans).

**P2. Its caller is the deferred lighting frame, RVA 0x284CB70** (8580 B; direct call at 0x284E159).

**P3. Per-frame globals**, bound by name in 0x57B99E, 0x57BECD and 0x57BF79 (the writers are not identified): `gHasDirectionalLights` 0x5039410, `gNumDirectionalLights` 0x5039428, `gDirectionalLightDir` 0x5039440, `gDirectionalLightColour` 0x5039458, `gDirectionalShadowMapTexture` 0x5039470, `gDirectionalShadow_NumCascades` 0x5039580, `gDirectionalShadow_ReferenceEndDistance` 0x50395C8, `g_directionalShadow_MSMMomentBias` 0x5039620.

**P4. The sun shadow map is made by two passes.** The `DirectionalShadows.Depth.%p` builder is at 0x5B2C80 (540 B; allocates a 0x58-byte object, submits through 0x6C2110). The `DirectionalShadows.MSM` pass is at 0x5B3530 (661 B; marker at 0x5B3644), reached from `ShadowMSMWrap` at 0x5B1356. `ShadowWrap` (0x5AE1E0) calls the depth builder and is itself called at 0x5A5A65, 0x5A5AEA and 0x5A8931. None of these filters casters by name; the filter must sit in the submit path (0x6C2110) or in the record admission (P9).

**P5. The per-pixel visibility** (read from the captured bytecode; written up in `docs/ambient-occlusion.md` lines 647-731). `ps_7EAC71963E66C5FE` (fixture `tools/flat_temporal_test/fixtures/ps_7EAC71963E66C5FE.dxbc`) loops over `cb2[35].x` cascades with a moment filter and returns 1.0 (lit) when no cascade holds the pixel. It reads a per-pixel flag byte: bit 2 forces full shadow; bits 6-7 divert to a precomputed screen-space term. The consumer `ps_7CECABDE34FFBE9E` multiplies diffuse and specular by that one scalar, the channel chosen by `dp3 ... cb2[44]`.

**P6. Light objects are filed by type in `StarLightMgr`, RVA 0x35EB720** (2578 B): `m_directionalLights` (lea at 0x35EBA12), `m_starLights` (0x35EBCF1), `m_dummyStarLights` (0x35EBD52), and six `m_location*Lights` lists. Its branch reads the byte at 0x5E889C3 (at 0x35EB74A).

**P7. Interior code exists; its tie to the sun is not shown.** `SpaceOrStationLoad` (0x1F4700, 11163 B) calls the code that names `InnerDockLightingComponent` (0x51A9928; function 0x12C9B70, call at 0x1F52E6) and the code that names `InternalSpacesManager` (0x5213268; function 0x1B0BBC0, call at 0x1F67E6). Function 0x12C8C54 (1365 B) holds `PlayerCanSeeInside`, `LandingPadExtraLighting`, `OutfittingLightsOn` and `HangarLiftDoors__`; it has no direct caller. A .data table at 0x5E78C40 holds the docking state names `Inside_Hangar`, `Outside_Hangar` and `StateTeleport_Inside_Hangar`.

**P8. A byte group at 0x5E889C0-C3 gates the light dispatch.** Reads only: 0x5E889C0 (at 0x35F8A46), 0x5E889C1 (at 0x35A80F9), 0x5E889C2 (at 0x360092A, in 0x36008B4), 0x5E889C3 (at 0x35A7C0E and 0x35EB74A). No store to these bytes is visible by a RIP-relative reference.

**P9. EDVR's view-mask reading holds in this build.** The draw-item builder is at RVA 0x42B4420: its prologue multiplies by the 0x6A0 view stride and compares a global byte at 0x5EA3398, adjacent to the DAT_145ea3399 that EDVR's notes name. `src/d3d11/cull_gate_probe.h` lines 19-33 give the admission: `view+0x570 & record mask`, with the record mask at rec+0x208. `src/d3d11/explorer_cam_core.h` lines 1487-1499 give the avatar's per-instance masks, rewritten every frame, and the part states 2 (shadow-ish views only) and 3 (main views only). The AMC dither fade at 0x3DD6040 also matches its call site. The station hull's masks are not measured.

**P10. Searched, not found:** a caster-flag string (CastShadow, NoShadow, ShadowCaster) in the exe's string tables. The star glare (`docs/sun-glare.md`) is a separate instanced draw and does not carry the sun.

## Inferred

- **I1.** The hull is excluded at record level (H1): its parts' masks lack the shadow views' bit, or the hull falls outside the shadow views' planes. Consistent with P4, P9 and the ship shadows. Unmeasured.
- **I2.** The byte group (P8) is game state, possibly the switch between `m_starLights` and `m_dummyStarLights` (P6). Meaning and writer unknown.
- **I3.** Status.json Flags bit 0 is Docked and bit 1 is Landed (the published layout). EDVR reads bit 4 (supercruise), bits 24-26 (seated) and Flags2 bits 0-2; it does not name bit 0.
- **I4.** The flag byte in P5 comes from `c_albedoFlagsTexture`.

## Dead ends

- ruled out: baked shadows (`c_enableBakedShadows`, flag bits 6-7) as the hangar's source, because ship shadows are dynamic and a baked term carries none.
- ruled out: the star glare as the sun's light, because it is a separate instanced draw with no shadow path.
- not searched yet: the submit path 0x6C2110 and the shadow views' planes, where the caster filter would sit.

## Fix design, ranked

### B. Attenuate the directional pass while docked in a station (first)
- **What:** while the gate is true, scale the sun's constant in the `RenderDirLights` buffer, either `c_directionalLightColour` or `c_globalLightScale` (bound only by that pass), down to a floor (default 0). The colour should feed both diffuse and specular, so the hangar loses the sun as a whole (inferred from P5).
- **Where:** a write-time patch of the buffer holding those constants, at the Map or UpdateSubresource before the directional draw. The slot is open; the first flight finds it. `census_cb_watch` already reads constant buffers.
- **Gate:** the inside signal below.
- **Cost:** one compare per frame and a 12-byte store per directional draw (one or two a frame).
- **Risks:** (a) a surface port that sets Docked would dim sunlit surfaces, so the gate must exclude Landed; (b) the open hangar mouth loses sun on exterior faces seen through it; (c) the flag arrives after the render state changes, so ramp over about 0.5 s to avoid a pop; (d) the slot is unknown until captured.
- **VR and flat:** the same pass in both; test each.

### A. Attenuate the sun at the light object (second)
- **What:** on the CPU side, scale the directional light's colour in the object `StarLightMgr` files (P6), before the buffer is filled. This also reaches any other reader of that object.
- **Needs:** the light object's layout (not found; needs a runtime read or a struct walk from `m_directionalLights`).
- **Risk:** writes game memory; a wrong offset dims something else. Use only after B proves the gate.
- **Variant:** force the dispatcher onto `m_dummyStarLights` through the byte group (P8), once its meaning is known. Not before.

### C. Admit the hull to the sun's shadow views (root cause, third)
- **What:** if H1 holds, OR the shadow view's bit into the station hull records' masks, or pass the admission for the shadow view only (P9). The walls then sit in the hull's shadow, which is the physical result.
- **Needs:** the shadow view's bit, the hull records' masks (census), and an identity for the hull's records.
- **Cost:** the shadow pass gains the hull's draws (count unknown until the census). The admission hook is one pointer compare per builder call; the builder's call count per frame is unmeasured (the celestial tees, on a similar path, see about 1,100 a frame, per `src/d3d11/celestial_motion.h`).
- **Risks:** self-shadow acne at creases; bias on the walls; per-object stencil tags (`docs/per-object-motion.md`) may need care.
- **If H2 instead:** the fix is per pixel (set bit 2 on hangar materials). Heavier, in the material pass.

## Inside signal (for A and B)

From Status.json, already read by `src/d3d11/journal_watch.h`:
- `docked = Flags & 0x1` (Docked; I3, verify in flight)
- `landed = Flags & 0x2` (must be 0: outdoor pads are sunlit)
- `supercruise = Flags & 0x10` (already gated in `celestial_motion.h`)
- `inShip = Flags & (1 << 24)`, `onFoot = Flags2 & 0x1`
- **Gate:** `docked && !landed && !supercruise && (inShip || onFoot)`. Unknown counts as off, as the celestial gate does.
- **Open:** whether Docked is set at surface ports and carriers. The first flight logs it.
- **Not usable yet:** the docking state names (P7); reading the bay state needs a runtime address.

## Environment

The pass structure is engine-internal, so the VR runtime (OpenXR, OpenVR, Oculus) should not change it. The flight must still name the runtime, headset, per-eye size and DLSS version, because the view count and shadow views depend on them. This analysis covers build 332841 only.

## Gates

- Build: `build.bat` by absolute path after any C++ change (none made here).
- Flight 1 (no fix): the decision table below.
- Flight 2 (B, flat): the walls' luminance falls to the floor in the dark phase; a sunlit exterior at a surface port and in open space is unchanged in the same frame; undocking brings the sun back inside the ramp.
- Flight 3 (B, VR): the same, in a headset.

## First flight (flat, Coriolis or Orbis)

1. Log Flags and Flags2 at: space, approach, docked, on foot in the hangar, and a surface port (I3, and the port case).
2. Two full cycles in the hangar, timed; note the bright and dark phases.
3. Each phase: eye dump of the walls' flag bits 2, 6, 7; the shadow-mask value; linear depth.
4. Draw census of the DirectionalShadows passes (the small D16 targets, `docs/anti-aliasing.md` line 932), inside the hangar and at 1 km outside the same station. Look for the hull's draws, which should appear in the main view from outside.
5. Read RVA 0x5E889C0-C3 (module base + RVA) at each phase and at the dock event. A change at the dock event names a zone switch (I2).
6. One skip of the `RenderDirLights` draw (`census_skip`, identified by its marker) in the bright phase.

| Observed | Reading |
|---|---|
| Hull draws in the shadow census outside, none inside; masks or planes differ | H1 confirmed: C via the record mask; B first |
| Hull draws present inside; shadow mask 1.0 on the walls in both phases | H1 wrong: H2, a per-pixel bypass |
| Walls' bit 2 clear, bits 6-7 zero, mask 1.0 | H2: no per-pixel interior mask |
| Skipping the directional draw stops the cycle | The sun is the source (H3 out) |
| Skipping it leaves the cycle | H3: ambient or IBL; B does not apply |
| Byte at C3 changes at dock | I2: a zone switch; check whether it selects the dummy star list |

## Method

The scripts (PE walk with `.pdata`, RIP-relative and call scans, capstone dumps) are in the session scratchpad and are not committed. The RVAs reproduce with them.
