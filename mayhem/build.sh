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
# writeup; here we apply the two BUILD-TIME mitigations it depends on:
#   * binding.c and interpreter.c are compiled for the harness with -D renames that
#     redirect fopen/fread/fwrite/fclose/rewind/ferror/popen/pclose to the safe
#     no-ops in mayhem/harness_stubs.c (never applied to the oracle build below).
#   * inet.c (real TCP sockets) is left OUT of the harness's source list; the same
#     harness_stubs.c provides no-op inet_* definitions instead.
# An unbounded LOLCODE loop (IM IN YR <loop> whose guard/body never makes it stop)
# is NOT mitigated here: it is an ordinary libFuzzer timeout under the Mayhemfile's
# cmd-level `timeout: 30`, exactly like any other hang (docs/netnew-worker-prompt.md
# SS6b) -- interpreter.c itself is compiled unmodified for the harness, same as
# every other lci source file.
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

# lci interpreter sources for the HARNESS build; main.c (has its own main()) and inet.c (real
# sockets, replaced by harness_stubs.c) are excluded. Every source, including interpreter.c,
# is compiled straight from the repo, unmodified.
LCI_LIB_SRCS=(interpreter.c lexer.c parser.c tokenizer.c unicode.c error.c binding.c)
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
