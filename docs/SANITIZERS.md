# Sanitizers — building and running the suite under ASan

**Added:** 2026-08-19, prompted by D-05. That fix left one guard whose bug is a
one-byte store into freed memory with *no behavioural symptom*, so nothing in
the test suite could distinguish the guard from a no-op. A sanitizer can. This
is how to run one, what the first run found, and — just as important — what a
clean run does and does not prove.

## Building

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DMUSHCLIENT_SANITIZE=address
cmake --build build-asan -j4
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build-asan
```

`MUSHCLIENT_SANITIZE` is a plain cache string handed to `-fsanitize=`, empty by
default. Anything clang accepts is accepted; `address` is what this tree has
been run with. **`thread` will not work as-is** — TSan needs every translation
unit instrumented, and this option deliberately leaves the vendored libraries
alone.

The default (empty) adds *nothing whatsoever* to any target. Verified rather
than asserted: after adding the option, reconfiguring the existing `build/` and
rebuilding recompiled **0** files, because no compile or link flag changed.

### What is instrumented, and what is not

| Instrumented | Not instrumented |
|---|---|
| `mushcore`, `mushcore_testsupport`, `mushgui_adapters`, `mushgui` | `mcpcre`, `mcsqlite3`, `mczlib` |
| every test executable | Lua, and the five embedded interpreters |

That line is deliberate, and it is not the coverage gap it looks like.
**ASan's heap checking comes from the intercepted allocator, not from
instrumentation.** Redzones and the quarantine are placed by `malloc` /
`operator new`, so a use-after-free or a heap-buffer-overflow *committed by*
uninstrumented vendored code is still caught in full, with a correct allocation
and free stack. What instrumenting the vendored libraries would add is stack-
and global-overflow checking *inside* them — and what it would cost is a long
tail of reports about deliberate tricks in SQLite and Lua that are not our bugs.
If you ever want them, add `mcpcre` / `mcsqlite3` / `mczlib` to the `foreach`
beside `mc_sanitize` in `CMakeLists.txt`.

## What the first run found

Seven of the 43 headless executables went red. **Three were genuine memory
errors**; the other four (seven test cases in all) were tests whose *instrument*
ASan replaces. Every one was ours — **nothing at all came from vendored code**,
which is worth recording because it was the expected source of noise.

### Genuine memory errors (3)

**1. `ProcessPreviousLine.cpp` — use-after-free on every gagging trigger.**
The omit-from-output loop deletes the batch of lines a gag matched, walking
backwards from the tail. It took its backward step *after* `RemoveTail()` had
freed the node it was stepping from:

```
READ of size 8 ... CTypedPtrList<CPtrList, CLine*>::GetPrev(void*&) mfc_collections.h:198
                   CMUSHclientDoc::ProcessPreviousLine() ProcessPreviousLine.cpp:791
freed by           CTypedPtrList<CPtrList, CLine*>::Unlink(...) mfc_collections.h:257
                   CTypedPtrList<CPtrList, CLine*>::RemoveTail() mfc_collections.h:132
                   CMUSHclientDoc::ProcessPreviousLine() ProcessPreviousLine.cpp:785
