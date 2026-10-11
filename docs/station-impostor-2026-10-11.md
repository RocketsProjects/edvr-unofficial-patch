# Station impostor and the interior switch (2026-10-11)

Offline analysis of `EliteDangerous64.exe` build 332841 (PE TimeDateStamp 1788384820, SizeOfImage 104894464, image base 0x140000000; gate matched). RVAs are file RVAs. No C++ changed, no build, no flight. A scratch capstone index (not committed) covers 21.2M instructions, 1.79M RIP and direct references, 361k absolute data pointers and 253k `.pdata` functions. **Proven** means exe bytes. **Inferred** means the bytes do not close the point.

## Status

- **State:** analysis only, on `claude/station-impostor-switch` from `claude/concourse-cull-design` (d0b85b9d). Nothing fixed, flown or merged.
- **Switch:** not pinned to one compare. The interior is a component state machine. The near and far decision is taken by proximity triggers in a group created at registration (P1, P7). Their threshold is not a constant in the exe (P8).
- **Candidate state:** hub field `+0x168` (dword) with pointer `+0x170`, both tested in the hub update 0x1B0A190 (P4). The writer of the -1 state is not found.
- **Load or visibility:** streaming-shaped (readiness gates, a render-thread wait, an unload vocabulary). Not proven (I3).
- **Impostor:** no station impostor string or asset name in the exe. "Imposters" and "Imposter Debug" are render debug toggles (P9).
- **Concourse:** a sub-area built on the hub-root path (0x1B17C2E, reached from 0x1B179A0), plus SocialSpaceLevelMount and SocialSpaceBroadcastSync as separate types (P5). Same family; sharing one switch is inferred.
- **Forcing "far":** not supported. Do not patch the streaming state.
- **Patch now:** none. Keep the render-side masking in `concourse-culling-2026-10-11.md`. The freeze experiment in Design (c) is a probe, not a fix.
- **Open:** the writer of -1 at `+0x168`; the class at `+0x170`; the owner at `+0xF8`; the trigger's inside flag; whether the hangar sits in the hub-root level; `player_location` storage.
- **Next flight (no engine change):** a state dump of the hub object at three distances on approach, with entry counters on 0x1B0A190, 0x1B179A0 and 0x1B17C2E (see Flight test).
- **Probe branch:** `origin/claude/shadow-gate-probe` (8b4ebbe6) has no `hotkey.state_dump`; no matching text in its tree. The watch list assumes a probe still to be written.
- **Probe keys:** none added. **Gate for any hook:** TimeDateStamp 1788384820, SizeOfImage 104894464.

## Proven

**P1. Registration routine.** `0x1F4700` (11163 bytes) registers component names and object sizes; it does not build a station. It makes about forty registration calls (DecalsManager 0x318, ProxyDecalsComponent 0x90, PortalRotationComponent 0x2A0, and the interior set through `0x1B0BBC0` at 0x1F67E6). It also calls `0xAB9DE0` at 0x1F685E, which creates PlayerProximityTriggeredGroupCollection; that is its only caller. Whether it runs on every station load is not checked.

**P2. Interior types and sizes.** `0x1B0BBC0` (3138 bytes) registers each name with its object size: InteriorSubLevelAttach 0x88, InternalSpaceComponent 0x1A0, EscapePodComponent 0x260, TurboLiftSystem 0x1B0, DockingArmBoardShipButton 0xD0, InteractionAnimationObject 0x2B0. It also registers InternalSpacesManager (0x5213268), StationInteriorComponent (0x5213BC8), StationInteriorHub (0x5213E48), SocialSpaceLevelMount (0x5213810) and SocialSpaceBroadcastSync (0x5213550). Each static descriptor takes vtable 0x4DDBEE8 in this routine. No per-type creator was found.

**P3. Hub object.** Constructor `0x1AF7DC0` (1098 bytes) stores the primary vtable `0x52139F8` at 0x1AF7EE2 and the secondary `0x5213A28` at 0x1AF7EF0. It writes `+0xF8`, `+0x118`, `+0x130`, `+0x170` and `+0x198`, and it loads `+0x168` from the dword at 0x5F5A268. That dword is a serial counter: 0x0A56A0 does `lock xadd` on 0x5E63010 and stores the result, so `+0x168` starts as a unique serial, not -1. Slot 0 of the primary vtable is `0x1AFC2E0`, the scalar deleting destructor (flags in EDX; it writes both vptrs and offsets by `+0x278`). The class is InternalSpacesManager or StationInteriorHub; the bytes do not say which.

