# Shipping-line coverage — the headless (mushcore) test suite

**As of:** 2026-07-19, immediately after the Phase 2 B-3 atomic flip (`fce551f`).
Earlier rounds (pre-flip, denominator 33,819 lines): 43.8% → 55.7% → 68.2% →
78.3% → 84.5% → 86.83%.

## Headline numbers

| Metric | Value |
|---|---|
| **Line coverage (raw)** | **81.46%** (30,294 / 37,190 shipping lines) |
| Region coverage | 80.17% |
| Function coverage | 80.48% |
| Branch coverage | 70.69% |
| Test executables | 28 suites + 3 in-app PAL selftests, ctest 31/31 |

Measured by `tests/coverage.sh` (llvm-cov over every `test_*` executable in
`build-cov`, third-party sources excluded — see the `IGNORE` regex in that
script for the exact exclusion list).

### Why the raw % moved (86.83% → 81.46%) — not a regression

The B-3 flip made `mushcore` compile the **real** `doc.h` / `doc.cpp` /
`doc_construct.cpp` and the plugin-wizard TUs in place of the deleted
`doc_portable.cpp` / `doc_stub.h`. That grew the instrumented denominator by
**+3,371 lines** (33,819 → 37,190) while covered lines *rose* by **+928**
(29,366 → 30,294). The dip in the ratio is purely those newly-instrumented but
not-yet-exercised lines — dominated by the real `doc.cpp` (2,091 lines, **19.94 %**
covered), `dialogs/plugins/PluginWizard.cpp` (667 lines, 9.15 %) and
`platform/doc_flip_gui_stubs.cpp` (205 lines, 8.78 %). No previously-covered line
lost coverage; this is fresh surface to test — **future coverage work, not a
regression**.

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

