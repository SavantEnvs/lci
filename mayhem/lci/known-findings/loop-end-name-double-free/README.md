# Heap use-after-free / double-free freeing the end-of-loop name in `parseLoopStmtNode`

**Where**: `parser.c`, `parseLoopStmtNode()`, right after the begin/end loop names are
confirmed to match:
```c
if (strcmp((char *)(name1->id), (char *)(name2->id))) {
	parser_error(PR_EXPECTED_MATCHING_LOOP_NAME, tokens);
	goto parseLoopStmtNodeAbort;
}

/* We no longer need the end-of-loop name */
deleteIdentifierNode(name2);
```
`name2` is genuinely no longer needed and is explicitly freed — but, unlike every other
ownership hand-off/free in this same function (e.g. `arg1 = NULL;` right after
`addExprNode(args, arg1)` succeeds), `name2` is never set to `NULL` afterward. If the *next*
parse step fails (trivially reachable: no trailing newline after `IM OUTTA YR <name>`), control
reaches `parseLoopStmtNodeAbort`, whose cleanup does, unconditionally:
```c
if (name2) deleteIdentifierNode(name2);   /* name2 is still non-NULL: freed again */
```

**Reproducer**:
```
HAI 1.3
IM IN YR loop UPPIN YR var TIL BOTH SAEM var AN 10
Vr
IM OUTTA YR loop KTHXBYE
```
`IM OUTTA YR loop` and `KTHXBYE` on the same line means the parser's required trailing newline
after the loop name is missing, so `parseLoopStmtNodeAbort` fires right after `name2` was
already freed. Confirmed under ASan on the harness binary: `heap-use-after-free` in
`deleteIdentifierNode`, `previously allocated by ... createIdentifierNode ... parseIdentifierNode
... parseLoopStmtNode`.

**Impact**: heap use-after-free / double-free, ASan-fatal; reachable by any caller of `lci`'s
parser on ordinary (if syntactically incomplete) LOLCODE source.

**Fix**: `name2 = NULL;` immediately after `deleteIdentifierNode(name2);`, mirroring the
`argN = NULL;` pattern this same function already uses for every other hand-off/free.

**Same bug family as** `mayhem/lci/known-findings/loop-update-double-free/` (missing
`= NULL` after an ownership transfer/free in this function's abort-cleanup path) — a
pre-existing site that finding's original writeup did not cover. Confirmed unchanged between
this integration's original base and current upstream `future` (not a sync regression).

**How this was found**: `mayhem/build/lci -dict=... -max_total_time=180` (a local mutation
smoke run over the seed corpus) while validating this integration's sync onto current
upstream `future`.
