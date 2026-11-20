#!/usr/bin/env python3
"""more_subjects.py -- the corpus for trigger_bench verify.

  more_subjects.py <work dir> <discworld_lines.txt> [trigger dumps...]

Reads make_corpus.py's patterns.txt and subjects.txt in <work dir>, and adds:
- the triggers in each dump (written by dump_triggers.lua in a running client: these
  include the ones plugins make at run time with AddTriggerEx, which no file shows),
  as patterns, and also written out on their own as <dump>.patterns for `time`;
- every pattern again in UTF-8 mode;
- subjects: the Discworld-like lines, with "> " before them and in other cases, and
  lines generated from each dumped pattern (a matching sample and mutations of it).
Writes all_patterns.txt and all_subjects.hex.
"""
import os, random, re, sys
import re._parser as sp, re._constants as sc

work, lines_file = sys.argv[1], sys.argv[2]
dumps = sys.argv[3:]
CASELESS, MULTILINE, UTF8 = 1, 2, 0x800

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

pats = {}
for l in open(os.path.join(work, "patterns.txt")):
    o, h = l.split()
    pats.setdefault((int(o), h), (1, "file"))

dumped = []
for d in dumps:
    out = []
    for l in open(d, encoding="latin-1"):
        f = l.rstrip("\n").split("\t")
        if len(f) < 14:
            continue
        plugin, name, en, rx, ic, ml = f[0], f[1], f[2], f[3], f[4], f[5]
        p = bytes.fromhex(f[13]).decode("latin-1")
        if rx != "1":
            p = to_regexp(p)
        o = (CASELESS if ic == "1" else 0) | (MULTILINE if ml == "true" or ml == "1" else 0)
        h = p.encode("latin-1").hex()
        out.append("%d %s %s %s:%s\n" % (o, h, en, plugin[:8], name or "-"))
        pats.setdefault((o, h), (1, name))
        dumped.append(p)
    with open(d + ".patterns", "w") as w:
        w.writelines(out)
    print("%s: %d triggers" % (d, len(out)))

for (o, h) in list(pats):
    pats.setdefault((o | UTF8, h), (1, "utf"))

subjects = set()
for l in open(os.path.join(work, "subjects.txt")):
    subjects.add(bytes.fromhex(l.strip()))
for l in open(lines_file, "rb"):
    b = l.rstrip(b"\r\n")
    for v in (b, b"> " + b, b.upper(), b.lower(), b.swapcase(), b + b"\n" + b):
        subjects.add(v)

# a matching sample of each dumped pattern, and mutations of it (as make_corpus.py does)
rnd = random.Random(20261119)
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

fail = 0
for p in dumped:
    try:
        tree = sp.parse(norm(p.replace("(?P<", "(?P<")))
        for _ in range(4):
            l = gen(tree, {})
            if any(ord(c) > 255 for c in l):
                continue
            b = l.encode("latin-1")
            subjects.add(b)
            if len(b) > 2:
                subjects.add(b[:-1])
                i = rnd.randrange(len(b))
                subjects.add(b[:i] + b"X" + b[i + 1:])
                subjects.add(b.upper())
                subjects.add(b.lower())
                subjects.add(b"> " + b)
    except Exception:
        fail += 1

with open(os.path.join(work, "all_patterns.txt"), "w") as f:
    for (o, h) in pats:
        f.write("%d %s\n" % (o, h))
with open(os.path.join(work, "all_subjects.hex"), "w") as f:
    for b in sorted(subjects):
        if b"\0" not in b:
            f.write(b.hex() + "\n")
print("all: %d patterns, %d subjects (generator failed on %d dumped patterns)" % (len(pats), len(subjects), fail))
