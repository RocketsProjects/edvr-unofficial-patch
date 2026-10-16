# Flat on-foot CZ frame rate (report of 2026-10-11)

## Status

State: BUILT on `claude/fps-drops-onfoot-czs-577f5a` (observers, copier, lock lines, flat cpu by part, then the review's fixes), NOT
FLOWN. A bounded experiment on one of three suspects, plus instruments that say whether it was the right one. NOT the fix: the
expected gain is about 4-5 ms of a 13-25 ms gap at most, and the per-draw render-thread work (about 8-9 ms a frame) is untouched.
Report: flat v0.19.1 (Steam, build 6ACAF769), RTX 3050, "40 fps without the mod, 20-25 with DLSS" in on-foot CZs.
Working files (gitignored): `reviews\flat-cz-fps-findings-2026-10-11.md`, `reviews\flat-cz-fps-review-2026-10-11.md`.

Evidence, from the reporter's log `edvr_gfx_20261011_184408.log`:
- In-CZ A/B: `fix.temporal_aa` toggled through F8, off at 18:58:43, 19:01:52, 19:13:28. vScreen 20 s windows: DLSS 21 fps
  (19:01:49), off 45 and 46 (19:02:29, 19:02:49), TAA 27 (19:03:29). Off is 1.5-2x faster each time: the temporal path costs
  10-25 ms a frame, and TAA is as slow as DLSS, so it is CPU, not the NGX resolve. 810p was no faster than 1080p (48 s of 810p).
- Heavy windows (over 1500 substituted draws a frame): Present minus GPU span 13.7 ms mean (median 13.9), 1.0 ms in light ones.
  The span runs from the first game draw to Present on the GPU timeline; it is not GPU busy time.
- EDVR's render-thread time, clock inflation removed (printed in the `flat cpu` line, about 1.6 ms): median 10.5 ms a frame,
  of which the per-draw work is about 8-9 ms.
- Job threads: about 2,500 merge (median) and 3,500 clear observer calls a frame. A clear costs 0.32 us low mode, 4.02 us high mode
  at the same call count: the high mode is waiting. Render-thread engine hooks: 0.42 us a call low, 2.14 us high, tracking the clear
  walks. The copier takes `g_mutex` about 2.7k times a frame; emissions run about 25 a second, copies about 70k.
Ranked causes (independent review): (1) EDVR's per-draw work on the render thread, about 2/3 of the delta, NOT touched here;
(2) `g_mutex` contention, about 2-3 ms on average, and only if the plan fix below holds; (3) observer work lengthening Elite's job
phase (H-obs primary): UNPROVEN, and these commits cannot prove it.

Built: `g_claimsLive` (emissions or plans) and `g_emissionsLive` (emissions) published by `ClaimLock`, the only way the header locks
`g_mutex`. A clear with no claim or plan skips the mutex. A merge with no emission gets a fixed token (no plan, walk or allocation),
under the lock too: a plan that holds nothing used to keep every fast path off. The copier skips the mutex for count 0, no lease, and
one fitting lease when no emission and no pool patch is published (seqlock view `g_poolView`). The clear walk uses small-buffer sets.
`ClaimLock` times 1 acquisition in 64 per caller (render thread from `emcpu::g_renderThreadId`, which both censuses publish every
frame; the gate cannot say, flat writes nobody into it on 15 frames in 16). The `flat cpu 5s` line ends with the render thread's
engine hooks by part. NOT built: that line while AA is off (its GPU span needs the draw scope, a device and an owner thread the
off state never creates: over 40 lines, and it touches the draw path).
The flight reads, per 30 s: the version line; `primary copy lock-free exits` (expect over 90% of calls); clear `nodes walked` (well under
100k, against 0.3-1.1M in this log); the lock lines (render copier, beginMap, endMap wait under 1 us average; a large render `wait`
beside a large `other` hold names the holder; all-zero n while `copier spans` grows means the instrument did not run); `flat cpu`
render hooks and the copier part under 0.3 ms in every window; then F8 off / TAA / DLSS, 30 s each, 3 cycles in one CZ.
Limits: the copier's patch flag is global. After a pool's first join in a frame (about one a frame here) every copy takes the
mutex until that pool's next Map, so expect perhaps half the copier gain. A rig cannot kill a weakened memory order (x86 hides it).
Open: was the reporter fine on 0.19.0? The observer and flat-substitution files are byte-identical between v0.19.0 and v0.19.1 and
the always-on 0.19.1 additions all run in the 45 fps off state. Ask which `temporal_aa` mode and scene they ran on 0.19.0.
NOT claimed: that the observers are on Elite's critical path; that this recovers the off/on delta; that any fps is gained.

## What changed

`src\d3d11\engine_velocity_primary_copy.h`: `g_claimsLive`, `g_emissionsLive`, `ClaimLock`, `AddressSet`, `mergeNoClaimsToken`,
`copierLockFree`, `g_poolView`, `Caller`, `formatLockLine`, `formatExitLine`. `stats()` folds the lock-free counters into
`clearCalls`, `clearNoClaims`, `mergeCalls`, `mergeWithoutClaims`, `copies`, `copierNoLease` and also reports them apart
(`clearLockFree` and the rest); the new `lock-free exits` line prints them. So "no-claim skips" on the observer-work line mixes
lock-free exits with locked skips: read the exits line for the split. `merge plans` counts full plans only, in VR logs too.
Merge nodes are not counted for a no-claim merge. The token's unwind revokes every claim registered since (the old empty plan spared
unrelated later owners after a clear or a frame change): safe direction, native unwinds only. A malformed list with no emission no
longer bumps the epoch, so it no longer fails a claim-carrying plan in flight: that plan validated its own list.
Safety: every lock-free exit reads a flag that each critical section publishes exactly before it unlocks, so a stale read linearises
the call before an in-flight critical section, which the old mutex allowed too. The engine's own ordering (an emit and a Map before the
copy of that item) only decides how often a join happens, not whether the result is right. Equivalence tests run the pre-change
walk and copier beside the new ones. Plans do not read each other, and a plan's staged claims are not in `g_emissions`, which is why
a merge and a copy may ignore plans while a clear may not (it must mark an in-flight plan invalid). Tests:
`tools\engine_velocity_test\primary_copy_observer_tests.h`; `engine_velocity_test.exe --bench` is opt-in.

