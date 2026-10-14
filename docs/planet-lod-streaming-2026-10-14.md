# Planet tile detail and generation budget: what the game does, what EDVR could change (2026-10-14)

Read-only static analysis of Elite build 332841 (PE stamp 1788384820, image size 104894464; the Epic exe is byte-identical to
`analysis\EliteDangerous64.exe`, sha256 prefix e6be8bbe). Method: capstone on the exe, plus Ghidra 12.1.3 headless against the existing
`analysis\ghidra\EDAnalysis` project, opened `-readOnly` (nothing saved). Tags: **CONFIRMED** = read in disassembly or decompile;
**INFERRED** = follows from what was read but no instruction states it; **UNKNOWN** = looked for, not found.

## Status

- **State (2026-10-14): FLIGHT 1 DONE; TRACE BUILT, NOT FLOWN (branch `claude/planet-lod-trace`).** Goal (Sean): close orbits of non-landable
  planets show ribbed, streaked low-detail surface and cloud tiles near the limb that pop to smooth. Stock behaviour.
- **Flight 1 (Sean, Terrain Work 3.0 in the fxcfg):** the ribbed seams at the limb still show and tiles still take seconds to pop in clean.
  So H1 (the budget binds) is weak: the budget at 3.0 is 60 % of the frame period, and it changed nothing visible. Consistent with the work
  table's prior on the 5090. The cause is somewhere the budget does not reach: head-of-line stall in the pump, late requests, CPU-side
  generation, or the ribbing is not streaming at all (H4). The trace below says which, in one flight.
- **Terrain generation is a GPU-time-budgeted job system, FIFO inside a task priority.** CONFIRMED. Per-frame budget in ms
  = `T * (0.24 + 0.12 * GpuSchedulerMultiplier)`, `T = 1000 / HMD refresh Hz` in VR (1000/60 flat). RVA 0x4C97460 (constants 0x5EE7BB0
  and 0x5EE7BB8), pump 0x4C97730. T is the NOMINAL period from the runtime's display frequency, not a measured frame time, so
  late frames cut generation per second in proportion. The budget is 24 to 36 % of wall time at any refresh rate.
- **`GpuSchedulerMultiplier` is the float in the `.fxcfg`; the loader's clamp is [1.2e-7, FLT_MAX] (CONFIRMED, 0x27F8620 defaults
  + 0x2855BC4..0x2855BDF clamp).** Nothing in the loader stops 3.0. Sean's files: Custom.4.1/4.3 0.9925, Custom.4.4 (today) 0.36075,
  VRUltra preset 0.0. That it is the in-game "Terrain Work" slider is INFERRED (no UI string in the exe; the value is slider-shaped).
- **Tile LOD is distance only.** CONFIRMED (0x444D6C0, decompile): per quadtree node, min and max distance from the camera to the
  node's camera-relative box against a per-level threshold table (stride 0x30, float at +0), plus a per-node bias byte. No FOV and no
  resolution in the split decision. FOV and viewport height enter ONLY the planet-level on/off gate (0x444B4A0: projected size >= 2 px).
  Camera = the render view record's pose (camera 0), mono. So option (c), feeding LOD the true eye frustum, has nothing to feed.
- **0x44597B0 is a box transform (a helper of the LOD band), not a selector and not a frustum.** CONFIRMED.
- **The threshold table is data, not code.** `<Terrain>` entries pick `LODTable` by name (Low, Medium, Default, Ultra, UltraPlus); the
  numbers live in packed game data. UNKNOWN. Sean ran TerrainQuality 4 (UltraPlus) in Custom.4.1/4.3; today's file has 2 (Default).
- **Request ordering: priority integer (task vtable +0x48), FIFO within it; no distance sort found.** CONFIRMED for the pump, UNKNOWN
  for what number a tile request carries (0x4467100 seeds 2000, then a virtual call overwrites it).
- **On this rig the work table says the budget is rarely scarce.** `GpuWorkTable.xml` (written by the game today, one card: RTX 5090)
  prices a MiddleStage batch of 17216 work at 0.20 ms against a 3.15 ms budget (90 Hz, Terrain Work 0.36). So "budget-limited" is
  a weak prior HERE and a strong one on slower GPUs. The flight must tell it from latency-limited.
