# Flat on-foot CZ frame rate (report of 2026-10-11)

## Status

State: BUILT on `claude/fps-drops-onfoot-czs-577f5a` in three commits (observers, copier, lock instrument), NOT FLOWN. A bounded
experiment on one of three suspects, plus the instruments that say whether it was the right one. Not the fix.
Report: flat v0.19.1 (Steam, build 6ACAF769), RTX 3050, "40 fps without the mod, 20-25 with DLSS" in on-foot CZs.
Working files (gitignored): `reviews\flat-cz-fps-findings-2026-10-11.md`, `reviews\flat-cz-fps-review-2026-10-11.md`.

Evidence, from the reporter's log `edvr_gfx_20261011_184408.log`:
- In-CZ A/B: `fix.temporal_aa` toggled through F8, off at 18:58:43, 19:01:52, 19:13:28. vScreen 20 s windows: DLSS 21 fps
  (19:01:49), off 45 and 46 (19:02:29, 19:02:49), TAA 27 (19:03:29). Off is 1.5-2x faster each time: the temporal path costs
  10-25 ms a frame, and TAA is as slow as DLSS, so it is CPU, not the NGX resolve. 810p was no faster than 1080p (48 s of 810p).
- Heavy windows (over 1500 substituted draws a frame): Present minus GPU span 13.7 ms median, 1.0 ms in light windows.
- EDVR's render-thread time, clock inflation removed (printed in the `flat cpu` line, about 1.6 ms): median 10.5 ms a frame,
  of which the per-draw work is about 8-9 ms.
- Job threads: about 2,800 merge and 3,500 clear observer calls a frame. A clear costs 0.32 us low mode, 4.02 us high mode at the
  same call count: the high mode is waiting. Render-thread engine hooks: 0.42 us a call low, 2.14 us high, tracking the clear
  walks. The copier takes `g_mutex` about 2.7k times a frame; emissions run about 25 a second, copies about 70k.
Ranked causes (independent review): (1) EDVR's per-draw work on the render thread, about 2/3 of the delta, NOT touched here;
(2) `g_mutex` contention, about 2-3 ms on average: commits 1 and 2; (3) observer work lengthening Elite's job phase (H-obs
primary): UNPROVEN, and these commits cannot prove it.

Built: (1) `g_claimsLive` + `ClaimLock` (the only way the header locks `g_mutex`): a clear with no live claim skips the mutex, a
merge with none gets a fixed token (no plan, walk or allocation), the walk uses small-buffer sets. (2) The copier skips the mutex
for count 0, no lease, and one fitting lease when no claim, plan or pool patch is published (seqlock view `g_poolView`).
(3) `ClaimLock` times 1 acquisition in 64 per caller: the log gains two `engine motion: primary copy lock (sampled 1 in 64, ...)`
lines per 30 s, per caller and thread class, wait and hold, average/maximum us. Expected gain of (1)+(2): about 2-3 ms of 10-25.
What the lock lines would show: render-thread `copier`/`beginMap`/`endMap` wait near 0 and clear walks rare means the contention
was removed; a large render `wait` against a large `other` hold names the holder; all-zero n while `copier spans` grows means
the instrument did not run. Lock-free exits take no lock and are not in the lines.
Open: was the reporter fine on 0.19.0? The observer and flat-substitution files are byte-identical between v0.19.0 and v0.19.1 and
the always-on 0.19.1 additions all run in the 45 fps off state. Ask which `temporal_aa` mode and scene they ran on 0.19.0.
Flights: F8 off / TAA / DLSS every 30 s in one on-foot CZ (the delta on another CPU), then the same CZ on this build: lock lines,
`other threads` merge and clear ms, `nodes walked` (about 0 with 0 claims), fps against substituted draws.
NOT claimed: that the observers are on Elite's critical path; that this recovers the off/on delta; that any fps is gained.

## What changed

`src\d3d11\engine_velocity_primary_copy.h`: `g_claimsLive`, `ClaimLock`, `AddressSet`, `mergeNoClaimsToken`, `copierLockFree`,
`g_poolView`, `Caller`; `stats()` folds the lock-free counters into `clearCalls`, `clearNoClaims`, `mergeCalls`,
`mergeWithoutClaims`, `copies`, `copierNoLease`, so the logged figures keep their meaning. Merge nodes are no longer counted for a
no-claim merge, and `merge plans` counts full plans only. `engine_velocity.cpp` prints the lock lines after the observer-work line.
Unwind of the no-claim token revokes every claim registered since (the old empty plan spared unrelated later owners after a
clear or a frame change): safe direction, native unwinds only; two old checks now assert it. A malformed list with no claims no
longer bumps the epoch or counts a merge failure. The full plan keeps its precision.
Copier: every entry point that can change a lease, base, size or patch sets `g_poolsDirty`; `ClaimLock` rebuilds the view and opens
the seqlock window only if something differs. An odd or moved sequence takes the mutex. A skipped call is ordered before any
critical section still in flight, which is legal on one premise not proven here: the engine finishes an item's emit, and the Map that
returned its destination, before that item's copy starts. Equivalence tests run the pre-change copier beside it on 21 configurations.
Tests: `tools\engine_velocity_test\primary_copy_observer_tests.h`; `engine_velocity_test.exe --bench` is opt-in.

## Benchmark

ns per call on an in-process fake of Elite's lists, one machine, same run for both columns.

| case | before | after |
|---|---|---|
| merge begin+end, no claims, 1 node | 325 ns, 11 allocs | 5.9 ns, 0 |
| merge begin+end, no claims, 8 nodes | 966 ns, 24 allocs | 5.9 ns, 0 |
| clear, no claims, a no-claim merge in flight | 2495 ns, 80 allocs | 5.2 ns, 0 |
| clear walking 1 / 8 nodes, claims present | 421 / 2486 ns | 70 / 743 ns, 0 allocs |
| 4 threads, merge no claims (per thread) | 1427 ns | 36 ns |
| 4 threads, merge then clear, no claims | 2784 ns | 63 ns |
| 4 threads, merge then clear, claims present | 3766 ns | 1996 ns |
| copier, active pool, 1 slot | 16.8 ns | 5.5 ns |
| copier against 3 threads calling `invalidateEmission` | 80 ns | 5.5 ns |
| 4 threads, copier (per thread) | 116 ns | 32 ns |
| `invalidateEmission`, lock + failed erase | 14.1 ns | 16.2 ns (the instrument) |

Rows 1-7 are commit 1 against the baseline, the copier rows commit 2 against commit 1, the last row commit 3 against commit 2: the
unsampled lock costs one thread-local increment, about 2 ns. The 4-thread no-claim figures are bounded by shared counters;
striping them was not worth the code.
