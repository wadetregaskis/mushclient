# Shipping-line coverage — the headless (mushcore) test suite

**As of:** 2026-07 coverage campaign, round 5
(rounds 1-4: 43.8% → 55.7% → 68.2% → 78.3% → 84.5%).

## Headline numbers

| Metric | Value |
|---|---|
| **Line coverage (raw)** | **86.83%** (29,366 / 33,819 shipping lines) |
| Region coverage | 85.74% |
| Function coverage | 84.09% |
| Branch coverage | 74.87% |
| **Reachable-adjusted line coverage** | **≈ 90.5%** (see denominator below) |
| Test executables | 27 suites (1,426 tests) + 3 in-app PAL selftests, ctest 30/30 |

Measured by `tests/coverage.sh` (llvm-cov over every `test_*` executable in
`build-cov`, third-party sources excluded — see the `IGNORE` regex in that
script for the exact exclusion list).

## What "reachable" means

`mushcore` is the headless engine: the full shared source compiled without a
window server, without the wx adapter, and with Lua as the only embedded
scripting engine.  A portion of the shipping lines compiled into it can never
execute in that configuration, no matter how many tests are written.  Those
lines are categorised below — each bucket was audited against annotated
`llvm-cov show` output, and the block line-counts are **measured**, not
guessed (blocks re-measured for round 5 carry the fresh numbers; blocks in
code untouched since the round-4 audit carry that audit's numbers).  The
buckets are deliberately conservative: only blocks that were individually
audited are counted, so the true unreachable denominator is somewhat larger
and the true reachable-adjusted coverage somewhat higher.

## Categorised unreachable-headless denominator

| Category | Missed lines | What it is |
|---|---:|---|
| GUI-only flows | **598** | `plugins.cpp` plugin-wizard body after `DoModal()` (332 — the headless stub always cancels); GUI-class headers compiled dead (`doc.h` inlines 37, `miniwindow.h` 19, `mainfrm_stub.h` 13, `UDPsocket.cpp` 13, `exceptions.h` 10, `HyprLink.h` 9, `MUSHview.h` 6, `genprint.h` 5, `sendvw.h`/`StatLink.h` 2 — 114 total); `doc_shared.cpp` MFC view-list walks + live-TLS teardown (26); `methods_info.cpp` window-metric functions over CWnd views (50); `methods_spellchecker.cpp` `SpellCheckCommand` command-window walk (54); `ProcessPreviousLine.cpp` output-view Invalidate walk (8); `lua_methods.cpp` notepad window-rect block behind the always-NULL headless `FindNotepad` (14) |
| Non-Lua engine arms | **204** | The COM/`COleSafeArray` dispatch halves of every dual-path script callback — taken only when the engine is VBScript/JScript/etc., which mushcore never embeds: `ExecuteTriggerScript` (33), `ExecuteAliasScript` (33), `plugins.cpp` VARIANT result-conversion (33), `mxpStart` open-tag (36), `mxpError` (30), `mxpClose` (20), `methods_plugins.cpp` `CallPlugin` (19) |
| Shim-inert stub bodies | **260** | `platform/mfc_ui.h` one-line GDI/window stubs (`return false;`, `{}` bodies, Win32 constant blocks) that exist so canonical code links; they contain no logic and only the wx product path would call them |
| Tool artifacts | **215** | `stdafx.h` vs `StdAfx.h`: llvm-cov counts the same physical header twice under its two include spellings (56 × 2 = 112); `platform/mfc_ui.h` multi-object merge attribution — 103 header-inline lines that a per-object `llvm-cov show` proves executed (e.g. `AfxDispatchUpdateUI`, the CDocument open/save shims) but that the merged view reports 0 because the profile record is bound to a different test binary's unexecuted instantiation |
| Defensive / compile-time-dead guards | **100** | `xmlparse.cpp`: `GetName` name-not-supplied/comment arms shadowed by the leading-character check (13), truncated-read + allocation-failure arms (8), CDATA-inside-definition double-check (1), the exponent-sign `p++` that tests the 'E' itself (1), `IsNumber`'s >LONG_MAX reject — unreachable on LP64 where `_atoi64` clamps at the same width (1); `doc_shared.cpp`: dead `break` after `return` (1), MCCP leftover-input memmove the drain loop cannot leave behind (1), MCCP realloc-failure arm (7), inflate-error-without-message arms (3), `strerror`-NULL fallback — glibc/macOS never return NULL (2), the GB2312 wrap split shadowed by the Big5 range test (1), `CMemoryException` catch (5), `CheckScriptingAvailable`-failure return behind the always-false headless stub (1), `LookupHostName` synchronous-fallback failure returns — the transport always exists (2); `doc_portable.cpp` `/dev/urandom`-failure fallbacks (12); `telnet_phases.cpp`: MCCP malloc-failure arm (4), the `static const bool g_debug = false` tracing block (14), `inflateReset`-failure arms v1+v2 (14); `mcdatetime.cpp` `ole_to_localtime` `ms_out` arm — no caller passes it (3); `platform/mfc_ui.h` UTF-16 surrogate-pair encode under `sizeof(wchar_t)==2` — compile-time dead on LP64 (6) |
| **Total categorised unreachable** | **1,377** | |

**Reachable-adjusted coverage:** 29,366 covered / (33,819 − 1,377) reachable
= **90.52%**.  Stated precisely: *of the shipping lines a headless test can
execute at all, 90.5% are executed by the suite* (conservative — see above).

The largest remaining honestly-reachable gaps are the MXP tag-family long
tail (`mxpClose`/`mxpDefs`/`mxpEntities`/`mxpOnOff`), `doc_portable.cpp`
host-callback plumbing, `methods_noting.cpp`/`methods_utilities.cpp`
formatting corners, and `posix_transport.cpp` paths that need live socket
failures.

## Per-file table

Lines / missed / coverage per shipping source file (llvm-cov, coverage build):

| File | Lines | Missed | Cover |
|---|---:|---:|---:|
| Color.cpp | 161 | 21 | 86.96% |
| Dmetaph.cpp | 652 | 73 | 88.80% |
| HyprLink.h | 9 | 9 | 0.00% |
| Line.cpp | 45 | 16 | 64.44% |
| MUSHview.h | 6 | 6 | 0.00% |
| OtherTypes.h | 150 | 9 | 94.00% |
| ProcessPreviousLine.cpp | 922 | 48 | 94.79% |
| Replace.cpp | 26 | 18 | 30.77% |
| StatLink.h | 1 | 1 | 0.00% |
| StdAfx.h | 56 | 56 | 0.00% |
| UDPsocket.cpp | 13 | 13 | 0.00% |
| ansi.cpp | 376 | 31 | 91.76% |
| doc.h | 37 | 37 | 0.00% |
| doc_display_shared.cpp | 88 | 17 | 80.68% |
| doc_portable.cpp | 655 | 118 | 81.98% |
| doc_scripting_shared.cpp | 44 | 42 | 4.55% |
| doc_shared.cpp | 2689 | 87 | 96.76% |
| evaluate.cpp | 714 | 63 | 91.18% |
| exceptions.cpp | 69 | 49 | 28.99% |
| exceptions.h | 10 | 10 | 0.00% |
| format.h | 7 | 0 | 100.00% |
| genprint.h | 5 | 5 | 0.00% |
| globalregistryoptions.cpp | 217 | 16 | 92.63% |
| lauxlib.h | 0 | 0 | - |
| lua.h | 0 | 0 | - |
| luaconf.h | 0 | 0 | - |
| mcdatetime.cpp | 165 | 13 | 92.12% |
| mcdatetime.h | 61 | 0 | 100.00% |
| miniwindow.h | 19 | 19 | 0.00% |
| mxp/mxp.cpp | 32 | 6 | 81.25% |
| mxp/mxp.h | 0 | 0 | - |
| mxp/mxpClose.cpp | 270 | 100 | 62.96% |
| mxp/mxpCloseAtomic.cpp | 99 | 19 | 80.81% |
| mxp/mxpDefs.cpp | 317 | 81 | 74.45% |
| mxp/mxpEnd.cpp | 67 | 18 | 73.13% |
| mxp/mxpEntities.cpp | 88 | 40 | 54.55% |
| mxp/mxpError.cpp | 118 | 35 | 70.34% |
| mxp/mxpMode.cpp | 61 | 5 | 91.80% |
| mxp/mxpOnOff.cpp | 93 | 30 | 67.74% |
| mxp/mxpOpenAtomic.cpp | 583 | 74 | 87.31% |
| mxp/mxpStart.cpp | 403 | 90 | 77.67% |
| mxp/mxp_phases.cpp | 69 | 0 | 100.00% |
| mxp/mxpinit.cpp | 52 | 1 | 98.08% |
| mxp/mxputils.cpp | 335 | 46 | 86.27% |
| pal/cpu_raster.cpp | 375 | 58 | 84.53% |
| pal/cpu_raster.h | 3 | 1 | 66.67% |
| pal/iclock.h | 1 | 0 | 100.00% |
| pal/isurface.h | 12 | 0 | 100.00% |
| pal/itransport.h | 2 | 0 | 100.00% |
| pal/iworldview.h | 1 | 0 | 100.00% |
| paneline.cpp | 47 | 33 | 29.79% |
| paneline.h | 36 | 16 | 55.56% |
| platform/compat.h | 208 | 26 | 87.50% |
| platform/doc_gui_method_stubs.cpp | 455 | 42 | 90.77% |
| platform/doc_stub.h | 385 | 61 | 84.16% |
| platform/mainfrm_stub.h | 13 | 13 | 0.00% |
| platform/mfc_collections.h | 158 | 3 | 98.10% |
| platform/mfc_exceptions.h | 70 | 35 | 50.00% |
| platform/mfc_object.h | 15 | 14 | 6.67% |
| platform/mfc_string.h | 258 | 39 | 84.88% |
| platform/mfc_time.h | 107 | 43 | 59.81% |
| platform/mfc_ui.h | 2617 | 418 | 84.03% |
| platform/posix_socket.h | 214 | 30 | 85.98% |
| platform/posix_transport.cpp | 571 | 67 | 88.27% |
| platform/windows_stubs.cpp | 80 | 30 | 62.50% |
| platform/windows_types.h | 26 | 14 | 46.15% |
| plugins.cpp | 1053 | 385 | 63.44% |
| plugins.h | 13 | 9 | 30.77% |
| png/png.h | 0 | 0 | - |
| regexp.cpp | 127 | 11 | 91.34% |
| regexp.h | 21 | 0 | 100.00% |
| scripting/bcconfig.h | 0 | 0 | - |
| scripting/lbc.c | 193 | 20 | 89.64% |
| scripting/lua_compress.c | 324 | 61 | 81.17% |
| scripting/lua_methods.cpp | 3759 | 67 | 98.22% |
| scripting/lua_scripting.cpp | 504 | 59 | 88.29% |
| scripting/lua_utils_portable.cpp | 568 | 86 | 84.86% |
| scripting/md5.c | 159 | 6 | 96.23% |
| scripting/md5.h | 0 | 0 | - |
| scripting/methods/methods_aliases.cpp | 494 | 85 | 82.79% |
| scripting/methods/methods_arrays.cpp | 287 | 18 | 93.73% |
| scripting/methods/methods_clipboard.cpp | 8 | 0 | 100.00% |
| scripting/methods/methods_colours.cpp | 106 | 29 | 72.64% |
| scripting/methods/methods_commands.cpp | 216 | 57 | 73.61% |
| scripting/methods/methods_database.cpp | 397 | 38 | 90.43% |
| scripting/methods/methods_defaults.cpp | 130 | 58 | 55.38% |
| scripting/methods/methods_info.cpp | 849 | 61 | 92.82% |
| scripting/methods/methods_infobar.cpp | 20 | 0 | 100.00% |
| scripting/methods/methods_logging.cpp | 25 | 1 | 96.00% |
| scripting/methods/methods_mapping.cpp | 66 | 1 | 98.48% |
| scripting/methods/methods_noting.cpp | 532 | 91 | 82.89% |
| scripting/methods/methods_plugins.cpp | 374 | 46 | 87.70% |
| scripting/methods/methods_sending.cpp | 53 | 18 | 66.04% |
| scripting/methods/methods_sounds.cpp | 62 | 2 | 96.77% |
| scripting/methods/methods_speedwalks.cpp | 112 | 35 | 68.75% |
| scripting/methods/methods_spellchecker.cpp | 203 | 54 | 73.40% |
| scripting/methods/methods_timers.cpp | 574 | 51 | 91.11% |
| scripting/methods/methods_tracing.cpp | 33 | 13 | 60.61% |
| scripting/methods/methods_triggers.cpp | 559 | 80 | 85.69% |
| scripting/methods/methods_udp.cpp | 89 | 35 | 60.67% |
| scripting/methods/methods_utilities.cpp | 351 | 89 | 74.64% |
| scripting/methods/methods_variables.cpp | 63 | 3 | 95.24% |
| scripting/methods/methods_worlds.cpp | 60 | 22 | 63.33% |
| scripting/methods/methods_xml.cpp | 171 | 33 | 80.70% |
| scripting/mt19937ar.c | 78 | 17 | 78.21% |
| scripting/number.h | 0 | 0 | - |
| scripting/scriptengine.cpp | 80 | 37 | 53.75% |
| scripting/scripting.h | 31 | 10 | 67.74% |
| scripting/sha256.c | 182 | 6 | 96.70% |
| scripting/sha256.h | 0 | 0 | - |
| scripting/shs.c | 111 | 10 | 90.99% |
| scripting/shs.h | 0 | 0 | - |
| scripting/world_debug_portable.cpp | 321 | 83 | 74.14% |
| scriptingoptions.cpp | 287 | 51 | 82.23% |
| sendvw.h | 1 | 1 | 0.00% |
| speedwalk.cpp | 189 | 27 | 85.71% |
| stdafx.h | 56 | 56 | 0.00% |
| telnet_phases.cpp | 607 | 36 | 94.07% |
| timers.cpp | 164 | 36 | 78.05% |
| utilities_shared.cpp | 534 | 20 | 96.25% |
| xml/xml_load_world.cpp | 1575 | 57 | 96.38% |
| xml/xml_save_world.cpp | 468 | 34 | 92.74% |
| xml/xml_serialize.cpp | 126 | 51 | 59.52% |
| xml/xmlparse.cpp | 994 | 32 | 96.78% |
| xml/xmlparse.h | 22 | 0 | 100.00% |

## Reproducing

```bash
tests/coverage.sh            # per-file summary (builds build-cov, runs all suites)
tests/coverage.sh show FILE  # annotated source for one file
tests/coverage.sh export     # lcov-style JSON → build-cov/coverage.json
```