- **Open hypotheses (signature in the flight below):** H1 budget-limited; H2 threshold-limited (pop range fixed); H3 latency-limited
  (compute ms per frame well under budget while pops occur; CPU AstroData threads or multi-stage readbacks); H4 the ribbing is shading
  of a stable coarse tile (sampler and mip bias), not streaming; H5 planet texture pool (`Planets` WorkPerFrame, TexturePoolBudget).
- **Ruled out by reading:** FOV as a tile-LOD input (decompile, above). **Not ruled out, not read this session:** the horizon-cone
  cull (0x44D0200, 48 planes from 0x4497D30) that decides which limb tiles are requested at all. The view-frustum cull cover
  (0x443E800, `docs\terrain-culling.md`) is a different consumer from the LOD tree.
- **Not found (so do not build on them):** the writer of the GpuTaskSystem boost byte ([front+0x20], selects a flat 30 ms budget);
  the consumer of `Planets` WorkPerFrame / TexturePoolBudget; the consumers of AppConfig `AstroData*`; the cloud tile path.
- **Next:** flight 2 (section "Flight 2"): the same orbit at 3.0 with the trace, one 5 s summary read at a seam, then AstroDataThreads 8,
  then `texture_anisotropy = 16`. Rank 1 below (a budget wrap) is demoted until the trace shows a saturated, undrained queue.
- **Pump head-of-line (new, CONFIRMED in code):** 0x4C97730 stops at the FIRST task that gets a zero grant once something ran, so one task
  with nothing available to run (waiting on a CPU-side input or a readback) starves every task behind it while the budget goes unspent.
  The trace sees it as `considered - run > 0` per frame with a backlog and `used_est_ms` far under `budget_ms`.
- **Temporary instrument, no key:** `src\d3d11\planet_lod_trace_*` (five read-only CodeHooks, both profiles, build 332841 only), rig
  `tools\planet_lod_trace_test` (63 checks, in the build). Delete both with this arc; nothing else depends on them.

## RVA table (build 332841)

