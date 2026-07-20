#!/bin/bash
# tests/mutate.sh — mutation-testing harness.
#
# Applies ONE source mutation, incrementally rebuilds build/, runs a test
# binary (or the whole suite), reports whether the test suite CAUGHT the bug,
# then always restores the file.
#
#   tests/mutate.sh FILE 'OLD' 'NEW' [test_binary]
#
# OLD must be a SINGLE-LINE snippet occurring EXACTLY ONCE in FILE (byte match,
# so CRLF is preserved).  test_binary is a name under build/tests (e.g.
# test_triggers); omit to run the full ctest.
#
# Verdicts:
#   KILLED     — the test(s) FAILED with the mutation → the code IS covered
#                correctly (good).
#   SURVIVED   — the test(s) PASSED despite the bug → the line executes but is
#                NOT meaningfully asserted (a weak/vacuous test — investigate).
#   BUILD-BROKE— the mutation didn't compile (equivalent mutant; inconclusive).
#   APPLY-FAIL — OLD not found exactly once (fix the spec).
#
# The file is restored via `git checkout` on exit (requires a clean working
# copy of FILE beforehand — the harness refuses otherwise).
set -uo pipefail
cd "$(dirname "$0")/.."

FILE="${1:?usage: mutate.sh FILE OLD NEW [testbin]}"
OLD="${2:?}"; NEW="${3:?}"; TESTBIN="${4:-}"

if ! git diff --quiet -- "$FILE"; then
  echo "REFUSE: $FILE has uncommitted changes; harness restores via git checkout." >&2
  exit 4
fi

restore() { git checkout -- "$FILE" 2>/dev/null; }
trap restore EXIT

# Apply the mutation at the byte level (CRLF-safe); require exactly one match.
python3 - "$FILE" "$OLD" "$NEW" <<'PY' || exit 3
import sys
f, old, new = sys.argv[1], sys.argv[2].encode(), sys.argv[3].encode()
raw = open(f, 'rb').read()
n = raw.count(old)
if n != 1:
    sys.stderr.write("APPLY-FAIL: pattern found %d times (need exactly 1)\n" % n)
    sys.exit(3)
open(f, 'wb').write(raw.replace(old, new))
PY

if ! cmake --build build -j8 >/tmp/mutate_build.log 2>&1; then
  echo "BUILD-BROKE  ($FILE: '$OLD' -> '$NEW' did not compile)"
  exit 2
fi

if [ -n "$TESTBIN" ]; then
  if ./build/tests/"$TESTBIN" >/tmp/mutate_test.log 2>&1; then
    echo "SURVIVED  ($TESTBIN PASSED with the bug — weak/vacuous coverage)"
  else
    echo "KILLED    ($TESTBIN failed as it should)"
  fi
else
  if ctest --test-dir build --output-on-failure >/tmp/mutate_test.log 2>&1; then
    echo "SURVIVED  (full ctest PASSED with the bug)"
  else
    echo "KILLED    ($(grep -c 'Failed' /tmp/mutate_test.log) test(s) failed)"
  fi
fi
