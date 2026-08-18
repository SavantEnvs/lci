# Guaranteed-hang seed: `IM IN YR main` never sees a GIMMEH sentinel

**Where**: `repro.lol`'s outer instruction loop, `IM IN YR main` (line 285) — a Brainfuck
interpreter written in LOLCODE. Each iteration does `GIMMEH instruction` (line 287) to read the
next BF opcode, then `executin`s it, and only `GTFO`s (breaks) when `executin` reports failure
(lines 289-291). `executin`'s own `IM IN YR main` has no other exit path.

**Cause**: this harness (`mayhem/fuzz_lci.c`) redirects stdin to `/dev/null` before every input, so
`GIMMEH`'s underlying `getchar()`/`feof(stdin)` loop (`interpreter.c`'s
`interpretInputStmtNode`) observes immediate EOF and yields an empty `YARN` on every call — it
never blocks. But `repro.lol`'s own loop logic never treats an empty/EOF read as a stop condition;
`executin` on an empty instruction is not one of the recognized failure opcodes, so the loop simply
calls `GIMMEH` again, forever. This is a **genuine native infinite loop** (`interpretMainNodeScope.c`'s
`interpretLoopStmtNode` is a bare `while (1) { ... }` with no iteration cap), not a blocked read —
belt-and-suspenders stdin redirection does not help here because the program's own logic, not the
I/O call, is what fails to terminate.

**Verified**: with the process-level watchdog (`mayhem_arm_deadline`/`setitimer(ITIMER_REAL, 1.5s)`
in `fuzz_lci.c`) disabled, this input hangs `interpretMainNodeScope()` indefinitely. With it
enabled, the harness process reliably exits via `_exit(70)` at ~1.5s wall time. This is expected,
by-design LOLCODE behavior (the language permits non-terminating loops), not a memory-safety
defect — so it is a harness-bounding concern, not a reported finding.

**Why this lives here and not in `mayhem/lci/testsuite/`**: `testsuite/` is replayed on *every*
Mayhem run, including the initial `-runs=5` single-process sanity probe Mayhem performs before
accepting a target as fuzzable. Because this input's hang is 100% deterministic, shipping it as a
seed meant every single run — local smoke test and Mayhem cloud run alike — hit the watchdog's
`_exit(70)` within the first few iterations and killed the whole process before the sanity probe
could complete, which Mayhem reports as "libFuzzer target failed to fuzz for 5 iterations" /
`tests_run=0, edges_covered=0`. Reproduced directly:

```
$ docker run --rm lci-commit:dbg /mayhem/build/lci -runs=1 /mayhem/mayhem/lci/testsuite/seed-5.lol   # (former path)
...
mayhem: hard watchdog deadline exceeded (unbounded IM IN YR loop, or a blocked native call), aborting
$ echo $?
70
```

Moving it here (out of the replayed corpus) fixes exactly that: the remaining 11 `testsuite/` seeds
all exit 0 under `-runs=1`, and `-runs=5`/`-runs=100` over `testsuite/` now complete normally. The
watchdog itself is still very much in effect and still correctly bounds a *fuzzer-discovered*
unbounded loop mid-campaign (that is the scenario it exists for) — the fix here is only about what
the initial, deterministic sanity/seed corpus is allowed to contain.
