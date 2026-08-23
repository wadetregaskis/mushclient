# Upstream bugs found & fixed during the macOS port

These are defects in shared, platform-independent logic — present in the
upstream Windows MUSHclient source, not port artifacts. Each was found while
bringing the real shipping code under the headless test suite, fixed in this
tree, and is suitable to report/patch upstream. Port file names are given with
their upstream equivalent where the port single-sources a method into a shared
TU (e.g. doc_shared.cpp ← doc.cpp).

A few rows are **not** fixed for Windows, and say so in the *Port commit* column
("port only", "not fixed", "port-guarded only"): the defect is real upstream, but
closing it there is a Windows-observable behaviour change that needs a decision
rather than a port fix. Those rows exist so the defect is on record for whoever
patches upstream, not as a claim that this tree fixed it.

| # | Area | Port commit | Upstream file / function |
|---|------|-------------|--------------------------|
| 1 | MXP element delete UAF | `2cae10b` | mxpDefs.cpp `CMUSHclientDoc::MXP_Element` |
| 2 | MCCP decompress realloc | `ae6bbe4` | doc.cpp (MCCP decompress grow loop) |
| 3 | Deferred-note drain UAF | `f0f2866` | doc.cpp `CMUSHclientDoc::OutputOutstandingLines` |
| 4 | XML double signed exponent | `49e5ba6` | xmlparse.cpp `Get_XML_double` |
| 5 | XML date day-range message | `49e5ba6` | xmlparse.cpp `Get_XML_date` |
| 6 | XML date second-range message | `49e5ba6` | xmlparse.cpp `Get_XML_date` |
| 7 | Timer at_time toggle HOUR→MINUTE | `eea4a45` | methods_timers.cpp `CMUSHclientDoc::SetTimerOption` |
| 8 | Custom-colour index used unchecked (`note_text_colour`) | `36db6c1` (partial) | methods_noting.cpp note-colour getters; prefspropertypages.cpp `FixSwatch` |
| 9 | xterm-256 colour index subscripts the 8-entry print-style arrays | port-guarded only | mushview.cpp `CMUSHView::PrintWorld` |
| 10 | DBCS wrap splits the character when `indent_paras` is off | this commit | doc_shared.cpp ← doc.cpp `CMUSHclientDoc::AddToLine` |
| 11 | `ClearOutput` leaves `m_last_line_with_IAC_GA` naming a deleted line | this commit | doc.cpp `CMUSHclientDoc::ClearOutput` |
| 12 | The queued-command "no log" flag can never suppress logging | this commit | TimerWnd.cpp `CTimerWnd::OnTimer`; mirrored in platform/wx_world_child.cpp |
| 13 | Windows' two queue drains disagree about case, so a `nolog` command echoes from one and not the other | this commit | TimerWnd.cpp `OnTimer` vs `ChangeTimerRate` |
| 14 | Menu-triggered alias's own `%1` is dropped (subscript computed, never used) | this commit | mushview.cpp `CMUSHView::OnMXPMenu` ACTION_ALIAS arm (now `doc_aliasmenu_shared.cpp` `ExecuteAliasMenuItem`) |
| 15 | NAWS sends a dimension byte of 255 as a bare IAC, corrupting the subnegotiation | this commit | doc_shared.cpp ← doc.cpp `CMUSHclientDoc::SendWindowSizes` |
| 16 | MXP hyperlink from the right-click menu refuses `https://` | this commit | mushview.cpp `CMUSHView::OnMXPMenu` ACTION_HYPERLINK arm |
| 17 | Double-click word scan reads one byte past the line buffer | this commit | mushview.cpp `CMUSHView::OnLButtonDblClk`, `OnDisplayTextattributes` |
| 18 | `CmcDateTime::operator-`'s Windows arm discards a null/invalid span status | this commit | mcdatetime.cpp `CmcDateTime::operator-(const CmcDateTime&)` |
| 19 | Doc ctor's unqualified `GetUniqueID()` picks the automation member and leaks its BSTR once per document | this commit | doc_construct.cpp (ex doc.cpp ctor) `m_strWorldID = GetUniqueID()` — since the 2010 import; fix is the `::` the three sibling callers already use |
| 20 | A MouseOver callback that deletes its own hotspot crashes the client | this commit | mushview.cpp `CMUSHView::Mouse_Move_MiniWindow` (now `miniwindow_compose.cpp` `MouseMoveMiniWindow`) |
| 21 | The second layout pass wipes the first pass's "would not fit" decision | this commit | mushview.cpp `CMUSHView::Calculate_MiniWindow_Rectangles` (now `miniwindow_compose.cpp` `CalculateMiniWindowRectangles`) |
| 22 | Trigger "multi-line" tick is not compared, so un-ticking it is discarded | this commit | prefspropertypages.cpp `CPrefsP8::CheckIfChanged` |
| 23 | Command-window colours are lost by visiting another prefs page | this commit | prefspropertypages.cpp `CPrefsP9` (no `OnKillActive`) |
| 24 | The default echo colour always reports a change (SAMECOLOUR vs -1) | this commit | configuration.cpp `CMUSHclientDoc::ChangedPrefsP9` |
| 25 | The default note colour always reports a change (the same defect) | this commit | configuration.cpp `CMUSHclientDoc::ChangedPrefsP17` |
| 26 | "Fit the last column to the list view" computes a width nothing applies | this commit | prefspropertypages.cpp `CPrefsP7`/`CPrefsP16`/`CPrefsP18::OnInitDialog` |
| 27 | The Timers list's Contents column header does not sort | this commit | prefspropertypages.cpp `CPrefsP16::CompareObjects` |
| 28 | The alias page's column arrays are indexed with the trigger page's enum | this commit | configuration.cpp `CMUSHclientDoc::LoadPrefsP7` |
| 29 | The printer-font picker opens on the font from before the sheet opened | this commit | GlobalPrefs.cpp `CGlobalPrefsP4::OnFont` |
| 30 | `metaphone()` copies a word into a 61-byte stack buffer with no bound | this commit | Utilities.cpp `metaphone` |
| 31 | Greyscale PNGs decode garbled, and palette PNGs do too from memory | this commit | png_load_shared.cpp ← Utilities.cpp `LoadPng` / `LoadPngMemory` |
| 32 | `world.BlendPixel` mode 64 (HSL) ignores the blend colour entirely | this commit | methods_pixel_shared.cpp ← methods_miniwindows.cpp `CMUSHclientDoc::BlendPixel` |
| 33 | `world.WindowWrite` writes a .bmp strict readers refuse to open | this commit | miniwindow.cpp `CMiniWindow::Write` |
| 34 | `Blend_ColorBurn`'s underflow clamp is dead code (unsigned comparison) | this commit | blending.h `Blend_ColorBurn` |
| 35 | `WindowAddHotspot`'s re-entrancy guard is shared by every open world | this commit | methods_miniwindows.cpp `CMUSHclientDoc::WindowAddHotspot` |
| 36 | `world.ArrayImport`'s table form crashes on any non-string value | this commit | lua_methods.cpp `L_ArrayImport` |
| 37 | `world.CallPlugin` keeps pointers into two Lua strings it has removed from the stack | this commit | lua_methods.cpp `L_CallPlugin` |
| 38 | `world.SetRemoveMapReverses` raises an error when passed a boolean | this commit | lua_methods.cpp `L_SetRemoveMapReverses` |
| 39 | Three world-table entries reject the documented `other:Method (…)` form | this commit | lua_methods.cpp `L_StripANSI`, `L_GetWorld`, `L_GetWorldById` |
| 40 | `pushVariant` hands an uninitialised `VARIANT` to `SafeArrayGetElement` | this commit | lua_methods.cpp `pushVariant` (VT_ARRAY arm) |
| 41 | The note colours a deferred note carries are never initialised | this commit | doc_construct.cpp `CMUSHclientDoc::CMUSHclientDoc` |
| 42 | `GetRecentLines` narrows its start position to `int`, so a large Count reads outside the buffer | this commit | methods_output.cpp `CMUSHclientDoc::GetRecentLines` |
| 43 | A names file with an empty section makes name generation divide by zero and subscript an empty array | this commit | NameGeneration.cpp `ReadNames`, `GenerateName` |
| 44 | `DebugHelper`'s `callbacklist` branch dereferences a NULL plugin | this commit | world_debug.cpp `CMUSHclientDoc::DebugHelper` |
| 45 | Five `DebugHelper` early returns leave the document stuck in a plugin's context | this commit | world_debug.cpp `CMUSHclientDoc::DebugHelper` |
| 46 | `GetTrigger` reports `omit_from_log` back as `eOmitFromOutput`, so a save/restore round-trip turns it into `omit_from_output` | this commit | methods_triggers.cpp `CMUSHclientDoc::GetTrigger` |
| 47 | `world.GetUdpPort` never considers `Last` itself, so it fails for any single-port range | this commit | methods_udp.cpp `CMUSHclientDoc::GetUdpPort` |
| 48 | `InfoTypes[]` reuses codes 31/32 for "world get focus"/"world lose focus", clobbering the "world connect"/"world disconnect" rows and omitting 33/34 | this commit | methods_info.cpp `InfoTypes[]` |
| 49 | Plugins dialog reads a stale `CPlugin*` once it leaves `m_PluginList` | this commit | dialogs/plugins/PluginsDlg.cpp `CPluginsDlg::OnUpdateNeedDescription` |
| 50 | Seven file dialogs `strcpy` an unbounded suggested name into a `_MAX_PATH` buffer | this commit | doc.cpp / dialogs (log-session, save-notes, select-sound et al) — bounded with `lstrcpyn` |
| 51 | A duplicate trigger/alias label whose replacement fails validation leaves the freed original in the map | this commit | xml_load_world.cpp `CMUSHclientDoc::Load_One_Trigger_XML` / `Load_One_Alias_XML` |
| 52 | A nested plugin dispatch (e.g. `world.Send` from inside `ON_PLUGIN_LINE_RECEIVED`) turns note batching off for the rest of the outer dispatch, so a later plugin's `Note`/`Tell` lands mid-line instead of being queued | this commit | plugins.cpp `CMUSHclientDoc::SendToAllPluginCallbacks*` (six dispatchers) |
| 53 | The plugin wizard re-sorts the whole world once per surviving item when removing exported triggers/aliases/timers/variables, making cleanup O(N² log N) | this commit | plugins.cpp `CMUSHclientDoc::OnFilePluginwizard` / `RemoveSelectedWizardItems` |
| 54 | The Lua `error_code`/`error_desc` tables key `ePluginCouldNotSaveState` on 30037 instead of 30038, duplicating `ePluginDoesNotSaveState`'s code | this commit | lua_methods.cpp `error_codes[]`; errors.cpp `error_descriptions[]` |
| 55 | `ExecuteLua`'s "function not found" arm returns without restoring `m_iNoteStyle` | this commit | lua_scripting.cpp `CScriptEngine::ExecuteLua` (both overloads) |
| 56 | `world.ArrayImport`'s un-escape pass mistakes a doubled backslash right before the real delimiter for an escaped one, eating the field separator and corrupting or rejecting the imported key/value pairs | this commit | methods_arrays.cpp `CMUSHclientDoc::ArrayImport` |
| 57 | `world.Help`'s fallback binds its Lua flag to `ShowFunctionslist`'s start-offset parameter instead of its `bLua` parameter, so a scripting-enabled world's Function List never lists Lua's own functions | this commit | methods_utilities.cpp `CMUSHclientDoc::Help`; scripting/functionlist.cpp `CMainFrame::OnGameFunctionslist` |
| 58 | `world.ImportXML` leaks the `CArchiveException` on every failed import, because its `catch (CArchiveException*)` has no parameter name to call `Delete()` on | this commit | methods_xml.cpp `CMUSHclientDoc::ImportXML` |
| 59 | `SetBackgroundImage` and `SetForegroundImage` duplicate a 66-line body, so any fix to the image-loading logic has to be made twice and can silently be made once | this commit | methods_output.cpp `CMUSHclientDoc::SetBackgroundImage` / `SetForegroundImage` |
| 60 | The RGB bold/unbold colour scans stop one palette entry short, so white text never swaps | this commit | ansi.cpp `CMUSHclientDoc::InterpretANSIcode` (`ANSI_BOLD` / `ANSI_CANCEL_BOLD`) |
| 61 | `CmcDateTime`'s constructor truncates a fractional second, so an `at_time` timer fires up to a second early | this commit | mcdatetime.cpp `mcDateFromTm` (via `CmcDateTime::SetDateTime`) |
| 62 | `DisplayMsg`'s tab stop measures the line in bytes, so a tab after multi-byte text lands on the wrong column | this commit | doc_shared.cpp ← doc.cpp `CMUSHclientDoc::DisplayMsg` (`'\t'` and period-space cases) |
| 63 | "Use SSL" is not compared in `ChangedPrefsP1`, so ticking it and nothing else is never applied to the document or saved to the world file | this commit | configuration.cpp `CMUSHclientDoc::ChangedPrefsP1` |
| 64 | `RecompileRegularExpressions` recompiles a NULL-expression trigger from the PREVIOUS trigger's pattern, or an empty one, which matches every line | this commit | configuration.cpp `CMUSHclientDoc::RecompileRegularExpressions` |
| 65 | `CTipDlg`'s constructor `strcat`s the tips-file suffix onto a fixed `MAX_PATH` buffer with no bound, smashing the stack for a long enough install path | this commit | TipDlg.cpp `CTipDlg::CTipDlg` |
| 66 | The mapper's "make alias" keeps its own stale copy of the dialog-to-`CAlias` field list, discarding seven fields the world-properties Aliases page already saves | this commit | MapDlg.cpp `CMapDlg::AddAlias`; prefspropertypages.cpp `CPrefsP7::UnloadDialog` |
| 67 | World Properties' `paste_delay`/`send_file_delay` DDV ceilings (10000) are narrower than the option table's (100000), so the dialog refuses to close on a value the table already accepted | this commit | prefspropertypages.cpp `CPrefsP10::DoDataExchange` / `CPrefsP13::DoDataExchange` |
| 68 | MCCP `Z_NEED_DICT` from a crafted zlib header is not treated as a decompress failure, spinning the decompressor in an unbounded 100%-CPU loop | this commit | doc_shared.cpp ← doc.cpp `CMUSHclientDoc::OnBytesReceived` (MCCP decompress loop) |
| 69 | MCCP stream-end residue can exceed both transports' receive buffers, overflowing a 9000-byte stack array with up to 10000 bytes of real post-stream data | this commit | platform/posix_transport.cpp; pal/win_transport.cpp |
| 70 | MCCP compression-start `memmove` into `m_CompressInput` has no size guard, overflowing the buffer when the post-`SE` remainder exceeds `COMPRESS_BUFFER_LENGTH` | this commit | doc_shared.cpp ← doc.cpp `CMUSHclientDoc::DisplayMsg` (`Phase_COMPRESS_WILL` / `Phase_SUBNEGOTIATION_IAC`) |
| 71 | `/triggerlist`, `/aliaslist`, `/variablelist` and `/acceleratorlist` compute their truncation width as `(m_nWrapColumn - N)` with no floor, so a narrow wrap column drives it to zero or negative and `CString::Left` (which clamps `n<=0` to empty) prints a bare `"..."` for every entry | this commit | world_debug.cpp `CMUSHclientDoc::DebugHelper` (triggerlist/aliaslist/variablelist/acceleratorlist) |
| 72 | `strnrchr` never increments its counter (`while (*s && n < count++)`), so it ignores the `count` length bound and scans to the first NUL; both MXP `<P>`-mode callers pass `m_pCurrentLine->text`, a `CLine` buffer of exactly `len` bytes with no NUL terminator, so a hostile long line with no space reads past `len` into uninitialised heap (and possibly past the allocation) | `6491c97` | utilities_shared.cpp `strnrchr`; doc_shared.cpp ← doc.cpp `CMUSHclientDoc::DisplayMsg` (MXP `<P>` last-space scan) |
| 73 | MXP's malformed-tag/definition error returns leak their argument lists: `MXP_StartTag`'s custom-element expansion drops the LOCAL `BuiltArgumentList` (the `CArgument` nodes already built for the tag) on an unterminated-entity abort, and both it and `MXP_Element` skip `DELETE_LIST` on their function-static `ArgumentList` on several early returns | this commit | mxpStart.cpp `CMUSHclientDoc::MXP_StartTag`; mxpDefs.cpp `CMUSHclientDoc::MXP_Element` |
| 74 | A telnet subnegotiation with no terminating `IAC SE` accumulates into `m_IAC_subnegotiation_data` with no size bound, so a hostile server that opens `IAC SB <type>` and streams data forever drives the client to unbounded heap allocation — memory exhaustion (on the 32-bit Windows build `std::string` throws at ~2GB, terminating) | this commit | telnet_phases.cpp `CMUSHclientDoc::Phase_SUBNEGOTIATION` / `Phase_SUBNEGOTIATION_IAC` |
| 75 | Colour-matching triggers compare the trigger's 0-7 ANSI number straight against the style's raw `iForeColour`/`iBackColour` without consulting `iFlags & COLOURTYPE`, so no colour condition can match an xterm-256, 24-bit, MXP or custom-palette run — while a `COLOUR_RGB` run whose `COLORREF` happens to equal a small ANSI number matches by coincidence | `ef5466f` | ProcessPreviousLine.cpp `CMUSHclientDoc::ProcessOneTriggerSequence` |
| 76 | A password typed at an `IAC WILL ECHO` prompt is hidden on screen but written verbatim to the session log: `m_bNoEcho` gates only `SendMsg`'s *echo* argument, while `DoSendMsg` logs on a separate one that `EvaluateCommand` fills from `LoggingInput()` unconditionally — and `Execute`'s `bOmitFromLog` local is hard-coded `false` and never read by `EvaluateCommand`, so Windows has no opt-out | port only (`32b28f6`) | doc_shared.cpp ← doc.cpp `SendMsg`/`DoSendMsg`; evaluate.cpp `EvaluateCommand`; methods_commands.cpp `Execute` |
| 77 | Miniwindow mouse routing holds `CMiniWindow*` and `CHotspot*` across script dispatch while `WindowDelete` refuses only the window whose own callback is running (`m_bExecutingScript`), so a callback deleting a *different* miniwindow leaves the routine hit-testing, dispatching from and storing into freed memory; `m_bExecutingScript` is also a plain `bool`, so a nested dispatch clears the outer call's self-delete guard on the way out | `1d45c28` | miniwindow_compose.cpp ← mushview.cpp `Mouse_Move/Down/Up/Wheel_MiniWindow`; methods_miniwindows.cpp `WindowDelete` |
| 78 | `GeneralFilter`'s vertical pass loads its `+2` tap from `row + 3` where the horizontal pass loads `+2`, so after the window shift the 5-tap kernel spans −1..+3: every blur/sharpen/edge/emboss is centred one row below the row it writes, and region row 2 is sampled by no output row at all | `d8de347` | miniwindow.cpp `CMiniWindow::GeneralFilter` (vertical pass) |
| 79 | `GeneralFilter`'s horizontal loop bound `col < iLastByte - 4` with `col += 3` stops at pixel `width-2`, so the rightmost pixel column of every filtered region is read as a neighbour but never written and survives every filter unprocessed | `d8de347` | miniwindow.cpp `CMiniWindow::GeneralFilter` (horizontal pass) |
| 80 | All four database column readers fetch an INTEGER column with `sqlite3_column_int` — 32 bits — so any value outside `int` range is silently truncated in every scripting language; the array reader then widens the already-truncated `int` to `long` and stamps `VT_I4`, making the loss look deliberate | `648622c` | methods_database.cpp `DatabaseColumnValue`/`DatabaseColumnValues`; lua_methods.cpp `GetDatabaseColumnValue`/`L_DatabaseColumnValues` |
| 81 | `SaveModified` performs the close side effects (`m_bWorldClosing`, `MXP_Off`, the `on_world_close` script) before returning, so `CDocTemplate::SaveAllModified`'s walk leaves every already-prompted world half-closed when the user cancels at a later one | port only (`4d7168f`) | doc.cpp `CMUSHclientDoc::SaveModified` |
| 82 | Convert ▸ Unix→DOS and DOS→Unix are unconditional byte filters that drop every `\r`, so running either over a classic-Mac (CR-only) notepad document deletes every line break in the file, collapsing it to one line — and File ▸ Save writes that back, with no confirmation and no undo | not fixed (port diverges) | TextView.cpp `CTextView::OnConvertUnixtodos` / `OnConvertDostounix` |
| 83 | `L_ReplaceNotepad` pushes its success boolean and then `return 0;` — zero result fields — so Lua callers get nil while the dispatch map hands every other engine the `VT_BOOL` `mushclient.odl` declares; it was the only push-with-`return 0` body in the file | `203c0fe` | lua_methods.cpp `L_ReplaceNotepad` |
| 84 | `GetLineInfo(n).time` builds its field as `(int) m_theTime.GetTime()`, truncating a 64-bit `time_t`, so every line's timestamp comes back negative from 2038-01-19 03:14:08 UTC | `43e27b8` | lua_methods.cpp `L_GetLineInfo` (table form) |
| 85 | `MXP_StartTag` records a built-in tag as open — an active-list entry plus a `START_TAG` marker style — before `MXP_OpenAtomicTag` asks the veto, and nothing unwinds it, so a vetoed tag stays on the active list: a spurious `StartNewLine` from its close, a swallowed outstanding-tag warning, a close-tag callback for an open that was suppressed, and a vetoed `<send>` rewriting the enclosing anchor's action | `3095c2a` | mxpStart.cpp `CMUSHclientDoc::MXP_StartTag`; mxpOpenAtomic.cpp `CMUSHclientDoc::MXP_OpenAtomicTag` |
| 86 | `calculate_line_and_column`'s two UTF-8 fix-ups disagree about direction — the first walks **forward** to the next character's lead byte (`col++`) where the half-way arm below walks **backward** (`col--`) — so the two arms can resolve the same click to different characters, and a click just left of a multi-byte character selects one too far right | not fixed (needs a Windows run) | mushview.cpp `CMUSHView::calculate_line_and_column` |
| 87 | lsqlite3 decides whether a column value survives as a Lua number with `n == i64`, where the usual arithmetic conversions widen the int64 operand to double — so the test reads `(double) i64 == (double) i64`, is vacuously true, and the exact-decimal-string branch beside it is unreachable at all three sites; any value a double cannot hold is pushed to Lua silently **rounded** (a column holding `9007199254740993` reads back as `9007199254740992`) | `fbb6695` | lsqlite3.c `vm_push_column`, `db_last_insert_rowid`, `db_push_value` — vendored third-party code, so upstream lsqlite3 carries it too |
| 88 | `Get_XML_date` builds its `CTime` with `nDST = 0` — "this wall-clock time is standard time" — so a date recorded during daylight saving is handed to `mktime` as if it were not, and reading the hour back through `localtime` returns it **one hour later**.  Measured on Windows: `"2020-05-06 07:08:09"` parses back as hour 8.  Invisible to the port, whose `CTime` shim ignores the argument and always passes `-1` | this commit | xmlparse.cpp `Get_XML_date` — the third bug in this function, after 5 and 6 |

## 1 — MXP `<!ELEMENT … DELETE>` use-after-free (`2cae10b`)
**Bug:** redefining or deleting a custom MXP element frees the old `CElement`
(`delete pElement`) but does not remove its key from `m_CustomElementMap`. The
map key is left pointing at freed memory; any later `<x>` use or redefinition of
the same element dereferences/deletes the dangling pointer.
**Fix:** call `m_CustomElementMap.RemoveKey(strName)` immediately after deleting
the element, so the map never holds a freed pointer.

## 2 — MCCP decompress buffer realloc dangling pointer (`ae6bbe4`)
**Bug:** when inflate returns `Z_BUF_ERROR` the output buffer is grown with
`realloc`, but `z_stream::next_out` still points into the *old* block. `realloc`
may move the allocation, after which inflate writes decompressed bytes through
`next_out` into freed memory.
**Fix:** record `next_out`'s offset from the buffer base before the `realloc`,
then re-point `next_out = buffer + offset` afterwards, so it tracks the moved
block.

## 3 — Deferred-note drain re-entrancy UAF (`f0f2866`)
**Bug:** `OutputOutstandingLines` iterates `m_OutstandingLines` while calling
`Tell()` inside the loop. `Tell` can re-enter the function (Tell → forced line
change → StartNewLine → ProcessPreviousLine → OutputOutstandingLines); the
re-entrant `m_OutstandingLines.clear()` invalidates the outer loop's iterator →
use-after-free. Latent (current call sites drain with `hard_return` set / onto a
comment line, so the recursion guard bails) but fragile.
**Fix:** `swap` the list into a local before iterating. The member is emptied up
front (a re-entrant call sees it empty and returns immediately) and the loop
walks the detached copy, immune to re-entrant mutation. Non-re-entrant behaviour
is unchanged.

