# Windows validation handoff

For the Claude instance on the Windows VM.  Your mission: build this repo's
Windows client, validate the recent cross-platform refactor, and check for
regressions against original/upstream MUSHclient behaviour.  Everything
Windows-side described below was written **by reasoning only** — no MSVC
compile or run has ever happened on it.  You are the first empirical check.

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
