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

**Verified**: with the interpreter step budget (`MAYHEM_STMT_BUDGET`, injected into a build-time
shadow copy of `interpreter.c` by `mayhem/build.sh`) disabled, this input hangs
`interpretMainNodeScope()` indefinitely. With it enabled, `interpretStmtNode()` returns `NULL`
once the budget is exhausted (this loop dispatches `interpretInputStmtNode` via `interpretStmtNode`
every iteration, so the budget fires), `interpretMainNodeScope()` unwinds normally, and
`LLVMFuzzerTestOneInput()` returns 0 — no process exit. This is expected, by-design LOLCODE behavior
(the language permits non-terminating loops), not a memory-safety defect — so it is a
harness-bounding concern, not a reported finding. Per PORTING.md, this harness does not install a
process-level timer/watchdog of its own; a hang the step budget can't see is caught by the
Mayhemfile's `timeout: 30` instead.

**Why this lives here and not in `mayhem/lci/testsuite/`**: `testsuite/` is replayed on *every*
Mayhem run, including the initial `-runs=5` single-process sanity probe Mayhem performs before
accepting a target as fuzzable. Before the step budget existed, this input's hang was 100%
deterministic, so shipping it as a seed meant every single run — local smoke test and Mayhem cloud
run alike — hung the sanity probe, which Mayhem reports as "libFuzzer target failed to fuzz for 5
iterations" / `tests_run=0, edges_covered=0`. The step budget above fixes that: the remaining 11
`testsuite/` seeds all exit 0 under `-runs=1`, and `-runs=5`/`-runs=100` over `testsuite/` now
complete normally, including this one. It stays out of `testsuite/` anyway (as a documented
known-finding instead) since it exists specifically to demonstrate the hang class, not to add
coverage.