## 4 — `Get_XML_double` rejects signed exponents (`49e5ba6`)
**Bug:** in the numeric-attribute validator, on seeing `'E'` the exponent-sign
check tests `*p` — but `*p` is still the `'E'` (the loop's `p++` has not run), so
a following `'-'`/`'+'` is never consumed and falls through to the
invalid-character branch. Values like `"1E-5"` are wrongly rejected.
**Fix:** inspect `p[1]` (the character after the `'E'`) and consume the sign when
present, so signed exponents parse. Strictly more permissive.

## 5 — `Get_XML_date` day-range error message (`49e5ba6`)
**Bug:** the day-of-month range check (`iDay < 1 || iDay > 31`) throws
`"Month must be in range 1 to 31"` — a copy-paste from the month check.
**Fix:** message corrected to `"Day must be in range 1 to 31"`. (Message text
only; no behavioural change.)

## 6 — `Get_XML_date` second-range error message (`49e5ba6`)
**Bug:** the seconds range check (`iSecond < 0 || iSecond > 59`) throws
`"Minute must be in range 0 to 59"` — a copy-paste from the minute check.
**Fix:** message corrected to `"Second must be in range 0 to 59"`. (Message text
only; no behavioural change.)

## 7 — `SetTimerOption("at_time")` copies HOUR into the MINUTE field (`eea4a45`)
**Bug:** toggling a timer between interval (`every`) and at-time modes copies the
component fields across, but the MINUTE copy takes the HOUR source on both arms —
a duplicated-line typo: `iEveryMinute = iAtHour` (should be `iAtMinute`) and
`iAtMinute = iEveryHour` (should be `iEveryMinute`). An "every 2h 30m 15s" timer
flipped to at-time becomes "at 2:02:15" (minute forced to the hour value), and
vice-versa; the hour and second fields copy correctly.
**Fix:** copy the matching minute field (`iEveryMinute = iAtMinute`,
`iAtMinute = iEveryMinute`). Behavioural change for any script/user that toggles a
timer's `at_time` option on a timer whose hour and minute differ.


## 8 — an unvalidated `note_text_colour` is used as an array index

`m_iNoteTextColour` is meant to be a 0..`MAX_CUSTOM` custom-colour index, but
its option-table row permits far more (`scriptingoptions_numeric.inc`):

```
{"note_text_colour", 4, O(m_iNoteTextColour), 0, 0xFFFFFF, OPT_RGB_COLOUR},
```

so `world.SetOption("note_text_colour", 300)` is accepted and stored. Nothing
downstream range-checks it.

**The serious instance — always live, both platforms.** `methods_noting.cpp`
uses it as a bare subscript in roughly ten places, guarded only by an
`== SAMECOLOUR` special case, e.g.:

```cpp
return m_customtext [m_iNoteTextColour];     // GetNoteColourFore
m_iNoteColourBack = m_customback [m_iNoteTextColour];
```

`m_customtext` / `m_customback` hold `MAX_CUSTOM` (16) entries, so any value in
16..65534 reads hundreds of elements past the array on ordinary `Note` /
`NoteColourFore` / `NoteColourBack` calls — no dialog involved.

**The readers are fixed (2026-08-05).** All seven now go through one guarded
accessor, `CMUSHclientDoc::NoteCustomColourIndex`, which returns -1 for
anything that is not a usable index; out of range then behaves exactly as
`SAMECOLOUR` already did, which is what the two readers that *did* check were
already doing. Removing that guard segfaults the test suite — the overrun was
real, not theoretical.

**The row now validates too (option D, 2026-08-05).** A new `OPT_CUSTOM_INDEX`
flag makes the range check say what the field actually is — 0..`MAX_CUSTOM`-1,
or `SAMECOLOUR` — so `SetOption("note_text_colour", 300)` returns
`eOptionOutOfRange` instead of being stored. It follows the shape
`OPT_ZERO_UNLIMITED` already established for `max_output_lines`: a legal set
that a plain minimum/maximum cannot express. Only the range check consults it.

It deliberately changes NOTHING else — not the stored encoding, and not the
serialisation. The off-by-one between the two script APIs is also deliberately
left: `world.NoteColour = 4` stores 3 and `SetOption("note_text_colour", 4)`
stores 4, and fixing that breaks either scripts or world files. After this
change both are at least VALID values rather than one of them being a silent
read past the end of a 16-entry array. If it is ever worth fixing, it is a
world-file format change with a version bump, not an option-row edit.

**Why the row was not simply RE-FLAGGED, which is the tempting one-liner:** `OPT_RGB_COLOUR` does not merely set a maximum: it is also
what routes the value through `Save_XML_colour` → `ColourToName` on the way to
the world file, so `note_text_colour` is stored on disk as a **colour name**,
not a number. Drop the flag and:

* the writer switches to `Save_XML_number` and the reader to the number branch,
  so **every existing world file's stored colour string stops parsing** and the
  setting silently reverts to the default; and
* `OPT_CUSTOM_COLOUR` — the flag `echo_colour` uses, and the one that matches
  how this field is actually consumed — carries a ±1 encoding
  (`doc_shared.cpp`: subtract 1 on set, add 1 on get, 0 ⇄ SAMECOLOUR), so every
  value would additionally **shift by one**.

**It is a singleton, not a class (swept 2026-08-05).** The obvious worry after
finding this is that other option rows lie about their type the same way. They
do not:

* Every one of the twelve `OPT_RGB_COLOUR` rows has a genuine `RGB(r,g,b)`
  default — **except `note_text_colour`, whose default is a bare `4`.** That is
  the mechanical tell, and it is worth remembering: a colour-flagged row whose
  default is a small integer is declaring the wrong thing.
* `echo_colour` is the only `OPT_CUSTOM_COLOUR` row on the world, correctly
  flagged and correctly capped at `MAX_CUSTOM`.
* The trigger table's `custom_colour` row is capped at `MAX_CUSTOM + 1`, which
  looks like an off-by-one and is not: `OTHER_CUSTOM` is 16, the "use the RGB
  fields instead" sentinel, and `ProcessPreviousLine.cpp:1445` tests for it
  before any subscript. Stored 0..15 index the arrays; 16 never reaches them.
* Every other site that subscripts `m_customtext` / `m_customback` is either
  range-guarded already (`world_debug.cpp:966`, `:1831`) or provably in range
  (`doc_shared.cpp:735` reads `m_echo_colour`, whose row caps it correctly).

So the semantically correct row (`OPT_CUSTOM_COLOUR`, max `MAX_CUSTOM`) needs a
world-file migration, not a one-line edit. Note also that
`world.NoteColour` (`methods_noting.cpp:139-146`) already implements that exact
1-based convention by hand and validates it — so the two script APIs onto this
one field disagree by one, and only the property validates. That is the real
shape of the defect, and it is still **Wade's call**.

**The instance fixed in `36db6c1`.** The same bad value reaches
`CPrefsP9::FixSwatch` / `CPrefsP17::FixSwatch`, which special-case only
`iNewColour == 0` and otherwise index `m_customtext [iNewColour - 1]`. The
argument is a `CComboBox::GetCurSel()`, which returns **`CB_ERR` (-1)** when the
stored value is outside the combo's range (`SetCurSel` rejects it), so the else
arm reads index **-2**. The two `OnSelchange*` handlers next door already wrap
the identical call in `if (... != CB_ERR)`; the `OnSetActive` call sites did
not. Fixed by moving the guard inside `FixSwatch` so every caller is covered.

Scope, after an adversarial review of the original report:
* The `CPrefsP17` path is real — reachable by `SetOption("note_text_colour", N)`
  with N in 16..65534.
* The `CPrefsP9` path is **not** reachable: `echo_colour` carries
  `OPT_CUSTOM_COLOUR`, and `SetBaseOptionItem` decrements after the range check,
  so its maximum lands on the last valid combo index. The guard there is
  harmless belt-and-braces, not a fix.
* Index -2 lands *inside* the same `CPrefsP3` object (about 46 `CColourButton`
  members precede the array), so the consequence is two swatch buttons painted
  with arbitrary colours — not a fault, and not something ASan detects by
  default (it does not catch intra-object overflow).
* Windows runs the same unguarded code. This was latent on the port only
  because the dialog bridge never invoked `OnSetActive` until `d762469`.

## 9 — an xterm-256 foreground index subscripts the 8-entry print-style arrays

**Where:** `mushview.cpp:3952-3967`, inside `CMUSHView::PrintWorld`'s per-style
loop.

**Bug:** the world's print styles live in `int m_nNormalPrintStyle [8]` and
`int m_nBoldPrintStyle [8]` (`doc.h:830-831`) — one entry per ANSI colour.
`PrintWorld` subscripts them with `pStyle->iForeColour` whenever the style is
`COLOUR_ANSI`:

```
    if (style & HILITE)
      print_font (pcb, pDoc->m_nBoldPrintStyle [pStyle->iForeColour]);
    else
      print_font (pcb, pDoc->m_nNormalPrintStyle [pStyle->iForeColour]);
```

but for an xterm-256 style `iForeColour` holds 0..255, not 0..7 — the same
field, in the same union of cases, that `CMUSHView::OnDraw` reads as
`xterm_256_colours [iForeColour]` after testing `iForeColour >= 8`
(`mushview.cpp:1365`), and that the drawing code asserts is `< 256`
(`mushview.cpp:1361`). So printing a world whose output contains any 256-colour
escape reads up to 248 ints past the end of an 8-int member array, and the
resulting garbage is used as a `FONT_*` bitmask.

**Consequence:** wrong bold/italic/underline on printed lines at best; the read
stays inside the `CMUSHclientDoc` object (both arrays are members, with plenty
following them), so it is an intra-object overflow — no fault, and not something
ASan catches by default. Same class as bug 8.

**Status:** not fixed upstream-style here, because the fix belongs in a Windows
TU this port does not exercise. The port's own print path
(`platform/wx_world_child.cpp` `CWorldChildFrame::PrintWorld`) declines to
reproduce it: it treats an index outside 0..7 as `WHITE`, which is already what
the non-ANSI arm of the same `if` does. A Windows fix would be the same clamp on
those four lines.

## 10 — DBCS wrap splits the character when "indent paragraphs" is off

**Found 2026-08-04 while writing coverage for the wrap split-point; fixed
2026-08-05 after Wade confirmed it should be corrected on Windows too.**

`CMUSHclientDoc::AddToLine`'s auto-wrap looks for a split point at "a multibyte
character or a space, whichever comes last". The DBCS arm records the index of
the **lead** byte of the pair, precisely so the wrap lands before the character
rather than inside it.

The `!m_indent_paras` block that follows was written for the SPACE arm:

```
if (!m_indent_paras)
  {
  saved_count--;    // one less to copy
  last_space++;     // one more on this line (the space)
  m_pCurrentLine->len = last_space; // this line is longer
  }
```

Keeping the space on the old line is right — you do not want a continuation line
starting with a space. But `last_space` may have come from the DBCS arm, where it
is the index of a **lead byte**, and incrementing it leaves the lead byte at the
end of the old line and starts the continuation line on the orphaned trail byte.
The double-byte character is split exactly as the DBCS arm exists to prevent.

**Reproduction** (now `WrapTest.DbcsPairStaysWholeWithIndentParagraphsOff`,
which fails without the fix): a non-UTF-8 world,
`wrap_column` 20, `indent_paras` off, and a line of GB2312 pairs `D6 D0` behind
a 5-byte ASCII prefix so the pairs sit on odd offsets. The split point is found
at byte 17 (a lead byte); `last_space++` makes it 18, so line 0 ends `… D6` and
line 1 begins `D0 …`. It happens on pair-aligned Big5 input too.

**Severity is limited by the default:** `indent_paras` defaults to **true**
(`scriptingoptions_numeric.inc:85`), so the block does not run unless the user
turns "indent paragraphs" off. A CJK world with that option off would see torn
characters at every wrap.

**Fix:** record WHICH arm found the split point (`bSplitAtSpace`, set false by
the DBCS arm and true by both space arms) and gate the `!m_indent_paras`
adjustment on it. A DBCS split point needs no adjustment at all — it is already
on a character boundary, so the continuation starts on a lead byte either way,
and there is no space to absorb.

The adjustment itself is untouched for the case it was written for, which the
new `WrapTest.SpaceSplitKeepsTheSpaceOnTheOldLineWithIndentParagraphsOff` pins:
with indenting off, a space split still leaves the space at the end of the old
line so the continuation does not begin with one. Mutating the gate *both* ways
kills tests — restoring the unconditional form fails the DBCS test, and
removing the adjustment entirely fails four space-arm tests.

**This changes Windows rendering** for a non-UTF-8 CJK world with "indent
paragraphs" off: the wrap now falls one byte earlier and the character stays
whole. That is the point of the fix, and it is on the Windows validation list
(`docs/WINDOWS-VALIDATION.md` § 4c-duodetricies).

The two earlier tests (`WrapTest.DbcsPairStraddlingWrapColumnMovesWholeToNextLine`,
`WrapTest.TrailByteInTheGapBetweenDbcsRangesIsNotASplitPoint`) still set
`m_indent_paras = 1` explicitly, and now say what the other setting does rather
than avoiding it.

## 11 — `ClearOutput` leaves the recorded IAC GA line number stale

**Bug:** `CMUSHclientDoc::ClearOutput` deletes every line and restarts the
numbering (`m_total_lines = 0`, then the re-seeded line becomes number 1), but
leaves `m_last_line_with_IAC_GA` holding the number it had before. That number
now names a line which does not exist, and — because the counter went back to
zero — usually one that *cannot* exist: `world.GetInfo(289)` will happily
report line 4,312 of a buffer holding one line.

A script using it the obvious way (`GetLineInfo(GetInfo(289), 1)` to re-read
the last prompt) gets a silently wrong line rather than an error, because
`GetLinePosition` clamps out-of-range numbers to the last line.

**Fix:** zero it alongside `m_total_lines`, which is the only place the two can
disagree. The port's own `m_last_prompt_line` — the line the PROMPT flag was
stamped on, used by Clear to Start — is reset in the same place and for the
same reason.

Covered by `Utilities.ClearOutputForgetsTheRememberedPromptAndGaLine`.


## 12 — the queued-command "no log" flag can never suppress logging

`SendMsg` queues a speed-walk command as a one-byte flag plus the text
(`doc_shared.cpp:2360-2377`). The flag is `E`/`N`/`I`/`W` for
queue/immediate x echo/no-echo, **lowercased when the caller asked for no
logging**:

```
   // nolog is the lower-case version of the flag
  if (!bLogIt)
    strEchoFlag.MakeLower ();
```

Every drain then recovers the two booleans the same way:

```
  m_pDoc->DoSendMsg (strCommand.Mid (1),
                     toupper (cMessageType) == QUEUE_WITH_ECHO ||
                     toupper (cMessageType) == IMMEDIATE_WITH_ECHO,
                     cMessageType >= 'A');      // log flag
```

`toupper` correctly makes the ECHO decision case-insensitive. But the log flag
is `cMessageType >= 'A'`, and every one of the eight codes is a letter:
`'E'` is 69 and `'e'` is 101 — **both are >= 'A' (65)**. The test is a
tautology, so `bLogIt` is true for every queued command and the lower-casing
above accomplishes nothing for logging. A speed-walk sent with logging
suppressed is logged anyway.

The intended test is almost certainly `cMessageType < 'a'` (upper case = log).

**Fix (Wade's call, 2026-08-05: make it work):** `c < 'a'` — upper case logs,
lower case does not. Done in the shared drain, so all three former copies get
it. **This changes log-file contents on both platforms**: a speed-walk sent
with logging suppressed stops appearing, which is what the flag was always
meant to do. See `docs/WINDOWS-VALIDATION.md` § 4c-duodetricies-bis.

## 13 — Windows' two queue drains disagree about case

`CTimerWnd::OnTimer` (`TimerWnd.cpp:52`) — the per-tick drain — compares with
`toupper`, so a lower-case `e` counts as QUEUE_WITH_ECHO and the command is
echoed. `CTimerWnd::ChangeTimerRate` (`TimerWnd.cpp:101`) — the "the delay was
just set to zero, flush everything" path — compares **case-sensitively**:

```
  m_pDoc->DoSendMsg (strCommand.Mid (1),
                     cMessageType == QUEUE_WITH_ECHO ||
                     cMessageType == IMMEDIATE_WITH_ECHO,
                     cMessageType >= 'A');
```

so the same command echoes when drained on a tick and does not when flushed.
Given that the paired upper/lower codes exist only to carry the log flag,
`OnTimer`'s reading is the intended one and `ChangeTimerRate` is the defect.

**The port already diverges here, accidentally in the right direction.** Its
`delay <= 0` arm (`platform/wx_world_child.cpp:4131-4140`) applies `toupper`,
matching `OnTimer` rather than `ChangeTimerRate`. So a no-log speed-walk
flushed by setting the delay to zero echoes on macOS and does not on Windows.
**Fix (Wade's call, 2026-08-05: `toupper` everywhere):** the flush arm now
calls the same shared body as the tick drain, so the two cannot disagree again.
The port was already on this side, so only Windows changes — a no-log command
flushed by setting the delay to zero now echoes, as it always did when drained
on a tick.

---

## 14 — a menu-triggered alias's own `%1` is silently dropped

`CMUSHView::OnMXPMenu`'s `ACTION_ALIAS` arm — used for both an MXP `<a>` menu's
alias action and the Ctrl+click word-menu (`CMUSHView::AliasMenu`) — has its own
hand-rolled `%`-substitution loop (it does not call `FixSendText`, which is what
a normally-typed/matched alias or trigger uses). For a `%<digit>` sequence it
computes `subscript = *++p - '0'` and then, immediately after, resumes
scanning — `subscript` is never read again. So an alias like `greet *` → send
`echo greeting %1`, fired from either menu, always sent `echo greeting ` with
the wildcard silently dropped, regardless of what the menu was invoked on.

This is not a case of the value being unavailable: `AliasMenu` already scans
the word under the mouse into `m_strWordUnderMenu` for exactly this purpose —
`GetInfo(86)` documents it as "word under menu in output window" and a script
attached to the alias could always read it that way. The plain `%1`
substitution path into the alias's own Send text, which is the obvious first
thing to try before reaching for a script, was simply never wired up.

**Fix:** `%1` now substitutes `m_strWordUnderMenu`. `%0` and `%2`-`%9` are
unchanged (still empty) — there is only ever one captured word for a menu
click, and `GetInfo(86)`'s own phrasing ("word", singular) is the closest
thing to a spec this feature has. Fixed in `doc_aliasmenu_shared.cpp`
`ExecuteAliasMenuItem`, which is now the single body `OnMXPMenu` calls into on
Windows too (2026-08-13's hoist), so this fixes both platforms from one edit.

Regression guard: `tests/test_aliases.cpp`
`AliasMenu.ExecuteSubstitutesWordUnderMenuIntoPercentOne`.

---

## 15 — a NAWS dimension of exactly 255 goes out as a bare IAC

`CMUSHclientDoc::SendWindowSizes` announces the window size as

    IAC SB NAWS <cols-hi> <cols-lo> <rows-hi> <rows-lo> IAC SE

building those nine bytes as a plain array and handing it to `SendPacket`. The
four payload bytes are written raw. RFC 854 is explicit that inside the telnet
stream "if a 255 is to be sent as data, it must be doubled", and RFC 1073 —
which defines NAWS — repeats it for exactly these bytes. So a width or height
whose high or low byte is 255 emits an *unescaped* IAC in the middle of the
subnegotiation: the MUD's telnet parser reads it as the start of a command,
consumes the following byte as that command, and the negotiation goes out of
step. With a wrap column of 255 the low byte is 0xFF and the very next byte is
the rows high byte, so the server sees `IAC <rows-hi>` and then never finds a
matching `IAC SE` where it expects one.

255 is reachable without contrivance: the wrap column is user-settable and the
"auto wrap to window width" path derives it from the pane, and both 255 columns
and (on a tall enough window) 255 rows are ordinary values. It is only the
single value 255 per byte that trips it, which is why it has survived — 256
comes out as `01 00` and is fine.

**Fix:** both arms now append the four payload bytes through a tiny
`AppendNAWSByte` helper that doubles a 0xFF, and send the resulting length
rather than `sizeof p`. The buffer is sized for the worst case (all four bytes
255). The Windows arm is fixed with the same helper in the same file, so this
is one edit for both platforms; nothing changes for any dimension that does not
contain a 255 byte.

Regression guard: `tests/test_connection.cpp`
`Naws.EscapesADimensionByteOfExactly255`.

---

## PNG loader reads past the end of every row libpng allocated

`LoadPng` and `LoadPngMemory` (now `png_load_shared.cpp:193` and `:353`;
`Utilities.cpp` upstream) size their row copy with

```
  long bpl = BytesPerLine (width, bit_per_pixel);   // padded to 4 bytes
  ...
  memcpy (p, row_pointers [iHeight - row - 1], bpl);
```

but `png_read_png` allocates each `row_pointers[i]` at exactly
`png_get_rowbytes()`, which is `width * channels` with **no padding**. For any
24bpp image whose width is not a multiple of 4, the copy therefore reads 1–3
bytes past the end of every row allocation — once per row, on every PNG load.

Measured at HEAD, printing both sizes from inside the loop:

| width | `png_get_rowbytes()` | `bpl` | over-read |
|---|---|---|---|
| 5 | 15 | 16 | 1 |
| 6 | 18 | 20 | 2 |
| 7 | 21 | 24 | 3 |
| 9 | 27 | 28 | 1 |

Confirmed under AddressSanitizer, reached straight from a script:

```
ERROR: AddressSanitizer: heap-buffer-overflow ... READ of size 28
    #1 LoadPng(char const*, void*&) png_load_shared.cpp:182
    #2 CMiniWindow::LoadPngImage(...) miniwindow.cpp:1205
    #5 L_WindowLoadImage(lua_State*) lua_methods.cpp:6480
```

32bpp is unaffected — `width * 4` is always 4-aligned, so `bpl` and `rowbytes`
agree. That is also why this went unnoticed: the alpha images are safe, and the
padding bytes are never read as pixel data, so nothing renders wrongly. It is a
latent crash, not a visible defect — which is precisely the kind that survives
for years.

**Fixed 2026-08-07** by copying `min(png_get_rowbytes(), bpl)` at both sites and
leaving the pad bytes as the zero the DIB came back with. **Not one visible
pixel changes on either platform**, so this needs no `UPSTREAM-DIVERGENCES.md`
entry — but it IS a change to shared code that ships on Windows, and belongs in
the Windows validation pass.

Regression guard: `tests/test_lua_surface.cpp`'s
`LoadImageMemoryDecodesAPngWithAPaddedStride` covers the second site, which had
no positive test at all before. **Both sites were reverted independently and
each was caught by ASAN**, so neither is guarded only by the other. Note that
the fixture must be **24bpp** — a first draft used the existing RGBA helper,
whose stride is 4-aligned at every width, and so could never have failed.

---

## `AltV` is published as a command but has no implementation, on either platform

`accelerators.cpp` is the table that gives MUSHclient commands their public
NAMES — the strings `world.DoCommand()` accepts and the Macros/Keypad UI binds
keys to. It publishes **eighteen** `Alt<letter>` names:

    AltA AltB AltJ AltK AltL AltM AltN AltO AltP AltQ AltR AltS AltT AltU
    AltV AltX AltY AltZ

but only **seventeen** macro slots exist. `OtherTypes.h`'s `MAC_ALT_*` enum has
A, B, J–U, X, Y, Z, and `sendvw.cpp`'s id→slot switch has exactly 17 `case`
labels. `ID_ALT_V` appears in precisely two places in the whole tree — the
accelerators table and its `#define` — with no `ON_COMMAND_EX` in `mushview.cpp`
or `sendvw.cpp` and no `case` label. So binding a key to `AltV`, or calling
`world.DoCommand("AltV")`, does nothing **on Windows too**. `DoCommand` cannot
report this: it posts `WM_COMMAND` and returns `eOK` regardless.

The excluded letters are the menu access keys — C, D, E, F, G, H, I, V, W —
so dropping V (View) from the macro slots was deliberate. What looks accidental
is that its *table entry* survived: `ID_ALT_W` (Window) is not published at all,
which is what V should have been. A leftover, not a design.

**Deliberately not fixed.** Adding a handler would invent a macro slot Windows
does not have, and removing the table entry would change what
`world.DoCommand("AltV")` returns (`eOK` → `eNoSuchCommand`) — a visible
behaviour change to shared code for a command nobody can be using, since it has
never done anything. Recorded so it is not re-triaged as a port defect.

**How it was found, and the correction that came with it.** A sweep for
"published command ids with no handler reference" flagged four candidates.
Three — `ActivityViewCloseWorld`, `ActivityViewSaveWorld`,
`ActivityViewSaveWorldAs` — were **false positives**: they have full
`ON_COMMAND` + `ON_UPDATE_COMMAND_UI` handlers in `ActivityView.cpp` and entries
in two recovered menus, and the sweep simply had not read that file. That sweep
was later deleted for failing its own mutation check; this entry is the one
real thing it found.

## 16 — MXP hyperlink opened from the popup menu refuses `https://` (this commit)
**Bug:** `OnMXPMenu`'s ACTION_HYPERLINK arm guards against arbitrary OS
commands with a scheme allowlist, but its copy lists only `http://` and
`mailto:`.  The other two copies of the same guard — the left-click arm a few
thousand lines earlier in the same file, and `CMUSHclientDoc::OpenBrowser`
(methods_utilities.cpp) — both list `https://` as well.  So an MXP hyperlink
to an https URL is rejected as "permission denied" when opened from the
right-click popup, while left-clicking the identical link opens it.
**Fix:** add the `https://` arm, matching the other two copies.  The three
copies should really be one helper; that hoist is deliberately deferred so it
does not collide with other in-flight work on utilities_shared.cpp.

## 17 — Double-click word scan reads one byte past the line buffer (this commit)
**Bug:** once a line is complete its `text` buffer is reallocated to exactly
`MAX (len, 1)` bytes, with no terminator (doc.cpp — `ProcessPreviousLine`;
doc_shared.cpp in this tree).  `calculate_line_and_column` returns
`col == pLine->len` when the click lands to the right of the text, and
`OnLButtonDblClk` then starts its backward word scan at `text [col]` — one
past the allocation.  The over-read decides the result: a byte that happens to
look like a space or a word delimiter yields no selection at all, anything
else selects the last word, so double-clicking to the right of a line behaves
differently run to run.  `OnDisplayTextattributes` has the same shape: it
clamps `startcol` to `pStartLine->len` and then reads `text [startcol]`, which
is one past the end whenever the selection spans lines (the same-line case is
bounded by its own `endcol > startcol` test).
**Fix:** clamp the column into `[0, len - 1]` before the scan, with an
early-out for an empty line — the same clamp `SetWordUnderMenu` already
carries in this tree, so the two copies of the scan agree again.  The
text-attributes clamp is applied *after* the valid-selection test, leaving the
set of accepted selections unchanged.

## 18 — `CmcDateTime::operator-`'s Windows arm discards a null/invalid span status (this commit)
**Bug:** on the `_WIN32` arm, the null/invalid early-outs build a real
`COleDateTimeSpan`, set ITS status to `null`/`invalid`, then `return (double)
spanResult;`. The function's return type is `CmcDateTimeSpan`, so that double
goes straight into `CmcDateTimeSpan(double)` — a constructor that unconditionally
calls `SetStatus(valid)` — so the caller always observes `valid` regardless of
which of the two null/invalid arms actually ran. The POSIX arm two lines below
does not have this bug: it already constructs and returns a `CmcDateTimeSpan`
directly.
**Fix:** construct a `CmcDateTimeSpan` and set its status directly in both
early-out arms, mirroring the POSIX arm, instead of routing the result through
`(double)`. `needs: windows-run` — unreachable from this platform; the POSIX arm
was already correct and is what `tests/test_mcdatetime.cpp`'s
`CmcDateTimeR4.NullAndInvalidStatusPropagation` exercises.

## 19 — Doc ctor's unqualified `GetUniqueID()` picks the automation member and leaks its BSTR (`3c58886`)
**Bug:** inside `CMUSHclientDoc`, the unqualified `GetUniqueID ()` call in the
constructor resolves to the automation member (the `IDispatch`-facing one)
rather than the free `::GetUniqueID ()` CString helper the three sibling call
sites (mainfrm.cpp, scriptingoptions.cpp, plugins.cpp) already use. The
automation member returns an `AllocSysString`'d `BSTR` whose ownership belongs
to the caller; `m_strWorldID = GetUniqueID ();` copies the text into the
`CString` and drops the `BSTR` on the floor, once per document, on both
platforms, since the original 2010 import.
**Fix:** qualify the call as `::GetUniqueID ()`, matching the three sibling
callers. Same string, no behavioural change; verified with `leaks(1)` — the
`AllocSysString` leak population in the test binary drops to zero.

## 20 — A MouseOver callback that deletes its own hotspot crashes the client (this commit)
**Bug:** after dispatching a hotspot's MouseOver, `Mouse_Move_MiniWindow`
re-looks-up the hotspot by id "in case previous callback deleted it" and sets
`pHotspot = NULL` when it is gone — then, five lines later, dereferences it
anyway:

```
    if (!pHotspot->m_sCancelMouseOver.empty ())
      hooks.TrackMouseLeave ();
```

so the exact scenario the re-lookup was added to survive takes the client down
instead.  Deleting a hotspot from its own MouseOver is ordinary plugin practice
— a button bar that redraws itself calls `WindowDeleteAllHotspots` and installs
a new set — and every other one of the five re-establish sites in the file
re-tests `pHotspot` before using it, so this one is simply the copy that was
missed.
**Fix:** `if (pHotspot && !pHotspot->m_sCancelMouseOver.empty ())`.  With the
hotspot gone there is no cancel-mouse-over to deliver, so not arming the
mouse-leave notification is also the right answer.  `needs: windows-run` only
to confirm the crash reproduces there; the logic is platform-independent and
`LuaSurface.MouseOverCallbackMayDeleteItsOwnHotspot`
(`tests/test_lua_surface.cpp`) exercises it headless — before the fix that test
SIGSEGVs, and its control (a MouseOver that leaves its hotspot alone) still
arms the notification, so "never call `TrackMouseLeave`" fails too.

## 21 — The second layout pass wipes the first pass's "would not fit" decision (this commit)
**Bug:** `Calculate_MiniWindow_Rectangles` runs twice per paint — once for the
draw-underneath windows, once for the rest — and clears `m_bTemporarilyHide`
for **every** miniwindow at the top of its loop, before the pass filter that
`continue`s past the windows this pass is not laying out.  The two passes run
back to back, so the non-underneath pass resets the flag on the underneath
windows the previous pass had just decided would not fit.  `WindowInfo (name,
6)` therefore always answers false for a draw-underneath window that had no
room, and any frontend that consults the flag between the two passes sees it
flap.
**Fix:** guard the reset with the same pass test the filter below uses —
`if (((mw->GetFlags () & MINIWINDOW_DRAW_UNDERNEATH) != 0) == bUnderneath)` —
so only the pass that actually lays a window out clears its flag.  Guarding
rather than moving the line below the filters keeps the reset happening for
windows that are not currently shown, which is what upstream's placement
achieved.  `needs: windows-run`.  Covered headless by
`LuaSurface.TemporarilyHideSurvivesTheOtherLayoutPass`
(`tests/test_lua_surface.cpp`), which also runs both passes against a wall wide
enough to fit the window and asserts the flag comes back false — so deleting
the reset outright fails as well.
## 22 — Trigger "multi-line" is not compared, so un-ticking it is discarded (this commit)
**Bug:** `CPrefsP8::CheckIfChanged` decides whether editing a trigger dirties
the world, and its comparison chain covers every field of `CTrigger` — including
`iLinesToMatch`, `bMultiLine`'s own sibling — except `bMultiLine` itself.  A user
who un-ticks "multi-line" and changes nothing else is told nothing changed, the
document is never marked modified, and the edit is thrown away at the next save
decision.
**Fix:** add a `bMultiLine` term next to the `iLinesToMatch` one, normalising
both sides to `bool` (the member is an `unsigned short`, the dialog field a
`BOOL`).

## 23 — Command-window colours are lost by visiting another prefs page (this commit)
**Bug:** `CPrefsP9::OnSetActive` re-seeds `m_ctlTextSwatch` and
`m_ctlBackgroundSwatch` from `m_input_text_colour` / `m_input_background_colour`,
but the only place those two members are ever written from the swatches is
`OnOK`.  So a colour picked on the Commands page and then left — by clicking any
other page in the sheet — is reverted to the value the sheet opened with the
moment the user comes back.  `CPrefsP3` and `CPrefsP5` do not have this defect:
both save their swatches in `OnKillActive`.
**Fix:** give `CPrefsP9` the same `OnKillActive`, copying the two swatches into
the two members.  `OnOK`'s identical copy becomes idempotent.

## 24 — The default echo colour always reports a change (this commit)
**Bug:** `LoadPrefsP9` encodes the world's `m_echo_colour` for the page as
`m_echo_colour == SAMECOLOUR ? 0 : m_echo_colour + 1`, but `ChangedPrefsP9`
undoes it arithmetically: `m_echo_colour != (page9.m_echo_colour - 1)`.  For the
default "same as normal text" that compares 65535 (the `unsigned short`
`SAMECOLOUR`) against `0 - 1` — an `int` -1 that no `unsigned short` can equal.
Opening World Properties and pressing OK therefore marks the document modified
every time, prompting to save a world nothing was changed in.
**Fix:** compare in the encoding `Load` wrote —
`(m_echo_colour == SAMECOLOUR ? 0 : m_echo_colour + 1) != page9.m_echo_colour`.
`SavePrefsP9` is deliberately left alone: its `-1`-wraps-to-65535 assignment is
what existing world files depend on.

## 25 — The default note colour always reports a change (this commit)
**Bug:** `ChangedPrefsP17` has the identical `SAMECOLOUR`-vs-`-1` defect on
`m_iNoteTextColour`, with the identical consequence.
**Fix:** mirror `LoadPrefsP17`'s transform, leaving `SavePrefsP17` untouched.

## 26 — "Fit the last column to the list view" computes a width nothing applies (this commit)
**Bug:** the alias, timer and variable pages each end `OnInitDialog` with a
block that measures the list control and stores a fitted width into
`m_iColWidth [eColumnContents]`.  By then `CGenPropertyPage::OnInitDialog` has
already restored the saved widths into that array AND inserted the columns from
it, and nothing reads `m_iColWidth` again — so the block has no effect at all and
the Contents column keeps the 150-pixel seed from configuration.cpp.
**Fix:** apply the figure with `SetColumnWidth`.  Only when the user has no
saved width of their own, though: the restore happens over the seed, so fitting
unconditionally would discard their width on every open.  The seed is captured
before the base call and the two compared afterwards; the decision itself is
`FittedLastColumnWidth` in prefspropertypages.h, which the headless suite drives.

## 27 — The Timers list's Contents column header does not sort (this commit)
**Bug:** `CPrefsP16::CompareObjects` has cases for Type, When, Group, Label and
Next, but none for `eColumnContents`, so clicking that header falls to
`default: iResult = 0` and the list does not move.  The alias and trigger pages
both compare their Contents column.
**Fix:** add `case eColumnContents: iResult = timer1->strContents.CompareNoCase
(timer2->strContents); break;`, following the sibling pages.

## 28 — The alias page's column arrays are indexed with the trigger page's enum (this commit)
**Bug:** `LoadPrefsP7` fills `page7`'s column arrays, but two of the subscripts
name `CPrefsP8::eColumnSequence` — the *trigger* page's enumerator — rather than
`CPrefsP7`'s own.  The two enums are independent declarations; this is correct
today only because both happen to give that name the value 1, and reordering
either one silently writes the alias page's justification and heading into the
wrong column.
**Fix:** use `CPrefsP7::eColumnSequence` at both sites.  No behaviour changes
today; the point is that it can no longer break from an unrelated edit.

## 29 — The printer-font picker opens on the font from before the sheet opened (this commit)
**Bug:** `CGlobalPrefsP4::OnFont` seeds `lf.lfFaceName` from
`App.m_strPrinterFont`, but the page's own choice lives in `m_strPrinterFont`
(DDX_Text on `IDC_FONT_NAME`), and `OnFont` writes the chosen face straight to
the control with `SetWindowText`.  So a second visit to the picker within one
Global Preferences session opens showing the pre-dialog font rather than the one
just chosen, and confirming it without re-selecting the face silently reverts the
earlier pick — most easily hit when reopening it only to change the size, since
the size half already uses the page's own `m_nPrinterFontSize`.
`CGlobalPrefsP10::OnFont` already does it the right way round.
**Fix:** start with `UpdateData (TRUE)` and seed from `m_strPrinterFont`,
matching `CGlobalPrefsP10::OnFont`.  `needs: windows-run` — the port's font
picker is the wx one behind `g_wxhook_font_dialog`, so only a Windows build
confirms the `CFontDialog` seeding end to end.

## 30 — `metaphone()` copies a word into a 61-byte stack buffer with no bound (this commit)
**Bug:** `metaphone` clamps its *output* length to `sizeof (wname) - 1` and then
fills `wname[61]` from the input with `for (ii=0; name[ii] != '\0'; ii++) { if
(isalpha(...)) ename[jj++] = ...; }` — no check on `jj`.  Any word with more
than 60 letters writes past the end of the buffer and smashes the stack frame.
It is latent rather than live: the function has **no caller anywhere in the
tree** (the only near-call, methods_spellchecker.cpp:328, is commented out, and
`utils.metaphone` goes through `MString::DoubleMetaphone` instead), so the first
caller added would be the one to find it.
**Fix:** `break` out of the fill loop once `jj` reaches `sizeof (wname) - 1`,
which is the same clamp the `metalen` line above it already applies.  Kept
rather than deleted so the reference implementation stays available.
## 31 — Greyscale PNGs decode garbled, and palette PNGs do too from memory (this commit)
**Bug:** both decoders ask libpng for `STRIP_16 | PACKING | BGR`, and only
`LoadPng` adds `EXPAND`.  Neither asks for `GRAY_TO_RGB`, which is a *separate*
transform: `EXPAND` promotes sub-8-bit samples and expands a palette, but leaves
an 8-bit greyscale image at one byte per pixel and a grey+alpha one at two.  The
row bytes are then `memcpy`'d straight into a 24/32bpp DIB as if they were BGR,
so every greyscale PNG renders as coloured noise a third of its width — and
`world.WindowLoadImage`/`WindowLoadImageMemory` still return `eOK`.  The missing
`EXPAND` in `LoadPngMemory` does the same to palette-indexed images, so the same
file loads correctly from disk and garbled from memory.
**Fix:** add `PNG_TRANSFORM_GRAY_TO_RGB` to both, and `PNG_TRANSFORM_EXPAND` to
`LoadPngMemory`.  Both are no-ops for truecolour input, so existing RGB/RGBA
decodes are byte-identical; `png_read_png` calls `png_read_update_info`, so the
`png_get_color_type` test that picks 24 vs 32bpp already sees the transformed
image.

## 32 — `world.BlendPixel` mode 64 (HSL) ignores the blend colour entirely (this commit)
**Bug:** case 64 declares `CColor cA, cB`, populates only `cA`, and then reads
`cB.GetRed/GetGreen/GetBlue` — a default-constructed colour, i.e. black.  Each
channel is therefore `Simple_Opacity (base, 0, Opacity)`: the base scaled by
`(1 - Opacity)`, which is pure black at opacity 1.0 and merely darkened below
it.  The blend colour never participates.
**Fix:** mirror what `CMiniWindow::BlendImage` case 64 does over a whole
surface — hue/360*255 to red, saturation*255 to green, luminance*255 to blue,
each `Simple_Opacity`-blended against the base — and drop the unused `cB`.

## 33 — `world.WindowWrite` writes a .bmp strict readers refuse to open (this commit)
**Bug:** `CMiniWindow::Write` fills a `BITMAPINFO` with
`biSize = sizeof bmi` — 44 bytes, because `BITMAPINFO` carries a trailing
`bmiColors [1]` — and then writes out only `bmi.bmiHeader`, which is 40.  The
`biSize` field is how a reader knows which header dialect follows, so the file
claims a 44-byte header it does not have.  Windows' own GDI is lenient enough to
open it; macOS ImageIO (Preview, Quick Look, `sips`) is not, and `WindowWrite`
returns `eOK` either way.
**Fix:** `bmi.bmiHeader.biSize = sizeof bmi.bmiHeader;`.  The same `bmi` goes on
to `WritePng`, which reads only `biWidth`/`biHeight`, so the PNG path is
unchanged.

## 34 — `Blend_ColorBurn`'s underflow clamp is dead code (this commit)
**Bug:** the macro tests `(255 - (((unsigned long)(255 - B)) << 8) / A) < 0`.
The division yields an `unsigned long`, so the subtraction is done unsigned and
the result can never be negative: the clamp never fires, and the `(uint8)` cast
truncates the wrapped value instead.  For the 32640 of 65536 (A, B) pairs where
the burn *should* clamp to 0, the macro returns an arbitrary byte — `A=1, B=0`
gives 255 (the opposite of the intended answer) and `A=100, B=0` gives 115.  It
corrupts modes 7 (ColorBurn), 9 (InverseColorBurn) and 20 (VividLight) of both
`world.WindowBlendImage` and `world.BlendPixel`.
**Fix:** cast the division result to `long` so the subtraction is signed, in the
test and in the value — the shape `Blend_Freeze` already uses two lines below.

## 35 — Three re-entrancy guards are shared by every open world (this commit)
**Bug:** the guard that stops a MouseOver callback recursing back into
`WindowAddHotspot` for ever is a function-local `static bool`, so it is one flag
for the whole process rather than one per document.  While world A is inside
`WindowAddHotspot`, a MouseOver script that adds a hotspot to an unrelated,
simultaneously-open world B is refused with `eItemInUse` — a world's hotspots
depending on what another world happens to be doing.
`CMUSHclientDoc::Screendraw` (`doc.cpp`) and `CMUSHclientDoc::PluginListChanged`
(`doc_shared.cpp`) are the same shape, and neither was covered by the original
fix.  Screendraw is the one that bites: it runs for every completed line, note
and command, so an `OnPluginScreendraw` handler that writes to another world
— `world.GetWorld ("Other"):Note (…)`, which reaches that document's
`Screendraw` synchronously through `DisplayMsg` → `ProcessPreviousLine` —
silently skips world B's own `OnPluginScreendraw` for that line, so a plugin
that captures all output loses it.  `PluginListChanged` costs the same way when
an `OnPluginListChanged` handler loads or reloads a plugin in another world.
The two sibling guards ten lines below Screendraw
(`m_bInPlaySoundFilePlugin`, `m_bInCancelSoundFilePlugin`) are already
per-document members doing exactly this job, so the statics are an oversight
rather than a design.
**Fix:** make all three `CMUSHclientDoc` members — `m_bInWindowAddHotspot`,
`m_bInScreendraw`, `m_bInPluginListChanged` — cleared in the constructor.
Per-document scope still terminates any A→B→A recursion, because the
document's own flag is set on the way in.  Covered by
`PluginGuards.ScreendrawGuardIsPerDocument` and
`PluginGuards.PluginListChangedGuardIsPerDocument` in `tests/test_plugins.cpp`,
each paired with a same-document control so deleting the guard fails too.

## 36 — `world.ArrayImport`'s table form crashes on any non-string value (this commit)
**Bug:** the table extension checks the *key* (`lua_type (L, -2) != LUA_TSTRING`
→ "table must have string keys") but not the value: it builds
`string sValue = lua_tostring (L, -1)` unconditionally.  `lua_tostring` returns
NULL for a boolean, table, function, thread, userdatum or nil, and constructing
a `std::string` from NULL is undefined — in practice MUSHclient dies on the
spot.  `world.ArrayImport ('t', {a = true})` — the natural way to import a set
of flags — takes the whole client down, taking any unsaved world with it.
**Fix:** check the value alongside the key with
`if (!lua_isstring (L, -1)) luaL_error (L, "table must have string values");`.
`lua_isstring` still accepts a number, which `lua_tostring` coerces in place;
that is safe here because it is the value slot, not the key `lua_next` is
iterating on.

## 37 — `world.CallPlugin` keeps pointers into two Lua strings it has removed from the stack (this commit)
**Bug:** `sPluginID` and `sRoutine` are raw `const char *` into Lua-owned
strings, and the next two statements are `lua_remove (L, 1)` twice — a
deliberate step, so a plugin calling back into us finds a clean stack.  Both
strings are then unreferenced for the entire call while eleven later sites read
them, including every error message.  A `collectgarbage` anywhere inside the
called routine frees them; the error text then reports whatever now occupies
that memory.  A plugin calling a routine in itself is the sharpest case, since
caller and callee share one Lua state.
**Fix:** copy both into `CString`s before the removes and point the two locals
at those copies, so every later read is of memory this frame owns.

## 38 — `world.SetRemoveMapReverses` raises an error when passed a boolean (this commit)
**Bug:** it is the only flag setter in the Lua world table that uses
`my_checknumber` rather than `optboolean`; its ten siblings (`SetMapping`,
`SetTrace`, `LogInput`, `Pause`, …) all take `optboolean (L, 1, 1)`.
`world.SetRemoveMapReverses (true)` — the form the documentation shows, and the
form every other flag accepts — therefore raises "number expected, got boolean"
and aborts the script.
**Fix:** `optboolean (L, 1, 1) != 0`.  `optboolean` falls through to
`my_checknumber`, so the numeric form is unchanged, and a missing argument now
defaults to on exactly as it does for the siblings.

## 39 — Three world-table entries reject the documented `other:Method (…)` form (this commit)
**Bug:** every entry in the world table is supposed to start with `doc (L)`,
which is what makes the file's own documented alternative form —
`other:AddMapperComment ("…")` against a world userdatum from
`world.GetWorld`  — work: `doc()` removes the userdatum from index 1 so the
arguments line up.  `L_StripANSI`, `L_GetWorld` and `L_GetWorldById` never call
it, so all three see the userdatum where they check for a string and raise
"string expected, got userdata".  `StripANSI` is the one a script would
naturally write that way.
**Fix:** `doc (L);` as the first statement of each.  The plain
`world.StripANSI (s)` form is unaffected — `doc()` only removes argument 1 when
it really is a `mushclient.world` userdatum.

## 40 — `pushVariant` hands an uninitialised `VARIANT` to `SafeArrayGetElement` (this commit)
**Bug:** the `VT_ARRAY + VT_VARIANT` arm declares `VARIANT val;` and passes it
straight to `SafeArrayGetElement`, which on Windows calls `VariantClear` on the
destination before filling it — i.e. it frees whatever the uninitialised `vt`
and pointer happen to name.  It is the same defect the file already fixes for
`L_GetTrigger`/`L_GetAlias`/`L_GetTimer` 2,480 lines below, which do call
`VariantInit`.  Every array-returning world method (`ArrayListAll`,
`GetTriggerList`, `GetVariableList`, …) goes through this arm.
**Fix:** `VariantInit (&val);` after the declaration.  No behaviour change on
the port — its `SafeArrayGetElement` shim copies without clearing — so this one
needs a **Windows debug run** (poisoned stack) to see the original fault.

## 41 — The note colours a deferred note carries are never initialised (this commit)
**Bug:** `m_iNoteColourFore` / `m_iNoteColourBack` get no value from the
document constructor and none from the options table, yet `Tell` reads them on
its deferred arm — the one taken while `m_bNotesNotWantedNow` is set, i.e.
inside a telnet-negotiation or plugin callback — and pushes them into
`m_OutstandingLines`.  The custom-palette lookup just below only overwrites
them when `NoteCustomColourIndex ()` returns an index, so a world whose
`note_text_colour` is the `SAMECOLOUR` sentinel (`world.NoteColour = 0`)
carries whatever the heap held into `OutputOutstandingLines`, and from there
into a real `AddStyle`.  On the port the read is unconditional, because the
headless constructor sets `m_bNotesInRGB` true.
**Fix:** initialise both beside `m_bNotesInRGB` / `m_iNoteStyle` with
literals — `RGB (255, 255, 255)` on `RGB (0, 0, 0)`, the pair
`SetNoteColourRGB` already falls back to.  Literals rather than
`m_normalcolour [WHITE]` / `[BLACK]` because `SetDefaultAnsiColours` does not
run until ~50 lines later, so the palette is not valid at that point.
## 42 — `GetRecentLines` narrows its start position to `int` before the clamp (this commit)
**Bug:** the first line to return is `m_sRecentLines.size () - Count`, an
unsigned `size_t` minus a signed `long`, stored into an `int`.  When `Count`
exceeds the number of lines held the difference wraps, and the narrowing keeps
only its low 32 bits — with three lines held and `Count` = 3,000,000,000 that
is +1,294,967,299, which is positive, so the `if (iPos < 0) iPos = 0;` clamp
below never fires.  The loop then subscripts the deque a billion entries past
its end, with `iPos != m_sRecentLines.size ()` — an equality that the runaway
index steps straight over — as its only stop condition.  Any plugin reaches it
with one bad argument to `world.GetRecentLines`; headless it is a segfault.
**Fix:** do the comparison at deque width and only subtract when it cannot
wrap (`if (Count > 0 && (size_t) Count < m_sRecentLines.size ())`), walk with a
`size_t`, and stop on `iPos < m_sRecentLines.size ()`.  A `Count` larger than
the buffer now returns the whole buffer, which is what a smaller overshoot
always did.  Covered by `LuaSurface.GetRecentLinesClampsACountBiggerThanTheBuffer`.

## 43 — An empty section in the names file is loaded as a valid names list (this commit)
**Bug:** `ReadNames` checks that each of `[start]`, `[middle]`, `[end]` and
`[stop]` is present, but not that any lines were collected between them, so a
file whose section markers are adjacent sets `bNamesRead = true` with one or
more of `FirstNames` / `MiddleNames` / `LastNames` empty.  `GenerateName` then
evaluates `rand () % FirstNames.GetSize ()` — a division by zero — and
subscripts the empty array.  It reaches the user through Edit ▸ Generate Name
and through `world.GenerateName`, and `world.ReadNamesFile` reports `eOK` for
the file that causes it.
**Fix:** after each collecting loop, refuse the file the same way the missing
markers already do — `if (FirstNames.GetSize () == 0) ThrowErrorException
("No names in [start] section of names file");` — so `bNamesRead` is never set
with an empty list and `world.ReadNamesFile` answers `eCouldNotOpenFile`.
NameGeneration.cpp is a `mushgui_adapters` TU (the headless build takes the
`platform/windows_stubs.cpp` `ReadNames`, which always throws), so there is no
headless coverage for this one and it wants a **Windows or GUI run**.
## 44 — `DebugHelper`'s `callbacklist` branch dereferences a NULL plugin (this commit)
**Bug:** `DebugHelper` starts by setting `m_CurrentPlugin = NULL` and only
fills it in when the argument carries a `_<plugin id>_` prefix.  The
`callbacklist` branch then walks `m_CurrentPlugin->m_PluginCallbacks` with no
null guard — unlike the `editplugin` branch beside it, which is written
`if (m_CurrentPlugin)`.  Reaching it with no plugin in context is an immediate
segfault.  Every link the client itself emits carries a validated prefix, so
the route is a script-authored `world.Hyperlink` whose action is
`!!138a692642ab4f9e7a1af63b:callbacklist()`; it is **not** reachable from MUD
output, MXP, or any UI-generated link, which is why it has survived.  No
platform ifdef is involved, so Windows crashes identically.
**Fix:** wrap the loop in `if (m_CurrentPlugin)`, with an
`else Note (TFormat ("No plugin selected."));`, and let control fall through to
the existing restore at the end of the function.  Needs a **Windows run** to
confirm the same link crashes there and no longer does.

## 45 — Five `DebugHelper` early returns leave the document stuck in a plugin's context (this commit)
**Bug:** `DebugHelper` saves the caller's plugin in `pSavedPlugin`, switches
`m_CurrentPlugin` to whichever plugin the clicked link named, and puts it back
in a single assignment at the very end.  Five branches return before reaching
it — `showtrigger`, `showalias` and `showtimer` when the named item does not
exist, and `editplugin`/`editscript` when the built-in notepad is used.  After
any of those the document is left in the clicked plugin's context, so every
later trigger/alias/timer/variable lookup (`GetTriggerMap()` and friends
already branch on `m_CurrentPlugin`) reads that plugin's tables instead of the
world's.  Clicking a stale item on the Debug summary page is enough; the sixth
early return, the one for an unknown plugin ID, already restores, which is what
makes the omission look like an oversight rather than a design.
**Fix:** repeat that branch's existing idiom —
`m_CurrentPlugin = pSavedPlugin;` immediately before each of the five
`return;`s.

## 46 — `GetTrigger` reports `omit_from_log` back as `eOmitFromOutput` (this commit)
**Bug:** `CMUSHclientDoc::GetTrigger` builds its returned `Flags` by testing
each trigger member and OR-ing in the matching bit, and every other line does
— `bOmitFromOutput` sets `eOmitFromOutput`, `bEnabled` sets `eEnabled` — except
the `omit_from_log` line, which also sets `eOmitFromOutput`. A script that
reads a trigger back with `world.GetTrigger` therefore cannot tell
`omit_from_log` from `omit_from_output`, and `AddTriggerEx (name, …, Flags,
…)` — which does decode `eOmitFromLog` correctly — turns a save/restore
round-trip of a `omit_from_log` trigger into an `omit_from_output` one.
**Fix:** OR in `eOmitFromLog` instead, matching `AddTrigger`'s own decode of
the same bit (`trigger_item->omit_from_log = (Flags & eOmitFromLog) != 0;`
a few dozen lines above). Covered by
`Triggers.GetTriggerReportsOmitFromLogAndOmitFromOutputAsDistinctBits`.

## 47 — `world.GetUdpPort` never considers `Last` itself (this commit)
**Bug:** `CMUSHclientDoc::GetUdpPort (First, Last)` validates that
`First <= Last`, explicitly allowing `First == Last` as a single-port range,
but then searches with `for (result = First; result < Last; result++)` —
a strict `<` that excludes `Last` from the scan. Any range where `Last` is the
only free port, including every single-port range, reports failure (`0`) even
though a free port exists.
**Fix:** change the loop condition to `result <= Last`, so the scan actually
covers the range the validity check already promised. Covered by
`LuaSurface.GetUdpPortConsidersTheLastPortInTheRange`.

## 48 — `InfoTypes[]` reuses codes 31/32 instead of 33/34 (this commit)
**Bug:** The descriptive table `InfoTypes[]`, consulted by
`world.GetInfoTypesList` and similar enumerations, lists "Script function for
world get focus" and "Script function for world lose focus" under codes 31
and 32 — but `GetInfo`'s own `switch` answers those two strings under codes 33
and 34 (`m_strWorldGetFocus`, `m_strWorldLoseFocus`); 31 and 32 are already
"Script function for world connect"/"…disconnect" a few rows above. The
duplicate codes clobber the connect/disconnect entries in anything that
builds a lookup keyed by `iInfoType`, and codes 33/34 are missing from the
table entirely.
**Fix:** renumber the two rows to 33 and 34, matching `GetInfo`'s switch.
Covered by `Info.InfoTypesTableHasNoDuplicateCodesAndCovers33And34`.
## 49 — `CPluginsDlg::OnUpdateNeedDescription` reads a stale `CPlugin*` (this commit)
**Bug:** every other selected-item handler in the Plugins dialog —
`OnShowDescription`, `OnEdit`, `OnEnable`, `OnDisable`, `OnRdblclkPluginsList` —
looks the row's `CPlugin*` item data up with
`find (m_PluginList.begin (), m_PluginList.end (), p)` before touching it, and
skips the row (`continue`) when it is not found.  `OnUpdateNeedDescription` —
the `ON_UPDATE_COMMAND_UI` handler behind the "Show Info" button — is the one
exception: it dereferences `p->m_strDescription` straight off the item data.
A plugin removed from `m_PluginList` while the dialog stays open, without a
`LoadList()` rebuilding the rows — `world.UnloadPlugin`/`ReloadPlugin` called
from a trigger or timer while the dialog's modal loop is pumping messages, or
the object `OnReload` already `delete`s before the `::UMessageBox` it shows on
a failed reload — leaves that row's item data pointing at memory this handler
then reads on the next idle-driven update-UI pass.
**Fix:** add the same `find()` liveness check the five sibling handlers
already have, skipping the row when its plugin is no longer in
`m_PluginList`.  `needs: windows-run` — canonical dialog code, unreachable
from the headless build; covered in-app by `GuiBridgeSelfTest`'s
`TestPluginsDlgDescriptionLiveness` (`platform/gui_bridge_selftest.cpp`),
which keeps the "erased" plugin object alive and only removes it from the
list, so the assertion exercises the liveness check itself rather than the
underlying use-after-free.

## 50 — Eight file dialogs `strcpy` an unbounded name into a `_MAX_PATH` buffer (this commit)
**Bug:** every "browse for a file" site in the app follows the same recipe —
`filedlg.m_ofn.lpstrFile = filename.GetBuffer (_MAX_PATH);` and then
`strcpy (filedlg.m_ofn.lpstrFile, suggested_name);`.  `GetBuffer (_MAX_PATH)`
hands out exactly `_MAX_PATH + 1` bytes, and nothing anywhere caps the length
of what is copied in: the log-session name is `FormatTime` applied to the
world's *Auto log file name* setting (`doc.cpp:754`), the trigger/alias/timer
set names are the world name plus a directory (`evaluate.cpp:617` and `:790`,
`GlobalPrefs.cpp:952`, `prefspropertypages.cpp:5299`), and the two sound
browsers copy in a whole saved path (`triggdlg.cpp:421`,
`prefspropertypages.cpp:9126`), and the world's beep sound copies in the
`beep_sound` option, which any script can set to any length
(`prefspropertypages.cpp:6136`).  All eight are user-settable and none is
validated for length, so a long enough one overruns the buffer.  There is no
platform ifdef involved; Windows overruns identically.  It has stayed quiet
because the `else` these copies live in is only taken when
`App.platform != VER_PLATFORM_WIN32s`, i.e. on every Windows since NT — the
arm everybody is on.
**Fix:** use the bounded copy the tree already uses elsewhere —
`lstrcpyn (filedlg.m_ofn.lpstrFile, suggested_name, _MAX_PATH);` — which
always NUL-terminates and matches the `nMaxFile` that MFC's own `CFileDialog`
gives the common dialog.  Covered headless by
`LogSession.AnOverlongSuggestedNameIsTruncatedRatherThanOverrunning` in
`tests/test_mfc_ui.cpp`, and in the app by "Beep sound browse (unbounded
initial path)" in the `--gui-bridge-selftest` harness.

**Addendum (2026-08-19):** the original sweep found seven, not eight.  The
eighth, `CPrefsP14::OnBrowseSounds`, was missed because the grep that found the
others keyed on the `App.platform == VER_PLATFORM_WIN32s` ternary they all sit
under — and this one has no such ternary, just a bare `strcpy`.  The lesson is
the usual one: a sweep is only as complete as the shape it greps for.

## 51 — A duplicate trigger/alias label whose replacement fails validation leaves the freed original in the map (this commit)
**Bug:** loading a trigger whose label is already taken takes the
`XML_OVERWRITE` arm (`xml_load_world.cpp:1371`), which `delete`s the existing
`CTrigger` but leaves `GetTriggerMap ()` keyed to it — the replacement is
expected to overwrite the key at the `SetAt` further down.  Between the two
sits the regular-expression compile (`:1394`), and a match text that does not
compile throws from there.  `Load_Triggers_XML` catches that with
`HandleLoadException`, which *reports and carries on*, so the `SetAt` never
runs and the map is left holding a freed pointer.  `SortTriggers ()` at the end
of the load then copies it into `m_TriggerArray`/`m_TriggerRevMap` and
`CompareTrigger` reads its `iSequence` (`doc.cpp:3325`, reached whenever there
are two or more entries), every later trigger evaluation walks the same array,
and `DELETE_MAP` in `~CMUSHclientDoc` frees it a second time.  `XML_OVERWRITE`
is set by an ordinary world open (`xml_serialize.cpp:50`) and by
`world.ImportXML` (`methods_xml.cpp:59-60`), so a world file or an imported
payload holding two same-labelled triggers, the second with a bad pattern, is
enough.  `Load_One_Alias_XML` (`:1602`) is the identical shape.  The timer arm
(`:1842`) has the same code but nothing between the delete and its `SetAt` can
throw, so it is not affected.
**Fix:** `RemoveKey` the label immediately before deleting the old object, so
the map never holds a freed pointer — exactly what bug 1 above did for the MXP
custom-element map (`mxpDefs.cpp:153`).  A refused replacement now takes the
original with it rather than leaving a corpse behind.  Covered by
`WorldLoadR5.DuplicateLabelWhoseReplacementFailsLeavesNoFreedEntry` in
`tests/test_world_load.cpp`, which loads a second valid trigger so that
`SortTriggers` really does compare two entries; without the fix it fails both
assertions and then crashes in the destructor.
## 52 — A nested plugin dispatch turns note batching off for the outer one (this commit)
**Bug:** every `SendToAllPluginCallbacks*` sets `m_bNotesNotWantedNow` true on
entry so that a plugin's `world.Note`/`Tell` is queued into
`m_OutstandingLines` and emitted after the walk, instead of landing in the
middle of the line being processed.  All six of them assign `false` on the way
out rather than putting back what they found, and the flag is a plain `bool`,
so the first callback that dispatches again unbatches the notes of every plugin
after it.  The path is ordinary: `ProcessPreviousLine` dispatches
`ON_PLUGIN_LINE_RECEIVED`, a handler calls `world.Send`, `DoSendMsg` dispatches
`ON_PLUGIN_SEND`, and *that* call's exit clears the flag for the rest of the
outer walk.  The plugin sitting after the sender then writes its note inline
and out of order.  `pSavedPlugin`, three lines above in the same functions,
already shows the shape the flag should have had.
**Fix:** save the incoming value alongside `pSavedPlugin` and restore it on
every exit path in all six dispatchers.  Covered by
`PluginGuards.NestedDispatchKeepsNoteBatchingOn` in `tests/test_plugins.cpp`.

## 53 — The plugin wizard re-sorts the whole world once per surviving item (this commit)
**Bug:** in `OnFilePluginwizard`'s four "now remove the items we exported"
loops the `if (iCount) { ... SortTriggers (); SetModifiedFlag (TRUE); }` block
sits *inside* the loop, before its closing brace, rather than after it.  Each
`SortTriggers`/`SortAliases`/`SortTimers` does a `SetSize`, a full map walk
rebuilding the array and the reverse map, and a `qsort`, and the wizard
pre-selects everything, so the removal is O(N² log N) in the number of items
instead of one sort at the end.  A world with a few thousand triggers takes
seconds to finish the wizard.
**Fix:** move each block past its loop's closing brace.  Nothing in a loop body
reads the sorted array or the modified flag, so this is behaviour-preserving,
and the `if (iCount)` guard keeps the nothing-selected case identical.  The
loops moved into `CMUSHclientDoc::RemoveSelectedWizardItems` so they can be
driven without a property sheet; covered by
`WizardTest.RemovingSelectedItemsSortsOncePerCategory` in
`tests/test_plugins.cpp`, which pins the complexity with a timed 3,000-trigger
run rather than a comment.
## 54 — The Lua `error_code`/`error_desc` tables collide on 30037 (this commit)
**Bug:** `errors.h` numbers `ePluginCouldNotSaveState` as 30038 — the next
code after `ePluginDoesNotSaveState`'s 30037 — and that is what
`methods_plugins.cpp` actually returns from a failed `world.SaveState`.  But
`lua_methods.cpp`'s `error_codes[]` (the `error_code.*` table a script reads)
and `errors.cpp`'s `error_descriptions[]` (`world.ErrorDesc` / the
`error_desc[]` table) both key the entry `30037` — the code
`ePluginDoesNotSaveState` already owns.  `MakeIntFlagsTable` builds each table
with one `lua_rawset` per row, so the duplicate key is last-write-wins:
`error_desc[30037]` ends up holding "Plugin could not save state..." text
overwriting "Plugin does not support saving state...", and `error_desc[30038]`
— the code `SaveState` actually returns — is nil.  A script cannot recognise
or report the failure it was actually given.
**Fix:** change the second entry to `30038` in both tables
(`lua_methods.cpp:7444`, `errors.cpp:58`) so each of the two distinct plugin
save-state errors has its own code and description.  Covered by
`LuaScripting.ErrorCodeTable` in `tests/test_lua_scripting.cpp`.  A **third**
copy of the same collision shipped in the Lua constants library
(`plugins/constants.lua:54`, `<include>`d by three shipped plugins), also
corrected to `30038`.

## 55 — `ExecuteLua`'s "function not found" arm drops the note style (this commit)
**Bug:** both `CScriptEngine::ExecuteLua` overloads save `m_iNoteStyle` and
force it to `NORMAL` before looking the callback up, meaning to restore it
once the callback returns — but the `!GetNestedFunction` ("this callback name
does not exist") arm `return true`s before reaching that restore.  A Lua
callback whose name was later removed or renamed therefore leaves the world's
note style stuck at `NORMAL` for the rest of the script batch, discarding
whatever an outer handler had set with `world.NoteStyle`.  The five embedded
engines' forward, immediately above in the same file, already restores
unconditionally on every path — Lua was the odd one out on both platforms; the
COM host's equivalent early return (`scriptengine.cpp:101`) restores too.
**Fix:** add the missing `m_pDoc->m_iNoteStyle = iOldStyle;` to the
`!GetNestedFunction` arm of both overloads.  Covered by
`SweepR4.ExecuteLuaMissingRoutineRestoresNoteStyle` and
`SweepR4.ExecuteLuaSimpleOverloadMissingRoutineRestoresNoteStyle` in
`tests/test_lua_scripting.cpp`.

## 56 — `world.ArrayImport` mistakes a doubled backslash for an escaped delimiter (this commit)
**Bug:** `ArrayImport`'s pre-pass un-escapes the exported string with a single
`FindAndReplace (sValues, sEscapedDelimiter, sTemp)` (methods_arrays.cpp:284-305).
`ArrayExport` doubles every backslash and then escapes the real delimiter, so a
key or value ending in a literal backslash right before a REAL, unescaped
delimiter is exported as two backslashes followed by the separator. The naive
left-to-right search matches the SECOND backslash plus the following delimiter
as if it were an escaped one and substitutes the scratch character over it,
eating the real field separator. With one such field the import fails loudly
with `eArrayNotEvenNumberOfValues`; with two, the field count stays even and
`ArrayImport` silently stores corrupted key/value pairs instead.
**Fix:** replace the blind `FindAndReplace` pre-pass with a left-to-right scan
that consumes a doubled backslash as an untouched literal pair (leaving it for
the existing post-split un-doubling) and only substitutes the scratch
character for a backslash immediately followed by the delimiter when that
backslash is not itself the second half of such a pair. Covered by
`LuaScripting.ArrayImportHandlesValueEndingInBackslashBeforeDelimiter` and
`LuaScripting.ArrayImportHandlesTwoValuesEndingInBackslashBeforeDelimiter` in
`tests/test_lua_scripting.cpp`.

## 57 — `world.Help` passes its Lua flag into `ShowFunctionslist`'s start-offset parameter (this commit)
**Bug:** `CMUSHclientDoc::Help`'s fallback call reads `ShowFunctionslist
(m_strFilter, bLua)` — the two-argument call binds `bLua` to the SECOND
parameter, `nStartChar` (`int`), not the fourth, `bLua` (`bool`), which then
takes its default of `false`. A Lua-enabled world's Function List therefore
never lists Lua's own functions, and the bogus `nStartChar` of 0 or 1 also
blanks `GetSelectedFunction`'s filter (its `nEndChar > nStartChar` guard fails
for 1/0). `CMainFrame::OnGameFunctionslist` (`functionlist.cpp:605`,
Windows-only) had the identical mistake — `ShowFunctionslist (s, true)`.
**Fix:** call `ShowFunctionslist (m_strFilter, 0, 0, bLua)` and
`ShowFunctionslist (s, 0, 0, true)` respectively, naming all four parameters so
neither collapses again. Covered by
`Utilities.HelpPassesBLuaAndZeroOffsetsToShowFunctionslist` in
`tests/test_utilities.cpp`.

## 58 — `world.ImportXML` leaks the `CArchiveException` on every failed import (this commit)
**Bug:** `ImportXML`'s `catch (CArchiveException*)` clause has no parameter
name, so it can never call `e->Delete()` — every malformed-XML import leaks the
heap-allocated exception object. The two other handlers in the same file
(`ExportXML`, `GetXMLEntity`) both call `Delete()` on the way out.
**Fix:** name the parameter and call `e->Delete()` before setting `iCount =
-1`, matching the other two handlers. Covered by
`LuaScripting.ImportXMLDeletesArchiveExceptionOnFailure` in
`tests/test_lua_scripting.cpp`, using a live-instance counter added to the
shim's `CArchiveException` (`platform/mfc_ui.h`) since a leak otherwise leaves
no other observable trace.

## 59 — `SetBackgroundImage` and `SetForegroundImage` duplicate a 66-line body (this commit)
**Bug:** the two methods are byte-identical apart from the three
`CMUSHclientDoc` members they touch (`m_BackgroundBitmap`/`m_ForegroundBitmap`,
`m_iBackgroundMode`/`m_iForegroundMode`,
`m_strBackgroundImageName`/`m_strForegroundImageName`), so any fix to the
image-loading logic — the PNG-vs-BMP sniff, the error mapping, the bitmap
lifetime — has to be made twice and can silently be made once, the exact
pattern behind several of the other entries in this file (portable twins,
forked TUs). No functional consequence today; the two bodies had not yet
diverged.
**Fix:** extract the shared body into a static `SetFrameImage` helper taking
the bitmap/mode/name triple by reference, and reduce both methods to a single
call each — behaviour-identical on both platforms. Covered by
`LuaSurface.SetImageFamilySharesOneBodyBackgroundAndForeground` in
`tests/test_lua_surface.cpp`, which drives both entry points over the same
four fixtures so a future edit landing on only one of them shows up as a
divergence.
## 60 — The RGB bold/unbold colour scans stop one palette entry short (this commit)
**Bug:** in RGB mode the client cannot re-derive a colour from its index at
paint time, so `ANSI_BOLD` and `ANSI_CANCEL_BOLD` swap the stored RGB value by
scanning for it in `m_normalcolour` / `m_boldcolour`.  Both arrays are `[8]`,
indexed `BLACK=0 … WHITE=7` (stdafx.h), but both scans are written
`for (int i = 0; i < 7; i++)` — so index 7 is never compared.  White is both
the commonest foreground and the default one, so in a world with
*Use custom colour 16 as default* set, bold and unbold silently did nothing to
plain white text; every other colour worked, which is why it went unnoticed.
The bound dates to the 2010 import.
**Fix:** `i < NUMITEMS (m_normalcolour)` / `i < NUMITEMS (m_boldcolour)`, the
idiom the rest of the tree already uses for these walks, so the arrays cannot
drift out from under the loops again.  Covered by
`AnsiRgbMode.BoldScanReachesTheLastPaletteEntry` in `tests/test_ansi.cpp` —
deliberately at index 7, because the pre-existing
`BoldUpgradeAndDowngradeScanPalette` exercises index 1 and passes either way.

## 61 — `CmcDateTime`'s constructor truncates a fractional second (this commit)
**Bug:** `CmcDateTime::SetDateTime` takes a `double nSec` and passes it straight
into `mcDateFromTm`, whose corresponding parameter is a `WORD` — so the implicit
conversion discards the fraction.  `world.SetTimerOption ("second", …)` accepts
up to 59.9999 and the Timer dialog's `IDC_AT_SECOND` field validates the same
range, but `timers.cpp` feeds `fAtSecond` through this constructor, so an
`eAtTime` timer configured for (say) 30.75 was built as 30.0 and fired up to
just under a second early.
**Fix:** widen `mcDateFromTm`'s seconds parameter to `double`.  Its range check
becomes `nSecond < 0.0 || nSecond >= 60.0` — identical accept/reject behaviour
for every value the callers can supply, and well-defined for a negative one,
which previously wrapped through the `WORD` conversion.  The rest of the body
needs no change: the seconds term already feeds a floating-point division by
`SECS_IN_DAY`, and the negative-serial sign convention a few lines below is
untouched.  Covered by `CmcDateTimeTest.ConstructorKeepsFractionalSeconds` in
`tests/test_mcdatetime.cpp`, which also pins the range check at both ends.
(`c9fb64d` had already worked around this truncation for `GetTimeNow`'s POSIX
arm by building whole seconds and adding the fraction onto `m_dt` afterwards;
that arm still reads correctly and is left alone, but its comment no longer
describes a live defect and has been corrected.)

## 62 — `DisplayMsg`'s tab stop measures the line in bytes (this commit)
**Bug:** the `'\t'` case computes the next tab stop as
`((m_pCurrentLine->len + 8) & 0xFFF8)` and guards it with
`m_pCurrentLine->len >= m_nWrapColumn`.  `len` is the *stored byte* count, but
both quantities are character columns — `AddToLine` says so in as many words
("for Unicode the width of the line is characters, not stored bytes") and
recomputes the length with `MultiByteToWideChar` for the very same wrap
comparison.  So in a UTF-8 world a tab following non-ASCII text pads to a
multiple of 8 *bytes*, landing short of the column the user sees, and the wrap
test fires early on a line that still has room.  The neighbouring period-space
case (`m_pCurrentLine->len < m_nWrapColumn`, which decides whether
`. ` becomes `.  `) has the identical confusion.
**Fix:** hoist the character-length rule into one file-local helper,
`LineLengthInChars`, and use it at all three sites — the two in `DisplayMsg`
and the original in `AddToLine`, whose three lines it replaces verbatim.  The
three measurements can no longer disagree.  ASCII worlds are unaffected: with
`m_bUTF_8` false the helper returns `len`.  Covered by
`WrapTest.Utf8TabStopCountsCharactersNotBytes` in `tests/test_display.cpp`,
which tabs an ASCII line and a multi-byte line to the same character column and
asserts the fixture's bytes and characters actually differ.

## 63 — "Use SSL" is not compared, so ticking it is discarded (this commit)
**Bug:** `CMUSHclientDoc::ChangedPrefsP1` decides whether pressing OK on World
Properties ▸ Connecting dirties the world.  It compares nine members — port,
name, server, auto-save and the five proxy fields — and omits `m_bUseSSL`, which
`SavePrefsP1` (`configuration.cpp:1395`) writes back and `LoadPrefsP1` (`:124`)
loads.  `SetModifiedFlag (TRUE)` fires only if some `ChangedPrefsP*` answers
true, and the `SavePrefs*` calls run unconditionally afterwards, so a user who
ticks "Use SSL" and changes nothing else gets it applied to the live document
and never written to the world file.  The reloaded world connects in the clear.
The omission dates from the TLS commit itself, which added only the Load/Save
lines, so Windows has it too.
**Fix:** add a `m_bUseSSL` term, normalising both sides to `bool` (the member is
an `unsigned short`, the dialog field a `BOOL`) — the same shape as the
`bMultiLine` term in bug 22 above.  Covered by "World-properties change
detection (Use SSL)" in the `--gui-bridge-selftest` harness.

## 64 — `RecompileRegularExpressions` recompiles from the PREVIOUS trigger's pattern (this commit)
**Bug:** the loop declares `CString strRegexp;` outside itself
(`configuration.cpp:2498`), then refreshes it only inside
`if (pTrigger->regexp)` while calling `regcomp` outside that guard.  A trigger
whose compiled expression is currently NULL is therefore recompiled from
whatever the previous iteration left in `strRegexp` — or from an *empty*
pattern if it is the first such entry, and an empty pattern matches every line
(`RegexpMatchEmpty` defaults to 1, so `regexec` drops `PCRE_NOTEMPTY`).
NULL-expression triggers are genuinely produced and kept in the map: by this
function's own `catch` (`:2524`) and by `CMUSHclientDoc::EvaluateTrigger`'s
variable-expansion recompile (`evaluate.cpp:431`).  Changing the world's UTF-8
setting is enough to run it.
**Fix:** compute `strRegexp` unconditionally, keeping only the `delete` inside
the guard.  The alias loop below repeats the defect verbatim and is fixed the
same way, though it is compiled out today (`ALIASES_USE_UTF8` is 0).  Covered by
"Trigger regexp recompile (stale pattern)" in the `--gui-bridge-selftest`
harness, which asserts each trigger matches its OWN text and not its
neighbour's — an assertion that fails in either map iteration order.

## 65 — `CTipDlg` `strcat`s the tips-file suffix past the end of a `MAX_PATH` array (this commit)
**Bug:** the constructor copies the module pathname into `char filename
[MAX_PATH]`, truncates it at the last separator, and then `strcat`s the suffix
on — `"\\tips.txt"` on Windows, `"/../Resources/tips.txt"` in a Mac bundle —
with nothing bounding the result.  `GetModuleFileName` hands back up to
`MAX_PATH - 1` characters, so an install path whose directory part reaches 238
(249 on the Mac, where the suffix is longest) smashes the stack the moment
Help ▸ Tip of the Day, or the tip shown at startup, runs.
**Fix:** hoist the join into `TipsFileName` in `utilities_shared.cpp`, which
builds it as a `CString` and so has no fixed length at all; all three platform
arms move with it.  Covered by
`Utilities.TipsFileNameSurvivesADirectoryLongerThanMAX_PATH` and
`Utilities.TipsFileNameDropsTheExecutableName` in `tests/test_utilities.cpp`.

## 66 — The mapper's "make alias" throws away seven of the dialog's fields (this commit)
**Bug:** `CMapDlg::AddAlias` (`MapDlg.cpp:193`) runs the same `CAliasDlg` as the
world-properties Aliases page, but keeps its own copy of the dialog-to-`CAlias`
field list — and that copy had fallen behind `CPrefsP7::UnloadDialog`
(`prefspropertypages.cpp:2703`) by seven fields: `bTemporary`, `strVariable`,
`iSequence`, `bKeepEvaluating`, `bOneShot`, `bEchoAlias` and
`bOmitFromCommandHistory`.  All seven are live controls in `CAliasDlg`'s DDX
map, so everything the user sets in them is silently discarded and the alias
keeps the `CAlias` constructor's defaults.  `UnloadDialog` even ends with the
comment "NB - also see MapDlg.cpp for alias processing", which is exactly the
kind of note that does not survive a decade.
**Fix:** one `CopyAliasFromDialog` helper, called by both.  `AddAlias`
additionally seeds `dlg.m_iSequence = DEFAULT_ALIAS_SEQUENCE` before `DoModal`,
because `CAliasDlg`'s constructor zeroes that field while `CAlias`'s uses the
default — copying it without seeding would give every mapper alias a sequence of
0.  Covered by "Mapper 'make alias' (dropped dialog fields)" in the
`--gui-bridge-selftest` harness.

## 67 — World Properties rejects a paste/send delay the option table allows (this commit)
**Bug:** `paste_delay` and `send_file_delay` are bounded 0..100000 in
`scriptingoptions_numeric.inc` (118 and 133), and `SetOptionItem` validates
against that table — so a script, an MXP payload or a world file can
legitimately set 50000.  But `CPrefsP10::DoDataExchange` and
`CPrefsP13::DoDataExchange` both run `DDV_MinMaxLong (pDX, m_iLineDelay, 0,
10000)` over the same two members.  Any value above 10000 therefore makes the
World Properties sheet refuse to close, with no way out of the dialog but to
lower a number the user never chose.
**Fix:** widen both `DDV_MinMaxLong` ceilings to 100000 so the dialog and the
option table agree.  Covered by "Send/Paste delay validation range" in the
`--gui-bridge-selftest` harness, which also asserts 100001 is still refused.
Needs a Windows run — see `docs/WINDOWS-VALIDATION.md`.

## 68 — MCCP `Z_NEED_DICT` from a crafted zlib header spins the decompressor forever (this commit)
**Bug:** `OnBytesReceived`'s decompression loop (`doc_shared.cpp` ← `doc.cpp`)
tests only `if (iCompressResult < 0)` for an inflate failure.  A server whose
MCCP stream sets the zlib `FDICT` header bit makes `inflate` return
`Z_NEED_DICT` (+2): it consumes no input, produces no output, is not negative
and is not `Z_STREAM_END`, so the outer `while (true)` re-`inflate`s the same
bytes with `avail_in` unchanged — an unbounded 100%-CPU loop that freezes the
UI, escapable only by force-quit.  The `avail_in <= 0` guard is *before* the
loop, so it never trips.  Entirely server-controlled: a remote denial of
service.
**Fix:** treat any result that is neither `Z_OK` nor `Z_STREAM_END` as a
decompress failure (`Z_BUF_ERROR` is already handled by the inner
grow-the-buffer loop), so `Z_NEED_DICT` and every other non-progress code
disconnect through the existing error arm.  Covered by
`MccpArms.DictionaryRequestDisconnectsInsteadOfSpinning` in
`tests/test_connection.cpp`, which feeds an `FDICT` stream and asserts the world
disconnects instead of hanging (reverting the fix makes that test time out).

## 69 — MCCP stream-end residue can overflow the transport's receive buffer (this commit)
**Bug:** at `Z_STREAM_END` the loop copies the unconsumed post-stream residue
back into the caller's buffer with `memcpy (buff, m_zCompress.next_in,
m_zCompress.avail_in)`.  The append-guard caps `avail_in` at
`COMPRESS_BUFFER_LENGTH` (10000), but the receive buffer `buff` points at was
`char buff[9000]` in both transports (`platform/posix_transport.cpp`,
`pal/win_transport.cpp`) — whose own comment even claimed it "must be less than
COMPRESS_BUFFER_LENGTH".  A crafted stream that reaches end-of-stream with
`avail_in` between 9001 and 10000 (a small compressed prefix, then a large plain
residue, accumulated across two short packets) overflows that stack buffer by
hundreds of bytes.
**Fix:** size both transport receive buffers at `COMPRESS_BUFFER_LENGTH` — the
residue is real post-stream telnet data and must not be dropped, so clamping the
`memcpy` would be wrong — and assert `avail_in <= COMPRESS_BUFFER_LENGTH` at the
copy.  Covered by `MccpArms.LargeStreamEndResidueIsPreservedNotTruncated` in
`tests/test_connection.cpp`, which drives `avail_in` past 9000 and asserts the
whole residue (a marker at its tail) survives.  The `pal/win_transport.cpp`
buffer change is by-reasoning and needs a Windows build run to confirm.

## 70 — MCCP compression-start `memmove` into the input buffer has no size guard (this commit)
**Bug:** when a world switches into compressed mode mid-packet
(`Phase_COMPRESS_WILL` and `Phase_SUBNEGOTIATION_IAC` in `DisplayMsg`,
`doc_shared.cpp` ← `doc.cpp`), each arm copies the post-`SE` remainder into
`m_CompressInput` with `memmove (m_CompressInput, p, size)` and no bound — even
though `m_CompressInput` is only `COMPRESS_BUFFER_LENGTH` bytes, and unlike the
sibling append path in `OnBytesReceived`, which disconnects when the data would
not fit.  A packet whose remainder exceeds the buffer (reachable when a plugin's
`ON_PLUGIN_PACKET_RECEIVED` rewrite or `world.Simulate` produces more than
`COMPRESS_BUFFER_LENGTH` bytes carrying an MCCP-start sequence) overflows the
heap.  Local rather than remote, but a real overflow.
**Fix:** guard both arms with the same `size > COMPRESS_BUFFER_LENGTH` →
disconnect check the append path already uses.  Covered by
`MccpArms.OversizeCompressionStartDisconnects` in `tests/test_connection.cpp`.

## 71 — Debug listings print a bare "..." at a narrow wrap column (`69155bd`)
**Bug:** `/triggerlist`, `/aliaslist`, `/variablelist` and `/acceleratorlist`
each compute their truncation width as `(m_nWrapColumn - N)` with no floor. At
a narrow enough wrap column that goes zero or negative, and `CString::Left`
clamps `n <= 0` to empty, so every entry printed a bare `"..."` instead of any
of the match/name/value text.
**Fix:** floor the width at a `MIN_DEBUG_TRUNCATE_WIDTH` of 20 columns, via a
new `ClampDebugTruncateWidth` helper in `world_debug.cpp` and the matching
change to `Truncate()` in its headless twin
`scripting/world_debug_portable.cpp`. Covered by
`SweepR4.DebugListingsTruncateAtNarrowWrapColumn` and
`SweepR4.DebugListingsTruncateLongValueToTheWrapBudget` in
`tests/test_lua_scripting.cpp`.

## 72 — `strnrchr` reads past the line buffer, ignoring its length bound (`6491c97`)
**Bug:** the loop was `while (*s && n < count++)` — the counter `n` is declared
`0` and never incremented, while `count++` grows the *bound* on every iteration,
so for any `count > 0` the comparison stays true and the only exit is the
terminating NUL: the `count` length limit documented in `stdafx.h` is ignored
entirely. Both callers — `doc_shared.cpp:3379` and `:3451`, the MXP
`<P>`-paragraph-mode last-space scan — pass `m_pCurrentLine->text`, a `CLine`
buffer that is exactly `len` bytes with no NUL terminator (`CLine::CLine` /
`AddToLine` never write one), so a hostile long line with no space scans past
its `len` valid bytes into uninitialised heap, and possibly past the
allocation. It also returned the wrong last-space position, corrupting
paragraph-mode space collapsing.
**Fix:** increment the counter, not the bound (`while (*s && n++ < count)`), so
the scan stops at `count`. Shared since the `Utilities.cpp` original; covered by
a mutation-verified test in `tests/test_utilities.cpp`.

## 73 — MXP argument-list early returns leak their CArgument nodes (this commit)
**Bug:** `CArgumentList` is a `CTypedPtrList` whose destructor frees only its
list nodes, not the `CArgument*` pointees — those are freed explicitly with
`DELETE_LIST`. Several MXP error returns skipped it. In `MXP_StartTag`'s
user-defined-element expansion the "No closing ';' in MXP element argument"
abort (`mxpStart.cpp`) returned without draining the LOCAL `BuiltArgumentList`,
so every `CArgument` already built for that tag leaked — and because it is a
stack local, not the function-static list, no later call ever reclaims it: a
permanent, attacker-reachable leak (define e.g. `<!ELEMENT boom '<font color=red
back="&x">'>`, then `<boom>` leaks the `color=red` argument). The same
function's atomic-path entity abort, and `MXP_Element`'s eight
malformed-definition returns plus its bad-attribute return, likewise skipped
`DELETE_LIST` on their function-static `ArgumentList` (which the next call's
`BuildArgumentList` drains, so latent unless MXP is then left idle). This
completes the class F21 (`0dae192`) fixed for the neighbouring returns.
**Fix:** `DELETE_LIST` the appropriate list(s) before each of these returns —
`BuiltArgumentList` and the static `ArgumentList` at the custom-element abort,
and the static `ArgumentList` at the atomic abort and every `MXP_Element` early
return. Covered by `MxpR4.CustomElementArgumentAbortDoesNotLeakTheBuiltList` in
`tests/test_mxp.cpp` (a differential live-heap-blocks test, mutation-verified).

## 74 — An unterminated telnet subnegotiation grows the collector without bound (this commit)
**Bug:** `Phase_SUBNEGOTIATION` appends every data byte to
`m_IAC_subnegotiation_data` (and `Phase_SUBNEGOTIATION_IAC` appends one more for
each escaped `IAC IAC`) until it sees `IAC SE`. Nothing capped the string's
length, so a hostile or broken server that opens a subnegotiation
(`IAC SB <type>`) and never sends `IAC SE` keeps the phase machine in
`HAVE_SUBNEGOTIATION` across packets and appends forever — unbounded heap growth
ending in memory exhaustion (on the sacred 32-bit Windows build `std::string`
throws at ~2GB, terminating). The sibling MCCP input buffer was hardened with a
`COMPRESS_BUFFER_LENGTH` cap (`759d778`); this accumulator had no equivalent.
**Fix:** cap `m_IAC_subnegotiation_data` at a file-local
`MAX_SUBNEGOTIATION_LENGTH` at both append sites; past it, drop the connection
via `OnConnectionDisconnect` + `UMessageBox`, the same decisive response the
MCCP input path already gives. The bound is 256 KB — deliberately larger than
the audit's "a few KB" suggestion so that no legitimate GMCP/MSDP/MTTS/CHARSET
payload is ever clipped, while still bounding the hostile-stream memory to a
fixed amount per connection (any finite cap defeats the exhaustion attack).
Upstream Windows has **no** cap at all, so this diverges from it only by adding
the bound; it is a security hardening of the shared `DisplayMsg` phase machine,
not a behaviour change any world relies on, and a Windows run should confirm the
256 KB ceiling clears real traffic. Covered by
`TelnetTest.Phase_SUBNEG_UnterminatedSubnegotiationIsCappedAndDisconnects` and
`Phase_SUBNEG_IAC_EscapedIACsAreAlsoCapped` in `tests/test_telnet.cpp`,
mutation-verified.

## 75 — Colour-matching triggers never consult the style's `COLOURTYPE` (`ef5466f`)
**Bug:** after a trigger's text matched, `ProcessOneTriggerSequence` found the style
run under the match and compared the trigger's stored ANSI colour number straight
against the style's raw members:

```cpp
iFlags = pStyle->iFlags & STYLE_BITS;
iForeColour = pStyle->iForeColour;
iBackColour = pStyle->iBackColour;
...
if (get_foreground (trigger_item->iMatch) !=
    ((iFlags & INVERSE) ? iBackColour : iForeColour))
  continue;
```

`get_foreground`/`get_background` yield 0-7, but `iForeColour`/`iBackColour` only
hold ANSI colour *numbers* while `(iFlags & COLOURTYPE) == COLOUR_ANSI`. `COLOURTYPE`
(`0x0300`) survives the `STYLE_BITS` (`0x0FFF`) mask — the comparison simply never
looked at it. Three consequences:

* **`COLOUR_RGB`** (24-bit `ESC[38;2;r;g;b`, MXP `<color>`, an earlier trigger's own
  colour change, a hyperlink recolour) stores a literal `COLORREF` there, so a
  condition on colours 1-7 could not match *except by coincidence* — `COLORREF` is
  `0x00BBGGRR`, so a near-black red is the integer 1. There was exactly one such
  false positive per ANSI colour, and black's (`0 == 0x000000`) is the one that made
  the bug look half-working.
* **`COLOUR_CUSTOM`** stores a 0-15 index into `m_customtext`, which the matcher read
  as if it were an ANSI number: custom slot 1 matched "red".
* **`COLOUR_ANSI` with an index >= 8** — which is what `ESC[38;5;n` produces, the
  xterm lookup being deferred to display time — could never match, so **no colour
  trigger fired on any xterm-256 line at all**.

**Fix:** both sides now resolve through `pal::resolveStyleColours` (`pal/style_colour.h`),
the one body `GetStyleRGB` paints with, and the comparison is made in RGB space. The
run's displayed pair comes from its real flags/colours; the trigger's 0-7 numbers are
resolved as an ANSI style carrying the **same `HILITE`/`INVERSE` flags as the run**,
with the two indices crossed over under `INVERSE`. Carrying `HILITE` across is what
preserves today's behaviour (a "red" condition keeps matching bold red, because
boldness is the separate `TRIGGER_MATCH_HILITE` condition); leaving the `INVERSE` swap
to the resolver also picks up `m_bAlternativeInverse`, which the hand-rolled swap
ignored entirely. `GetStylePalette` is snapshotted at most **once per line**, lazily,
and only when some trigger actually carries `TRIGGER_MATCH_TEXT`/`_BACK`; the snapshot
is dropped after any trigger that fired into a script, since `world.SetNormalColour`
can repaint the palette mid-line.

**Not a second finding:** the trigger dialog offers "(any)" plus exactly eight colour
names and `CPrefsP8::SaveDialog` stores `((index - 1) & 7) << 4`, and
`get_foreground`/`get_background` mask with `0x07`, so the stored value can never
leave 0-7 by either route. `iMatch` exists only on `CTrigger` (`OtherTypes.h:571`) —
aliases and timers have no colour conditions, and `ProcessOneTriggerSequence` is the
only matcher.

The fix deliberately changes *what matching means* — see
`docs/UPSTREAM-DIVERGENCES.md` §11. Covered by `TriggerColourMatch.*` (7 groups) in
`tests/test_triggers.cpp`, over a deliberately non-degenerate palette fixture where
every entry is a large distinct value, so no index can be confused with its own
resolved colour.

## 76 — A password typed at an `IAC WILL ECHO` prompt is written to the session log (port only: `32b28f6`)
**Bug:** when a server sends `IAC WILL ECHO`, `telnet_phases.cpp:397` sets `m_bNoEcho`.
The only consumer of that flag on the send path is `SendMsg` (`doc_shared.cpp:2374`):

```cpp
  if (m_bNoEcho)
     bEcho = false;
```

It gates **drawing only**. `DoSendMsg` logs on a completely separate argument, and on
Windows a typed line reaches it through `CSendView::SendCommand` -> `Execute` ->
`EvaluateCommand`, whose send-now arm passed `LoggingInput()` (i.e. `m_log_input`)
unconditionally. So with *File > Log Session* open and **"log my input"** on, the
password is hidden on screen and written verbatim to the log file. Windows also has no
way to opt out: `Execute` declares its `bOmitFromLog` local as `false`
(`methods_commands.cpp:275`) and the parameter is never read by `EvaluateCommand`
anyway.

**Fix (port only):** `evaluate.cpp` gained a `CSuppressInputLogging` guard keyed off the
`bOmitFromLog` argument, giving the existing parameter the meaning its name always
claimed. **Windows is not fixed by this.** The guard is inert there: the sole Windows
caller of `EvaluateCommand` is `Execute`, which passes `bOmitFromLog = false`, so
`m_bActive` is false and the guard neither reads nor writes `m_log_input`. Making
Windows safe would mean gating the *log* on `m_bNoEcho` in `SendMsg`/`DoSendMsg` — a
Windows-observable behaviour change that needs a decision, not a port fix.

**Residual, not fixed on either platform.** `ProcessOneAliasSequence` echoes the typed
line itself with `bEchoAlias` seeded from `m_display_my_input`, **not** from
`m_bNoEcho`, so `SendMsg`'s echo guard does not cover it: a password matching an
enabled alias with *echo alias* set lands in `m_LineList` as a `USER_INPUT` line even
at a masked prompt. After the fix that line carries no `LOG_LINE` flag and nothing is
written at the time, but the leak is **retrospective** — when a log is opened later,
`doc.cpp:893` replays the buffer and accepts a line if
`(flags & LOG_LINE) || (flags & NOTE_OR_COMMAND)`, and `NOTE_OR_COMMAND` is `0x03`
(`COMMENT | USER_INPUT`), so **any** `USER_INPUT` line qualifies regardless of
`LOG_LINE`; the only gate left is `m_log_input` at open time, which is back to its real
value by then. The recommended fix is one line in `evaluate.cpp` —
`bool bEchoAlias = m_display_my_input && !m_bNoEcho;` — which is arguably an upstream
bug fix in its own right (the server said "stop echoing" and the client echoes anyway)
but changes Windows and so needs sign-off. Severity is low: it needs an alias whose
pattern matches the password, *echo alias* set on it, `display_my_input` on, and a log
opened afterwards.

**Adjacent, recorded not changed:** `SendImmediate` is the only `Send*` that logs
without being asked to (`DoSendMsg (Message, m_display_my_input, m_log_input)`), where
`Send`, `SendNoEcho` and `SendPush` all pass `false`. It is plausibly deliberate — it
is the queue-jumping twin of a *typed* immediate send — but it is an asymmetry worth
knowing about.

Covered by the password-path tests added with `32b28f6`; the comment corrections in
`5f78f45` remove an earlier claim that `m_bNoEcho` guaranteed anything about the log.

## 77 — Miniwindow mouse routing dereferences windows a callback has already deleted (`1d45c28`)
**Bug:** `WindowDelete` (`methods_miniwindows.cpp:832`) refuses with `eItemInUse` only
when `it->second->m_bExecutingScript` is set, and that flag is set only on the window
whose own callback is currently dispatching. Every *other* window is deletable from
inside a callback — including the ones the dispatch routine is holding across
`SendMouseEventToPlugin`:

| routine | pointer left dangling | by which callback |
|---|---|---|
| `MouseMoveMiniWindow` | `mw` | `ON_PLUGIN_MOUSE_MOVED` (no flag set at all), then the previous window's `CancelMouseOver` |
| `MouseDownMiniWindow` | `mw` | the previous window's `CancelMouseOver` |
| `MouseUpMiniWindow` | `mw` | the drag `ReleaseCallback`, then the previous window's `CancelMouseDown` |
| `MouseWheelMiniWindow` | `mw` | only via the nested-clear below |

The routine then hit-tested the freed window's `m_Hotspots`, read `m_sCallbackPlugin`
out of it, **dispatched the dead window's own callbacks**, and stored
`m_bExecutingScript = false` into freed memory.

A second, independent hole: `m_bExecutingScript` is a plain `bool`, not a counter, so a
**nested** dispatch on the same window clears it on the way out and the outer dispatch
loses its self-delete protection. That is reachable, not theoretical —
`WindowAddHotspot` re-enters the mouse routing synchronously when the new hotspot lands
under the cursor (`methods_miniwindows.cpp:583`), so a `MouseOver` callback that adds a
hotspot and *then* deletes its own window is allowed to, and the outer routine carries
on using it.

**Fix:** re-look-up each held window by the id already in hand after every call that can
run script, and abandon that branch if it is gone. This is deliberately **not**
"protect every window in use": that would answer `eItemInUse` to the legitimate
cross-window delete plugins do all the time (a toolbar button dismissing its own popup)
and would break working plugins, on Windows too. Re-lookup is already this file's idiom
one container down. The self-delete guard is unchanged.

**Contract points that must not be broken by later work:**

1. `SendMouseEventToPlugin` takes its `string` arguments **by value**
   (`miniwindow.h:369-375`). That is load-bearing, not an oversight: all twelve call
   sites pass strings living inside objects the callback may destroy. Changing those
   parameters to `const string&` would silently reintroduce a use-after-free at every
   one of them.
2. Every re-lookup id must be captured **by value before** the dispatch —
   `m_sPreviousMiniWindow` can be reassigned by a nested dispatch.

**Two adjacent hazards checked and deliberately left alone.** `WindowDelete` calls
`SortWindows()`, which reallocates `m_MiniWindowsOrder` and invalidates every iterator
into it; every loop over that vector and over `m_MiniWindows` was audited and **none
currently reaches script**, so no code change was made. The one to watch is
`world_debug.cpp:1496` (`DebugInfo`), which calls `Note()` per window and erases from a
`std::map` while iterating it — safe today only because `Note` fires no plugin callback
and no synchronous paint on either platform. Separately, the five existing
"re-establish the hotspot pointer" blocks in these routines all read `mw->m_Hotspots`,
so before this fix they were *themselves* the use-after-free whenever `mw` had died;
fixing the window pointers is what makes the existing hotspot idiom sound.

**Known limitation, stated rather than hidden:** `MouseWheelMiniWindow` does nothing
after its dispatch except `mw->m_bExecutingScript = false`, so its guard prevents a
one-byte store into freed memory with no observable consequence — only a sanitizer can
see it, and its test survives mutation of that one guard. The observable half of the
same hazard is covered in `MouseMove`. An ASan build over `test_lua_surface` would close
that gap and is the natural follow-up. Covered by ten controls in
`tests/test_lua_surface.cpp`, including
`MouseOverCallbackMayLoseItsOwnWindowToANestedDispatch`.

## 78 — `WindowFilter`'s vertical kernel is centred one row off, and skips region row 2 (`d8de347`)
**Bug:** `GeneralFilter` runs a separable 5-tap kernel — blur, sharpen, edge detect,
emboss, lesser blur, minor blur — as a horizontal pass then a vertical one. Both
preload window slots 1..4 with taps -2,-1,0,+1 and then, per output pixel, shift left
one slot and load the +2 tap, which puts the kernel centre on the pixel being written.
The horizontal pass loads that tap from `col + rgb + 6` (six bytes = two pixels), but
the vertical pass loaded `row + 3`. After the shift its window therefore held rows
-1..+3, so the kernel was centred on the row *below* the one it wrote — every filtered
region comes out displaced by one row. Worse, the row at offset +2 from the first
output row is never read at all: the preload supplies -2..+1 (clamped to rows 0,0,0,1)
and the loop's first load is row 3, so **region row 2 is sampled by no output row
whatsoever** and its content is silently dropped.

**Fix:** load `row + 2`, matching the horizontal pass and the +1 the preload leaves in
slot 4. The existing `min (max (…), iHeight - 1)` clamp already covers the bottom edge,
and the sliding window still holds pre-filter values for the rows behind (each was read
two iterations before that row was written), so the in-place pass is unaffected.
Covered by `LuaSurface.WindowFilterKernelIsCentredAndCoversTheLastColumn` in
`tests/test_lua_surface.cpp`, which blurs a lone bright pixel through `pal::CpuRaster`
and asserts the result is symmetric about that pixel's own row and that the
formerly-skipped row contributes; mutation-verified both ways. Reached by
`world.WindowFilter` on both platforms — see `docs/UPSTREAM-DIVERGENCES.md` §12.

## 79 — `WindowFilter` never filters the rightmost pixel column (`d8de347`)
**Bug:** the horizontal pass loops `for (col = 0; col < iLastByte - 4; col += 3)` where
`iLastByte == width * 3`. Stepping by 3, the last iteration is the largest multiple of
3 strictly below `3*width - 4`, which is `3*(width-2)` — pixel `width-2`. The rightmost
pixel column is read as a neighbour by the clamped look-ahead but never written, so it
survives every blur/sharpen/edge/emboss unfiltered, leaving a one-pixel-wide
unprocessed stripe down the right of the region.
**Fix:** `col < iLastByte`. The widened bound is in range in both directions: the write
reaches at most `col + rgb == iLastByte - 1`, and the look-ahead read was already
clamped to `iLastByte - 1` — the same edge clamp columns `width-2` and `width-3` have
always used, so no new clamp semantics are introduced. The left-edge clamp (which
resolves an off-region tap to byte index 0, i.e. pixel 0's BLUE byte for all three
channels) and the in-place B/G/R pass ordering are deliberately **unchanged**, so a
filtered left edge is still slightly channel-skewed; that is upstream behaviour and the
covering test pins the exact values so a later change to it is visible. Covered by the
same test as 78, asserting exact post-filter values for the last column under both blur
and sharpen.

## 80 — SQLite INTEGER columns truncate to 32 bits in all four readers (`648622c`)
**Bug:** SQLite stores an INTEGER column as a signed 64-bit value, but every reader that
hands one to a script fetched it with `sqlite3_column_int`, which returns a 32-bit
`int`. Four call sites, covering all six scripting languages: `DatabaseColumnValue`
(`methods_database.cpp:401`) and `DatabaseColumnValues` (`:687`) on the
automation-dispatch path used by Python, Perl, JavaScript, Tcl and Ruby, and
`GetDatabaseColumnValue` (`lua_methods.cpp:1894`) and `L_DatabaseColumnValues` (`:2088`)
on the Lua path. `DatabaseGetField` inherits the defect through both. A rowid or
timestamp past 2^31 came back wrapped — `2147483648` read as `-2147483648` — with no
error and no indication. The array reader was doubly wrong on Windows: it widened the
already-truncated `int` into a `long` and stamped `VT_I4`, so the widening looked
deliberate.

**Fix:** `sqlite3_column_int64` at all four sites, plus one shared banding decision in
the new `scripting/db_int64.h` that all four readers call, because neither destination
can carry a full int64 — the dispatch VARIANT has no 64-bit integral type (`VT_I8`
exists nowhere in the shipping dispatch surface, nor in `platform/mc_dispatch.*`), and a
Lua number is a double:

| value | representation | why |
|---|---|---|
| within 32-bit signed range | `VT_I4` / Lua number | unchanged — nothing that works today moves |
| magnitude up to 2^53 | `VT_R8` / Lua number | exactly representable as a double |
| larger | `BSTR` / Lua string, decimal | lossless, and detectable as `type(v) == "string"` |

The string band is not an invention: `DatabaseLastInsertRowid`
(`methods_database.cpp:495-506`) has always formatted its `sqlite3_int64` with `"%I64d"`
and returned a string for exactly this reason, and the bundled `lsqlite3`
(`lsqlite/lsqlite3.c:111-121`) independently does the same. Only the threshold differs:
lsqlite3's test is value-dependent (so it returns 2^60 as a *number*, a power of two
being exact), whereas the 2^53 band is predictable from the magnitude alone.

The change is a widening only, so no existing plugin is affected unless it was already
reading a truncated value. **Worth announcing to script authors:** a column past 2^53
now arrives as a *string*, so a plugin doing arithmetic on one without checking `type()`
gets a Lua "attempt to perform arithmetic" error instead of a silently wrong number.
Erroring is the better failure, but it is a visible change.

**Also fixes a read-after-free** the string band would otherwise have inherited:
`DatabaseColumnValues`' text arm declared its `CString` inside the `switch` arm and
assigned it to the `COleVariant`, which on the port borrows the buffer
(`platform/mfc_ui.h`); the `CString` died at the end of the arm, while
`COleSafeArray::PutElement` — the thing that actually copies the string — runs after the
`switch`. Short values survived on SSO luck. The declaration is hoisted to the loop body.

Covered by nine cases in `tests/test_database.cpp`, mutation-verified per site (each of
the four `sqlite3_column_int64` calls reverted individually, and both band boundaries
moved in both directions). A Windows run is not strictly required — the four readers are
shared code with no platform arms — beyond confirming `CString::Format("%I64d", …)`
behaves as it always has there, which is a formality since that is MSVC's native
modifier.

## 81 — `SaveModified` half-closes every world it has already prompted (port only: `4d7168f`)
**Bug:** `CMUSHclientDoc::SaveModified` performs the close side effects —
`m_bWorldClosing = true`, `MXP_Off(true)`, the `on_world_close` script — *before*
returning its answer. `CDocTemplate::SaveAllModified` walks the open documents calling
`SaveModified()` on each, so cancelling at world 3 leaves worlds 1 and 2 with their
close script already run and `m_bWorldClosing` stuck true, with nothing closed.

This **corrects the premise** the port's own fix was written against: the D-09 brief
said "Windows parity — prompt all worlds first, and if the user cancels, nothing
closes", but stock Windows has the same half-closed state the port had. The structural
shape (`SaveAllModified()` then `CloseAllDocuments()`) is Windows'; the ordering defect
inside `SaveModified` is upstream's.

**Fix (port only):** `SaveModified()` was split into `BOOL QueryCloseWorld()` (the
connection warning, the `m_bSaveWorldAutomatically` save, the variables-only prompt and
`CDocument::SaveModified()`, same code in the same order) and `void CommitCloseWorld()`
(`m_bWorldClosing`, `MXP_Off`, the close script), with
`SaveModified() = QueryCloseWorld() ? (CommitCloseWorld(), TRUE) : FALSE`. The macOS
quit hook is the only caller that uses the two halves separately, so **Windows behaviour
is unchanged** — it would get the same fix for free by routing its quit through the two
halves, which is deliberately not done here. This is therefore an improvement over
upstream rather than parity with it.

The one side effect the query pass can still have is a save the user asked for (or
configured), through `DoSave`, which fires `on_world_save` and `ON_PLUGIN_WORLD_SAVE` —
identical to Windows, where the same `DoSave` runs from inside `SaveAllModified`.
Confirmed **absent** from the query pass: `m_bWorldClosing`, `MXP_Off` (and therefore
`OnMXP_Stop` + `ON_PLUGIN_MXP_STOP`), the `on_world_close` script,
`SendToAllPluginCallbacks(ON_PLUGIN_CLOSE)`, `CPlugin::SaveState`,
`PersistBeforeTeardown` and `CloseLog`. See `docs/WINDOWS-VALIDATION.md`
§4c-quadragies for the two Windows deltas that need eyeballing.

## 82 — Convert > Unix->DOS / DOS->Unix destroys a Mac-line-ending document (not fixed; the port diverges)
**Bug:** `CTextView::OnConvertUnixtodos` and `OnConvertDostounix` (`TextView.cpp:283-350`)
are unconditional byte filters over the whole `CEdit` buffer: `OnConvertUnixtodos` skips
every `\r` and emits `\r` only ahead of a `\n`; `OnConvertDostounix` skips every `\r`. A
notepad holding a classic-Mac (CR-only) document therefore loses **every line break in
the file** to either command, collapsing it to a single line — and File > Save then
writes that back. The Convert menu offers all four conversions unconditionally, with
nothing indicating which one matches the document, no confirmation and no undo of the
file.

**Not reproduced on Windows** (no Windows build here); read from the canonical source.
**Not fixed here**, because the macOS port does not have the defect and fixing it would
be a Windows behaviour change on its own: the wx control holds LF and nothing else, so
the port carries the line-ending style out of band on the document
(`CTextDocument::m_iLineEndings`, inside `#ifndef _WIN32`) and the four ops retag it
rather than rewriting bytes. That divergence, and the two behavioural differences that
follow from it, are recorded in CLAUDE.md under the notepad line-ending representation —
this row exists so the upstream defect is on record for whoever patches upstream.

## 83 — `world.ReplaceNotepad` returns nil in Lua and a boolean in every other engine (`203c0fe`)
**Bug:** `L_ReplaceNotepad` calls `lua_pushboolean` with the result of
`CMUSHclientDoc::ReplaceNotepad` and then `return 0;` — zero result fields — so the
value is discarded and every Lua caller sees nil. It was the only body in
`lua_methods.cpp` that pushes a result and then returns 0 (`L_MtSrand`'s `lua_pushnil`
is the `lua_next` iteration idiom, not a result). Meanwhile `mushclient.odl` declares
`[id(61)] boolean ReplaceNotepad(BSTR, BSTR)` and the automation dispatch map registers
it `VT_BOOL`, so Python, Perl, JavaScript, Tcl and Ruby all receive the boolean. Its two
siblings on the same underlying body, `AppendToNotepad` and `SendToNotepad`, return
theirs to Lua.
**Fix:** `return 1;`. Covered by `LuaSurface.NotepadFamilyWithNoNotepadOpen` (false when
the title does not exist) and `LuaSurface.NotepadFamilyWithANotepadOpen` (true on
success, plus a check that the replace actually happened); mutation-verified both ways.
This is Lua-visible on Windows too — see `docs/UPSTREAM-DIVERGENCES.md` §13.

## 84 — `GetLineInfo(n).time` truncates the timestamp through `(int)` (`43e27b8`)
**Bug:** the table form of `world.GetLineInfo` built its `time` field as
`(int) pLine->m_theTime.GetTime ()`. `CTime::GetTime()` returns a 64-bit `time_t`, so
the cast discards the high half: from 2038-01-19 03:14:08 UTC every line's `.time` comes
back negative. Present since the first commit and untouched by the port. A Lua number is
a double, which represents any realistic date exactly, so the narrowing bought nothing —
the value was widened to double immediately afterwards by the `MakeTableItem` overload.
**Fix:** widen with `(double)` instead of narrowing with `(int)`. The explicit cast is
kept rather than dropped entirely because it also pins the intended
`MakeTableItem (…, const double)` overload against the implicit `COleDateTime(double)`
conversion — today the standard conversion wins and the resolution is unambiguous, but an
added overload could silently change it.
**Not affected:** the type-code form, `world.GetLineInfo(n, 9)`, goes through
`CMUSHclientDoc::GetLineInfo` (`methods_info.cpp`), which builds a `VT_DATE` VARIANT from
`COleDateTime` and does no narrowing. Only the Lua table form was ever wrong — worth
stating because the decision text suggested testing through the type-code form, which
cannot observe this bug.
**Script-visible on both platforms:** any plugin comparing `.time` against its own clock
changes answer for post-2038 lines. No realistic plugin can have been relying on the
negative value. Covered by `LuaScripting.GetLineInfoTimeSurvives2038` in
`tests/test_lua_scripting.cpp`, which stamps a line with 2038-01-20 and asserts the exact
positive value; mutation-verified (restoring `(int)` fails it with `-2.1474e+09`).

## 85 — Vetoing an atomic MXP tag leaves the tag on the active list (`3095c2a`)
**Bug:** `MXP_StartTag` records a tag as open — an `m_ActiveTagList` entry plus a
`START_TAG` marker style — *before* it calls `MXP_OpenAtomicTag`, which is where a
built-in tag's veto is asked (`OnMXP_OpenTag` / `OnPluginMXPopenTag`,
`mxpOpenAtomic.cpp:73-84`). The veto cannot be asked earlier because the argument list it
passes is only rebuilt inside `MXP_OpenAtomicTag`. A **custom element's** veto
(`mxpStart.cpp:162-176`) returns before any of that bookkeeping exists, so the two paths
disagreed: a vetoed built-in tag stayed on the active list. `mxpOpenAtomic.cpp:84`
carried the identical comment to `mxpStart.cpp:175` — "they didn't want to go ahead with
this tag" — but only the custom path achieved it. The styling half already worked (the
run subsequent text uses is created with pre-tag attributes before the call and the
attributes are restored afterwards, so a vetoed `<font>`/`<color>` left no colour); only
the bookkeeping leaked.