> **⚠ Pre-flip (round 5) audit — pending re-audit.** The per-category line counts
> and the 33,819-line denominator below **predate the B-3 flip** and cite files
> that no longer exist (`doc_portable.cpp`, `doc_stub.h`); the reachable-adjusted
> **≈ 90.5 %** figure is computed on that old denominator. It is retained as the
> last completed audit. A fresh categorisation against the new 37,190-line
> denominator is outstanding — it must additionally classify the real
> `doc.cpp` / `doc_construct.cpp` GUI-only arms and the
> `PluginWizard` / `doc_flip_gui_stubs.cpp` stub bodies the flip newly
> instrumented.

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
| ansi.cpp | 376 | 30 | 92.02% |
| chatsock.h | 0 | 0 | - |
| Color.cpp | 161 | 21 | 86.96% |
| DDV_validation.cpp | 29 | 14 | 51.72% |
| dialogs/DebugLuaDlg.h | 1 | 0 | 100.00% |
| dialogs/plugins/PluginWizard.cpp | 667 | 606 | 9.15% |
| dialogs/plugins/PluginWizardSheet.cpp | 40 | 18 | 55.00% |
| dialogs/ProgDlg.h | 1 | 1 | 0.00% |
| dialogs/ScriptErrorDlg.h | 1 | 1 | 0.00% |
| dialogs/world_prefs/genpropertypage.h | 2 | 2 | 0.00% |
| dialogs/world_prefs/prefspropertypages.h | 7 | 7 | 0.00% |
| Dmetaph.cpp | 652 | 73 | 88.80% |
| doc.cpp | 2091 | 1674 | 19.94% |
| doc.h | 282 | 38 | 86.52% |
| doc_construct.cpp | 482 | 130 | 73.03% |
| doc_display_shared.cpp | 88 | 17 | 80.68% |
| doc_shared.cpp | 2748 | 99 | 96.40% |
| evaluate.cpp | 714 | 63 | 91.18% |
| exceptions.cpp | 69 | 49 | 28.99% |
| exceptions.h | 10 | 10 | 0.00% |
| format.h | 7 | 0 | 100.00% |
| genprint.h | 5 | 5 | 0.00% |
| globalregistryoptions.cpp | 217 | 16 | 92.63% |
| HyprLink.h | 9 | 9 | 0.00% |
| lauxlib.h | 0 | 0 | - |
| Line.cpp | 45 | 16 | 64.44% |
| lua.h | 0 | 0 | - |
| luaconf.h | 0 | 0 | - |
| mcdatetime.cpp | 165 | 13 | 92.12% |
| mcdatetime.h | 61 | 0 | 100.00% |
| miniwindow.h | 19 | 19 | 0.00% |
| MUSHclient.h | 1 | 0 | 100.00% |
| MUSHview.h | 7 | 7 | 0.00% |
| mxp/mxp.cpp | 32 | 6 | 81.25% |
| mxp/mxp.h | 0 | 0 | - |
| mxp/mxp_phases.cpp | 69 | 0 | 100.00% |
| mxp/mxpClose.cpp | 270 | 100 | 62.96% |
| mxp/mxpCloseAtomic.cpp | 99 | 19 | 80.81% |
| mxp/mxpDefs.cpp | 317 | 81 | 74.45% |
| mxp/mxpEnd.cpp | 67 | 18 | 73.13% |
| mxp/mxpEntities.cpp | 88 | 40 | 54.55% |
| mxp/mxpError.cpp | 118 | 35 | 70.34% |
| mxp/mxpinit.cpp | 52 | 1 | 98.08% |
| mxp/mxpMode.cpp | 61 | 5 | 91.80% |
| mxp/mxpOnOff.cpp | 93 | 30 | 67.74% |
| mxp/mxpOpenAtomic.cpp | 583 | 74 | 87.31% |
| mxp/mxpStart.cpp | 403 | 90 | 77.67% |
| mxp/mxputils.cpp | 335 | 46 | 86.27% |
| othertypes.h | 150 | 9 | 94.00% |
| pal/cpu_raster.cpp | 375 | 58 | 84.53% |
| pal/cpu_raster.h | 3 | 1 | 66.67% |
| pal/iclock.h | 1 | 0 | 100.00% |
| pal/isurface.h | 12 | 0 | 100.00% |
| pal/itransport.h | 2 | 0 | 100.00% |
| pal/iworldview.h | 1 | 0 | 100.00% |
| paneline.cpp | 47 | 33 | 29.79% |
| paneline.h | 36 | 16 | 55.56% |
| platform/compat.h | 208 | 26 | 87.50% |
| platform/doc_flip_gui_stubs.cpp | 205 | 187 | 8.78% |
| platform/doc_gui_method_stubs.cpp | 455 | 42 | 90.77% |
| platform/doc_headless_stubs.cpp | 34 | 12 | 64.71% |
| platform/doc_port_glue.cpp | 506 | 106 | 79.05% |
| platform/gdi_headless.cpp | 103 | 1 | 99.03% |
| platform/headless_app.cpp | 67 | 9 | 86.57% |
| platform/mainfrm_stub.h | 15 | 14 | 6.67% |
| platform/mfc_collections.h | 158 | 3 | 98.10% |
| platform/mfc_exceptions.h | 70 | 35 | 50.00% |
| platform/mfc_object.h | 15 | 14 | 6.67% |
| platform/mfc_string.h | 258 | 39 | 84.88% |
| platform/mfc_time.h | 107 | 42 | 60.75% |
| platform/mfc_ui.h | 2505 | 330 | 86.83% |
| platform/posix_socket.h | 214 | 30 | 85.98% |
| platform/posix_transport.cpp | 571 | 67 | 88.27% |
| platform/windows_stubs.cpp | 59 | 35 | 40.68% |
| platform/windows_types.h | 26 | 14 | 46.15% |
| plugins.cpp | 1052 | 385 | 63.40% |
| plugins.h | 13 | 9 | 30.77% |
| png/png.h | 0 | 0 | - |
| ProcessPreviousLine.cpp | 922 | 48 | 94.79% |
| regexp.cpp | 127 | 11 | 91.34% |
| regexp.h | 21 | 0 | 100.00% |
| Replace.cpp | 26 | 18 | 30.77% |
| resource.h | 0 | 0 | - |
| scripting/bcconfig.h | 0 | 0 | - |
| scripting/lbc.c | 193 | 20 | 89.64% |
| scripting/lua_compress.c | 324 | 61 | 81.17% |
| scripting/lua_methods.cpp | 3758 | 67 | 98.22% |
| scripting/lua_scripting.cpp | 517 | 70 | 86.46% |
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
| scripting/shs.c | 111 | 1 | 99.10% |
| scripting/shs.h | 0 | 0 | - |
| scripting/world_debug_portable.cpp | 321 | 83 | 74.14% |
| scriptingoptions.cpp | 287 | 51 | 82.23% |
| sendvw.h | 1 | 1 | 0.00% |
| serialize.cpp | 55 | 49 | 10.91% |
| speedwalk.cpp | 189 | 27 | 85.71% |
| StatLink.h | 1 | 1 | 0.00% |
| StdAfx.h | 12 | 12 | 0.00% |
| stdafx.h | 59 | 7 | 88.14% |
| telnet_phases.cpp | 607 | 36 | 94.07% |
| TextDocument.h | 1 | 1 | 0.00% |
| timers.cpp | 181 | 50 | 72.38% |
| UDPsocket.cpp | 13 | 13 | 0.00% |
| utilities_portable.cpp | 7 | 7 | 0.00% |
| utilities_shared.cpp | 534 | 20 | 96.25% |
| xml/xml_load_world.cpp | 1569 | 51 | 96.75% |
| xml/xml_save_world.cpp | 468 | 26 | 94.44% |
| xml/xml_serialize.cpp | 126 | 51 | 59.52% |
| xml/xmlparse.cpp | 994 | 32 | 96.78% |
| xml/xmlparse.h | 22 | 0 | 100.00% |

## Reproducing

```bash
tests/coverage.sh            # per-file summary (builds build-cov, runs all suites)
tests/coverage.sh show FILE  # annotated source for one file
tests/coverage.sh export     # lcov-style JSON → build-cov/coverage.json
```
