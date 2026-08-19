#!/usr/bin/env python3
"""Guard: docs/UPSTREAM-BUGS.md's index table and '## N' sections stay 1:1.

Why this exists
----------------
The file has an index table at the top (`| # | Area | Port commit | ... |`)
and a numbered `## N — ...` write-up section per row, one row's story told
twice for two different readers (skim the table; read the section).  They
drifted apart repeatedly, in both directions, because parallel fix commits
each picked "the next number" from a stale view of the file and collided at
merge time:

  * Table rows 17 and 19 had no matching `## 17` / `## 19` section at all
    (the write-up was simply never added, even though row 19's own commit
    message said "Recorded as UPSTREAM-BUGS entry 19").
  * `## 15` and `## 16` were each written TWICE, for two different bugs,
    because two sections were appended under the same next-available number.
  * From row 52 onward the table and the sections disagreed on which number
    named which bug for eleven rows straight, and nine further sections
    (54 more of them once the run was traced out) had no table row at all.

None of that is visible from reading either the table or the sections in
isolation -- only cross-checking the two catches it, which is what a human
skim of the file does not reliably do (this rotted for weeks before an audit
caught it).  This lint does that cross-check on every `ctest` run instead.

What it checks, for the numbered rows/sections of docs/UPSTREAM-BUGS.md:

  1. the table's row numbers are exactly 1..N, strictly increasing, no gaps,
     no duplicates;
  2. the file's `## N` section headers are exactly the same set 1..N;
  3. the `## N` sections appear in the file in strictly increasing numeric
     order (so a section can't be numbered right but physically misplaced).

It deliberately does NOT touch the file's few intentionally unnumbered
addenda (`## PNG loader ...`, `` ## `AltV` is published ... ``) -- those are
a documented exception (see docs/UPSTREAM-BUGS.md's own precedent) and never
had table rows to begin with, so they are simply invisible to the `## N`
pattern this lint matches.

Usage: check_doc_sections.py [repo-root]
Exit status 0 = clean, 1 = violations found (details printed to stdout).
"""

import os
import re
import sys

DOC_PATH = os.path.join("docs", "UPSTREAM-BUGS.md")

TABLE_ROW_RE = re.compile(r"^\|\s*(\d+)\s*\|")
SECTION_RE = re.compile(r"^## (\d+) ")


def parse(text):
    """Return (table_row_numbers, section_numbers_in_file_order)."""
    lines = text.split("\n")

    table_rows = []
    in_table = False
    for line in lines:
        if line.startswith("| # |"):
            in_table = True
            continue
        if not in_table:
            continue
        if line.startswith("|---"):
            continue
        m = TABLE_ROW_RE.match(line)
        if m:
            table_rows.append(int(m.group(1)))
            continue
        if line.startswith("|"):
            continue          # some other table row shape; ignore
        in_table = False      # first non-table line ends the table

    sections = []
    for line in lines:
        m = SECTION_RE.match(line)
        if m:
            sections.append(int(m.group(1)))

    return table_rows, sections


def check(text):
    """Return a list of human-readable violation strings; [] if clean."""
    table_rows, sections = parse(text)
    problems = []

    if not table_rows:
        problems.append("no index-table rows found (table header '| # |' "
                         "missing or table empty)")
        return problems

    # 1. Table rows: exactly 1..N, strictly increasing, no gaps/dupes.
    expected = list(range(1, max(table_rows) + 1))
    if table_rows != expected:
        seen = set()
        dupes = sorted({n for n in table_rows if n in seen or seen.add(n)})
        missing = sorted(set(expected) - set(table_rows))
        out_of_order = table_rows != sorted(table_rows)
        if dupes:
            problems.append("table row number(s) repeated: %s"
                             % ", ".join(str(n) for n in dupes))
        if missing:
            problems.append("table row number(s) missing (gap in 1..%d): %s"
                             % (max(table_rows),
                                ", ".join(str(n) for n in missing)))
        if out_of_order and not dupes and not missing:
            problems.append("table rows are not in strictly increasing "
                             "numeric order: %s" % table_rows)

    # 2. Section headers: same set as table rows.
    table_set = set(table_rows)
    section_set = set(sections)

    rows_without_section = sorted(table_set - section_set)
    if rows_without_section:
        problems.append(
            "table row(s) with no matching '## N' section: %s"
            % ", ".join(str(n) for n in rows_without_section))

    sections_without_row = sorted(section_set - table_set)
    if sections_without_row:
        problems.append(
            "'## N' section(s) with no matching table row: %s"
            % ", ".join(str(n) for n in sections_without_row))

    # 3. Section headers: no duplicate numbers, and appear in increasing
    #    numeric order as they occur in the file (independent of #2 above,
    #    since a duplicate can still satisfy the set-equality check).
    seen = set()
    dupes = sorted({n for n in sections if n in seen or seen.add(n)})
    if dupes:
        problems.append(
            "section number(s) used more than once ('## N' written twice "
            "for two different bugs): %s"
            % ", ".join(str(n) for n in dupes))

    if not dupes and sections != sorted(sections):
        problems.append(
            "'## N' sections are not physically in increasing numeric "
            "order in the file: %s" % sections)

    return problems


