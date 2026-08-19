#!/usr/bin/env python3
"""Guard: no tracked file may contain an unresolved merge-conflict marker.

Why this exists
----------------
`docs/UPSTREAM-BUGS.md` shipped at HEAD for over a week carrying three literal
git conflict markers -- `<<<<<<< HEAD`, `=======` and `>>>>>>> fix/f24` -- left
behind by a botched merge resolution.  Nothing caught it: the markers sit in
prose, so they render as a slightly odd divider in most editors and GitHub's
own diff view, not as an error.  A build/test lint that greps for git's own
marker syntax would have failed the moment the bad merge landed instead of
sitting undetected for a week.

This scans every file `git ls-files` tracks -- source, docs, everything, the
same "the tree" a human would mean by "grep the tree" -- for:

  * a line starting `<<<<<<< ` (git always follows it with a ref/branch name
    and a space, so this shape is unambiguous -- nothing legitimate starts a
    line that way), or
  * a line starting `>>>>>>> ` (same reasoning, the other side of a conflict), or
  * a bare `=======` line sitting within a few lines of one of the above.

The bare `=======` marker is only flagged near a `<<<<<<<`/`>>>>>>>` line
because, unlike the other two, a lone row of seven equals signs *could*
legitimately appear as a plain-text divider; the proximity check is what
tells a real conflict remnant apart from that.

Usage: check_conflict_markers.py [repo-root]
Exit status 0 = clean, 1 = violations found (paths printed to stdout).
"""

import os
import subprocess
import sys

START = "<<<<<<< "
END = ">>>>>>> "
MID = "======="

# How many lines either side of a bare "=======" make it count as part of a
# conflict block rather than an unrelated divider.  Wide on purpose: the real
# incident this guards against (docs/UPSTREAM-BUGS.md, 2026-08) had the
# "=======" 24 lines from the nearest other marker because the markers had
# been left out of order by a later hand-repair, not a fresh unresolved merge.
PROXIMITY = 50


def tracked_files(root):
    out = subprocess.check_output(
        ["git", "-C", root, "ls-files", "-z"])
    return [p for p in out.decode("utf-8", "surrogateescape").split("\0") if p]


def check_file(path):
    """Return a list of (lineno, line) violations in `path`."""
    with open(path, "rb") as fh:
        raw = fh.read()
    # Decode 1 byte == 1 char so this never raises on a binary file; a
    # conflict marker is plain ASCII, so this is exact for the thing we
    # actually look for.
    lines = raw.decode("latin-1").split("\n")

    anchors = [i for i, line in enumerate(lines)
               if line.startswith(START) or line.startswith(END)]

    bad = []
    for i, line in enumerate(lines):
        if line.startswith(START) or line.startswith(END):
            bad.append(i)
        elif line == MID and any(abs(i - a) <= PROXIMITY for a in anchors):
            bad.append(i)

    return [(i + 1, lines[i]) for i in sorted(bad)]


# ── selftest ─────────────────────────────────────────────────────────────────
# Mirrors check_ascii_literals.py's own approach: a lint that silently stops
# finding things is worse than none, so it carries a fixture that must catch a
# planted real conflict block AND must NOT flag a lone "=======" divider that
# has no conflict markers anywhere near it.
#
# The fixture is built from START/END/MID rather than written out literally --
# a literal `<<<<<<< HEAD` line right here would make this checker flag its
# OWN source file when it scans the tree.
SELFTEST_LINES = (
    ["line one, ordinary text",
     START + "HEAD",
     "our side",
     MID,
     "their side",
     END + "some-branch",
     "line seven, ordinary text"]
    + ["filler"] * 60
    + [MID, "trailing filler"]
)
SELFTEST_SOURCE = "\n".join(SELFTEST_LINES) + "\n"


def selftest():
    import tempfile

    expected = {2, 4, 6}         # the real conflict block, all three markers
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "probe.txt")
        with open(path, "wb") as fh:
            fh.write(SELFTEST_SOURCE.encode("utf-8"))
        found = {lineno for lineno, _line in check_file(path)}

    missed = expected - found
    spurious = found - expected
    for lineno in sorted(missed):
        print("SELFTEST FAIL: line %d should have been flagged but was not"
              % lineno)
    for lineno in sorted(spurious):
        print("SELFTEST FAIL: line %d was flagged but is not part of a "
              "real conflict block" % lineno)
    if missed or spurious:
        print("selftest FAILED (%d missed, %d spurious)"
              % (len(missed), len(spurious)))
        return 1
    print("selftest OK: %d violations detected, the lone divider far from "
          "any marker was correctly ignored" % len(expected))
    return 0


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--selftest":
        return selftest()

    root = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(
        os.path.dirname(os.path.abspath(__file__)))

    failures = 0
    checked = 0
    for rel in tracked_files(root):
        path = os.path.join(root, rel)
        if not os.path.isfile(path):
            continue                      # e.g. a submodule gitlink entry
        checked += 1
        for lineno, line in check_file(path):
            failures += 1
            print("%s:%d: unresolved merge-conflict marker\n    %s"
                  % (rel, lineno, line))

    if failures:
        print("\n%d conflict marker(s) found.  These are leftovers from an "
              "unresolved or\nmis-resolved merge -- remove them (and check "
              "the surrounding text reads\ncorrectly) before committing."
              % failures)
        return 1

    print("checked %d tracked files: no unresolved conflict markers" % checked)
    return 0


if __name__ == "__main__":
    sys.exit(main())
