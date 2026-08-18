# Heap use-after-free / double-free in `parseLoopStmtNode`'s error-cleanup path

**Where**: `parser.c`, `parseLoopStmtNode()`. For an increment/decrement loop header
(`IM IN YR <loop> UPPIN YR <var>` / `NERFIN YR <var>`), the function builds the loop's
update expression as:

```c
op = createOpExprNode(type, args);          /* parser.c:3582 */
if (!op) goto parseLoopStmtNodeAbort;
update = createExprNode(ET_OP, op);         /* parser.c:3586 */
if (!update) goto parseLoopStmtNodeAbort;
```

`createExprNode(ET_OP, op)` takes ownership of `op` (it becomes `update->expr`) — but unlike
every other ownership transfer in this same function (e.g. `arg1 = NULL;` right after
`addExprNode(args, arg1)` succeeds, `arg2 = NULL;` right after `addExprNode(args, arg2)`
succeeds), `op` (and `args`, which is owned by `op`) is never set to `NULL` afterward.

If **any** later parse step in this function fails — e.g. the loop header isn't immediately
followed by a newline, or a `WILE`/`TIL` predicate fails to parse — control jumps to
`parseLoopStmtNodeAbort`, whose cleanup does, unconditionally:

```c
if (update) deleteExprNode(update);   /* parser.c:3766 -- recursively frees op, args, ... */
...
if (op) deleteOpExprNode(op);         /* parser.c:3772 -- op is a dangling pointer: freed again */
if (args) deleteExprNodeList(args);   /* parser.c:3773 -- args is also already freed */
```

`update` was already set (non-NULL) when the later step failed, so `deleteExprNode(update)`
runs first and frees `op` (and transitively `args`) as part of tearing down `update`. The stale
`op` and `args` pointers are then still non-NULL locals, so the two `if` checks below pass and
free (`args`)/use-after-free-read (`op`, inside `deleteOpExprNode`) the same memory again.

**Reproducer** (`repro.lol`, 3 lines, no mutation/fuzzing needed):

```
HAI 1.3
IM IN YR loop UPPIN YR var BOGUS
KTHXBYE
```

`UPPIN YR var` parses fully and builds `update` (wrapping `op`/`args`) successfully. The parser
then expects a newline (optionally preceded by `WILE`/`TIL`); the trailing `BOGUS` token is
neither, so `acceptToken(&tokens, TT_NEWLINE)` fails and the function aborts through the path
above. Confirmed under ASan on the harness binary (`/mayhem/build/lci -runs=1 repro.lol`):

```
==ERROR: AddressSanitizer: heap-use-after-free on address ... in thread T0
READ of size 8 ... in deleteOpExprNode parser.c:1467:27
    #1 parseLoopStmtNode parser.c:3772:11
    #2 parseStmtNode parser.c:4267:9
    #3 parseBlockNode parser.c:4360:10
    #4 parseMainNode parser.c:4433:10
    #5 LLVMFuzzerTestOneInput mayhem/fuzz_lci.c:215
freed by thread T0 here:
    #0 free
    #1 deleteExprNode parser.c
    #2 parseLoopStmtNode parser.c:3766:15
    ...
previously allocated by thread T0 here:
    ... createOpExprNode parser.c:3582 ...
```

**Not a harness artifact**: this is a pure front-end (lexer/tokenizer/parser) defect, reached by
`parseMainNode()` before `interpretMainNodeScope()` ever runs — nothing about the fuzz harness's
stubbed STDIO/SOCKS bindings or its interpreter step budget is involved. Any caller of
`lci`'s parser (including the plain CLI, `lci <file>`) hits this on the same malformed input.

**Impact**: a heap use-after-free (ASan-reported; a plain/non-sanitized build would silently read
freed heap memory, and — since `args`'s underlying `ExprNodeList` storage is also double-freed —
potentially corrupt the allocator) triggered by a syntactically-invalid but trivially-crafted
LOLCODE source file. Memory-safety defect, not an interpreted-language "expected non-terminating
behavior" like the two findings above it in this directory.

**One-line upstream fix**: set `op = NULL;` immediately after `update = createExprNode(ET_OP, op);`
succeeds (mirroring the `arg1 = NULL;`/`arg2 = NULL;` pattern already used earlier in this same
function), so the abort path's `if (op) deleteOpExprNode(op);` becomes a no-op once ownership has
moved to `update`. (`args` becomes unreachable the same way once `op` owns it, so nulling `op`
alone is not quite sufficient by itself if `createOpExprNode` fails to take `args` in some other
path — but for this specific reachable defect, nulling `op` right after the successful
`createExprNode(ET_OP, op)` call is the minimal fix.)

**How this was found**: `mayhem/build/lci -runs=200 mayhem/lci/testsuite` (letting libFuzzer mutate
the seed corpus, not just replay it) hit this within the first ~90 mutated inputs while validating
the interpreter step-budget fix (see `mayhem/lci/known-findings/gimmeh-eof-unbounded-loop/`) — i.e.
this is a real bug the harness is now capable of finding once interpretation-phase hangs no longer
kill the whole fuzzing process before mutation gets this far. Minimized by hand from the fuzzer's
mutated crashing input (which had NUL bytes injected into a `BOTH SAEM` token) down to the 3-line
repro above once the abort-path double-free was understood.
