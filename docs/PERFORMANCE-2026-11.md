# Plugin speed, November 2026

What made plugins slow, what was changed, and what it measured. The machine: the
Windows 11 ARM64 VM (8 cores), Release ARM64 builds, nothing else running (except
for one stretch, noted below, when another build was).

## How it was measured

- **The client, unattended.** Two copies of the same package side by side: A is the
  8ff1d376 build (the code this work started from), B the build under test. Each run
  opens a world with Quow's six Discworld plugins (752 triggers, 643 enabled; about 600
  of them QuowMinimap makes at run time with `AddTriggerEx`), runs the benchmarks from
  the world's script, and quits. Plugin state, the scrollback file, Quow's user
  database, the world file and the preferences are put back before every run. A and B
  alternate, A first in odd rounds and B first in even ones, and the figures below
  are medians.
- **The benchmarks.** `tools/bench_lines.lua` (new): server output through `Simulate`,
  as lines a second, three ways: one plain sentence; a Discworld-like mix
  (`tools/trigger_bench/discworld_lines.txt`: rooms, chat, tells, combat, prompts),
  where Quow's catch-all colour handler fires on every line and a few lines match
  other triggers; and `discworld_matching.txt`, lines that each fire a Quow trigger
  with a Lua callback (exits, tells, combat, the score prompt), most of which redraw
  a miniwindow. And `tools/bench.lua`, as before.
- **Headless.** `tools/trigger_bench` (new) runs PCRE2 and `src/mc_pcre.c`, built
  `/O2`, with Quow's 643 enabled patterns over the same lines, the way `regexec`
  does. It is also the proof for the prefilter below.
- **Profiles.** A small sampling profiler (suspend the client's main thread every
  millisecond, walk its stack with dbghelp, against a Release build linked with
  `/DEBUG`), run during the benchmarks. Not committed: it is 150 lines of scaffolding.

Run-to-run noise is about 5% on most figures; a figure only counts as a change when
the runs don't overlap.

## Where the time went

At the start, with Quow's plugins, a Discworld-like line cost about 265 us and a
plain one 245 us:

- **The trigger patterns.** About 40%: QuowMinimap's ~600 combat-statistics patterns
  begin `^(?:> )?((?:(?!You )(?:the frog that looks an awful lot like )?(?:\w+\b\W*?){1,8})`,
  which PCRE tries at every split of the first eight words before failing on the
  literal that follows. PCRE checks for one required byte; nothing checked for the
  required text.
- **Clock reads.** Two `QueryPerformanceCounter` calls per trigger per line, for the
  "time taken" figure: about 8%.
- **`App`.** On Windows `App` was `AfxGetApp ()`, which finds the app through MFC's
  thread-local module state, with a critical section, and `regexec` read it three
  times per trigger: about 7%.
- **Compiling Lua.** QuowColourHandler is a send-to-script trigger on `*`: forty lines
  of Lua, compiled afresh for every line from the MUD (about 10%).
- **Miniwindows.** Lines that fire Quow's callbacks redraw miniwindows. On this
  high-dpi display Quow's bitmap fonts are drawn through a mask and `StretchBlt`, and
  each `WindowText` made and deleted a bitmap and a brush, each `WindowLine` a pen.
  For those lines this was most of the time.
- **Invalidation.** Each line invalidated the whole output window two or three times
  over, each a kernel call, though after the first nothing changes until it paints.
  Once the rest was done this was the biggest single item for plain lines (18%).

## What was changed (all kept)

| commit | change |
|---|---|
| ce3f8c28 | `App` names `theApp` directly |
| a975dc5b | the required-literal prefilter in `regexec`; no CString copy or `strlen` per trigger; one in sixteen prefiltered attempts timed |
| fe60792b | `ParseLua` keeps the last chunk compiled under each name; Trace message only built when tracing |
| 9b8a5eba | miniwindows keep the last pen, the scaled-text brush and the mask bitmap |
| f3ef2dc8 | the output view invalidates once until it paints |
| 69ed4e5d | the prefilter takes alternations: a set of literals, any of which must be there |

Each step, against the build before it or against A, alternated (lines a second;
the prefilter row includes the `App` commit):

