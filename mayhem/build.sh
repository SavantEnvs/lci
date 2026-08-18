#!/usr/bin/env bash
#
# mayhem/build.sh — build the lci LOLCODE fuzz harness (front end + interpreter) plus
# its upstream ctest suite.
#
# lci is a LOLCODE interpreter. Its CLI (`lci @@`) is a plain executable with no
# SanitizerCoverage, so fuzzing it directly measures 0 edges. We instead build an
# IN-PROCESS libFuzzer harness (mayhem/fuzz_lci.c) over the SAME pipeline the CLI
# drives — scanBuffer -> tokenizeLexemes -> parseMainNode -> interpretMainNodeScope
# (lexer/tokenizer/parser/unicode/error + the interpreter/VM/bindings) — compiled
# WITH the fuzzing engine so lci's own sources are instrumented (edges > 0) and
# ASan/UBSan see defects anywhere in that pipeline, front end AND interpretation.
#
# Interpreting untrusted LOLCODE reaches outside the process in three ways lci's
# own error handling does not guard against (IM IN YR <loop> with no break is a
# genuine unbounded native loop; CAN HAS STDIO?/DUZ <cmd>/CAN HAS SOCKS? are real
# file/process/socket primitives). mayhem/fuzz_lci.c's header comment has the full
# writeup; here we apply the BUILD-TIME mitigations it depends on:
#   * binding.c and interpreter.c are compiled for the harness with -D renames that
#     redirect fopen/fread/fwrite/fclose/rewind/ferror/popen/pclose to the safe
#     no-ops in mayhem/harness_stubs.c (never applied to the oracle build below).
#   * inet.c (real TCP sockets) is left OUT of the harness's source list; the same
#     harness_stubs.c provides no-op inet_* definitions instead.
#   * a SHADOW COPY of interpreter.c (never the committed interpreter.c in-repo --
#     same "shadow copy, not upstream files" pattern the oracle build below already
#     uses) gets a small build-time-injected step budget so an unbounded LOLCODE loop
#     (IM IN YR <loop> whose guard/body never makes it stop -- fuzzer-discovered OR
#     already sitting in the accumulated seed corpus) returns a normal interpretation
#     error instead of spinning forever (per PORTING.md, Mayhem owns timeouts, so
#     nothing in mayhem/ installs a process-level watchdog of its own -- the
#     Mayhemfile's `timeout: 30` is the backstop). See FUZZ_SHADOW below for why this has to be a
#     source-level injection (a link-time `ld --wrap=interpretStmtNode` was tried
#     first specifically to avoid ANY interpreter.c-shaped copy at all, and does not
#     work on this toolchain -- verified empirically) and why a shadow copy rather
#     than an in-repo edit keeps the integration purely additive.
#   * a SHADOW COPY of parser.c fixes several reachable heap UAF/double-free bugs in
#     parseLoopStmtNode()'s error-cleanup path (see PARSER_SHADOW below and
#     mayhem/lci/known-findings/loop-update-double-free/) -- real, upstream,
#     memory-safety defects a prior productive run found and Mayhem stored in this
#     target's server-side accumulated corpus, where they now abort EVERY
#     subsequent run's single-process regression-testing replay (0 edges,
#     has_critical_errors=true) unless the harness survives them. Same
#     shadow-copy-keeps-it-additive reasoning as interpreter.c above.
#   * a SHADOW COPY of tokenizer.c fixes a reachable unsigned-underflow OOB in
#     tokenizeLexemes()'s error-cleanup path (see TOKENIZER_SHADOW below) --
#     mutation-fuzzing this integration's own seed corpus found it in under two
#     minutes, so it is exactly the kind of shallow defect that would otherwise
#     re-poison the accumulated corpus on the very next productive run.
#   * a SHADOW COPY of binding.c fixes a reachable NULL-pointer dereference in
#     ilookupWrapper() (see BINDING_SHADOW below and
#     mayhem/lci/known-findings/socks-resolv-null-string-deref/) for the same
#     reason -- it is a real crash on ordinary DNS-resolution failure, not a
#     harness artifact.
#
# We build three artifacts from the upstream sources:
#   build/lci             sanitized (ASan+UBSan, halting) + DWARF-3 libFuzzer target -> the Mayhem target
#   build/lci-standalone  same harness against $STANDALONE_FUZZ_MAIN (run-once repro, no libFuzzer rt)
#   build-tests/          upstream CMake+ctest suite (normal flags, REAL bindings) -> mayhem/test.sh oracle
#
# The oracle build is upstream's own CMake+ctest suite (~325 golden-output tests driven by
# test/testDriver.py), applied to a SHADOW COPY of the source tree (never to upstream files
# in-repo). One additive accommodation remains necessary:
#   * CMakeLists.txt declares cmake_minimum_required(2.8), which cmake >= 3.31 rejects outright;
#     -DCMAKE_POLICY_VERSION_MINIMUM=3.5 lets configure proceed.
# (testDriver.py's own python3 compatibility -- previously patched here -- is now handled
# natively by upstream/future's own "Make the test suite run on a current Python" commit; see
# the oracle-build section below for why re-patching it now would be actively wrong.)
# No network; everything builds from the checkout + apt-installed readline/ncurses dev packages.
set -euo pipefail

