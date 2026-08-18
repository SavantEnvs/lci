# Heap-buffer-overflow: type confusion in `cachedLookup`'s not-found diagnostic

**Where**: `vm.c`, `cachedLookup(Proto *p, int k, ScopeObject *from)`. Its "name not found in
any enclosing scope" fallback:
```c
IdentifierNode *id = (IdentifierNode *)p->kptrs[k];
error(IN_VARIABLE_DOES_NOT_EXIST, id ? id->fname : NULL, id ? id->line : 0, name->str);
```
casts `p->kptrs[k]` straight to `IdentifierNode *` and reads `->fname`/`->line`. That is correct
when `cachedLookup()` is reached from a plain variable/`OPC_GETVAR` lookup (`kptrs[k]` really is
an `IdentifierNode *` there) — but `callByName()` (in `vm.c`) reuses this *same* helper for a
**function-call** lookup, where `kptrs[k]` is actually a `FuncCallExprNode *`
(see `callByName`'s own `(FuncCallExprNode *)p->kptrs[k])->name`). Once a call names a function
that plain does not exist, this fallback fires and reinterprets the smaller `FuncCallExprNode`
allocation as the (differently laid out, generally larger) `IdentifierNode` struct — reading
`->line` past the end of the real allocation.

**Reproducer**:
```
HAI 1
I IZ fun YR SUM OF 1 AN 2.345 MKAY
KTHXBYE
```
`fun` is never defined. Confirmed under ASan on the harness binary
(`/mayhem/build/lci repro.lol`):
```
==ERROR: AddressSanitizer: heap-buffer-overflow ... READ of size 4 ...
    #0 cachedLookup vm.c:1716:14
    #1 callByName vm.c:2262:21
    #2 run vm.c
    #3 callProto vm.c
    #4 interpretMainNodeScope interpreter.c
0x...568 is located 0 bytes after a 24-byte region ... allocated by ... createFuncCallExprNode parser.c:1376
```
Reachable by calling **any** undefined function — about as shallow as a bug gets.

**Impact**: heap-buffer-overflow read, ASan-fatal; on a non-sanitized build, silent
out-of-bounds read whose garbage bytes end up in an error message's `fname`/`line` fields.

**Fix**: report the same error without assuming which struct `kptrs[k]` actually is — use
`NULL`/`0` for `fname`/`line` (the message body, `name->str`, is unaffected and always valid
regardless of caller context) rather than casting and dereferencing a pointer whose true type
depends on which of `cachedLookup()`'s two callers reached this path.

**Pre-existing since the bytecode VM merge**: `cachedLookup()`, `callByName()`, and the whole
bytecode-VM call path are new in upstream's "Add a bytecode machine and a native code
generator" commit — this is a genuine defect introduced by that merge, not something this
integration's original base ever exercised (the old base had no VM to reuse this helper from
two differently-typed call sites).

**How this was found**: `mayhem/build/lci -dict=... -max_total_time=20` (a ~20s local mutation
smoke run over the seed corpus) while validating this integration's sync onto current
upstream `future`.
