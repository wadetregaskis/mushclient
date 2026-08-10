# Windows validation handoff

For the Claude instance on the Windows VM.  Your mission: build this repo's
Windows client, validate the recent cross-platform refactor, and check for
regressions against original/upstream MUSHclient behaviour.  Everything
Windows-side described below was written **by reasoning only** — no MSVC
compile or run has ever happened on it.  You are the first empirical check.

> **STATUS 2026-08-10 — this has now been built and run.  Read §8 (Results)
> before working from anything below it**: every §4c question is answered, §6's
> five deltas are measured (§8w), the four `sendvw.cpp` divergences have been
> run (§8v), one prediction in §4b was wrong, and eight defects were found and
> fixed (`0abb31b`, `50a1303`, `c24926f`, `08c8fa8`, `e4236e8` — a complete
> zChat outage — `edc407e` and `ed60501`), with the build broken at HEAD until
> the first of those.  Sections 1–7 are left exactly as written so the
> predictions can be scored against what actually happened.  **§8z** is down to
> one item (mapper redraw while playing); **§8-end** lists the files that still
> need a macOS build.

## 1. Context in three paragraphs

This repo is Nick Gammon's MUSHclient (a Windows MFC MUD client) plus a
macOS/Linux port and, since 2026-07, a **Platform Abstraction Layer (PAL)**
refactor: platform-agnostic engine + thin per-platform adapters behind small
interfaces (`pal/*.h`).  The spec is `docs/PAL.md`.  The owner (Wade) decided
Windows is *not* forked around: it routes through the same PAL, with native
adapters whose bodies are the exact original Win32/MFC call sequences
relocated behind the seams.  **The prime directive: Windows behaviour must
not be perturbed.**  Structural change is fine; observable behaviour changes
are defects unless listed in §6.

Three seams are live on every platform:

- **ISurface** (`pal/isurface.h`) — miniwindow raster drawing.  Windows
  adapter: `pal/gdi_surface.cpp`.  All `world.Window*` drawing in
  `miniwindow.cpp` goes through it unconditionally.
- **ITransport** (`pal/itransport.h`) — sockets + TLS + DNS.  Windows
  adapter: `pal/win_transport.cpp`.  `CWorldSocket`/`worldsock.{h,cpp}` are
  **deleted**; the whole connection lifecycle is single-sourced in
  `doc_shared.cpp` (compiled by MSVC).
- **IWorldView** (`pal/iworldview.h`) — the per-world window (output pane,
  command bar, titles, info bar).  Windows adapter:
  `pal/win_worldview.cpp`, doc-owned, wired in `doc_construct.cpp`.  Five of
  six method families have had their engine-site `#ifdef _WIN32` forks
  deleted (the seam call is the only path); the display family
  (`appendLine`/repaint) has NOT been flipped — Windows still paints by pull
  (`UpdateAllViews` → `CMUSHView::OnDraw`), unchanged.

The macOS side is live-verified continuously and is NOT your concern except:
if you must edit a **shared** file (anything not whole-file
`#ifdef _WIN32`), flag it clearly in your report so the mac side gets
re-verified.  Prefer Windows-only fixes.

## 2. Building

- Solution: `MUSHclient_2022.sln` / `MUSHclient.vcxproj` (Win32, MBCS,
  `/std:c++14` — the project sets no `<LanguageStandard>`, so **no C++17**
  anywhere in code MSVC compiles; the pal headers were scrubbed for this).
- OpenSSL 3.x is expected at `$(SolutionDir)openssl\include` +
  `$(SolutionDir)openssl\lib` (`libssl.lib`, `libcrypto.lib`; `Crypt32.lib`
  from the SDK).  If the `openssl\` directory is absent in your clone, ask
  Wade where his copy lives — it is not committed.
- New PAL `.cpp` files use the project's default PCH setting (`NotUsing`)
  and include `stdafx.h` as an ordinary header.  This is the established
  pattern (`pal/gdi_surface.cpp` builds this way).

**Expect first-compile errors.**  Fixing them is part of your job.  Keep
fixes minimal and semantics-preserving; if a fix changes behaviour rather
than syntax, treat it as a finding (§7).  The riskiest constructs, in order:

1. `pal/win_transport.cpp` — MFC `CAsyncSocket` subclass, a message-only
   window (`HWND_MESSAGE`, `GWLP_USERDATA`, `WM_APP+401..403`),
   `WSAAsyncGetHostByName`, the local `tcp_keepalive`/`SIO_KEEPALIVE_VALS`
   block, OpenSSL calls, `CertOpenSystemStore` import loop.
2. `pal/win_worldview.cpp` — view loops over `CMUSHView`/`CSendView`,
   `Frame.m_wndInfoBar` rich-edit operations (`CHARFORMAT`,
   `SetBackgroundColor`), `CWindowPlacement`.
3. `doc_shared.cpp` — newly MSVC-compiled sections: the transport events
   bridge, `CreateTransport()` (`pal::CreateWinTransport` on `_WIN32`), the
   connection lifecycle (`ConnectSocket`, `OnConnect`, `OnBytesReceived`,
   `ConnectionEstablished`, `OnConnectionDisconnect`, `LookupHostName`/
   `LookupHostNameComplete`, `SendPacket`, `StartSSLHandshake`,
   `OnTls{Established,Failed}`), `GetSocketError`, `GetConnectionStatus`,
   `OpenLog`.  One known near-miss was already fixed (`<ws2tcpip.h>` for
   `getaddrinfo`); expect siblings.
4. `doc.h` / `doc_construct.cpp` — new members `m_pTransport`,
   `m_pTransportEvents`, `m_pWorldView` (+ NSDMIs), `ConnectionOpen()`.
5. `mainfrm.cpp` — the `WM_USER_HOST_NAME_RESOLVED` dispatcher now serves
   **only chat sockets** (world DNS lives inside the transport).

If a fix belongs to a whole-file-`#ifdef _WIN32` TU, commit freely.  Repo
commit discipline (binding): messages never mention Claude/the assistant, no
Co-Authored-By trailer, author is Wade's global git identity, plain
`git commit`, no push unless Wade says so.

## 3. Baseline for regression comparison

Two baselines, use both:

- **Official MUSHclient** (5.06/5.07 installer from mushclient.com) running
  side by side — the gold standard for observable behaviour (rendering,
  stats text, prompts, timings).
- **This repo's history** — every relocation names its source.  The three
  adapter files carry per-method "source site" maps in their header
  comments; `git show <commit>` recovers any deleted original.  Pivotal
  commits: `249534f` (CWorldSocket retired; T5b), `f26b54c` (win_transport),
  `a8e0a28` (transport liveness-gate fixes), `02b7336`/`024e483` (worldview
  adapter + wiring), `614ae7d`/`de9970b`/`a9061ce` (the WV-3/4/5 family
  flips), `fbea7a2..1101426` (T3 lifecycle single-sourcing).

## 4. What to validate, in priority order

### 4a. Connection lifecycle (highest risk — total rewrite of the plumbing)

Test world: **Discworld — `discworld.starturtle.net`, port `4242`** (uses
MCCP compression, so the inflate path gets exercised for free).  Wade's
daily world; creating a throwaway guest character is fine, but don't touch
existing characters.

