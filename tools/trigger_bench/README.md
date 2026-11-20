# trigger_bench

The trigger path's regular expressions, headless: PCRE2 and the client's own
`src/mc_pcre.c`, built with `/O2` into one program, with no GUI and no client.

- `verify` is the proof for the required-literal prefilter (`mc_pcre_required_literals`,
  used by `regexec`): every pattern against every subject, and wherever the prefilter
  would skip a pattern, the pattern must not match, with `PCRE_NOTEMPTY` and without.
  A `MISSED` line is a trigger the prefilter would make miss a line, silently. There
  must be none.
- `time` runs every enabled pattern over each subject the way `regexec` does (`strlen`,
  two clock reads, the JIT), with and without the prefilter and the clock reads, and
  lists the dearest patterns.
- `which` says which enabled patterns match each subject.

## The corpus

Patterns: everything `tools/pcre_compare/make_corpus.py` collects (triggers and aliases
in every plugin and world in the tree, `rex.new`, the tests' `regcomp` calls, plain
matches converted, and `tricky.py`'s edge cases), plus the triggers a running client
actually has, from a dump made in the client with `dump_triggers.lua`: Quow's
QuowMinimap makes about 600 of its 750 triggers at run time, with `AddTriggerEx`, so no
file shows them. Every pattern is tried as written and in UTF-8 mode.

Subjects: make_corpus.py's (a matching sample of each pattern, mutations of it, ordinary
lines, raw 8-bit and UTF-8 bytes), samples made the same way from the dumped patterns,
and `discworld_lines.txt` (Discworld-like output, hand-written) in several cases and
with `> ` before it.

`discworld_lines.txt` and `discworld_matching.txt` (lines that each fire one of Quow's
triggers) are also what `tools/bench_lines.lua` feeds the real client.

## Running it

In the client, with the plugins installed (Game > Immediate):

```lua
dofile [[<repository>\tools\trigger_bench\dump_triggers.lua]]   -- writes triggers-dump.txt
```

Then:

```powershell
powershell -File tools\trigger_bench\build_and_run.ps1 -Work $env:TEMP\trigger_bench `
    -Python <a python> -Patterns <path>\triggers-dump.txt
```

`-Mode verify` or `-Mode time` runs one half; `-Passes N` sets how many times `time`
goes over the lines.

`TB_MAX_LITERALS=1` in the environment limits the prefilter to single literals (no
sets from alternations), for comparing the two.
