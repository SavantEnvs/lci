# NULL-pointer dereference on `SOCKS'Z RESOLV` lookup failure

**Where**: `interpreter.c`, `castStringExplicit()`, `case VT_STRING:` (`strlen(getString(node))`
on a string ValueObject whose `data.s == NULL`). Reached via `castStringImplicit()` from
`interpretAssignmentStmtNode()`/`interpretPrintStmtNode()` — i.e. as soon as the value is used in
almost any expression context (assignment, `VISIBLE`, concatenation, ...), not only when printed.

**Cause**: `binding.c`'s `ilookupWrapper()` does:

```c
char *h = inet_lookup(addr);
ValueObject *ret = createStringValueObject(h);
```

`inet_lookup()` (`inet.c`) returns `0`/`NULL` whenever `gethostbyname()` fails to resolve the
name — the ordinary, expected outcome for an unresolvable hostname, not an exceptional condition.
`createStringValueObject()` stores whatever pointer it is given without a NULL check, so the
resulting `ValueObject` is a `VT_STRING` value with `data.s == NULL`. Nothing downstream treats that
as "nil"/"NOOB" the way an actually-absent value would — `castStringExplicit()`'s `VT_STRING` case
unconditionally calls `strlen(getString(node))`, which dereferences the NULL pointer.

**Reproducer**: `repro.lol` — `CAN HAS SOCKS?`, `RESOLV "example.com"` (a name this sandboxed build
cannot resolve), then use the result (a plain assignment is enough to trigger the crash; `VISIBLE`
is not required). Confirmed via gdb on a debug build of the harness pipeline (front end +
`interpretMainNodeScope`), no sanitizer needed to observe the segfault:

```
Program received signal SIGSEGV, Segmentation fault.
__strlen_evex () at ../sysdeps/x86_64/multiarch/strlen-evex.S:79
#0  __strlen_evex ()
#1  castStringExplicit (node=..., scope=...) at interpreter.c:1459
#2  castStringImplicit (node=..., scope=...) at interpreter.c:1229
#3  interpretAssignmentStmtNode (node=..., scope=...) at interpreter.c:3287
#4  interpretStmtNode (...) at interpreter.c:3933
...
```

**Not a harness artifact**: this integration's fuzz harness (`mayhem/harness_stubs.c`) links
no-op `inet_*` stubs instead of real sockets, and the stub `inet_lookup()` always returns NULL —
but that is *exactly* the return value the REAL `inet_lookup()` already produces on any failed
`gethostbyname()` call (see `inet.c`'s own doc comment: `Returns ... 0  Unable to resolve name.`).
Upstream's own binding test, `test/1.4-Tests/13-Bindings/3-socket/1-lookup/test.lol`, exercises this
exact call shape (`RESOLV "localhost"`) and only avoids the crash because `"localhost"` is
essentially always resolvable via `/etc/hosts`. Any other hostname — or any environment without
working DNS/network egress, which a fuzzing sandbox typically is — hits this path on real,
unmodified `inet.c` too.

**Impact**: a LOLCODE program (or a library import of one) that resolves a name and then uses the
result before checking it is nil crashes the interpreter (DoS) instead of getting a nil/error value
it could branch on. Low severity (no memory corruption beyond the NULL read; lci has no
sandboxing/trust boundary today), but a real, upstream, reachable defect.

**One-line upstream fix**: in `ilookupWrapper()` (`binding.c`), return `NOOB`/nil (or an
interpreter error) instead of `createStringValueObject(NULL)` when `inet_lookup()` returns NULL —
mirroring how `fopenWrapper()`'s NULL `FILE*` is already handled safely by `ferrorWrapper()`'s
`file == NULL || ferror(file)` short-circuit.

**How this was found**: while extending this integration's fuzz harness from front-end-only
(lex/tokenize/parse) to the full pipeline including `interpretMainNodeScope()`, the SOCKS binding's
host-facing socket calls were replaced with safe no-ops (matching failure semantics) rather than
being fuzzed with live sockets. Manually exercising each neutered library (`STDIO`, `DUZ`, `SOCKS`)
against the built harness — the standard "does the neutering degrade gracefully" check before
trusting the target to fuzz unattended — surfaced this crash on the very first `RESOLV` probe.