**Four observable consequences, all confirmed by test and all now fixed:**

1. A spurious `StartNewLine`: `</p>` found the suppressed `<p>` still open, so
   `MXP_CloseAtomicTag`'s `MXP_ACTION_P` arm ran and broke the line. `</ul>` / `</ol>`
   do the same when the line is non-empty.
2. The dangling tag counted toward the every-100 outstanding-tag warning — and consumed
   the `m_iLastOutstandingTagCount` marker, so the warning for the *genuine* hundredth
   tag was then swallowed.
3. A close-tag callback (`OnMXP_CloseTag` / `ON_PLUGIN_MXP_CLOSETAG`) fired for an open
   that was suppressed.
4. A vetoed `<send>` nested in a hyperlink closed "early": `MXP_CloseAtomicTag`'s
   SEND/HYPERLINK arm rewrote every enclosing style's action from the suppressed marker
   onwards, substituting `&text;` with the *inner* text. Measured:
   `<a href="go &text;">click <send>me</send> now</a>` with `send` vetoed produced an
   anchor whose later runs sent `go me` while its earlier runs sent `go click me now` —
   one link, two commands.

**Fix — unwind on veto, not reorder.** `MXP_OpenAtomicTag` gained an optional
`bool * pbVetoed`; the built-in call site (`mxpStart.cpp:341`) passes it and undoes the
active-tag record, the `m_iLastOutstandingTagCount` marker and the `START_TAG` marker
style. Every added line is inside `if (bVetoed)`, so the non-vetoed path — which is
essentially all MXP traffic — is byte-for-byte unchanged. Consequence 4 needed no code of
its own: with no active-tag entry, `MXP_EndTag` never reaches `MXP_CloseTag`. The
custom-element call site (`mxpStart.cpp:594`, one call per expanded atom) deliberately
does **not** pass the out-param: an atom is not the tag, and `</mytag>` must still close.
The unwind is additionally guarded on the active-tag tail actually being the tag being
unwound.