**P4. Hub update.** Slot 3 of `0x52139F8` is `0x1B0A190` (184 bytes). Checks in order:
1. owner = `[this+0xF8]`; if `owner->vf[28]()` (offset 0xE0) returns 0, exit;
2. if `owner->vf[80]()` (offset 0x280) returns 0, exit;
3. if `[this+0x168] == -1`, `[this+0x170] != 0` and `[this+0x170]->vf[6]()` returns 0, call `0x1AF4700`. That is a 22-byte prologue that falls into `0x1AF4716` (388 bytes). It writes through `&[this+0x168]` and has no float compare;
4. otherwise, loop `0x1B0A248` (136 bytes): for i below `count = [this+0x260]`, elements of 0x88 bytes at `[this+0x268]`, call the hub-root builder `0x1B179A0` with a pointer at stride `0x1000 / count` from `[this+0x198]`.

**P5. Level builders.** `0x1B179A0` (654 bytes) logs "Not ready to make hub root level" (lea at 0x1B17B53). It branches into the sub-area builder `0x1B17C2E` ("Not ready to make interior sub-area level") with `je` at 0x1B17BFE and `jne` at 0x1B17C15. The `jne` follows `call [rax+0xB0]` (vf[22]) on another object. So the concourse sub-area is built inside the same per-element path, conditionally.

**P6. Activity.** `InteriorStateChangeActivity`: GetTypeName `0x283DB90` (slot 1 of vtable 0x52EC3A0). The constructor writes the vptr at 0x280AD9A (31 bytes). Slot 2, `0x2853900` (55 bytes), reads `[this+0xF0]->vf[28]()` and jumps into `0x2853A9E` inside `0x2853A99` (229 bytes). That routine compares `[this+0x118]` with 2 and logs "is Waiting to complete RT initialisation" (string 0x52EC460). It returns true when the dword is 2.

**P7. Proximity triggers.** `PlayerProximityTrigger`: vtable 0x5230758. Constructor `0x1D34600` (262 bytes) writes the vptr at 0x1D34651, creates a shape at `[this+0x68]` through the generic creator `0x869EC0` (type id from the .data dword 0x5F02B6C), and copies two .data dwords (0x5F02CFC, 0x5F02CF8) into `+0x38` and `+0x50`. `PlayerProximityTriggeredActivityGroup`: vtable 0x513FBF8, constructor `0xAB7310` (226 bytes).

**P8. No distance constant in these classes.** Triage of every code slot in the hub vtables (30 unique), the activity vtable (15) and the trigger vtable (15): none contains `sqrtss`, `sqrtsd`, `comiss`, `ucomiss` or a float constant load from .rdata. A threshold, if any, is per-object data, or sits in the shape code behind `0x869EC0`.

**P9. Imposter names are render debug.** "Imposters" (0x52E70D8) and "Imposter Debug" (0x52E71B0) sit in the render debug label table at 0x5E82BD0, beside Bloom, Blur, Asteroids and Beam lights. Their readers are render functions: 0x2859DF0 (bytes 0x5E82BCD and 0x5E82BCE), 0x285B730 (0x5E82BCC) and 0x2872892 (0x5E82BCF). The dword at 0x5E82BC0 is a signed override (negative means off), read by 0x28360D0. None is station code.

**P10. Unload vocabulary.** "Unloaded" (0x5109760), "QueuedUnload" (0x51097A0), "NtfyUnload" (0x5135C60), "ReqUnload" (0x5135C70) and "Unloading" (0x5135C80) form a generic load-state vocabulary. "Unloading" is referenced by 0x1B42790 (651 bytes), which is not on the hub path, and read once by 0xA30173.

**P11. Configuration.** The root XML files (GraphicsConfiguration.xml, GpuWorkTable.xml, DeviceDefaults.xml, AppConfig.xml, AudioConfiguration.xml) contain no key matching Impost, Interior, SubLevel, Hangar, SocialSpace, InternalSpace, StationInterior, Stream, LoadDistance or UnloadDistance.

**P12. Names that look relevant and are not.** `InnerDockEntrance` (0x5217430) is read in a ship-module type switch at 0x1B3CA10 (Drive, Thruster, ShieldEmitter and so on). `InnerDockEntranceModule` (0x5349420) is referenced from 0x3079CE0, which 0x1F4700 calls at 0x1F67B4. `SetSubLevelAttachSlot` (0x5215500) is read byte by byte by a name matcher at 0x1B15BE0, not a setter. The "SocialSpace" reader at 0x145FF92 names carrier liveries and bars.

