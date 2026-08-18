# NULL deref on a malformed/indirect loop-variable identifier in `parseLoopStmtNode`

**Root cause** (the actual bug — see `mayhem/build.sh`'s `IDABORT_OLD`/`IDABORT_NEW` patch):
`parser.c`'s `parseIdentifierNode()` has a catch-all `else` branch (neither a direct
`TT_IDENTIFIER` nor an indirect `SRS <expr>` matched) that calls `parser_error(...)` but,
unlike every other error path in that function, does **not** `goto parseIdentifierNodeAbort;`
afterward. `type`/`data` are then left holding whatever garbage already occupied those stack
locals, and execution falls through to
`ret = createIdentifierNode(type, data, slot, fname, line)` — building a fully-formed
`IdentifierNode` out of uninitialized memory instead of failing.

**Where this surfaces**: `parseLoopStmtNode()`'s increment/decrement loop variable
(`IM IN YR loop UPPIN YR <var>`) and its "for function loops" variable (`I IZ func YR <arg>
MKAY`) both call `parseIdentifierNode()` and immediately treat the result as `IT_DIRECT`
(`strlen(var->id)` / `strcpy(id, var->id)`), with no check — unlike the loop's own *name*
(`name1`/`name2`, checked a few lines away with
`if (name->type != IT_DIRECT) { parser_error(...); goto ...Abort; }`). Once
`parseIdentifierNode()` fails to parse an identifier at all (e.g. the token after `UPPIN YR`
is a number, not an identifier), the uninitialized `type` can coincidentally read back as
`IT_DIRECT` while `data` (equally uninitialized) is coincidentally `NULL` — exactly what a
mutation-fuzzing run reproduced.

**Reproducer** (hand-verified, deterministic):
```
HAI 1.3
IM IN YR loop UPPIN YR 5
KTHXBYE
```
Confirmed under UBSan on the harness binary (`/mayhem/build/lci repro.lol`):
```
parser.c:3561:38: runtime error: null pointer passed as argument 1, which is declared to never be null
/usr/include/string.h:408:33: note: nonnull attribute specified here
SUMMARY: UndefinedBehaviorSanitizer: undefined-behavior parser.c:3561:38 in parseLoopStmtNode
```
Reachable straight through `parseMainNode()` on ordinary (if malformed) LOLCODE source — not a
harness artifact.

**Fix, two parts**:
1. (Root cause) `parseIdentifierNode()`'s catch-all `else` branch now `goto
   parseIdentifierNodeAbort;`s after reporting the error, matching this function's own
   convention everywhere else — so it can never again hand back a garbage-typed node.
2. (Defense in depth, mirrors the existing `name1`/`name2` pattern) both loop-variable sites
   also now explicitly reject a non-`IT_DIRECT` identifier with
   `parser_error(PR_EXPECTED_IDENTIFIER, tokens)` before `strlen`/`strcpy` — so a
   *legitimately* parsed indirect identifier (`SRS <expr>`, whose `id` is a real but
   non-string `ExprNode *`) in the loop-variable position is rejected cleanly instead of
   being read as a string, rather than relying solely on fix (1).

**How this was found**: `mayhem/build/lci -dict=... -max_total_time=20` (a ~20s local mutation
smoke run over the seed corpus) while validating this integration's sync onto current
upstream `future`; minimized by hand from the fuzzer's mutated crashing input (which had NUL
bytes injected in place of the loop-variable token) down to the 3-line repro above.

**Pre-existing, not a sync regression**: this code (`parseIdentifierNode`, the loop-variable
`strlen`/`strcpy` sites) is byte-for-byte unchanged between this integration's original base
commit and current upstream `future` — the bug has always been there, just never previously
found/documented.
