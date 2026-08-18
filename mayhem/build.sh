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
# writeup; here we apply the three BUILD-TIME mitigations it depends on:
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
#     error instead of spinning until fuzz_lci.c's process-level watchdog kills the
#     whole libFuzzer worker. See FUZZ_SHADOW below for why this has to be a
#     source-level injection (a link-time `ld --wrap=interpretStmtNode` was tried
#     first specifically to avoid ANY interpreter.c-shaped copy at all, and does not
#     work on this toolchain -- verified empirically) and why a shadow copy rather
#     than an in-repo edit keeps the integration purely additive.
#
# We build three artifacts from the upstream sources:
#   build/lci             sanitized (ASan+UBSan, halting) + DWARF-3 libFuzzer target -> the Mayhem target
#   build/lci-standalone  same harness against $STANDALONE_FUZZ_MAIN (run-once repro, no libFuzzer rt)
#   build-tests/          upstream CMake+ctest suite (normal flags, REAL bindings) -> mayhem/test.sh oracle
#
# The oracle build is upstream's own CMake+ctest suite (~325 golden-output tests driven by
# test/testDriver.py). Two additive accommodations, applied to a SHADOW COPY of the source tree
# (never to upstream files in-repo):
#   * testDriver.py predates python3: subprocess.communicate() returns bytes but the expected
#     output is read as str, so every golden-output comparison would fail. The shadow copy gets
#     text-mode pipes (universal_newlines=True) — test semantics are unchanged.
#   * CMakeLists.txt declares cmake_minimum_required(2.8), which cmake >= 3.31 rejects outright;
#     -DCMAKE_POLICY_VERSION_MINIMUM=3.5 lets configure proceed.
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
# per-statement cost is high enough that a "generous" 5,000,000 took ~12s wall time -- ten
# times past fuzz_lci.c's 1.5s watchdog deadline, so the watchdog fired first and silently
# defeated the whole point of a fast in-process bailout. 50,000 leaves roughly 2x wall-clock
# headroom under the watchdog even accounting for host load, while still being far more than
# any legitimate seed in mayhem/lci/testsuite/ needs (verified: -runs=1 on every seed
# individually, and a -merge=1 coverage pass over the whole directory, both exit 0 in
# low-single-digit milliseconds of libFuzzer-reported execution time).
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
 * of spinning until mayhem/fuzz_lci.c's process-level watchdog kills the whole
 * worker. Injected into a build-time SHADOW COPY of this file by mayhem/build.sh
 * (see that file's header comment) -- the committed interpreter.c is unmodified.
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
\tif (!main) return 1;
\tret = interpretBlockNode(main->block, scope);'''
MAIN_NEW = '''int interpretMainNodeScope(MainNode *main, ScopeObject *scope)
{
\tReturnObject *ret = NULL;
\tif (!main) return 1;
\tMAYHEM_BUDGET_RESET();
\tret = interpretBlockNode(main->block, scope);'''
src = replace_once(src, MAIN_OLD, MAIN_NEW, "interpretMainNodeScope body")

open(path, "w").write(src)
PYEOF

# lci interpreter sources for the HARNESS build; main.c (has its own main()) and inet.c (real
# sockets, replaced by harness_stubs.c) are excluded. interpreter.c comes from the patched
# shadow copy above; every other source is compiled straight from the repo, unmodified.
LCI_LIB_SRCS=("$FUZZ_SHADOW/interpreter.c" lexer.c parser.c tokenizer.c unicode.c error.c binding.c)
HARNESS=mayhem/fuzz_lci.c
STUBS=mayhem/harness_stubs.c
# -lm -lncurses -lreadline mirrors upstream's CMake link line (binding.c pulls them in).
# -lrt is for fuzz_lci.c's timer_create()/timer_settime() watchdog (harmless/no-op link on
# glibc >= 2.34, where librt's symbols moved into libc but the compat shim stays present).
LINK_LIBS=(-lm -lncurses -lreadline -lrt)
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

# 1) Sanitized libFuzzer target — the interpreter sources ARE instrumented (edges) so ASan/UBSan
#    see defects anywhere in the lexer/tokenizer/parser/interpreter. $DEBUG_FLAGS after the
#    sanitizer flags so -gdwarf-3 wins.
# shellcheck disable=SC2086
$CC $SANITIZER_FLAGS $DEBUG_FLAGS $LIB_FUZZING_ENGINE -O1 -w -I. "${HOST_DENY_DEFS[@]}" \
    "$HARNESS" "$STUBS" "${LCI_LIB_SRCS[@]}" -o build/lci "${LINK_LIBS[@]}"

# 2) Standalone (non-fuzzer) reproducer: same harness + sources against the run-once driver (no
#    libFuzzer runtime). Respects $SANITIZER_FLAGS/$DEBUG_FLAGS (so an empty SANITIZER_FLAGS still
#    yields a natural-crash repro with DWARF symbols). Lives at $SRC/lci-standalone (the canonical
#    /mayhem/<fuzzer>-standalone location verify-repo checks).
# shellcheck disable=SC2086
$CC $SANITIZER_FLAGS $DEBUG_FLAGS -O1 -w -I. "${HOST_DENY_DEFS[@]}" \
    "$STANDALONE_FUZZ_MAIN" "$HARNESS" "$STUBS" "${LCI_LIB_SRCS[@]}" -o lci-standalone "${LINK_LIBS[@]}"

# 3) Oracle build: upstream's own CMake project (normal flags, NO sanitizers, REAL bindings incl.
#    inet.c) + its ctest suite, configured against a shadow copy of the tree carrying the python3
#    driver fix. This is a fully unmodified/unstubbed lci — its STDIO and socket binding tests
#    (test/1.4-Tests/13-Bindings/{1-stdio,3-socket}) exercise real file and socket code, as
#    upstream intends.
ALL_SRCS=(interpreter.c lexer.c main.c parser.c tokenizer.c unicode.c error.c binding.c inet.c)
SHADOW=/tmp/lci-tests-src
rm -rf "$SHADOW" build-tests
mkdir -p "$SHADOW"
cp -a CMakeLists.txt cmake test "${ALL_SRCS[@]}" ./*.h "$SHADOW"/
sed -i 's/stderr=subprocess.PIPE)/stderr=subprocess.PIPE, universal_newlines=True)/' \
    "$SHADOW/test/testDriver.py"
grep -q universal_newlines "$SHADOW/test/testDriver.py"  # the driver fix must have applied
cmake -S "$SHADOW" -B build-tests \
      -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
      -DCMAKE_C_COMPILER="$CC" \
      -DCMAKE_C_FLAGS="-O2 -w $COVERAGE_FLAGS" \
      -DCMAKE_EXE_LINKER_FLAGS="$COVERAGE_FLAGS" >/dev/null
cmake --build build-tests -j"$MAYHEM_JOBS" >/dev/null

echo "build.sh: built build/lci (libFuzzer target), build/lci-standalone (repro), build-tests/ (ctest oracle)"
