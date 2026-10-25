#!/usr/bin/env python3
"""Guard: certain test helper types may be DEFINED in at most one file under tests/.

Why this exists
----------------
The `HookGuard<T>` RAII template — including its four-line preceding comment
— was pasted byte-identical into three test files (test_mfc_ui.cpp,
test_triggers.cpp, test_lua_scripting.cpp), all by the same commit (99e3c84)
that, the same day, extracted `RecordingTransport` into tests/fake_transport.h
specifically to stop this shape from recurring.  Nothing caught the copy
because every file compiles and passes on its own — duplication across
translation units is invisible to a per-file build.

`tests/mc_test.h` is already included by every test .cpp in the suite, so any
helper meant to be shared belongs there, once.  This lint greps every tracked
file under tests/ for a `struct <name> {` definition of each name in
DUPLICATE_GUARD_SYMBOLS and fails if more than one file defines it — the
`.h` it belongs in doesn't count as a "copy" of itself, so mc_test.h is
exactly the one file this allows.

Usage: check_duplicate_test_helpers.py [repo-root]
Exit status 0 = clean, 1 = violations found (paths printed to stdout).
"""

import os
import re
import subprocess
import sys

# Names that must be defined in exactly one file under tests/.  Add to this
# list if another helper gets copy-pasted the way HookGuard was; the lint
# then guards it for free.
DUPLICATE_GUARD_SYMBOLS = ("HookGuard",)

TESTS_SUBDIR = "tests"


def tracked_test_files(root):
    out = subprocess.check_output(
        ["git", "-C", root, "ls-files", "-z", "--", TESTS_SUBDIR])
    paths = [p for p in out.decode("utf-8", "surrogateescape").split("\0") if p]
    return [p for p in paths if p.endswith((".cpp", ".h", ".hpp"))]


def definitions_of(symbol, root, files):
    """Return the list of relative paths that DEFINE `struct symbol {`."""
    pattern = re.compile(r"\bstruct\s+" + re.escape(symbol) + r"\b")
    hits = []
    for rel in files:
        path = os.path.join(root, rel)
        if not os.path.isfile(path):
            continue                      # e.g. a submodule gitlink entry
        with open(path, "rb") as fh:
            raw = fh.read()
        text = raw.decode("latin-1")      # 1 byte == 1 char; never raises
        if pattern.search(text):
            hits.append(rel)
    return hits


# ── selftest ─────────────────────────────────────────────────────────────────
# Mirrors check_ascii_literals.py / check_conflict_markers.py: the lint must
# catch a planted duplicate AND must NOT flag a symbol defined exactly once,
# or a lint that never fires is worse than none.
def selftest():
    import tempfile

    ok = True

    with tempfile.TemporaryDirectory() as tmp:
        # Two files defining the same helper: must be flagged.
        for name in ("a.cpp", "b.cpp"):
            with open(os.path.join(tmp, name), "w") as fh:
                fh.write("template <typename T>\nstruct ProbeGuard { T slot; };\n")
        hits = definitions_of("ProbeGuard", tmp, ["a.cpp", "b.cpp"])
        if sorted(hits) != ["a.cpp", "b.cpp"]:
            print("SELFTEST FAIL: expected both copies flagged, got %r" % (hits,))
            ok = False

    with tempfile.TemporaryDirectory() as tmp:
        # One file only: must NOT be flagged.
        with open(os.path.join(tmp, "only.cpp"), "w") as fh:
            fh.write("struct ProbeGuard { int slot; };\n")
        with open(os.path.join(tmp, "user.cpp"), "w") as fh:
            fh.write("void f() { ProbeGuard<int> g(x); }\n")   # a USE, not a def
        hits = definitions_of("ProbeGuard", tmp, ["only.cpp", "user.cpp"])
        if hits != ["only.cpp"]:
            print("SELFTEST FAIL: expected exactly one file flagged, got %r"
                  % (hits,))
            ok = False

    if not ok:
        print("selftest FAILED")
        return 1
    print("selftest OK: duplicate definitions are caught, a single "
          "definition (and mere uses of it) are not")
    return 0


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--selftest":
        return selftest()

    root = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(
        os.path.dirname(os.path.abspath(__file__)))

    files = tracked_test_files(root)
    failures = 0
    for symbol in DUPLICATE_GUARD_SYMBOLS:
        hits = definitions_of(symbol, root, files)
        if len(hits) > 1:
            failures += 1
            print("struct %s is defined in %d files under tests/ (should be "
                  "exactly 1, in mc_test.h):" % (symbol, len(hits)))
            for rel in hits:
                print("    %s" % rel)

    if failures:
        print("\n%d duplicated test helper(s) found.  Move the definition "
              "into tests/mc_test.h\n(every test .cpp already includes it) "
              "and delete the other copies." % failures)
        return 1

    print("checked %d symbol(s) across %d tracked files under tests/: no "
          "duplicate definitions" % (len(DUPLICATE_GUARD_SYMBOLS), len(files)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