**Why not the alternatives** (both checked, both rejected): *moving the veto earlier*
reorders code that runs on every tag and changes what the callback is handed —
`MXP_OpenAtomicTag` rebuilds the arguments into the single-quoted `name='value'` form,
where `MXP_StartTag` would pass the raw tag text, and `plugins/MXP_statusbar.xml:42`
parses that argument with a regex that *requires* the single quotes, so the reorder
breaks a shipped plugin (confirmed at HEAD); it would also move the veto ahead of the
`&entity;` substitution pass, handing handlers unsubstituted values. *Asking the veto
from inside `MXP_StartTag`* changes the custom-element path's semantics too, and would
collapse the two asks a plugin gets for `<!ELEMENT mytag '<B>'>` into one.

**Behaviour change, deliberate:** a `</p>`/`</b>`/... after a vetoed open now reports
`wrnMXP_OpenTagNotInOutputBuffer` in the MXP debug window. That is the *correct* new
behaviour and is exactly what a vetoed **custom** element already did, so it is parity
being restored, not a new wart. Note also that the two veto conventions are opposite: a
**plugin**'s `OnPluginMXPopenTag` vetoes by returning **false** (`MXP_StartTagScript`
negates it), while a **world script**'s `OnMXP_OpenTag` vetoes by returning **true**.

