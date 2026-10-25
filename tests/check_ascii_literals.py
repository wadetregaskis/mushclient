#!/usr/bin/env python3
"""Guard: narrow string literals in the shipping source must be ASCII-only.

Why this exists
---------------
`Note()`, `Tell()`, `ColourNote()`, `DisplayMsg()` and friends take *bytes*.  The
output window decodes those bytes with the **world's** encoding, so a UTF-8 em
dash in an app-generated message renders as "â€”" in every world that is not set
to UTF-8 — which is the default.  The defect is invisible in review because the
source looks perfect in any modern editor; it only shows up on screen, in a
running app, connected to a world.

So: any non-ASCII byte in a *narrow* string literal is an error.  Wide literals
are fine (`wxT("…")` becomes `L"…"`, which the compiler converts from the UTF-8
source correctly and wx renders as Unicode), and so is an explicit
`wxString::FromUTF8("…")`.  UI text therefore stays free to use real typography;
only the byte-oriented paths are constrained.

Escapes are unaffected: "caf\\xE9" is ASCII *source* and stays legal — a
deliberate byte is not the mistake this catches.

Escape hatch: put `ascii-lint: allow` in a comment on the offending line.

Usage: check_ascii_literals.py [source-root]
Exit status 0 = clean, 1 = violations found (paths printed to stdout).
"""

import os
import re
import sys

# The project's own code.  Vendored trees (lua, pcre, sqlite3, zlib, png,
# openssl, spell, miniaudio…) are excluded: they are not ours to reformat, and
# none of them writes to the world output window.
INCLUDE_DIRS = ("src",)
EXCLUDE_DIRS = ("src/platform/miniaudio",)
EXTENSIONS = (".cpp", ".h", ".c", ".mm")

# A narrow literal is exempt when it is the argument of one of these.  wxT/_T
# make it wide; FromUTF8 states the encoding explicitly.
WIDENING_CALL = re.compile(r"(?:\bwxT|\b_T|\bFromUTF8)\s*\($")

ALLOW_MARKER = "ascii-lint: allow"


def scan(text):
    """Lex `text` (latin-1 decoded, so 1 char == 1 byte).

    Returns (literals, blanked) where `literals` is a list of
    (start, end, prefix, body) for every string literal — `start` indexes the
    prefix, not the quote — and `blanked` is a copy of the text with comment and
    literal *contents* replaced by spaces, so it can be searched as pure code.
    """
    n = len(text)
    blanked = list(text)
    literals = []
    i = 0
    while i < n:
        c = text[i]

        # ── comments ──────────────────────────────────────────────────────
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                blanked[i] = " "
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
            for k in range(i, end):
                if text[k] != "\n":
                    blanked[k] = " "
            i = end
            continue

        # ── character literals ────────────────────────────────────────────
        if c == "'":
            j = i + 1
            while j < n and text[j] != "'":
                j += 2 if text[j] == "\\" else 1
            for k in range(i, min(j + 1, n)):
                blanked[k] = " "
            i = min(j + 1, n)
            continue

        # ── string literals ───────────────────────────────────────────────
        if c == '"':
            # Walk back over the encoding prefix (L, u, U, u8, R and pairs).
            start = i
            while start > 0 and text[start - 1] in "LuU8R":
                start -= 1
            prefix = text[start:i]
            if "R" in prefix:                       # raw string: R"delim(...)delim"
                open_paren = text.find("(", i)
                delim = text[i + 1:open_paren] if open_paren > 0 else ""
                close = text.find(")" + delim + '"', open_paren + 1 if open_paren > 0 else i)
                end = n if close < 0 else close + len(delim) + 2
                body = text[i:end]
            else:
                j = i + 1
                while j < n and text[j] != '"':
                    j += 2 if text[j] == "\\" else 1
                end = min(j + 1, n)
                body = text[i + 1:end - 1]
            literals.append((start, end, prefix, body))
            for k in range(i, end):                 # blank the contents, keep the quotes
                if text[k] not in '"\n':
                    blanked[k] = " "
            i = end
            continue

        i += 1

    return literals, "".join(blanked)


def is_widened(literal_start, literals_by_end, blanked):
    """True if this literal is (transitively) the argument of a widening call.

    Back-scans over whitespace, blanked-out comments and *preceding string
    literals*, so `wxT("a"\n     "b")` exempts both halves — the continuation
    literal is not itself preceded by `wxT(`.
    """
    k = literal_start - 1
    while k >= 0:
        if blanked[k].isspace():
            k -= 1
        elif blanked[k] == '"' and (k + 1) in literals_by_end:
            k = literals_by_end[k + 1] - 1        # jump past the preceding literal
        else:
            break
    return bool(WIDENING_CALL.search(blanked[:k + 1]))


def check_file(path):
    with open(path, "rb") as fh:
        raw = fh.read()
    if not any(b >= 0x80 for b in raw):
        return []

    text = raw.decode("latin-1")
    literals, blanked = scan(text)
    literals_by_end = {end: start for (start, end, _p, _b) in literals}
    lines = text.split("\n")

    bad = []
    for start, _end, prefix, body in literals:
        if any(ch >= "\x80" for ch in prefix):      # can't happen, but be safe
            continue
        if prefix.strip("R") in ("L", "u", "U", "u8"):
            continue                                # wide / explicitly encoded
        if not any(ch >= "\x80" for ch in body):
            continue
        lineno = text.count("\n", 0, start) + 1
        if ALLOW_MARKER in lines[lineno - 1]:
            continue
        if is_widened(start, literals_by_end, blanked):
            continue
        bad.append((lineno, readable(lines[lineno - 1].strip()),
                    describe(body)))
    return bad


