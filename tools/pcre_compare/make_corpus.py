#!/usr/bin/env python3
"""make_corpus.py -- writes patterns.txt and subjects.txt for pcre_compare.

  make_corpus.py <out dir> <repository> [more directories to scan...]

Patterns: every regular expression in the plugins, worlds and Lua files under the
given directories (triggers, aliases, rex.new), the test suite's regcomp calls, what
non-regexp triggers turn into, and tricky.py's edge cases.  Subjects: lines made from
the patterns themselves (a matching sample and mutations of it), real-looking lines,
and the same with raw 8-bit bytes and with UTF-8.
"""
import glob, html, os, random, re, sys
import re._parser as sp, re._constants as sc
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tricky

CASELESS, MULTILINE = 1, 2

out, repo = sys.argv[1], sys.argv[2]
roots = [repo] + sys.argv[3:]
pats = {}          # (bytes pattern, options) -> source

def add(p, opts, src):
    if isinstance(p, str):
        p = p.encode("latin-1", "replace")
    if p and b"\0" not in p:
        pats.setdefault((p, opts), src)

def truth(v):
    return v is not None and v.lower() in (b"y", b"yes", b"1")

tag = re.compile(rb'<(trigger|alias)\b((?:[^>"]|"[^"]*")*)>', re.S)
attr = re.compile(rb'([A-Za-z_]+)\s*=\s*"([^"]*)"', re.S)

def to_regexp(s):
    """What ConvertToRegularExpression makes of a plain match (Utilities.cpp)."""
    o = "^"
    for c in s:
        if c == "*":
            o += "(.*?)"
        elif c.isalnum() or c == " " or ord(c) >= 128:
            o += c
        else:
            o += "\\" + c
    return o + "$"

def scan_xml(data, src):
    for m in tag.finditer(data):
        a = {k.decode().lower(): v for k, v in attr.findall(m.group(2))}
        if "match" not in a:
            continue
        s = html.unescape(a["match"].decode("latin-1"))
        if not s:
            continue
        o = CASELESS if truth(a.get("ignore_case")) else 0
        if truth(a.get("multi_line")):
            o |= MULTILINE
        if truth(a.get("regexp")):
            add(s, o, src)
        else:
            add(to_regexp(s), o, src)

lua_string = r'''(?:"((?:[^"\\\n]|\\.)*)"|'((?:[^'\\\n]|\\.)*)'|\[\[(.*?)\]\]|\[=\[(.*?)\]=\])'''
rexnew = re.compile(r'\brex\.new\s*\(?\s*' + lua_string, re.S)

def unlua(s):
    def one(m):
        c = m.group(1)
        if c.isdigit():
            return chr(int(c) & 255)
        return {"n": "\n", "t": "\t", "r": "\r", "\\": "\\", '"': '"', "'": "'", "a": "\a", "b": "\b",
                "f": "\f", "v": "\v"}.get(c, c)
    return re.sub(r'\\(\d{1,3}|.)', one, s, flags=re.S)

def scan_lua(text, src):
    for m in rexnew.finditer(text):
        if m.group(1) is not None:
            add(unlua(m.group(1)), 0, src)
        elif m.group(2) is not None:
            add(unlua(m.group(2)), 0, src)
        else:
            add(m.group(3) or m.group(4) or "", 0, src)

def uncpp(s):
    def one(m):
        c = m.group(1)
        if c[0] == "x":
            return chr(int(c[1:], 16) & 255)
        return {"n": "\n", "t": "\t", "r": "\r", "\\": "\\", '"': '"', "'": "'", "a": "\a", "b": "\b",
                "f": "\f", "v": "\v", "0": "\0"}.get(c, c)
    return re.sub(r'\\(x[0-9a-fA-F]{1,2}|.)', one, s, flags=re.S)

testre = re.compile(r'\bregcomp\s*\(\s*"((?:[^"\\\n]|\\.)*)"')
testmatch = re.compile(r'match=\\"((?:[^"\\\n]|\\[^"])*)\\"')

for root in roots:
    for dirpath, dirs, files in os.walk(root):
        dirs[:] = [d for d in dirs if d not in (".git", "WinRel-ARM64", "WinDebug-ARM64", "WinTest-ARM64", "deps", "docs",
                                                "selftest-dist", "pcre", "pcre2", "sqlite3")]
        for name in files:
            path = os.path.join(dirpath, name)
            low = name.lower()
            try:
                if low.endswith((".xml", ".mcl", ".lua")):
                    data = open(path, "rb").read()
                    if low.endswith((".xml", ".mcl")):
                        scan_xml(data, path)
                    scan_lua(data.decode("latin-1"), path)
                elif low.startswith("test_") and low.endswith(".cpp"):
                    text = open(path, "rb").read().decode("latin-1")
                    for m in testre.finditer(text):
                        add(uncpp(m.group(1)), 0, path)
                    for m in testmatch.finditer(text):
                        add(uncpp(m.group(1)), 0, path)
            except OSError:
                pass

real = len(pats)
for p in tricky.patterns():
    add(p, 0, "tricky")
    add(p, CASELESS, "tricky")
print("patterns: %d from files, %d with the edge cases" % (real, len(pats)))

rnd = random.Random(20261031)
POSIX = {"[:digit:]": r"0-9", "[:alpha:]": "a-zA-Z", "[:alnum:]": "a-zA-Z0-9", "[:space:]": r"\s", "[:upper:]": "A-Z",
         "[:lower:]": "a-z", "[:punct:]": r"!-/:-@", "[:word:]": r"\w", "[:print:]": r" -~", "[:graph:]": r"!-~"}

