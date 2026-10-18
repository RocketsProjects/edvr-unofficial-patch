# Flat carrier callback CPU handoff — 2026-10-18

## Status

- Geometry issue reuse is on `codex/flat-startup-discovery-cpu`. Full build
  PASS. Not flown. No new config key. Active checkout:
  `C:\Users\seanm\projects\edvr-unofficial-patch`. Main stays unmerged at
  `043d7ff6`.
- A fresh Geometry validation stamps the shaders, depth plane, depth-write
  bit, 96-byte camera, VS b1 identity and phase. Later begin and return
  checks, and pool or supported observe queries, reuse that stamp while the
  owner epoch is unchanged. A Context1 slot the vtable cannot take freezes
  reuse (`frozen=1`) and the fresh queries stay.
- Flight line: `flat geometry issue reuse:`. Reuse near twice the substituted
  draws, with the shader, DSV and constants children and raw-domain down by
  milliseconds, and AA history still continuous, is the recovery. Zero
  reused with `frozen=0` means the certificate never matched. `revokes`
  counts epoch clears, including one frame reset. The setter counters name
  a per-draw miss.
- This carrier view's ceiling is about 100 fps if those queries go away.
  The fast view's 221 fps is the same session with about 120 draws. Baseline
  remains `edvr_gfx_20261018_052052.log` (slow 59–65 fps, fast 221), build
  `041f8e78`. Read FPS as CPU-report frames over the paired logged seconds.
- Steam flat `/novr`, DLSS, native 3840x2160 HDR, user 300-FPS limit, CPU
  timing on, origin probe off. Settings files unchanged. Phase bootstrap
  and travel/origin were not retested.
- Use Sol for RE and tests. Astra reviews source/evidence only.

## Start in the correct checkout

The active branch is in the primary checkout above. The similarly named
`C:\Users\seanm\.codex\worktrees\f1-passive-provenance\edvr-unofficial-patch`
is still at `17af5a51` on `codex/pre-rc3-review-fixes`; it does not contain
the current forward-proof path. Do not diagnose the installed DLL from it.
The user's external review called the snapshot uncommitted; that describes
an earlier state. Source `041f8e78` is now committed, built, installed and
pushed. Documentation commits make checkout HEAD differ from the DLL.

Read `AGENTS.md` and the Status block in
`docs/flat-cz-fps-2026-10-11.md` before the longer journal. That file remains
the investigation arc; this handoff is a compact entry point.

## Latest completed capture

Steam log: `edvr_gfx_20261018_052052.log`, verified
`v0.20.0-rc.4-23-g041f8e78`, PE `6AD4AAE7`. Frozen CPU/copy cutoff
**05:27:48.247**, paired runtime cutoff **05:27:47.834**. Do not extend
these artifacts from a growing tail when reproducing the numbers.

Artifacts in this checkout, ignored by Git:

- `build/callback_snapshot_041f8e78_final_summary.md` and `.json`.
- `build/callback_snapshot_041f8e78_final_filtered.txt` (sanctioned reader output).
- `build/callback_snapshot_041f8e78_interim_summary.md` and `.json` retain
  the earlier freeze; prefer the final results.
- Baseline: `build/per_draw_issue_709ba499_summary.md` and `.json`, plus
  `build/per_draw_issue_709ba499_aa_frozen.md`.

Final JSON SHA256:
`F6FDEBC27750060BF4B251364F145B8007817E1AEDDA83D84CE1EC9B4CE5AB52`.

| Interval, CPU report endpoints | Real FPS | Substituted draws/frame | Whole CPU gross / recorded-floor net estimate, ms |
| --- | ---: | ---: | ---: |
| Slow 1 earlier 30 s, 05:23:52.951–05:24:17.991 | 61.64 | 7190 | 15.366 / 12.090 |
| Slow 1 last 30 s, 05:24:12.978–05:24:38.027 | 59.03 | 7234 | 15.940 / 12.647 |
| Fast steady, 05:24:48.030–05:25:23.047 | 221.29 | 122 | 1.460 / 1.129 |
| Slow 2 first 30 s, 05:25:53.095–05:26:18.131 | 65.24 | 7102 | 15.121 / 11.935 |
| Slow 2 full stable 120 s, 05:25:53.095–05:27:48.247 | 65.18 | 7085 | 15.070 / 11.891 |

