# Camera rows after a drop: a parked camera's zero

## Status

*Last updated 2026-10-11: the continuity fix is PARKED on `claude/vr-camera-continuity`
(not for merge); the per-draw census is built on `claude/vr-camera-diag`, NOT flown.*

- **2026-10-11 (open):** a docked hangar shows one-frame camera excursions (75 m / 2.9 deg
  and smaller). The census (`diag.temporal_draws`, default off) asks whether a view-pose
  draw exists every frame and whether its VS hash or projection identifies it. Next:
  same hangar, key on, Pause on visible resets (2026-10-11 entry).
- **2026-10-11 (built, NOT flown):** `fix.hmd_camera_tag` hooks coordinator `0x594B60` and stamps the writes made inside it; the chooser takes the frame's latest stamp. Entry at the end. Next: hangar, key on then off.

- **State:** the 2026-09-25 gate fix (branch `claude/camera-rows-carry`, merged
  to main) is FLOWN and working as designed, and its open hypothesis is now
  closed by evidence: H2 CONVICTED 2026-09-27 (the dated entry, eye run 055427)
  -- a parked stay of 4674 frames (~52 s) in one frontier session, and a dump
  that caught a carried ~zero through a 12.6 px head move: the world path's MV
  read ~0 for 14 straight frames while the content moved every frame, and the
  station hull (84% on the world path at that range) blurred under its own
  motion. The stand-down the doc deferred is now owed: after how many carried
  frames, and to what.
- **Symptom:** a brief sky/world smear during a roll or turn, two frames long,
  wherever the transition-flash tracker logs a "camera parked at" line. Found
  in eye run 050423 (flight log `edvr_gfx_20260925_050051`, Frontier, build
  c9cab91e = v0.18.0-rc.1). And the long form: a sustained smear for the length
  of a stay, 055427.
- **Cause (confirmed from the dump):** after the chooser landed on a parked
  auxiliary pass (148 deg from the view, rejected, carried), the next frame
  took the same pass's identical write. That delta was zero, and under a still
  head a zero passes the 3-degree test. The old gate ACCEPTED it and stored it
  as the last good. The next frame, back on the view, carried the zero.
- **Open hypothesis, the entry (H1):** the chooser left the view because
  nothing in 19996 was within 3 deg of 19995's rows: a hitch of about 16 frames
  of flight (below). In this dump the hitch was probably the eye run's own draw
  census. The fix does not depend on H1.
- **H2 (closed, convicted):** "if stays run long, a stale carry is a worse
  guess than the head path, and the world path should stand down after a few
  frames." Stays run long: 4674 frames. The stand-down is the open design.
- **Open, the scale (H3, the 2026-09-27 entry):** the rows' delta undershot the
  head's by (1+k), k = -0.347, over a whole frontier session near the station,
  with a steady (+0.11, -0.88) px registration offset on the world-depth
  probes; auxiliary passes jump ~9300 units every ~120 frames and were chosen
  on 4032 frames. Interleaved auxiliary writes or a sampling skew against the
  head stream -- undecided.
- **Ruled out / declined:** see "Declined" at the end. It covers measuring from
  the last accepted rows, and refusing every delta after a drop.
- **Latest evidence (162120):** scene-row blocks match the selected rows within
  5e-6; stale carry and auxiliary contamination do not explain that run. The
  substitution defect is in the kinematic-motion doc (2026-09-28); H2 and H3 open.
- **Next:** no Coriolis flight until a concrete repair is proven. The roll/turn
  protocol in "Verification" is the H2 stand-down test once designed.
- **Environment:** depends on the world path (`fix.temporal_aa`, depth motion)
  and the game's view rows at float 932 of the scene block. It does not depend
  on the VR runtime, headset or DLSS version, since it reads the game's own
  rows. Flown rig: Pimax (runtime "Pimax OpenXR") on EDVR's native runtime,
  DLSS 2037x1969 -> 4074x3938 per eye. Fixed-size table: the chooser's ring of
  256 writes (unchanged).


## The event (eye run 050423, eye 0)

`eye_050423_motion.csv`, crops 0 to 5, turn and move computed from the
rows:

| frame | rows prev -> now | turn | move | rowsOk | world delta used |
|---|---|---|---|---|---|
| 19994 | view -> view | 0.72 deg | 0.73 | 1 | its own |
| 19995 | view -> view | 0.88 deg | 0.76 | 1 | its own (D95) |
| 19996 | view -> AUX | 148.45 deg | 21.65 | 0 | D95 carried |
| 19997 | AUX -> AUX (identical) | 0.000 | 0.000 | **1** | **identity, accepted** |
| 19998 | AUX -> view | 148.36 deg | 7.47 | 0 | **identity carried** |
| 19999 | view -> view | 1.56 deg | 1.73 | 1 | its own |

AUX is `(-199.5, 1491.6, 779.8)`. It matches the transition-flash tracker's
"parked" cameras in the same session (`(-170 +1170 +595)` at 05:04:18,
`(-204 +1548 +812)` at 05:04:26). Those cameras sit along the ship's path,
re-placed every second or two. `rowsBound` is 1 on every frame, so the
auxiliary pass writes into the same block as the view.

The census line at 05:04:31 covers the whole session (3 min 40 s). The
delta was dropped on 120 eye-frames as another camera's: 60 frames over two
eyes. An event of this shape drops two (the entry and the exit), so that is
up to 30 events, about one every 7 s. The chooser resynced ("nothing
followed last frame's") on 52 frames. Only stays of two frames or more hit
the bug: a one-frame stay has no frame in between to accept a zero. The
accepted zeros were counted nowhere; the new "parked" count is their
witness.

## The entry (H1): a hitch

On every frame the turn and the move keep the same ratio, about 0.9 deg
per unit: a steady roll and speed, with each frame's share scaling with its
frame time (20008, a long frame, has 2.88 deg and 3.19 units). From 19995
to 19998 the view turned 25.9 deg and moved 29.1 units. That is the same
ratio, so it is the same flight, but it is about 16 frames of it in three.
So the view's own write in 19996 was more than 3 deg from 19995's rows, the
chooser found nothing continuous, and its resync took the bound block's
latest write, which was the parked pass's. The eye run armed its draw
census at 05:04:23.499 (`DC begin census=1 frames=3`), which wrote 10,804
log lines in two seconds. That is the likely hitch in this dump. The rest
of the session's events happened without a dump, so hitches in ordinary
flight also enter this way. A dump taken with the census off, or a timed
frame log, would separate the two.

Nothing reprojects a quarter-second hitch. The view's own delta at 19996
was well over 3 deg and would have been dropped anyway. Carrying the last
real delta across it is the best available.

## The mechanism

`chooseCameraRows` follows continuity from last frame's pick, and the
plausibility block measured each frame's delta from last frame's pick. Once
a frame was rejected, its rows (the parked pass) became the next frame's
reference for both of them. The chooser then found the parked pass's
identical write continuous (the twin fallback, "for a camera that truly
stood still"). The gate saw a zero delta against a head turning 0.02 deg
and passed it, because the 3-degree test only bounds the ship's turn and a
zero is a plausible ship turn. Stored as the last good, the zero was then
carried by the exit frame.

## The fix (temporal_math.h: `temporalCameraGateStep`, `temporalCameraGateAdvance`)

- The gate keeps a standing for the reference rows (`refOwn`). It moves on
  once a frame, at `temporalPassFrameBoundary`. The rows are the view's own
  only if a delta to them was measured and neither eye refused it. Rows no
  delta reached, such as the first after a frame with no write, are not.
- A delta measured from rows that are not the view's own, whose orientation
  matches them to the last bit, is refused (`Parked`). A tracked head does
  not hold the same orientation from one frame to the next, so rows that
  did not turn at all belong to a parked or world-fixed camera. The last
  good delta is carried, and the rows stay suspect through the whole stay,
  including a pass re-parked with the same orientation.
- A delta that turns and passes the 3 degrees restores the standing. The
  view resuming after a hitch or a resync (19999) is accepted at once, as
  before, so recovery costs no frame.
- Unchanged: the 3-degree test (`Another`); the floating origin's jump
  (the translation dropped before the last-good store); the carried-delta
  witnesses (`g_camCarried`, and `g_camCarriedJump`, zero by construction);
  a still twin after the view's own rows is still accepted as the identity.
- New census clause: "on N as a parked camera's (not turned at all, from
  rows a drop had left; the longest stay M frames)". N counts eye-frames;
  M counts frames, once for both eyes. The eye trace's `rowsOk` is 0 on a
  refused frame.
- The world delta arithmetic moved unchanged from the pass's lambda into
  `temporalWorldFromRows`, so the rig runs the pass's own code. The rig
  reproduces the DLL's dumped `cameraR`/`cameraTv` for 19995 and 19999 to
  1e-5.

## Verification

Next flight, Frontier install:

```bash
python tools\install_edvr.py --target frontier --verify-only
python tools\edvr_log.py --target frontier --expect-build HEAD --version
python tools\edvr_log.py --target frontier --grep "registration, the rest|camera parked at"
```

- Pass: in `eye_<stamp>_motion.csv`, no frame that follows a `rowsOk 0`
  frame shows a zero prev -> now rotation with `rowsOk 1` during a roll. A
  refused frame shows `rowsOk 0`, and its `cameraR` is the last real delta
  (about a degree of roll), not the identity.
- The census's new "parked camera's" count is non-zero on a flight with
  parked-camera lines. That is the witness: those are the frames the old
  gate would have accepted as zero. Its "longest stay" answers H2: a few
  frames leaves the carry as it is, and tens of frames call for the stand-
  down.
- Fail signatures: a `rowsOk 1` identity after a `rowsOk 0` during a roll.
  Either the fix is not running (check the version line), or the parked
  pass's rows changed orientation by a hair between frames, which is a
  variant that bitwise identity does not catch and needs its own look. A
  world that visibly spins on after a roll has stopped, with a long stay
  on the census, is H2.

## Declined

- **Measuring the next delta from the last ACCEPTED rows** (asked for as
  the first option): after a real jump of more than 3 deg (a hitch), every
  later frame is measured across the jump from a stale anchor, and none is
  accepted again. It would need an escape hatch, and that hatch is this
  fix's recovery rule.
- **Refusing every delta after a drop** (the second option, literally): it
  either never recovers, if a refused frame counts as a drop, or accepts
  the second frame of a two-frame parked stay, if only the 3-degree test
  counts. The standing is the working form of it: refuse what did not turn,
  and restore on what did.
- **A tighter angle test against the carried delta** (the ship's turn rate
  is continuous): it needs a threshold between 0.45 deg (19999 against
  19995's rate, a real change of frame time) and 0.88 deg (a zero against
  19995). That is a threshold nudge, not a cause.
- **Changing the chooser's reference after a drop:** the chooser must keep
  following the rejected rows. After a hitch those rows ARE the view, and
  continuity from them is how the view is found again.

## 2026-09-27 -- H2 convicted, and a scale problem beside it (eye run 055427)

Frontier, build v0.18.0-rc.2-2-g2b285732, parked close to a coriolis port;
game build 332841. The session census (05:54:42, whole 3.4 min session): the
camera's delta was dropped as a parked camera's on 13,462 eye-frames and the
LONGEST STAY WAS 4674 FRAMES (~52 s) -- H2's witness is in, and tens of
frames were the bar for the stand-down. The eye run (055427, 16 frames,
05:54:27) caught a stay spanning real motion: the stored world-path MV reads
~0 from its frame 2 through 15 while the rendered content, measured by grid
search on the raw frames, moved 0.6-12.6 px a frame (a 12.6 px head move at
frames 1-3, then a steady ~1 px/frame drift). Deep space reads the same zero
as the hull, so the carried delta itself was ~zero. DLSS got that MV on 84%
of the port face and dragged its history: the station blurred along its
motion, exactly H2's "a stale carry is a worse guess than the head path".
Full measurement table in docs/kinematic-motion-injection-2026-09-19.md's
2026-09-27 entry.

Beside the stays, a second, chronic reading: "the rows turned (1+k) times
the head, k = -0.347 (x -0.309, y -0.295, z -0.544), leading by -0.17
frames" over the session, and the history's best match sat (+0.11, -0.88) px
from the world path's prediction across 5758 probes -- a steady offset, i.e.
a lag or a scale, not noise. The rows also chose "another's" block on 4032
frames while auxiliary passes near the station jumped ~9300 units every ~120
frames. Whether the undershoot is interleaved auxiliary writes or a sampling
skew against the head stream is open -- call it H3. H1's hitch question also
stands: the eye run's own hitch remains the suspected entry for the dump's
stay, and the 05:53:52 window (mid-approach, no dump running) already had
parked carries, so ordinary flight enters too.

Open design question, now owed an answer: the stand-down -- after how many
carried frames, and to what (no history at all, or the head path) when the
world's own rows cannot be trusted. 055427's stay outlasted a 12.6 px move;
that is the cost of the current answer.

## 2026-09-28 -- 162120 does not reproduce the stale-carry defect

Frontier eye run 162120, failed-fix build f2f27f97: all 16 actual captured
scene-constant camera blocks match the selected rows within 5e-6, and all 32
eye evaluations have rowsOk=1, rowsBound=1, rowsFollow=30. The ledger's
pre-boundary frame convention is deliberate: ledger.frame = CSV.frame+1
(object_probe.cpp, ledgerNoteDraw), not a C/D motion lag.

ruled out: stale carry and later auxiliary selection contamination as the blur
cause IN THIS RUN, because the accepted/bound rows are the rows used by the
captured scene. This does not retract 055427's 4674-frame stay or its dead
world vectors; H2 remains a real, separately evidenced defect outside this
dump.

Independent raw-image warps need <=0.05 input px median quiet correction on
engine pixels but roughly 0.3-0.6 px on world-covered details, with a 6 px
opening-motion miss. Production-patcher hash proof identifies the alleged
native/refused shaders as EDVR's own substitutions, mishandled by the old
PS-shadow probe. Current main already contains F7's ownership repair, absent
from this installed build. The old-source F7 reproduction, stronger next-frame
regression, and full production/installer validation have passed; promotion,
installation, and headset verification follow. The complete ROI table and
reconstruction-floor exclusion live in
docs/kinematic-motion-injection-2026-09-19.md's 2026-09-28 entry. No new flight
is needed for diagnosis before that verified repair is installed.

## 2026-10-11 -- hangar excursions: the census, and the continuity fix parked

**Finding (eye run edvr_gfx_20261011_055853, docked hangar, DLSS, eye 0, TCAM
frames 9684-12461).** The chosen rows left the view pose on one-frame
excursions to another camera: 75 m / 2.9 deg (flagged 0x2000 origin step, frames
11663, 11743, 11756, 11768, 11780, 11792, 11804), 19.8 m (11732-33), and 0.14 m /
2 deg (20 pairs). Normal frames rotate by a median 0.039 deg a frame.

**Cause (code).** `chooseCameraRows` tests continuity by orientation alone: a
write within 3 deg of last frame's chosen rows. A camera facing the same way but
elsewhere passes. The gate then zeroes a 50 m+ step's translation and keeps the
2-3 deg rotation error, and the next frame takes the excursion as its reference.

**What the log rules in or out (1,536 eye-0 TCAM lines).**
- The projection z row (`proj`) is (0, 0.0250000004) on every sampled line,
  excursions included. It cannot tell the cameras apart. The x and y rows (the
  FOV tangents) are not captured.
- Selected at the view and the first recognised draw at the view: 1,346. Selected
  at the view and the draw ODD: 181. Selected odd and draw odd: 9. The selected
  write is always newer than the draw's write. So the first-draw snapshot is not
  an independent oracle: it sits at the odd pose on 181 frames where the chooser is
  right.

**The continuity fix is PARKED.** Branch `claude/vr-camera-continuity`, pushed as
a8fec2d2 (WIP, not for merge, unbuilt as a unit): a constant-velocity chain
(position 1 m + 2 x step; rotation 0.5 deg + 2 x rotation step), re-anchor on the
second agreeing frame, and a gate guard. Its rig cases pass (147 ok lines on that
tree). Parked because the question it depends on is not answered yet.

**The census (this branch, `claude/vr-camera-diag`).** Key `diag.temporal_draws`,
default OFF (temporary; its render-thread cost is unmeasured). With the key on,
every recognised eye-target scene draw is recorded per eye per frame in a 256-frame
ring: the rows the draw's VS b1 object held (translation, rotation as a quaternion),
the VS hash, the index count, the draw's index in the eye's frame, and the
projection block's floats 784-799. At most 32 draws per eye-frame are kept; the
rest are counted. The Pause dump writes a `TDRW legend` line, `TDRWF` per eye-frame
(drawn, kept) and `TDRW` lines (at most 4000 a dump, newest frames). The 30 s
`temporal draw census:` line gives the draws and the census's own QPC cost. The
OpenXR trace gets one `projection_raw` line per eye per session: the FOV tangents
EDVR hands Elite, as integers in units of 1e-7.

**Rig.** `tools\temporal_test`: N recognised draws give N records with their VS
hashes, index counts, translation, quaternion and P floats; a 40-draw eye keeps 32
and counts 40; a slot is reset when the ring comes round; key off stores and counts
nothing; a non-rotation write keeps its P floats but no rotation; a record that
does not fit its buffer is refused. The dump's cap is not in the rig.

**Next flight.** Sean in the same hangar, `diag.temporal_draws = on`, Pause on
visible resets. The read: is there a draw on every frame, excursion frames
included, whose rows are the view pose (compare TDRW T and R with the TCAM
`selected rows` of the same frame)? Does its VS hash, or its projection P, identify
it deterministically? Also record the census cost from the 30 s line.

## 2026-10-11 -- the headset block's writer, traced statically (build 332841)

Question: which code writes the headset eye camera's rows into the camera block the chooser reads, and what separates that write from the other cameras' writes. Offline only: disassembly of `EliteDangerous64.exe` (PE stamp 1788384820, image size 104894464, so build 332841 by `tools\elite_hotspots.py --pe-info`). No EDVR code changed; helper scripts stay in the scratchpad. RVAs are image RVAs.

### Proven by disassembly

1. **Headset call chain.** Per-frame VR job `0x2868C30` (parts `0x2868C30..0x286938A`):
   - calls the camera driver `0x28431D0` at `0x2868C8B` (rcx = `[r14+0x1188]`, edx = 2, r8 = `[r14+0x11C0]`);
   - then calls coordinator `0x594B60` at `0x286906E` with rcx = `[[r14+0x1180]+0x18]` and dl = 1 (`mov dl,1` at `0x2869068`).
   - `0x594B60` has one direct caller in the image, `0x286906E`.
2. **Driver output.** The driver copies two composed matrices into objects at +0x20 (`0x59F1A0` called at `0x28435A0` and `0x28435C4`). In the C2 model (`tools\c2_derive_test\c2_derive_model.h`) +0x20 is the Cam source axes, so the eye views are Cam-type objects. That is inferred from the offset; the object's kind is not read here.
3. **Coordinator `0x594B60`** (`0x594B60..0x594D89`). With C = rbx and cam = `[C+0xE0]`:
   - push loop (`0x594BFA..0x594C2C`): for each item in the list at `[C+0x108]` (count at +8, array at `[list]`), set item+0x38 = 1, then `call [item+0x60]` at `0x594C1D` with rdx = C+0xE8 and r8 = cam+0x1A0;
   - commit loop (`0x594C81..0x594CA4`): `call [item+0x88]` at `0x594C92`, rcx = item;
   - a loop over `[C+0xF8]` calling `0x6BE790` (`0x594CC7`), then `0x5919E0(C+0x1E0)` and `0x5875B0(C)`.
   - It never calls the refresh `0x5921F0` itself. C+0x248 is a call counter, incremented on each call.
4. **The refresh `0x5921F0`** (entry; the flat injector's detour site is `0x592200`). Direct callers: `0x594E0E`, `0x594EA6` and `0x594FDC`, all in the view loop `0x594D90` (one caller, `0x58F2EF` in `0x58EF90`), and `0x58DE6E` in wrapper `0x58DE50` (five callers). The view loop pushes items through `+0x70` (`0x594E4E`, rdx = cam) and `+0x78` (`0x594ED2`), not `+0x88`.
5. **Upload primitive `0x51B640`.** Map (`0x51E190`, which calls `0x50F000`), then memcpy of sizeA * sizeB bytes (sizeA = r9; sizeB = the fifth argument, stored at `[rsp+0x20]` at each call site; `imul` at `0x51B655`), then Unmap (`0x523190`). Twelve direct call sites:

| site | sizeA (r9) | sizeB (stack) | note |
|---|---|---|---|
| `0x501A4A` | 1 | word `[r14+4]` | |
| `0x5DB414` | eax (dynamic) | `0x60` | |
| `0x27BC487` | 2 | 8 | |
| `0x280EDCB` | min(ebx, edx) | `0x38` | |
| `0x287506E` | min(esi, edx) | `0x38` | |
| `0x28A13EC` | dynamic | dynamic | |
| `0x28A1433` | dynamic | dynamic | |
| `0x3849A0C` | esi | `0x1C` | |
| `0x3C01A5F` | 1 | 8 | |
| `0x448438B` | ecx | `0xA0` | |
| `0x4C53126` | `[rdi+rbx*8+8]` | `0x10` | record loop, item 6 |
| `0x4C83E32` | eax (dynamic) | `0x30` | |

   Only `0x4C53126` has a runtime count times a stride of 16. A 3776-byte (236 x 16) block cannot be picked out of the static code alone.
6. **Record loop `0x4C52E40`** (body `0x4C52E82..0x4C53164`). For records at `[r14+0x160]` (count `[r14+0x158]`, 40-byte stride, rbx = 5n) it calls `0x51B640` at `0x4C53126` with src = rec+0x10, count = rec+8, ctx = rec+0x18, stride 16. It is slot `+0x10` of the vtable at `0x5A6E0F8`, whose constructors are `0x4C52358` and `0x4C526B8` (`lea rax,[rip+...]`). That table's `+0x60` and `+0x70` hold the stub `0x39F0`, and it has no `+0x88`.
7. **Pose chain.** `GetEyePose` `0x4E25A0` (render dispatch entry at `0x4E24638`) and the job-thread caller `0x4E3DF0` (entry at `0x4E246B8`) both reach the fetch `0x4E3690`, which calls WaitGetPoses at `0x4E3715`. Direct callers of the fetch: `0x4E2401` (in `0x4E2350`), `0x4E264C` (in `0x4E25A0`), `0x4E3E0E` (in `0x4E3DF0`) and `0x4E59C5` (in `0x4E5900`).
8. **The commit slot is not an identity.** `call [reg+0x88]` appears 1652 times across 1352 functions.

Call graph:

```
VR job 0x2868C30
 +- driver 0x28431D0            (rcx [r14+0x1188], edx 2, r8 [r14+0x11C0]) @0x2868C8B
 |    +- composer 0x283D4C0 (x2); eye-view copies 0x59F1A0 @0x28435A0/0x28435C4 (+0x20)
 +- coordinator 0x594B60        (rcx [[r14+0x1180]+0x18], dl 1) @0x286906E
      +- push: items [C+0x108], item.vt[+0x60](item, C+0xE8, cam+0x1A0), cam=[C+0xE0]
      +- commit: item.vt[+0x88](item) @0x594C92      <- Map/memcpy/Unmap inferred here
      +- [C+0xF8] -> 0x6BE790; 0x5919E0(C+0x1E0); 0x5875B0(C)
refresh 0x5921F0 <- 0x594E0E, 0x594EA6, 0x594FDC (view loop 0x594D90 <- 0x58F2EF in 0x58EF90); 0x58DE6E (wrapper 0x58DE50)
upload  0x51B640: Map 0x51E190 -> 0x50F000; memcpy sizeA*sizeB; Unmap 0x523190
record loop 0x4C52E40 -> upload @0x4C53126 (vtable 0x5A6E0F8 +0x10)
```

### Inferred, not proven

- **The commit writes the headset block.** The commit at `0x594C92` is the headset's commit and is synchronous inside `0x594B60`. The Map, memcpy and Unmap happen inside the item's `+0x88` method, whose class is unresolved. A breadth-first search over direct calls from `0x594B60` (30 functions, depth 4) found no path to `0x51B640` and no `[reg+0x70]` Map call, as expected when the path runs through vtable calls. The transition-flash doc's Ghidra pass names this function as the upload of `cb1[275]`; that agrees with the commit loop but is not re-verified here.
- **Thread.** The VR job's thread is unknown: its only direct caller, `0x27BB7D0`, lies outside .pdata. The render-thread GetEyePose path fits the CPU profile, but nothing here confirms it.
- **Which camera `cam = [C+0xE0]` is.** The coordinator never refreshes it, so whether it is the kind-5 eye or another object is open.
- **Mailbox.** The writer `0x2874B20` (gated by `_stricmp` on two names) and the tick `0x10730A0` come from `docs\design-transition-flash-engine-fix-2026-09-23.md`. Only the writer's pointer at `0x52E89E0` and the tick's caller `0x1030C20` were re-checked here.

### Ruled out

- ruled out: K (vtable `0x5A6E0F8`) as the coordinator's item class, because its `+0x60` and `+0x70` are stubs and it has no `+0x88`.
- ruled out: `0x5A6E108` as a vtable base, because the real base is `0x5A6E0F8` and the table's later slots are ASCII names.
- ruled out: the `+0x88` slot as the discriminator, because 1652 call sites share it.
- inconclusive: the store scan for displacements 0xE90 and 0xC40. The hits in `0x4B5DFD0..0x4B9F7B0` are large SIMD routines, not camera packers.

### Proposed hook (not built, not flown)

- **Site.** CodeHook on `0x594B60` (EXE+0x594B60). Pin the 30 prologue bytes: `48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 54 41 56 41 57 48 83 EC 20`. The relay (as in the flat producer probe on `kimi/camera-producer-probe`) bumps a thread-local depth, calls the trampoline, and drops the depth again.
- **Gate.** PE stamp 1788384820 and image size 104894464; the prologue match; the call site `0x2869068` reads `B2 01 48 8B 48 18 E8`, and its rel32 resolves to `0x594B60`. Any miss: not installed, and the chooser is unchanged.
- **Detour.** The depth goes up by one, the detour records C (rcx) and the frame, calls the original, and the depth goes down. C+0x248 is the call counter and can cross-check the frame.
- **Chooser.** On the Unmap side in `vscreen.cpp` (the `camResource` block near line 3410), a write made while depth > 0 is stamped as the headset's. `chooseCameraRows` takes the latest stamped write of the frame; with none, it keeps today's choice.
- **Per-frame cost.** One relay call and two thread-local stores per frame (tens of ns). One thread-local read per Unmap (about 1 ns).
- **Other builds.** Inert; the chooser is unchanged.

### Next flight (what would confirm or refute)

- Log per frame: depth, C, and the number of stamped Unmaps. Expect one stamped set per frame, including a block of 3776 bytes or more. Refute: no stamped block while depth > 0, which means the commit is deferred and the inferred step fails.
- Compare the stamped block's rows at float 932 with the eye draws' TDRW rows, using the 5e-6 test from 2026-09-28.
- Static follow-up that would turn the inferred step into a proof: find the vtable of the items in `[C+0x108]` (the constructors that store it into objects added to that list) and read its `+0x88`.

## 2026-10-11 -- the headset tag, built on build 332841 (NOT flown)

Branch `claude/vr-camera-hmd-tag`, from `claude/vr-camera-source-trace` e1d7c4c9. Full build green; the commit and `--dll-only` promotion are in the git log.

**What was built.**
- **Hook** (`src\d3d11\hmd_camera_tag_hook.cpp`): a CodeHook on `EliteDangerous64.exe+0x594B60`, the coordinator. Gated on the PE stamp 1788384820 and image size 104894464, the 30 prologue bytes, and the 7 call-site bytes at `0x2869068` (`B2 01 48 8B 48 18 E8`) whose rel32 resolves to `0x594B60`. Any miss is one `hmd camera tag: inert:` line, nothing patched, the chooser unchanged. VR only; flat is inert.
- **Detour**: a thread-local depth scope (RAII, so a C++ throw restores it) around the original call. It records rcx, the thread id and the frame. It allocates and logs nothing.
- **Stamp**: `temporalPassNoteSceneWrite` (`src\d3d11\temporal_pass.cpp`, called from the camResource Unmap at `src\d3d11\vscreen.cpp`, the `g_temporalCameraBudget` block) marks a rotation write `hmd` when the Unmap runs on a thread inside the coordinator.
- **Chooser** (`chooseCameraRows`): after today's pick, the frame's latest stamped write replaces it when `fix.hmd_camera_tag` is on. With no stamp, or the key off, the pick is today's. The counts run either way, so the line says what the tag would have done.
- **Key**: `fix.hmd_camera_tag`, default on, in `edvr.ini` `[fix]` as a hidden flight A/B (it must go when the arc closes). Flat reads it `off`: the key is not on the flat allow-list (`runtime_profile.h`).
- **Pure core**: `src\common\hmd_camera_tag.h` (gate, depth scope, `latestStamped`, `decide`, the 30 s line's text). Rig: `tools\temporal_test`, 25 new checks (gate on the true bytes and on one-byte changes, a stamp chosen over a newer unstamped pose, the no-stamp fallback, key off, depth balance across nesting and a throw, the 30 s line, the census cost line, the TDRW `hmd` field). All pass.

**Instruments.**
- `temporal hmd tag: hook=live|inert:<why> coordinator_calls=.. per_frame=.. stamped_writes=.. frames_with_stamp=a/b chosen_by_tag=.. fallback=.. disagree=.. key=on|off last_frame=.. last_thread=.. last_c=..` every 30 s, on its own QPC clock at the frame boundary, zeros included.
- TCAM `cf` bit `0x400` = the pick is the stamped write (legend updated).
- TDRW gains `hmd=1` when the draw's object rows equal the frame's latest stamped rows made before the draw.

**The census line (`temporal draw census:`), root cause.** `temporalPassRegistration` returns at its entry guard when `g_intervalFrames == 0`, and the census line was the last thing it printed. `g_intervalFrames` is advanced only by the native resolve's stats readback in `pollSlots`. A trained (DLAA/DLSS) frame queues its slot with `statsDone = true` (`temporal_pass.cpp`, the enqueue: `statsDone = !(statsWritten || (ran && !usedDlaa && !leanOwn))`), so the readback never runs. The trained path still advances `g_treats`, so the totals line printed and the census did not. Evidence: `edvr_gfx_20261011_072408.log` (DLAA; "first trained frame" at 07:24:13) has 5 TDRW legend lines and no census line, and none of the 25 recent logs carries a `temporal aa registration:` line. The census counters were also reset inside that function after the return, so on the trained path they were never reset and the cost was a session total. Fix: the line is now `temporalPassCensusLine()`, called from vscreen's totals block, with its own counters and frame count (`g_tdrwFrames`; the old denominator was `g_rowsFramesSum`, which only the registration reset). The cadence is the 20 s totals window (`kTotalsWindowMs`), not the 30 s this doc's earlier note said. Not fixed here: the registration line has the same guard, so it still never prints on the trained path.

**Risks.**
- The stamp assumes the headset block's Map/memcpy/Unmap run on the thread that calls the coordinator. That is inferred, not proven. If they run elsewhere, `stamped_writes=0`, every frame is a fallback, and the chooser is today's. That is the designed failure mode, but the flight will look like "hook live, nothing stamped".
- The coordinator also calls `0x6BE790`, `0x5919E0` and `0x5875B0` inside the scope. A camera-block Unmap made there on the same thread would be stamped too. A wrong stamp shows as `disagree` and as TDRW rows that differ from the view.
- The chooser runs at the frame's first treat. A stamp written after that is not seen this frame, as with the legacy pick.
- The hook's last-call fields are per call, not per frame: `last_frame` reads the frame the chooser was on, which can lag the game's frame by one.

**Next flight** (hangar, the same docked scene as 2026-10-11 055853; Pause on visible resets):
1. `fix.hmd_camera_tag = on`, `diag.temporal_draws = on`. Expect: `hook=live`; `per_frame` about 1.00; `frames_with_stamp` about all frames; `chosen_by_tag` about all frames; `disagree` nonzero only on the one-frame excursion frames (the 0x2000 origin-step frames); no visible resets on Pause.
2. `fix.hmd_camera_tag = off`, same scene. Expect: the same `hook=live` and counts, `chosen_by_tag=0`, and the excursion frames back, with `disagree` still counted (it says what the tag would have done).
3. In either run, the `temporal draw census:` line should now print each 20 s totals window. Its cost figure is per window, not a session total.
4. TDRW `hmd=1` rows should equal the TCAM `selected rows` (the view pose) on every frame that has a stamp.
Refute: `stamped_writes=0` with `hook=live` means the Unmap is not on the coordinator's thread; the next static step is the `[C+0x108]` item vtable's `+0x88`.