| RVA | What | Tag |
|---|---|---|
| 0x2855A50, 0x2893120 | `.fxcfg` load and save of five float options (LODDistanceScale +0x124, HMDRenderTargetMultiplier +0x130, SSAAMultiplier +0x13C, **GpuSchedulerMultiplier +0x148**, FFXCASIntensity +0x154; min/max at +4/+8 past each). Names at 0x52EB998..0x52EB9F8 | CONFIRMED |
| 0x2842D10, 0x2842DA0 | option-name tables: 9 int options (0 DirectionalShadowQuality .. 6 SurfaceSamplerQuality), 15 uint (5 EnvironmentQuality, 10 TerrainQuality, 11 TerrainLodBlendingQuality, 12 SurfaceMaterialQuality) | CONFIRMED |
| 0x27F8620 | default option struct: GpuSchedulerMultiplier cur 0, min 1.19e-7, max FLT_MAX; HMD multiplier 0.5..2.0 | CONFIRMED |
| 0x2841CA0 | **T**: VR `1000.0 / float(display frequency)` (0x2841CC5 call, 0x2841CD7 divide, no zero guard); else 1000/60 | CONFIRMED (also `docs\steam-missing-terrain-exit-2026-09-14.md`) |
| 0x12904B0 | terrain system tick: reads options+0x148 at **0x129055A**, T via the Obj2 getter, calls the budget function at **0x1290591** | CONFIRMED |
| 0x4C97460 | **budget**: args rcx this, rdx ctx, r8b flag, r9 &{double,u32}, stack double T at [rsp+0x28] and float multiplier at [rsp+0x30] at entry. Constants 0x5EE7BB8 (0.36), 0x5EE7BB0 (0.24), 0x5EE7BC0 (30.0), 0x5EE7BC8 (1.0); statics 0x6051F31/32/33 are read-only zero bytes. Head `48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 54 41 55 41 56 41 57` | CONFIRMED |
| 0x4C97730 | **pump**: two ring queues, priority (vtable +0x48) then FIFO; per task asks the table how much work fits the remaining ms, runs it, subtracts the table's cost; stops on budget, a zero grant, or wall-clock past budget | CONFIRMED |
| 0x4C95BF0, 0x4C956C0, 0x4C95A60 | work-from-time lookup, minimum-grant fallback (a first task always gets one granule), per-task table fetch | CONFIRMED |
| 0x4C927F0 | GpuTaskSystem front ctor; boost byte [this+0x20] set 0 at 0x4C92823; writer UNKNOWN | CONFIRMED / UNKNOWN |
| 0x4C92914..0x4C92D40 | loads `GpuWorkTable.xml` (string 0x5616DB8) and `SharedGpuWorkTable.xml` (0x5A70728) | CONFIRMED |
| 0x7F59D0..0x7F5AD3 | AppConfig `<StellarForge AstroDataTime AstroDataThreads AstroDataCache>`; clamps 5..30, 1..8, 1..1024; defaults 10, 3, 256 (0x7E6974); consumers UNKNOWN | CONFIRMED parse |
| 0x7F5EEE..0x7F5FCB | AppConfig `<TerrainTiming TimerMode=Default/Gpu/Cpu>` into [cfg+0x304] (Gpu = 0x0001, Cpu = 0x0100); passed to the generator as 0/1/2 (0x128C5E0, 0x4C92600) | CONFIRMED |
| 0x284BA00 | GraphicsConfiguration.xml loader; Planets vector at this+0x2C8 via **0x289C400** (entry stride 0x18: +8 TextureSize, +0xC CloudsEnabled, +0x10 WorkPerFrame, +0x14 TexturePoolBudget) | CONFIRMED parse |
| 0x289F510 | `<Terrain>` entry parser: +0 LODTable id, +2 QualityID, +4 PatchLighting, +8 BlendTargetsResolution, +0xC WindVectorFieldResolution, +0x10 FineMipsToDrop, +0x11 LODsToDrop, SysMapDownRes | CONFIRMED parse |
| 0x28635D0 | four `TerrainSamplerQuality` samplers, quality 0..3 = filter 7 / 8 with 0 / 2 / 4 / 8 (anisotropy) | CONFIRMED build; reading of the pair as filter and anisotropy INFERRED |
| 0x12772B0, 0x1277370 | per-frame update of the AstroSurface manager from the view record: fov degrees [view+0x240] (from `[game fov setting+0x2C]`), two floats +0x244/+0x248, flag +0x24D | CONFIRMED |
| 0x3D0EC20, 0x448E0B0 | push fov to each surface; stores tan(fov/2) at surface+0x8E0 | CONFIRMED |
| 0x3D097B0 | per surface, calls 0x444B4A0 with the view's camera block (position doubles at +0x60/+0x68/+0x70) | CONFIRMED |
| 0x444B4A0 | **AstroSurface update**: altitude = distance - radius at +0x8C0 (radius +0x880); gate: `[+0x908] * R/(horizon * tan(fov/2)) > 4.0 * 0.5` (constants 0x5EA39F8, 0x4DE4E90) else the tile tree is idle; builds cone, calls 0x4462AE0, 0x444D170, 0x4454210 | CONFIRMED |
| 0x4462AE0 -> 0x443EDD0 | creates the top-level tiles per cube face (6 x N x N, bitmask at +0xB00), 0x390-byte objects | CONFIRMED |
| 0x444D170 | tree driver: per face, calls 0x444D6C0 with the context and the 1/N cell size | CONFIRMED |
| **0x444D6C0** | **LOD split decision**, recursive over 4 children (stride 0x340): box via 0x44597B0, min and max distance, walk levels against table [ctx->+0x10]+level*0x30, + per-node bias [node+0x2B8]; flags in [node+0x2B4]; calls 0x4467100 to request | CONFIRMED |
| 0x44597B0 | box transform to camera-relative corners (fills 8 corners and min/max extents) | CONFIRMED |
| 0x4457260 | fills the threshold table from the planet description: copies floats from [surface+0x11F0] (count +0x11E8) into the 0x30-stride table at surface+0x11B0, last entry times 0x4E2F890 | CONFIRMED |
| 0x4467100 | request submit: request struct (priority seed 2000 at the end, then a virtual +0x38), list insert at ctx[8]+0x20; calls generator through 0x44C6A90 (vtable +0x40) | CONFIRMED |
| 0x44D0200, 0x4497D30 | horizon-cone test, 48-plane table built once from planet geometry | CONFIRMED (earlier docs) |
| 0x5EA3980..0x5EA3A40 | engine tunables of the tile tree (data, writable): 0x5EA398B=1, 0x5EA398C u32 6, 0x5EA3994 u32 7, 0x5EA399C u32 1, 0x5EA39A0 u32 10, 0x5EA39A8 double 1.1, 0x5EA39F4 float 5.0, 0x5EA39F8 double 4.0 | CONFIRMED values; meanings of all but 0x5EA39F8 INFERRED |