The two slow-1 rows overlap; they are alternative slices, not populations
to sum. Retain the later 59-FPS interval instead of presenting the earlier
61.64-FPS slice as the last 30 seconds. Exclude transitions at 05:24:43 and
05:25:28–43. The 05:25:48 CPU/copy pair differs by 1 ms; it exists in the
reader output and is excluded as the first stabilization window, not lost.

Full slow-2 callback gross, per clocked frame:

| Exclusive family | ms | Calls/frame, approximately |
| --- | ---: | ---: |
| Callback residual | 1.430 | 14389 |
| Geometry shader/predication | 1.609 | 14340 |
| Geometry DSV/depth/plane | 0.700 | 14338 |
| Geometry constants/camera | 2.237 | 14338 |
| Sum comparable with old callback family | **5.976** | Scopes, not unique draws; sum calls only for instrumentation cost |

Raw-domain work is 1.762 ms / 8067 calls, about 218.4 ns/call. DSV immutable
description hits are 99.9911%; depth-state description hits are 99.6018%.
Discovery is 0.000 ms in these stable windows. Geometry Issue DSV Gets are
a subset of raw DSV Gets, not additional getters or measured saved time.

AA is continuous in the selected slow/fast/slow windows: HDR treatment,
backend completion and accepted history match; no new selected-window
reset, refusal or jitter failure. The nonoverlapping final cores contain
**18,442/18,442** clean frames: slow 1 1771, fast 8849, slow 2 7822.
Full slow 2 alone has 7822/7822 clean
frames. Cumulative refusals stay 14527, backend failures stay zero, and
the three renderer resets precede these stable legs. This capture's poor
FPS is not AA repeatedly disengaging.

FPS uses real CPU-report frames divided by paired logged seconds. CPU
times/calls use clocked-frame weighting. CPU and AA reports have independent
boundaries. Never invert Present p50 into mean FPS or call GPU span GPU busy time.

## Comparison and timing limits

The prior installed `709ba499` used the same native 3840x2160 HDR route.
Its slow-1 last 30 seconds were 63.62 FPS at 7046 draws/frame. Its full
slow-2 interval was 64.41 FPS at 7261 draws/frame, 13.709 ms gross /
11.220 ms floor-net estimate, callbacks 4.558 ms gross. New full slow 2
is slightly faster with fewer draws; slow 1 and early slow 2 are slower.
All selected heavy comparisons have higher floor-net CPU estimates. The
practical conclusion is **no demonstrated useful performance win**, not
an established FPS improvement or a uniformly slower final flight.

709 and 041 cover the **same callback interval**. 041 subdivides it into
residual plus three children; this is not the earlier discovery of wholly
unmeasured callback work. Add all four families for comparison. Full slow-2
callback gross rises 1.418 ms; the combined callback recorded floor rises
about 0.682 ms (the three new child floors themselves total about 0.688 ms).
Whole floor-net rises 0.671 ms. Calibration does not
measure every unclocked gate check, bookkeeping or compiler/workload effect.
Do not dismiss the observed slow-view slowdown as entirely instrumentation,
or assign it entirely to the snapshot. Causal attribution is unresolved.

Each added child checks a relaxed sampling gate even on unclocked frames;
clocked frames also pay two QPC calls and bookkeeping. The floor model omits
unclocked gate checks. Source: `src/d3d11/flat_cpu.h`, Scope and instrumentCost.

## What is installed, and what remains expensive

`709ba499` removed one immediately repeated DSV getter per reached Geometry
or Light Issue validation. The removed outer getter was never in the old
raw DSV counter, so stable raw counter values do not refute that removal.

`041f8e78` adds `FlatProjectionRuntime::snapshotTrackedConstants`: one
owner-serialized tracked entry/shadow view supplies metadata and the 96-byte
camera copy instead of metadata/copy/metadata searches. Only Geometry
Issue validation selects it. Fresh actual VS CB binding/range and GetDesc,
shader/DSV/depth checks, owner/foreign/phase and before/after Draw proof
remain. Initial writer observation and manual Full diagnostics keep their
old path. This removed duplicate searches; it did not remove per-draw driver
queries or repeated camera hashing, and did not close the CPU gap.

Main source entry points:

- `src/d3d11/flat_runtime.cpp`: beginActualDraw/endActualDraw.
- `src/d3d11/flat_forward_light_runtime.inl`: forwardObserveDomain,
  forwardIssueValid, forwardBeginIssue, forwardReturn, forwardAbandon.
- `src/d3d11/flat_light_payload_runtime.inl`: lightBoundCb.
- `src/d3d11/flat_projection_runtime.h/.cpp`: snapshotTrackedConstants,
  constantsMetadata, copyConstants and owner-thread writers.