| step | plain | Discworld mix | matching (callbacks) | Simulate plain | Note |
|---|---:|---:|---:|---:|---:|
| `App` (vs A) | 4,312 -> 5,161 | 3,750 -> 4,390 | 1,332 -> 1,526 | 4,660 -> 6,133 | 1.05x |
| + prefilter (vs A) | 4,453 -> 14,269 | 3,640 -> 7,970 | 1,148 -> 1,624 | 4,821 -> 20,236 | |
| + Lua chunks (vs A) | 3,980 -> 18,312 | 3,042 -> 8,748 | 908 -> 1,705 | 5,029 -> 29,491 | |
| miniwindow objects (vs the step before) | 1.00x | 1.16x | 1.32x | 0.99x | |
| invalidate once (vs the step before) | 1.17x | 1.08x | 0.96x (noise) | 1.21x | 2.89x |
| literal sets (vs the step before) | 1.54x | 1.12x | 1.07x | 0.95x (see below) | 0.96x (noise) |

(A's own figures drift between sessions, from 4,312 to 3,980 for plain lines; the
ratios within a session are what to compare.)

Headless, Quow's 643 enabled patterns, microseconds a line, clock reads left out:

| | Discworld-like lines | the plain line |
|---|---:|---:|
| before | 65 - 75 | 103 - 129 |
| one literal | 13 | 22.5 |
| literal sets | 10.8 | 9.2 |

## Before and after

Release ARM64, Quow's six plugins, five runs of each alternated, medians, lines (or
calls) a second. A is 8ff1d376, B is 69ed4e5d. The bench.lua figures are from a run
of bench.lua on its own, also five rounds.

| | A | B | B / A |
|---|---:|---:|---:|
| plain sentence (bench_lines) | 4,111 | 34,791 | **8.5x** |
| Discworld-like mix | 3,772 | 11,231 | **3.0x** |
| lines that fire Quow's callbacks | 1,217 | 2,060 | **1.7x** |
| Simulate plain (bench.lua) | 5,025 | 35,964 | 7.2x |
| Simulate ANSI colour | 6,431 | 37,455 | 5.8x |
| +200 non-matching regex triggers | 4,269 | 30,833 | 7.2x |
| +200 non-matching wildcard triggers | 3,592 | 26,674 | 7.4x |
| +1 matching trigger calling a Lua function | 3,546 | 24,620 | 6.9x |
| +1 colouring trigger | 3,591 | 24,788 | 6.9x |
| Note | 110,588 | 348,767 | 3.2x |
| ColourNote (3 runs) | 97,896 | 247,695 | 2.5x |
| SetVariable + GetVariable | 835,901 | 847,394 | 1.0x |
| Repaint (full window) | 929 | 898 | 1.0x |
| scroll + repaint through the buffer | 619 | 622 | 1.0x |
| WindowText x40 + RectOp | 691 | 676 | 1.0x |
| ... and Repaint each | 306 | 303 | 1.0x |

In time per line: a plain line with Quow's plugins went from about 245 us to 29 us,
a Discworld-like one from 265 us to 89 us, and one that fires a callback that redraws
a miniwindow from 820 us to 485 us (most of what is left there is the drawing).

The literal sets came out 3 to 8% slower on bench.lua's lines, which end in a
number, in two sessions, though headless the same line is faster with them (8.3 ->
7.3 us) and figures they cannot touch (Note) moved as much in the same runs. A likely
cause (inferred, not shown): on those lines PCRE2 rejects the alternation patterns
itself, by their last required character, faster than a set of up to sixteen literals
can be searched. So regexec leaves the set alone where the line lacks that character,
and lets PCRE2 answer. Later runs were on a busy machine and settled nothing; the
final runs above, on a quiet one, show no loss anywhere, but the step was not
isolated again.

## The prefilter: why it can't make a trigger miss

`mc_pcre_required_literals` reads the pattern as PCRE2 will and finds text that every
match must contain: a run of plain characters at the top level, or in a group that is
itself required (not quantified, not a lookaround); and for an alternation whose every
branch has one, the set of them, one of which must be there. Anything it does not
fully understand gives up on the whole pattern: option settings such as `(?i)`, verbs
such as `(*ACCEPT)`, conditions, recursion, callouts, `\Q..\E`, extended mode, a brace
that isn't a plain `{n}`, `{n,}` or `{n,m}`, an escape with no meaning. A quantifier
takes the character before it out of the run. Caseless, letters are compared ignoring
ASCII case and nothing outside ASCII is taken; in UTF-8 mode nothing outside ASCII is
taken, nor caseless K and S (the Kelvin sign and the long s match them).