**Not fixed, same shape:** the `"No closing \";\""` abort (`mxpStart.cpp:302-309`) still
leaves the same dangling tag; it is a separate early return and was deliberately not
coupled to this fix. `mxpCloseAtomic.cpp:31-104` also still rewrites enclosing styles'
actions for any *legitimately* nested `<send>`/`<a>` — upstream behaviour, and with the
veto fixed there is no longer a suppressed open that can trigger it spuriously.

Covered by the atomic-veto battery added in `4f4b667`, which closed a real gap: the file
had a Lua fixture whose `OnOpen` already returned `name == 'suppressme'`, but nothing
ever sent `<suppressme>`, so the atomic-veto branch had **zero** coverage at HEAD. See
`docs/WINDOWS-VALIDATION.md` §4c-duodequadragies.

## 86 — REFUTED: the two UTF-8 click fix-ups compose, they do not disagree (this commit)

**The claim was:** `CMUSHView::calculate_line_and_column` corrects a click that lands
mid-character twice, and the two corrections walk in opposite directions. The first,
right after `col--` makes the column zero-relative (`mushview.cpp:1927-1932`), walks
**forward** to the next character's lead byte:

```cpp
  if (pDoc->m_bUTF_8)
    while ((col > 0) && (col < pLine->len) && (pLine->text [col] & 0xC0) == 0x80)
      col++;
```

