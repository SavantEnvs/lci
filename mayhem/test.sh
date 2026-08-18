#!/usr/bin/env bash
#
# mayhem/test.sh — RUN lci's upstream ctest suite (built by mayhem/build.sh into build-tests/,
# from a fully unmodified/unstubbed lci — see build.sh's oracle-build comment). ~325 golden-output
# tests: each runs `lci <test.lol>` via test/testDriver.py and diffs stdout against the committed
# expected output (or asserts an error is raised) — a behavioral known-answer oracle straight from
# upstream. Independent of, and unaffected by, the fuzz harness's host-primitive neutering (that
# only applies to build/lci and lci-standalone, never to build-tests/).
set -uo pipefail
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH
: "${MAYHEM_JOBS:=$(nproc)}"
cd "${SRC:-/mayhem}"

emit_ctrf() {
  local tool="$1" passed="$2" failed="$3" skipped="${4:-0}" pending="${5:-0}" other="${6:-0}"
  local tests=$(( passed + failed + skipped + pending + other ))
  cat > "${CTRF_REPORT:-$SRC/ctrf-report.json}" <<JSON
{
  "results": {
    "tool": { "name": "$tool" },
    "summary": {
      "tests": $tests,
      "passed": $passed,
      "failed": $failed,
      "pending": $pending,
      "skipped": $skipped,
      "other": $other
    }
  }
}
JSON
  printf 'CTRF {"results":{"tool":{"name":"%s"},"summary":{"tests":%d,"passed":%d,"failed":%d,"pending":%d,"skipped":%d,"other":%d}}}\n' \
    "$tool" "$tests" "$passed" "$failed" "$pending" "$skipped" "$other"
  [ "$failed" -eq 0 ]
}

[ -x build-tests/lci ] || { echo "FATAL: build-tests/lci missing — mayhem/build.sh must build the test suite" >&2; emit_ctrf cmake-ctest 0 1; exit 1; }

# Direct KAT probes through the dynamically-linked build-tests/lci binary itself, ahead of the
# ctest runner: ctest/CMake test binaries only report a case as failed/passed by the launched
# process's exit status, and (per docs/netnew-worker-prompt.md SS4) that alone is not proof the
# runner actually read the fixture under sabotage -- so assert exact stdout independently here.
KAT_HELLO=$(printf 'HAI 1.3\n\tVISIBLE "HELLO WORLD"\nKTHXBYE\n' | build-tests/lci -)
KAT_SUM=$(printf 'HAI 1.3\n\tVISIBLE SUM OF 40 AN 2\nKTHXBYE\n' | build-tests/lci -)
KAT_LOOP=$(printf 'HAI 1.3\n\tIM IN YR loop UPPIN YR i TIL BOTH SAEM i AN 3\n\t\tVISIBLE i\n\tIM OUTTA YR loop\nKTHXBYE\n' | build-tests/lci -)
KAT_CONCAT=$(printf 'HAI 1.3\n\tVISIBLE SMOOSH "foo" AN "bar" MKAY\nKTHXBYE\n' | build-tests/lci -)

kat_failed=0
echo "$KAT_HELLO" | grep -qxF "HELLO WORLD" || { echo "KAT FAIL: hello-world VISIBLE mismatch: [$KAT_HELLO]" >&2; kat_failed=1; }
echo "$KAT_SUM"   | grep -qxF "42"           || { echo "KAT FAIL: SUM OF 40 AN 2 mismatch: [$KAT_SUM]"       >&2; kat_failed=1; }
printf '%s\n' "$KAT_LOOP" | grep -qxF "0" && printf '%s\n' "$KAT_LOOP" | grep -qxF "1" && printf '%s\n' "$KAT_LOOP" | grep -qxF "2" \
  || { echo "KAT FAIL: IM IN YR loop 0..2 mismatch: [$KAT_LOOP]" >&2; kat_failed=1; }
echo "$KAT_CONCAT" | grep -qxF "foobar"      || { echo "KAT FAIL: SMOOSH foo bar mismatch: [$KAT_CONCAT]"    >&2; kat_failed=1; }

if [ "$kat_failed" -ne 0 ]; then
  echo "FATAL: one or more direct KAT probes failed (see above)" >&2
  emit_ctrf cmake-ctest 0 1
  exit 1
fi
echo "KAT probes OK: KAT_HELLO=HELLO WORLD KAT_SUM=42 KAT_LOOP=0,1,2 KAT_CONCAT=foobar"

LOG=$(ctest --test-dir build-tests -j"$MAYHEM_JOBS" --output-on-failure 2>&1)
rc=$?
echo "$LOG" | tail -20

# ctest summary: "100% tests passed, 0 tests failed out of 325"
TOTAL=$(echo "$LOG"  | sed -n 's/.*tests failed out of \([0-9]\+\).*/\1/p' | tail -1)
FAILED=$(echo "$LOG" | sed -n 's/.*, \([0-9]\+\) tests failed out of.*/\1/p' | tail -1)
if [ -z "$TOTAL" ] || [ -z "$FAILED" ]; then
  echo "FATAL: could not parse ctest summary (ctest rc=$rc)" >&2
  emit_ctrf cmake-ctest 0 1
  exit 1
fi
# Fold the 4 direct KAT probes into the reported total/passed counts (they already ran above and
# are unconditional -- a missing binary or wrong output already exited non-zero).
KAT_TOTAL=4
PASSED=$(( TOTAL - FAILED + KAT_TOTAL ))
TOTAL=$(( TOTAL + KAT_TOTAL ))

emit_ctrf cmake-ctest "$PASSED" "$FAILED"