# ── selftest ─────────────────────────────────────────────────────────────────
# Mirrors check_conflict_markers.py / check_duplicate_test_helpers.py: the
# lint must catch each planted violation AND must NOT flag a clean fixture,
# or a lint that never fires is worse than none.
def _fixture(table_rows, section_lines):
    """Build a minimal doc: an index table for `table_rows`, then whatever
    '## ...' lines the caller supplies (verbatim, so a bad fixture can omit,
    duplicate or reorder sections on purpose)."""
    lines = ["# fixture", "", "| # | Area |", "|---|------|"]
    for n in table_rows:
        lines.append("| %d | row %d |" % (n, n))
    lines.append("")
    lines.extend(section_lines)
    return "\n".join(lines) + "\n"


def _clean_sections(nums):
    out = []
    for n in nums:
        out.append("## %d — bug number %d" % (n, n))
        out.append("**Bug:** ...")
        out.append("**Fix:** ...")
        out.append("")
    return out


def selftest():
    ok = True

    def expect_clean(name, text):
        nonlocal ok
        problems = check(text)
        if problems:
            print("SELFTEST FAIL (%s): expected no violations, got: %s"
                  % (name, problems))
            ok = False

    def expect_dirty(name, text, must_mention):
        nonlocal ok
        problems = check(text)
        joined = " | ".join(problems)
        if not problems:
            print("SELFTEST FAIL (%s): expected a violation, got none" % name)
            ok = False
        elif must_mention not in joined:
            print("SELFTEST FAIL (%s): expected a violation mentioning %r, "
                  "got: %s" % (name, must_mention, problems))
            ok = False

    # A well-formed doc: rows 1..5, matching sections in order.  Must pass.
    expect_clean("clean 1..5",
                 _fixture([1, 2, 3, 4, 5], _clean_sections([1, 2, 3, 4, 5])))

    # A missing section (row 3 has no '## 3'), the exact shape of the real
    # rows-17/19 incident.
    expect_dirty("missing section",
                 _fixture([1, 2, 3, 4], _clean_sections([1, 2, 4])),
                 "no matching '## N' section: 3")

    # A section with no table row (the real rows-56..67 incident: sections
    # existed but the table never grew rows for them).
    expect_dirty("orphan section",
                 _fixture([1, 2, 3], _clean_sections([1, 2, 3, 4])),
                 "no matching table row: 4")

    # A duplicated section number (the real '## 15' / '## 16' written twice
    # incident).
    dup_sections = _clean_sections([1, 2]) + ["## 2 — a different bug",
                                               "**Bug:** ...", "**Fix:** ...",
                                               ""]
    expect_dirty("duplicate section",
                 _fixture([1, 2, 3], dup_sections + _clean_sections([3])),
                 "used more than once")

    # A gap in the table itself (row 2 skipped).
    expect_dirty("table gap",
                 _fixture([1, 3, 4], _clean_sections([1, 3, 4])),
                 "gap in 1..4")

    # Sections present and matched but physically out of file order.
    out_of_order = _clean_sections([1, 3, 2])
    expect_dirty("sections out of order",
                 _fixture([1, 2, 3], out_of_order),
                 "not physically in increasing numeric order")

    if not ok:
        print("selftest FAILED")
        return 1
    print("selftest OK: missing sections, orphan sections, duplicate "
          "section numbers, table gaps and out-of-order sections are all "
          "caught; a clean doc is not flagged")
    return 0


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--selftest":
        return selftest()

    root = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(
        os.path.dirname(os.path.abspath(__file__)))

    path = os.path.join(root, DOC_PATH)
    if not os.path.isfile(path):
        print("%s: not found -- nothing to check" % DOC_PATH)
        return 0

    with open(path, "rb") as fh:
        text = fh.read().decode("utf-8")

    problems = check(text)
    if problems:
        print("%s: row<->section reconciliation FAILED:" % DOC_PATH)
        for p in problems:
            print("  - %s" % p)
        print("\nEvery index-table row 1..max must have exactly one "
              "matching '## N' write-up section (same subject), sections "
              "in numeric order, no gaps or duplicates.  Fix the table "
              "and/or the sections so they agree again.")
        return 1

    table_rows, _sections = parse(text)
    print("%s: %d table row(s), each with exactly one matching '## N' "
          "section, in order" % (DOC_PATH, len(table_rows)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