while the half-way arm below it (`:1943-1947`) walks **backward** with `col--`. So — the
reasoning went — the same click resolves to different characters depending on which arm
runs, and a click just left of a multi-byte character selects one too far right. The
entry proposed a one-token fix (`col++` → `col--`), and recorded that it needed a Windows
run before anyone made it, because `CMUSHView` is never constructed on the port.

**The run has now happened, and the answer is that there is no defect.** The two walks
**compose**; each normalises the byte the step before it produced, in the direction that
step went.

The loop measures the boundary **at** `col` and then advances `col` past the character
there, so on exit `col` is the boundary *after* the character the click landed in, and
`col--` is "that boundary, minus one **byte**". For a single-byte character that is
already the next character's lead byte; for a multi-byte one it is a continuation byte,
and the forward walk is exactly what normalises it to the same thing. The half-way arm
then steps back from "the next character" to "this character", and its backward walk
normalises *that*.

**Measured** on Windows with `"Aé"` + `"B"` — bytes `A`=0, lead=1, continuation=2,
`B`=3 — against the single-byte controls either side:

| arm | click in `A` | click in `é` | click in `B` |
|---|---|---|---|
| `bHalfWay = false` | 1 | **3** | 4 |
| `bHalfWay = true`, left half | 0 | **1** | — |
| `bHalfWay = true`, right half | 1 | **3** | — |

Every row is the **same rule** for one-byte and two-byte characters, which is the only
thing the asymmetry could have broken. The half-way row is also exactly what a drag
selection wants: anchor before the character when the click is in its left half, after it
when the click is in its right.

**The proposed fix was tried and measured too, and it is a regression.** Changing the
forward walk to `col--` moves the left-half answer from 1 to 0, so a selection started in
the left half of an accented character would begin one character too far left — the very
defect the entry was trying to remove, introduced in its place.

**How it was tested without a GUI**, since "needs a Windows run" turned out to mean "needs
five lines nobody had written": `calculate_line_and_column` is public, its inputs are the
document's line list and font metrics, and the only Windows object it insists on is a
`CClientDC` — which, given a **NULL** `CWnd*`, is the screen DC and needs no window.
`CMUSHView`'s constructor is protected (`// create from serialization only`) but
`RUNTIME_CLASS(CMUSHView)->CreateObject()` is the framework's own way past that, and
`CDocument::AddView` attaches the result safely because `CWnd::GetParentFrame` answers
NULL for a windowless view rather than asserting. The document must be destroyed **before**
the view (`~CDocument` runs `DisconnectViews`, which detaches without deleting);
`RemoveView` is the tidy-looking call and the wrong one, because removing the last view
runs `OnCloseDocument` — `delete this` on an auto-delete document.

`tests/win/test_gap_coverage.cpp`'s `ClickColumnR6` pins both arms, controls included, so
the refutation stays refuted.

**Recorded as `docs/DECISION-QUEUE-2026-08.md` D-26; that row can be closed.**

## 87 — lsqlite3's int64 round-trip test is vacuously true, so the module rounds silently (`fbb6695`)
**Bug:** the bundled Lua `sqlite3` module decides whether an INTEGER column survives
as a Lua number by round-tripping it through a `lua_Number`. The intent is plain from
the shape — push a number if the value is held exactly, else push the exact decimal
text — and all three sites spell it the same way (`vm_push_column`,
`db_last_insert_rowid`, and the `sqlite3_value` variant `db_push_value`):

```c
sqlite_int64 i64 = sqlite3_column_int64 (vm, idx);
lua_Number   n   = (lua_Number) i64;
if (n == i64)
    lua_pushnumber (L, n);
else
    lua_pushlstring (L, (const char *) sqlite3_column_text (vm, idx), …);
```

In `n == i64` the usual arithmetic conversions widen the `sqlite_int64` operand to
`double` before comparing, so the expression reads `(double) i64 == (double) i64` and
is **always true**. The `else` branch is unreachable on any platform with IEEE
doubles, and a value a double cannot hold comes back silently rounded: a column
holding `9007199254740993` (2^53+1) reads as the Lua number `9007199254740992`.
Verified rather than inferred — the test that used to pin the defect asserted exactly
that value.

**Fix:** round-trip back through `sqlite_int64`, which is what the test was meant to
do, behind a named helper (`lsqlite_double_holds_int64_exactly`) shared by the three
sites. The range check comes **first**, because `(double) INT64_MAX` rounds *up* to
exactly 2^63 and casting that back to an integer is undefined behaviour — arm64
saturates to `INT64_MAX`, which would call the value exact and reinstate the bug at
the one input most likely to be tested, while x86-64 yields `INT64_MIN` and would
not. Comparing against 2^63 before the cast removes the undefined behaviour and the
platform dependency together.

**Why it is worth having:** this is the app's *other* SQLite surface. A script
reaches these three functions through `sqlite3.open_memory()`, where it reaches
`world.Database*` for the same data by another route — and the two disagreed. Entry
80 above fixed the `world.Database*` readers' 32-bit truncation, and `e24ab4b` then
moved their middle band from a 2^53 magnitude cap to the same round-trip test; this
closes the last of the gap, so both surfaces now band identically. The shared
reasoning and the band boundaries live in `scripting/db_int64.h`.

**Script-visible, and deliberately so.** A value above 2^53 that a double cannot hold
now arrives as a lossless decimal string where it used to arrive as a wrong number.
That is a type change for anything reading such a column, which is why it was taken
as a decision rather than folded into entry 80's fix (the branch report for the
`world.Database*` realign raised it as an open question and left the module alone;
the call was made afterwards, and this is it). Covered by
`DatabaseTest.Lsqlite3ModuleBandsExactlyAsWeDo`, which asserts both halves — exactly
representable values stay numbers, including 2^60 and `INT64_MIN`; non-representable
ones are the string — and pins `INT64_MAX` specifically, since that is the input the
range guard exists for. Mutation-checked: restoring the vacuous test fails it.

**Vendored code.** `lsqlite/lsqlite3.c` is third-party and is in `MUSHclient.vcxproj`,
so the defect ships to Windows too and the fix is a local patch that upstream
lsqlite3 does not carry. See `docs/WINDOWS-VALIDATION.md` §4c-sexquadragies.

## 88 — `Get_XML_date` tells `mktime` that summer is standard time, so every parsed hour is an hour late (this commit)

**Bug:** the function's last line builds the result as

```cpp
tValue = CTime (iYear, iMonth, iDay, iHour, iMinute, iSecond, 0);
```

The trailing `0` is MFC's `nDST`, which `CTime` passes straight through as
`tm_isdst` to `mktime`. `0` does not mean "no opinion" — it is a positive claim
that the wall-clock time being converted is **standard** time. For any date
inside a daylight-saving period the CRT therefore converts it as standard time,
producing a `time_t` an hour earlier than intended; reading a component back
through `localtime` — which is what `CTime::GetHour()` does — returns the hour
one **later** than the string said.

Measured rather than inferred, on Windows on 2026-08-21 with
`tests/test_xmlparse.cpp`'s `XmlAttrR5.DateParseArms`:

| attribute | parsed back as |
|---|---|
| `2020-05-06 07:08:09` | hour 8 |
| `2020-05-06` (no time) | hour 1, not 0 |
| `2020-05-06 01` | hour 2 |

Every hour in the fixture is off by exactly one, and the shift is the local
zone's DST offset — so the size of the error depends on the reader's timezone
and the time of year, and it is zero for half the year, which is a good part of
why it has survived.

**Fix:** pass `-1` ("work it out"), which is `CTime`'s own default and what the
three other `CTime` component constructors in this tree already get by not
naming the parameter. `xmlparse.cpp:1592` is the only site in the tree that
passes an explicit `nDST`.

**Where it shows:** `Get_XML_date` has one caller that reaches user data —
`xml_load_world.cpp:2472`, a plugin's `date_written`, which the Plugins dialog
displays. Cosmetic in effect, but it is a value read straight back out of a file
the client itself wrote, so a plugin's date drifts by an hour every time it is
loaded during summer time.

**Why the port never saw it:** `platform/mfc_time.h`'s `CTime` names the
parameter `int /*dst*/` and always sets `tm_isdst = -1`. The port has therefore
been giving the correct answer all along, and this is one of the cases where
Windows and the port genuinely differed and the *port* was right. It took the
Windows unit-test target (`docs/WINDOWS-VALIDATION.md` §10j, §10k) to see it: the
test that caught it is not new and had been green on macOS for months.


## 89 — `ExtractDirectory` looks only for a backslash on Windows, so a plugin path with forward slashes resolves `$PLUGINDIR` to the current directory (this commit)

`utilities_shared.cpp`'s `ExtractDirectory` had two arms. The POSIX one searched
for **both** separators and took the later, with the comment "also accept `'\'`
in case of cross-platform world files". The Windows one searched for a backslash
only:

```cpp
int iSlash = str.ReverseFind ('\\');
if (iSlash == -1)
   str = ".\\";
else
  str = str.Left (iSlash + 1);
```