## 1. The settings and their consumers

`Settings.xml` carries none of them. The names are in the preset `Options\Graphics\Custom.4.4.fxcfg` (and `OptionDefaults\*.fxcfg`).

- **Terrain Work** = `GpuSchedulerMultiplier` (INFERRED label, CONFIRMED consumer). Float. Budget fraction f = 0.24 + 0.12 * value:
  0 -> 0.240, 0.36075 -> 0.283, 0.9925 -> 0.359, 1 -> 0.360, 3 -> 0.600, 5 -> 0.840 of the frame period. At 90 Hz (T 11.11 ms):
  2.67, 3.15, 3.99, 4.00, 6.67, 9.33 ms. At 120 Hz (8.33 ms): 2.00, 2.36, 2.99, 3.00, 5.00, 7.00 ms. It is a cap on the table's ESTIMATED
  GPU ms for generation work, spent only while tasks are queued; an empty queue costs nothing.
- **Terrain Quality** = `TerrainQuality` (0..4: Low, Medium, High, Ultra, UltraPlus; 5 entries in `<Terrain>`, so 4 is legal and Sean has used it).
  Picks `LODTable`, `LODsToDrop` (Low 1, others 0), `FineMipsToDrop` (Low, Medium 1), `BlendTargetsResolution` (256, 512, 512, 1024,
  1024), `WindVectorFieldResolution`, UltraPlus adds `CoarseColourMipsToDrop`. Consumers of the numbers: UNKNOWN beyond the parser.
- **Terrain Material Quality** = `SurfaceMaterialQuality` (1, 3, 5, 7 levels in `<SurfaceMaterial>`); shader constant
  `gSurfaceMaterialQualityReduced` (0x27F4190 binds it). Shading cost, not streaming.
- **Terrain Sampler Quality** = `SurfaceSamplerQuality` (INFERRED label). Builds four `TerrainSamplerQuality` samplers (0x28635D0); the top is 8x.
- **Terrain LOD Blending** = `TerrainLodBlendingQuality` -> `PatchMorphing` 0 / 1 / 2.
- **Environment Quality** = `EnvironmentQuality` 0..3 -> `<Environment>` item `Planets` QualitySetting 0..3 -> `Planets[n]`:
  TextureSize 512 / 1024 / 2048 / 2560, AtmosphereSteps 4 / 5 / 6 / 6, **WorkPerFrame 96 / 132 / 264 / 328**, **TexturePoolBudget 12 / 32 / 90 / 100**.
  The consumer of WorkPerFrame and TexturePoolBudget is UNKNOWN. Whether this is the distant-planet texture path that non-landable
  bodies use is INFERRED from the names alone. Sean: 3 today, 0 in Custom.4.3.
