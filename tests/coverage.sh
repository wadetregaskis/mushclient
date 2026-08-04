#!/bin/bash
# tests/coverage.sh — build the coverage-instrumented mushcore + tests, run the
# suite, and produce llvm-cov reports over the shipping sources.
#
#   tests/coverage.sh            # summary report (per-file)
#   tests/coverage.sh report     # same
#   tests/coverage.sh export     # llvm-cov JSON to build-cov/coverage.json
#
# COVERAGE_ALLOW_DIRTY=1 accepts a run in which some suite failed or died.
# The numbers will be an UNDER-count; see the guard below for why that is
# refused by default.
#   tests/coverage.sh show FILE  # annotated source for one file
#
# Uses the Xcode LLVM toolchain (llvm-profdata / llvm-cov via xcrun).
set -euo pipefail
cd "$(dirname "$0")/.."

BUILD=build-cov
MODE="${1:-report}"

cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=Debug -DMUSHCLIENT_COVERAGE=ON >/dev/null
cmake --build "$BUILD" -j8 -- -k 2>/dev/null | tail -1 || cmake --build "$BUILD" -j8 | tail -1

PROFDIR="$BUILD/profraw"
rm -rf "$PROFDIR"; mkdir -p "$PROFDIR"

# Run every mushcore test executable with a distinct profile file.
# llvm-cov wants ONE positional binary (FIRST) plus the rest as -object (OBJS);
# passing every binary as -object leaves no positional slot, so `show FILE`
# mis-reads the source path as an object file.  Keep both forms.
FIRST=""
OBJS=()
DIRTY=()
for t in "$BUILD"/tests/test_*; do
  [ -x "$t" ] || continue
  name=$(basename "$t")
  rc=0
  LLVM_PROFILE_FILE="$PROFDIR/$name.profraw" "$t" >/dev/null 2>&1 || rc=$?
  # TWO distinct ways a suite degrades the profile, and both have to be caught:
  #  - it FAILS its assertions.  main() still returns, so the profile runtime's
  #    atexit hook writes a full-shaped .profraw and it IS merged -- with
  #    whatever coverage the run happened to reach before giving up.
  #  - it dies on a signal or is killed.  Nothing is written at all, so the
  #    merge input set is simply shorter and llvm-profdata accepts it without
  #    a word.  Only the file check sees this one.
  if [ "$rc" != 0 ]; then
    echo "warn: $name failed (exit $rc)" >&2
    DIRTY+=("$name")
  elif [ ! -s "$PROFDIR/$name.profraw" ]; then
    echo "warn: $name wrote no profile" >&2
    DIRTY+=("$name")
  fi
  if [ -z "$FIRST" ]; then FIRST="$t"; else OBJS+=(-object "$t"); fi
done
[ -n "$FIRST" ] || { echo "error: no test executables under $BUILD/tests - build failed?" >&2; exit 1; }

# Do not let a degraded run replace a good profile.  llvm-profdata merges a
# short or under-exercised input set without complaint, so ONE flaky suite
# silently swaps merged.profdata for a worse one -- and the next report shows a
# well-covered file at a fraction of its real number.  That happened during the
# 2026-08-02 audit: a timing-sensitive transport test did not contribute on one
# run and posix_transport.cpp briefly read 13%.
if [ ${#DIRTY[@]} -gt 0 ] && [ "${COVERAGE_ALLOW_DIRTY:-0}" != 1 ]; then
  echo "error: ${#DIRTY[@]} suite(s) did not run clean: ${DIRTY[*]}" >&2
  echo "       keeping the existing $BUILD/merged.profdata rather than replacing" >&2
  echo "       it with an under-counted one.  Re-run when the suite is clean, or" >&2
  echo "       set COVERAGE_ALLOW_DIRTY=1 to accept it." >&2
  exit 1
fi

# Merge to a temp file and move it into place only on success, so an interrupted
# or failing merge cannot leave a truncated merged.profdata behind either.
xcrun llvm-profdata merge -sparse "$PROFDIR"/*.profraw -o "$BUILD/merged.profdata.new"
mv -f "$BUILD/merged.profdata.new" "$BUILD/merged.profdata"

# Report over shipping sources only (exclude tests/, third-party, shims-with-no-logic).
IGNORE='tests/|pcre/|sqlite|zlib|lua-5|miniaudio|luacom/|scripting/lpeg/|scripting/number.c|scripting/bits.c|scripting/lrexlib|/usr/|mc_test'

# bash 3.2 (what macOS ships as /bin/bash) treats "${OBJS[@]}" on an EMPTY array
# as an unbound variable under `set -u`, so a tree holding a single test binary
# would die with "OBJS[@]: unbound variable" instead of reporting.  Hence the
# ${OBJS[@]+"${OBJS[@]}"} form below.
case "$MODE" in
  export)
    xcrun llvm-cov export "$FIRST" ${OBJS[@]+"${OBJS[@]}"} -instr-profile="$BUILD/merged.profdata" \
      -ignore-filename-regex="$IGNORE" -format=text > "$BUILD/coverage.json"
    echo "wrote $BUILD/coverage.json"
    ;;
  show)
    # first binary positional, rest -object, THEN the source path (positional)
    xcrun llvm-cov show "$FIRST" ${OBJS[@]+"${OBJS[@]}"} -instr-profile="$BUILD/merged.profdata" "$2"
    ;;
  *)
    xcrun llvm-cov report "$FIRST" ${OBJS[@]+"${OBJS[@]}"} -instr-profile="$BUILD/merged.profdata" \
      -ignore-filename-regex="$IGNORE"
    ;;
esac
