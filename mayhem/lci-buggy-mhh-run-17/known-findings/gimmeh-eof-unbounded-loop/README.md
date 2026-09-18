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

**Verified**: this input hangs `interpretMainNodeScope()` indefinitely — it never returns. The
harness (`mayhem/fuzz_lci.c`) installs no timer, alarm or watchdog of its own
(docs/netnew-worker-prompt.md §6b: "Hangs are findings — Mayhem owns timeouts, the harness never
does"); the only bound on this exec is the Mayhemfile's cmd-level `timeout: 30`, under which
libFuzzer reports it as an ordinary timeout. This is expected, by-design LOLCODE behavior (the
language permits non-terminating loops), not a memory-safety defect — so it is a harness-bounding
concern here, not something reported as a distinct finding.

**Why this lives here and not in `mayhem/lci-buggy-mhh-run-17/testsuite/`**: `testsuite/` is
replayed on *every* Mayhem run, including the initial `-runs=5` single-process sanity probe Mayhem
performs before accepting a target as fuzzable. Because this input's hang is 100% deterministic,
shipping it as a seed means every single run — local smoke test and Mayhem cloud run alike — would
need the full `timeout: 30` to elapse within the first few sanity-probe iterations, which stalls or
fails that probe outright. (An earlier revision of this harness carried a process-level watchdog
that `_exit(70)`d on this exact seed instead, which is worse, not better: two cloud runs came back
at `tests_run=0, edges_covered=0` / "libFuzzer target failed to fuzz for 5 iterations" before the
seed was identified and moved here — a hard-exiting harness is a harness Mayhem cannot probe at
all, whereas an ordinary timeout is recoverable. That watchdog has since been removed; see
`mayhem/fuzz_lci.c`'s header comment.)

Keeping this reproducer out of the replayed corpus avoids the deterministic hang entirely: the
remaining seeds all exit 0 under `-runs=1`, and `-runs=5`/`-runs=100` over `testsuite/` complete
normally. A *fuzzer-discovered* unbounded loop mid-campaign is still possible and is fine — it
costs one `timeout: 30` exec and is recorded like any other finding, exactly per §6b.
