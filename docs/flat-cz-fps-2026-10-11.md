# Flat on-foot CZ frame rate (report of 2026-10-11)

## Status

State: BUILT on `claude/fps-drops-onfoot-czs-577f5a`, NOT FLOWN. A bounded experiment on one of three suspects, not the fix.
Report: flat v0.19.1 (Steam, build 6ACAF769), RTX 3050, "40 fps without the mod, 20-25 with DLSS" in on-foot CZs.
Working files (gitignored): `reviews\flat-cz-fps-findings-2026-10-11.md`, `reviews\flat-cz-fps-review-2026-10-11.md`.

Evidence, from the reporter's log `edvr_gfx_20261011_184408.log`:
- The log holds an in-CZ A/B: `fix.temporal_aa` toggled through F8, off at 18:58:43, 19:01:52, 19:13:28. vScreen 20 s windows:
  DLSS 21 fps (19:01:49), off 45 and 46 (19:02:29, 19:02:49), TAA 27 (19:03:29). Off is 1.5-2x faster each time: the
  temporal path costs 10-25 ms a frame, and TAA is as slow as DLSS, so it is CPU, not the NGX resolve.
- 1080p DLAA and 810p DLSS ran the same speed (810p is only 48 s of the log).
- Heavy windows (over 1500 substituted draws a frame): Present minus GPU span 13.7 ms median, 1.0 ms in light windows.
- EDVR's render-thread time, clock inflation removed (printed in the `flat cpu` line, about 1.6 ms): median 10.5 ms a frame, of
  which the per-draw work is about 8-9 ms.
- Job threads: about 2,800 merge and 3,500 clear observer calls a frame. A clear costs 0.32 us in the low mode and 4.02 us in
  the high mode at the same call count: the high mode is waiting. Render-thread engine hooks: 0.42 us a call low, 2.14 us high,
  and the high mode follows the clear walks (`nodes walked`). The 36.7M-call totals are one 19:09:32 snapshot, not the session.
- The copier takes `g_mutex` about 2.7k times a frame on the render thread; emissions run about 25 a second, copies about 70k.
Ranked causes (independent review): (1) EDVR's per-draw work on the render thread, about 2/3 of the delta, NOT touched here;
(2) `g_mutex` contention between render-thread copier and tees and job-thread clear walks that exist only because every merge
registers a plan, about 2-3 ms on average; (3) observer work lengthening Elite's job phase (H-obs primary): UNPROVEN.

Built (commit 1): a published flag `g_claimsLive` (no source claim and no detached plan), kept exact by `ClaimLock`, the only way
the header locks `g_mutex`. A clear with the flag false returns without the mutex; a merge with it false gets a fixed token with
no plan, no walk and no allocation; the clear walk's three sets are small-buffer sets (no heap up to 8 nodes). Expected gain:
about 2-3 ms of the 10-25 ms delta.
Open: was the reporter fine on 0.19.0? The observers and the flat substitution are byte-identical between v0.19.0 and v0.19.1
and the always-on 0.19.1 additions all run in the 45 fps off state, so the diff cannot explain it. Ask which `temporal_aa`
mode and which scene they ran on 0.19.0.
Flights that discriminate: F8 off / TAA / DLSS every 30 s in one on-foot CZ (the delta on another CPU); the same CZ on this build,
reading the `other threads` merge and clear ms, `nodes walked` (about 0 while claims are 0) and fps against substituted draws.
NOT claimed: that the observers are on Elite's critical path; that this recovers the off/on delta; that any fps is gained.

## What changed

`src\d3d11\engine_velocity_primary_copy.h`: `g_claimsLive`, `ClaimLock`, `AddressSet`, `mergeNoClaimsToken`; `stats()` folds the
lock-free counters into `clearCalls`, `clearNoClaims`, `mergeCalls`, `mergeWithoutClaims`. Merge nodes are no longer counted
for a no-claim merge, and `merge plans` counts full plans only. The observer-work log line says so.
Unwind of the no-claim token revokes every claim registered since (the old empty plan spared unrelated later owners after a
clear or a frame change). Safe direction, native unwinds only; two old checks now assert it. The full plan keeps its precision
(tests in `primary_copy_observer_tests.h`). A malformed list with no claims no longer bumps the epoch or counts a merge failure.

## Benchmark

`engine_velocity_test.exe --bench` (opt-in, `tools\engine_velocity_test\primary_copy_observer_tests.h`): in-process fake of
Elite's lists, ns per call, one machine, same run for both columns.

| case | before | after |
|---|---|---|
| merge begin+end, no claims, 1 node | 325 ns, 11 allocs | 5.9 ns, 0 |
| merge begin+end, no claims, 8 nodes | 966 ns, 24 allocs | 5.9 ns, 0 |
| clear, no claims | 12.9 ns | 5.2 ns |
| clear, no claims, a no-claim merge in flight | 2495 ns, 80 allocs | 5.2 ns, 0 |
| clear walking 1 node, claims present | 421 ns, 16 allocs | 70 ns, 0 |
| clear walking 8 nodes, claims present | 2486 ns, 80 allocs | 743 ns, 0 |
| merge with a live claim, 1 node (full plan) | 325 ns | 259 ns |
| 4 threads, merge no claims (per thread) | 1427 ns | 36 ns |
| 4 threads, clear no claims (per thread) | 95 ns | 32 ns |
| 4 threads, merge then clear, no claims | 2784 ns | 63 ns |
| 4 threads, merge then clear, claims present | 3766 ns | 1996 ns |
| copier, active pool, no claims, 1 slot | 16.9 ns | 16.8 ns |
| 4 threads, copier (per thread) | 119 ns | 119 ns |

The 4-thread no-claim figures are bounded by two shared counters; striping them was not worth the code.
