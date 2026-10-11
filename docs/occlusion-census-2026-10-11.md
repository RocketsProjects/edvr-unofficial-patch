# Occlusion census, 2026-10-11

## Status

- State: BUILT on branch `claude/occlusion-census`, pushed; NOT merged, NOT installed, NOT flown.
- Question (Sean's theory): Elite does no occlusion culling in station interiors. Sitting in an enclosed hangar, it still
  draws ships outside, the station's exterior hull and the planet. Measure it.
- Instrument: `src\d3d11\occlusion_census.cpp` (glue) and `occlusion_census_core.cpp` (queries, arithmetic, formats). For four
  frames, every scene draw into the scene's colour target with the depth test on gets an occlusion query. A draw whose query
  reports zero samples drew no visible pixel at the point Elite issued it.
- Keys (temporary diagnostics, remove when this arc closes, quoted per Scope control):
  `hotkey.occlusion_census` (empty = off) and `diag.occlusion_census = off|on`. Both allowed in flat.
- Cost: zero per draw while unarmed (the game's trampolines stay in the real-draw slots). While recording, one query and two
  context calls per depth-on scene draw, plus topology, depth-state and blend-state reads. Pool 32768 queries, record table
  131072 records (about 96 bytes each, about 12.6 MB at the cap); query objects are made on first use, and their driver-side
  memory is not measured here. Read-back polls once per frame with DONOTFLUSH and never blocks; a query that has not answered
  after 600 frames is called lost.
- Whole frames only: a frame whose draws overflow the pool is dropped whole and the capture ends there.
- Rig: `tools\occlusion_census_test` (WARP): one zero of three measured draws, the depth func and depth-only tag, a
  depth-only pre-pass draw, whole-frame rollback on overflow, the lost-results and no-draws trace states, the CSV and report formats.
- Parser: `python tools\edvr_log.py --occlusion-census` (self-test in edvr_log.py's gate).
- Open: the flight below. Nothing is concluded from a single flight.
- Hotkey: the 2026-10-11 flight (10:46) never fired `hotkey.occlusion_census` (F6). Presses were refused by focus, most likely; see Log.
- Ruled out (hotkey path, 2026-10-11): Elite binding F6 (Custom.4.2.binds has no Key_F6, only F1, F9, F11, F12); a fault
  in tickHotkeys (none logged); a dead live rebind (setBinding re-registers and the rig covers it); a suspended menu (suspension is only
  set by a menu capture, and the log's first "menu: open" is at 10:48:29); a discarded request (it logs unconditionally when pressed).
- Caveat that limits the reading (not a bug): a zero means "hidden at the moment this draw was issued". A draw Elite issues
  before the walls that hide it still has samples. So a low zero share does not prove Elite culls well, and a high one proves
  the draws were wasted at their draw time. See "Limits".
- Next flight: hangar, docked, hangar doors closed. Press the key once facing the back wall, once looking up, and once in open
  space as the control. Read each capture with `edvr_log.py --occlusion-census`.

## The instrument

`hotkey.occlusion_census` arms one capture; `diag.occlusion_census = on` arms one about 20 s after the first in-scene frame.
The capture starts at the next frame tick and records the next four frames of draws. Each draw is one of:

- scene draw, depth test on: bracketed by `ID3D11DeviceContext::Begin/End` on an occlusion query, read back later;
- scene draw, depth test off (UI, full-screen passes): counted as `skipped_no_depth`, never queried;
- draw off the scene target: counted as `other_target`;
- draw on a deferred context: counted as `foreign_context`, never queried.

"Scene target" means the eye texture in VR (the draw path's own `rtv0Eye` answer for the bound RTV0) and, in flat, a 2D target
the size the flat runtime's scene sizes name. Triangles come from the topology (triangle list and strip, with adjacency), times
instances; the indirect and auto draws name no count, so they are measured but contribute 0 triangles.

## How to read it

`python tools\edvr_log.py --occlusion-census` prints each capture with its VERDICT lines:

- `zero_sample_triangles` over `triangles` is the headline: the share of the measured scene's triangles that drew no visible
  pixel at issue time.
- The shader pairs (`vs`, `ps`) ranked by zero triangles say which shader is drawing the hidden geometry. Compare them with
  the top single draws, which give the frame, the eye or flat target, the render-target and depth-view ids, and the depth
  state (`test-write` or `test-nowrite`).
- `by depth state` groups the measured draws by comparison function, depth write and depth-only (a pre-pass: no colour
  target, or colour write mask zero). A main pass with EQUAL or GREATER_EQUAL and writes off tests the final depth of the
  pre-pass, so its zero count does not depend on draw order; the parser prints that as a VERDICT. A LESS pass with writes on
  is order-dependent, and its zero count is only a lower bound on what the scene could hide.
- `samples_p50` per pair separates "almost hidden" (small counts) from "fully hidden" (0).
- `accounting` must read `lost=0` and `overflow=0` for the figures to be complete; the parser says so when they are not.
- `no draw reached the scene target` means the scene-target test matched nothing (wrong target size or no eye target), so the
  zero share is meaningless for that capture.

The CSV `edvr_logs\occlusion_census_<stamp>.csv` has one row per scene draw in issue order: frame, eye or flat, VS and PS
hashes, kind, count, instances, samples, triangles and status (`measured`, `nodepth`, `overflow`, `noquery`, `lost`, `pending`).

## The flight

Hangar, docked, doors closed, not moving. Use the Explorer Cam only if the view must change; say which.

1. Back wall, facing it: press the key once. Read the capture. Expect the hull and the outside ships to show zero if the theory holds.
2. Looking up at the roof: press once. The hull above and the planet's sky should be the zero-heavy part.
3. Open space, outside the station, same ship: press once as the control. A high zero share here would mean the test itself
   reports hidden geometry that is not hidden, which would falsify the instrument.

Each press is one capture. Note in the doc which capture is which; do not average them.

## Limits

- Order. The query asks whether a draw passes the depth test at the moment Elite issues it. Walls drawn after the outside
  geometry leave that geometry with samples. The census cannot see later draws. A draw-order-independent answer would re-test
  each draw against the finished depth buffer, with depth-equal and writes off, after the frame; that costs a second GPU pass
  and state changes, and it is a follow-up, not part of this instrument.
- Flat shader hashes are read from the binding shadow at the real call. Where a flat fix substitutes a draw's bindings, the
  hashes name the substitution, not the game's own shader.
- Four frames. A hangar window can be dominated by one scene's draws; repeat presses before concluding.
- No fix is proposed from this instrument. Culling would be a root-cause change in how EDVR or the game orders draws, and needs
  its own hypothesis and a flight.

## Log

- 2026-10-11: instrument built (branch `claude/occlusion-census`, base `claude/vr-camera-writes` 46805658). Keys
  `hotkey.occlusion_census` and `diag.occlusion_census`, both allowed in flat. Rig and parser added.
- 2026-10-11: flight edvr_gfx_20261011_104619 (v0.19.1-39-gc09b563a, VR). The live reload at 10:46:53 bound F6 (census) and
  F7 (sun-skip). Neither key ever logged a request or a flip. Both loggers print on every accepted press, so the Hotkey
  edge was never true. Pause, on the same build, dumped twelve times from 10:49. The two keys had no focus-refused note
  (only the history and census keys did), so a press refused for focus was silent. The fix makes the note apply to both.
  Which window had focus during the 10:46:53 to 10:48:29 presses is not in the log; the next flight decides it: a line
  "The occlusion census key was pressed, but another window had focus" means focus, and no such line with no request
  means look elsewhere.
