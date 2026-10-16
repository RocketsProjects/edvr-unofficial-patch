# Particle billboards: is there one engine-side root cause? (2026-10-16)

## Status

- **State:** read-only analysis, no code, no flight. Companion to
  `docs/particle-billboards.md` (the Frontier write-up, left unchanged).
- **Verdict:** the single engine-side root cause exists and the arc already
  named it: every camera-facing billboard family spans its quad on the camera
  basis in the scene constant buffer, `cb1[277]` (right, inferred), `cb1[278]`
  (up) and `cb1[279]` (forward). That is a GEOMETRY bug. It shows with temporal
  AA off and on a monitor. It is not a motion-vector bug, so no velocity fix can
  make `fix.particle_billboard = steady` unnecessary.
- **What steady changes:** only the basis, per vertex (face = particle minus
  the viewer, up = world up). It leaves motion vectors, depth, blend and history
  weight alone. Its design says the bug is the basis.
- **The cheaper engine-level fix that exists (E1, not built):** rewrite
  `cb1[277]`/`cb1[278]` once per view, in the game's mapped scene CB at Unmap, to
  a roll-free basis. That fixes ROLL for every family at once (the 7 shaders,
  plus `68DDDEF04D9894AF` and the untranscribed ones), with no shader swap and
  no per-draw work. It cannot fix the eccentric foreshortening, because facing
  the viewer is per particle and only a vertex shader can read the particle.
  Steady is the minimal COMPLETE fix, and its cost is in how it is implemented,
  not in what it does (see "Cost" below).
- **Open hypotheses:**
  - H1: only billboard-family shaders read `cb1[277]`/`cb1[278]`. This gates E1.
  - H2: `[277..279]` are per eye, and differ between eyes on canted displays
    (Pimax), which gives the stock quads a different orientation in each eye.
  - H3: part of steady's frame cost is inherent: un-foreshortened peripheral
    quads mean more translucent fill. The rest is CPU overhead.
- **Ruled out:**
  - ruled out: a motion-vector root cause that would retire steady, because
    steady changes only the quad's geometry, and the roll and swim show with
    temporal AA off and on a monitor (particle-billboards.md "The symptom").
    Correct motion vectors would make the TAA faithfully reconstruct a plume
    that still rolls with the head.
  - ruled out: a per-draw constant fix for foreshortening, because the facing
    is per particle (particle_fix.cpp:228-233; the per-emitter facing retired
    2026-09-23, particle_fix.cpp:208-224).
- **Evidence needed before E1:** (1) a corpus scan of every dumped VS and PS for
  `cb1[277]`/`[278]` reads, where cb1 is the 5376-byte scene CB (H1); (2) one
  log of `cb1[276..279]` for both eyes on Pimax and on SteamVR, which settles
  H2 and the "[277] = right" inference. The probe that particle_fix.h:15-18
  describes no longer exists; only `shapeOk` reads those registers.
- **Next flight:** none of its own. If E1 is wanted, build the two instruments
  above as log-only and fly them in one session at a geyser field.
- **Temporary keys:** none added.

## The engine side, from the bytecode and the census

Plume VS `EB787F983BC1F5A3` (docs/shaders/particle-vs.asm):

- Position: `v0` (per vertex) times `cb1[222].z`, then through `cb0[9..11]`, the
  208-byte camera block's world rows. The result is camera-relative world space.
- Basis: `right = normalize(cross(cb1[278], cb1[279]))` and
  `up = normalize(cross(cb1[279], right))`, built in the VS from the scene
  constants. It is not built on the CPU and not stored per particle. The
  velocity-aligned and axis-aligned modes (ALIGNBLENDBRIGHT) use the particle's
  own DIRECTION and AXIS instead and are correct.
- `cb1[279]` also drives the near-fade (`dp3 cb1[279], pos`, line 102). A fade
  under 0.001 collapses the quad (lines 188-191).
- Clip: `cb1[270..273]`, the view projection. `o1` passes the clip x, y and w to
  the PS, the soft-particle depth fade.
- No previous-frame position is output, and nothing writes velocity.
- The stretched sky billboard `68DDDEF04D9894AF` takes `cb1[277]` and `cb1[278]`
  directly (particle-stretched-vs.asm:12-16). Same registers, different body.
  `DB113566B6A59C5B` (a supplied axis or world up) is the correct member.

Census row (analysis/npc_blur/log_1112_1115.txt, 11:13:50, eye-sized viewport
2016x1949):

    X n=72 i=1 vh=EB787F983BC1F5A3 sd=92 ia=0 / ia=72 ds=02wA bl=12,6,1/2,6,1

- `n=72 i=1`, stride 92: 12 quads in one draw, with the whole particle record
  repeated on each vertex and a signed DIMENSIONS value per corner. The CPU
  writes 4 vertices per particle, which implies a buffer simulated and refilled
  each frame. The buffer's Usage is not in the census.