- Connect by **hostname** (exercises `WSAAsyncGetHostByName` inside the
  adapter's message-only window) and by raw IP.  Watch for the
  "--- Connected on <date> ---" note and the MOTD rendering.
- Type/send commands; verify echo, command history (up-arrow), and that the
  MUD receives input.
- Disconnect via menu; verify the full stats block ("Disconnected on …,
  Connected for …, Received N lines, sent N lines…").  Reconnect.
- Server-side close (log out of the MUD): same stats block, no zombie state.
- Cancel-during-connect: disconnect while "connecting"/during DNS.
- Bad hostname: expect the "Unable to resolve host name" path.
- TLS: any TLS MUD, or `openssl s_server` on the VM with a self-signed
  cert.  Expect: cert-verify failure → note + prompt ("connect anyway?") →
  accepting reconnects over TLS with a warning note; a plain handshake
  failure offers unencrypted fallback.  Success shows
  "--- Connected via TLSx.y (cipher) ---".
- Keepalives on (world config) — just verify no misbehaviour.
- SOCKS4/5 if convenient (any local SOCKS proxy); byte-level tests exist on
  the mac side, so this is lower priority.
- Long idle session: no drops, timers still fire.

### 4b. Miniwindows / drawing (GDI adapter, `world.Window*`)

**Quow's Discworld plugin pack** is the acceptance workload (heavy mapper
drawing, fonts, images, hotspots, drag).  Get it via
`packaging/fetch-discworld-plugins.sh` semantics — it downloads Quow's
portable package (see the script's `--url`/`QUOW_PACKAGE_URL`); on Windows
you can equally install Quow's pack the normal way.  Compare the mapper,
inventory/status windows, tooltips, hotspot hover/click/drag, and miniwindow
z-order against the official client side by side.

Also port the mac conformance checklist to a Lua script (run via
Game ▸ Immediate, Ctrl+I): `platform/miniwindow_selftest.cpp` lists ~28
probes (create/resize/delete lifecycle, RectOp fill exclusivity, lines,
shapes, gradients, fonts/TextWidth, error codes).  **Windows-expected values
differ from the port in three pinned places**: out-of-bounds
`WindowGetPixel` = **-1** (CLR_INVALID as a Win32 long; the port sees
4294967295), line endpoints are **excluded** (GDI `LineTo`; the port draws
inclusive), and stroked shape outlines are **crisp pen colour** (no
antialiasing; the port blends).  No-such-window `WindowGetPixel` = -2
everywhere.

### 4c. The flipped IWorldView families (routes changed, behaviour must not)

Drive each through Lua (Immediate window) and the UI:

- **Title**: `SetTitle`, `SetMainTitle` — child/main title bars update.
- **Info bar**: `ShowInfoBar(1)`, `Info("text")`, `InfoColour("red")`,
  `InfoBackground("yellow")`, `InfoFont("Courier New", 12, 1)`,
  `InfoClear()` — including that InfoClear resets to the default format.
- **Command bar**: `SetCommand` (only fills when empty; `eCommandNotEmpty`
  otherwise), `GetCommand`, `SelectCommand`, `SetCommandSelection`,
  `PushCommand`, `GetCommandList` (newest first), `DeleteCommandHistory`,
  `GetInfo(236)`/`GetInfo(237)` selection endpoints.
- **Window management**: `ActivateClient`, `SetWorldWindowStatus(1..4)`
  (max/min/restore/normal), `GetInfo(125)` fullscreen,
  `GetMainWindowPosition`.
- **Output**: `Pause(1)`/`Pause(0)` (freeze), `DeleteOutput`/clear-output
  menu (confirm + scroll home), `SetOutputFont`, `SetSelection` +
  `GetSelectionStartLine/EndLine/StartColumn/EndColumn`,
  `SetBackgroundColour`.

### 4c-bis. Two small hoists out of `mushview.cpp` (2026-07-29)

Both are meant to be behaviour-preserving refactors of code that the wx port
had reimplemented differently; each replaced an inline body with a call into a
shared TU, so a Windows run is what confirms "behaviour-preserving".

- **`CMUSHView::OnUpdateNeedSelectionForCopy`** is now one line:
  `pCmdUI->Enable (pDoc->EditCopyTarget () != eCopyNothing)`
  (`doc_editcopy_shared.cpp`).  The old body clamped the selection columns to
  their lines' lengths, enabled on a non-degenerate range, and otherwise
  deferred to `m_bottomview->CEditView::OnUpdateNeedSel`.  The shared version
  reads the same clamp from `CWinWorldView::outputSelection` (which is that
  arithmetic, moved) and tests the command window with `CEdit::GetSel`.
  **Check:** Edit ▸ Copy greys and ungreys exactly as before — no output
  selection and no command-window selection → grey; either one → live; a
  single-character output selection → live; a caret (click, no drag) → grey.
  Also check Copy still copies the *output* selection in preference to the
  command window's when both have one, and that Copy as HTML follows the same
  enable state.
- **`CMUSHView::AutoWrapWindowWidth`** now calls
  `CMUSHclientDoc::ClampWrapColumn (iWidth)` in place of its two inline bounds
  checks (`< 20 → 20`, `> MAX_LINE_WIDTH → MAX_LINE_WIDTH`).  Identical
  arithmetic; the shared copy exists because the port's auto-wrap path was
  clamping to a different range.  **Check:** with "auto wrap to window width"
  ticked, resizing the window still re-wraps output and the wrap column still
  saturates at both ends (a very narrow window, and a very wide one with a
  small font).

### 4c-ter. printf format attributes (2026-08-02, `73f30e7`)

Windows-shared headers gained `MC_PRINTF_FORMAT(...)` on TFormat, CFormat,
CString::Format, CMUSHclientDoc::Trace, ThrowErrorException and CErrorException.
The macro expands to **nothing** under MSVC, so the only thing to confirm is that
it compiles: `stdafx.h`, `format.h`, `doc.h` and `exceptions.h` each define the
macro defensively with `#ifndef`, so include order cannot leave it undefined.
**Check:** the Windows build compiles clean, with no new warnings.

Four behaviour changes came with it, all of the same shape — a runtime string was
being used as a printf FORMAT, so a literal `%` in it read a vararg that was never
pushed.  Each now passes the text as an argument instead.  On Windows the old code
misbehaves identically, so these are fixes on both sides, not divergences:

- `CMUSHclientDoc::TraceOut` (`methods_tracing.cpp`) — script-supplied.
  **Check:** with Trace on, `world.TraceOut("100% done: %s")` prints that text
  literally rather than garbage.
- `lua_scripting.cpp`, two `Trace(TFormat(...))` sites carrying a plugin-supplied
  callback name.  **Check:** trace output for a plugin whose callback name
  contains a `%` (rename one to test).
- `exceptions.cpp` `throw new CErrorException(strText)`.  **Check:** an error whose
  substituted text contains a `%` — e.g. open a world file named `50%.mcl` that
  fails to parse — reports the name intact.
- `MUSHclient.cpp` first-run welcome message, `Format(Translate(...))`.
  **Check:** the first-run dialog still shows its text (delete the "seen it" flag).

Also three casts deleted in `mxp/mxpOpenAtomic.cpp`: a `long` was cast to
`LPCTSTR` and then read back by `%ld`.  Same printed value on both ABIs.
**Check:** an MXP `<support>` reply still lists `option=value` pairs correctly.

### 4c-quater. Accelerators moved into the shared engine (2026-08-03)

Three edits, all meant to leave Windows byte-for-byte unchanged:

1. `scripting/methods/methods_accelerators.cpp` — the HACCEL half of
   `AcceleratorTo` is now inside `#ifdef _WIN32`.  Everything Windows used to run
   still runs, in the same order, with one cosmetic difference: the `ACCEL
   aWanted` declaration was hoisted a few lines earlier (it is a POD assigned
   from the same values).  The only reachable behaviour change would be if the
   view-list walk mattered on Windows for something other than finding the
   HACCEL — it does not.
   **Check:** `/Note (Accelerator ("Ctrl+F8", "hello"))` still binds; pressing
   Ctrl+F8 in the world window still executes `hello`; `AcceleratorTo` with an
   empty Send still un-binds; `table.foreach (AcceleratorList (), print)` still
   lists what is bound.  Also check a key bound by a *plugin*, since the plugin
   id bookkeeping (`m_CommandToPluginMap`) sits in the same block.
2. `KeyCodeToString` and `StringToKeyCode` moved from `Utilities.cpp` into
   `utilities_shared.cpp` — same bodies, same project, different TU, so the
   headless engine can link them.  `Utilities.cpp` keeps the
   `extern tVirtualKeyMapping VirtualKeys [];` declaration for its other users.
   **Check:** the accelerator column in the macros page of world preferences,
   and the key names shown in `AcceleratorList`, still read the same.
3. `scripting/methods/methods_accelerators.cpp` moved from the mac-only adapter
   list into `mushcore` in `CMakeLists.txt`.  No effect on the `.vcxproj`, which
   always compiled it.

### 4c-quinquies. Scripting-engine seam (2026-08-03)

Two shared-source changes, both meant to be Windows no-ops:

1. Four MXP hooks (`mxp/mxpStart.cpp`, `mxpClose.cpp` ×2, `mxpError.cpp`) and
   `world.CallPlugin` (`scripting/methods/methods_plugins.cpp`) changed their
   rich-callback gate from `IsLua()` to `UsesEmbeddedCallback()`.  On Windows
   `IsPython/IsPerl/IsJS/IsTcl/IsRuby` are compile-time `false`
   (`scripting/scripting.h`), so the two expressions are identical there.
   **Check:** a Lua world with `OnMXP_OpenTag` configured still receives the tag
   name, the argument text and the argument table; `world.CallPlugin` still
   reaches a Lua plugin.  A VBScript/JScript world should be unchanged — it takes
   the COM arm on both platforms.
2. `xml/xml_load_world.cpp` — five conditions that derive
   `m_bPluginProcessesOpenTag`/`CloseTag`/`SetVariable`/`SetEntity`/`Error` from a
   plugin's callbacks were restored after being disabled by a contaminated
   commit.  The Windows source is back to what it always was.
   **Check:** install a plugin implementing `OnPluginMXPopenTag` and confirm it
   fires on an MXP tag.

3. `LuaError`'s tail (the dialog-vs-output-window routing and the error log) is
   factored into a new free function `ReportScriptError` in the same file, called
   by `LuaError` and by the five embedded engines.  A pure extraction: the body
   is the former tail, unchanged, and Windows compiles both.
   **Check:** with "note errors to the output window" OFF, a Lua run-time error
   still raises the Scripting Error dialog, and ticking its "show errors in the
   output window next time" box still sets the world option; with it ON, the four
   ColourNote lines still appear.  Also that `log_script_errors` still appends to
   `script_error_log.txt`.

4. `doc_construct.cpp` gained two calls, both inside `#ifndef _WIN32`: the
   constructor registers the document with the port's world-proxy liveness
   registry and the destructor deregisters it, first thing.  Windows compiles
   neither.  **Check:** nothing — this is invisible to the Windows build; listed
   only because the file is shared.

5. Three shared-source fixes with Windows-visible edits:
   - `stdafx.h` gains `#define VariantClearOwned(pv) VariantClear(pv)` inside
     `#ifdef _WIN32`, and `scripting/lua_methods.cpp`'s `pushVariant` calls it
     instead of `VariantClear` for a VARIANT it owns.  Identical on Windows.
     `pushVariant` also gained a defaulted `bOwned` parameter, passed `false` for
     SAFEARRAY elements -- on Windows `SafeArrayGetElement` COPIES the BSTR, so
     the old code was already leaking each element copy there; this fixes that
     too.  **Check:** `table.foreach(world.GetOptionList(), print)` still lists
     the options, and repeated `world.GetVariable` in a loop does not grow the
     process.
   - `scripting/lua_methods.cpp` `VariantInit`s the eighteen out-parameters of
     `L_GetTrigger` / `L_GetAlias` / `L_GetTimer`.  **Check:**
     `world.GetTrigger("nosuch")` returns `eTriggerNotFound` and nils.
   - `plugins.cpp` initialises three `bool bResult` to `true`.  **Check:** a
     plugin implementing only `OnPluginConnect` does not suppress sends,
     displays or commands.

`scripting/scripting.h` also gained `CScriptEngine::TraceCallback`, declared
inside the existing `#else` (non-Windows) arm, and its definition in
`scripting/lua_scripting.cpp` is `#ifndef _WIN32`.  Nothing to check beyond a
clean compile.

### 4c-sexies. "Convert wildcards to lower case" reaches the embedded engines (2026-08-03)

The per-trigger option `CTrigger::bLowercaseWildcard` was applied on Windows but
did nothing in five of the port's six languages, because `FixWildcard` applies it
when `evaluate.cpp` fills `trigger_item->wildcards[]` — the array the **COM**
callback and `GetTriggerInfo(101..110)` read — while the embedded engines built
their own list from the regexp, raw. Lua reads the regexp raw on Windows too, so
Lua keeps ignoring the option; that asymmetry is upstream's and is now pinned by
a test on both sides (`tests/test_triggers.cpp`,
`platform/engines_selftest.cpp`).

Shared-source edits, all intended as Windows no-ops:

1. `regexp.h` / `regexp.cpp` gain `t_regexp::GetCallbackWildcards (bool, vector<string>&)`.
   **New function, no Windows caller.** Check: it compiles.
2. `scripting/scripting.h` — the rich `ExecuteLua` declaration and the
   `MC_DECL_EXEC` macro gain a trailing `const bool bLowercaseWildcards = false`.
   Every definition and link-stub was updated to match
   (`scripting/lua_scripting.cpp`, `scripting/scriptengine.cpp`,
   `platform/*_engine.cpp`). Check: MSVC accepts the defaulted argument and the
   declaration/definition pairs agree.
3. `scripting/lua_scripting.cpp` — the Lua body **ignores** the new parameter
   (there is an explicit `(void)` cast and a comment saying why). The forwarding
   block that passes it on is inside `#ifndef _WIN32`.
4. `doc_shared.cpp` — `ExecuteTriggerScript`'s `UsesEmbeddedCallback()` arm now
   passes `NULL` for `result` and `trigger_item->bLowercaseWildcard` after it. On
   Windows `UsesEmbeddedCallback() == IsLua()`, and Lua ignores the flag, so this
   is a compile-only change there.

**Check:** in a Lua world, a regexp trigger with **Convert wildcards to lower
case** ticked and a script function set — the function's `wildcards` table must
still arrive with the ORIGINAL case, while `GetTriggerInfo(name, 101)` returns
the folded one. Then the same trigger in a **VBScript or JScript** world: the
`wildcards` array it receives must still be folded, and still 1-relative with the
whole matching line at index 10. Neither should have changed.

### 4c-septies. Bad UTF-8 bytes are repaired, as Windows already did (2026-08-03)

`OutputBadUTF8characters` (`telnet_phases.cpp`) re-encodes a byte that is not
valid UTF-8 as if it were ANSI — `MultiByteToWideChar(CP_THREAD_ACP)` then
`WideCharToMultiByte(CP_UTF8)`. **The `#ifdef _WIN32` arm is untouched.** The
port's `#else` arm used to pass the raw byte through, which left invalid UTF-8 in
the line buffer and cost two things Windows never loses:

- `ProcessPreviousLine.cpp`'s `_pcre_valid_utf` gate then skipped the line, so a
  single stray byte silently disabled **every trigger** for that line;
- the wx output pane's decode is all-or-nothing, so the stray byte blanked the
  whole coloured run it landed in.

The port now reproduces CP1252 (the ANSI codepage on the Western Windows this
was written for). Also new: `UTF8NextChar` in `utilities_shared.cpp` — a shared
tolerant UTF-8 walker with **no Windows caller**, used by the pane so its
character count and its decoded text come from one place.

**Check:** nothing should differ on Windows. Worth confirming only that a world
in UTF-8 mode receiving a stray Latin-1 byte (e.g. `caf\xE9` from a server
claiming UTF-8) still shows the accented character and still fires a trigger
matching that line — that is the behaviour the port was missing, so it should
already be true on Windows.

### 4c-octies. Command-bar selection offsets are bytes, not characters (2026-08-03)

`CWorldChildFrame::commandSelection` / `setCommandSelection` /
`replaceCommandSelection` (`platform/wx_world_child.cpp`) used wxTextCtrl
CHARACTER offsets, while `GetCommandText()` and every shared body that scans it
index BYTES -- which is also what Windows' `CEdit::GetSel` / `SetSel` are over
its ANSI buffer. The seam now converts, via the new `UTF8ByteToCharOffset` /
`UTF8CharToByteOffset` in `utilities_shared.cpp` (**no Windows caller**).

### 4c-octies-bis. QUESTION TO ANSWER: what shape do wildcards reach a non-Lua callback in?

Not a regression check — an **open design question** that cannot be settled off
Windows. The port's five embedded engines hand a trigger/alias callback a
0-based array: `[0]` = whole match, `[1..n]` = captures, length `n+1`. That is
the shape the port's *own Lua* uses. Windows' COM arm builds something different
(`doc_shared.cpp`, the `eWildcards` SAFEARRAY): 1-relative, always
`MAX_WILDCARDS` (10) long, captures at 1..9, and the **whole matching line at
index 10**.

What we cannot determine from the source is what a *script* actually indexes,
because that depends on the per-language COM bridge. pywin32, for instance,
marshals a SAFEARRAY to a plain tuple and drops the lower bound — if so, a
Windows ActivePython callback sees %1 at index 0 and the whole line at index 9,
a third shape agreeing with neither.

**Please determine, with ActivePython (or PerlScript / ActiveState Tcl — any
non-Lua, non-VBScript ActiveScript engine) installed:** in a trigger callback
with two captures, print the wildcards parameter's length and every index. Report
(a) the lowest valid index, (b) the length, (c) which index holds the whole
matching line, and (d) what an out-of-range capture reads as. Then the port can
match the shape a real Windows script sees rather than the shape the C++ builds.

**Check on Windows:** nothing changed there, but this pins the contract the
Windows adapter already satisfies -- with a non-ASCII character early in the
command bar, Tab completion must complete the word under the caret without
mangling the line, and `GetInfo(236)`/`GetInfo(237)` fed back into
`world.SetCommandSelection` must reselect the same text.

**Port verification debt:** the conversions themselves are covered headlessly
(`tests/test_utilities.cpp`, `Utf8OffsetTest`), but their *application* in those
three wx functions needs a real `wxTextCtrl` and has not been exercised -- there
is no output-pane/world-window in-app selftest yet.

### 4c-nonies. Text of unproven validity no longer decodes all-or-nothing (2026-08-03)

**Port-only files** (`platform/wx_notepad.cpp`, `wx_mfc_impl.cpp`,
`wx_world_child.cpp`) — nothing for Windows to check, listed for completeness.
Three more `wxString::FromUTF8` sites were handed bytes that are not guaranteed
valid UTF-8, and FromUTF8 returns an EMPTY string rather than partial text:

- notepad append/replace and notepad creation (world output, or arbitrary script
  bytes via `world.AppendToNotepad`);
- notepad **Convert ▸ Base64 decode**, whose output is arbitrary binary — the
  result was `SetValue("")`, i.e. the conversion silently ERASED the notepad.

All now use `DecodeUTF8Lenient`. Separately, the MXP tooltip run-walk in
`wx_world_child.cpp` measured runs with `run.text.size()` (BYTES) against a
rendered CHARACTER column, so the wrong run's hint was returned after the first
non-ASCII run; it now uses `RunCharLen`, as the hyperlink walk beside it already
did.

**Port verification debt:** the decoders themselves are covered headlessly
(`Utf8NextCharTest`), but these four call sites are GUI-only. The notepad text
area is not exposed to AX, so text cannot be placed in one without synthesising
global keystrokes — see the notepad note in CLAUDE.md's verification debt.

### 4c-decies. Connection menu update-UI now matches doc.cpp (2026-08-03)

**Port-only** (`platform/wx_mfc_impl.cpp`), listed because the predicates were
copied from `doc.cpp` and should stay in step:

- **Disconnect** was enabled only at `eConnectConnectedToMud`; Windows enables it
  for every non-zero phase (`doc.cpp:1913`). Phases 1-8 — the two name lookups,
  the connect, the three SOCKS waits, the SSL handshake — are precisely when a
  user wants to cancel, and the port greyed it out there. §4a's
  "cancel-during-connect" scenario was unreachable from the menu.
- **Connect** omitted Windows' `m_server != "0.0.0.0"` guard (`doc.cpp:1904`), so
  it was offered on a world with no server configured.

**Port verification debt:** the connecting-phase window is transient and the
update-UI path is GUI-only, so this is verified by inspection against `doc.cpp`
rather than by exercise. The structural fix would be to route these through the
canonical `CMUSHclientDoc::OnUpdate*` handlers (as `kCanonCmds` already does for
other commands) instead of restating the predicate — worth doing if a third
divergence turns up here.

### 4c-undecies. Command-entry path: history ordering, Repeat, PasteCommand (2026-08-03)

**Port-only** (`platform/wx_world_child.cpp`, plus the `#ifndef _WIN32` arm of
`scripting/methods/methods_commands.cpp`), but these were written by reading
`sendvw.cpp` and are **not exercised by any test** — the whole command-entry path
is GUI-only here. Please try to break them.

1. **Command history is now recorded AFTER the command runs** (was: before
   everything). Windows does the add at `sendvw.cpp:736`, after `Execute`, which
   is what makes three things work — and all three were broken here:
   - an alias with **omit from command history** (`evaluate.cpp:896` sets
     `m_bOmitFromCommandHistory` *during* evaluation) — nothing in `platform/`
     read that flag at all, so the option was write-only;
   - a plugin's `OnPluginCommandEntered` returning `"\t"` (discard) no longer
     leaves the command in history;
   - a plugin that REWRITES the command now has the rewritten text recorded, not
     what was typed.
   The recorded string is the post-plugin, pre-auto-say-strip, pre-backslash
   text, matching `strOriginalCommand`. `Execute() != eOK` now returns early
   without recording, as `sendvw.cpp:729` does.
2. **Input ▸ Repeat Last Command** went through `EvaluateCommand` directly;
   Windows routes it through the ordinary send path (`SendCommand(strText,
   TRUE)`, `sendvw.cpp:1818`). It now does auto-say, backslash translation, the
   depth reset, `Execute` and the history re-add.
3. **`world.PasteCommand`** was a silent no-op whenever the command bar was not
   empty (it went through `setCommand`, which refuses a non-empty bar) and
   always returned `""`. It now splices at the caret / over the selection and
   returns the previously selected text, as `CEdit::ReplaceSel(Text, TRUE)` does.

**Where these could be imperfect — worth checking specifically:**
- Does Windows record history for a command that **auto-say** consumed? Read as
  yes (the add sits after both arms of the auto-say if/else), and the port now
  matches — but this was inferred from indentation, so confirm it.
- The port deliberately does NOT honour `always_record_command_history`
  (`wx_world_child.cpp:369`): its history is persisted to a **plaintext file**,
  so it never records while echo is off. That divergence was pre-existing and is
  intentional — do not "fix" it to match Windows.
- `PasteCommand` relies on the command-bar selection seam being UTF-8 **byte**
  offsets (see § 4c-octies). If Windows disagrees about what `GetInfo(236/237)`
  measures, both change together.
- Repeat Last Command's history re-add dedups against the tail, so repeating the
  most recent command is a no-op for the list. Confirm Windows does the same
  (`AddToCommandHistory` skips `strCommand == m_last_command`, so it should).

### 4c-duodecies. world.SetScroll: pixels, and the Visible flag (2026-08-03)

**Port-only.** Two defects in the same method:

- `CWorldChildFrame::setScroll` treated its argument as a **1-based line**;
  Windows treats it as a **device-pixel Y** (`methods_output.cpp:766-786`
  `pt.y = Position; ScrollToPosition(pt, false)`, repeated verbatim in
  `pal/win_worldview.cpp`). The port contradicted itself too — `GetInfo(296)`
  returns `scrollPos()` in pixels, so reading the position and scrolling back to
  it landed roughly line-height times too far up. Now divides by the line
  height.
- `SetScroll`'s `Visible` argument was discarded, and `m_bScrollBarWanted` has
  no other writer besides `doc_construct.cpp:136`, so `GetInfo(120)` reported a
  hard-coded `true`. It is recorded now. The port still does not *hide* the
  scrollbar (a `wxScrolledWindow` that can scroll shows one) — a cosmetic
  difference, but no longer a lie about the setting.

**Worth checking on Windows:** that `world.SetScroll(world.GetInfo(296), true)`
is a no-op — i.e. that 296 and SetScroll really are the same unit there. The
whole fix rests on that reading.

**Note for future work:** `platform/doc_gui_method_stubs.cpp` carries a
**duplicate** `SetScroll` for the headless test link, and the two had drifted —
the stub dropped `Visible`, so the test suite could not observe the product's
behaviour. They are aligned now, but that file is a standing twin-drift hazard;
CLAUDE.md's endgame is to retire the family.

### 4c-terdecies. Miniwindow mouse events + two drawing fixes (2026-08-03)

**Port-only** (`platform/wx_outputpane.{h,cpp}`, `wx_world_child.cpp`). Quow's
pack is the workload that exercises all of these — hotspots with mouse
callbacks. Five gaps against `mushview.cpp`:

- **Middle button never dispatched.** `MW_MOUSE_MIDDLE` (0x200) appeared nowhere
  in `platform/`; there was no `EVT_MIDDLE_DOWN` binding at all, and middle-up
  went straight to a clipboard paste. Windows dispatches both edges
  (`mushview.cpp:2399`, `:2517`). Now dispatched on both.
- **Ctrl+wheel tested the modifier first**, so it zoomed the output font and the
  hotspot scrollwheel callback was unreachable while the pointer was over a
  miniwindow. Windows dispatches to the hotspot first (`mushview.cpp:5160`) and
  only falls back to the view at `:5165`; the flags a plugin receives include
  `MW_MOUSE_CTRL`. Order reversed to match.
- **Right double-click was swallowed** — wxOSX sends the second rapid click as
  `EVT_RIGHT_DCLICK` and nothing bound it, so it reached neither the hotspot nor
  the context menu. Windows has `OnRButtonDblClk` dispatching
  `MW_MOUSE_RH | MW_MOUSE_DBL` (`mushview.cpp:4893`).
- **No leave-window handler**, so `CancelMouseOver` never fired and a hotspot
  stayed lit after the pointer left the pane. Windows arms `TME_LEAVE` and its
  `WM_MOUSELEAVE` arm passes `CPoint(-1,-1)` (`mushview.cpp:6785`).
- **Hotspot tooltips showed `"title\tbody"` raw**, tab and all. Windows splits
  on the tab, putting the first part in the tooltip's bold title
  (`mushview.cpp:6601-6620`). `wxToolTip` has no title, so the port joins with a
  newline — the point is that the tab must not survive.

Plus: search-highlight text was drawn with one batched `DrawText`, flowing at
the font's natural advances while its background rectangle was laid out on the
integer character grid, so highlighted text stepped out of its own highlight.
Now per-cell, like `DrawRunChunk`.

**Worth checking / deciding on Windows:**
- Middle-click over the output window still pastes here when no hotspot claims
  it. On GTK that is the primary-selection convention and deliberate; on macOS
  it is neither a platform convention nor Windows behaviour (`CView`'s middle
  handlers default to nothing). **Is the macOS paste wanted?** Pre-existing, not
  changed here.
- Ctrl+wheel now reaches hotspots first, so a plugin that claims the wheel takes
  the font-zoom gesture with it while the pointer is over its window. That is
  Windows behaviour, but confirm it does not feel broken with Quow's pack
  loaded.

### 4c-quaterdecies. DDV_MinMax* validators actually validate now (2026-08-03)

**Port-only** (`platform/mfc_ui.h`), but with a **real regression risk worth
watching for** — see below.

All five `DDV_MinMax*` overloads were `{}`. That made every numeric range in
every property page advisory across **78 call sites**: a value outside the
declared range was accepted, written to the doc member and saved to the world
file, with nothing downstream re-checking it. They now do what MFC does — show
"Please enter an integer between X and Y." and call `pDX->Fail()`, which aborts
the exchange so the dialog stays open and the member keeps its old value.
(Measured on Windows — see §8t. This paragraph used to say "a number", which is
MFC's wording only for the *real* overloads; the port said that for all five
until `mfc_ui.h` was corrected.) The
load direction (`m_bSaveAndValidate == FALSE`) still never validates, so a world
that *already* holds an out-of-range value can still open its property page.

**The risk:** any existing world file or default that sits outside a declared
range will now block OK on that page, where before it sailed through. That is
Windows behaviour, so a Windows build should reject the same values — but if a
page here refuses to close on values Windows accepts, the range constant or the
member's default is wrong and that is the real bug.

**Smoke-checked on macOS (2026-08-03)** against a real, long-lived world
(Discworld, Quow's plugin pack loaded): World Properties opened, six numeric
pages were visited — Output, Paste, Send, Commands, Timers, Printing, and
switching pages already runs `UpdateData(TRUE)` on the outgoing one — and **OK
closed the dialog cleanly**, with the world file byte-identical afterwards. So
no stock setting trips a validator here. Still worth the same pass on Windows
with a differently-configured world.

### 4c-quindecies. Two display/log corruption fixes (2026-08-03)

**Port-only.**

1. **A palette change re-appended the whole scrollback to an open session log.**
   `RebuildOutputView` clears the pane and re-emits every line, and the port
   forwards every line the pane accepts to `LogLine` — so with logging on, one
   ANSI-colour tweak (or a plugin calling `world.SetBoldColour`) appended a
   verbatim second copy of the entire buffer to the log file, a third on the
   next tweak, and so on. The log hook is now detached across the rebuild. The
   engine's own File ▸ Log Session path was never affected.

2. **Omit-from-output did not rewind `m_gui_lines_emitted`.** The non-Windows
   drain derives what to paint from that counter, and every other deletion site
   rewinds it (`DeleteLines`, `RemoveChunk`); the gag path was the outlier.
   **Honest status: I could not reproduce a failure.** Two attempts at the
   reported scenario — a gag on a wrapped paragraph arriving in two reads with a
   drain between — did not produce a deficit, because in that flow the paragraph
   is deleted before the drain paints it, and removing the fix does not fail the
   test. So this is a **consistency fix against a latent hazard**, not a
   confirmed bug: the counter can no longer exceed what the list justifies. If a
   Windows session ever shows lines going missing after a gag, this is the first
   place to look; equally, if it never does, the fix costs nothing.

### 4c-sexdecies. Mono pattern brushes keep their colours (2026-08-03)

**Port-only** (`platform/wx_surface.cpp`). GDI colours a 1-bpp pattern brush
from the DC — 0 bits take the text colour, 1 bits the background — but wx's
`CreatePatternBrush` ignores `SetTextColor`/`SetBkColor` entirely and painted the
stipple literally black and white. So `WindowCircleOp` / `WindowPolygon` brush
styles **8-12** came out monochrome whatever colours the plugin asked for. The
8x8 tile is now built already coloured (64 pixels; cheap).

**Two things this does NOT fix, both found while testing it:**

1. ~~**The pattern's SHAPE still collapses.**~~ **Also fixed** (same day). `kFine`
   is a 50/50 checkerboard (`0xAA`/`0x55`) and only one colour was appearing. I
   first blamed the HiDPI read path; making that exact did not help, which
   located the fault in the DRAW — wx scales an 8x8 *logical* stipple onto the 2x
   canvas and the alternating pixels average away. The tile is now built at
   **physical** resolution (each pattern pixel replicated across the scale x
   scale block, with the bitmap's scale factor set so its logical size is still
   8x8), and the selftest asserts both colours survive.
2. **Transparent gaps** (`bg == -1`) are not expressible as a stipple without a
   mask, so they currently paint the gap in the foreground colour instead of
   leaving the destination alone. Windows leaves it alone.

Worth confirming on Windows what styles 8-12 actually look like with distinct
pen and brush colours, so there is a reference to match.

### 4c-septendecies. HiDPI: BGR24 pixel READS are exact now (2026-08-03)

**Port-only** (`platform/wx_surface.cpp`). `readPixels`' BGR24 path blitted the
canvas into a temporary **unscaled 1x** DIB, and miniwindow canvases are created
at the display scale (`CreateCanvasBitmap`) — so on a Retina display every read
resampled each 2x2 block of physical pixels down to one. `getPixel` had already
been routed around this and its comment explained why; every other BGR24 caller
(`CMiniWindow::Filter`, the image-alpha ops) still went the old way. BGR24 reads
now go through the same direct backing-bitmap path the alpha format uses and
drop the alpha channel.

**The WRITE half is done too** (same day): `wxgdi_write_bgra` writes the backing
bitmap directly, replicating each logical pixel across the scale x scale
physical block — the exact inverse of the read's top-left sampling, so the
round-trip is lossless. Pinned by a miniwindow-selftest case that paints
alternating 1px columns of different colours, runs `WindowFilter` with an
identity brightness, and asserts both columns come back byte-identical; it fails
when either half falls back to the BitBlt path.

### 4c-duodevicies. Four command-bar commands hoisted out of `CSendView` (2026-08-03)

**Windows code changed** — `sendvw.cpp` (4 sites) and
`pal/win_worldview.cpp` (1). New shared TU `doc_commandbar_shared.cpp`, added to
`MUSHclient.vcxproj`. Nothing was meant to change on Windows; all four bodies
moved verbatim onto `CMUSHclientDoc`, reaching the bar/history through
`pal::IWorldView` (whose Windows adapter reads the very `CSendView` members the
originals used). The port had no equivalent for any of them, which is why they
moved.

| Was | Now | Reached by |
|---|---|---|
| `CSendView::OnRepeatLastWord` | `CMUSHclientDoc::RepeatLastWord` | Edit ▸ Recall Last Word / Ctrl+Backspace |
| second half of `CSendView::OnInputGlobalchange` | `CMUSHclientDoc::GlobalChangeCommand` | Input ▸ Global Change |
| the caret gate in both arrow arms of `PreTranslateMessage` | `CMUSHclientDoc::CommandCaretAtEdge` | Up / Down in the command bar |
| the script-prefix test in `OnChar`'s spell-check arm | `CMUSHclientDoc::IsScriptPrefixCommand` | Return, with "spell check on send" |

**What to check on Windows:**

1. **Recall Last Word, both arms.** With `ctrl_backspace_deletes_last_word`
   OFF (the shipped default): send `get sword from bag`, type `put ` and press
   Ctrl+Backspace — expect `put bag`. Turn the option ON: type
   `get sword from`, Ctrl+Backspace — expect `get sword` with the caret at
   column 9 (the space goes too). With a selection and the option ON, the
   selection is simply deleted.
   The delete arm is expressed differently: upstream selected the whole control
   and re-inserted `Left(iPos) + Mid(nStartChar)`, then moved the caret to
   `iPos`; it is now the single splice `ReplaceSel("")` over
   `[iPos, nStartChar)`. Same text, same caret, one undo record instead of a
   whole-control rewrite — **confirm undo (Ctrl+Z) restores the deleted word.**

2. **Global Change.** Select part of the command bar, run Input ▸ Global
   Change: only the selection changes, and Ctrl+Z undoes it. With nothing
   selected the whole line changes. A from-string that matches nothing still
   pops "No replacements made for …" — with the string **as typed**, before the
   backslash escapes are expanded (the message now comes from `OnInputGlobalchange`
   rather than from inside the moved body).

3. **Multi-line command bar.** Compose two lines with Shift+Return, put the
   caret in the middle, press Up/Down: the caret moves, the text is untouched.
   From the very start or the very end the arrows browse the history as before.

4. **Line endings.** `CWinWorldView::replaceCommandSelection` now promotes a
   lone `\n` to `\r\n` before `ReplaceSel` — this is where
   `OnInputGlobalchange`'s `strNew.Replace ("\n", ENDLINE)` went, so the shared
   body can stay platform-free (wxTextCtrl wants bare LF). Check a Global
   Change whose replacement contains `\n`: the bar must show two real lines,
   not a stray control character. Tab completion is the other caller and never
   passes a newline, so it should be unaffected — smoke it anyway.

5. **Spell check on send.** With a script prefix configured and "spell check on
   send" on, a line starting with the prefix must not be checked; an ordinary
   misspelled line must be, and Cancel must abort the send with the text still
   in the bar. (Unchanged on Windows — the same expressions, now behind a
   predicate — but it is the cheapest of the five to confirm.)

### 4c-undevicies. WindowAddHotspot's re-detect goes through IWorldView (2026-08-03)

**Touches shared code AND Windows.** `CMUSHclientDoc::WindowAddHotspot`
(`scripting/methods/methods_miniwindows.cpp`) ends with the
"was the mouse already over the hotspot I just created?" arm. It used to walk
this doc's view list for a `CMUSHView` and call `Mouse_Move_MiniWindow`; the
ports never construct a `CMUSHView`, so it was dead there (a plugin that
rebuilds its hotspots on every redraw lost the hover highlight under a
motionless pointer). It now calls the new
`pal::IWorldView::mouseMoveMiniWindow(x, y)`, and **`CWinWorldView`
(`pal/win_worldview.cpp`) contains that same view walk verbatim**, so Windows
should behave identically. Nothing here can be compiled on macOS.

What to check on Windows: a plugin miniwindow with a hotspot whose `MouseOver`
callback is visible (a highlight repaint or a `Note`). Park the pointer over
the hotspot without moving it, then have the plugin call
`WindowDeleteAllHotspots` + `WindowAddHotspot` (e.g. on a trigger). The
`MouseOver` must fire immediately, exactly as before this change, and
`WindowInfo(win, 19)` must name the hotspot. Also confirm that adding a
hotspot **away** from the resting pointer fires nothing.

### 4c-vicies. `world.Bookmark` relocated into the shared engine (2026-08-04)

**Windows code changed** — the body moved from
`scripting/methods/methods_output.cpp` into `doc_shared.cpp`, and the
"alter appearance of all output windows" `CMUSHView` walk at the end of it is
now `pal::IWorldView::setBookmark`, with **that same walk verbatim** in
`CWinWorldView` (`pal/win_worldview.cpp`). The flag write on the `CLine` is
untouched. It had to move because `methods_output.cpp` is GUI-only, so the
headless build linked an inert stub and nothing could test the real body — and
because the port kept an entirely separate bookmark store that `world.Bookmark`
never reached.

What to check on Windows: `world.Bookmark(n, 1)` marks line *n* (the bookmark
gutter/highlight appears), `world.Bookmark(n, 0)` clears it,
`world.GetLineInfo(n, 7)` agrees with what is drawn in both directions, and
Display ▸ Bookmark Selection still toggles. Out-of-range line numbers must
still do nothing at all.

### 4c-unvicies. Unread-line accounting reads one predicate (2026-08-04)

**Windows code changed, but provably equivalently** — `doc_shared.cpp`'s
`StartNewLine` tested `m_pActiveCommandView == NULL && m_pActiveOutputView ==
NULL` inline; it now calls `!IsWorldViewActive()`, and the Windows definition of
that (`doc.h:1177`) is `m_pActiveCommandView != NULL || m_pActiveOutputView !=
NULL`. De Morgan says these are the same expression, and no other Windows code
moved — the port needed the seam because neither `CView` pointer is ever set
there.

What to check on Windows: with two worlds open, output arriving in the
**background** one bumps its Activity-window unread count; switching to it
clears the count. That is the only observable, and it must be exactly as before.

### 4c-duovicies. `CMiniWindow::Menu` gained a defaulted `pDoc` (2026-08-04)

**Signature change only on the Windows path.** `Menu(Left, Top, Items, pView)`
became `Menu(Left, Top, Items, pView, pDoc = NULL)`; the new argument is read
only inside `#ifndef _WIN32`, where there is no `CMUSHView` to convert the
anchor through. Windows still converts via `pView` on exactly the same lines.
`CMenu::TrackPopupMenu` also stopped discarding its `flags` argument (it was
`UINT /*flags*/` in the port's shim, `mfc_ui.h` — not a Windows file).

What to check on Windows: `world.WindowMenu(win, x, y, "~lt|One|Two")` and the
other `~` anchor prefixes put the menu where they always did, from a click and
from an alias/timer alike.

### 4c-tervicies. `SpellCheckString` gained an optional out-parameter (2026-08-04)

**Windows-visible signature, no Windows caller.**
`CMUSHclientApp::SpellCheckString(strText, bAll)` became
`(strText, bAll, bool* pbCancelled = NULL)`. Nothing on Windows passes the third
argument — `CSendView::OnChar` reaches the same fact through `SpellCheck()`
returning false — so the only Windows risk is the recompile. The port needs it
because it has no `SpellCheck(CWnd*, CEdit*)` equivalent and could not otherwise
tell "user cancelled" from "checked clean" (both hand back the original text).

What to check on Windows: spell check on send still works, and Cancel in the
suggestion dialog still aborts the send with the text left in the bar.

### 4c-quatervicies. Things I could NOT verify here at all (2026-08-04)

Recorded so a Windows run knows where to look rather than re-deriving it. All of
these are macOS-side fixes whose *Windows* arm was read but never executed:

- **The freeze-on-selection reading.** The port now freezes output whenever a
  non-empty selection exists, from `!get_selection (newrgn) && pDoc->m_bAutoFreeze`
  at `mushview.cpp:2453`, `:2613` and `:4837`, plus `pal/win_worldview.cpp`'s
  `setOutputSelection`. `get_selection` returning **true for an empty region**
  (`mushview.cpp:2765`, `:2826`) is what makes that condition mean "there IS a
  selection", and it is the whole basis of the port's `FreezeIfSelection`. If it
  reads the other way on a live Windows build — i.e. if dragging a selection on
  Windows does **not** freeze output — the port is now wrong in a very visible
  way and the four call sites should come back out.
- **`keep_pause_at_bottom` as a latch.** Read from `ScrollToPosition`
  (`mushview.cpp:4680`): with the option on, `m_freeze = m_freeze || (pt.y <
  bottom)`, so arriving at the bottom never unfreezes. Confirm on Windows that
  scrolling back down with the option ticked really does leave output paused,
  and that the hint bar / Pause command is what resumes it.
- **The double-click ordering.** The port freezes, then releases capture, then
  dispatches the word, then auto-copies — and skips the copy on
  `eDblClickAbort`. That is `OnLButtonDblClk`'s order as read
  (`:4837` freeze, `:4860` the abort `return`, `:4884` the copy). Worth watching
  once on Windows with "copy selection to clipboard" on: double-click a word
  while disconnected, cancel the "not connected" prompt, and confirm the
  clipboard is **not** touched.
- **`world.TextRectangle`'s 20-pixel floor.** `pal/text_rect.h` implements
  `max (right, left + 20)` from `GetTextRectangle`. An offset larger than the
  window is the only way to reach it; a plugin is unlikely to, but if the
  Windows floor turns out to be applied at a different point the port's wrap
  column will differ by a character or two in that corner.

### 4c-quinvicies. Unbounded scrollback: the jump index and the 0 sentinel (2026-08-04)

**Substantial Windows-shared change** — `doc.h`, `doc.cpp`, `doc_shared.cpp`,
`ProcessPreviousLine.cpp`, `methods_output.cpp`, `methods_noting.cpp`,
`xml/xml_save_world.cpp`, `xml/xmlparse.cpp` callers, and the world-prefs page.
`CMUSHclientDoc`'s layout changes, so Windows needs a full rebuild too.

**1. `m_pLinePositions` is now `CLinePositionIndex m_LinePositions`**
(`line_position_index.h`, new, added to the vcxproj). Was a `POSITION` array
sized from the buffer cap but indexed by the live line count; now a `std::deque`
of chunk heads indexed relative to the current list head. Behaviour is meant to
be identical: a chunk may still be NULL, `GetLinePosition` still falls back to
chunk *n*−1 and then to the list head, and "allocated" is still a distinct state
from "empty" (the guard several callers use for "has `SetUpOutputWindow` run").

What to check on Windows — all of it should be invisible, which is the point:

1. Open a world, let the buffer fill past `max_output_lines` (set it to 500 to
   get there fast) and keep it running. Scrollback, scrollbar, Find, Recall and
   Go To Line must all behave exactly as before. A renumbering error here does
   **not** crash: `GetLinePosition` starts from the wrong chunk head and walks
   forward, so `world.GetLineInfo(n, 1)` returns a line off by a multiple of
   100. Spot-check a few line numbers against what is on screen.
2. World Properties ▸ Output ▸ lines: change it up and down on a full buffer
   (this runs `FixUpOutputBuffer`, which rebuilds the index from scratch).
3. `world.DeleteLines(n)` and a gagging trigger (`omit_from_output`) both
   invalidate chunk heads — run a trigger that gags on a busy world and confirm
   the scrollback stays coherent.

**2. `max_output_lines = 0` now means unlimited.** New option flag
`OPT_ZERO_UNLIMITED` (`doc.h`), honoured by `SetBaseOptionItem`
(`doc_shared.cpp`) — zero is legal below the stated minimum — and the ceiling
rose from 500,000 to 100,000,000. Nothing else in the table carries the flag, so
no other option changes.

Three knock-on edits that are worth a specific look because they touch generic
machinery:

- **`Save_XML_number` gained a `bWriteZero` parameter** (default false, so every
  existing caller is unchanged). It is passed true only for `OPT_ZERO_UNLIMITED`
  options. Needed because a zero is normally omitted from the world file and the
  loader keeps the member's current value for an absent numeric attribute — so
  without this, "unlimited" silently came back as 5,000 on the next load.
  **Check a save/reload of an ordinary world produces a byte-identical file** to
  one saved before this change (no stray `="0"` attributes anywhere).
- **The world loader widens the minimum to 0 for `OPT_ZERO_UNLIMITED` options**
  (`xml_load_world.cpp`) and lets `SetOptionItem` apply the real "0, or at least
  200" rule. A world file with `max_output_lines="5"` is still rejected — but now
  by `SetOptionItem` returning `eOptionOutOfRange` rather than by
  `Get_XML_number` throwing, so **the error the user sees changes**: previously a
  per-option exception (caught, option skipped, default kept), now a silent skip.
  Same outcome, quieter path. Worth confirming a hand-edited bad value still
  leaves the world loadable.
- **`DDV_MinMaxLong` on the prefs page is now conditional** on the value being
  non-zero, and a new confirmation appears when 0 is entered ("…means UNLIMITED
  scrollback… Continue?"). Check that a non-zero out-of-range value still gets
  the standard MFC range message, and that Cancel on the new prompt leaves the
  page open with the value intact.

**3. The disconnect statistics line** (`doc_shared.cpp`) now prints
"Output buffer has N lines in it (no limit set)." when unlimited, instead of the
"N/M lines (x% full)" form — which would have divided by zero. Ordinary bounded
worlds must still print the percentage exactly as before.

**Not yet done, and deliberately:** the scroll geometry is still 32-bit
(`platform/wx_outputpane.cpp` is port-only, so this is not a Windows concern),
and `CMUSHView`'s own scroll sizes on Windows are unchanged. Setting 0 on a
Windows build will therefore run into MFC's own limits long before the engine's;
that is expected and is not what this change is claiming to fix.

### 4c-sexvicies. Two latent MXP-entity hazards, deliberately untested (2026-08-04)

Both found while writing coverage for `mxp/mxpEntities.cpp`. Neither is fixed
and neither has a test, for reasons that matter:

1. **`MXP_GetEntity` reads `strName[1]` without a length check** (`:76`). For
   `strName == "#"` — a MUD sending the three bytes `&#;` — that indexes one past
   the last character. On the port it is defined behaviour: `mfc_string.h`'s
   `operator[]` is a plain `std::string` subscript, and `std::string::operator[]
   (size())` legally returns the NUL. **On Windows, real MFC's
   `CString::operator[]` calls `GetAt`, which ASSERTs on an out-of-range index in
   a debug build** — so `&#;` from a hostile or buggy MUD fires an assertion
   there and does nothing here.

   No test was written because a test that is green on macOS and asserts on
   Windows is worse than none. **What to check on Windows:** connect a debug
   build to something that can send a bare `&#;` (a scratch MUD or a
   `world.Note` through the MXP path) and see whether it asserts. If it does,
   that is an upstream bug worth a length guard.

2. **`isxdigit` / `isdigit` / `toupper` are passed a plain `char`** (`:80`,
   `:89`, `:106`). A high-bit byte inside an entity — `&#x` followed by 0x80-0xFF
   — is a negative `int` to the ctype functions, which is undefined per the
   standard. macOS's `__maskrune` happens to bounds-check, so it does not
   misbehave here; MSVC's implementation historically indexes a table and can
   read out of bounds. Not assertable from a test, since the well-defined
   behaviour and the undefined one look identical when the platform is lenient.

### 4c-septemvicies. A new `CLine::flags` bit, and the trigger grouping test (2026-08-04)

The macOS "Clear to Start" command needs to know which line is the server's
prompt, so `MarkPromptLine` (`telnet_phases.cpp`, called from the IAC GA and
IAC EOR arms of `Phase_IAC`) now stamps a new **`PROMPT = 0x20`** bit on that
line and records its number in a new `CMUSHclientDoc::m_last_prompt_line`. Both
are in shared source, so **Windows sets the bit too** — nothing there reads it
yet.

**Why this is on the list.** `ProcessPreviousLine` decides which soft-wrapped
segments form one logical line for trigger matching by comparing whole
`CLine::flags` values (`:188`). Every *other* consumer of that field tests
individual bits, so a new bit is invisible to them; this one comparison is not.
And PROMPT is stamped *after* the prompt's text has arrived — the GA byte comes
last — so on a prompt wider than the wrap column it lands on the final segment
only. Compared naively, the group would end at that segment and a trigger would
match a tail fragment instead of the whole prompt. `PROMPT_GROUPING_MASK`
(`OtherTypes.h`) is what prevents it.

Pinned headlessly by
`TriggerR5.WrappedPromptMatchesAsOneLogicalLineDespiteThePromptFlag`, which
fails if the mask is widened to `~0`. That test runs the real shared code, so
the risk of a Windows-only difference is low — but trigger matching is the
hottest user-visible path in the app, so:

**What to check on Windows:** connect to a MUD that sends IAC GA (most do) with
`convert_ga_to_newline` **on**, a wrap column narrower than the prompt, and a
trigger whose pattern spans the prompt's wrap boundary. It must still fire. Do
the same with the option **off** (the default) to confirm the unterminated
prompt behaves exactly as before. Then check any trigger that matches ordinary
wrapped output still fires — the mask must not have loosened grouping either.

Also here: `ClearOutput` now zeroes `m_last_line_with_IAC_GA` alongside
`m_total_lines` (`docs/UPSTREAM-BUGS.md` #11). `world.GetInfo(289)` therefore
returns 0 after a Clear Output Buffer instead of a phantom line number.
**Check:** `GetInfo(289)` before and after Clear Output; the "before" value must
be unchanged from today.

### 4c-duodetricies. DBCS wrap with "indent paragraphs" off (2026-08-05)

`docs/UPSTREAM-BUGS.md` #10, fixed rather than merely recorded — **this one
deliberately changes what Windows draws.**

`AddToLine`'s `!m_indent_paras` block moved the split byte back onto the old
line unconditionally. That is right for a space (the continuation must not
begin with one) and wrong for a DBCS lead byte, where it stranded the trail
byte at the head of the next line. The adjustment is now gated on which arm
found the split point.

**What to check on Windows:** a non-UTF-8 world (Big5 or GB2312 encoding), a
narrow `wrap_column`, "indent paragraphs" **off**, and CJK text long enough to
wrap several times. Before: occasional mojibake at the right margin, one torn
character per wrap, worse when an odd-length ASCII prefix puts the pairs on odd
offsets. After: every wrap falls on a character boundary. The wrap now falls up
to one byte earlier, so line breaks in such a world will move — that is the fix
working, not a regression.

**And the control:** with "indent paragraphs" off and ordinary ASCII text, a
line that wraps at a space must still keep that space at the END of the old
line, exactly as before. If continuation lines start showing a leading space,
the gate is wrong in the other direction.

### 4c-undetricies. The TLS failure recovery is now one body (2026-08-05)

**`mainfrm.cpp` is edited here** — one of the few Windows-only files this port
has touched — so it needs a compile before anything else.

`CMainFrame::PreTranslateMessage`'s `WM_USER_SSL_FALLBACK_PROMPT` arm and the
port's equivalent (`platform/wx_mfc_impl.cpp:499`) were hand-written twins.
Each decided, on its own, what the user's answer to a TLS failure prompt
*means*: a certificate that could not be verified sets
`m_bSSLAllowInvalidCert` and stays **encrypted**, while a failed handshake sets
`m_bUseSSL = false` and reconnects **in the clear**. Two copies, no test on
either platform, and a mistake is invisible — no error, no output, the world
simply connects.

Both now call `CMUSHclientDoc::ApplyTlsFallbackChoice(bCertFailure,
bUserAgreed)` (`doc_shared.cpp`), which owns the decision; the frontends keep
only the wording of the question and the reconnect. Behaviour is intended to be
identical.

**What to check on Windows:**
1. It compiles — the change is six lines in `mainfrm.cpp` and could not be
   compile-checked here.
2. Connect to a TLS world with an untrusted/self-signed certificate. Answer
   **Yes**. The connection must proceed **and still be encrypted** — confirm
   with `world.GetInfo(...)` for the SSL state, or the world's own report.
   This is the one that matters: the wrong outcome is plaintext, and nothing
   on screen would say so.
3. Answer **No** to the same prompt: nothing should reconnect and no flag
   should change.
4. Force a handshake failure (an old TLS version, or a non-TLS port with SSL
   on). Answer **Yes**: it must reconnect unencrypted, and it must NOT leave
   `m_bSSLAllowInvalidCert` set behind.

Covered headlessly by the four `Connection.Tls*` tests, including a 2x2 table
that fails if both arms are ever made to do the same thing.

### 4c-duodetricies-bis. The command queue has one drain now (2026-08-05)

**`TimerWnd.cpp` is edited** — needs a compile.

`CTimerWnd::OnTimer`, `CTimerWnd::ChangeTimerRate`'s flush arm and the port's
replica were three copies of the same drain. All three now call
`CMUSHclientDoc::DrainOneQueuedSlot` / `DrainAllQueuedCommands`
(`doc_shared.cpp`). Two deliberate behaviour changes come with it, both agreed:

**1. "No log" now actually suppresses logging (UPSTREAM-BUGS #12).** The log
flag was recovered as `cMessageType >= 'A'`, and all eight flag bytes are
letters, so it was always true. It is now `c < 'a'`.
**Check:** open a session log, set a speed-walk delay, and send a speed-walk
through a path that suppresses logging (a plugin or script calling `SendNoEcho`
/ the no-log `Send` variants). Those commands must now be ABSENT from the log
file; everything else must still be there. This is the one that changes what
users' log files contain.

**2. The zero-delay flush echoes like the tick drain (UPSTREAM-BUGS #13).**
`ChangeTimerRate` compared the echo flag case-sensitively where `OnTimer` used
`toupper`.
**Check:** queue a speed-walk with a delay, then set the speed-walk delay to 0
while commands are still pending (World Properties ▸ Commands, or
`world.SetOption("speed_walk_delay", 0)`). The pending commands flush, and a
no-log one must now be ECHOED in the output window where before it was silent.

**3. Nothing else moved.** Same commands, same order, same pacing. The
`ShowQueuedCommands` status-line update is still the caller's job in both
drains, exactly as before.

Covered headlessly by the ten `QueueDrain.*` tests, including the eight-code
flag table and the pacing/carry-over arithmetic.

### 4c-tertricies. The chat handshake's decisions moved to a shared header (2026-08-05)

**`chatsock.cpp` is edited, and it IS compiled on Windows** (`MUSHclient.vcxproj:231`)
— so this needs a compile before anything else. The new `pal/chat_protocol.h`
is not in the project's `ClInclude` list, which is consistent with
`pal/style_colour.h`, already included the same way from `doc_shared.cpp`.

`CChatSocket::OnReceive`'s handshake decisions — the `YES:` / `CHAT:` / `ZCHAT:`
prefix validation, the remote-name and callback-address splits, the zChat file
block size, and the `ON_PLUGIN_CHAT_ACCEPT` veto — are now pure functions in a
shared header, so the headless suite can reach them. Nothing headless links
`chatsock.cpp` and `platform/windows_stubs.cpp` defines `OnReceive` as an empty
body, so none of this had ever been under test.

**One deliberate behaviour change, and it moves macOS TO Windows.** MFC's
`CString::Find(char)` is `_tcschr` and stops at an embedded NUL; the port's
`CString` is `std::string`-backed and does not (`platform/mfc_string.h:179`).
For a packet `"CHAT:Fr\0ed\n1.2"` the port found the newline at 5 and Windows
found none — so a chat peer sending a NUL in its negotiation line was parsed
differently on the two platforms, changing the recorded name and whether a
callback address was recorded at all. `pal::chatFindChar` implements the
`strchr` semantics for both builds, so **Windows should be byte-for-byte
unchanged** and macOS now matches it.

**What to check on Windows:**
1. It compiles.
2. A normal MudMaster chat session still connects both ways — call out, and
   accept an incoming call — with the name shown correctly at both ends.
3. A zChat session still negotiates: the file block size must go to 1024, and
   `CHAT_STATUS` / `CHAT_STAMP` must still be sent (MudMaster gets neither).
4. A plugin returning false from `OnPluginChatAccept` must still refuse the
   connection with no prompt. This is the security-shaped one: a refusal that
   is ignored gives the peer live snooping and file transfer.

Covered headlessly by `test_chat_protocol` (a new ctest entry — the suite goes
from 55 to 56). The refactor was verified by the reviewer as a differential
against a faithful MFC `CString` model over 400,027 generated packets, with no
mismatches, and the harness proved non-vacuous by seeding four differences.

### 4c-quatertricies. note_text_colour is validated as an index (2026-08-05)

`docs/UPSTREAM-BUGS.md` #8. Shared code: `doc.h`, `doc_shared.cpp`,
`scriptingoptions_numeric.inc`.

The readers were guarded earlier; the row now validates too, via a new
`OPT_CUSTOM_INDEX` flag consulted only by the range check.

**This changes what `SetOption` ACCEPTS, on both platforms.** A script passing
anything outside 0..15 (or `SAMECOLOUR`) now gets `eOptionOutOfRange` where it
previously got success and a silently wrong colour.

**What to check on Windows:**
1. `world.SetOption("note_text_colour", 3)` still succeeds and
   `world.GetOption("note_text_colour")` still returns **3** — the stored
   encoding must be unchanged, or every existing world file shifts by one.
2. `world.SetOption("note_text_colour", 300)` now returns `eOptionOutOfRange`
   and leaves the previous value in place.
3. Open a world saved BEFORE this change with a non-default note colour set via
   World Properties ▸ Notes: the colour must be exactly as it was. The
   serialisation is untouched (still a colour NAME via `ColourToName`), and this
   is the check that it really is.
4. `world.SetOption("echo_colour", 16)` and
   `world.SetOption("hyperlink_colour", 0xFFFFFF)` must still succeed — the new
   flag is on one row only.

### 4d. Everything else (smoke)

World open/save, prefs dialogs, triggers/aliases/timers on live MUD output,
logging, notepads, plugins list, scripting languages you have available.
These paths were mostly untouched on Windows but the doc grew members and
several TUs changed — a broad smoke matters.

## 5. Deliberately NOT flipped (don't report as missing/broken)

These sites still run their original Windows code directly; each is a
genuine semantic fork, documented in commits `de9970b`/`a9061ce`:
`SetInputFont` (port maps input→output font), `PasteCommand`,
`SendTo`→command (plugin-notify delta), `ChangeInputFont`'s view fixup,
`MoveWorldWindowX`/`GetWorldWindowPositionX` (`Which`>1 multi-view), Lua
`GetMainWindowPosition`/`GetWorldWindowPosition` (param variants),
`Activate` (child-frame semantics), `SetScroll` (Visible flag),
`DeleteLines` view resync, `GetInfo(296)`, `GetInfo(257/258)`.  Also the
whole **display family** (line rendering) — unchanged pull model.

## 6. Known intentional behaviour deltas (expected; verify they're benign)

> **All five were checked on 2026-08-10 — see §8w.**  1, 3 and 4 are benign as
> described; 5 is not provokable; and 2 turned out to have quietly dropped the
> WSA error code from the message it reports, which was a defect and is fixed
> (`ed60501`).

1. **Bytes-sent statistics count once.**  Upstream double-counted
   (`SendPacket` and again in `CWorldSocket::OnSend`); session "sent" byte
   stats will read ~half the old values.
2. **Failed DNS *initiation*** reports through the ordinary "Unable to
   resolve host name" path instead of its own message box.
3. **Cert-refused TLS teardown sends no close_notify** (session was never
   trusted).
4. **Hard send-error** now leaves FD_READ|FD_CLOSE armed so the peer's FIN
   completes disconnect bookkeeping (upstream silently closed and left a
   zombie "connected" world).
5. **Socket-create failure** reports via the generic connect-failure path
   and `world.Connect` no longer returns FALSE for it (WSA-not-initialised /
   handle-exhaustion only).

Anything else that differs from official MUSHclient is a finding.

## 7. Reporting

For each finding: subsystem, repro steps, expected (official client)
vs actual, and — where you can — the responsible adapter method + its
source-site (the header maps in `pal/win_worldview.cpp`,
`pal/win_transport.cpp`, `pal/gdi_surface.cpp` give you the mapping).
Separate clearly: (a) compile fixes applied, (b) behavioural regressions,
(c) suspected-but-unconfirmed, (d) upstream bugs faithfully preserved (these
exist and are deliberate).  Wade arbitrates anything ambiguous.

Two specific review requests from the mac side, where reasoning had the
least to grip:
- `pal/win_transport.cpp`: the message-only-window lifecycle (posted
  messages after `DestroyWindow`, `WSAAsyncGetHostByName` handle matching)
  and deleting the transport from inside its own `CAsyncSocket` callback.
- `pal/win_worldview.cpp` `outputSelection()`: the fused four-getter
  selection math (1-based, clamped, `endcol + 1`) against
  `GetSelectionStartLine` et al. in official-client behaviour.

---

## 8. Results of the first empirical pass (2026-08-08)

Environment: Windows 11 Pro 26200, VS 2022 Build Tools (MSVC 14.44, v143,
Windows SDK 10.0.26100), Debug|Win32.  Baseline for every comparison: the
**official MUSHclient 5.06** installed at `C:\Program Files (x86)\MUSHclient`
(Nick Gammon's last release), driven through the *same* scripts as the new
build so the two outputs can be diffed rather than eyeballed.

### 8a. Build — was broken at HEAD; fixed in `0abb31b`

**The Windows build did not compile.**  59 unique errors on the first pass,
from five places where a declaration had drifted into the `#ifndef _WIN32`
arm while its definition or its use stayed unconditional:

| Site | What was wrong |
|---|---|
| `doc.h` | `#include "pal/style_colour.h"` and `#include "line_position_index.h"` sat inside `#ifndef _WIN32`, but `CMUSHclientDoc` declares `GetStylePalette (pal::StylePalette&)` and the `m_LinePositions` member unconditionally.  The class body did not parse; 55 of the 59 errors were this one omission and its cascade. |
| `doc.h` | the `Phase_OSC` declaration was guarded, although `telnet_phases.cpp` defines it and `doc_shared.cpp` dispatches `HAVE_OSC` to it on every platform. |
| `mcdatetime.cpp` | both `#include "pal/iclock.h"` and the single definition of `pal::g_clock` were guarded, while `GetTimeNow()` consults `g_clock` unconditionally. |
| `doc_shared.cpp` | uses `CSendView` in its `_WIN32` arm without including `sendvw.h`. |
| `methods_output_shared.cpp` | uses `CMUSHView` in its `_WIN32` arm without including `MUSHview.h`. |

All five are include/declaration placement — no behaviour changed on either
platform.  Afterwards: **links clean, with no warnings from our own code**
(`WinDebug/MUSHclient.exe`, reports version 5.07).  What remains is `LNK4099`
from OpenSSL's static libs (no PDB shipped) and three pre-existing `C4244`s in
vendored `scripting/lua_compress.c`.

**Release|Win32 builds and runs too** (checked at the end of the third pass,
`d62906c`): zero errors, and the only warnings are the same pre-existing ones
in vendored C — `lua_compress.c` plus two `GetVersionEx` deprecations in
`sqlite3.c`.  Both conformance scripts were re-run against `WinRel` and gave
identical answers to the Debug build, `miniwindow_conformance` included at
64/64 — so nothing here depends on `_DEBUG` or on being unoptimised.

Lesson worth carrying to the mac side: a shared header's `#ifndef _WIN32`
block is the most dangerous place to put an include, because nothing on macOS
can notice.  Adding a member whose *type* comes from a guarded include breaks
Windows silently until somebody compiles it.

**Running the built exe needs runtime assets staged beside it.**  MSVC writes
`WinDebug\MUSHclient.exe`, but the app resolves `locale\en.dll`,
`spellchecker.lua`, `lua5.1.dll`, `tips.txt`, `help.db` and friends relative to
the executable, so a fresh `WinDebug` gives, in order: a missing-DLL system
error, "Failed to load resources file", then a scripting error about
`spellchecker.lua`.  Copying the *data* files (not the exe) from an installed
5.06 makes it run.  Worth knowing before diagnosing a "broken build" that is
really a bare directory.

### 8b. Two reusable conformance scripts (new)

Both live in `tests/lua/` and run in any client — paste into Game ▸ Immediate
(Ctrl+I), or point a world's `script_filename` at a driver that `dofile`s them.
Writing them in Lua is the whole trick: **the same file runs against a stock
MUSHclient**, so "is this a regression?" becomes a diff instead of a judgement.

- `tests/lua/miniwindow_conformance.lua` — §4b's checklist, ported from
  `platform/miniwindow_selftest.cpp` (64 probes).  **64/64 pass** on Windows.
- `tests/lua/api_conformance.lua` — the scriptable half of §4c as `key = value`
  lines, for diffing two clients.

### 8c. §4b miniwindows — 64/64, and §4b's pinned differences were 3-for-4

The three platform differences §4b predicted are **confirmed**: out-of-bounds
`WindowGetPixel` is `-1` on Windows (the port sees 4294967295); GDI's `LineTo`
**excludes** the endpoint (the port draws it); a stroked outline is the crisp
pen colour with no antialiasing.

**A fourth was missed, and belongs in §4b**: real GDI reports Courier New 10pt
as `tmMaxCharWidth` = 10 and `tmAveCharWidth` = 8, so
`WindowFontInfo(7) == WindowFontInfo(6)` — which the mac selftest asserted for
a fixed-pitch font — is **false on Windows**.  The glyph cell is wider than the
average advance even when every advance is identical.  Only `max >= average`
holds on both.  Fixed in `50a1303`: `CDC::GetTextMetrics` used to skip the
measuring loop for a fixed-pitch font and hand back the average, and now
measures every font; the selftest and the Lua script both assert
`max >= average`.

Everything else agreed exactly, including the ones the mac side had only just
fixed and could not check here: RoundRect right/bottom-exclusive, Chord filling
the cap rather than the pie, mono pattern brushes 8–12 keeping *both* caller
colours (288 red / 288 green pixels in the checkerboard, no black or white),
DrawEdge honouring the `BF_` side flags, `EDGE_ETCHED`'s two rings differing,
and the `WindowFilter` identity round-trip being lossless.

### 8d. §4a connection lifecycle — works, against a real MUD

Validated against **discworld.starturtle.net:4242** (the world §4a names),
connecting **by hostname** — the mechanism §7 flagged as having the least
reasoning grip: `WSAAsyncGetHostByName` posting to the adapter's message-only
window.

Confirmed in one session: async DNS resolve, connect,
`--- Connected on Saturday, August 08, 2026, 3:32 PM ---`, the full 30-line
MOTD rendered correctly, `Send` reaching the MUD and its reply coming back,
IAC GA handled with `GetInfo(289)` naming the `Your choice: ` prompt line (56),
`Disconnect`, and the complete statistics block:

```
--- Disconnected on Saturday, August 08, 2026, 3:32 PM ---
--- Connected for 0 days, 0 hours, 0 minutes, 7 seconds. ---
--- Received 57 lines, sent 1 line.
--- Output buffer has 61/10000 lines in it (0.6% full).
--- Matched 0 triggers, 0 aliases, and 2 timers fired.
```

`IsConnected()` is false afterwards, with no zombie state.  Automatic reconnect
after a peer close was also seen working (a mis-written test server closed the
socket and the client re-established by itself).

**A warning about loopback test servers.**  A PowerShell `TcpListener` echo
server on `127.0.0.1` produced a connection that established but carried no
data in either direction.  That looked alarming until the *official 5.06* was
pointed at the same server and behaved **identically** — so it is the harness,
not the client.  If you write a loopback MUD stub, verify it against a stock
client before believing anything it tells you.

Still untested here: TLS (all of §4a's certificate/handshake matrix, and so
§4c-undetricies items 2–4), SOCKS proxies, and long idle sessions.

### 8e. §4c — differential results against 5.06

`tests/lua/api_conformance.lua` run on both clients.  **Identical output** for:
the whole command-bar family (`SetCommand` filling only when empty and
returning `eCommandNotEmpty` = 30020 otherwise, `GetCommand`, `SelectCommand`,
`SetCommandSelection`, `GetInfo(236)`/`(237)` including clamping past the end);
command history (newest-first `GetCommandList`, the count limit,
`DeleteCommandHistory`); bookmarks (§4c-vicies — set, clear, `GetLineInfo(n,7)`
agreeing in both directions, out-of-range line numbers doing nothing);
accelerators (§4c-quater); printf-format handling (§4c-ter —
`Note("literal percent: 100% done %s")` comes out verbatim); and `GetInfo(289)`
across Clear Output.

**§4c-sexies is answered.**  A regexp trigger with *convert wildcards to lower
case* ticked and a Lua script callback behaves **identically in 5.06 and this
build**: the callback receives a 0-based table — `[0]` the whole matching line,
`[1]`/`[2]` the captures — with the **original** case (`Alice`, `Bob`), while
`GetTriggerInfo(name, 101/102)` returns the **folded** ones (`alice`, `bob`)
and `110` the folded whole line.  The asymmetry the mac side pinned is
upstream's, and it is preserved exactly.

**§4c-duodecies is answered: `SetScroll`'s argument really is a device-pixel Y,
and `GetInfo(296)` reports the same unit.**  With the buffer filled past a
screenful, `SetScroll(150)` gives `GetInfo(296) == 150` in **both** clients,
`SetScroll(-1)` reaches the same bottom value in both, feeding `GetInfo(296)`
back into `SetScroll` is a no-op in both, and `SetScroll(-2)` changes nothing.
(On a near-empty buffer 5.06 answers `-248` where this build answers `0`,
because `highest = lastline * fontheight - windowheight` goes negative and
upstream clamps *up* to it, while this build's `will_scroll` short-circuit
skips the scroll entirely.  That difference comes from fork commit `9975939`
in 2020, not from the PAL work.)

Differences found, all explained, **none a regression**:

| Probe | 5.06 | this build | Verdict |
|---|---|---|---|
| `SetSelection` + the four `GetSelection*` | absent | works (2/2/1/6, clamped end column 36) | `SetSelection` does not exist in 5.06 — a later addition, not a change |
| `SetOption("note_text_colour", 300)` | `0` — accepted, stored **300** | `30026` (`eOptionOutOfRange`), keeps 3 | **intended** (§4c-quatertricies / UPSTREAM-BUGS #8); confirms upstream really did accept it |
| `SetOption("max_output_lines", 0)` | `30026` | `0`, reads back 0 | **intended** (§4c-quinvicies) — 0 now means unlimited |
| `GetInfo(125)` | `nil` | `false` | cosmetic; both falsy |

### 8f. An undocumented, Windows-visible divergence: world-load warnings are no longer modal

**RESOLVED in `50a1303`** — Wade confirmed the change was not intended, and
upstream's conditional is back: the modal returns on Windows, while the port
(whose `AfxMessageBox` is a stderr stub that shows nothing) also queues the
detail as a note.  Verified afterwards against 5.06: same modal, same text.
The account below is what the first pass found.

Not in §6 and not in `docs/UPSTREAM-DIVERGENCES.md`, so recording it here.
`CMUSHclientDoc::LoadError` (`xml/xml_load_world.cpp:607`) used to call
`AfxMessageBox` when the output buffer was not yet set up; it now **always**
emits the `[WARNING]` title and the detail as red `ColourNote`s.

Demonstrated with one world file carrying a bad `id` and an unknown attribute:
5.06 puts up a modal *"Line 10: World "id" field must be 24 hex characters
(option 'id' not set)"* that must be acknowledged, while this build writes the
same text into the world's output window and loads straight through.  The
information is not lost — but a user who mis-edits a world file now gets
something they can scroll past instead of something they must dismiss.  The
in-code comment says this was deliberate (the message box "is easy to miss" on
the port).  **A decision for Wade**: if it stays, it belongs in
`docs/UPSTREAM-DIVERGENCES.md` as entry 6.

### 8g. §4c-quater's "empty Send un-binds" is wrong for both clients

`AcceleratorTo("Ctrl+F8", "", 0)` leaves `Ctrl+F8` listed in
`AcceleratorList()` in **5.06 and this build alike**.  The check should be
reworded: an empty Send does not un-bind upstream either, so there is nothing
to preserve.  §8j reads the code to explain why, and recommends what should
happen instead.


### 8i. Second pass (2026-08-08, later): TLS, chat, MXP entities, world save

**TLS — the whole 2x2 is confirmed on Windows** (§4a's TLS bullet and
§4c-undetricies items 1-4), against `openssl s_server` with a self-signed
certificate on 127.0.0.1:4443 and, for the handshake-failure arm, against a
plain telnet port with `use_ssl="y"`.

| Scenario | Answer | Result |
|---|---|---|
| certificate could not be verified (`IP address mismatch` -- the cert is CN=localhost, reached by IP, so `SSL_set1_host` did its job) | **Yes** | reconnects and is **still encrypted**: `--- WARNING: proceeding with UNVERIFIED TLS certificate ... ---` then `--- Connected via TLSv1.3 (TLS_AES_256_GCM_SHA384) ---`.  `use_ssl` stays 1 |
| same | **No** | nothing reconnects, nothing changes, `use_ssl` still 1 |
| handshake failure (`error:0A0000C6:SSL routines::packet length too long`) | **Yes** | reconnects **in the clear** -- plain MOTD, no "Connected via" line, `use_ssl` becomes 0 |

That is the one §4c-undetricies said mattered most: the wrong outcome for
arm 1 is silent plaintext, and it does not happen.  `mainfrm.cpp` compiles and
its prompt wording is intact.  Also seen working on the way past: the
connect-failure path (`Unable to connect ... code = 10061 (Connection
refused)` plus the phase line and the help URL).

**Chat (§4c-tertricies) — handshake and veto both correct.**  One instance
listening on 4050 and calling itself over loopback:

- accepted: *Incoming chat call / Accepted call from 127.0.0.1 / Session
  established*, then **`Chat session accepted, remote user: "ProbePeer"`** and
  the same for *remote server* -- the remote-name split (the part that moved
  into `pal/chat_protocol.h`) is right.  `ChatEverybody` delivered, teardown
  closed 2 connections cleanly.
- vetoed: with a plugin whose `OnPluginChatAccept` returns false, the log shows
  **`Server rejected chat session attempt.`**, no session is created, no prompt
  appears, `ChatEverybody` returns `eNoChatConnections` (30044) and
  `ChatDisconnectAll` closes 0.  This is the security-shaped one and it holds.

(`OnPluginChatAccept` is a PLUGIN callback only -- a world script defining it
is never consulted.  Worth knowing before writing this test again.)

**§4c-sexvicies -- neither hazard fires, and the first one provably cannot.**

1. `MXP_GetEntity`'s unchecked `strName[1]`: modern MFC explicitly permits it.
   `atlsimpstr.h:369` is `ATLASSERT( (iChar >= 0) && (iChar <= GetLength()) );`
   with the comment *"Indexing the '\0' is OK"*, and `GetAt` (:492) is the same.
   So `&#;` reads the terminator legitimately -- the concern came from older MFC
   semantics.  Nine malformed entities (`&#;`, `&#x;`, `&#`, `&;`, `&#xZZ;`,
   `&#999999999;`) were fed through `Simulate` with `use_mxp` forced to
   `eUseMXP`; all were consumed, no assertion, no dialog, process alive.
2. The high-bit `isxdigit`/`toupper` hazard also did not fire (`&#x\xE9;`,
   `&#\xE9;`, `&\xE9\xC8;` in the same run).  **Caveat**: the byte may have been
   repaired by `OutputBadUTF8characters` before MXP saw it, so this is "did not
   reproduce", not "cannot happen".  A length/cast guard would still be
   harmless; nothing here demands one.

**§4c-quinvicies save check -- passes.**  Saving the same world twice gives
byte-identical files, and the only `="0"` in the output is a legitimate
`<key name="0">` keypad entry, so `Save_XML_number`'s new `bWriteZero` is not
leaking zeros.

**...but a save is NOT byte-identical to 5.06's, for an unrelated reason.**
Diffing a save from each client, everything matches except the version/date
stamps and this:

```
-   <colour seq="6" rgb="magenta" />        -> +   <colour seq="6" rgb="fuchsia" />
-   <colour seq="7" rgb="cyan" />           -> +   <colour seq="7" rgb="aqua" />
-  <colour seq="9" ... text="magenta" ...>  -> +  <colour seq="9" ... text="fuchsia" ...>
```

`ColourToName` (`utilities_shared.cpp:993`) walks `App.m_ColoursMap` -- an MFC
`CMapStringToPtr` -- and returns the FIRST name whose RGB matches, with its own
comment admitting *"colour might match more than one name"*.  magenta/fuchsia
and cyan/aqua are exact aliases, so which one wins is decided by hash-map
iteration order.  That order differs between the two binaries; the colour table
itself also gained seven names in June 2019, after 5.06 shipped (`90eb7fe`,
`4ac65e7`).  **Not attributable to the PAL work** -- but it does mean saved
world files are not stable across a compiler change or a colour-table edit,
which is worth knowing for anyone who keeps worlds in version control.  Making
`ColourToName` deterministic (an explicit preferred-name list rather than
whatever the map yields first) is the fix if that matters; it is a data-format
decision, so it is left for Wade.

### 8j. Un-binding an accelerator: what it should do (now fixed, c24926f)

§4c-quater assumed an empty Send un-binds; §8g showed it does not, in either
client.  Reading `AcceleratorTo` (`scripting/methods/methods_accelerators.cpp`)
explains why, and the two halves disagree:

- The **HACCEL** is rebuilt without the key (the `remove_if` before the
  `if (Send [0])` branch), so **the key really does stop firing**.
- The **maps** are not touched.  `m_AcceleratorToCommandMap`,
  `m_CommandToMacroMap` and `m_CommandToPluginMap` are only ever written in the
  `if (Send [0])` arm, so the stale entries survive -- which is why
  `AcceleratorList` still reports `Ctrl+F8 = hello` for a key that can no longer
  do anything.

So upstream half-deletes: the binding stops working but still enumerates.  That
is a bug, not a design -- `AcceleratorList` is how a script asks "is this key
taken?", and it answers wrongly.  It also leaves `m_CommandToPluginMap` naming
a plugin as the owner of a binding that is not live, which matters when a
plugin is unloaded.

The port already did the right thing: `AcceleratorTo`'s `#else` arm erases all
three map entries when `Send` is empty.  **Done in `c24926f`**: that erase moved
out of the `#else` so both platforms run it, leaving the Windows HACCEL rebuild
exactly as it was.  The only observable change on Windows is that
`AcceleratorList` stops listing keys that were already dead.  Being a
deliberate departure from upstream it is recorded as
`docs/UPSTREAM-DIVERGENCES.md` entry 6.


### 8l. Third pass: DBCS wrap, the command queue, MCCP

All three differentially against 5.06, all three showing exactly the intended
change and nothing else.

**§4c-duodetricies (DBCS wrap) — the fix works, and the control holds.**  This
is the one change that deliberately alters what Windows *draws*, so it wanted
the closest look.  A Big5 world (`utf_8` off, `wrap_column` 20, *indent
paragraphs* off) fed CJK byte pairs through `Simulate`, with the wrapped lines
read back through `GetLineInfo` and scanned for lead/trail bytes rather than
eyeballed:

| | 5.06 | this build |
|---|---|---|
| Big5, even offset | **5 torn characters** — every wrap strands a lead byte at end of line | **0**, every line 18 bytes, all breaks on a character boundary |
| Big5, odd ASCII prefix | **5 torn** | **0** |
| ASCII control (wraps at a space) | byte-for-byte identical between the two clients — the space stays at the END of the old line, no continuation begins with one |

So `UPSTREAM-BUGS.md` #10 reproduces exactly on stock 5.06 and is gone here,
the wrap does fall up to a byte earlier as predicted (18 vs 19), and the gate
did not loosen in the other direction.

**§4c-duodetricies-bis (the command queue) — both deliberate changes
confirmed, with exact counts.**  Against a real MUD, four commands queued
behind a speed-walk delay, two asking to be logged and two not:

- *"No log" now suppresses logging* (UPSTREAM-BUGS #12).  5.06 wrote **4** of
  the 4 sent commands into the session log — the flag was recovered as
  `cMessageType >= 'A'`, which every flag byte satisfies, so it never
  suppressed anything.  This build writes **2**.  Total log size differs by
  exactly those two lines (104 vs 102), so nothing else moved.
- *The zero-delay flush echoes like the tick drain* (UPSTREAM-BUGS #13).  Three
  commands queued behind a 30-second delay, then the delay set to 0 to force
  `ChangeTimerRate`'s flush arm: 5.06 echoed **1** of 3 (the two lower-case
  flags failed its case-sensitive test and went out silently); this build
  echoes **3**.

**MCCP is confirmed, not merely inferred.**  `GetInfo(103)` goes false → true
on connecting to Discworld and `GetInfo(208)` reports type **2**, so MCCP v2
really was negotiated and the whole MOTD came through the inflate path.
Identical in 5.06, including the detail that both leave the flags set after
disconnect (`m_bCompress` is cleared by the next `ConnectSocket`, not by
teardown) — so that is upstream's, not a transport-rewrite leak.

**§4c-quater / §8j (accelerators) — fixed and verified.**  The erase moved out
of its `#else` (`c24926f`), so an empty `Send` now clears the maps as well as
the table on both platforms.  `tests/lua/api_conformance.lua` reported
`accel.list.after.unbind = {1=Ctrl+F8 = hello}` before and reports an empty
list now.  Recorded as `docs/UPSTREAM-DIVERGENCES.md` entry 6.

**An incidental confirmation that the restored modal earns its keep.**  While
setting these up, a world file carrying `reconnect_on_link_failure="n"` — not
an option in either client — sailed through unnoticed under the note-only
behaviour, and stops the load with a modal now.  That is precisely the class of
mistake §8f was about.

### 8m. §4c-quatervicies item 1 settled by inspection: the freeze-on-selection reading is right

The first of the four "could not verify here at all" items does not need a run.
`CMUSHView::get_selection` (`mushview.cpp:2716`) returns **true when the region
is EMPTY** — explicitly, in three places: the past-end-of-data early return
(`:2740`), the same-line degenerate case (`:2766`), and the tail
`return bReturn; // true if empty region` (`:2828`).  So
`!get_selection (newrgn) && pDoc->m_bAutoFreeze` really does mean "there IS a
selection and auto-freeze is on → freeze", which is what the port's
`FreezeIfSelection` was built on.

The three Windows call sites (`:2453`, `:2613`, `:4837`) are also **original
upstream code, not edits** — they are present verbatim in the initial import
`ad0cd12`.  The fourth site, `CWinWorldView::setOutputSelection`, is
`SetSelection`'s Windows body relocated: diffed against `02b7336^` it is
verbatim, down to the `m_pin_line` / `m_pin_col` assignments and the
`UpdateAllViews (NULL)` at the end.

So nothing to un-do: the reading was correct and no Windows behaviour changed.
The other three items in §4c-quatervicies (the `keep_pause_at_bottom` latch,
the double-click ordering, `TextRectangle`'s 20-pixel floor) still need a
pointer and a human.

Two other §7 review requests, also settled by reading rather than running:

- **`pal/win_worldview.cpp` `outputSelection()`** — diffed against the four
  separate getters as they stood at `a9061ce^`.  The fused body is faithful:
  same view walk, same `m_LineList.GetCount () > 0` guard (so a `CMUSHView`
  with an empty buffer still falls through to the next view), same
  `MIN (col, line->len)` clamps, same no-selection test, same `+ 1`s, same
  "first output view wins, else zeros".  §8e then confirmed it empirically —
  `SetSelection (2, 2, 1, 6)` reads back 2/2/1/6, a degenerate selection reads
  back as none, and a past-the-end column clamps to the line length.
- **`pal/win_transport.cpp`'s message-only window** — the two mechanics that
  worried the mac side both behaved: `WSAAsyncGetHostByName` completions match
  on the handle and resolve a real hostname (§8d), and destroying the transport
  from inside its own event is exercised on every disconnect through
  `CDocTransportEvents::onClosed`, which deletes it before the bookkeeping.
  Dozens of connect/disconnect cycles across these passes, including
  server-initiated closes, TLS teardowns and an automatic reconnect, produced
  no crash.  `connect()`'s immediate-success arm is the one that stayed
  untested: on loopback MFC's `Connect` can return TRUE inline, and the deferred
  `WM_WT_CONNECTED` post is what keeps the engine from re-entering — it did not
  misbehave, but nothing here proved which arm was taken.


### 8o. Fourth pass: ColourToName pinned, and Quow's pack actually runs

**`ColourToName` is deterministic now (`08c8fa8`).**  §8i recorded that a save
from this build differed from 5.06's by `magenta`→`fuchsia` and `cyan`→`aqua`,
and left the fix as a decision.  Taken: the aliased names were measured on both
clients first, with `RGBColourToName` over all nine RGB values that carry two
names.  Four of the nine disagreed — and this tree was not self-consistent
either, answering `dimgrey` but `darkgray`, because the winner is whichever
name MFC's hash buckets yield first:

| RGB | names | 5.06 | before | after |
|---|---|---|---|---|
| `0x00ffff` | aqua / cyan | cyan | aqua | **cyan** |
| `0xff00ff` | fuchsia / magenta | magenta | fuchsia | **magenta** |
| `0xd3d3d3` | lightgray / lightgrey | lightgrey | lightgray | **lightgrey** |
| `0x2f4f4f` | darkslategray / -grey | darkslategray | darkslategrey | **darkslategray** |
| `0x696969` | dimgray / dimgrey | dimgray | dimgrey | **dimgray** |
| the other four | | agreed already | | unchanged |

The choice is pinned to what every *shipped* MUSHclient emitted: 5.06's answer
for the three pairs it already had, and the `-gray` spelling for the six whose
`-grey` alias arrived after 5.06 (that being the only spelling an existing
world file can be carrying).  Anything unlisted falls back to the
alphabetically first match, so the result no longer depends on a hash.

Two checks: `RGBColourToName` now answers identically to 5.06 for all nine, and
each round-trips through `ColourNameToRGB`; and **a saved world file is
byte-identical to one saved by 5.06 apart from the version string and the
timestamp**, which is the strongest form of §4c-quinvicies' save check.

**§4b's acceptance workload — Quow's Discworld pack — loads and runs.**  It was
already in the tree under `packaging/discworld-plugins/`, so no download was
needed; the mapper wants its database at `.\quow_plugins\maps\` relative to the
working directory, which is the only setup step.

All **six** plugins load and enable, CowBar (the 1.5 MB mapper) included, and
between them they build:

- **14 miniwindows**, each the size it asked for, from 4×390 margins to a
  1599×4 strip and a 1008×150 comms panel;
- **86 hotspots** across them;
- **19 fonts** through `WindowFont`;
- **2 images** through `WindowLoadImage` — so `png_load_shared.cpp` and the GDI
  surface's image path both ran on real PNGs;
- and the canvases are real: `WindowGetPixel(name, 1, 1)` reads back non-zero
  colours (`0x36...`, `0x52...`, `0x29...`) from four of them, so the plugins'
  drawing landed rather than going nowhere.

The mapper also opened its SQLite database without complaint, and the world
auto-connected to Discworld over MCCP with all of that active — no scripting
errors, no dialogs, nothing in the output window but the plugins' own notices.

What this does NOT cover is what a mapper *looks* like: geometry, layering and
redraw under movement still want a human at the screen.  But everything an
automated check can reach — plugin loading, Lua at that scale, the miniwindow
lifecycle, fonts, images, hotspots, SQLite, and a live compressed connection
underneath — works.


### 8p. Fifth pass: SOCKS, zChat (and a real bug), the mapper, hotspot re-detect

Four items §8z had listed as unreachable turned out to be reachable with a
little machinery.  The harnesses are in the session scratchpad, not the repo,
but each is a dozen lines of C# hosted in PowerShell and is described here well
enough to rebuild.

**SOCKS 4 and 5 (§4a) — validated at the byte level.**  Not through an open
proxy found online: a MUD session carries credentials, an unknown third party
is a bad place to send them, and a proxy under our own control is a far better
witness because it can log what the client actually sent.  A local logging
proxy, with the world pointed at the real Discworld through it:

- **SOCKS4**: `04 01 10 92 51 BB 33 06` + `probeuser` + NUL — version 4, CONNECT,
  port `0x1092` = 4242, address `51 BB 33 06` = 81.187.51.6.  18 bytes total
  with a **4-byte address field**, which is exactly what the `unsigned int`
  comment in `doc_shared.cpp` is guarding (an 8-byte `unsigned long` would have
  put a malformed 22-byte request on the wire).
- **SOCKS5 with authentication**: greeting `05` + methods `00 02`, proxy picks
  `02`, client sends RFC 1929 auth (`ver=1 user="probeuser"`), proxy answers
  `01 00`, then `05 01 00 01` + address + port.  Correct throughout.

Both then relayed, and the MUD's MOTD arrived in the client each time.

**zChat (§4c-tertricies item 3) — and it was completely broken.**  There is no
"zChat server" to find: it is peer-to-peer between MUD clients, so the way to
see what MUSHclient puts on the wire is to be the peer.  A peer that calls IN
chooses the dialect with its first packet, which is what makes zChat reachable
at all.

The handshake parses correctly — `GetChatInfo` reports connection type **1**
(zChat), the remote name, the callback address `127.0.0.1` and port `4051` split
correctly out of the trailing five characters, and the **file block size raised
from 500 to 1024**, which was the specific thing §4c-tertricies asked about.
MudMaster stays at type 0 / 500 and gets `CHAT_VERSION` only, as it should.

But the messages themselves went out **one byte each** — see `e4236e8`.  A zChat
frame is 2 bytes of little-endian command then 2 bytes of little-endian length,
so for every command below 256 the second byte is a NUL, and modern ATL's
`CSimpleStringT::Append` clamps at the first NUL even when handed an explicit
length (`nLength = StringLengthN(pszSrc, nLength);`, with the comment "Make
sure we don't read pass end of the terminating NULL").  A stock 5.06 sends 47
bytes of intact `CHAT_VERSION` / `CHAT_STATUS` / `CHAT_STAMP`; this tree sent
17, of which three were message stumps.  Both directions, since the receive
buffer is appended the same way — so zChat could not work at all.

**It is a toolchain regression, not a port one**: `SendChatMessage`, `SendData`
and `OnSend` are byte-for-byte as imported, and 5.06's binary (built against an
older MFC whose `Append` was a plain memcpy) is fine.  Anyone building this
source with VS2022 gets it.  It would bite chat file transfers too, where a
block of an ordinary binary file is very likely to contain a NUL.  Fixed by
appending through `SetString(ptr, len)`, which does not clamp; verified
byte-identical to 5.06 afterwards, with MudMaster unchanged.

**Quow's mapper, looked at rather than measured (§4b).**  `PrintWindow` with
`PW_RENDERFULLCONTENT` captures the window's own rendering without needing it
focused or unobscured, so the composition §8o could not check is checkable
after all.  With the pack loaded and the world connected, the Ankh-Morpork
minimap draws correctly: the street grid, the room labels (CONTRACT BRIDGE,
BRASS BRIDGE, FILIGREE ST, MERCHANT, ASSASSINS…), the coloured room markers,
the blue title strip and the in-game clock line beneath it.  The comms panel
renders its `All / Tells / Group / Talkers / One` tab headers, and the stats
column its position marker, XP lines and HP/GP/B bars.  Everything sits where
it belongs — no overlap, no clipping, no torn text — with the MUD's own output
filling the rest of the pane.

**The hotspot re-detect (§4c-undevicies) — confirmed, both directions.**
Synthetic `WM_MOUSEMOVE` posted straight to the `AfxFrameOrView140sd` view
drives `CMUSHView::OnMouseMove` without touching the user's cursor.  With a
probe plugin whose hotspot callbacks write to a file:

- `WindowAddHotspot` fires `MouseOver` **synchronously, inside the call** — the
  log line lands before `AddHotspot` returns — when the last known mouse
  position is inside the new hotspot.  That is precisely the arm that now goes
  through `pal::IWorldView::mouseMoveMiniWindow` into `CWinWorldView`'s
  `CMUSHView` walk, so the relocation works.
- Adding a hotspot away from that position fires nothing, and
  `WindowInfo(win, 19)` stays empty.  The control holds.

There is one obstacle to going further, which MFC arms
`TrackMouseEvent(TME_LEAVE)` on the first mouse move: Windows then posts
`WM_MOUSELEAVE` for a cursor that is not really there and
`m_last_mouseposition` reverts to the `(-1,-1)` sentinel.  **§8q gets past it**
and finishes the rest of the mouse family.


### 8q. The mouse paths, after all — §4c-terdecies and §4c-undevicies

§8p said posted mouse messages could provoke the hotspot paths but could not
*park* a pointer, because MFC arms `TrackMouseEvent(TME_LEAVE)` on the first
move and Windows then posts `WM_MOUSELEAVE` for a cursor that is not really
there, resetting `m_last_mouseposition` to its `(-1,-1)` sentinel.  That was
giving up too early: the leave is *queued*, so anything queued ahead of it
still sees the position we set.

The lever is an accelerator.  `AcceleratorTo(key, script, 12)` binds a command
in the `ACCELERATOR_FIRST_COMMAND` range (12000, `mainfrm.h`) handled by
`CMUSHView`'s `ON_COMMAND_RANGE` (`mushview.cpp:261`), so posting
`WM_MOUSEMOVE` and then `WM_COMMAND 12000` to the view in one burst runs the
script in the same pump turn, before the leave lands.  No real cursor is
touched, and nothing on the desktop moves.

With a probe plugin that logs every hotspot callback and its flags:

| Check | Result |
|---|---|
| **§4c-undevicies re-detect** | pointer parked at (100,50); `ReAddHotspot` confirms `mouse now at (100,50)`, and `MouseOver` fires **synchronously inside `WindowAddHotspot`** — the log line lands between the delete and the add returning — with `WindowInfo(19)` then naming `probe_hotspot` |
| **…and its control** | with the pointer still at (100,50), adding a 2x2 hotspot at (0,0) fires nothing and `WindowInfo(19)` stays empty |
| **Middle button** (§4c-terdecies) | `MouseDown flags=0x200 (MIDDLE)` **and** `MouseUp flags=0x200 (MIDDLE)` — both edges dispatched, which is the Windows behaviour the port was changed to match |
| **Right double-click** | `MouseDown flags=0x60 (RH\|DBL)` — exactly `MW_MOUSE_RH \| MW_MOUSE_DBL` |
| **Ctrl+wheel ordering** | the hotspot's scrollwheel handler fires (`flags=0x780000`), so the wheel reaches the hotspot **before** the view's font zoom, confirming `mushview.cpp:5160` dispatches first and only falls back at `:5165` |
| **Leave-window** | each posted move is followed by the OS's own `WM_MOUSELEAVE`, and the log shows the matching `CancelMouseOver` — the `CPoint(-1,-1)` arm working as described |

So every mouse behaviour §4c-terdecies cites as the Windows reference is
confirmed to actually be the Windows behaviour, and §4c-undevicies' relocation
of the re-detect through `pal::IWorldView::mouseMoveMiniWindow` works in both
directions.

**One correction to §4c-terdecies, and it is only a comment.**  It says of
Ctrl+wheel that "the flags a plugin receives include `MW_MOUSE_CTRL`".  They do
not, on either platform.  `OnMouseWheel` builds the miniwindow flags as
`(zDelta < 0 ? MW_MOUSE_SCROLL_BACK : 0) | (abs (zDelta) << 16)`
(`mushview.cpp:5160`) and never consults `nFlags`; measured here as `0x780000`
for a 120-unit forward notch with MK_CONTROL held.  The port's forwarder
(`wx_world_child.cpp:1071`) builds the identical expression, so the two agree —
what was wrong was the claim, in this document and in a comment in
`platform/wx_outputpane.cpp`, both now fixed.  The *ordering* half of that
bullet — hotspot before zoom — is confirmed above and is the part that
mattered.

### 8r. Chat file transfer — eight runs, all byte-exact

The one remaining §8z item that was actually reachable.  A file transfer is the
only thing that drives raw binary through the chat framing, and both halves of
it are exactly the input shape `e4236e8` was about: the sender builds a
`CString` **from file bytes** (`chatsock.cpp:1233`) and the receiver
accumulates raw bytes into `m_outstanding_input` (`chatsock.cpp:401`).

The payload is 1234 bytes of `(i * 7 + 3) mod 256`.  That is deliberate: it
contains 4 NULs — the first at offset 219, inside the first block either way —
and 5 `0xFF`s, which for MudMaster are the framing terminator appearing
unescaped inside a message body.  1234 is a multiple of neither block size, so
the final block is partial and therefore zero-padded on the wire
(`ZeroMemory`, `chatsock.cpp:1197`).

| direction | dialect | blocks | 5.07 | 5.06 | sumcheck (both) |
|---|---|---|---|---|---|
| MUSHclient sends | zChat | 2 × 1024 | 1234 B, 0 differing | same | `5C2FF204 F1071C0E 4F1E4C4A 5AFFE491 905079E4` |
| MUSHclient sends | MudMaster | 3 × 500 | 1234 B, 0 differing | same | `A37589A8 3D598668 C9402805 4AA0F156 08E9BF1A` |
| MUSHclient receives | zChat | 2 × 1024 | 1234 B, 0 differing | same | `5C2FF204 …` |
| MUSHclient receives | MudMaster | 3 × 500 | 1234 B, 0 differing | same | `A37589A8 …` |

Every combination delivers the file byte-for-byte, and the sumcheck the sender
announces equals the one the receiver computes.  Harnesses:
`chat/filepeer.ps1` (peer receives) and `chat/filesender.ps1` (peer sends),
with `conformance/driver_filexfer.lua` and `driver_filerecv.lua`.

**The receive direction is scriptable after all**, which is what made this
testable without a human answering two modal dialogs.  All three gates are real
options: `can_send_files` per connection (`methods_chat.cpp:49`, and it
defaults **false** — `chatsock.cpp:95`), plus `auto_allow_files`
(`scriptingoptions_numeric.inc:14`) and `chat_file_save_directory`
(`scriptingoptions_alpha.inc:9`).  Grepping `scriptingoptions.cpp` for them
finds nothing and reads as "not scriptable"; the tables are the `.inc` files.

**One thing this turned up, and it is upstream.**  The sumcheck is not a hash
of the file: the same 1234 bytes produce two different numbers depending on the
dialect, because the block size feeds straight into `shsUpdate`.  The source
says why — "this code assumes that the buffer size is a multiple of
SHS_BLOCKSIZE bytes long" (`scripting/shs.c:219-221`) — and the implementation
simply `memcpy`s any remainder into `shsInfo->data`, where the *next* call
overwrites it.  1024 is a multiple of 64 and 500 is not, so a MudMaster
transfer silently drops 52 bytes per block out of the digest while still
counting them in the bit length.

This is not a port regression and not a transfer defect: stock 5.06 produces
the same two numbers, both ends of a transfer compute it the same way, so it
still detects a corrupted transfer between two MUSHclients — which is all it is
used for.  The port's only edit to `shs.c` (`43e2bd2`) is an include guard plus
`unsigned long` → `unsigned int`, which is a no-op on Win32 and a correctness
fix on LP64.  Recording it because "sumcheck" reads like a checksum of the
file, and it is not one; anything that ever compares it across dialects, or
against an externally computed SHA, will be wrong.

### 8s. §4c-quatervicies is now closed — all four items

§8m settled item 1 (the freeze-on-selection reading) by inspection.  The other
three were listed as "`CMUSHView` code no script can reach", which was true of a
*script alone* and false once the accelerator lever from §8q is available.

**Item 2 — `keep_pause_at_bottom` really is a latch.**  `m_freeze` is not
scriptable, but its effect is: `addedstuff` only jumps to the end when not
frozen (`mushview.cpp:1815`), so the vertical scroll position after appending
lines reports the freeze state exactly.  Posting `WM_VSCROLL` reaches
`CMUSHView::OnVScroll` (`:4033`), which handles every `SB_` code, so the posted
message takes the same path a scrollbar drag does.

| step | option OFF | option ON |
|---|---|---|
| at bottom | 5864 | 6764 |
| page up ×3 → freezes | 4736 | 5636 |
| back to bottom | 5864 | 6764 |
| append 60 lines | **6764** — followed the output | **6764** — did not move |
| append 60 more | — | **6764** again, range now 8954 |
| page down ×3, append 60 | — | 7892 → **7892**, still frozen |

That is `mushview.cpp:4682` doing exactly what the port read it as: with the
option on, arriving at the bottom leaves `m_freeze` set, and nothing short of
the Pause command clears it.

**Item 3 — the double-click ordering, and the auto-copy that must not run.**
With `copy_selection_to_clipboard` on and the world disconnected:

- **Control** (neither `double_click_sends` nor `double_click_inserts`):
  double-clicking a word replaced a clipboard sentinel with `wordone`.  So the
  posted `WM_LBUTTONDBLCLK` really does reach `OnLButtonDblClk`, resolve the
  word, and reach the auto-copy at `:4884`.
- **`double_click_sends` on**: the click raised "The connection to
  DblClickWorld is not open. Attempt to reconnect?" (`doc.cpp:2732`) and the
  sentinel was **still on the clipboard** afterwards — with **either** answer.
  Both are correct: `CheckConnected` returns true either way, so
  `OutputWordDoubleClicked` returns `eDblClickAbort` and the caller's early
  `return` at `:4861` skips the copy.

The control is what makes this evidence: without it, "clipboard untouched"
would equally describe a double-click that never arrived.

**Item 4 — `world.TextRectangle`'s 20-pixel floor.**  Two independent answers,
and they agree.

By inspection: `git log -L 6685,6723:mushview.cpp` returns exactly one commit —
`ad0cd12`, the initial import.  `CMUSHView::GetTextRectangle` has never been
edited, so the floor is at the original point and `pal/text_rect.h` was
transcribed from it rather than the other way round.

Empirically, the whole chain is script-driven (`TextRectangle` calls
`AutoWrapWindowWidth` itself, `methods_output.cpp:572`), so `GetOption
("wrap_column")` reads the result.  With both clients' world windows pinned to
the same size (view client 979×391), 5.06 and this build agree on every line:

| rectangle | wrap column, both builds |
|---|---|
| reset `(0,0,0,0)` | 122 |
| right `-200` (offset from edge) | 97 |
| right `-100000` (floor) | 20 |
| left 100, right `-100000` | 20 |
| right `300` (absolute) | 37 |
| right `10` (absolute, below the floor) | 20 |
| bottom `-100000` only | 122 — unchanged, as it should be |

implied character width 8 and implied output width 977–984 in both, bracketing
the measured 979.

Two things worth keeping from this one:

- **The 20-*pixel* floor is invisible through the wrap column**, because
  `ClampWrapColumn`'s 20-*column* floor swallows it: an un-floored rectangle
  would give a negative width and clamp to 20 as well.  Its only observable
  effect is on drawing geometry.  So this test proves the resolution agrees; it
  does not, and cannot, isolate the floor by itself.
- **Probing from `OnWorldOpen` measures the wrong window.**  The MDI child is
  still being created, so the view's client rect is an interim size; the first
  run of this probe reported 134 vs 135 and looked like a real one-column
  divergence.  It was not — the two clients restore *different* world-window
  placements (1199 vs 1216 px wide), and pinning both to 1000×400 made every
  number identical.  Any Windows-vs-Windows comparison of a width-derived value
  has to pin the window first.

**One environmental artifact, so it is not re-investigated:** both clients pop
"`/tmp/ows.mcl` contains an invalid path." at startup on this VM, with no world
file and nothing matching in the registry.  Stock 5.06 does it identically, so
it is leftover state on this machine, not a port defect.

### 8t. §4c-quaterdecies — the DDV reference, measured

The property-page validators were on §8z as "wants a human at the dialogs".
They do not: a property sheet answers to posted messages like anything else.
`world.DoCommand` posts its menu command to the **frame**
(`methods_commands.cpp:328`), so `WM_COMMAND ID_GAME_CONFIGURE_OUTPUT` (32300)
to the frame opens World Properties on the Output page; the wrap-column edit is
`IDC_WRAP_COLUMN` = 2008, whose validator is
`DDV_MinMaxUInt (pDX, m_nWrapColumn, 20, MAX_LINE_WIDTH)` with
`MAX_LINE_WIDTH` = 500 (`prefspropertypages.cpp:5847-5848`, `doc.h:94`) — and
the page even prints "(20 to 500)" beside it.

| value typed | what MFC did |
|---|---|
| `5` | **"Please enter an integer between 20 and 500."**, sheet stayed open |
| `600` | the same message, sheet stayed open |
| `100` | accepted, sheet closed, world file byte-identical on disk |

So the port's new DDV behaviour is right in substance — reject, complain, keep
the dialog open, leave the member alone — and the ranges in the shipping pages
are ones Windows itself enforces, which was the stated risk.

**One wording defect, found by having the real string to compare against.**
MFC has *two* messages: `AFX_IDP_PARSE_INT_RANGE` ("Please enter an integer
between %1 and %2.") for the integral overloads and `AFX_IDP_PARSE_REAL_RANGE`
("Please enter a number between %1 and %2.") for `float`/`double`. The port's
`McDdvFailRange` (`platform/mfc_ui.h`) said "a number" for all five, so every
integer field on macOS reported itself in the language of a real-number field.
Fixed by defaulting the noun to "an integer" and passing "a number" from the
two real overloads. §4c-quaterdecies above quoted the wrong string too, which
is how it survived review — the description and the code agreed with each
other and both disagreed with MFC.

This is **port-only** code (`mfc_ui.h` is `#ifndef _WIN32` and is not in
`MUSHclient.vcxproj`), so Windows is unaffected; it does need the macOS build,
along with the other port-side files listed at the end of §8.

Two harness notes, both of which cost time:

- **`GetWindowText` cannot read another process's controls.** It returns the
  cached caption, which for an `Edit` is empty — so every read came back `""`
  while the writes were landing perfectly. `WM_GETTEXT` works cross-process and
  `WM_SETTEXT` always did. The give-away was that MFC answered with the
  *range* message rather than a parse error, which only happens if a number
  really did arrive.
- The menu command has to go to the **frame**. Posted to the view it is silently
  dropped: `ID_GAME_CONFIGURE_*` is not in the view's routing.

### 8u. §4c-tervicies — spell check on send, and Cancel

Also drivable, for the same reason: the command bar is `CSendView`, a
`CEditView`, so its window is a plain `Edit` child of the world's MDI child
frame, and `CSendView::OnChar` acts on `VK_RETURN` from the message map
(`sendvw.cpp:320`).  `WM_SETTEXT` puts a command in it and a posted `WM_CHAR`
13 presses Return.  Global spell check needed no setting up — `EnableSpellCheck`
defaults to 1 (`globalregistryoptions.cpp:34`) and this VM had never overridden
it — and `spell_check_on_send` is a world option.

| | spell check ON | spell check OFF |
|---|---|---|
| typed | `hello thair` | `hello there` |
| dialog | **Spell Check**, Misspelt = `thair` | none |
| after | bar still `hello thair` | — |
| send happened? | **no** — no reconnect prompt | **yes** — "The connection to SpellWorld is not open. Attempt to reconnect?" |

The reconnect prompt is what makes this conclusive.  The world is disconnected,
so a send that really reaches `SendCommand` must go through `CheckConnected`
and ask; Cancel produced no such prompt, which is `OnChar` returning at
`sendvw.cpp:342` before the send.  The bar keeping its text is true in both
columns (the disconnected send does not clear it either), so on its own it
proves nothing — the earlier attempt at this test rested on it and was
worthless.

The checker itself works: `teh` and `thair` were both flagged, `hello` and
`quick` were not.  So §4c-tervicies' Windows risk — that the added defaulted
third parameter on `SpellCheckString` broke the recompile or the behaviour — is
answered: it built (§8a) and the two-argument `App.SpellCheck (this,
&GetEditCtrl())` path Windows uses is unchanged and unaffected.

### 8z. Still not covered

Everything that was ever on this list has come off it except one item: SOCKS,
zChat, the mapper's appearance, the hotspot re-detect, the mouse family, chat
file transfer, all four of §4c-quatervicies, the property-page validators and
the spell checker are done — §8p, §8q, §8r, §8s, §8t, §8u.  What is left:

- **Mapper behaviour over time** — geometry and layering render correctly in a
  still capture (§8p), but redraw as a character moves through rooms needs
  someone playing a MUD, which is a different kind of test from anything here.

**Why the rest fell.**  Three items were written off as "needs a human at the
dialog" or "`CMUSHView` code no script can reach", and none of that was true.
The whole of the GUI answers to posted messages: `WM_COMMAND` in the
`ACCELERATOR_FIRST_COMMAND` range runs a script in the view's message queue
(§8q), menu commands go to the frame (§8t), `WM_VSCROLL` scrolls, `WM_CHAR`
types, and a modal is a `#32770` that answers to its button's control id.  The
two things that are genuinely hard are (a) anything needing a *real* cursor
position that outlives the next queued message, and (b) judging what something
looks like.  The mapper is (b).

The recurring mistake in the earlier passes was to conclude "unreachable" from
"unreachable *from Lua*".  Worth remembering next time the phrase appears.

### 8v. The four `sendvw.cpp` divergences, run at last

`docs/UPSTREAM-DIVERGENCES.md` entries 1, 2, 3 and 5 are deliberate departures
from stock MUSHclient applied to canonical source, and its verification status
said flatly that **a Windows run was still required** — `CSendView` is never
instantiated on the port, so that side type-checks them and nothing more.  This
is that run.  Unlike every other differential here the two traces are *supposed*
to differ, and the shape of the difference is the check.

History for all four: `abc, abcd, abcde, northeast, north` (oldest first).

| step | this build | stock 5.06 |
|---|---|---|
| **1** type `north`, Alt+Up | **`north`** | **`northeast`** |
| **2** type `no`, Alt+Down ×3 | `northeast` → `north` → **`no`** | `northeast` → `north` → **`""`** |
| **3** type `abc`, Up ×2, Down ×3 | `north` → `northeast` → `north` → **`abc`** → `""` | `north` → `northeast` → `north` → **`""`** → `""` |
| **5** Shift+Return, type, Alt+Return | `"line one\r\n"`, then `"line one\r\nline two\r\n"` | sent both times, no break inserted |

Every other step is identical, including the walk order in both directions, so
nothing else has drifted in that file.

**Entry 1 needed the history in the right order to mean anything.**  `Alt+Up`
starts at `m_msgList`'s **tail** — the newest — and walks back
(`sendvw.cpp:1949`), so the exact match is only in its path when `north` is
*newer* than `northeast`.  Seeded the other way round both clients answer
`northeast` and the test proves nothing; the first run of this did exactly
that.

**Entry 5's two named hazards are both answered.**  `ES_MULTILINE` +
`ReplaceSel("\r\n")` puts a real break in the control (the bar reads back as
`"line one\r\nline two\r\nline three"`), and consuming `WM_SYSKEYDOWN` for
Alt+Return does **not** leave Alt latched: typing immediately afterwards still
lands in the bar, `GetGUIThreadInfo` still reports the edit as the focus window,
and no menu is owned.  Plain Return still sends and clears the bar.

**How entry 5 was driven, because three obvious approaches all fail.**
`PreTranslateMessage` decides the chord with `GetKeyState`
(`sendvw.cpp:1695-1696`), and that is not something a posted message can
satisfy:

- `PostMessage (WM_KEYDOWN, VK_SHIFT)` — a thread's key-state table is
  maintained by the input layer, not by messages arriving in its queue.  The
  give-away was that a posted *plain* Return sent the command while the same
  Return with a posted modifier ahead of it also just sent: the message was
  arriving, the modifier was not.
- `AttachThreadInput` + `SetKeyboardState` — retrieving a keyboard message
  re-syncs the thread's key state from the queue's own record, which has no
  shift in it, so the fake state is gone before it is read.
- `keybd_event` — correct, but injected keys go to the **focus** window, and
  this app had none.

That last one is the load-bearing discovery.  **This VM stacks three dialogs at
startup** — Tip of the Day, "`/tmp/ows.mcl` contains an invalid path", and
sometimes a reconnect prompt — and they *stack*, so dismissing one reveals the
next, which is why they looked like something firing on a timer.  Until all are
gone MFC never gives the command bar the keyboard focus: `GetGUIThreadInfo`
reported the focus window as the **frame**.  Clear them all and the focus lands
on the edit, after which ordinary injected keys work.  Stock 5.06 stacks the
same dialogs, so this is the machine, not the build (§8s) — but any future
Windows test that needs real keystrokes must drain the dialog queue first, and
`GetGUIThreadInfo` is how to check it worked without perturbing it.

Entries 1-3 need none of that: they live in the four `ON_COMMAND` handlers
(`sendvw.cpp:51,52,98,99`), so a `WM_COMMAND` posted at `CSendView` runs them
directly.  Typing, though, must be one posted `WM_CHAR` per character —
`WM_SETTEXT` does not raise `EN_CHANGE`, so `m_bChanged` stays false and every
"remember what they were typing" arm (`:407`, `:1833`) skips silently.

### 8w. §6's five "expected; verify they're benign" deltas

Never individually ticked off until now.  Four are benign; the fifth is not
provokable; and looking for the second turned up a real defect that was **not**
in §6 (fixed in `ed60501`).

**1. Bytes-sent statistics count once — confirmed, and it is the *correct*
number.**  A counting server says what actually crossed the wire; `GetInfo(217)`
says what the client believes.  Sending a 40-character line:

| | on the wire | `GetInfo(217)` |
|---|---|---|
| this build | 42 bytes | **42** |
| stock 5.06 | 42 bytes | **84** |

`GetInfo(216)` (received) reads 33 in both and was never doubled.  So the delta
is real, expected, and an improvement: upstream's "sent" figure was simply
wrong.

**2. Failed DNS reports through the ordinary path — confirmed, but it had lost
something.**  An unresolvable host prints in the output window with no modal, in
both clients.  What differed is the text:

```
5.06        Unable to resolve host name for "x.invalid", code = 11001 (Host not found)
this build  Unable to resolve host name for "x.invalid"
```

Single-sourcing the completion into `LookupHostNameComplete` reduced the
adapter's report to a `bool`, so `WSAGETASYNCERROR`'s code and its text were
dropped.  That is an observable Windows change and it is not listed in §6, so
by §1's rule it is a defect — fixed in `ed60501` by giving `ITransport` a
**non-pure** `lastResolveError()` (default 0) rather than widening
`onResolved`, which would have touched every implementer and every test double
for a string suffix.  Re-measured afterwards: byte-identical to 5.06.  The
*initiation* failure that §6 actually describes (`WSAAsyncGetHostByName`
returning 0) is resource-exhaustion-only and was not provoked, but it now
records `WSAGetLastError` too, so it reports the same detail through the same
line.

**3. Cert-refused TLS teardown sends no close_notify — confirmed, and there is
no shipped client to compare with.**  A TLS server with a self-signed
certificate, counting *raw* bytes underneath `SslStream` so encryption does not
hide them: after the handshake the client sent **0 bytes** and then EOF.  A
close_notify would have been a small alert record.

The mechanism is visible in the diff.  Pre-PAL, `m_bSSL_Connected = true` was
set at `doc.cpp:5641` *before* `SSL_get_verify_result` was checked, so
`SSLCleanup`'s `if (m_bSSL_Connected) SSL_shutdown()` fired on a refused
certificate.  The adapter sets `m_tlsUp` *after* the check
(`pal/win_transport.cpp:506` vs the refusal return at `:499`), so `teardownTls`
skips the shutdown.  Benign: no application data was ever exchanged, the TCP
close still happens, and the only consequence is that a server may log an
unexpected EOF instead of a clean shutdown.

**Worth knowing for every other TLS comparison in this document: stock 5.06 has
no TLS.**  Pointed at the same world it answers `Line 28: Attribute not used:
use_ssl="y" (world)` and connects in plain text.  So "upstream" in §6 delta 3
means *this repo before the PAL*, not Nick's shipped client, and no §4a TLS item
can be differentially tested against 5.06 at all.

**4. Hard send-error leaves `FD_READ|FD_CLOSE` armed — no zombie, in either
build.**  A server that accepts, waits, then aborts with `SO_LINGER 0` (an RST,
not a FIN), while the world sends once a second across the abort:

```
t= 7s  Send=0      IsConnected=true
t= 8s  Send=30002  IsConnected=false     <- the abort lands
```

then the full disconnect banner, the statistics block, and `The "AbortWorld"
server has closed the connection`.  **Stock 5.06 produces the identical trace**
(with doubled byte counts, per delta 1).  So this provocation shows equivalence
rather than the fix: an RST gives the read side an event too, so the send error
is never the only signal, which is the case upstream mishandled.  Reaching the
original failure would need a peer that stops reading without closing — worth
noting as the one part of delta 4 still unexercised.

**5. Socket-create failure — not provokable here.**  It requires WSA
not-initialised or handle exhaustion; neither is reachable without deliberately
breaking the process, and the arm being tested is the one that then reports
through the generic connect-failure path.  Recorded as untested rather than
claimed.

### 8-end. What still needs the OTHER platform

Everything above was compiled and run on Windows.  Six of the files this pass
touched are built by the macOS/Linux side and have **not** been compiled or
tested there, so they want a `cmake --build` and a `ctest` run before anyone
calls this finished:

| file | change | why it matters over there |
|---|---|---|
| `utilities_shared.cpp` | `ColourToName` made deterministic (§8o) | **shared** — it changes which name the *mac* build writes into a world file, so it is the one to check first |
| `xml/xml_load_world.cpp` | the `#ifndef _WIN32` arm restoring the world-load warning (§8f) | port-only arm of a shared TU |
| `platform/mfc_ui.h` | DDV range wording (§8t) | port-only; a header, so it rebuilds a lot |
| `platform/wx_gdi_compat.cpp` | `GetTextMetrics` measures max char width for every font, not only fixed-pitch | port-only |
| `platform/miniwindow_selftest.cpp` | new assertion that max ≥ average char width | port-only; it is an in-app selftest, so `ctest` is exactly where it shows up |
| `platform/wx_outputpane.cpp` | comment only (§8q) | no behaviour |
| `pal/itransport.h` + `doc_shared.cpp` | `lastResolveError()` and the message suffix (§8w) | **shared** — but the new method is non-pure and defaults to 0, so the POSIX transport needs no edit and the mac message is unchanged.  Still a header both sides compile |

The Windows-shared files (`chatsock.cpp`, `doc.cpp`, `doc.h`,
`doc_shared.cpp`, `mcdatetime.cpp`, `methods_accelerators.cpp`,
`methods_output_shared.cpp`) all compile on both, but only Windows has run
them; the accelerator change in particular (`c24926f`) alters behaviour on both
platforms and its macOS arm is untested.

The macOS side may also want `lastResolveError()` implemented in its own
transport — `getaddrinfo`'s `EAI_*` code would make the port's message as
informative as Windows' — but nothing breaks without it.
