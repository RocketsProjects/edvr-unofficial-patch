# Windows release builds from the Linux device

## Status

- State: current `main` is `de80be5d`. Prior full validation and packaging run
  `39134563542` at `8d5d25c` remains valid prior proof; the flat draw collision
  is resolved.
- Latest run `39138326218` and retry attempt 2 failed the same H6
  `heartbeat_writer_test` late-close assertion. One of 46 heartbeat checks
  failed on each attempt; the build stopped after 49 of 176 jobs had run. H5
  scheduling measured 821 ms and 829 ms.
- The heartbeat writer, its test, and `build.bat` are unchanged between
  `8d5d25c` and `de80be5d`. Failure logs and diagnostics are retained under
  `.toolchains/windows/ci-39138326218/`.
- Evidence: H6's fixed 300 ms sleep can assert before its worker runs. A
  temporary 700 ms hook delay reproduced the old failure; waiting for the hook
  and worker exit passed the delayed case and caught a mutant that ignored the
  close. The logs expose no counters, so the state of the native failures
  remains unproven.
- Candidate: synchronize only the H6 test and report its counters on failure.
  The edited rig passed all 46 checks under local MSVC/Wine; production code
  remains unchanged. The exact new commit still requires full native Windows CI
  validation.
- Next: validate the corrected commit and retain its verified artifact paths;
  then install flat + XeSS on the Linux device without `--ini` and verify it.
  Preserve the existing flat settings and device setup. No public release is
  requested.
- Prior resolved pointers: the flat capture collision and annotated-tag
  selection edge case are documented below; neither explains the current H6
  failure.

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

## October 18: workflow tag selection

A branch proof originally always made a lightweight runner-local CI tag. On a
commit already carrying an annotated release tag, Git still selected the
annotated tag, so the expected stamp did not match. A scratch Git repository
reproduced that failure. The workflow now reuses the exact supported version
tag at HEAD, or creates the runner-local proof tag when HEAD is untagged.

The actual PowerShell block passed scratch checks for real release tags,
annotated and lightweight tags on branch proofs, an untagged branch proof, and
a clear rejection of an unsupported exact tag. Actionlint and PowerShell syntax
checks passed before restarting the native proof.

## October 18: native proof completed

[Run
39134563542](https://github.com/characterecho-sean/edvr-unofficial-patch/actions/runs/39134563542)
passed at source `8d5d25c3263705fcc5b7d98396be971129f51cf7`. The complete
`build.bat --wait-lock --jobs 4` took 19 minutes 37 seconds, including the
production DLLs, test rigs, installer and final gates. The log ends with
`Native OpenXR build and all gates passed.`

The full-build receipt has status `full-pass`, describe string
`v0.0.0-ci.39134563542.1`, and input fingerprint
`1075b69559f4fb6662fe840f5c5c6c7b85b0e49d7aa69023dd5ad5a769751b70`.
`package.bat` then produced the main, Windows-installer and Linux-installer
ZIPs. All three downloaded archives passed `sha256sum -c SHA256SUMS` on this
device; the Linux scripts retain executable ZIP attributes.

Logs, receipt and proof packages are retained locally under the Git-ignored
`.toolchains/windows/ci-39134563542/`. The proof tag exists only on the runner;
no release version tag or public release was created.

## October 18: heartbeat writer retry failure

Current `main` at `de80be5d` failed native workflow run `39138326218` in H6's
late-close assertion in `heartbeat_writer_test`; retry attempt 2 failed the
same way at the same commit. One of 46 heartbeat checks failed in each run; the
build stopped after 49 of 176 jobs had run. H5 scheduling was 821 ms on attempt
1 and 829 ms on attempt 2. The downloaded diagnostics and failed logs are under
`.toolchains/windows/ci-39138326218/`, including `failed-job.log` and
`failed-attempt-2.log`.

The fixture posted to the heartbeat, slept a fixed 300 ms, then asserted that
there were no calls, no writes, and the writer was closed. Only the worker hook
sets the closed flag. The heartbeat writer, its test, and `build.bat` are
unchanged from the previously validated `8d5d25c` commit to `de80be5d`. The
logs have no counters that reveal the observed fixture state, so they cannot
prove that either native failure was only scheduling.

A temporary rig delayed the hook by 700 ms: the old H6 check failed before the
hook closed the writer. The candidate waits for the hook to finish and the
writer to exit before checking the same no-call, zero-write, closed conditions.
That delayed case passed with the candidate. With the inner close check removed
from a temporary header, the same case failed as intended: one call and one
write after close. The repository rig compiled with local MSVC and passed all
46 checks under Wine; `mutants.py --self-test` passed its 25 source anchors.
Production code is unchanged. Full native Windows validation of the candidate
commit is still required before installing its artifacts for flat XeSS testing.
