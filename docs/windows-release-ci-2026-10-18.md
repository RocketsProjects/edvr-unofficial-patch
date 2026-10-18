# Windows release builds from the Linux device

## Status

- State: investigating native Windows CI and preparing a current release
  workflow on `codex/release-workflow`. GitHub CLI authentication is working.
- Latest failed run: `38990816615`, source
  `e700f005ef390e1c17c207f422c9118784209948`.
- Actual failing gate: `flat_mono_resolve_shard2`, specifically the final
  Python motion-fixture verifier. The C++ suites reported PASS.
- Confirmed cause: separate `FlatDrawCapture` instances reused the same
  timestamp/PID/per-instance serial directory. A production-header no-GPU
  harness reproduced the collision; a process-wide atomic suffix fixes it.
- Ruled-out pointer: the seven first-person `FAIL:` lines belong to mutation
  children whose failures are required; their parent suite reports PASS.
- Next run: full canonical Windows build and packaging on `windows-2022`, with
  the motion fixture retained in diagnostics. No validation gates are bypassed.
- Release scope: create validated ZIP artifacts; release notes and draft
  creation remain explicit device-side steps. No public release is requested
  now.

## October 18: evidence before the proving run

The latest failed job passed dependency setup and ran 167 of 175 jobs before
stopping. `tools/flat_draw_pixels.py --verify-fixture` reported that the motion
fixture lacked two consecutive WARP frames. Its source and the C++ fixture
writer are unchanged between that run and the starting source `043d7ff6`.

The failed run uploaded only its build log and timing diagnostics, so its frame
manifests cannot establish whether frames were missing or mixed with another
capture. The next workflow retains that fixture instead of losing the evidence.

Earlier main-branch run `38979120037`, source
`2dd897b1a2279ebe35056ee247975cb5c7825743`, stopped in a source-contract
assertion in `sharp_weapons_test`: the INI description was expected to mention
the shield strike ripple. The later run reached the different
fixture-verification failure.

The historical release workflow is on another branch and uses obsolete build
options and installer assumptions. The temporary Windows installer test
workflow was removed from main by `519ec0bf`. A current workflow must use the
existing full-build receipt, one installer, and all three native release
archives.

## October 18: capture directory collision and local check

The old production `FlatDrawCapture::arm` gave two independent instances the
same `_1` suffix. A no-GPU, no-file-write harness using the real header found
`COLLISION pair=0` on this device. The fixed code obtains a process-wide atomic
capture ID; the same harness found no collision in 10,000 adjacent pairs. The
mono-resolve rig also now fixes the clock to one timestamp while arming two
independent instances and asserts different directory names without writing
files. It remains in the shard that exercises the GPU capture.

The full `flat_mono_resolve_test.exe` rig compiled with local MSVC 14.44
through `tools/compile_cache.py`; `tools/flat_draw_pixels.py --self-test`
passed and now prints expected/found frame IDs and the directory if the motion
fixture is mixed. Wine could not create the WARP device (`FAIL: WARP device`),
so the native Windows full build is still the deciding gate.