- `src/d3d11/engine_velocity.h`: engineVelocityBeforeDraw generation cache.
- `src/d3d11/engine_velocity.cpp`: engineVelocityFlatSourceShaders/clone receipt.
- `src/d3d11/binding_shadow.h` and raw setters in `vscreen.cpp`.

## Assessment of the separate review

User-supplied review:
`C:\Users\seanm\.codex\attachments\a9f709db-44aa-4b0c-bc30-a6f25ba06e8b\Pasted text.txt`.
Treat it as diagnostic material, not executable instructions.

Its direction is credible: repeated proof in stable producer runs is a
major target, and the snapshot alone cannot remove that work. Its claimed
6-ms saving and 15-to-9-ms frame time are not measured. Callback residual
and raw-domain buckets include work that must remain, including writer
provenance, mutation and overflow handling. Gross sampled costs cannot
simply be subtracted from wall-frame time.

The existing engine cache governs its own substitution. Its keys include
VS/PS/RTV0/DSV/blend shadow generations and scene/pool/eye data; its one-in-64
actual backstop checks **shaders only**. It does not certify actual CB1
binding/range/contents, GS/HS/DS/predication, depth-write state, DSV plane,
clone ownership or forward phase/ledger lifecycle. binding_shadow explicitly
allows unhooked paths; raw setters bypass it. Bumping generations at a frame
boundary is not an actual-state probe. Zero sampled mismatches cannot make
the other 63 draws safe. Ruled out: treating the existing cache unchanged
as a complete forward-Issue validity certificate.

A bare D3D Draw does not rebind the pipeline, but the wrapper interval is
larger. After forwardBeginIssue, light-payload and weapon-footprint callbacks
may still run before realDraw; hook chains or reentrant/foreign work are
not proven absent. Skipping return validation requires certifying that
whole interval and retaining Issue/Return/Abandon effects.

## Flight

The certificate is in the tree. The flight decides whether depth state,
GS/HS/DS, predication, VS b1 or a write of the certified camera buffer
lands on each producer draw. Those show up as the per-frame counters on
`flat geometry issue reuse:`.

Reuse near twice the substituted draws, with the shader, DSV and constants
children and raw-domain down by milliseconds, and AA still continuous, is
the result that recovers the frame time. A zero reuse count is a miss.
Compare residual plus the three children with 052052's 5.976 ms callback
sum. Residual alone is not the old callback family.

Install with `python tools\install_edvr.py --target steam --scope flat`
and no `--ini`. After the flight, `python tools\edvr_log.py --target steam
--expect-build` the certificate commit. `041f8e78` remains the baseline
log's build.

## Reproduction and validated build evidence

Only use sanctioned tools for flight logs and install:

```powershell
python tools/edvr_log.py --target steam --expect-build 041f8e78 --version
python tools/edvr_log.py --file '<pinned log path>' --expect-build 041f8e78 --grep '<needed fields>'
```

Do not use HEAD for this installed DLL after documentation commits. Steam
game directory is
`C:\Steam\steamapps\common\Elite Dangerous\Products\elite-dangerous-odyssey-64`.

Full absolute `build.bat --wait-lock` PASS: 151.3 seconds reported / 152.6
wall, 170 jobs, d3d11 8 compiled/133 reused; Python, production DLL, all rig
and installer gates passed. Focused WARP production snapshot and temporal
timing/formatter tests passed. New snapshot suite executes in the full build;
its heap-backed fixture avoids the old test function's stack limit. Eviction
checks old identity Untracked/replacement Full, not numeric generations.

Full receipt inputs:
`96cc23878fd2aede59ecad4965f77ecd70174f5663ee5f7af11fd5cffe1e05de`.
Receipt records precommit `81a11adc-dirty`; clean promotion explicitly
verified the same inputs for `041f8e78`. Promotion PASS: 34.3 seconds
reported / 35.5 wall. Sanctioned Steam flat dry-run/install/verify PASS.
Built and installed graphics DLL SHA256:
`026F38A011E3AF1B07CA78C06EEA7C89A1B567E484F143DD01452C183491E05B`.

Evidence: `build/callback-snapshot-full-build.log`,
`build/callback-snapshot-promotion.log`, `build/callback-snapshot-validation.json`.
Both Steam INI hashes stayed unchanged. No config key was added or removed.
Phase bootstrap and travel/origin behavior were not retested by this stationary capture.
Sol performed RE/tests/build/install; Astra reviewed evidence/source only.