`regexec` skips the pattern only when none of the literals is in the subject, the
start offset is in range, and (in UTF-8 mode) the subject is plain ASCII: wherever
running the pattern could have reported an error instead, it is run. The one
difference left: a pattern that would have hit its match limit on a line without its
literal now simply doesn't match that line, where before it raised an error and was
disabled.

`tools/trigger_bench verify` is the proof: every trigger and alias pattern in the
tree's plugins and worlds, `rex.new` patterns, the test suite's, pcre_compare's edge
cases, and the 752 triggers a client has with Quow's plugins, each as written and in
UTF-8 mode (11,068 patterns), against 33,428 subjects (each pattern's own matching
samples and mutations of them, the Discworld-like lines in several cases, raw 8-bit
and UTF-8 bytes): 637 million checks, of which the prefilter skipped 234 million;
none of those matched, and the literal search agreed with a plain search every time.

## Other checks

- All ARM64 unit tests (2,176 passed, MFC assertions 187 from 12 suites, as before
  plus the new ones) and the engines selftest (513 passed, 19 skipped).
- Every benchmark run reports how many triggers fired: identical in A and B.
- Miniwindows: after the same 435 lines in a build with and one without the GDI
  change, each of Quow's 14 miniwindows was written out with `WindowWrite` and
  compared byte for byte. All identical except the comms window, whose timestamps
  differ between any two runs of the same build too.
- Painting: a script Simulated bursts of 500 lines, single lines and Notes every
  0.3 s and counted the output window's paints between them (`GetInfo (295)`): every
  one was painted, in this build and in A. (Not looked at on screen: the desktop
  was in use.)

## What a plugin can see

Nothing but the speed, with three things to know:

- **Match attempts** (`GetTriggerInfo` 38) count every attempt, prefiltered or not.
- **Time taken** (`GetTriggerInfo` 37, the trigger list, `GetInfo`'s totals) still
  times every run of a pattern. An attempt the prefilter decides costs about as much
  as a clock read, so one in sixteen of those is timed and counts sixteen times over:
  an estimate, right on average. The figures are much smaller than before, because the
  matching is.
- **Match-limit errors** on a line without the pattern's literal no longer happen
  (above).

## Tried and not kept

- **`/Ob2` instead of `/Ob1`** in the Release builds: 1 to 3% on most figures, inside
  the noise run by run, for a 4.5% larger exe. Reverted.
- **`lua5.1.dll` with `/GL` and link-time code generation**, built privately: pure Lua
  loops 0.94x to 1.05x, the client's figures unchanged. Not adopted; nothing in
  `packaging\windows\deps` needs rebuilding.
- **PGO for Lua**: not tried. With Quow's plugins, running Lua code is now a few per
  cent of a line in the profiles (compiling it was the cost), so even a 20% faster interpreter would
  be lost in the noise, at the price of a training run in the release pipeline.
- **Sampling every timed attempt** (not just prefiltered ones) would save most of the
  remaining clock reads, about 5% of a plain line. Not done: it would make the time of
  a rarely tried trigger an estimate too, and that is a script-visible figure.

## Not done, and why

- **A combined prefilter across all triggers** (one scan of the line for every
  literal, then a test per trigger): the per-trigger check is now about 15 ns, nine
  microseconds a line for Quow's 643, about a third of what is left of a plain line
  (29 us). A
  shared scan might halve that. Its correctness would rest on the same harness; worth
  doing only if trigger counts grow much further.
- **Embedded engines' dispatch** (`src/platform/mc_dispatch.cpp`: a lower-cased name
  and a hash lookup per world call): a few hundred nanoseconds against the
  microseconds a Python or JavaScript call itself costs.
- **The remaining miniwindow cost**: on a scaled display each `WindowText` in a bitmap
  font is still a measure, a mask, `SaveDC`, a clip, `StretchBlt` and `RestoreDC`.
  Drawing a line of text once into a cached mask, or clipping without `SaveDC`,
  could change pixels at the clip edges; left alone.
- **LuaJIT, Python's JIT, YJIT**: ruled out in the brief.
- **PCRE JIT on Linux and Intel Macs**: already on (PCRE2, `pcre2/src/config.h`
  `SUPPORT_JIT`).

## The port

The shared changes (`mc_pcre.c`, `regexp.*`, `evaluate.cpp`, `lua_scripting.cpp`) are
uncompiled there. `App` is already a plain global on the port, and the GDI and view
changes are Windows only. CMake's Release type gives `-O3`; link-time optimisation is
not on (`CMAKE_INTERPROCEDURAL_OPTIMIZATION`), and may be worth trying on the Mac,
where it can be measured.