So `ExtractDirectory("plugins/images/x.png")` answered `".\"` on Windows and
`"plugins/images/"` on the port.

**Why it is a defect rather than a platform difference: the function's own
caller already expects forward slashes.** `xml/xml_load_world.cpp` decides
whether a plugin filename is relative by asking whether it contains **either**
separator:

```cpp
if (strFileName.Find ("\\") == -1 &&
    strFileName.Find ("/") == -1)
  strFileName = CString (Make_Absolute_Path (App.m_strPluginsDirectory)) + strFileName;
```

Six lines later it hands that same string to `ExtractDirectory` to set
`m_CurrentPlugin->m_strDirectory`. A path with forward slashes is therefore a
shape the loader **expressly** anticipates, and the very next thing it does with
it mis-parses it.

**Where it shows:** `m_strDirectory` is what `$PLUGINDIR` expands to
(`xml_load_world.cpp`, the `::Replace` block), so a plugin loaded from a path
written with forward slashes had every `$PLUGINDIR` resolve to the *current
directory* instead of its own. Files the plugin ships beside itself (sounds,
images, `require`d Lua) are then looked for in the wrong place. Forward slashes
get into world files the ordinary way: a world file authored or edited on macOS
or Linux, one hand-written, or one produced by a script.

**Fix:** use the POSIX arm's search on both platforms, looking for both
separators and taking the later. The only thing left under `#ifdef` is the
no-separator fallback, which still names the platform's own separator (`".\"`
against `"./"`).

The widening cannot break a path the old code got right: for any string with no
`'/'` in it the two searches return the same index, and a string that *does*
contain `'/'` is exactly the case that was wrong. Win32 has accepted `'/'` as a
path separator since NT, so the answer is usable as a path either way.

**Why the port never saw it:** the port only ever compiled the other arm. The
divergence came from widening the POSIX side without carrying the reasoning back
across the `#ifdef`, so the comment explaining *why* both separators matter
ended up inside the one arm that no longer needed explaining.

**Test:** `tests/test_utilities.cpp`'s
`Utilities.ExtractDirectoryAcceptsBackslashSeparators`. It needed no new case to
catch this; two of its four assertions failed the moment the suite was enabled
on Windows. It now asserts **both** mixed orders instead of one. Only
`a\b/c.png` distinguishes the arms: a backslash-only search answers `a/b\c.png`
correctly by accident, because there the backslash *is* the later separator. The
pair pins the rule rather than one example of it. The fourth assertion, the
no-separator fallback, is the one genuinely platform-specific line and is the
only part still under an `#ifdef`.


## 90 — A Lua MXP handler that returns nothing suppresses every tag, and every error (this commit)

`ExecuteLua` hands the handler's answer back through a `bool *`, and writes a
default first:

```cpp
if (result)
  {
  *result = true;                  // <-- the default
  if (lua_gettop (L) > 0)          //     only overwritten when the handler
    ...                            //     actually returned something
  }
```

That default is correct, and deliberate: the parameter answers **"keep
processing?"**, and "a handler that returns nothing keeps processing" is a
reference contract all six languages conform to, asserted for each of them in
`platform/engines_selftest.cpp`.

The defect is that **two call sites read that same value as a different
question**:

| call site | what it reads `true` as | COM arm, handler returned nothing | Lua arm |
|---|---|---|---|
| `mxpStart.cpp` `MXP_StartTagScript` | **veto the tag** | `VT_EMPTY` → `return false`, proceed | **veto** |
| `mxpError.cpp` `MXP_error` | **suppress the error** | `VT_EMPTY` → carry on | **suppress** |

Both COM arms can tell "said nothing" from "said true", because a handler that
returns nothing leaves the `COleVariant` as `VT_EMPTY`; the comment at the first
of them is explicit about the intent — "if the function returns a non-zero
result, don't go ahead". The Lua arm has no such signal, so it acts on a default
that was written to answer a different question.

**Why it stayed hidden.** Every existing test of these callbacks returns an
explicit boolean. The case that fails is the one a real script is most likely to
contain — the one-armed veto:

```lua
function OnMXPopenTag (name, args)
  if name == 'u' then return true end
end
```

That is the natural way to write "suppress `<u>`", and for every *other* tag it
falls off the end of the function. The result was that a world with such a
handler suppressed **all** MXP markup, while the identical handler written in
VBScript suppressed only `<u>`. At the other site a handler that merely logs —
no `return` anywhere, which is what a logging handler looks like — silenced the
`ON_PLUGIN_MXP_ERROR` broadcast to every loaded plugin as well as the
debug-window display.

**A second defect at the same two lines.** Both sites declared their flag as a
bare `bool result;`. `ExecuteLua` has four early returns that never reach the
assignment — no Lua state, unknown dispid, the handler's name not found, and a
run-time error in the handler — so a handler that *threw* left the caller
reading uninitialised stack. At `MXP_StartTagScript` that means a broken handler
vetoed tags at random.

**The fix is additive, and that is deliberate.** `*result` must not be changed
to default false: that would break the six-language keep-processing contract
above. What the two MXP sites actually lack is the ability to tell "said
nothing" from "said true". So `ExecuteLua` gained an optional
`bool * pbReturnedAValue`, set from `lua_gettop (L) > 0` in Lua's own body, and
the two sites honour `result` only when it says the handler really answered —
which is exactly what `VT_EMPTY` gives their COM arms.

Nothing else moves: the six plugin call sites do not pass the new parameter, the
keep-processing contract is untouched, and the five embedded engines return
before Lua's body and never set it, so with the flag seeded `true` at the call
sites their behaviour is bit-for-bit what it was. Both sites also seed `result`
itself, which removes the uninitialised read.

**Tests:** `MxpVetoR5.OpenTagHandlerThatReturnsNothingDoesNotVetoTheTag`
(`tests/test_mxp.cpp`) uses the one-armed handler above and asserts both halves
in one test — `<b>` opens, `<u>` does not — so neither can pass with a handler
that is never consulted.
`PluginCallback.WorldErrorHandlerThatReturnsNothingStillTellsThePlugins`
(`tests/test_plugins.cpp`) checks that the world's own error handler running does
not stop the plugins hearing about the error, and asserts the world handler ran
so the plugin's silence cannot be blamed on there having been no error. Both
were confirmed to fail before the fix.

**Still open: the five embedded engines on the port.** They are unchanged by
this commit and still wrong in the same way — a Python `OnMXP_OpenTag` returning
`None` still vetoes every tag. Each writes its own default rather than reporting
whether the handler answered: `py_engine.cpp:552,555` (`*result = true`, then
`Py_None` → `true` under the comment "no explicit return → keep going"),
`perl_engine.cpp:411`, `js_engine.cpp:579`, `tcl_engine.cpp:365` (the non-`TCL_OK`
arm) and `ruby_engine.cpp:512` (`Qnil` → `true`). Each needs one line: set
`pbReturnedAValue` from whatever it already uses to spot the no-return case.
Verification is `--engines-selftest`, which needs a macOS GUI build, so it is not
part of this commit. Windows is unaffected either way — all five are
`#ifndef _WIN32`.

## 91 — `world.Save (name, true)` on a never-saved world throws instead of saving (this commit)

`L_Save` (`scripting/lua_methods.cpp`) implements the Save-**As** flag by
remembering the document's path name, saving under the new one, and putting the
old one back:

```cpp
CString strOldName = pDoc->GetPathName ();
...
lua_pushboolean (L, pDoc->Save (my_checkstring (L, 1)));
if (bSaveAs)
  pDoc->SetPathName (strOldName, FALSE);
```

For a world that has never been saved, `GetPathName ()` is `""` — and
`CDocument::SetPathName ("")` is not a quiet no-op. MFC fully qualifies the name
before storing it, `AfxFullPath` fails on an empty string, and the failure
branch is:

```cpp
if (AfxFullPath (szFullPath, lpszPathName) == FALSE)
  {
  ASSERT(FALSE);
  AfxThrowFileException (CFileException::badPath, -1, lpszPathName);
  }
```

(`doccore.cpp:291-297` in MFC 14.44.) So the restore raises
`CFileException::badPath` **out of a Lua C function**, past `lua_pcall`'s C
frames. The file on disk is written correctly first — the throw is in the
book-keeping after it — which is what makes this look like an intermittent
"scripting error" rather than a save failure.

Reachable in the product: File ▸ New World, then any plugin or script line that
calls `world.Save (filename, true)`. A world opened from a file has a path name
and is unaffected, which is why it has survived.

**Fix:** only restore a name there is one to restore.

```cpp
if (bSaveAs && !strOldName.IsEmpty ())
  pDoc->SetPathName (strOldName, FALSE);
```

With nothing to go back to the document keeps the name just written, which is
what File ▸ Save As does for an untitled document.

**Found by** the Windows unit-test target: `LuaMarshal.SaveWritesWorldFile` and
`SweepR4.WorldSaveRoundTripWithEverything` both call `world.Save (path, true)`
on a fresh `CMUSHclientDoc`. The port never saw it because its `CDocument` shim
accepts an empty path name. The frame came from the assertion-stack hook
described in `docs/WINDOWS-VALIDATION.md` §10z; before that the run reported
only `doccore.cpp(293) : Assertion failed!`, which names a line in MFC and
nothing about the caller.

## 92 — The "bytes sent" statistic is counted twice

**Where:** `doc_shared.cpp`'s `CMUSHclientDoc::SendPacket` and
`worldsock.cpp`'s `CWorldSocket::OnSend`, both as of `249534f^`.

`m_nBytesOut` is incremented in **two** places on the same data:

```cpp
// SendPacket, once per call, for the whole buffer
m_pSocket->m_outstanding_data.append (lpBuf, nBufLen);
m_pSocket->OnSend (0);   // in case FD_WRITE message got lost, try to send again
m_nBytesOut += nBufLen;

// CWorldSocket::OnSend, again, for every chunk that drains
count = Send (m_outstanding_data.data (), m_outstanding_data.length ());
if (count != SOCKET_ERROR)
  pDoc->m_nBytesOut += count; // count bytes out
```

`SendPacket` calls `OnSend (0)` itself, so both run on every send. Once the
buffer has fully drained the two contributions are equal, and the figure is
exactly **double** the bytes that went out. (It is not a fixed factor while data
is still queued, which is why it never looks like an obvious 2×.)

Everything that reports the number is affected: the world-properties Info page's
`Sent: %I64d bytes` (`dialogs/world_prefs/configuration.cpp:552`), `world.GetInfo
(217)` and its Lua twin (`scripting/methods/methods_info.cpp:633`,
`scripting/lua_methods.cpp:3737`), and `world.Debug`'s `Sent:` line
(`world_debug.cpp:1397`). **Bytes RECEIVED is counted once and is right**, so a
world's own statistics page shows a plausible-looking 2:1 out:in ratio for a
session that was actually about 1:1 — which is exactly why nobody notices.

`chatsock.cpp:501` counts the chat socket's bytes once and is unaffected.

**Fix:** count where the engine hands the data over, not again as it drains.
This is what the transport relocation did (`doc_shared.cpp:1785` carries the
note), so this build now reports the true figure on both platforms — a
behavioural difference from stock 5.06, registered as
`docs/UPSTREAM-DIVERGENCES.md` § 20.

**Found by** reading `CWorldSocket::OnSend` out of git history while writing
`tests/win/test_win_transport.cpp`, to check whether the adapter had reproduced
its one-`Send`-per-`FD_WRITE` structure faithfully. It had. The byte counter
beside it had not, deliberately, and nothing recorded that.
`WinTransport.BytesSentAreCountedOncePerSendPacket` pins the fixed behaviour.

## 93 — "Save notes to file" is an OPEN dialog, so it truncates without asking

**Where:** `dialogs/world_prefs/prefspropertypages.cpp`, `CPrefsP11::OnSaveNotes`
(the world-properties Notes page, the **Save…** button).

```cpp
CFileDialog filedlg (TRUE,   // loading the file
                     "txt",
                     "",
                     OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT,
                     ...
filedlg.m_ofn.lpstrTitle = "File to save notes into";
...
f = new CFile (filedlg.GetPathName (), CFile::modeWrite | CFile::modeCreate);
```

`CFileDialog`'s first parameter is `bOpenFileDialog`, and it is `TRUE`. So the
panel is an **Open** dialog wearing a save dialog's title, and
`OFN_OVERWRITEPROMPT` — which the author asked for explicitly — is inert,
because an open panel has no overwrite confirmation to give. `CFile::modeCreate`
then truncates. **Choosing an existing file destroys it with no warning.**

It is a copy-paste: `CPrefsP11::OnLoadNotes` sits directly above with the same
four arguments and the same `// loading the file` comment, which came along with
them.

This is the only site in the tree shaped this way. Every other
`OFN_OVERWRITEPROMPT` passes `FALSE` (`chatsock.cpp:1082`, `doc.cpp:740`,
`evaluate.cpp:850`, `mushview.cpp:3997`, `plugins.cpp:1051`,
`prefspropertypages.cpp:1156` and `:8019`) or derives it from a `bLoad`
parameter that is `FALSE` on the save arm (`prefspropertypages.cpp:7238`,
`scripting/lua_utils.cpp:750`).

**Measured on the running client**, not inferred: driving Game ▸ Configure ▸
Notes ▸ Save… and reading the resulting panel's controls gave

```
FILE PANEL title: 'File to save notes into'
   button id=1      '&Open'          <- before
   button id=1      '&Save'          <- after
```

**Fix:** `FALSE`. The overwrite prompt then works because it is finally attached
to a dialog that implements one.