def readable(line):
    """Echo a source line with its non-ASCII bytes shown as \\xNN.

    Printing the raw bytes back would just reproduce the mojibake, which is
    exactly the thing that is hard to see.
    """
    return "".join(ch if ch < "\x80" else "\\x%02X" % ord(ch) for ch in line)


def describe(body):
    """Name the offending characters, e.g. `U+2014 EM DASH (\\xE2\\x80\\x94)`.

    Almost every real instance is a typographic character pasted or
    autocorrected into the source, so naming it says what to replace.
    """
    import unicodedata
    raw = body.encode("latin-1")
    try:
        chars = [c for c in raw.decode("utf-8") if ord(c) >= 0x80]
    except UnicodeDecodeError:
        return " ".join("\\x%02X" % b for b in raw if b >= 0x80)

    out = []
    for c in dict.fromkeys(chars):                  # de-dup, keep order
        name = unicodedata.name(c, "?")
        enc = "".join("\\x%02X" % b for b in c.encode("utf-8"))
        out.append("U+%04X %s (%s)" % (ord(c), name, enc))
    return ", ".join(out)


# ── selftest ──────────────────────────────────────────────────────────────────
# A lint that silently stops finding things is worse than no lint, and this one
# has a lot of "must NOT flag" cases (comments, wxT, continuation literals, hex
# escapes) any of which could over-match and neuter it.  So the checker carries
# its own fixture: `--selftest` asserts one specific line is flagged and every
# exempt shape is not.  Run by ctest alongside the tree scan.
SELFTEST_SOURCE = '''\
// A comment with an em-dash \xe2\x80\x94 must not be flagged.
/* nor a block comment \xe2\x80\x94 like this one */
void f() {
    doc->Note("caught em-dash \xe2\x80\x94 here");
    doc->Note("plain ascii - here");
    wxLogMessage(wxT("wide ellipsis\xe2\x80\xa6 fine"));
    wxMessageBox(wxT("first line\xe2\x80\xa6\\n"
                     "continuation \xe2\x80\x94 still wide"));
    puts(L"L-prefixed \xe2\x80\x94 fine");
    puts(wxString::FromUTF8("explicit \xe2\x80\x94 utf8"));
    puts("escaped caf\\xE9 is ASCII source");
    puts("suppressed \xe2\x80\x94 here");  // ascii-lint: allow
    char c = '\xe2\x80\x94';
    doc->Note("second catch \xc2\xb1 2");
}
'''


def selftest():
    import tempfile

    expected = {4, 14}          # the only two lines that must be reported
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "probe.cpp")
        with open(path, "wb") as fh:
            fh.write(SELFTEST_SOURCE.encode("latin-1"))
        found = {lineno for lineno, _line, _codes in check_file(path)}

    missed = expected - found
    spurious = found - expected
    for lineno in sorted(missed):
        print("SELFTEST FAIL: line %d should have been flagged but was not"
              % lineno)
    for lineno in sorted(spurious):
        print("SELFTEST FAIL: line %d was flagged but is an exempt shape"
              % lineno)
    if missed or spurious:
        print("selftest FAILED (%d missed, %d spurious)"
              % (len(missed), len(spurious)))
        return 1
    print("selftest OK: %d violations detected, all exempt shapes ignored"
          % len(expected))
    return 0


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--selftest":
        return selftest()

    root = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(
        os.path.dirname(os.path.abspath(__file__)))

    targets = []
    for name in sorted(os.listdir(root)):           # top-level shipping source
        if name.endswith(EXTENSIONS) and os.path.isfile(os.path.join(root, name)):
            targets.append(os.path.join(root, name))
    for sub in INCLUDE_DIRS:
        for dirpath, dirnames, filenames in os.walk(os.path.join(root, sub)):
            rel = os.path.relpath(dirpath, root)
            if any(rel == x or rel.startswith(x + os.sep) for x in EXCLUDE_DIRS):
                dirnames[:] = []
                continue
            for f in sorted(filenames):
                if f.endswith(EXTENSIONS):
                    targets.append(os.path.join(dirpath, f))

    failures = 0
    for path in targets:
        for lineno, line, codes in check_file(path):
            failures += 1
            print("%s:%d: non-ASCII byte(s) %s in a narrow string literal\n    %s"
                  % (os.path.relpath(path, root), lineno, codes, line))

    if failures:
        print("\n%d violation(s).  Narrow literals reach the output window as raw"
              "\nbytes and are decoded with the world's encoding, so non-ASCII"
              "\nshows up as mojibake.  Use ASCII, or wxT(...) if the string is"
              "\nwx UI text.  Last resort: an `ascii-lint: allow` comment." % failures)
        return 1

    print("checked %d files: no non-ASCII narrow string literals" % len(targets))
    return 0


if __name__ == "__main__":
    sys.exit(main())