def norm(s):
    for k, v in POSIX.items():
        s = s.replace(k, v)
    return re.sub(r"\(\?[>|]", "(?:", s).replace("++", "+").replace("*+", "*").replace("?+", "?")

CATS = {sc.CATEGORY_DIGIT: "0123456789", sc.CATEGORY_WORD: "abcXYZ_09", sc.CATEGORY_SPACE: " ",
        sc.CATEGORY_NOT_DIGIT: "ab .", sc.CATEGORY_NOT_WORD: " .,'", sc.CATEGORY_NOT_SPACE: "ab1."}

def gen(items, groups):
    r = ""
    for op, av in items:
        if op is sc.LITERAL:
            r += chr(av)
        elif op is sc.NOT_LITERAL:
            r += rnd.choice([c for c in "aZ 1." if ord(c) != av] or ["x"])
        elif op is sc.ANY:
            r += rnd.choice("aZ 1.")
        elif op is sc.IN:
            cands, neg = [], False
            for o, a in av:
                if o is sc.NEGATE:
                    neg = True
                elif o is sc.LITERAL:
                    cands.append(chr(a))
                elif o is sc.RANGE:
                    cands.append(chr(rnd.randint(a[0], min(a[1], a[0] + 25))))
                elif o is sc.CATEGORY:
                    cands.append(rnd.choice(CATS.get(a, "a")))
            if neg:
                cands = [c for c in "aZ1 .,:" if c not in cands] or ["~"]
            r += rnd.choice(cands or ["a"])
        elif op in (sc.MAX_REPEAT, sc.MIN_REPEAT, sc.POSSESSIVE_REPEAT):
            lo, hi, body = av
            for _ in range(rnd.randint(lo, min(hi, lo + 3))):
                r += gen(body, groups)
        elif op is sc.SUBPATTERN:
            g = gen(av[3], groups)
            if av[0]:
                groups[av[0]] = g
            r += g
        elif op is sc.ATOMIC_GROUP:
            r += gen(av, groups)
        elif op is sc.BRANCH:
            r += gen(rnd.choice(av[1]), groups)
        elif op is sc.GROUPREF:
            r += groups.get(av, "")
        elif op is sc.GROUPREF_EXISTS:
            r += gen(av[1], groups)
    return r

lines = set()
genfail = 0
for (p, o), src in pats.items():
    if src == "tricky":
        continue
    try:
        tree = sp.parse(norm(p.decode("latin-1")))
        for _ in range(3):
            l = gen(tree, {})
            if "\n" in l or any(ord(c) > 255 for c in l):
                continue
            b = l.encode("latin-1")
            lines.add(b)
            if len(b) > 2:
                lines.add(b[:-1])
                i = rnd.randrange(len(b))
                lines.add(b[:i] + b"X" + b[i + 1:])
                lines.add(b + b" extra")
                lines.add(b"> " + b)
                lines.add(b.upper())
    except Exception:
        genfail += 1

gen_lines = sorted(lines)
# Thin to a size that can run in minutes; the tricky subjects and raw bytes stay.
rnd.shuffle(gen_lines)
gen_lines = gen_lines[:12000]

real_text = [
    b"", b" ", b"a", b"You say: hello there", b"Bob tells you: Are you there?", b"The grey wolf bites you.",
    b"HP:100/120 GP:50/60 XP:12345", b"You have 3 items in your inventory.", b"[Newbie] Sorrow: hi all",
    b"Exits: north, south, east and west", b"> ", b"Obvious exits: north, down.", b"A small brown dog is here.",
    b"You can't go that way.", b"An orange 12-sided die lands on 7!", b"12:30:01 pm", b"\t tabbed\tline",
    b"Trailing newline\n", b"two\nlines\nhere", b"crlf\r\nline", b"  leading and trailing  ",
    b"\xe9l\xe8ve caf\xe9 \xa3100", b"\xff\xfe raw", b"caf\xc3\xa9 r\xc3\xa9sum\xc3\xa9", b"\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e line",
    b"\xf0\x9f\x98\x80 emoji", b"bad \xc3 utf", b"bad \xed\xa0\x80 surrogate", b"\x80 isolated", b"\xc0\x80 overlong",
    b"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", b"a" * 300 + b"!", b"ab" * 200, b"word " * 60,
]
real_text += tricky.subjects()
marked = []
for b in gen_lines[:4000]:
    if len(b) > 3:
        i = rnd.randrange(len(b))
        marked.append(b[:i] + rnd.choice([b"\xe9", b"\xa3", b"\xff", b"\x80", b"\xc3\xa9", b"\xe6\x97\xa5", b"\xf0\x9f\x98\x80"]) + b[i:])
all_lines = list(dict.fromkeys(real_text + gen_lines + marked))
print("subjects: %d (generator failed on %d patterns)" % (len(all_lines), genfail))

with open(os.path.join(out, "real_count.txt"), "w") as f:
    f.write(str(real))
with open(os.path.join(out, "patterns.txt"), "w") as f:
    for (p, o) in pats:
        f.write("%d %s\n" % (o, p.hex()))
with open(os.path.join(out, "subjects.txt"), "w") as f:
    for b in all_lines:
        f.write(b.hex() + "\n")
with open(os.path.join(out, "patterns_sources.txt"), "w", encoding="latin-1", errors="replace") as f:
    for (p, o), src in pats.items():
        f.write("%s\t%d\t%s\n" % (src, o, p.decode("latin-1")))