**Note for the port.** The macOS symptom was *different and more visible* — an
`NSOpenPanel` will not return a name that does not exist, so the button could
never create a new file — and `platform/mfc_ui.h` compensated for it in the shim
with an `openOrCreate` heuristic ("an open-style dialog whose flags say the file
need not exist and that it should warn before overwriting"). That heuristic was
the *blocker stated at the wrong layer*: it patched the shim for a typo one
level up, and it could not have fixed Windows, where the open panel happily
returns new names and only the warning is lost. With the call site corrected the
heuristic is unreachable; it is left in place with a comment saying so.

## 94 — MXP `</p>` uses the line a gagging trigger just freed

**Where:** `mxp/mxpClose.cpp`, `CMUSHclientDoc::MXP_CloseTag`.

`MXP_CloseTag` opens by scanning backwards through `m_LineList` for the matching
start tag, and caches four things about where it found it:

```cpp
CLine  * pLine;    POSITION oldlinepos;
CStyle * pStyle;   POSITION oldstylepos;
```

It then dispatches the tag through `MXP_CloseAtomicTag`, and afterwards uses all
four again without re-checking any of them:

```cpp
pLine->styleList.RemoveAt (oldstylepos);
...
m_pCurrentLine->styleList.AddTail (pStyle);
pStyle->iFlags &= ~START_TAG;
```

For `</p>` the dispatch is (`mxp/mxpCloseAtomic.cpp`)

```cpp
case MXP_ACTION_P:
   m_bInParagraph = false;
   StartNewLine (true, 0);
```

`StartNewLine` with a hard break runs `ProcessPreviousLine`, and a trigger with
**omit from output** does this (`ProcessPreviousLine.cpp`):

```cpp
delete pLine;                // delete contents of tail iten
m_LineList.RemoveTail ();    // get rid of the line
```

The line `MXP_CloseTag` is holding is the **tail** — that is where its backwards
scan began — so it is the first one gagged away, taking its `styleList` and
therefore `pStyle` with it (`~CLine` deletes every style it owns). Everything
after the dispatch then reads and writes freed memory.

**Reachable from the MUD**: any server sending `<p>…</p>` in secure mode to a
world with a gagging trigger. `p` is secure-only (`mxp/mxpinit.cpp` gives it no
`TAG_OPEN`), so a plain-mode world is safe; a Discworld-style MXP world is not.

**Measured**, with a Debug CRT that fills freed memory with `0xDD`:

```
*** ACCESS VIOLATION (0xC0000005) -- writing 0xDDDDDDDD
   0  CPtrList::RemoveAt
   1  CMUSHclientDoc::MXP_CloseTag   mxp/mxpClose.cpp:337
   2  CMUSHclientDoc::MXP_EndTag     mxp/mxpEnd.cpp:111
   3  CMUSHclientDoc::MXP_collected_element
   4  CMUSHclientDoc::Phase_MXP_ELEMENT
   5  CMUSHclientDoc::DisplayMsg
```

**Fix:** re-find the line before touching it, and return if it has gone — the
same liveness check `aca2478` gave the Plugins dialog for a stale `CPlugin*`.
The re-scan is bounded by how far back the opening tag was, which the forward
scan already measured, so it costs no more than the search that produced the
pointer.

Nothing leaks on the bail-out path: `~CLine` released the styles and their
actions when it went.

**Shared code — the same defect ships on macOS**, where a sanitizer would call
it a heap-use-after-free rather than an access violation.
`MXPCloseR6.ParagraphCloseSurvivesAGaggingTrigger` in `tests/test_mxp.cpp` runs
on both.

## 95 — "Cancel" on world properties is not a cancel

**Where:** `dialogs/world_prefs/configuration.cpp`,
`CMUSHclientDoc::LoadPrefsP23` (the Chat page's load half).

```cpp
  // take reasonable defaults

  if (m_strOurChatName.IsEmpty ())
    m_strOurChatName = m_name;      // try login character name

  if (m_strOurChatName.IsEmpty ())
    m_strOurChatName = DEFAULT_CHAT_NAME;  // still nothing? blech!

  if (m_IncomingChatPort == 0)
    m_IncomingChatPort = DEFAULT_CHAT_PORT;  // won't use port 0

  if (m_cChatForegroundColour == m_cChatBackgroundColour)
    {
    m_cChatForegroundColour = RGB (255, 0, 0);
    m_cChatBackgroundColour = RGB (0, 0, 0);
    }

  // show whether we really are accepting calls
  m_bAcceptIncomingChatConnections = m_pChatListenSocket != NULL;
```

Every one of those writes **the document**, not the page — and `LoadPrefsP23`
runs when the property sheet is *built*, alongside the other twenty-two, so it
happens whichever page the user asked for and before anything is shown. By the
time there is a button to press, the document has already changed, and **Cancel
cannot undo it**: the change did not happen at OK.

`SetModifiedFlag` is not called either, so the world is not marked dirty and
nothing prompts. The new values simply sit in the document until the next save
— autosave, File ▸ Save World Details, quitting a world with
`save_world_automatically` — and are written then.

**Measured on the running client.** A world file with no `chat_name`, opened,
Game ▸ Configure ▸ *MUD name/IP address* (not even the Chat page), **Cancel**,
File ▸ Save World Details, diff:

```
CANCEL : WORLD FILE CHANGED
   =>    chat_name="Name-not-set"
```

**Fix:** default into locals and put them on the *page*. Nothing is lost:
`SavePrefsP23` copies all of these back on OK, unconditionally
(`configuration.cpp`'s save block is a straight-line list, not gated on
`ChangedPrefs*`), so the OK path lands on exactly the values it always did. It
also now *says* so — `ChangedPrefsP23` can finally see the difference, so a
sheet that fills in defaults marks the world modified, which it should always
have done.

Verified both ways on the running client after the change:

```
Chat page, OK      -> WORLD FILE CHANGED:  chat_name="Name-not-set"
Chat page, CANCEL  -> world file unchanged
```

**Scope.** Only `LoadPrefsP23` does this; the other twenty-two `LoadPrefsP*`
functions assign to `pageNN.*` and nothing else, checked by walking each body
for a bare `m_… =`. So this is one page's slip, not a pattern — but it is the
page every sheet loads.

## 96 — A short chat file block writes heap memory into the user's file

**Where:** `chatsock.cpp`, `CChatSocket::Process_File_block`.

```cpp
UINT expected = m_iFileBlockSize;
if (m_iBlocksTransferred == m_iFileBlocks)
  expected = m_iFileSize % m_iFileBlockSize;   // smaller final block
if (expected == 0)
  expected = m_iFileBlockSize;

CString strBuffer = strMessage;
char * pBuff = strBuffer.GetBuffer (strMessage.GetLength ());
...
m_pFile->Write (pBuff, expected);
...
shsUpdate (&m_shsInfo, (unsigned char *) pBuff, expected);
```

`expected` is what the **peer promised**: `m_iFileBlockSize` comes from the
handshake (`request.fileBlockSize`, 1024 for zChat) and `m_iFileSize` from its
file-start message (`m_iFileSize = atol (strSize)`). `pBuff` is a buffer of
`strMessage.GetLength ()` bytes — what actually arrived. **Nothing compares the
two.**

The two transports differ, and only one is safe:

| transport | how the block is sliced |
|---|---|
| MudMaster | `if (iLength < (m_iFileBlockSize + 2)) return;` then `Mid (1, m_iFileBlockSize)` — waits for a full block, so the slice is always exactly the block size |
| **zChat** | length taken from two bytes of the peer's own message header, `Mid (4, iLength)` — **whatever the peer said, 0 to 65535, never compared to the block size** |

So a peer sending a one-byte `CHAT_FILE_BLOCK` over zChat makes the receiver
`Write (pBuff, 1024)` out of a one-byte allocation: **1023 bytes past the end of
a heap block, written into the file the user is receiving**, and fed to the SHA
sumcheck alongside. The guard the zChat arm needs is the one the MudMaster arm
already has.

**Reachable:** the chat file-transfer flow — a peer this client is chatting with
offers a file and the user accepts it, or `m_bAutoAllowFiles` ("accept files
automatically") is on and no acceptance is needed. Both ends of a chat session
are ordinary MUSHclient users; this needs a modified or malfunctioning one at
the other end, not a compromised MUD.

**Fix:** refuse a block shorter than promised, rather than clamping. A short
block means the transfer is already wrong, and silently writing a short one
produces a corrupt file whose only symptom is the sumcheck failing at the end.
The abort path already exists and already deletes the half-received file.

**Shared code — the same defect ships on macOS**, where a sanitizer would call
it a heap-buffer-overflow read. `ChatFileBlockR6.AShortBlockIsRefusedRatherThan
PaddedFromTheHeap` in `tests/win/test_gap_coverage.cpp` drives
`Process_File_block` directly (the socket-level suite is excluded on Windows for
using `socketpair`) and was verified non-vacuous: with the guard replaced by
`if (false)`, all three of its assertions fail.

## 97 — A script routine name is used as a printf format (Windows COM engines)

**Where:** `scripting/scriptengine.cpp`, `CScriptEngine::ExecuteScript`, inside
the file's own `#ifdef _WIN32`.

```cpp
m_pDoc->Trace (TFormat ("Executing %s script \"%s\"", szType, szProcedure));
```

`CMUSHclientDoc::Trace` is **printf-style** — `doc.h` declares it
`void Trace (LPCTSTR lpszFormat, ...) MC_PRINTF_FORMAT(2, 3)` and its body does
`strMsg.FormatV (lpszFormat, argList)`. The line above hands it the
*already-composed* string as the format, with no varargs behind it. A `%` in
`szProcedure` is therefore a conversion specifier read against an empty
argument list.

`szProcedure` is a script routine name: it comes from a world file's
`script="…"` attribute on a trigger, alias or timer, or from
`world.AddTrigger (…, "name")`. A routine called `100%s` is enough.

**This was already found and fixed — three times — in the Lua twin**, which
carries the reasoning verbatim at each site:

```cpp
// Trace takes a FORMAT; szProcedure is a plugin-supplied callback name, so
// handing the composed string straight in would let a '%' in it read a
// vararg that does not exist.
m_pDoc->Trace ("%s",
  (LPCTSTR) TFormat ("Executing %s script \"%s\"", szType, szProcedure));
```

(`scripting/lua_scripting.cpp`, three occurrences.) `exceptions.cpp` carries a
comment about the same hazard. The COM path is `#ifdef _WIN32`, so the port
never compiled it and the sweep that fixed the others never reached it.

**Fix:** the same one line the Lua path uses. A tree-wide grep for a
`MC_PRINTF_FORMAT`-annotated function called with a computed first argument now
returns nothing but the two legitimate `va_list` forwarders in `exceptions.cpp`.

**Narrow but real:** it needs a `%` conversion in a routine name, tracing on
(Game ▸ Trace), and a COM scripting language — VBScript, JScript, PerlScript or
PythonScript, i.e. not Lua. That is a Windows-only combination, which is exactly
why it survived.

## 98 — A second chat file offer corrupts, or frees, the transfer in progress

**Where:** `chatsock.cpp`, `CChatSocket::Process_File_start`.

The function had **no in-progress guard**. Its only gates were
`m_bCanSendFiles`, the `"filename,filesize"` argument count, the slash check,
and whether the user wanted the file. A second `CHAT_FILE_START` arriving
mid-transfer therefore ran straight through to

```cpp
m_pFile = new CFile (m_strOurFileName, CFile::modeCreate | CFile::modeReadWrite);
```

**leaking the open `CFile`** and orphaning the half-received file it was
writing. And the failure arm of that same `new` did

```cpp
delete m_pFile;    // in case it was set up
```

**without nulling it.** On its own that is harmless — `m_bDoingFileTransfer` is
false, so `Process_File_block` and `StopFileTransfer` both return early — but
together with the missing guard it is a **write through freed memory**: a
transfer is running (so the flag is true), a second offer's open fails, and the
next `CHAT_FILE_BLOCK` writes to the deleted `CFile`.

**It bites even when the user says no.** `m_iFileSize = atol (strSize)` runs
*before* the accept prompt, so a second offer that is refused still overwrites
the running transfer's size — and that feeds `m_iFileBlocks` and the
final-block arithmetic, so the file being received is truncated or padded.
Measured: with the guard removed, `m_iFileSize` reads 2048 where the running
transfer had 4096.

**Fix:** refuse a second `CHAT_FILE_START` while one is running, with a
`CHAT_FILE_DENY` in the same style as the existing refusals; and null `m_pFile`
after deleting it, as every other site that deletes it already does.

`ChatFileBlockR6.ASecondFileStartIsRefusedWhileATransferIsRunning` pins it, and
was verified non-vacuous.

**Shared code — the same defect ships on macOS.** Same family as 96, found in
the same function's neighbourhood by the same sweep.

## 99 — `CTimer`'s constructor leaves `bEnabled` uninitialised

**Where:** `OtherTypes.h`, `CTimer::CTimer`.

The constructor assigns twenty-one members and not `bEnabled`. Its two
siblings — `CAlias` and `CTrigger` — both do `bEnabled = TRUE;`.

That is not cosmetic, because `Get_XML_boolean` leaves its output **untouched**
when the attribute is absent and defaults are wanted:

```cpp
if (!Get_XML_string (node, sName, strValue, bUseDefault))
  if (bUseDefault)
    return false;          // bValue never assigned
```

and the timer loader reads it through that path
(`GET_BOOL_BOOL ("enabled", t->bEnabled)`, with `bUseDefault` coming either
from the element's own `defaults="y"` or from the enclosing `<include>`). So a
`<timer defaults="y">` with no `enabled=` attribute — ordinary hand-written
plugin XML — took whatever was on the heap. Under a Debug CRT that is `0xCD`
fill, i.e. reliably true; under a Release allocator it is whatever the previous
occupant left.

**Fix:** `bEnabled = TRUE;`, matching the siblings. Pinned by
`ConstructorDefaultsR6.ATimerIsEnabledOnConstructionJustLikeAnAliasAndATrigger`,
which asserts all three together — a timer that loaded *disabled* where an
identically-written alias loaded enabled would be a subtler bug than the
uninitialised read.

## 100 — `world.SetOption ("wrap_column", n)` under-allocates the line buffer and then lies about it

**Where:** `scriptingoptions.cpp`, the `OPT_FIX_WRAP_COLUMN` arm of
`SetOptionItem`.

```cpp
delete [] m_pCurrentLine->text;
m_pCurrentLine->text = new char [MAX (m_pCurrentLine->len, Value)];
```

Two things wrong, and the second is what makes the first dangerous.

**The size is in the wrong unit.** `CLine`'s own constructor sizes the buffer in
BYTES from a wrap column counted in CHARACTERS, and a UTF-8 world needs four
bytes each:

```cpp
if (bUnicode) iMemoryAllocated = nWrapColumn * 4;
else          iMemoryAllocated = nWrapColumn;
```

This site used the raw column count, so on a UTF-8 world the re-allocated buffer
is **a quarter** of what the wrap logic is entitled to fill.

**And `iMemoryAllocated` was never updated.** `DisplayMsg`'s emergency bail-out
is

```cpp
// in the Unicode case we are allocating 4 bytes per character so we shouldn't
// overshoot the allocated memory, but just in case, we test that we don't
if (((iLineLength >= m_nWrapColumn) && bNewCharacter) ||
    (m_pCurrentLine->len >= m_pCurrentLine->iMemoryAllocated))
```

— the single guard against overrunning this buffer, and it was answering for the
allocation the line used to have. The other site that re-allocates the same
buffer (`doc_shared.cpp`, the end-of-line shrink) updates `iMemoryAllocated`
first and allocates from it; only this one did not.

**Measured**, with the update removed: a UTF-8 line built at wrap 80 has
`iMemoryAllocated == 320`; after `SetOption ("wrap_column", 200)` the buffer is
**200 bytes** and the record still reads **320**. The guard therefore permits
120 bytes of writing that the allocation cannot take, and MUD output is what
does the writing.

**Reachable from a script**: `world.SetOption ("wrap_column", n)` is a
documented API, and the world-properties Output page changes the same option.
Shrinking is the dangerous direction and is the ordinary one — 80 columns from a
wider default.

**Fix:** compute the size once, into `iMemoryAllocated`, with the constructor's
own arithmetic, and allocate from it. Pinned by
`WrapColumnResizeR6.TheLineBufferAndItsRecordedSizeStayInStep`, verified
non-vacuous.

**Shared code — the same defect ships on macOS**, where a UTF-8 world is the
common case rather than the unusual one.

## 101 — Four world options silently revert to their defaults when set to zero

**Where:** `xml/xml_save_world.cpp`, the numeric arm of the option loop.

Two rules that only compose when the default is zero:

- `Save_XML_number` omits an attribute whose value is 0, unless the caller asks
  for it (`bWriteZero`).
- The loader keeps the member **as constructed** when a numeric attribute is
  absent — `Get_XML_number` with `bUseDefault` leaves its output untouched, and
  the constructor value is the table default.

So an option whose default is non-zero loses a deliberate 0 on every save.

`max_output_lines` is the one row anybody had noticed; it was given
`OPT_ZERO_UNLIMITED` to force the write, and both `docs/AUDIT-2026-08.md` and
`docs/WINDOWS-VALIDATION.md` close that story with *"Any **future** option where
0 is a real setting rather than 'unset' has the same trap"*. **Four existing
rows already had it:**

| option | default | range |
|---|---|---|
| `pixel_offset` | 1 | 0–20 |
| `fade_output_opacity_percent` | 20 | 0–100 |
| `tool_tip_visible_time` | 5000 | 0–120000 |
| `tool_tip_start_time` | 400 | 0–120000 |

Every one of them accepts 0 from `world.SetOption` and from its prefs page, and
0 means something in each: no offset, fade to transparent, show immediately.

**Measured**, with the fix reverted: set all four to 0, save, and none of the
four attributes is in the file; reload and they read 1, 20, 5000 and 400.

**Fix:** write a zero whenever the option's default is non-zero, which is
exactly the condition under which omitting it loses information. A row whose
default is 0 is written exactly as before, and a non-zero value always was —
so no existing world file changes except by gaining the four attributes it
should always have carried.

`WorldLoad.AZeroSurvivesOnEveryOptionWhoseDefaultIsNotZero` pins all four,
checking both the bytes on disk and the reload. The two checks are not
redundant: the boolean and alpha arms of the loader *do* zero an absent
attribute, so a reload-only test could pass on a build where the numeric arm
started doing the same.

## 102 — Replacing a trigger, alias or timer whose own script is running frees it

**Where:** `scripting/methods/methods_triggers.cpp` (`AddTriggerEx`),
`methods_aliases.cpp` (`AddAlias`), `methods_timers.cpp` (`AddTimer`) — the
`eReplace` arm of each.

`ProcessPreviousLine` brackets a trigger's script like this:

```cpp
trigger_item->bExecutingScript = true;     // cannot be deleted now
SendTo (...);                              // the script runs here
trigger_item->bExecutingScript = false;    // can be deleted now
```

`DeleteTrigger` honours that comment:

```cpp
// can't if executing a script
if (trigger_item->bExecutingScript)
  return eItemInUse;
```

The `eReplace` arm of `AddTriggerEx` did not — it went straight to
`delete trigger_item`. So a script that replaces the very trigger that invoked
it frees the object, and the `= false` on the line after `SendTo` writes into
freed memory. `DeleteAlias` and `DeleteTimer` have the same guard and
`AddAlias`/`AddTimer` had the same hole.

**Reachable, and not exotic:** `world.AddTrigger (name, …, eReplace)` from
inside that trigger's own script. Re-registering yourself — to change the match
text, the sequence, or the response — is an ordinary thing for a plugin to do,
and `eReplace` exists precisely so that it can be done without a separate
delete.

**Measured**, with the three guards removed: all three calls return `eOK`
instead of `eItemInUse`, and the map's pointer changes underneath the caller —
`0A7A5F58` → `0A7A6500` for the trigger, and likewise for the alias and the
timer. The old address is the one the caller is about to write `false` into.

**Fix:** the same guard the matching `Delete*` has, at the point where
`bReplace` is decided, returning `eItemInUse`. Pinned by
`ReplaceWhileRunningR6.AllThreeRefuseToReplaceAnItemWhoseScriptIsOnTheStack`,
which also asserts the refusal is about the *script* and not about replace being
broken: with the flag cleared, the same three calls succeed.

**Shared code — the same defect ships on macOS.**

## 103 — Three archive writes report success before the bytes are written

**Where:** `plugins.cpp` (`CPlugin::SaveState`),
`dialogs/world_prefs/prefspropertypages.cpp` (export variables) and
`evaluate.cpp` (`Save_Set` — export triggers/aliases/colours/macros/timers).

All three have the same shape:

```cpp
try {
  f  = new CFile (name, CFile::modeCreate | CFile::modeReadWrite);
  ar = new CArchive (f, CArchive::store);
  Save_World_XML (*ar, …);
  bError = false;              // success declared HERE
}
catch (CFileException *)   { … }
catch (CMemoryException *) { … }
catch (CArchiveException *){ … }

delete ar;                     // ...and the bytes are written HERE
delete f;
```

`Save_World_XML` only fills the archive's buffer. The write happens when the
`CArchive` is destroyed — **after every catch arm** — so:

- **A failed write is reported as success.** `CPlugin::SaveState` returns its
  `bError` to `world.SaveState ()`, so a script is told the state was saved when
  it was not. `CFile::modeCreate` truncated the old state file first, so what
  the user actually has is nothing.
- **And the failure throws out of a destructor.** MFC's `~CArchive` is

  ```cpp
  CArchive::~CArchive()
  {
      if (m_pFile != NULL && !(m_nMode & bNoFlushOnDelete))
          Close();      // -> Flush(), which can throw
      Abort();
  }
  ```

  with no guard of its own. Under C++11 a destructor is implicitly `noexcept`,
  so a `CFileException` from that `Flush` terminates the process.

**Fix:** `ar->Close ()` inside the `try`, before success is declared. The
subsequent `delete ar` then finds `m_pFile == NULL` and does nothing.

**No new test.** The success path is already pinned by
`PluginState.SaveStateWritesVariablesToStateFile`, which reads the file's
contents back and is green with the change; the failure path needs a write that
fails, which this suite has no way to arrange. That is an honest gap rather than
a covered one.

**Shared code — all three sites ship on both platforms.**

## 104 — The Send-to-world line preamble loses its trailing space on every load

**Where:** `scriptingoptions_alpha.inc`, the `send_to_world_line_preamble` and
`send_to_world_line_postamble` rows.

`Get_XML_string` trims leading and trailing whitespace unless the row carries
`OPT_KEEP_SPACES` (`xml_load_world.cpp` passes
`!(iFlags & OPT_KEEP_SPACES)` as its trim flag). The saver preserves the space —
`Save_XML_string` only collapses newlines and escapes markup — so the bytes on
disk are right and the loader is what changes them.

And `doc.cpp` concatenates with **no separator**:

```cpp
full_line  = dlg.m_strLinePreamble;
full_line += str;
full_line += dlg.m_strLinePostamble;
```

so the space in `"say "` is the only thing between the verb and the text. Set
one, save, reload, and every line is sent as `sayhello`.

**These two were the only non-multiline `*amble`/prefix rows in the table
without the flag.** The six `log_line_*amble_*`, both `paste_line_*amble`,
`recall_line_preamble`, `chat_message_prefix`, `speed_walk_prefix` and all three
`timestamp_*` rows have it. That is twelve to two — an omission, not a
judgement.

**Measured**, with the flag removed again: `"say "` loads as `"say"` and `" ."`
as `"."`, while the paste twins keep theirs.

**Fix:** `OPT_KEEP_SPACES` on both rows.
`WorldLoad.ASendPreamblesTrailingSpaceSurvivesTheLoad` pins it, with the paste
pair as the control — a test that checked only the send pair would pass on a
build where the trim had been removed wholesale.

## 105 — `world.SetTimerOption` can give an "every" timer a zero interval, and it then fires on every poll

`scripting/methods/methods_timers.cpp`, `SetTimerOption`.

An interval timer whose hour, minute and second are all zero fires on **every**
poll of `CheckTimers`, indefinitely. `AddTimer` refuses that combination
outright:

```cpp
// can't have a zero time for "every" timers
if (((Hour == 0) && (Minute == 0) && (Second == 0.0)) && !(Flags & eAtTime))
  return eTimeInvalid;
```

and so does the Timers dialog, in `CTimerDlg::DoDataExchange`:

```cpp
if (ts1 <= CmcDateTimeSpan (0, 0, 0, 0))
    {
    ::TMessageBox("The timer interval must be greater than zero.");
```

`SetTimerOption` sets `hour`, `minute` and `second` one at a time and checked
neither the component it was writing nor the combination it was producing, so
zeroing whichever component was last still non-zero arrived at exactly the state
the other two entry points exist to prevent — returning `eOK`.

Negative values were never a route: the `TimerOptionsTable` rows carry a minimum
of 0, so `SetBaseOptionItem` answers `eOptionOutOfRange` for those. Zero was the
only hole.

**Why it runs away.** `CheckTimers` fires anything whose `tFireTime` is not in
the future, then advances it:

```cpp
timer_item->tFireTime += CmcDateTimeSpan (0, timer_item->iEveryHour,
                                          timer_item->iEveryMinute,
                                          timer_item->fEverySecond);

// in case clock changes or some such thing, make sure timer will be due to
// fire in the future, not the past, or it might go mad and keep firing

if (timer_item->tFireTime <= tNow)
  ResetOneTimer (timer_item);
```

Adding a zero span leaves `tFireTime` where it was — in the past — so the guard
fires, and `ResetOneTimer` sets `tFireTime = tNow + interval`, which for a zero
interval is `tNow` again. **The comment names the symptom exactly** ("it might
go mad and keep firing") and the guard it introduces cannot reach this case,
because the case is the one where adding the interval is a no-op.

**Measured**, not argued. `TimerIntervalR6.ZeroIntervalTimerFiresOnEveryPoll`
calls `CheckTimers` three times back to back with no time allowed to pass: the
zero-interval timer matches 3 times, a legitimate five-second one at most once.

**Fix:** the same combined check `AddTimer` already makes, applied in
`SetTimerOption` **before** `SetBaseOptionItem` so that a refusal leaves nothing
half-written. Pinned by
`TimerIntervalR6.SetTimerOptionRefusesToZeroTheLastComponentOfAnEveryTimer`,
which also asserts that zeroing a component while another stays non-zero is
still allowed, that `iEveryMinute` is unchanged after the refusal, and — as
controls — that `AddTimer` gives the same `eTimeInvalid` and that an *at*-time
timer may still be set to midnight.

**Not fixed, and deliberately:** the XML world loader makes no such check
either, so a hand-edited world file can still contain a zero-interval timer.
Rejecting one at load time would fail to open a file that opens today, which is
a different risk from refusing a script call, and belongs in its own change.

## 106 — The world saver rounds a timer's interval to a value its own loader refuses

`xml/xml_save_world.cpp`, `Save_XML_double`.

A timer's seconds are declared to **four** decimals everywhere the app reads or
validates them:

```cpp
GET_DOUBLE ("second", t->fEverySecond, 0, 59.9999);     // xml_load_world.cpp
DDV_MinMaxDouble (pDX, m_fEverySecond, 0., 59.9999);    // TimerDlg.cpp
```

and to **two** where it writes them:

```cpp
CString strNumber = CFormat ("%.2f", fNumber);
```

`AddTimer` accepts any interval but exactly zero, and the dialog refuses only
`<= 0`, so `world.AddTimer ("blip", 0, 0, 0.001, ...)` — or typing `0.001` into
the Timers dialog — is legal on both routes. Saving the world writes it as
`second="0.00"`, and the loader then **refuses the app's own output**:

```cpp
if (!(t->iEveryHour || t->iEveryMinute || t->fEverySecond > 0.0))
  ThrowErrorException ("Timer for 'every' time cannot have zero interval");
```

**Measured**, before the fix, with three timers in the file:

- the file contained `second="0.00"` for the sub-0.005 s one;
- the reload dropped exactly that timer and kept the other two — the per-item
  `HandleLoadException` means the blast radius is one timer, not the section;
- `LoadWorldFromFile` still returned TRUE, so the world opens normally with the
  timer simply gone;
- the user gets `[WARNING] <world file>` and `Timer for 'every' time cannot
  have zero interval` in the output window — naming a file the app itself had
  just written.

This is the same end state as `UPSTREAM-BUGS` 105 approached from the other
side. There, `SetTimerOption` let a script *create* the zero interval that
`AddTimer` and the dialog both refuse; here the saver *manufactures* it out of
a legal value. Four places had an opinion about a zero interval and three of
them agreed.

**Fix:** `Save_XML_double` takes a precision, defaulting to 2 so the only other
caller (`plugins.cpp`'s `"requires"` version number, which wants exactly two)
is untouched, and the two timer-second sites pass 4 — the precision the loader
and the dialog already declare. Trailing zeros are trimmed back to two decimals
so that widening the format does **not** rewrite every existing world file:
`5.0` still writes `"5.00"`, and only `0.001` spends the extra digits.

**Residual, and deliberate:** an interval below 0.00005 s still rounds to zero.
That is now *below the declared precision of the field* rather than inside it,
so it is a stated limit instead of a silent disagreement. Note also that
`CheckTimers` is driven by a 100 ms poll at best, so nothing below that is
distinguishable at runtime anyway.

Pinned by `TimerSaveR6.SubCentisecondIntervalSurvivesASaveAndReload`, which
checks the written text, the round trip, and — as the control for the trim —
that a whole-second timer still writes `"5.00"`. There is deliberately **no**
textual negative assertion for `second="0.00"`: `offset_second="0.00"` is a
legal superstring of it, and the first draft of the test failed on exactly that.

## 107 — A SOCKS 5 proxy that answers with a non-IPv4 bind address puts binary garbage on the first line

`doc_shared.cpp`, `ProcessProxyResponse3`.

RFC 1928 §6 gives the CONNECT reply as

```
+----+-----+-------+------+----------+----------+
|VER | REP |  RSV  | ATYP | BND.ADDR | BND.PORT |
| 1  |  1  |   1   |  1   | Variable |    2     |
+----+-----+-------+------+----------+----------+
```

where BND.ADDR is **4** bytes for ATYP 1 (IPv4), **16** for ATYP 4 (IPv6) and
**1 + N** for ATYP 3 (a domain name). Only the IPv4 case is ten bytes, and the
code said

```cpp
iBytesToMove = 10;
```

unconditionally. The caller then does

```cpp
memmove (buff, &buff [iBytesToMove], count - iBytesToMove);
count -= iBytesToMove;
```

so with an IPv6 bind address the remaining **12** bytes of the proxy's reply
are moved to the front of the receive buffer and handed to `DisplayMsg` — the
session opens with a line of binary. A domain-name reply does the same with
however many bytes the name ran to.

This is not a memory-safety bug: the caller guards with
`if (count <= iBytesToMove) return;`, so a *short* reply is dropped rather than
over-read.

**Fix:** derive the length from ATYP. An ATYP the RFC does not define keeps the
old ten-byte assumption — there is nothing better to guess from — and the test
pins that as a choice rather than leaving it undefined.

**Not fixed, and it needs more than a length calculation: a reply split across
TCP segments.** There is no accumulation buffer for the proxy phase; each
`ProcessProxyResponse*` sees one packet and must answer from it. A short read
still reports success off `buff [1]`, and the rest of the reply arrives in the
next packet, by which time the phase is `eConnectConnectedToMud` and it is
displayed. Closing that means giving the proxy phase a persistent buffer and a
"need more data" return value — a change to the phase machinery, not to this
arm, and worth its own decision. A ≤22-byte reply written immediately after
connect rarely splits, which is why this has not been noticed; it is a real
hole all the same.

Pinned by `Socks5R6.ConnectReplyLengthFollowsTheAddressType`, which asserts the
returned byte count — the value the caller consumes — for all three address
types, for an undefined one, and for a refusal.

## 108 — Opening World Properties and clicking OK rewrites "accept incoming calls" to match runtime state

`dialogs/world_prefs/configuration.cpp`, `LoadPrefsP23`.

The Chat page's "&Accept incoming calls on port:" tick was filled from the
**socket**, not the preference:

```cpp
// show whether we really are accepting calls
page23.m_bAcceptIncomingChatConnections = m_pChatListenSocket != NULL;
```

while `SavePrefsP23` copies that same tick straight back into the persisted
`m_bAcceptIncomingChatConnections`. So OK on a dialog nobody touched
overwrites the saved preference with runtime state, and it goes wrong in
**both** directions:

- the listen socket **failed to bind** — the port is already in use, which is
  easy to arrange with two worlds on the default 4050 — so the tick shows
  clear, and OK turns the saved preference **off**. The next launch does not
  even try, and nothing told the user their setting had changed.
- a script called **`world.ChatAcceptCalls`**, so the tick shows set, and OK
  turns the preference **on** in a world that never asked for it.

The socket normally does come up at world open (`SetUpOutputWindow`
→ `ChatAcceptCalls (0)` when the flag is set), which is why the first case
needs a bind failure to appear and why this has gone unnoticed. The second
needs only a plugin.

**Fix:** show the preference. Nothing is lost: a bind failure is already
reported when it happens, by `ChatNote` inside `ChatAcceptCalls` — and OK on a
world whose bind failed now **retries** it, because `SavePrefsP23`'s "start
accepting if required" arm runs on a flag that is still true, instead of
falling into the `else` that stops.

Pinned by `PrefsChange.ChatAcceptTickShowsThePreferenceNotTheSocket`. With the
fix reverted it fails on three assertions, the last of which is the data loss
itself: `m_bAcceptIncomingChatConnections` is FALSE after `SavePrefsP23` on an
untouched page.

**A note on the test, because it caught a wrong assumption of mine.** The first
draft asserted `ChangedPrefsP23` was false on a fresh document and failed —
not because of this bug, but because `UPSTREAM-BUGS` 95 deliberately made
`LoadPrefsP23` put *defaults* for the chat name, port and two colours on the
page while leaving the document alone, so the page legitimately differs from
the document until OK. 95's own comment says exactly that. The test now seeds
those four fields so the comparison isolates the tick. **A page-wide "did
anything change" check is not a test of one field**, and reading it as one
turns a documented behaviour into a phantom regression.

## 109 — The trigger clipboard dropdown offers a wildcard the clipboard code cannot copy

`ProcessPreviousLine.cpp`, and the mapping now lives in `OtherTypes.h`.

The Trigger dialog's clipboard dropdown (`IDC_WILDCARD_CLIPBOARD`) is populated
from `DLGINIT` with **eleven** entries — `(none)`, then `1` … `10` — so the
selected index is the wildcard number. The code that acts on it was

```cpp
if (trigger_item->iClipboardArg &&
    trigger_item->iClipboardArg >= 0 &&
    trigger_item->iClipboardArg < MAX_WILDCARDS)
   putontoclipboard (trigger_item->wildcards [trigger_item->iClipboardArg]...);
```

with `MAX_WILDCARDS == 10`, so the usable range was **1 … 9** and the last
entry the dropdown offers stored 10, failed the bound, and silently copied
nothing.

**Wildcard 10 is not a nonexistent wildcard.** The array handed to scripts is
built 1-relative with `MAX_WILDCARDS` elements, and its final element is
deliberately not a capture group:

```cpp
// i should be MAX_WILDCARDS (10) now ;)
COleVariant v (trigger_item->wildcards [0].c_str ()); // the whole matching line
```

(`doc_shared.cpp:4500`, and the same again in `evaluate.cpp:1195` for aliases.)
So a script's `wildcards[10]` is the whole matching line — which is the natural
reading of what the dropdown's `10` offers, and the clipboard code could reach
it by neither index: 10 failed the upper bound, and 0, where the whole line
actually lives in `trigger_item->wildcards`, fails the truthiness guard that
means "(none)".

The `iClipboardArg >= 0` term beside it was dead: the member is
`unsigned short`.

**Fix:** `ClipboardWildcardIndex` (`OtherTypes.h`) maps `clipboard_arg` onto the
vector — 0 is "copy nothing", 1…9 are `%1`…`%9`, and 10 is `wildcards [0]`, the
whole matching line. It **remaps** rather than widening the bound, because
widening it would index a 10-element vector at 10; that is what the old bound
was there for, and the fix keeps that protection while making the dropdown's
last entry mean what scripts already mean by it.

Two tests, at two levels, because neither alone is enough:

- `ClipboardWildcardR6.TenNamesTheWholeLineAndZeroNamesNothing` asserts the
  mapping, including the property that matters — **no value the option table
  and the loader accept (0…10) can index past the end of the vector**. It
  deliberately does not drive the call site: that would put text on the
  machine's real clipboard and clobber whatever the user had copied.
- `tests/win/app_probes/clipboard.lua` drives the real call site inside the
  running app, through an actual trigger match, and can afford to because
  `world.GetClipboard`/`SetClipboard` let it save and restore. It seeds
  `SENTINEL` before each match so "copied nothing" is visible, and checks
  `clipboard_arg` 1, 3, 10 and 0 — the last as the control. Against the app
  built before the fix it failed on exactly the `10` assertion and passed the
  other three, which is the end-to-end proof the unit test cannot give.
