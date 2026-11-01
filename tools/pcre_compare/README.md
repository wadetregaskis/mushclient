# pcre_compare

Links PCRE 8.45 and the client's own `src/mc_pcre.c` over PCRE2 into one program,
and runs them side by side, to show that the move from one to the other changed nothing
a plugin can see.  For each pattern, as written and with `PCRE_UTF8`, it compares:

- whether it compiles (`only-8.45` and `only-PCRE2` are the differences);
- its capture count and name table;
- for every subject, from the start and on from the end of each match, with
  `PCRE_NOTEMPTY` and without, 8.45's JIT against PCRE2's JIT, and both interpreters: the
  return code and every capture's offsets;
- for patterns with callouts, what a callout is told, with it returning 0 and 1;
- then the speed of the two, in the same process, turn about.

`make_corpus.py` collects the patterns: every trigger and alias in the plugins and worlds
under the repository and any directories given with `-Scan` (what a plain, non-regexp
match turns into as well), `rex.new` in Lua and in plugins, the test suite's `regcomp`
calls, and `tricky.py`'s edge cases.  The subjects are made from the patterns: a matching
sample and mutations of it, ordinary lines, and the same with raw 8-bit bytes and UTF-8,
valid or not.

## Running it

PCRE 8.45's source is no longer in the tree.  Get it from the history, and point at it:

```powershell
git archive 84bc2b10 pcre | tar -x -C $env:TEMP\p8
powershell -File tools\pcre_compare\build_and_run.ps1 -Pcre8 $env:TEMP\p8\pcre -Work $env:TEMP\pcre_compare `
    -Python <a python> -Scan packaging\discworld-plugins
```

`-InterpStride N` runs the interpreters on every Nth subject; 0 skips them.

## What it says

Differences are reported as `DIFF REAL` (a pattern from a real file) or `DIFF edge` (one
of `tricky.py`'s).  `DIFF REAL` must stay at none: any new one needs explaining, and
probably a change to `mc_pcre_rewrite`.

The `edge` ones that remain are understood, and are listed in the comment above
`mc_pcre_rewrite` in `src/mc_pcre.c`.  They are of these kinds:

| | |
|---|---|
| `only-PCRE2` | PCRE 8 refused it and PCRE2 takes it, so nothing that worked is affected: `(?<=a{1,3})`, long group names, `(?^)`, `(*sr:...)`, `\p{Script=Greek}`, `(?R)` (which PCRE2 reports at run time) and so on. |
| `only-8.45` | `(?(R1)a\|b)` with no group 1.  A loud failure, of something nobody writes. |
| `match-*` | `(?i)\p{Lu}`, `\p{Greek}` on the character U+0342, `(?xx)[a b]`, and `\8(a)(b)(c)(d)(e)(f)(g)(h)`. |

Also expected: `callout-log` on `^(?C1)word(?C2) ` when the subject has no "w" or so:
PCRE2's start optimisations rule the match out before the first callout, where 8.45's did
not.  The return code agrees.  And `new-jit-vs-int`, with `match-int-*`, on the catastrophic
`^(([a-z])+.)+[A-Z]([a-z])+$` and `(a|aa)+$`: the match limit counts differently in each engine, so
at the limit one gives up and another finishes.