- `TerrainCheckerboardRenderingEnabled`: false in Sean's Settings.xml and fxcfg; true in `VRUltra.fxcfg`. A cheap thing to check on any report
  of ribbing (the distant-hills arc, `docs\terrain-history-shimmer-2026-09-17.md`, found it to be the game's checkerboard).

## 2. LOD selection

Chain, CONFIRMED: 0x12772B0 / 0x1277370 (per frame, from the view record) -> 0x3D097B0 -> 0x444B4A0 -> 0x4462AE0 (top tiles) and 0x444D170 -> 0x444D6C0.
Inside 0x444D6C0: the camera is a position (double triple at the view block +0x60), the tile a box; the decision reads two
distances (nearest and farthest box point), walks levels from the finest down comparing the nearest distance with `table[level].f0`,
adds `[node+0x2B8]`, clamps to the tree depth, sets "needs finer" (0x400) and "keep" (0x200) bits, and recurses into four children only
while the node is wanted and above the context's level limit. FOV, aspect and eye are not read. The tile system as a whole is gated by the
planet's projected size using a fov in degrees read at 0x1277370 (`[obj+0x2C]`; `docs\terrain-culling.md` reads it as the game FOV setting,
`Settings.xml` `<FOV>` 55.15 today), not the eye's field. In VR that gate
still passes for any planet one can see; it is not a lever for limb detail.

Consequence: a tile at the limb is chosen by its distance (hundreds of km at 10 to 20 km altitude), so the level it gets there is whatever
the LODTable gives at that distance. Whether the pop is "the table's coarse level, then finer as it nears" (H2) or "the right level, late"
(H1, H3) is exactly what the numbers cannot say offline.

## 3. Queue and per-frame count

Requests: the tree issues one per wanted node (0x4467100) in traversal order (face, then cell, then child), no distance sort found. Tasks
enter two rings in the GpuTaskSystem. The pump (0x4C97730) takes the head of the active ring unless the pending head has a strictly higher
vtable +0x48 value, asks 0x4C95BF0 how many work units of that task fit the remaining ms (the XML table is interpolated and extrapolated
linearly beyond its last row; granularity is the `size` attribute, 64 or 256 or 1), runs them, subtracts the table's cost, and repeats.
It ends on: remaining ms <= 0, a zero grant (then, if nothing ran this frame, 0x4C956C0 grants one granule), or wall clock past the budget.
So the count per frame is ms-bound, not count-bound. The `Planets.WorkPerFrame` number is a SEPARATE, count-style budget whose
consumer I did not find.

Rough capacity on Sean's card (table rows for the RTX 5090, ms per batch of about 17k work): CoarseStage 0.037, MiddleStage 0.17 to 0.20,
Materials 0.042, Materials_ScatterMasks 0.047; PatchLighting_ShadowAOGeneration 0.2 to 0.31 per 15k. If a batch is a few patches, one
frame at 3 ms holds tens of patches. INFERRED; the unit of "work" is not decoded.

Latency, not only throughput: tasks include `BoundingBoxCalculation`, `Terrain_Combined_Planet_MaterialsEx_Physics` and BC3/4/5 compression, so
a tile passes several stages (and probably a GPU-to-CPU hop for bounds and physics). Stage latency of a few frames each would look like a pop
a second late without ever touching the budget (H3). INFERRED.

## 4. What the ribbing is

INFERRED, not shown: a coarse tile covers four times the area of its children at the same texture size (`BlendTargetsResolution`), so at the
limb it is magnified along the tangent and minified radially; the sampler is anisotropic at most 8x on `SurfaceSamplerQuality` 3 (0x28635D0),
and `FineMipsToDrop` or `LODsToDrop` drop levels on Low and Medium. EDVR's `advanced.texture_anisotropy` and `advanced.texture_lod_bias` (hook
on `CreateSamplerState`, `src\d3d11\device_hook.cpp` ~2430..2490) DO reach these samplers (they are created through the device;
linear and anisotropic filters only), so 16x is available today. They cannot add detail the tile never had, and they do not touch a shader
that picks its mip by hand. With temporal AA on, `texture_lod_bias=auto` is NEGATIVE (log2 of the render fraction, -0.4 to -1.0): sharper mips
on a grazing coarse texture are a candidate for moire or ribbing. That is a one-line A/B (set it to 0).

## Options (ranked; cost is GPU or CPU per frame)