## Inferred

- **I1. Switch chain.** A proximity trigger in a station-registered group starts an activity; the activity waits for render-thread readiness (P6); the hub update then changes the level build (P4). Each link is plausible. The trigger-to-activity link is not traced.
- **I2. Threshold is data.** It is per-trigger or per-station data, or it sits in the shape code (P8). A flight can measure it; the exe does not hold it.
- **I3. Streaming.** Level builds gate on readiness (P5), and the activity waits on render-thread initialisation (P6). A visibility-only switch needs neither. Not proven.
- **I4. Hangar placement.** The hub-root level is built once per attach element, so the player's hangar is probably inside one of them. Not proven.
- **I5. Impostor.** The mail-slot placeholder is probably a model LOD, or a placeholder removed when the hub-root level builds. No string names it.
- **I6. Meaning of -1.** -1 is "no interior selected", and the chooser runs only then. The writer of -1 is not found.

## Dead ends

- ruled out: "Imposters" as the station impostor, because they are render debug names with render readers and no station link (P9).
- ruled out: `+0x168` as a plain interior index at construction, because the value is a serial counter (P3).
- ruled out: a distance literal in the hub, activity or trigger classes, because no compare, sqrt or float load is present (P8).
- ruled out: an XML streaming-distance key, because no root XML matches (P11).
- ruled out: `InnerDockEntrance` as the mail slot, because the string is a ship-module type (P12).
- ruled out: `SetSubLevelAttachSlot` as a setter (P12).
- ruled out: the SocialSpace reader as the concourse switch (P12).
- open: whether `player_location` (0x557A138, blackboard) has storage; never found.

## Design (not coded)

- **(a) Sealed hangar.** Do not force the streaming switch. Hub-root levels are built per attach element. A forced -1, or a skipped update, could unload or stall the hangar the player stands in. Use the render-side mask in `concourse-culling-2026-10-11.md`, which leaves the hangar cell drawn.
- **(b) Seated in the cockpit.** The concourse sub-area is built inside the hub-root path (P5). A render mask avoids touching the streaming state. Keep the existing design.
- **(c) Probe-only freeze experiment.** An early return at the hub update while sealed. Hook site `0x1B0A190`, prologue `48 89 5C 24 20 55 56 57 48 83 EC 40` (12 bytes). The update is virtual, so one entry hook covers every instance. Expected: no rebuild while sealed. Risk: the same update may drive pad and door state. Fly only with logs, never as a fix on this evidence.
- **Not recommended:** forcing the sub-area or hub-root state to "far". There is no threshold to hook, the switch streams, and the hangar is at risk.

## Flight test and state dump

Watch list for a probe (to be written; C++ work, not done here):
- Hub `H`: find by scanning for the 8-byte value `0x1452139F8` (primary vptr; the secondary `0x145213A28` is at +8). Watch `+0xF8`, `+0x118`, `+0x130`, `+0x168`, `+0x170`, `+0x198`, `+0x260` and `+0x268`.
- Owner at `H+0xF8`: record its vptr, which identifies the class for the P4 slots 28 and 80.
- Activity `A` (vptr `0x1452EC3A0`): watch `+0xF0` and `+0x118`.
- Trigger `T` (vptr `0x145230758`): diff bytes `+0x30` to `+0x80` each frame. Its inside flag is unidentified.
- Attach elements: `[H+0x268] + i*0x88`, diffing the first 0x88 bytes.
- Entry counters on `0x1B0A190`, `0x1B179A0`, `0x1B17C2E` and `0x2853900`, each logging `H+0x168`.
- Control: the render debug byte `0x5E82BCD`, to show the probe is not reading stale memory.

Flight: approach from beyond the trigger to docking, dumping at three distances; once docked; once sealed. Decision: if `H+0x168` changes and builds start only after that change, the selection is the switch. If builds start with no change, look at `T`. If neither changes, the switch is outside this hub.

## Method

Scratch only, not committed: `scratchpad\impostor\xr.py` (capstone sweep; commands `build`, `strings`, `xref`, `fn`, `vt`, `triage`, `strmap`, `who`), run with `python -I`. Outputs sit in the same folder.