## Benchmark

ns per call on an in-process fake of Elite's lists, one machine, same run for both columns.

| case | before | after |
|---|---|---|
| merge begin+end, no claims, 1 node | 325 ns, 11 allocs | 5.9 ns, 0 |
| merge begin+end, no claims, 8 nodes | 966 ns, 24 allocs | 6.0 ns, 0 |
| clear, no claims, a no-claim merge in flight | 2495 ns, 80 allocs | 5.2 ns, 0 |
| clear walking 1 / 8 nodes, claims present | 421 / 2486 ns | 63 / 836 ns, 0 allocs |
| 4 threads, merge then clear, no claims | 2784 ns | 65 ns |
| 4 threads, merge then clear, claims present | 3766 ns | 1518-1882 ns |
| copier, active pool, 1 slot | 16.8 ns | 5.5 ns |
| copier against 3 threads calling `invalidateEmission` | 80 ns | 5.6 ns |
| 4 threads, copier (per thread) | 116 ns | 33 ns |
| merge beside an open plan that holds nothing | 268 ns, 8 allocs | 5.9 ns, 0 |
| copier beside an open plan that holds nothing | 20.8 ns | 5.5 ns |
| `invalidateEmission`, lock + failed erase | 14.1 ns | 16.6 ns (the instrument) |

The first rows are the observer commit against the baseline, the copier rows against the observer commit, the plan rows against
the lock-line commit, the last row the instrument's price (one thread-local increment per locked call, about 2 ns). The 4-thread
no-claim figures are bounded by shared counters; striping them was not worth the code.