| # | Option | Where | Mechanism and effect | Cost | Risk | Kind |
|---|---|---|---|---|---|---|
| 0 | No code: raise Terrain Work, set TerrainQuality 3 or 4, in the fxcfg | `Custom.4.4.fxcfg` | GpuSchedulerMultiplier above 1 is legal (clamp to FLT_MAX); TerrainQuality 4 = UltraPlus LODTable | up to +1.3 ms per 1.0 at 90 Hz, only while queued | the in-game options screen rewrites the file; a game update may clamp | stock lever; do first |
| 1 | **Backlog-aware budget** | CodeHook **0x4C97460**, call site 0x1290591; change stack double T ([rsp+0x28]) or float multiplier ([rsp+0x30]) before forwarding. Same stack-arg pattern as `terrain_cull_cover_*` (0x443E800) | While a close orbit is detected, force the multiplier to K (say 3.0, f 0.60) or hold it at the highest of recent values; optionally only while the queue is non-empty (the two counts the pump itself tests first: [pump this+0x40] and the active ring's +0x20) | 0 when idle; up to +3.5 ms GPU per frame at 90 Hz while queued; one hook call per frame | VR frame misses if the rest of the frame is near 11 ms; mitigate with a cap and a headroom gate from EDVR's own frame timing | **root fix IF H1** |
| 1b | Same, by constants | write 0x5EE7BB8 (0.36 -> 0.5) and 0x5EE7BB0; only 0x4C97460 reads them | f = 0.24 + 0.26 * TW, always | as 1, no gate | needs VirtualProtect on .rdata; no close-orbit gating | root if H1; blunt |
| 1c | Close-orbit gate | CodeHook 0x444B4A0 (head `4C 8B DC 55 53 56 41 54 41 55 49 8D AB 48 FF FF FF`), read-only: after the original, altitude [rcx+0x8C0] and radius [rcx+0x880] (doubles) | sets a per-frame flag "altitude < 0.3 * radius" that option 1 reads | negligible | low (read only) | enabler for 1 |
| 2 | Threshold scale | CodeHook 0x4457260 (head `48 8B C4 4C 89 48 20 4C 89 40 18 48 83 EC 78`): scale the float at +0 of each 8-byte source row (param 2) by s before the copy | tiles refine farther out; pops move to larger range. Tile count at each level grows ~ s^2 | steady generation load ~ s^2 (1.25 -> 1.56x) | scaled table must stay monotonic; combine with option 1 | root if H2; the stock equivalent is TerrainQuality 4 |
| 3 | Predicted camera for the LOD only | CodeHook 0x444B4A0 arg 3 (camera block, position doubles at +0x60..+0x70; args 4 and 5 are the blocks at view+0x120 and +0x1A0): pass a copy advanced by v * tau | tiles ahead refine tau seconds early, tiles behind coarsen early; GPU cost about neutral | negligible CPU; generation shifts | medium-high: the same position feeds the horizon cone (0x44D0200), so the trailing limb can cull early; the copy must carry every field read | prefetch (d); only if H3 |
| 4 | Request priority toward travel and view | 0x4467100 / task priority (vtable +0x48) | reorder so tiles near the velocity vector go first | none | HIGH: the priority number's meaning is UNKNOWN; do not build before it is read | (b); low confidence |
| 5 | Fallback sampling for coarse tiles | draw-level, via the draw census; cb2 LOD morph constants name coarse patches | per-draw anisotropy or bias | small | medium; draw identity work | compensation; cosmetic only |
| - | True VR frustum into LOD | none | LOD is distance only (section 2) | - | - | (c) is moot; the frustum cull is already covered |

**Root versus compensation.** Roots: options 1 and 2, whichever the flight picks (1 for a budget-bound system, 2 for a threshold-bound one).
Compensation: 5, and anything that sharpens or biases what a coarse tile shows. Option 3 and 4 are prefetch and ordering: they change WHEN
detail arrives, not how much budget there is, and only help if the system is latency- or order-bound.

## The flight

One session, two launches, one scripted pass: a fixed orbit at 15 to 25 km over the same non-landable body with a surface mesh, same
speed, three laps per leg, headset recording or fpsVR running, EDVR log read with `python tools\edvr_log.py --target steam
--expect-build HEAD`. Per leg also press `hotkey.gpu_pass_profile` (60 frames) once mid-lap and keep `top_cs_dispatches` and the compute
rows (compute ms per frame is the budget meter).

| Leg | Change (live unless noted) | H1 budget | H2 threshold | H3 latency | H4 sampler | H5 pool |
|---|---|---|---|---|---|---|
| L0 | baseline (Terrain Work 0.36, Terrain Quality 2, `texture_lod_bias=auto`) | compute ms near 3.15 during pops | - | compute ms far under 3.15 during pops | - | - |
| L1 | Terrain Work 1.0 | pops later and fewer; compute ms to ~4.0 | no change | no change | no change | no change |
| L2 | Terrain Work 0 | pops earlier; compute ms cap 2.67 | no change | no change | no change | no change |
| L3 | Terrain Quality 4 (UltraPlus) | maybe | **pops move to larger range** | no change | no change | no change |
| L4 | game FOV 55 -> 90 | no change | no change | no change | no change | no change (tile LOD ignores FOV; a change here REFUTES section 2) |
| L5 | Environment Quality 0 vs 3 | - | - | - | - | **pop/ribbing change** |
| L6 | new launch: `advanced.texture_lod_bias = 0`, `advanced.texture_anisotropy = 16` | no change | no change | no change | **ribbing amplitude changes, pop timing not** | no change |

Reading: pop timing and compute ms track Terrain Work -> H1, build option 1 with gate 1c. Pop range fixed across L1/L2 but moves with L3 ->
H2, option 2 (or just run TerrainQuality 4). Compute ms stay far under budget while pops occur and nothing in L1 to L3 moves them -> H3; try
`<StellarForge AstroDataThreads="8" AstroDataCache="1024" AstroDataTime="30"/>` in AppConfig.xml (parsed at 0x7F59D0; effect UNKNOWN) before
any hook, and instrument the queue at 0x4C97730 (ring counts [lVar16+0x20], [this+0x40]) in a diagnostic build. Ribbing changes only in L6
-> H4: ship the existing keys' advice, no engine change. Record the dated result here as a one-line `ruled out: X, because Y` per leg.

## Flight 2: the trace (built 2026-10-14)

Five read-only CodeHooks, installed once at startup, never changing an argument, a result or a byte of the game's memory; all five or none
(`planet lod trace: inert: <why>; nothing was patched`). Pinned by PE stamp and image size, the first 24 bytes of each function and the one
E8 that calls it. Sites: budget 0x4C97460, pump 0x4C97730, grant 0x4C95BF0 (once per task the pump considers), tile-node update
0x444D6C0, tile request 0x4467100. Cost: when both pump rings are empty the pump detour is two loads and a forward; the node and request
detours run per tile node per frame near a planet (a few relaxed adds each); the thread census (about 0.1 ms) runs once per 5 s window and
only when work was queued.

**Lines (graphics log, grep `planet lod trace:`).** On install: `hooks live: budget exe+0x4C97460 pump exe+0x4C97730 grant exe+0x4C95BF0
node exe+0x444D6C0 request exe+0x4467100 (...)`. Every 5 s while work was queued, four lines:

1. `planet lod trace: 5.0 s: pumps=N idle=M budget_ms avg=A max=B used_est_ms avg=C max=D bound=P% zero_budget=Z tasks considered=X run=Y (y/frame)
   queue_after pending avg=a max=b active avg=c max=d backlog=P% streak_max_ms=S oldest_age_ms=O prio=K of [value:count,...]`
   A frame is a pump call with something queued at entry; `idle` counts pump calls with both rings empty. `budget_ms` is what the pump was
   granted, `used_est_ms` the work table's ESTIMATED ms it spent (not a GPU timer), `bound` the share of frames that spent at least 90 % of
   the budget. `considered - run` is zero-grant exits (head-of-line). `streak_max_ms` is the longest time the queue stayed non-empty after a
   pump; `oldest_age_ms` is the longest a sampled task pointer was seen continuously queued (a floor, sampled every 250 ms, up to 64 per
   ring; pointers can be reused). `prio` is the latest sample's task priorities (vtable +0x48) as value:count.
2. `planet lod trace: budget call: n=N T_ms=a..b multiplier=c..d boost_seen=K est_ms_per_run=e work_per_run=f types=[0xID/gran:runs:work:ms ...]`
   T and the multiplier as the game passed them (confirms `T*(0.24+0.12*multiplier)` and the 3.0 actually in force), the boost byte, and the
   top five task types by estimated ms (id = interned name id; gran is the work table's `size`: 64 Combined_Planet, 256 TransformToSAPatchSpace /
   PatchLighting / Atmospheres, 153 Skirts, 1 Assembly and BC).
3. `planet lod trace: tree: nodes=N (n/frame) wanting_finer=W children=C requests=Q new_handles=H by_level [L:none/pending/ready ...] new_by_level [L:n ...]`
   (only when a node was visited). `wanting_finer` is the nodes the LOD pass wants split this window (flag 0x400, so a count of wanted splits,
   not of splits done); merges are not counted (the update marks children, it does not free them). `pending` is state 1, `ready` state 2 (the
   state names are INFERRED). `new_handles` is requests that created a request handle (node+0x30 went from zero), by level.
4. `planet lod trace: threads (N in process; busy = CPU/wall, one core = 100%): <module>+0x<start> xN sum=P% max=P% | ...` top six groups of
   threads by start address. The first busy window after an idle stretch has no percentages. Not identified: which group is the AstroData
   pool (AppConfig `AstroDataThreads`); the step below finds it by the count that moves.

Idle: no line for a window with nothing queued; at most one `planet lod trace: idle: ... hooks ever run: budget=N pump=N grant=N node=N
request=N` a minute. The lifetime counts make a zero a measurement: if a hook never ran its count is 0 and the other sites' counts say
whether the game reached it.

**How to read it.**
- `bound` near 100 % and `backlog` near 100 % with `considered - run` near 0: budget-bound. Flight 1 says this is not the case; the multiplier
  in line 2 should read 3.0 and `budget_ms` near 6.7 at 90 Hz.
- `backlog` high, `used_est_ms` far under `budget_ms`, `considered - run` of 1 or more a frame: head-of-line stall. The fix is not budget; it is
  the dependency the head task waits for. The `types` list names only the types that did run; ask for a build that logs the head task's
  type if this shows.
- `backlog` near 0 but `by_level` shows nodes `pending` (state 1) that do not clear: the requests exist and the tasks are not in the pump's
  rings, so the delay is upstream of the pump: CPU-side generation or a readback. Look at line 4.
- `backlog` near 0, `pending` zero, `wanting_finer` and `new_handles` small while the seams show: the requests are not coming, or come late:
  the distance table (0x4457260, TerrainQuality) or prediction is the fix.
- Seams visible with all of the above quiet, and unchanged by any of it: shading of a stable coarse tile (H4).

**The steps (the same close orbit, same speed, a seam in view; three laps each).**
1. Terrain Work 3.0 as flown; read one 5 s summary while a seam is in view (all four lines, and note the clock time of the pop you watched).
2. Close the game. In `AppConfig.xml` (game directory; keep a copy first) add, as a direct child of `<AppConfig>`:
   `<StellarForge AstroDataThreads="8" AstroDataCache="1024" AstroDataTime="30"/>`. The clamps are read at 0x7F59D0..0x7F5AD3: threads 1 to 8,
   cache 1 to 1024, time 5 to 30; defaults 3, 256, 10 (0x7E6974); 8 is accepted and is the ceiling. Where the element sits and what the
   three values drive are INFERRED (parse order and clamps read, consumers not). Fly the orbit again. Line 4 should show five more threads
   in one start-address group (that is the pool); if a pop is earlier and line 3's `pending` clears faster, the CPU side was the delay.
3. Close the game, restore `AppConfig.xml`, set `advanced.texture_anisotropy = 16` (and, for the A/B, `advanced.texture_lod_bias = 0`) in
   `edvr.ini`; fly the orbit once more and also hover with the seam in view. The edge ribbing may be grazing-angle sampling of the far tiles
   (the terrain sampler tops out at 8x, `SurfaceSamplerQuality` 3) rather than loading: if the seams change in a static hover and the
   trace was quiet, it is sampling, not streaming.

## Method note

Scripts and decompiles are in the session scratch dir (`planet\`); nothing in the repo or the game directory was changed. The Ghidra project was opened
`-readOnly`. The EDVR sampler reading is from `src\d3d11\device_hook.cpp` on this branch.
