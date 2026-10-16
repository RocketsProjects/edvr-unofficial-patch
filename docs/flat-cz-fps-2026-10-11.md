# Flat on-foot CZ frame rate (report of 2026-10-11)

## Status

State (2026-10-16): the observer reduction below is PORTED onto current main as `claude/flat-settlement-cpu` (7 commits, clean
cherry-pick, no ADS work), with four more instruments for the settlement CPU cost (H1-H4, section "2026-10-16", the flight plan is
there). BUILT, NOT FLOWN, NOT MERGED. Question for the flight: which of the 5.8 ms a frame (0.52 in the cockpit; draws 360 -> 10.5k)
is the thunk's remainder, the constructor's lookup or predicate block, the destructor, g_mutex contention, or the wrapper's guarded
eager runs. Flight log: `edvr_gfx_20261016_023749.log` (flat, Steam, 2026-10-16) is the diagnosis; no flight on this build yet.
Ruled out so far: nothing new (the 2026-10-16 log only ranked the families); the 2026-10-11 notes below stand.
Earlier state: BUILT on `claude/fps-drops-onfoot-czs-577f5a` (observers, copier, lock lines, flat cpu by part, then the review's fixes), NOT
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


## 2026-10-16: the settlement CPU arc (flat, on foot at a settlement)

Diagnosis, from the flat log `edvr_gfx_20261016_023749.log` (Steam, flat, DLSS): EDVR's render-thread CPU at a settlement is about 5.8 ms
a frame net against 0.52 in the cockpit; draws went about 29x (360 to 10.5k a frame). The `flat cpu 5s` buckets are exclusive-time
families. "other" was the draw hook's constructor shell, its destructor shell and the dispatch shell, about 117 ns net a call, two
calls a draw. The engine motion render-thread hooks were the one superlinear bucket (66 to 445 ns a call); the job-thread clear and
merge share one `std::mutex`, and 84-86% of their calls had no claims yet built a plan, a set and a vector anyway.

Branch `claude/flat-settlement-cpu` (from origin/main cbff0142). Two parts.

### Part 1: the fix, ported (no new code)

The observer reduction was reviewed and built on 2026-10-11 and never landed. It is ported by `git cherry-pick` of its seven commits
(457ce3a8 and its six parents on `claude/fps-drops-onfoot-czs-577f5a`; they apply cleanly, touch no ADS-naming-veto file):
c086b5d1 (merge and clear observers skip the mutex, the walk and the heap when no claim is live), f75840d9 (the pool copier's quiet
cases skip the mutex), 9d6a9b3e (lock timing per caller and thread class), d4739533 (`flat cpu 5s` splits the render thread's engine
hooks by part: this is H3's per-part print), 5c18371c (lock lines file the render thread under "render" in flat), 178d7c52 (plans that
hold nothing no longer keep the fast paths locked; the lock-free share is printed), 457ce3a8 (the doc). The reviewer's concerns, from
the Status block above, are what 5c18371c and 178d7c52 fixed: the lock-line render/other mislabel in flat, and empty merge plans
keeping the fast paths off. Both are in the port. Open as before: the observers are NOT proven to be on Elite's critical path.

Rig evidence (`tools\engine_velocity_test`, `primary_copy_observer_tests.h`; the rig exits at its first failing check, so each
mutant below stops at one): fail-before, by mutating the shipped header and compiling the rig against it (mutants run by hand,
sources restored, hashes compared):
- clear's lock-free exit off: `FAIL: plans: afterwards a merge, a clear and a copy all take the lock-free paths`
- merge's lock-free exit off, or the copier's: `FAIL: plans: while that plan is in flight a new merge and a copy both skip the mutex,
  and no second plan is registered` (those checks hold the mutex on another thread: a call that takes it cannot complete)
- the exits AND the locked stale-flag exits off (the old behaviour: a walk, a plan, allocations): `FAIL: primary observer: no-claim clear
  bypass avoids the native traversal; neither it nor the full one-node walk allocates`
Claimed paths are unchanged (the equivalence tests run the pre-change walk and copier beside the new ones), and a claim racing a
no-claim exit is never lost: every lock-free exit reads a flag each critical section publishes before it unlocks (see "What changed").

### Part 2: instruments (no behaviour change, no config key)

Hypotheses, each with the line that confirms or refutes it:
- H1 the constructor shell is the cost, in its lookup block (views, `flatContractKind` calls, pool-family pair, tone slots) or its
  predicate block (weapon, overlay, untrusted nomination). Confirm: `ctor lookup block` or `ctor predicate block` ms per frame holds
  most of the old "other". Refute: `draw ctor shell` (the remainder) or `draw dtor shell` holds it.
- H2 EDVR's per-draw cost is spread over scopes the families do not name, or the game's own draw is slower for EDVR's state. Confirm:
  `draw hook remainder` (the thunk's unscoped time) is large, or `game draw after substituted` per call is well above `game draw
  other`. Refute: the families add up to `EDVR hook cost` with a remainder under 0.1 ms and the three game draws cost the same a call.
- H3 `g_mutex` contention between the render thread and the job threads. Confirm: `lock contention` shows render or other counts
  that are a double-digit share of the estimated acquisitions, or the render thread's hooks by part (`clear`, `merge`) track the
  contended counts. Refute: contended is a few per window while hooks by part still cost 66-445 ns a call (then it is the work, not
  the wait).
- H4 the overlay guard (shader pairs BBE/DB3E) forces eager runs that cost many D3D calls. Confirm: `eager guarded` runs a frame is
  non-trivial and its calls a run and microseconds a run are well above `lazy`. Refute: guarded runs are a few a frame.

Scopes (one per site, none per draw logged): `flat_cpu.h` splits kOther into `draw ctor shell` (kDrawCtor), `draw dtor shell`
(kDrawDtor) and `dispatch shell` (kDispatchShell), with `ctor lookup block` and `ctor predicate block` inside the constructor
(Scope::stop() ends them mid-function; locals outlive them). `draw hook remainder` is a scope around the whole flat branch of the five
draw thunks in vscreen.cpp (Draw, DrawAuto, DrawIndexed, DrawInstanced, DrawIndexedInstanced), so it is the thunk's own time less
everything nested, and the Cell now also keeps INCLUSIVE wall time for it. The game's draw call is timed in three families by what it
met: `game draw substituted` (the draw is a substituted producer), `game draw after substituted` (the previous watched draw was
one: the driver meets state engine motion put back or left bound), `game draw other`. All use the census gate, so they are clocked on
the same one frame in 16 as the rest; a scope's whole wall time is subtracted from its parent exactly once, and the game's draw
families are printed but kept OUT of "EDVR per frame total" and out of the other threads' "every other family".

COMPARABILITY: "EDVR per frame total" on this build includes the new `draw hook remainder` (EDVR time the old total did not see) and
nothing of the game's draw. "other" in older logs = `draw ctor shell` + `ctor lookup block` + `ctor predicate block` + `draw dtor shell`
+ `dispatch shell` here. The line is now up to 6 lines of 1000 characters at the worst plausible values (kMaxLines 7, kMaxTokens 96).

Line formats (examples from the rig's fake data; real lines are single lines):
- families, in `flat cpu 5s:` as before, the new names: `... + draw dtor shell 0.400 ms (calls 10500) + dispatch shell 0.020 ms
  (calls 90) + ctor lookup block 0.700 ms (calls 10500) + ctor predicate block 0.600 ms (calls 10500) + draw hook remainder 0.500 ms
  (calls 10500) + game draw substituted 1.100 ms (calls 2400) + game draw after substituted 0.500 ms (calls 700) + game draw other 2.000
  ms (calls 7400) + camera inject ...`
- H2: `; draw hook flat branch (render thread, per clocked frame): wall 8.300 ms over 10500 draws, game draw 3.600 ms, EDVR hook cost 4.700
  ms (unscoped remainder 0.500 ms); game draw per call: substituted 0.46 us a call (2400 a frame), after substituted 0.71 us a call
  (700 a frame), other 0.27 us a call (7400 a frame)`. "-" when a family has no call, never 0.00.
- H3 per part (from d4739533, last on the line): `; engine motion hooks by part (render thread, per clocked frame): emit 0.000 ms (calls
  0.0), ... merge 0.080 ms (calls 900), clear 0.120 ms (calls 2600), ...`; contention is a third lock line every 30 s: `engine motion:
  primary copy lock contention (30 s; acquisitions that found the mutex held, per thread class, of ~acquisitions): ... clear walk:
  render 0 of ~0, other 41 of ~7680 | copier: render 7 of ~19200, other 0 of ~0 | ...`. Every acquisition tries `try_lock` first
  and counts a failure before it blocks (exact); "of ~N" is the 1-in-64 timed sample times 64.
- H4: `; engine motion runs (the wrapper's brackets, a frame; the time is the clocked frames'): lazy 2380.0 runs/frame (4.0 D3D calls/run,
  0.46 us/run, 1.100 ms per clocked frame), eager guarded (overlay t3) 20.0 runs/frame (24.0 D3D calls/run, 15.00 us/run, 0.300 ms per
  clocked frame), eager capture armed 0.0 runs/frame (- D3D calls/run, - us/run, 0.000 ms per clocked frame), lazy flushes 500.0
  flushes/frame (11.0 D3D calls/flush, 4.40 us/flush, 2.200 ms per clocked frame)`. A run is one substituted draw; its D3D calls are
  BeginDraw's plus, for an eager run, its restore; a lazy run's restore is paid by whichever event flushes it (`lazy flushes`).

If the code never ran: every family and the run line print as zeros (never absent), the per-call times print "-", and the run counts
beside `engine motion wrapper D3D calls .../frame over N substituted draws/frame` disagree: runs summing to nothing while that N is
large means the counting is dead. Contention all zeros beside lock lines with a large n means nothing met on the lock, OR (check the
version line) the build is stale. Rigs: `flat_cpu_tests.h` (stop(), wall, the thunk line, game draw per call, empty-window dashes, the
runs line), `flat_temporal_test.cpp` wiring pins, `flat_lazy_tests.h` (runs by kind against the overlay sequence, the capture-off
case and the census line), `primary_copy_observer_tests.h` (contention counted while the caller is still blocked, per thread class,
exact text, worst-case length). Mutants caught: contention counting removed -> `FAIL: contention: a call that blocks on another thread
is counted once, under other, while it is still blocked`; run counting removed -> `FAIL: flat lazy: with the lazy form off every run of
the overlay sequence is filed as an eager capture run, with its D3D calls`.

Flight plan (flat, Steam or Epic install, one on-foot settlement, 2 min; `python tools\install_edvr.py --target steam --scope flat`, then
`--verify-only`, then `python tools\edvr_log.py --target steam --expect-build HEAD` first):
1. DLSS on: read `draw hook flat branch`, `draw hook remainder`, `ctor lookup block`, `ctor predicate block`, `draw dtor
   shell`, the by-part hooks, the contention line and the runs line. The per-draw cost is what H1/H2 rank; one flight eliminates several.
2. A/B: F8 `fix.temporal_aa` off and back (the census runs only while a temporal mode is selected; the thunk scopes then cost a gate
   load and a compare). For a true no-EDVR baseline see below.
3. Compare the lock-free share (`primary copy lock-free exits`, expect over 90%) against the 2026-10-16 log's 84-86% no-claim calls.

`install_edvr.py --suspend` exists (`--target <dir|steam|epic|frontier>`, `--dry-run` to preview). It renames the game directory's
d3d11.dll to d3d11.dll.edvr-suspended (one rename, hashed before and after; only an EDVR build is accepted) and `--resume` puts it back.
It refuses unless the game is PROVEN stopped, and the DLL is loaded at launch, so each leg of an A/B needs a game restart: suspend,
launch, fly, quit, resume. Settings, receipts and backups stay. It takes the graphics proxy only, so a d3d11 chain behind it (Sean's
Epic `real_dll = d3d11_edhm.dll`, 3Dmigoto) is out of the run too: Elite loads Windows' d3d11.dll.