- `ds=02wA`: the depth test is DISABLED, so nothing is written to depth (the
  write mask is moot).
- `bl=1 2,6,1`: premultiplied alpha (ONE, INV_SRC_ALPHA, ADD).
- per-object-motion.md:1152-1160: the 336-stride engine record never reaches
  these families. (That passage's "instanced-billboard particle cluster",
  `0357BBB2...`/`8289669D...`, is the light volumes per gpu-pass-profile-2026-10-12.md.
  The label is wrong and nothing here depends on it.)

Ghidra: the CPU writer of registers 277-279 was not located, and nothing here
needs it. `FUN_143d0cf10` stores doubles at +0x1150..+0x1170 of a CPU camera
object (analysis/decomp/flash/decomp_3D0CF10.txt), which is not the CB. EDVR's
hook is the D3D Map/Unmap of the scene CB. vscreen.cpp already tees that
buffer: `camResource`, then `temporalPassNoteSceneWrite`.

## The candidates

| | candidate | verdict | discriminating evidence |
|---|---|---|---|
| a | view-dependent orientation missing from the motion vectors | TRUE, but it is geometry first | roll and swim show with AA off (the symptom list); steady changes only the basis |
| b | per-eye orientation | INFERRED for canted displays (H2) | `cb1[270..273]` is necessarily per eye, so the block is too; `[277..279]` per eye is unlogged |
| c | no previous state for dynamic particle buffers | TRUE, and out of reach | CPU-expanded stride-92 stream, no record, no previous output; the flipbook (UVSCURRENT/NEXT, TEXBLEND) cannot be expressed as motion at all |
| d | translucency without depth, so the motion vectors belong to whatever is behind | TRUE, and it is the TAA problem | `ds=02wA`; dlss-blur-elements-2026-10-14.md group G1; fixed EDVR-side (coverage or reactive), not by steady |

Steady encodes (a)+(b) as geometry. Under TAA it also helps (a) incidentally:
a steady quad is world-fixed under a pure head rotation, so the camera-term
reprojection is right for it, apart from (d).

## Engine-level fixes

- **E1, the roll-free basis in the scene CB.** At the scene CB's Unmap, before
  the real Unmap, write `[277] = normalize(cross(worldUp, fwd))` and
  `[278] = cross(fwd, [277])` into the game's mapped memory. Take `fwd` from the
  view rows EDVR already copies, or read `[279]` (16 bytes). Cost: once per
  view, 32 bytes written. Writes to write-combined memory are cheap; it is the
  reads that are slow (issue-65 doc). It needs no draw-ladder term, no shader
  swap and no per-family transcription. Covers: roll, for every family,
  including the ones never transcribed. Leaves: the foreshortening, and the
  per-eye forward. Risk: any non-billboard reader of `[277]`/`[278]` (H1); and
  the world-up assumption, which steady also makes ((0,1,0); level up measured
  at (0.056, 0.981, 0.185)).
- **E2, one basis for both eyes.** Write the head-centre forward into both
  eyes' `[279]` as well. That fixes the canted-display stereo mismatch (H2), but
  it also moves the near-fade depth axis per eye. Only if H2 is measured.
- **E3, full facing.** Per particle, so a vertex shader is required: that is
  steady. No single-point engine fix exists outside the game's code.
- **E4, previous-basis motion vectors.** Rejected: (d) means the TAA never
  samples a billboard's own motion, and correct motion would preserve the roll.

## Cost: where steady's price can come from (for the pricing pass, not priced here)

1. `particleWantsDraws()` holds the per-draw ladder open by itself
   (vscreen.cpp:1689). With temporal AA and FSR off it may be the term that
   keeps the gate open, if nothing else default-on subscribes.
2. `particleCapture` memcpys the WHOLE scene CB (ByteWidth, 5376 B) out of the
   game's mapped pointer at every Unmap (vscreen.cpp:3345-3349, 3437-3439;
   particle_fix.cpp:390-398). That pointer is write-combined on AMD (issue-65
   doc:324, Windows AMD 1-3 GB/s). The fix uses 160 bytes, `[270..279]`. This
   is a second read of a buffer EDVR's camera tee already reads.
3. Per matched draw: VSGetConstantBuffers, a 32-byte Map, the 3x3 solve
   (`solveCamera`, which runs per draw rather than per Unmap), VSGetShader plus
   two Sets plus two restores.
4. GPU, inherent to correct facing: peripheral quads are no longer
   foreshortened (cos of the eccentricity), so wide-FOV headsets draw more
   soft-particle fill. An engine fix with full facing would pay this too.

Discriminator: steady against stock in a scene where the steady note reports
0 draws. If fps still drops there, the cost is 1 or 2, not the substitution.
