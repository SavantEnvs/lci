# NULL-pointer deref calling a method on an undeclared scope variable

**Where**: `interpreter.c`, `interpretFuncCallExprNode()`:
```c
dest = getScopeObject(scope, scope, expr->scope);

target = getScopeObjectLocalCaller(scope, dest, expr->name);
if (!target) return NULL;
```
`dest` (the object a method call targets, e.g. the `foo` in `foo IZ bar YR ... MKAY`) is used
completely unchecked, unlike the very next line's `target`. `getScopeObject()` legitimately
returns `NULL` whenever the target scope identifier does not resolve (trivially reachable: call
a method on any undeclared variable). The `NULL` `dest` then flows into
`getScopeValue()`/`updateScopeValue()` → `resolveTerminalSlot()`, which "succeeds" with a NULL
`*parent` (its own `target->slot` chain is empty for a plain identifier, so it never
dereferences `dest` to notice), and the NULL `parent` then reaches the new O(1)
`findScopeSlot()`'s `if (scope->idxcap)` — a NULL-pointer member access.

**Reproducer**:
```
HAI 1.3
HOW IZ I fun YR a
VISIBLE a
IF U SAY SO

i IZ fun YR SUM OF 2 AN 2.345 MKAY
KTHXBYE
```
`i` (lowercase) is never declared, so `i IZ fun YR ...` targets an undeclared scope variable.
(The dangling `IF U SAY SO` also poisons this program's bytecode-VM analysis, routing
execution through the tree-walking interpreter fallback where this code lives.) Confirmed
under UBSan on the harness binary: `member access within null pointer of type 'ScopeObject'`
in `findScopeSlot`, called (with a NULL `parent`) from `getScopeValue`'s lookup loop, from
`getScopeObject`, from `interpretFuncCallExprNode`.

**Impact**: NULL-pointer dereference, reachable by any LOLCODE program that calls a method
on an undeclared variable.

**Fix**: `if (!dest) return NULL;` right after `dest = getScopeObject(...)`, mirroring the
very next line's own `if (!target) return NULL;`. No separate `error()` call is needed:
`getScopeValue()`'s own `IN_VARIABLE_DOES_NOT_EXIST` already reports the undeclared-variable
error before `getScopeObject()` returns `NULL` up the stack.

**Pre-existing since the bytecode VM merge**: `findScopeSlot()` and the "return NULL, let the
caller decide" contract on `resolveTerminalSlot()` are both new in upstream's "Add a bytecode
machine and a native code generator" commit; the *old* `resolveTerminalSlot()` used a
different (`status =`) calling convention this integration's original base relied on, so this
exact NULL-`dest`-reaches-a-raw-pointer-dereference path did not previously exist.

**How this was found**: `mayhem/build/lci -dict=... -max_total_time=180` (a local mutation
smoke run over the seed corpus) while validating this integration's sync onto current
upstream `future`.