[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH

: "${SANITIZER_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer}"
: "${DEBUG_FLAGS:=-g -gdwarf-3}"
: "${CC:=clang}"
: "${LIB_FUZZING_ENGINE:=-fsanitize=fuzzer}"
: "${STANDALONE_FUZZ_MAIN:=/opt/mayhem/StandaloneFuzzTargetMain.c}"
: "${MAYHEM_JOBS:=$(nproc)}"
: "${COVERAGE_FLAGS=}"
export SANITIZER_FLAGS DEBUG_FLAGS CC LIB_FUZZING_ENGINE STANDALONE_FUZZ_MAIN MAYHEM_JOBS COVERAGE_FLAGS

cd "${SRC:-/mayhem}"

# ---- fuzz-only shadow copy of interpreter.c: build-time interpreter step budget ----
#
# interpretStmtNode()'s only caller (interpretStmtNodeList()) and interpretLoopStmtNode()'s
# `while (1)` both live inside interpreter.c itself, so the ONLY way to bound them is a
# source-level check inside that file -- there is no cross-translation-unit boundary for a
# -D rename (like HOST_DENY_DEFS below) or a linker `--wrap` to interpose on (both were
# tried; `ld --wrap=interpretStmtNode` specifically does NOT work here: clang/lld resolve
# that intra-object call directly, bypassing --wrap entirely -- verified empirically, a
# wrapped build still hung past a 30s deadline on the known hanging seed with zero wrapper
# invocations observed). So this patches a SHADOW COPY of interpreter.c -- never the
# committed interpreter.c in-repo, which stays byte-for-byte identical to upstream/future --
# exactly the same "never edit upstream files in-repo, only a scratch copy" pattern the
# oracle build's $SHADOW below already uses for testDriver.py/CMakeLists.txt. The oracle
# build (section 3) compiles the REAL, unpatched interpreter.c, so upstream's own ctest
# suite always exercises byte-for-byte unmodified interpreter behavior.
#
# What gets injected (see budget_remaining/MAYHEM_STMT_BUDGET below): a counter reset once
# per interpretMainNodeScope() call (fuzz_lci.c's LLVMFuzzerTestOneInput() entry point), and
# a check-and-decrement at the top of interpretStmtNode() (bounds any loop/function/block
# whose BODY executes statements -- the common case, including every hang documented in
# mayhem/lci/known-findings/) and at the top of interpretLoopStmtNode()'s `while (1)` (bounds
# a loop with an EMPTY body that would otherwise spin purely on interpretExprNode() without
# ever reaching interpretStmtNode()). Budget exhaustion returns NULL/1, the SAME "an error
# occurred during interpretation" convention interpreter.c already uses pervasively, so every
# existing caller already unwinds and frees its own ScopeObject correctly on that path.
#
# 50,000 is deliberately far below what it sounds like it should need to be: measured
# empirically (ASan+UBSan build, -runs=1 on the known hanging
# mayhem/lci/known-findings/gimmeh-eof-unbounded-loop/repro.lol), this interpreter's
# per-statement cost is high enough that a "generous" 5,000,000 took ~12s wall time --
# well past the Mayhemfile's `timeout: 30` once host load is accounted for. 50,000 leaves
# ample wall-clock headroom under that timeout even on a slow training host, while still
# being far more than any legitimate seed in mayhem/lci/testsuite/ needs (verified:
# -runs=1 on every seed individually, and a -merge=1 coverage pass over the whole
# directory, both exit 0 in low-single-digit milliseconds of libFuzzer-reported execution
# time).
FUZZ_SHADOW=/tmp/lci-fuzz-src
rm -rf "$FUZZ_SHADOW"
mkdir -p "$FUZZ_SHADOW"
cp interpreter.c "$FUZZ_SHADOW/interpreter.c"
python3 - "$FUZZ_SHADOW/interpreter.c" <<'PYEOF'
import sys

path = sys.argv[1]
src = open(path).read()


def replace_once(s, old, new, label):
    n = s.count(old)
    if n != 1:
        raise SystemExit(f"budget patch: expected exactly 1 match for {label!r}, found {n}")
    return s.replace(old, new, 1)


BUDGET_BLOCK = '''#include "interpreter.h"

/*
 * mayhem: bound total interpreter work per interpretMainNodeScope() call so an
 * unbounded LOLCODE loop returns a normal "interpretation error" (NULL/1) instead
 * of spinning forever (Mayhem's own Mayhemfile `timeout: 30` is the backstop for
 * anything this budget can't see). Injected into a build-time SHADOW COPY of this
 * file by mayhem/build.sh (see that file's header comment) -- the committed
 * interpreter.c is unmodified.
 */
#define MAYHEM_STMT_BUDGET 50000UL
static unsigned long mayhem_stmt_budget_remaining = MAYHEM_STMT_BUDGET;
#define MAYHEM_BUDGET_RESET() (mayhem_stmt_budget_remaining = MAYHEM_STMT_BUDGET)
#define MAYHEM_BUDGET_CHECK_OR(fail_stmt) \\
\tdo { \\
\t\tif (mayhem_stmt_budget_remaining == 0) { \\
\t\t\tfail_stmt; \\
\t\t} \\
\t\tmayhem_stmt_budget_remaining--; \\
\t} while (0)
'''
src = replace_once(src, '#include "interpreter.h"\n', BUDGET_BLOCK, "top-of-file include")

STMT_OLD = '''ReturnObject *interpretStmtNode(StmtNode *node,
                                ScopeObject *scope)
{
\treturn StmtJumpTable[node->type](node, scope);
}'''
STMT_NEW = '''ReturnObject *interpretStmtNode(StmtNode *node,
                                ScopeObject *scope)
{
\tMAYHEM_BUDGET_CHECK_OR(return NULL);
\treturn StmtJumpTable[node->type](node, scope);
}'''
src = replace_once(src, STMT_OLD, STMT_NEW, "interpretStmtNode body")

LOOP_OLD = '''\twhile (1) {
\t\tif (stmt->guard) {
\t\t\tValueObject *val = interpretExprNode(stmt->guard, outer);'''
LOOP_NEW = '''\twhile (1) {
\t\tMAYHEM_BUDGET_CHECK_OR({ deleteScopeObject(outer); return NULL; });
\t\tif (stmt->guard) {
\t\t\tValueObject *val = interpretExprNode(stmt->guard, outer);'''
src = replace_once(src, LOOP_OLD, LOOP_NEW, "interpretLoopStmtNode while(1)")

MAIN_OLD = '''int interpretMainNodeScope(MainNode *main, ScopeObject *scope)
{
\tReturnObject *ret = NULL;
\tProto *proto;
\tif (!main) return 1;

\tcompileProgram(main);'''
MAIN_NEW = '''extern unsigned long mayhem_vm_budget_remaining;
#define MAYHEM_VM_BUDGET 50000UL

int interpretMainNodeScope(MainNode *main, ScopeObject *scope)
{
\tReturnObject *ret = NULL;
\tProto *proto;
\tif (!main) return 1;
\tMAYHEM_BUDGET_RESET();
\tmayhem_vm_budget_remaining = MAYHEM_VM_BUDGET;

\tcompileProgram(main);'''
src = replace_once(src, MAIN_OLD, MAIN_NEW, "interpretMainNodeScope body")

# interpretLoopStmtNode()'s WILE/TIL guard evaluation dereferences interpretExprNode()'s
# result unconditionally, but that call legitimately returns NULL on any interpretation error
# (e.g. "variable does not exist" for a guard expression referencing an undefined variable) --
# a NULL pointer dereference, found by mutation-fuzzing this integration's own seed corpus.
# Same "shadow copy, never the committed file" reasoning as the step budget above.
GUARD_VAL_OLD = '''\t\t\tValueObject *val = interpretExprNode(stmt->guard, outer);
\t\t\tValueObject *use = val;'''
GUARD_VAL_NEW = '''\t\t\tValueObject *val = interpretExprNode(stmt->guard, outer);
\t\t\tif (!val) {
\t\t\t\tdeleteScopeObject(outer);
\t\t\t\treturn NULL;
\t\t\t}
\t\t\tValueObject *use = val;'''
src = replace_once(src, GUARD_VAL_OLD, GUARD_VAL_NEW, "interpretLoopStmtNode guard NULL check")

# resolveIdentifierName()'s fallback branch (id->type is neither IT_DIRECT nor IT_INDIRECT)
# recurses on the SAME `id` it was just called with to build an error message -- since `id`
# hasn't changed, this hits the identical branch again, forever: unconditional infinite
# recursion, not merely a deep one, so the existing MAYHEM_STMT_BUDGET counter (which only
# instruments interpretStmtNode()/interpretLoopStmtNode()'s while(1), not this recursive
# helper) never gets a chance to fire before the native call stack overflows. ASan reports
# this as a fatal stack-overflow. Found by mutation-fuzzing this integration's own seed
# corpus. Fix: report the same diagnostic without recursing -- there is no safe way to
# describe an identifier of an invalid type, so use a fixed placeholder instead of a
# recursively-resolved name.
RESOLVE_OLD = '''\t} else {
\t\tchar *name = resolveIdentifierName(id, scope);
\t\terror(IN_INVALID_IDENTIFIER_TYPE, id->fname, id->line, name);
\t\tfree(name);
\t}'''
RESOLVE_NEW = '''\t} else {
\t\terror(IN_INVALID_IDENTIFIER_TYPE, id->fname, id->line, "<unknown>");
\t}'''
src = replace_once(src, RESOLVE_OLD, RESOLVE_NEW, "resolveIdentifierName infinite-recursion fix")

# interpretFuncCallExprNode()'s `dest = getScopeObject(scope, scope, expr->scope);` -- resolving
# the object a method call targets, e.g. the `foo` in `foo IZ bar YR ... MKAY` -- is used
# completely unchecked, unlike the very next line's `target = getScopeObjectLocalCaller(...);
# if (!target) return NULL;`. getScopeObject() legitimately returns NULL whenever the target
# scope identifier does not resolve (trivially reachable: call a method on any undeclared
# variable, e.g. `undeclaredVar IZ someFunc YR ... MKAY`) -- getScopeValue()'s own
# IN_VARIABLE_DOES_NOT_EXIST already reports that case before returning NULL up through
# getScopeObject(), so nothing here needs its own error() call, only the same "check before
# use" `dest` already gets everywhere else in this function. Left unchecked, the NULL `dest`
# flows into getScopeValue()/updateScopeValue() -> resolveTerminalSlot(), which happily
# "succeeds" with a NULL *parent (its own target->slot chain is empty for a plain identifier,
# so it never dereferences dest to notice), and the NULL parent then reaches the new O(1)
# findScopeSlot()'s `if (scope->idxcap)` -- a NULL-pointer member access, confirmed under
# UBSan on this exact repro. findScopeSlot()/its NULL-parent-tolerant caller loop are both new
# (added by the same "Add a bytecode machine and a native code generator" commit that added
# the escape analysis this integration's VM_SHADOW patch works around), so this is a genuine
# upstream regression from that merge, not a pre-existing gap. Found by mutation-fuzzing this
# integration's own seed corpus.
DEST_OLD = '''\tdest = getScopeObject(scope, scope, expr->scope);

\ttarget = getScopeObjectLocalCaller(scope, dest, expr->name);'''
DEST_NEW = '''\tdest = getScopeObject(scope, scope, expr->scope);
\tif (!dest) return NULL;

\ttarget = getScopeObjectLocalCaller(scope, dest, expr->name);'''
src = replace_once(src, DEST_OLD, DEST_NEW, "interpretFuncCallExprNode dest NULL check")

open(path, "w").write(src)
PYEOF

# ---- fuzz-only shadow copy of vm.c: build-time bytecode-VM step budget ----
#
# Upstream added a bytecode compiler + VM (vm.c/jit.c, "Add a bytecode machine and a native
# code generator") between this integration's original base and current future:
# interpretMainNodeScope() now compileProgram()s the parsed tree once and, when that
# succeeds (the common case), runs it through callProto()/run() instead of the tree-walking
# interpretBlockNode() -- the tree walker (and the MAYHEM_STMT_BUDGET check patched into
# interpretStmtNode()/interpretLoopStmtNode() above) is now only a FALLBACK for the rare
# program compileProc() can't lower (analysisPoisoned). So the SAME unbounded-native-loop
# risk the step budget above exists for (IM IN YR <loop> with no break, GIMMEH-in-a-loop-
# that-never-sees-EOF) now runs primarily through run()'s bytecode dispatch loop, which the
# original budget cannot see at all -- verified: MAIN_OLD/MAIN_NEW's old anchor (a direct
# `interpretBlockNode()` call) no longer even matches this version of interpreter.c. This
# patches a SECOND, independent counter into a SHADOW COPY of vm.c (never the committed
# vm.c in-repo), decremented once per bytecode instruction dispatched in run()'s `for (;;)`
# loop -- exactly the tree-walker's "one decrement per unit of interpreter work" shape, just
# at the bytecode-instruction granularity instead of the statement granularity, since a
# bytecode loop back-edge is just another dispatched instruction here, not a distinguishable
# control-flow event. Reset once per interpretMainNodeScope() call (see MAIN_NEW above,
# which resets this shadow's `mayhem_vm_budget_remaining` via extern the same way it resets
# MAYHEM_STMT_BUDGET) so the two counters bound the SAME kind of per-input work, whichever
# path (compiled or tree-walked) a given program takes.
#
# The JIT half of this addition (jit.c: compiles hot Protos to real native machine code via
# mmap(PROT_EXEC)/mprotect) is deliberately left OUT of scope for a step-budget check: unlike
# interpretStmtNode()/run(), there is no source-level statement to inject a counter check
# into -- the "check" would have to be machine code emitted into the JIT's own code buffer,
# which is a fundamentally different (and far riskier) kind of patch than every other shadow
# copy in this file. Instead the harness disables JIT compilation entirely for the fuzz
# target via the LCI_NOJIT environment variable upstream already wired into jitCompileAll()
# (jit.c) for exactly this purpose -- see fuzz_lci.c's LLVMFuzzerInitialize(). With JIT
# compilation disabled, every Proto's ->jitcode stays NULL, so callProto() always takes the
# run() path this shadow copy bounds; jit.c's codegen itself is therefore not exercised by
# this fuzz target (unchanged: the oracle build below, section 3, still runs upstream's own
# ctest suite -- including its JIT-enabled paths -- against the REAL, unpatched vm.c/jit.c).
VM_SHADOW=/tmp/lci-vm-src
rm -rf "$VM_SHADOW"
mkdir -p "$VM_SHADOW"
cp vm.c "$VM_SHADOW/vm.c"
python3 - "$VM_SHADOW/vm.c" <<'PYEOF'
import sys

path = sys.argv[1]
src = open(path).read()


def replace_once(s, old, new, label):
    n = s.count(old)
    if n != 1:
        raise SystemExit(f"vm budget patch: expected exactly 1 match for {label!r}, found {n}")
    return s.replace(old, new, 1)


BUDGET_BLOCK = '''#include "jit.h"

/*
 * mayhem: bound total bytecode-instruction dispatch per interpretMainNodeScope() call
 * (interpreter.c resets this via extern once per call) so an unbounded LOLCODE loop
 * compiled to bytecode returns a normal VM_ERROR instead of spinning forever, mirroring
 * the tree-walker's MAYHEM_STMT_BUDGET (mayhem/build.sh's FUZZ_SHADOW). Injected into a
 * build-time SHADOW COPY of this file by mayhem/build.sh (see that file's VM_SHADOW
 * comment) -- the committed vm.c is unmodified.
 */
unsigned long mayhem_vm_budget_remaining = 50000UL;'''
src = replace_once(src, '#include "jit.h"\n', BUDGET_BLOCK, "top-of-file include")

RUN_OLD = '''\tfor (;;) {
\t\tconst Instr i = *pc++;
\t\tswitch ((Opcode)i.op) {'''
RUN_NEW = '''\tfor (;;) {
\t\tconst Instr i = *pc++;
\t\tif (mayhem_vm_budget_remaining == 0) return VM_ERROR;
\t\tmayhem_vm_budget_remaining--;
\t\tswitch ((Opcode)i.op) {'''
src = replace_once(src, RUN_OLD, RUN_NEW, "run() dispatch loop budget check")

# cachedLookup()'s "not found" diagnostic path casts p->kptrs[k] to IdentifierNode* and reads
# ->fname/->line unconditionally -- correct when cachedLookup() is reached from a plain
# variable/GETVAR lookup (kptrs[k] really is an IdentifierNode* there), but callByName() reuses
# this SAME helper for a FUNCTION lookup, where kptrs[k] is actually a FuncCallExprNode*
# (see callByName's own `(FuncCallExprNode *)p->kptrs[k])->name`). Once a call names a
# function that plain does not exist -- trivially reachable, `I IZ <undefined> MKAY` -- this
# is a type confusion that reads ->line at IdentifierNode's offset out of the (smaller)
# FuncCallExprNode allocation: a heap-buffer-overflow READ, confirmed under ASan on exactly
# that repro within the first minute of mutation-fuzzing this integration's own seed corpus.
# Fix: report the same error, but never assume which struct kptrs[k] actually is here --
# `name` (p->knames[k]) is valid regardless of which caller reached this path.
LOOKUP_OLD = '''\t{
\t\tIdentifierNode *id = (IdentifierNode *)p->kptrs[k];
\t\terror(IN_VARIABLE_DOES_NOT_EXIST, id ? id->fname : NULL,
\t\t\t\tid ? id->line : 0, name->str);
\t}
\treturn NULL;'''
LOOKUP_NEW = '''\terror(IN_VARIABLE_DOES_NOT_EXIST, NULL, 0, name->str);
\treturn NULL;'''
src = replace_once(src, LOOKUP_OLD, LOOKUP_NEW, "cachedLookup type-confusion fix")

open(path, "w").write(src)
PYEOF

# ---- fuzz-only shadow copy of parser.c: fix reachable heap UAF/double-free bugs ----
#
# parseLoopStmtNode() (parser.c) builds its loop "update" expression (and, separately, its
# WILE/TIL predicate "guard" expression) out of several heap nodes, wrapping each one in an
# owning parent as it goes -- e.g. `op = createOpExprNode(type, args)` makes `op` own `args`,
# then `update = createExprNode(ET_OP, op)` makes `update` own `op`. EIGHT call sites (four
# ownership chains) do this ownership transfer without nulling the now-redundant local
# afterward (unlike every OTHER ownership transfer in the very same function, e.g.
# `arg1 = NULL;` right after `addExprNode(args, arg1)` succeeds). If any LATER parse step in
# the function fails -- trivially reachable, e.g. a stray token where a newline is expected --
# control jumps to parseLoopStmtNodeAbort, whose cleanup unconditionally frees BOTH the
# wrapping parent (which recursively frees the child already) AND the stale child local: a
# double-free / heap use-after-free. Documented in full, with a 3-line repro and an ASan
# trace, at mayhem/lci/known-findings/loop-update-double-free/README.md for the op/args site;
# the other three chains (varcopy/arg1, one/arg2, the TIL-predicate predop/predargs, and the
# function-call-loop node/scope/name/args) are the identical bug, reachable the same way,
# just undocumented until this integration -- found by walking every "package X into an
# expression" hand-off in the function after fixing the first one surfaced a SECOND
# use-after-free at a different line on the very same 3-line repro (varcopy/arg1, nested one
# level deeper than op/args). This is "not a harness artifact" (the README's own words) -- a plain
# `lci <file>` CLI build hits it on the same malformed input -- so it belongs in parser.c, not
# in this harness. But parser.c is unmodified upstream, and this integration must stay exactly
# one purely-additive commit over it (GATE 5) -- so, same "shadow copy, never the committed
# file" pattern as interpreter.c's step budget above, the harness build compiles a build-time
# PATCHED COPY. The one-line-per-site fix mirrors the existing arg1/arg2 pattern already used
# in this function: null the local immediately after its successful hand-off to the new owner,
# so the abort path's `if (x) delete...(x);` becomes the no-op it already is for arg1/arg2/id.
# The oracle build (section 3 below) compiles the REAL, unpatched parser.c, so upstream's own
# ctest suite always exercises byte-for-byte unmodified parsing behavior.
PARSER_SHADOW=/tmp/lci-parser-src
rm -rf "$PARSER_SHADOW"
mkdir -p "$PARSER_SHADOW"
cp parser.c "$PARSER_SHADOW/parser.c"
python3 - "$PARSER_SHADOW/parser.c" <<'PYEOF'
import sys

path = sys.argv[1]
src = open(path).read()


def replace_once(s, old, new, label):
    n = s.count(old)
    if n != 1:
        raise SystemExit(f"parser UAF patch: expected exactly 1 match for {label!r}, found {n}")
    return s.replace(old, new, 1)


# Site -2: parseIdentifierNode()'s catch-all `else` branch (neither a direct TT_IDENTIFIER nor
# an indirect `SRS <expr>` matched) calls parser_error() but, unlike every other error path in
# this function, does NOT `goto parseIdentifierNodeAbort;` afterward -- so `type` and `data`
# (both otherwise only ever set inside the two branches above) are left at whatever garbage
# value already occupied those stack locals, and execution falls through to
# `ret = createIdentifierNode(type, data, slot, fname, line)`, building a fully-formed
# IdentifierNode out of uninitialized memory. This is the actual root cause behind Site -1a/-1b
# below manifesting as a NULL (rather than merely "wrong-typed") `id`: whatever garbage `type`
# happened to hold can equal IT_DIRECT by chance while `data` (uninitialized) is coincidentally
# NULL, which is exactly what a mutation-fuzzing run reproduced. Confirmed pre-existing and
# unchanged by the upstream sync (same before and after the bytecode-VM merge). Fix: add the
# missing abort, matching this function's own convention everywhere else.
IDABORT_OLD = '''\telse {
\t\tparser_error(PR_EXPECTED_IDENTIFIER, tokens);
\t}'''
IDABORT_NEW = '''\telse {
\t\tparser_error(PR_EXPECTED_IDENTIFIER, tokens);
\t\tgoto parseIdentifierNodeAbort;
\t}'''
src = replace_once(src, IDABORT_OLD, IDABORT_NEW, "parseIdentifierNode missing-abort fix")

# Site -1a: defense in depth for the SAME finding IDABORT just root-caused (see that patch's
# comment and mayhem/lci/known-findings/loop-var-indirect-identifier-deref/ for the actual
# fuzzer-found repro and root cause). Independent of that bug, the increment/decrement loop
# variable ("UPPIN YR i") is parsed with a plain parseIdentifierNode() call and immediately
# treated as IT_DIRECT (`strlen(var->id)` / `strcpy(id, var->id)`, i.e. `id` is assumed to be a
# plain C string) -- but parseIdentifierNode() can LEGITIMATELY return an IT_INDIRECT
# identifier too (a computed name, "SRS <expr>"), whose `id` field is a `void *` pointing at an
# ExprNode, not a string. Unlike the loop's OWN name a few lines above (name1/name2, both
# explicitly checked with `if (name->type != IT_DIRECT) { parser_error(...); goto ...Abort; }`),
# the loop VARIABLE was never given the same check, so a well-formed indirect loop variable
# would still reach strlen()/strcpy() on a non-string pointer even after IDABORT closes the
# uninitialized-garbage path. Mirrors the name1/name2 check already present in this function.
VARTYPE_OLD = '''\t\tvar = parseIdentifierNode(&tokens);
\t\tif (!var) goto parseLoopStmtNodeAbort;'''
VARTYPE_NEW = '''\t\tvar = parseIdentifierNode(&tokens);
\t\tif (!var) goto parseLoopStmtNodeAbort;
\t\tif (var->type != IT_DIRECT) {
\t\t\tparser_error(PR_EXPECTED_IDENTIFIER, tokens);
\t\t\tgoto parseLoopStmtNodeAbort;
\t\t}'''
src = replace_once(src, VARTYPE_OLD, VARTYPE_NEW, "var = parseIdentifierNode (increment/decrement loop) IT_DIRECT check")

# Site -1b: the SAME bug, second site: a "for function loops" loop variable (`I IZ func YR
# <arg> MKAY`) is pulled out of its wrapping ET_IDENTIFIER ExprNode (`temp = (IdentifierNode
# *)(arg->expr)`) and, just like site -1a, immediately strlen()/strcpy()'d as though it must
# be IT_DIRECT -- reachable the identical way (the unary argument can itself be an indirect
# identifier). Same fix, applied where `temp` is bound instead of `var`.
TEMPTYPE_OLD = '''\t\ttemp = (IdentifierNode *)(arg->expr);
\t\targ = NULL;'''
TEMPTYPE_NEW = '''\t\ttemp = (IdentifierNode *)(arg->expr);
\t\targ = NULL;
\t\tif (temp->type != IT_DIRECT) {
\t\t\tparser_error(PR_EXPECTED_IDENTIFIER, tokens);
\t\t\tgoto parseLoopStmtNodeAbort;
\t\t}'''
src = replace_once(src, TEMPTYPE_OLD, TEMPTYPE_NEW, "temp = (IdentifierNode *)(arg->expr) IT_DIRECT check")

# Site 0a: arg1 = createExprNode(ET_IDENTIFIER, varcopy) hands `varcopy` to `arg1` -- null
# `varcopy` so the abort path's `if (varcopy) deleteIdentifierNode(varcopy);` can't
# double-free it. (arg1 itself is already correctly nulled a few lines down, right after
# `addExprNode(args, arg1)` succeeds -- this is the one hop further back that was missed.)
VARCOPY_OLD = '''\t\targ1 = createExprNode(ET_IDENTIFIER, varcopy);
\t\tif (!arg1) goto parseLoopStmtNodeAbort;'''
VARCOPY_NEW = '''\t\targ1 = createExprNode(ET_IDENTIFIER, varcopy);
\t\tif (!arg1) goto parseLoopStmtNodeAbort;
\t\tvarcopy = NULL;'''
src = replace_once(src, VARCOPY_OLD, VARCOPY_NEW, "arg1 = createExprNode(ET_IDENTIFIER, varcopy)")

# Site 0b: arg2 = createExprNode(ET_CONSTANT, one) hands `one` to `arg2` -- null `one` so the
# abort path's `if (one) deleteConstantNode(one);` can't double-free it. Same one-hop-further
# pattern as varcopy/arg1 above.
ONE_OLD = '''\t\targ2 = createExprNode(ET_CONSTANT, one);
\t\tif (!arg2) goto parseLoopStmtNodeAbort;'''
ONE_NEW = '''\t\targ2 = createExprNode(ET_CONSTANT, one);
\t\tif (!arg2) goto parseLoopStmtNodeAbort;
\t\tone = NULL;'''
src = replace_once(src, ONE_OLD, ONE_NEW, "arg2 = createExprNode(ET_CONSTANT, one)")

# Site 1a: op = createOpExprNode(type, args) hands `args` to `op` -- null `args` so the abort
# path's `if (args) deleteExprNodeList(args);` can't double-free it once `op` (and later
# `update`) owns it.
OP_OLD = '''\t\top = createOpExprNode(type, args);
\t\tif (!op) goto parseLoopStmtNodeAbort;'''
OP_NEW = '''\t\top = createOpExprNode(type, args);
\t\tif (!op) goto parseLoopStmtNodeAbort;
\t\targs = NULL;'''
src = replace_once(src, OP_OLD, OP_NEW, "op = createOpExprNode(type, args)")

# Site 1b: update = createExprNode(ET_OP, op) hands `op` to `update` -- null `op` (documented
# known-findings/loop-update-double-free one-line fix).
UPDATE_OP_OLD = '''\t\tupdate = createExprNode(ET_OP, op);
\t\tif (!update) goto parseLoopStmtNodeAbort;'''
UPDATE_OP_NEW = '''\t\tupdate = createExprNode(ET_OP, op);
\t\tif (!update) goto parseLoopStmtNodeAbort;
\t\top = NULL;'''
src = replace_once(src, UPDATE_OP_OLD, UPDATE_OP_NEW, "update = createExprNode(ET_OP, op)")

# Site 2a: node = createFuncCallExprNode(scope, name, args) hands `scope`/`name`/`args` to
# `node` -- same bug, undocumented: the "For function loops" abort cleanup re-frees all three.
FUNCCALL_OLD = '''\t\tnode = createFuncCallExprNode(scope, name, args);
\t\tif (!node) goto parseLoopStmtNodeAbort;'''
FUNCCALL_NEW = '''\t\tnode = createFuncCallExprNode(scope, name, args);
\t\tif (!node) goto parseLoopStmtNodeAbort;
\t\tscope = NULL;
\t\tname = NULL;
\t\targs = NULL;'''
src = replace_once(src, FUNCCALL_OLD, FUNCCALL_NEW, "node = createFuncCallExprNode(scope, name, args)")

# Site 2b: update = createExprNode(ET_FUNCCALL, node) hands `node` to `update`.
UPDATE_FUNCCALL_OLD = '''\t\tupdate = createExprNode(ET_FUNCCALL, node);
\t\tif (!update) goto parseLoopStmtNodeAbort;'''
UPDATE_FUNCCALL_NEW = '''\t\tupdate = createExprNode(ET_FUNCCALL, node);
\t\tif (!update) goto parseLoopStmtNodeAbort;
\t\tnode = NULL;'''
src = replace_once(src, UPDATE_FUNCCALL_OLD, UPDATE_FUNCCALL_NEW, "update = createExprNode(ET_FUNCCALL, node)")

# Site 3a: predop = createOpExprNode(OP_NOT, predargs) hands `predargs` to `predop` -- same
# bug, undocumented: the "For loop predicates" abort cleanup re-frees both.
PREDOP_OLD = '''\t\t\tpredop = createOpExprNode(OP_NOT, predargs);
\t\t\tif (!predop) goto parseLoopStmtNodeAbort;'''
PREDOP_NEW = '''\t\t\tpredop = createOpExprNode(OP_NOT, predargs);
\t\t\tif (!predop) goto parseLoopStmtNodeAbort;
\t\t\tpredargs = NULL;'''
src = replace_once(src, PREDOP_OLD, PREDOP_NEW, "predop = createOpExprNode(OP_NOT, predargs)")

# Site 3b: guard = createExprNode(ET_OP, predop) hands `predop` to `guard`.
GUARD_OLD = '''\t\t\tguard = createExprNode(ET_OP, predop);
\t\t\tif (!guard) goto parseLoopStmtNodeAbort;'''
GUARD_NEW = '''\t\t\tguard = createExprNode(ET_OP, predop);
\t\t\tif (!guard) goto parseLoopStmtNodeAbort;
\t\t\tpredop = NULL;'''
src = replace_once(src, GUARD_OLD, GUARD_NEW, "guard = createExprNode(ET_OP, predop)")

# Site 3c: once the loop's beginning/end names are confirmed to match, `name2` (the end-of-loop
# name) is explicitly deleteIdentifierNode()'d -- it is genuinely no longer needed -- but,
# unlike every hand-off site above, is never nulled afterward. If the very next parse step
# fails (trivially reachable: no trailing newline after `IM OUTTA YR <name>`, e.g.
# `IM OUTTA YR loop KTHXBYE` on one line), control reaches parseLoopStmtNodeAbort, whose
# cleanup unconditionally re-frees `name2` because it still looks non-NULL: a heap
# use-after-free / double-free, found by mutation-fuzzing this integration's own seed corpus.
# Same bug family, same fix, as loop-update-double-free (mayhem/lci/known-findings/) -- this
# is a pre-existing site that family's original writeup did not cover (unchanged by the
# upstream sync: identical before and after the bytecode-VM merge), not a new regression.
NAME2_OLD = '''\t/* We no longer need the end-of-loop name */
\tdeleteIdentifierNode(name2);'''
NAME2_NEW = '''\t/* We no longer need the end-of-loop name */
\tdeleteIdentifierNode(name2);
\tname2 = NULL;'''
src = replace_once(src, NAME2_OLD, NAME2_NEW, "deleteIdentifierNode(name2) double-free fix")

# Site 4: acceptToken() (the parser's single most-used primitive) and parser_error() both used
# to dereference the current token unconditionally past the NULL-terminated token stream (any
# truncated LOLCODE file). Upstream independently hardened both of these on current `future`
# (acceptToken() now has its own `if (!(*tokens)) return 0;` guard, and parser_error() now
# reports PR_UNHANDLED_STRING instead of dereferencing) -- verified by exact-match against
# this checkout's parser.c, so there is nothing left for this integration to patch here; the
# two sibling helpers below (parser_error_expected_token/_either_token) were NOT hardened the
# same way and still need the shadow-copy fix.
EXPECTED_TOKEN_OLD = '''void parser_error_expected_token(TokenType token,
                                 Token **tokens)
{
\terror(PR_EXPECTED_TOKEN,
\t\t\t(*tokens)->fname,
\t\t\t(*tokens)->line,
\t\t\tkeywords[token],
\t\t\t(*tokens)->image);
}'''
EXPECTED_TOKEN_NEW = '''void parser_error_expected_token(TokenType token,
                                 Token **tokens)
{
\tif (!(*tokens)) return;
\terror(PR_EXPECTED_TOKEN,
\t\t\t(*tokens)->fname,
\t\t\t(*tokens)->line,
\t\t\tkeywords[token],
\t\t\t(*tokens)->image);
}'''
src = replace_once(src, EXPECTED_TOKEN_OLD, EXPECTED_TOKEN_NEW, "parser_error_expected_token NULL guard")

EXPECTED_EITHER_OLD = '''void parser_error_expected_either_token(TokenType token1,
                                        TokenType token2,
                                        Token **tokens)
{
\terror(PR_EXPECTED_TOKEN,
\t\t\t(*tokens)->fname,
\t\t\t(*tokens)->line,
\t\t\tkeywords[token1],
\t\t\tkeywords[token2],
\t\t\t(*tokens)->image);
}'''
EXPECTED_EITHER_NEW = '''void parser_error_expected_either_token(TokenType token1,
                                        TokenType token2,
                                        Token **tokens)
{
\tif (!(*tokens)) return;
\terror(PR_EXPECTED_TOKEN,
\t\t\t(*tokens)->fname,
\t\t\t(*tokens)->line,
\t\t\tkeywords[token1],
\t\t\tkeywords[token2],
\t\t\t(*tokens)->image);
}'''
src = replace_once(src, EXPECTED_EITHER_OLD, EXPECTED_EITHER_NEW, "parser_error_expected_either_token NULL guard")

open(path, "w").write(src)
PYEOF

# ---- fuzz-only shadow copy of tokenizer.c: fix a reachable unsigned-underflow OOB ----
#
# tokenizeLexemes() builds its output array with a "delete the just-added/just-failed token,
# NULL that slot as a sentinel, then deleteTokens() (which scans forward freeing entries until
# it hits a NULL slot)" cleanup idiom, used at three early-return sites. That idiom is correct
# PROVIDED at least one token has already been appended (`retsize > 0`, so `ret` is non-NULL
# and `ret[retsize - 1]`/`ret[retsize - 2]` is a real, already-tracked slot to reuse as the
# sentinel) -- but none of the three sites check that. If the FIRST lexeme in the file is
# already invalid (`retsize == 0`, `ret == NULL`), `retsize - 1` underflows (`retsize` is
# unsigned) to a huge index, so `ret[retsize - 1]` computes an out-of-bounds offset from a
# NULL pointer -- undefined behavior (UBSan: "applying non-zero offset ... to null pointer" at
# tokenizer.c, reachable via `scanBuffer -> tokenizeLexemes`, i.e. any lci input, not a harness
# artifact) and, on a build without UBSan, a wild read/write through that bogus address.
# Trivially reachable: mutation-fuzzing this integration's own seed corpus for under two
# minutes found it (any input whose very first token isn't recognized, e.g. a file that
# doesn't start with a valid LOLCODE token). Same shadow-copy reasoning as parser.c/binding.c
# above -- tokenizer.c is unmodified upstream. The oracle build (section 3) compiles the REAL,
# unpatched tokenizer.c.
TOKENIZER_SHADOW=/tmp/lci-tokenizer-src
rm -rf "$TOKENIZER_SHADOW"
mkdir -p "$TOKENIZER_SHADOW"
cp tokenizer.c "$TOKENIZER_SHADOW/tokenizer.c"
python3 - "$TOKENIZER_SHADOW/tokenizer.c" <<'PYEOF'
import sys

path = sys.argv[1]
src = open(path).read()


def replace_once(s, old, new, label):
    n = s.count(old)
    if n != 1:
        raise SystemExit(f"tokenizer underflow patch: expected exactly 1 match for {label!r}, found {n}")
    return s.replace(old, new, 1)


# Site 1: unknown-token error. No token was made this iteration, so `token` is NULL here --
# the intent is only to free whatever was already appended, which is a no-op when retsize==0.
UNKNOWN_OLD = '''\t\t\terror(TK_UNKNOWN_TOKEN, fname, line, image);
\t\t\t/* Clean up */
\t\t\tdeleteToken(ret[retsize - 1]);
\t\t\tret[retsize - 1] = NULL;
\t\t\tdeleteTokens(ret);
\t\t\treturn NULL;'''
UNKNOWN_NEW = '''\t\t\terror(TK_UNKNOWN_TOKEN, fname, line, image);
\t\t\t/* Clean up */
\t\t\tif (retsize > 0) {
\t\t\t\tdeleteToken(ret[retsize - 1]);
\t\t\t\tret[retsize - 1] = NULL;
\t\t\t\tdeleteTokens(ret);
\t\t\t}
\t\t\treturn NULL;'''
src = replace_once(src, UNKNOWN_OLD, UNKNOWN_NEW, "unknown-token cleanup")

# Site 2: addToken() failure (list growth realloc failed). `token` itself is independently
# freed either way; the ret[]-sentinel dance is again only meaningful once retsize > 0.
ADDTOKEN_OLD = '''\t\t\t/* Clean up */
\t\t\tif (token) deleteToken(token);
\t\t\tdeleteToken(ret[retsize - 1]);
\t\t\tret[retsize - 1] = NULL;
\t\t\tdeleteTokens(ret);
\t\t\treturn NULL;'''
ADDTOKEN_NEW = '''\t\t\t/* Clean up */
\t\t\tif (token) deleteToken(token);
\t\t\tif (retsize > 0) {
\t\t\t\tdeleteToken(ret[retsize - 1]);
\t\t\t\tret[retsize - 1] = NULL;
\t\t\t\tdeleteTokens(ret);
\t\t\t}
\t\t\treturn NULL;'''
src = replace_once(src, ADDTOKEN_OLD, ADDTOKEN_NEW, "addToken-failure cleanup")

# Site 3: final NUL-terminator realloc failure. `retsize` was already pre-incremented in the
# realloc call above this block, so the "at least one real token" guard is `retsize > 1` here.
FINAL_OLD = '''\tmem = realloc(ret, sizeof(Token *) * ++retsize);
\tif (!mem) {
\t\tdeleteToken(ret[retsize - 2]);
\t\tret[retsize - 2] = NULL;
\t\tdeleteTokens(ret);
\t\treturn NULL;
\t}'''
FINAL_NEW = '''\tmem = realloc(ret, sizeof(Token *) * ++retsize);
\tif (!mem) {
\t\tif (retsize > 1) {
\t\t\tdeleteToken(ret[retsize - 2]);
\t\t\tret[retsize - 2] = NULL;
\t\t\tdeleteTokens(ret);
\t\t}
\t\treturn NULL;
\t}'''
src = replace_once(src, FINAL_OLD, FINAL_NEW, "final-realloc-failure cleanup")

open(path, "w").write(src)
PYEOF

# ---- fuzz-only shadow copy of binding.c: fix a reachable NULL-pointer dereference ----
#
# ilookupWrapper() (CAN HAS SOCKS? / RESOLV) stores inet_lookup()'s return straight into a
# VT_STRING ValueObject with no NULL check. inet_lookup() legitimately returns NULL on any
# failed gethostbyname() -- an ordinary, expected outcome (a fuzzing sandbox has no working
# DNS, and this harness's own no-op inet_lookup() stub -- see harness_stubs.c -- always
# returns NULL too, matching that real failure mode exactly). The very next use of the value
# in almost any expression context (castStringExplicit()'s VT_STRING case, interpreter.c)
# calls strlen() on the NULL data pointer -- a NULL-deref crash under ASan, and a real SIGSEGV
# on a plain unsanitized build. Full writeup, repro and gdb trace at
# mayhem/lci/known-findings/socks-resolv-null-string-deref/README.md, including its own
# one-line upstream fix: return nil instead of a string wrapping NULL. Same shadow-copy
# reasoning as parser.c above -- binding.c is unmodified upstream, so the fix goes into a
# build-time patched copy, never the committed file. The oracle build (section 3) compiles
# the REAL, unpatched binding.c.
BINDING_SHADOW=/tmp/lci-binding-src
rm -rf "$BINDING_SHADOW"
mkdir -p "$BINDING_SHADOW"
cp binding.c "$BINDING_SHADOW/binding.c"
python3 - "$BINDING_SHADOW/binding.c" <<'PYEOF'
import sys

path = sys.argv[1]
src = open(path).read()

OLD = '''\tchar *h = inet_lookup(addr);

\tValueObject *ret = createStringValueObject(h);
\treturn createReturnObject(RT_RETURN, ret);
}'''
NEW = '''\tchar *h = inet_lookup(addr);

\t/* mayhem: h is NULL on any resolution failure (the ordinary case in a
\t * fuzzing sandbox with no DNS) -- wrapping it in a VT_STRING ValueObject
\t * makes the very next use of the value strlen() a NULL pointer. Return
\t * nil instead, mirroring how a failed lookup should surface to LOLCODE.
\t * Injected into a build-time SHADOW COPY of this file by mayhem/build.sh
\t * -- the committed binding.c is unmodified. */
\tValueObject *ret = h ? createStringValueObject(h) : createNilValueObject();
\treturn createReturnObject(RT_RETURN, ret);
}'''

n = src.count(OLD)
if n != 1:
    raise SystemExit(f"binding NULL-deref patch: expected exactly 1 match, found {n}")
src = src.replace(OLD, NEW, 1)

open(path, "w").write(src)
PYEOF

# lci interpreter sources for the HARNESS build; main.c (has its own main()) and inet.c (real
# sockets, replaced by harness_stubs.c) are excluded. interpreter.c/parser.c/tokenizer.c/
# binding.c/vm.c come from the patched shadow copies above; every other source (including
# jit.c -- see VM_SHADOW's comment for why the JIT itself is disabled at runtime instead of
# patched) is compiled straight from the repo, unmodified.
LCI_LIB_SRCS=("$FUZZ_SHADOW/interpreter.c" lexer.c "$PARSER_SHADOW/parser.c" "$TOKENIZER_SHADOW/tokenizer.c" unicode.c error.c "$BINDING_SHADOW/binding.c" intern.c "$VM_SHADOW/vm.c" jit.c)
HARNESS=mayhem/fuzz_lci.c
STUBS=mayhem/harness_stubs.c
# -lm -lncurses -lreadline mirrors upstream's CMake link line (binding.c pulls them in).
LINK_LIBS=(-lm -lncurses -lreadline)
# Redirect binding.c's/interpreter.c's host-facing libc calls to the safe no-ops in
# harness_stubs.c (see that file's header + fuzz_lci.c's for the full rationale). Applying
# these renames to the whole harness compile command is harmless: no other linked source
# calls any of these eight names.
HOST_DENY_DEFS=(
	-Dfopen=mayhem_denied_fopen
	-Dfread=mayhem_denied_fread
	-Dfwrite=mayhem_denied_fwrite
	-Dfclose=mayhem_denied_fclose
	-Drewind=mayhem_denied_rewind
	-Dferror=mayhem_denied_ferror
	-Dpopen=mayhem_denied_popen
	-Dpclose=mayhem_denied_pclose
)
mkdir -p build

# Disable LeakSanitizer preventively for every fuzz/standalone binary (ASan's memory-corruption
# checks + UBSan stay on) -- this fleet fuzzes for memory corruption, not leaks, and an
# unrelated leak (e.g. the interpreter's own error paths) would otherwise halt a run exactly
# like a real crash, including replaying the server-side accumulated corpus. See
# mayhem/lsan_off.c and PORTING.md.
# shellcheck disable=SC2086
$CC -c $SANITIZER_FLAGS $DEBUG_FLAGS -w mayhem/lsan_off.c -o /tmp/lsan_off.o

# 1) Sanitized libFuzzer target — the interpreter sources ARE instrumented (edges) so ASan/UBSan
#    see defects anywhere in the lexer/tokenizer/parser/interpreter. $DEBUG_FLAGS after the
#    sanitizer flags so -gdwarf-3 wins.
# shellcheck disable=SC2086
$CC $SANITIZER_FLAGS $DEBUG_FLAGS $LIB_FUZZING_ENGINE -O1 -w -I. "${HOST_DENY_DEFS[@]}" \
    "$HARNESS" "$STUBS" "${LCI_LIB_SRCS[@]}" /tmp/lsan_off.o -o build/lci "${LINK_LIBS[@]}"

# 2) Standalone (non-fuzzer) reproducer: same harness + sources against the run-once driver (no
#    libFuzzer runtime). Respects $SANITIZER_FLAGS/$DEBUG_FLAGS (so an empty SANITIZER_FLAGS still
#    yields a natural-crash repro with DWARF symbols). Lives at $SRC/lci-standalone (the canonical
#    /mayhem/<fuzzer>-standalone location verify-repo checks).
# shellcheck disable=SC2086
$CC $SANITIZER_FLAGS $DEBUG_FLAGS -O1 -w -I. "${HOST_DENY_DEFS[@]}" \
    "$STANDALONE_FUZZ_MAIN" "$HARNESS" "$STUBS" "${LCI_LIB_SRCS[@]}" /tmp/lsan_off.o -o lci-standalone "${LINK_LIBS[@]}"

# 3) Oracle build: upstream's own CMake project (normal flags, NO sanitizers, REAL bindings incl.
#    inet.c) + its ctest suite, configured against a shadow copy of the tree. This is a fully
#    unmodified/unstubbed lci — its STDIO and socket binding tests
#    (test/1.4-Tests/13-Bindings/{1-stdio,3-socket}) exercise real file and socket code, as
#    upstream intends.
#
#    This integration's original base predated upstream's own "Make the test suite run on a
#    current Python" commit, so the shadow copy used to carry a one-line python3-compat sed for
#    test/testDriver.py (bytes vs str over subprocess.communicate()). Current upstream/future
#    already ships that fix natively -- applying the old sed ON TOP of it now double-patches
#    Popen into text mode while testDriver.py still reads the expected-output fixture as bytes
#    ('rb'), so EVERY output comparison silently mismatches (bytes never equal str in Python 3):
#    verified this is exactly what turned a clean 325/325 ctest pass (pristine, unpatched
#    testDriver.py) into 53/329 once the obsolete sed was reapplied. Removed; the shadow copy
#    below is otherwise unmodified from the checked-in tree.
ALL_SRCS=(interpreter.c lexer.c main.c parser.c tokenizer.c unicode.c error.c binding.c inet.c intern.c vm.c jit.c)
SHADOW=/tmp/lci-tests-src
rm -rf "$SHADOW" build-tests
mkdir -p "$SHADOW"
cp -a CMakeLists.txt cmake test "${ALL_SRCS[@]}" ./*.h "$SHADOW"/
cmake -S "$SHADOW" -B build-tests \
      -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
      -DCMAKE_C_COMPILER="$CC" \
      -DCMAKE_C_FLAGS="-O2 -w $COVERAGE_FLAGS" \
      -DCMAKE_EXE_LINKER_FLAGS="$COVERAGE_FLAGS" >/dev/null
cmake --build build-tests -j"$MAYHEM_JOBS" >/dev/null

echo "build.sh: built build/lci (libFuzzer target), build/lci-standalone (repro), build-tests/ (ctest oracle)"
