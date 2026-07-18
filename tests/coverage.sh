#!/bin/bash
# tests/coverage.sh — build the coverage-instrumented mushcore + tests, run the
# suite, and produce llvm-cov reports over the shipping sources.
#
#   tests/coverage.sh            # summary report (per-file)
#   tests/coverage.sh report     # same
#   tests/coverage.sh export     # lcov-style JSON to build-cov/coverage.json
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
BINARIES=()
for t in "$BUILD"/tests/test_*; do
  [ -x "$t" ] || continue
  name=$(basename "$t")
  LLVM_PROFILE_FILE="$PROFDIR/$name.profraw" "$t" >/dev/null 2>&1 || echo "warn: $name failed"
  BINARIES+=(-object "$t")
done

xcrun llvm-profdata merge -sparse "$PROFDIR"/*.profraw -o "$BUILD/merged.profdata"

# Report over shipping sources only (exclude tests/, third-party, shims-with-no-logic).
IGNORE='tests/|pcre/|sqlite|zlib|lua-5|miniaudio|luacom/|scripting/lpeg/|scripting/number.c|scripting/bits.c|scripting/lrexlib|/usr/|mc_test'

case "$MODE" in
  export)
    xcrun llvm-cov export "${BINARIES[@]}" -instr-profile="$BUILD/merged.profdata" \
      -ignore-filename-regex="$IGNORE" -format=text > "$BUILD/coverage.json"
    echo "wrote $BUILD/coverage.json"
    ;;
  show)
    xcrun llvm-cov show "${BINARIES[@]}" -instr-profile="$BUILD/merged.profdata" "$2"
    ;;
  *)
    xcrun llvm-cov report "${BINARIES[@]}" -instr-profile="$BUILD/merged.profdata" \
      -ignore-filename-regex="$IGNORE"
    ;;
esac