```

This one is *not* merely a latent memory error. Reading the freed node's `prev`
gets whatever libmalloc left in the block, which is usually null — so the loop
stopped after a single line, and **a gag on a wrapped multi-line paragraph left
most of the paragraph on screen**. The regression test added with the fix
(`PalWorldView.GagDeletesExactlyItsOwnBatchAndNothingBefore`) fails on the
unfixed code with no sanitizer at all: 10 lines survive where 4 should.

Upstream it is latent rather than harmful, because MFC's `CList` never returns
nodes to the heap — `FreeNode` pushes them onto a free list and leaves `pPrev`
intact. The port's `CTypedPtrList` really does `delete n`.

The identical bug in the sibling loop — `DeleteLines`, in
`scripting/methods/methods_output_shared.cpp` — was found and fixed long ago,
and its comment describes this exact hazard. **This instance was missed**, one
file away from a comment explaining the bug. That is the per-root-cause miss
`CLAUDE.md` warns about, and it is the strongest argument for owning a sanitizer
build: the reasoning had already been done and written down, and it still took
an instrument to find the second copy.

**2. `platform/mfc_ui.h` — `SafeArrayGetElement` read past its caller's array.**
The `LONG*` overload built `long ix[2] = { index[0], index[1] }`, but this shim
is one-dimensional throughout (every path indexes `rgsabound[0]`, and the `long*`
overload it forwards to reads `index[0]` alone). Every caller passes a
one-element array, so `index[1]` was off the end — to build a value nothing
subsequently looked at.

```
READ of size 4 ... SafeArrayGetElement(SAFEARRAY*, int*, void*) mfc_ui.h:1992
Address 0x00016b639dc4 is located in stack of thread T0
```

An out-of-bounds *read* has no observable effect, so no behavioural test can
catch this one. A sanitizer is the only instrument; the test that carries it is
`MiscShims.SafeArrayLongPointerOverloads`, whose one-element index array is now
commented as load-bearing.

**3. `tests/test_mfc_ui.cpp` — a `CMemFile` assertion read off the end of the
buffer.** `Write("abcdef", 6)` stores exactly six bytes and appends no
terminator; the test then compared `GetMemoryPos()` with `EXPECT_STREQ`, i.e.
`strcmp`, which runs past the allocation. Ours, and a real out-of-bounds read,
just in test code rather than shipping code. Now compared by length.

Note that #2 only became visible *after* #3 was fixed: **ASan halts on the first
error in an executable**. Re-run after every fix before believing it is clean.

### Tests whose instrument ASan replaces (7 cases, 4 executables)

Not product bugs, and not ordinary test bugs either — these measure the
**allocator**, and a sanitizer replaces the allocator wholesale:

| Test | What it measures | Saw |
|---|---|---|
| `RegexpTest.RecallTextDoesNotLeakItsCompiledRegexp` | RSS growth vs a 2 MB budget | 141 MB |
| `MXPDefs.RedefinitionEarlyReturnsDoNotLeakTheArgumentList` | live block count, exact | +8 |
| `MxpR4.OpenTagVetoDoesNotLeakTheArgumentList` | live block count, exact | +18 |
| `MxpR4.CustomElementArgumentAbortDoesNotLeakTheBuiltList` | live block count, exact | 10 vs 12 |
| `LuaScripting.StringReturningMethodsDoNotLeakTheirVariant` | RSS growth vs a 3 MB budget | 32 MB |
| `DatabaseTest.OpenFailureClosesTheHandleSqliteAllocated` | RSS growth vs a 2 MB budget | 111 MB |
| `GdiHeadless.DeletingABitmapObjectReleasesItsSurface` | that a freed address is **recycled** | never |

Redzones inflate every block, and the quarantine keeps freed blocks out of
circulation — which breaks the count- and RSS-based ones, and makes the last one
*structurally unable* to reproduce the aliasing it exists to pin. They now say
so with `MC_SKIP_UNDER_SANITIZER` (`tests/mc_test.h`), which prints a visible
`[SKIP]` and a reason rather than passing vacuously, and which is inert in a
normal build — verified: they still run and pass there. LeakSanitizer, not a
hand-rolled block count, is the right instrument for them under ASan; that is a
separate piece of work and has not been done.

### One GUI entry stands down

`selftest_notepad_loopfree` runs `mushgui` under **Guard Malloc**
(`DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib`). Guard Malloc and a
sanitizer are both allocator replacements and cannot both be loaded — the
process is killed before any harness runs. `tests/CMakeLists.txt` therefore
does not register that entry when `MUSHCLIENT_SANITIZE` is set, which is why
the sanitized tree has 69 entries and the normal one 70.

## A clean run is not a proof of absence

Three independent reasons, all of which this project has already met:

1. **Changing the allocator changes the timing.** An earlier investigation in
   this tree found a bug that a different allocator hid. ASan's allocator is
   very different — bigger blocks, different addresses, no reuse — so a race or
   an ordering bug that reproduces under libmalloc may simply not occur here.
2. **Guard Malloc has been more effective than ASan on at least one bug here.**
   It puts every allocation on its own page and unmaps it on free, so it catches
   a stale read that ASan's quarantine happens to tolerate. It is not a
   substitute for ASan and ASan is not a substitute for it — note that the one
   test in this suite deliberately built around libgmalloc is exactly the one
   that cannot run under ASan. `MallocScribble=1` / `MallocPreScribble=1` are
   cheaper still and worth a pass.
3. **ASan only sees what the run executes.** The paint path is never entered by
   any selftest, so nothing reachable only from a paint is covered at all.

`GdiHeadless.DeletingABitmapObjectReleasesItsSurface` is the sharpest
illustration: under ASan that test *cannot fail*, because the allocator
behaviour it depends on is gone. A green sanitizer run answers "no error was
detected on these paths, with this allocator", and nothing wider.

## Can the GUI be sanitized?

**Yes, and it works** — better than expected. `mushgui` gets `mc_sanitize` too,
so `-DMUSHCLIENT_SANITIZE=address` builds the app itself, not just the headless
suite. All 18 runnable in-app selftests pass under it (the 19th is the Guard
Malloc one above), with no findings, and the whole sanitized tree is 69/69.

Two caveats on how much that is worth. wxWidgets, AppKit and the five embedded
interpreters are uninstrumented system or third-party code, so on exactly the
paths the in-app selftests exercise most heavily ASan can say the least — heap
errors are still caught, nothing else is. And the paint path is never entered by
any selftest, so the GUI code with no coverage at all still has none. It is
worth reaching for when investigating a specific GUI-side crash; it is not where
the value of this option lies.

## Should there be a sanitized ctest entry?

**Recommendation: no separate registered entry — keep it a documented build
mode, and run it before merges that touch memory ownership.**

The cost is entirely in the build, not the run:

| | `ctest` | Wall time |
|---|---|---|
| normal `build/` | 70/70 | 38.1 s |
| `build-asan/` | 69/69 | 29.9 s |

The run is free. But the sanitized suite needs a *whole second build tree* — it
cannot share objects with the normal one — and a ctest entry inside the normal
build would have to shell out to a second configure-and-build, which is not
something a test should do. Registering it inside `build-asan` itself adds
nothing, since every entry there is already sanitized.

So the pragmatic shape is what this document gives: two commands to create the
tree, one to run it. If it should be prevented from rotting, a CI job is the
right shape, not a ctest entry. Flagged as a decision rather than assumed.

## Reading a report

* `ASAN_OPTIONS=detect_leaks=0` — LeakSanitizer is off by default on Darwin
  anyway, and this tree has known deliberate leaks.
* ASan **halts on the first error** in an executable. After fixing one, re-run
  that executable before believing it is clean.
* The exit code is 1 for an ordinary test failure and 134 (SIGABRT) for a
  sanitizer report. Gate on the exit code, never on grepping the output.
* Frames inside `libclang_rt.asan_osx_dynamic.dylib` are the interceptor, not
  the bug — the first frame in our own code is the one to read.
