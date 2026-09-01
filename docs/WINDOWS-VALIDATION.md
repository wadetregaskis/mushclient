# Windows validation handoff

For whoever next has a Windows build in front of them.  The job: build this
repo's Windows client, and check that the cross-platform refactor — and the
many fixes that have landed on top of it — did not perturb Windows
behaviour.  The prime directive is §1's: **Windows behaviour must not change**
unless the change is listed as intended.

> ## STATUS — 2026-08-20 (later)
>
> **Round one is done.  Round two is half done: it now builds and its
> scriptable half has been measured; its dialog-driven half has not.**
>
> **Round one (2026-08-08 … 2026-08-10)** compiled and ran this source on
> Windows for the first time.  Its record is **§8**, and it is measurement
> rather than reasoning: the build was broken at HEAD, and eight defects were
> found and fixed (`0abb31b`, `50a1303`, `c24926f`, `08c8fa8`, `e4236e8` — a
> complete zChat outage — `edc407e`, `ed60501`);
> §6's five deltas were measured (§8w); the four `sendvw.cpp` divergences were
> run (§8v); and most of §4 was answered differentially against stock 5.06.
>
> **Round two's first pass (2026-08-20)** is **§10**.  The build was broken at
> HEAD again in two places — one of them exactly as §9b predicted, the other
> not predicted at all — and both are fixed.  Debug and Release then build and
> run clean and agree with each other.  The three highest-risk §9c rows (MCCP
> `FDICT`, the unbounded subnegotiation, the NAWS 255) are confirmed against
> stock 5.06, as are ~25 more; **§4c-quaterquadragies' four font measurements
> are answered (§10g)** and so is §4c-quintricies except its live-world item.
> **One new defect was found in round-two code, and fixed: §10e** — the
> subnegotiation cap was not one-shot, so one hostile stream raised eight modal
> dialogs and put the attacker's filler in the output window.
>
> **The macOS side has now run (2026-08-22), and round two is merged.**  All 95
> commits compiled by clang on the first try; `ctest` 77/77 and ASan clean, once
> two things were dealt with.  `lint_doc_sections` was red — round two added
> twenty-one `UPSTREAM-BUGS` write-ups without their index rows, and that
> checker only runs here.  And `UPSTREAM-BUGS` 90's remaining half, the one
> thing §10af said only macOS could fix, is fixed: a no-return MXP handler no
> longer vetoes every tag in the five embedded engines.  Both blind edits
> §10af flagged turned out to be correct, and both are pinned now.  The full
> record, including three §10af claims that are wrong and one new defect this
> found, is the last subsection of **§10af**.
>
> **§10i is the one to read if you only read one.**  A spike measured what it
> would take to run the suite on Windows, and the answer is *days, not the six
> to ten engineer-weeks a survey of the tree predicted*: the tests already
> include the real headers, so on Windows they compile against real MFC.  One
> test file compiles with zero errors after four small fixes and links against
> the app's own objects with **zero unresolved symbols**; what is left is MFC
> console-host initialisation.  The prize is the **249 `#if*_WIN32` sites across
> 70 TUs whose Windows arm has never been executed under test**.
>
> **§8 and §10 supersede anything above them that they contradict.**  §§1–7
> keep their original wording so the predictions can be scored against what
> happened, so check a §4 section's **ANSWERED** / **OUTSTANDING** tag before
> working from its text.  One prediction in §4b was wrong; §4c-quater's premise
> was wrong for both clients (§8g); §4c-terdecies carries a correction that was
> itself re-opened (§8q); and §9c's `00f634e` row is right for the wrong-looking
> reason (§10c).
>
> **What round two still owes is §10h**, and it is essentially §4's
> dialog-driven half: the world-preferences walk (`44f0737`), the MXP veto
> (§4c-duodequadragies), everything needing a plugin, and everything needing
> real typed input.  §9 remains the inventory.

## How to use this document

1. **§10h** first — it is the shortest statement of what is left.
2. **§2** to build, **§3** for the baselines, **§3b** for how to drive the GUI
   without a human — round one worked that out and it is not obvious.
   `tests/mud_stub.py` is the loopback server; do not write another.
3. **§9** for round two's commit inventory, and **§10** for what the first
   round-two pass measured against it.
4. **§4** for the per-change checklists.  Each subsection is tagged with
   whether round one answered it.  The untagged 2026-08-19 sections
   (§4c-quintricies onward) are all round-two work.
5. **§5** and **§6** list what is *supposed* to differ.  Read both before
   filing anything.
6. **§7** for how to report.
7. **§10af** if you are on macOS rather than Windows — it is the handoff: every
   shared hunk this round changed, with what a `ctest` run should check.  §11
   describes the third test surface, which is Windows-only by design.

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

**The IWorldView adapter has kept growing since**, and each addition is a
place where a Windows body was relocated rather than reimplemented:
`mouseMoveMiniWindow` (§4c-undevicies), `setBookmark` (§4c-vicies),
`mainClientRect` and the world-child reading of "the main window"
(§4c-unquadragies), and `setCommandWindowHeight` / `setOutputWidthPixels`
(§4c-quinquadragies).  Two methods left §5's not-flipped list along the way —
`SetInputFont` and `GetMainWindowPosition`.

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

**It built clean at the end of round one, in both configurations.**  Debug|Win32
and Release|Win32 both link with no warnings from our own code; what remains is
`LNK4099` from OpenSSL's static libs (no PDB shipped), three pre-existing
`C4244`s in vendored `scripting/lua_compress.c`, and two `GetVersionEx`
deprecations in `sqlite3.c`.  The full account is §8a.

**Expect first-compile errors again anyway.**  Round two is 93 commits of
unverified canonical-source change (§9), so the same trap is open: fixing them
is part of the job.  Keep fixes minimal and semantics-preserving; if a fix
changes behaviour rather than syntax, treat it as a finding (§7).

**Where round one's 59 errors actually were, because the shape recurs.**  All
five sites were a declaration that had drifted into an `#ifndef _WIN32` arm
while its definition or its use stayed unconditional — nothing on macOS can
notice that, so it breaks Windows silently until somebody compiles.  The
worst of them was a *member* whose type came from a guarded `#include`: the
class body did not parse, and 55 of the 59 errors were that one omission
cascading.  §8a has the table.  **A shared header's `#ifndef _WIN32` block is
the most dangerous place in this tree to put an include.**

**Running the built exe needs runtime assets staged beside it.**  MSVC writes
`WinDebug\MUSHclient.exe`, but the app resolves `locale\en.dll`,
`spellchecker.lua`, `lua5.1.dll`, `tips.txt`, `help.db` and friends relative to
the executable.  A fresh `WinDebug` therefore gives, in order: a missing-DLL
system error, "Failed to load resources file", then a scripting error about
`spellchecker.lua`.  Copy the *data* files (not the exe) from an installed 5.06
and it runs.  Worth knowing before diagnosing a "broken build" that is really a
bare directory.

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

**Drive both clients with the same script rather than eyeballing them.**  Two
Lua conformance scripts live in `tests/lua/` and run in *any* MUSHclient —
paste into Game ▸ Immediate (Ctrl+I), or point a world's `script_filename` at a
driver that `dofile`s them.  Because the same file runs against stock 5.06,
"is this a regression?" becomes a diff instead of a judgement:

- `tests/lua/miniwindow_conformance.lua` — §4b's checklist as 64 probes.
- `tests/lua/api_conformance.lua` — the scriptable half of §4c as
  `key = value` lines, for diffing two clients.

They are deliberately **not** registered with ctest; they are comparison
instruments, not unit tests.

**And a server to point them at: `tests/mud_stub.py`** (added 2026-08-20;
Python 3, stdlib only, no build step).  It has an interactive mode — a fixed
banner and a deterministic command vocabulary, which is what
`docs/GUI-DRIVE-PLAN.md` drives — and a `--scenario` mode carrying the scripted
protocol sequences: `ttype`, `naws`, `mccp_normal`, `mccp_fdict`,
`subneg_unbounded`, `subneg_overflow_inject`, `subneg_big_ok`, `plain`
(`--list` prints them with what each is for).  Everything the client sends is
logged as raw bytes *and* as a decoded telnet trace, so "did it double the
255?" is answerable from a file.  **Extend it rather than writing another**: it
is the third such server this project has had, and the other two were an inline
copy in a doc and a PowerShell one written for round two.

**Reach for a unit test first.**  Most of the telnet phase machine, MCCP, NAWS
escaping and the subnegotiation cap is reachable headless from
`tests/test_telnet.cpp` against the real `CMUSHclientDoc`, and a unit test is
faster, deterministic and runs in `ctest`.  A live client is for what a unit
test structurally cannot see — the window, the modal dialogs, and the
Windows-only arms (`SendWindowSizes` derives its dimensions from `CMUSHView`,
which no headless build constructs).  Round two's three genuinely new findings
were all of that kind.

Five things round one learned about this baseline, each of which cost time:

- **Stock 5.06 has no TLS at all.**  Pointed at a `use_ssl="y"` world it
  answers `Line 28: Attribute not used: use_ssl="y" (world)` and connects in
  plain text.  So "upstream" in every TLS comparison means *this repo before
  the PAL*, not Nick's shipped client, and no §4a TLS item can be diffed
  against 5.06.
- **Verify any loopback MUD stub against a stock client before believing it.**
  A PowerShell `TcpListener` echo server produced a connection that established
  and carried no data — which looked alarming until 5.06 behaved identically
  against it.  The harness was wrong, not the client (§8d).
- **Pin both windows to the same size before comparing any width-derived
  value.**  The two clients restore different world-window placements, so a
  wrap column read from each differed by one and looked like a real divergence
  (§8s).
- **Do not probe from `OnWorldOpen`.**  The MDI child is still being created,
  so the view's client rect is an interim size (§8s).
- **A saved world file is byte-identical to 5.06's** apart from the version
  string and the timestamp — that is a strong, cheap whole-file check, and it
  only became true once `ColourToName` was made deterministic (§8o).

## 3b. Driving the app without a human

Round one's biggest methodological finding was that almost none of this needs
a person at the keyboard, and three items had been written off as unreachable
on the strength of being unreachable *from Lua*.  Do not re-derive any of it:

- **A script can run inside the view's message queue.**  `AcceleratorTo(key,
  script, 12)` binds in the `ACCELERATOR_FIRST_COMMAND` range handled by
  `CMUSHView`'s `ON_COMMAND_RANGE`, so posting `WM_MOUSEMOVE` and then
  `WM_COMMAND 12000` in one burst runs the script before the queued
  `WM_MOUSELEAVE` lands.  That is what makes mouse position testable (§8q).
- **Menu commands must be posted to the *frame*.**  At the view they are
  silently dropped (§8t).
- **`GetWindowText` cannot read another process's controls** — it returns the
  cached caption, empty for an `Edit`.  `WM_GETTEXT` works cross-process;
  `WM_SETTEXT` always did (§8t).
- **`WM_SETTEXT` does not raise `EN_CHANGE`**, so `m_bChanged` stays false and
  every "remember what they were typing" arm skips.  Type with one posted
  `WM_CHAR` per character (§8v).
- **Injected keystrokes need the focus, and this VM stacks three dialogs at
  startup.**  Until they are all dismissed the focus is the frame, not the
  command bar.  `GetGUIThreadInfo` is how to check without perturbing it (§8v).
- **`PrintWindow` with `PW_RENDERFULLCONTENT`** captures a window's own
  rendering without focusing or unobscuring it — that is how the mapper was
  looked at (§8p).
- **`WindowGetPixel` sampled into a checksum** compares two *processes'*
  rendering pixel-for-pixel, since it is computed in-process from the surface
  (§8x).

What stayed genuinely hard is (a) anything needing a real cursor position that
outlives the next queued message, and (b) judging what something *looks* like.

## 4. What to validate, in priority order

Every subsection written before round one now carries a tag saying whether
round one answered it.  **§8 never mentions fifteen of the thirty-six
subsections that predate it**, so the old 2026-08-10 status banner's claim that
"every §4c question then written is answered" was not true.  Six of the fifteen
are port-only or were answered in substance under another name; the other nine
were simply not reached — §4c-bis, §4c-septies, §4c-undecies, §4c-unvicies,
§4c-duovicies, §4c-septemvicies, §4c-octies-bis, and items 1–4 of
§4c-duodevicies.  In summary:

| Verdict | Sections |
|---|---|
| **Answered** | §4c, §4c-ter, §4c-quater, §4c-sexies, §4c-octies, §4c-duodecies, §4c-terdecies, §4c-quaterdecies, §4c-sexdecies, §4c-septendecies, §4c-undevicies, §4c-vicies, §4c-tervicies, §4c-quatervicies, §4c-quinvicies, §4c-sexvicies, §4c-duodetricies, §4c-undetricies, §4c-duodetricies-bis, §4c-tertricies, §4c-quatertricies |
| **Partly answered** | §4a (keepalives, long idle), §4b (the workload ran; appearance under movement did not), §4c-quinquies (items 1, 3, 5), §4c-duodevicies (items 1–4), §4d |
| **Outstanding** | §4c-bis, §4c-septies, §4c-undecies, §4c-unvicies, §4c-duovicies, §4c-septemvicies |
| **Open question, never answered** | §4c-octies-bis |
| **Nothing for Windows** | §4c-nonies, §4c-decies, §4c-quindecies |
| **Round two — never built or run** | §4c-quintricies … §4c-undequinquagies (all 2026-08-19) |

### 4a. Connection lifecycle (highest risk — total rewrite of the plumbing)

> **ANSWERED in part (round 1).**  Hostname DNS, connect, MOTD, send, IAC
> GA, disconnect statistics and reconnect: §8d.  The whole TLS matrix: §8i.
> SOCKS 4 and 5 at the byte level: §8p.  MCCP: §8l.  **Still outstanding:**
> keepalives, and a long idle session.

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

> **ANSWERED (round 1), with one prediction wrong.**  64/64 probes: §8c —
> which also found a *fourth* platform difference this section had missed
> (`tmMaxCharWidth` > `tmAveCharWidth` for a fixed-pitch face, fixed in
> `50a1303`).  Quow's pack: §8o.  The mapper looked at: §8p.  400 redraws
> checksum-identical to 5.06: §8x.  The Lua port of the checklist asked for
> below **already exists** — `tests/lua/miniwindow_conformance.lua`, see §3.

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

> **ANSWERED (round 1).**  Differentially against 5.06, §8e — identical
> output for the whole command-bar family, history, bookmarks, accelerators
> and printf handling; four differences found, all explained, none a
> regression.

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

> **OUTSTANDING.**  Round one never reached either check.

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

> **ANSWERED (round 1), §8e.**  It compiles, and `Note("literal percent:
> 100% done %s")` comes out verbatim.

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

> **ANSWERED (round 1) — and its premise was wrong.**  §8g: an empty `Send`
> does **not** un-bind, in either client.  §8j reads the code and explains
> why, and the erase was hoisted to both platforms in `c24926f`; §8l re-ran
> it.  A follow-up (`eb8c2fe`, §9) then fixed an id leak the hoist
> introduced.

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

> **PARTLY ANSWERED.**  Item 2's `#ifndef _WIN32` arm was restored (§8f) and
> the build is clean (§8a); item 5's `SafeArrayGetElement` leak was later
> found to have a second defect (`8ede7bf`, §9).  **Items 1, 3 and 5's
> script checks are outstanding.**

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

> **ANSWERED (round 1), §8e.**  Behaves identically in 5.06 and this build;
> the Lua-vs-`GetTriggerInfo` asymmetry is upstream's and is preserved
> exactly.

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

> **OUTSTANDING** — low priority: nothing is expected to differ on Windows.

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

> **ANSWERED in substance (round 1), §8e.**  `GetInfo(236)`/`(237)` are
> identical in both clients, including clamping past the end.

`CWorldChildFrame::commandSelection` / `setCommandSelection` /
`replaceCommandSelection` (`platform/wx_world_child.cpp`) used wxTextCtrl
CHARACTER offsets, while `GetCommandText()` and every shared body that scans it
index BYTES -- which is also what Windows' `CEdit::GetSel` / `SetSel` are over
its ANSI buffer. The seam now converts, via the new `UTF8ByteToCharOffset` /
`UTF8CharToByteOffset` in `utilities_shared.cpp` (**no Windows caller**).

### 4c-octies-bis. QUESTION TO ANSWER: what shape do wildcards reach a non-Lua callback in?

> **STILL OPEN — nothing has answered this.**  It is the one question in
> this document that needs a non-Lua ActiveScript engine installed, and no
> round has had one.

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

> **Nothing for Windows.**  Port-only; listed for completeness.

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

> **Nothing for Windows.**  Port-only; the predicates are pinned by
> inspection against `doc.cpp`.

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

> **OUTSTANDING.**  Port-only code, but the four "where these could be
> imperfect" questions at the end are about *Windows* and none has been
> answered.

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

> **ANSWERED (round 1), §8e.**  `SetScroll`'s argument really is a
> device-pixel Y and `GetInfo(296)` reports the same unit, in both clients.

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

**Note, since this was written:** the duplicate `SetScroll` this paragraph
warned about lived in `platform/doc_gui_method_stubs.cpp`, and **that file no
longer exists** — the whole stub family was adopted into `mushcore` during the
2026-08 endgame, so the twin-drift hazard it described is gone.

### 4c-terdecies. Miniwindow mouse events + two drawing fixes (2026-08-03)

> **ANSWERED (round 1), §8q** — every cited Windows reference behaviour
> confirmed.  Note the correction inside §8q was itself **re-opened**: the
> Ctrl+wheel flag question is not settled.

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

> **ANSWERED (round 1), §8t** — and it turned up a wording defect in the
> port (`edc407e`).

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

> **Nothing for Windows.**  Port-only.  Item 2 remains a consistency fix
> against a latent hazard, not a confirmed bug.

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

> **ANSWERED in substance (round 1), §8c** — mono pattern brushes 8–12 keep
> *both* caller colours (288 red / 288 green in the checkerboard, no black
> or white).

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
2. **Transparent gaps** (`fg == -1`, i.e. `BrushColour == -1`) are not
   expressible as a stipple without a mask, so they currently paint a flat tile
   in the resolved foreground instead of leaving the destination alone. Windows
   leaves it alone.

   **Correction (this row named the wrong field, and the code tested it).** For
   a mono pattern `BrushSpecFromWin32` puts `BrushColour` in `spec.fg`
   (`SetTextColor`) and `PenColour` in `spec.bg` (`SetBkColor`) — the reverse of
   the hatch arm — and upstream keys `SetBkMode` on **`BrushColour`**. So the
   transparent trigger is `spec.fg`, as `pal/isurface.h` and
   `pal/gdi_surface.cpp` both state. The wx adapter tested `spec.bg`, which
   broke it in two directions at once: a real `BrushColour == -1` request took
   the OPAQUE branch and painted the 1-bits in `PenColour`, obliterating what
   the plugin meant to show through; and a `PenColour` of `-1` — which Windows
   treats as an ordinary opaque background — collapsed the tile to one flat
   `BrushColour` where Windows draws a two-tone pattern. Fixed; both directions
   are pinned in `--pal-miniwindow-selftest`.

Worth confirming on Windows what styles 8-12 actually look like with distinct
pen and brush colours, so there is a reference to match. Two specific reads are
now owed, neither answerable from macOS:

- **What does GDI render for `CLR_INVALID` in the slot that is NOT the
  transparency trigger** — i.e. `SetBkColor (0xFFFFFFFF)` under
  `BrushColour != -1`? The high-order type byte is undefined: GDI either masks
  it to `0x00FFFFFF` (white) or fails the call and leaves the DC's default. The
  port assumes white. Drive
  `WindowCircleOp (w, 2, 4, 4, 40, 40, -1, 5, 1, ColourNameToRGB ("red"), 8)`
  and read the two tile colours back with `WindowGetPixel`.
- **And for `SetTextColor (0xFFFFFFFF)` under `BrushColour == -1`**, which is
  what the 0-bits take in the genuinely-transparent case. Same call with the
  two colours swapped.

### 4c-septendecies. HiDPI: BGR24 pixel READS are exact now (2026-08-03)

> **ANSWERED in substance (round 1), §8c** — the `WindowFilter` identity
> round-trip is lossless in both clients.

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

> **PARTLY ANSWERED.**  Item 5 (spell check on send) is §8u.  **Items 1–4 —
> Recall Last Word, Global Change, the multi-line caret gate and the
> line-ending promotion — are outstanding.**

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

> **ANSWERED (round 1), §8p and §8q** — the re-detect fires synchronously
> inside `WindowAddHotspot`, and the control holds.  A later fix (`47896a0`,
> §9) added a capture guard.

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

> **ANSWERED (round 1), §8e** — set, clear, `GetLineInfo(n,7)` agreeing both
> ways, out-of-range doing nothing.

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

> **OUTSTANDING.**  The one observable — a background world's unread count —
> was never checked.

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

> **OUTSTANDING.**  The `~` anchor prefixes were never checked.

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

> **ANSWERED (round 1), §8u.**

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

> **ANSWERED — all four items.**  Item 1 by inspection (§8m); items 2, 3 and
> 4 empirically (§8s).

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

> **ANSWERED (round 1), §8i and §8o** — saves are byte-identical between two
> runs, and byte-identical to 5.06's apart from the version string and
> timestamp.

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

  **Superseded, 2026-08-22.** The `bWriteZero` argument is no longer keyed on
  `OPT_ZERO_UNLIMITED` alone but on whether the option's *default* is non-zero
  — because four rows that were never given the flag had exactly the same trap
  and were losing a deliberate 0 on every save (`docs/UPSTREAM-BUGS.md` 101).
  The byte-identical check above therefore no longer holds: an ordinary world
  now also carries `pixel_offset`, `fade_output_opacity_percent`,
  `tool_tip_visible_time` and `tool_tip_start_time` whenever any of them is 0.
  That is the four attributes it should always have had, and nothing else
  changes — a row whose default is 0 is written exactly as before.
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

> **ANSWERED (round 1), §8i** — neither hazard fires, and the first provably
> cannot: modern MFC explicitly permits indexing the terminator.

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

> **OUTSTANDING**, and it is the highest-value of the unanswered pre-§8
> items: it is about trigger grouping, the hottest user-visible path in the
> app.

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

> **ANSWERED (round 1), §8l** — 5 torn characters in 5.06, 0 here, and the
> ASCII control byte-for-byte identical between the clients.

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

> **ANSWERED (round 1), §8i** — the whole 2×2 confirmed, including that
> answering Yes to an unverified certificate stays **encrypted**.

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

> **ANSWERED (round 1), §8l** — both deliberate changes confirmed with exact
> counts (4→2 logged, 1→3 echoed).

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

> **ANSWERED (round 1), §8i, §8p and §8r** — handshake, veto, zChat
> negotiation, and eight byte-exact file transfers.  §8p also found
> `e4236e8`, a complete zChat outage caused by modern ATL.

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

> **ANSWERED (round 1), §8e** — and it confirmed upstream really did accept
> an out-of-range value.

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

### 4c-quintricies. Colour-matching triggers resolve through the palette (2026-08-19)

`docs/UPSTREAM-BUGS.md` 75, `docs/UPSTREAM-DIVERGENCES.md` §11. Shared code:
`ProcessPreviousLine.cpp`, `pal/style_colour.h`. Commit `ef5466f`.

**This is the highest-regression-risk item of the whole campaign.** The comparison
every colour trigger goes through now resolves both sides to RGB instead of comparing
the trigger's 0-7 ANSI number against the style's raw members, so matching is by
appearance rather than by colour number. On Windows that changes a matcher that has
behaved one way since 2010.

**What to check on Windows:**
1. A trigger with *match text colour* = red, against a plain `ESC[31m` line: must still
   fire. This is the regression guard — everything that matched before must still match.
2. The same trigger against an `ESC[38;5;1m` (xterm-256) line and a
   `ESC[38;2;170;0;0m` (24-bit) line: **now fires, where it never did before.** That is
   the fix.
3. A trigger with *match text colour* = black against a `COLOUR_RGB` run that is not
   black: must **stop** firing. That false positive (`COLORREF 0x000000 == ANSI 0`) is
   the one removal.
4. Bold: a "red" condition must still match bold red — `HILITE` is carried across into
   the resolve, and boldness is the separate *match bold* condition.
5. Inverse, with **and** without `alternative_inverse` set: the two indices cross over
   under `INVERSE`, and the resolver (unlike the old hand-rolled swap) consults the bold
   table. Both arms need a look.
6. Run a real xterm-256 world with an existing, non-trivial trigger set for a session
   and watch for triggers that fire where they used to be silent. Nothing headless can
   answer that question.

### 4c-sextricies. Three Lua bindings join the other five engines (2026-08-19)

`docs/UPSTREAM-BUGS.md` 83 and 84, `docs/UPSTREAM-DIVERGENCES.md` §13 and §14. Shared
code: `scripting/lua_methods.cpp`. Commits `203c0fe`, `092c1f4`, `43e27b8`.

All three are one-token changes in canonical source that Windows compiles, and all three
are script-visible there.

**What to check on Windows:**
1. `world.ReplaceNotepad("t", "text")` from Lua returns **true**/**false**, not nil.
   Confirm the boolean is correct both ways (a title that exists, and one that does
   not). A script written `== nil` now takes the other branch — that is intended.
2. `world.SetTrace(1)` from Lua prints `TRACE: Trace on` in the output window, and
   `world.SetTrace(0)` prints `TRACE: Trace off`, exactly as `world.Trace = 1` from
   JScript already does. **Eyeball where the flushed half-built line lands** — the flush
   happens before `m_bTrace` is set, so a pending prompt should appear on its own line
   ahead of the marker.
3. A repeated `world.SetTrace(1)` must remain a no-op (no second marker), and
   `world.GetTrace()` must still report the member.
4. `world.GetLineInfo(n).time` on a line stamped after 2038-01-19 must be positive.
   Hard to provoke live; the cheap Windows check is that `.time` for an ordinary
   present-day line is unchanged to the second.

### 4c-septentricies. WindowFilter's pixels, and SQLite INTEGER widening (2026-08-19)

`docs/UPSTREAM-BUGS.md` 78, 79 and 80; `docs/UPSTREAM-DIVERGENCES.md` §12. Shared code:
`miniwindow.cpp`, `scripting/methods/methods_database.cpp`,
`scripting/lua_methods.cpp`, new `scripting/db_int64.h` (added to `MUSHclient.vcxproj`
as a `ClInclude`). Commits `d8de347`, `648622c`.

**What to check on Windows:**
1. `world.WindowFilter` with a blur over a region containing a single bright pixel: the
   result must be symmetric about that pixel's **own** row (it used to be centred one
   row below), and the rightmost pixel column of the region must be filtered like the
   rest (it used to survive untouched).
2. Any plugin that draws with `WindowFilter` — the output shifts by one row and gains a
   filtered right edge. Cosmetic, but plugin authors who tuned a blur by eye will see it.
3. `world.DatabaseColumnValue` on a column holding `2147483648`: must return
   `2147483648`, not `-2147483648`. From Lua and from JScript.
4. A column past 2^53 must come back as a **decimal string**. This is deliberate and
   lossless, but it is a type change: confirm `type(v) == "string"` in Lua and
   `VT_BSTR` from JScript.
   **Superseded later the same day — read §4c-sexquadragies before running this one.**
   `e24ab4b` replaced the 2^53 *magnitude* cap with a *value* test, so a value above
   2^53 that a double happens to hold exactly (2^60, `INT64_MIN`) is now a **number**
   again. As written here, item 4 would fail on those. The band it should be read as
   is "anything a double would round".
5. A column that fits in 32 bits must return the same type and value it always has —
   `VT_I4` / Lua number. Nothing that works today may move.
6. `CString::Format("%I64d", …)` behaves as it always has (MSVC's native modifier, so
   this is a formality — but the string band depends on it).

### 4c-duodequadragies. Vetoing an atomic MXP tag now unwinds it (2026-08-19)

`docs/UPSTREAM-BUGS.md` 85. Shared code: `mxp/mxpStart.cpp`, `mxp/mxpOpenAtomic.cpp` —
both in `MUSHclient.vcxproj` (`:835`, `:839`), so this ships to Windows. Commit
`3095c2a`.

Nothing here touches `CMUSHView`; the whole change is in the MXP engine, which the
headless tests do execute. What wants a Windows run is the interaction with real plugins
and the on-screen result.

**Note the two veto conventions are opposite, and the tests use the script one:** a
**plugin**'s `OnPluginMXPopenTag` vetoes by returning **false** (`MXP_StartTagScript`
negates it, `mxpStart.cpp:634-638`), while a **world script**'s `OnMXP_OpenTag` vetoes
by returning **true**.

**What to check on Windows:**
1. A plugin implementing `OnPluginMXPopenTag` that returns `false` for `p`. Have the
   server (or `world.Simulate`) send `<p>hello</p>world`. **Expect:** `hello` and
   `world` on ONE line. Before the fix they were on two.
2. Same plugin, vetoing `send`. Send
   `<a href="go &text;">click <send>me</send> now</a>` and click each part of the link.
   **Expect:** every part sends `go click me now`. Before the fix, the text from
   `<send>` onwards sent `go me`.
3. Same plugin, vetoing `b`. Send `<b>x</b>`. **Expect:** the plugin's
   `OnPluginMXPcloseTag` is NOT called for `b`, and the MXP debug window shows
   "Closing MXP tag </b> does not have corresponding opening tag" — that warning is the
   *expected, correct* new behaviour, not a regression.
4. `plugins/MXP_statusbar.xml` still works against a MUD that uses MXP status tags: its
   regex depends on the single-quoted argument form `MXP_OpenAtomicTag` builds, which
   this fix deliberately does not move.
5. **Control:** with the veto plugin unloaded, `<p>hello</p>world` must still break the
   line, and 100 unclosed tags must still raise "Now have 100 outstanding MXP tags".
6. Custom elements: `<!ELEMENT mytag '<b><i>'>` with the plugin vetoing `b`. **Expect:**
   the plugin is asked three times (`mytag`, `b`, `i`), the text is italic but not bold,
   and `</mytag>` closes cleanly with no warning.

### 4c-undequadragies. Bright SGR 90-97 / 100-107 and OSC swallowing (2026-08-19)

`docs/UPSTREAM-DIVERGENCES.md` §15 and §16. Shared code: `ansi.cpp`,
`telnet_phases.cpp`, both compiled unconditionally. Added in `f06e369`; recorded as a
divergence by the D-23 decision.

The code has shipped for a while; what is new is the decision that it belongs in the
Windows build too. It has never been run there.

**What to check on Windows:**
1. `ESC[91m bright red` renders in the world's **bright** red (bold palette in RGB
   mode; xterm palette index 9 in ANSI-table mode), not in the preceding colour. Same
   for 92-97 and for backgrounds 100-107.
2. A world in *custom colours* mode is promoted to RGB for these codes, exactly as it
   already is for 30-47.
3. `ESC[7m` inverse combined with a 90-97 code honours the inversion on both the
   RGB and the ANSI-table arm.
4. `ESC ] 0 ; some title BEL` produces **nothing** in the output window. Before, the
   payload text was printed as literal garbage. Confirm the `ST` (`ESC \`) terminator
   works as well as `BEL`.
5. Control: an unterminated `ESC ]` must not swallow the rest of the session — check
   what the phase machine does when the terminator never arrives.

### 4c-quadragies. The world-close split, the input-log guard, and the miniwindow re-lookup (2026-08-19)

`docs/UPSTREAM-BUGS.md` 76, 77 and 81. Shared code: `doc.cpp`/`doc.h`, `evaluate.cpp`,
`miniwindow_compose.cpp`. Commits `32b28f6`, `1d45c28`, `4d7168f`.

All three are behaviour-preserving on Windows **by reasoning**, and each has a specific
thing that reasoning could have got wrong.

**1. `SaveModified` split into `QueryCloseWorld` + `CommitCloseWorld` (`4d7168f`).** Same
code, same order; `SaveModified()` is now
`QueryCloseWorld() ? (CommitCloseWorld(), TRUE) : FALSE`. Two deltas, both believed
unreachable:

- **`m_bWorldClosing` on a refusal.** The old single
  `m_bWorldClosing = CDocument::SaveModified();` assigned FALSE when the user cancelled
  at the unsaved-changes prompt and left the member untouched when the user cancelled at
  the *earlier* two prompts. The new code assigns FALSE on all three. It is FALSE in
  every state that can reach there (initialised FALSE in `doc_construct.cpp:105`, and
  only a commit sets it), so this should be invisible.
- **`CommitCloseWorld` early-returns when `m_bWorldClosing` is already true.** The old
  code would have re-run `MXP_Off` and the close script on a second `SaveModified()` for
  the same document. The MFC paths were traced and no double call was found
  (`CFrameWnd::OnClose` runs `CanCloseFrame` -> `SaveModified` for a child frame with an
  active document; `SaveAllModified` -> `SaveModified` only for the main frame when
  `GetActiveDocument()` is NULL; `CloseAllDocuments` calls `OnCloseDocument`, not
  `SaveModified`), so the guard should be dead code on Windows.
  **Check: close a world, close the app, and close a world while several are open —
  `on_world_close` must fire exactly once each.**

Windows behaviour is otherwise unchanged: the macOS quit hook is the only caller that
uses the two halves separately, so Windows keeps `SaveAllModified`'s behaviour, half-
closed worlds and all (that upstream defect is `docs/UPSTREAM-BUGS.md` 81).

**2. `CSuppressInputLogging` in `evaluate.cpp` (`32b28f6`).** Additive and inert on
Windows: the sole Windows caller of `EvaluateCommand` is `Execute`, which passes
`bOmitFromLog = false`, so the guard neither reads nor writes `m_log_input`. **Check:**
with *File > Log Session* open and "log my input" on, a typed command still appears in
the log exactly as before — including a command typed at a masked (`IAC WILL ECHO`)
prompt, which on Windows still leaks and is *supposed* to until the decision in
`docs/UPSTREAM-BUGS.md` 76 is taken.

**3. Miniwindow mouse re-lookup (`1d45c28`).** Shared TU, so the fix lands on both
platforms. **Check:** no behaviour change in the *surviving* paths — a plugin whose
hotspot callbacks add/remove hotspots, drag, and move between windows must behave
exactly as before. The ten headless controls cover the abandoned branches; what Windows
adds is that the ordinary case still works.

**4. Reference claims read out of `doc.cpp:2101-2180`** for the port's Cmd+W chain
(`e08268f`, port-only). Sanity-check against a running Windows build if convenient: a
connected world (`ConnectionOpen()` **and** `m_iConnectPhase == eConnectConnectedToMud`)
with `App.m_bConfirmBeforeClosingWorld` set gets `"This will end your <name> session."`
as `MB_OKCANCEL | MB_ICONINFORMATION`; that warning is asked **before** the
`m_bSaveWorldAutomatically` save; and there is no general "Close &lt;world&gt;?"
confirmation anywhere.

### 4c-unquadragies. AddTrayIcon's new arm, the world-menu extraction and the main client rect (2026-08-19)

Shared code: `mainfrm.cpp`, `mainfrm.h`, `pal/win_worldview.cpp` (new override),
`scripting/methods/methods_info.cpp`. Commits `1ed8720`, `7d733e4`, `6b13a53`. Typed on
macOS; neither `CMyStatusBar` nor `CWinWorldView` can be executed here.

**1. `CMainFrame::AddTrayIcon` gained a `#ifndef _WIN32` arm.** The Windows body is
unchanged but is now wrapped in `#else … #endif`.
- [ ] The Windows build compiles `mainfrm.cpp` (the `#endif // _WIN32` sits between the
      `DestroyIcon` call and the closing brace).
- [ ] With Global Preferences > Tray/Taskbar > Icon Placement = *System tray*, the tray
      icon still appears at startup and after OK-ing the prefs dialog.
- [ ] Tray Icon 1..9 and a custom `.ico` still select the right icon.

**2. `CMainFrame::CollectWorldMenuItems`.** The world-list loop was lifted out of
`LeftTrayClick` unchanged (only a null guard on `App.m_pWorldDocTemplate` is added, which
cannot be reached on Windows). **To check:** open three worlds, give two of them unread
lines, then left-click the tray icon. The menu must list all three sorted by name, each
with its `" (n)"` unread suffix, and choosing one must still raise that world — exactly
as before. A world whose `m_view_number - 1` is >= `TRAY_MENU_COUNT` must still be
omitted.

**3. `TrackPopupMenu`'s return value** is now captured into `nSelected` and discarded
under `_WIN32` with `(void) nSelected;`. **To check:** the same tray-menu click — one
`WM_COMMAND`, one world raised, no double activation. If Windows ever raises two worlds
per click, this is the line.

**4. `pal::IWorldView::mainClientRect`** (new override in `pal/win_worldview.cpp`;
`methods_info.cpp` cases 249/250). Those two cases no longer have a `#ifdef _WIN32` arm:
they call `GetMainClientRect`, which on Windows goes through
`CWinWorldView::mainClientRect` -> the same `Frame.GetClientRect(&rect)` they ran inline,
returning `rect.bottom` as the height and `rect.right` as the width. **To check:**
`world.GetInfo(249)` and `world.GetInfo(250)` against the main frame's client area, on a
stock and a maximised window, and with the toolbars and status bar toggled — the numbers
must be identical to 5.06's. The doc's `m_pWorldView` is created unconditionally in the
ctor (`doc_construct.cpp:214`), so the no-view fallback is unreachable on Windows; if it
were ever reached, `GetInfo(249)/(250)` would answer a character-grid guess from
`m_gui_rows`/`m_gui_cols`, which Windows never updates.

**5. `mainfrm.h` now includes `<map>` and `<string>`** for the new method's signature.
Confirm the MFC precompiled-header build still compiles clean.

### 4c-duoquadragies. `script_strict_arguments`, and two canonical deletions (2026-08-19)

`docs/UPSTREAM-DIVERGENCES.md` §17, §18 and §19. Shared code: `doc.h`,
`scriptingoptions_numeric.inc`, `doc.cpp`, `doc_shared.cpp`, `OtherTypes.h`, `flags.h`,
`dialogs/world_prefs/prefspropertypages.{h,cpp}`, `dialogs/world_prefs/configuration.cpp`
— all in `MUSHclient.vcxproj`. Commits `fa694a4`, `0ad0cd7`, `1b17ec8`.

**1. The option exists, saves and loads, and changes nothing.** `doc.h` gains
`m_bStrictScriptArguments` and `scriptingoptions_numeric.inc` gains the
`script_strict_arguments` row; the consumer is `platform/mc_dispatch.cpp`, which is
`#if !defined(_WIN32)`.
- **Compiles and round-trips:** build the Windows target, open a world, and from a script
  run `world.SetOption("script_strict_arguments", 1)` then
  `world.GetOption("script_strict_arguments")` — it must answer 1. Save the world, reopen
  it, and read the option again: still 1. Confirm it also appears in
  `world.GetOptionList()`. This is the only thing the Windows build is expected to do
  with it.
- **It really is inert there:** with the option **on**, call `world.SetVariable("x")`
  from JScript or VBScript. It must fail exactly as it did before the option existed —
  `DISP_E_BADPARAMCOUNT` from COM, not the port's message — because COM refuses the call
  before any MUSHclient code is entered. The port's wording ("expected at least 2
  arguments, got 1") must **not** appear on Windows.
- **The claim the feature rests on, worth confirming empirically while there:** that
  `IDispatch::Invoke` really does refuse a short list for these methods. The tree has
  asserted it in a comment since `tests/test_mc_dispatch.cpp:388`, and the dispatch map's
  total absence of `VTS_VARIANT` is the reason, but it has never been observed on Windows
  in this project. `world.EnableTrigger("t")` from JScript is the cheapest probe: MFC
  should reject it rather than pass a default. **If it turns out COM supplies a missing
  optional instead, the parity argument in `docs/UPSTREAM-DIVERGENCES.md` §17 is wrong
  and should be rewritten** — the option would then be a port-only strictness rather than
  a return to Windows behaviour. (The feature itself still works either way; only the
  rationale changes.)
- **Not affected, but cheap to confirm:** `world.GetTrigger` / `GetAlias` / `GetTimer`
  from JScript, which on Windows take by-ref variables for their `VTS_PVARIANT` slots.
  The port counts only the script-supplied half of those parameter lists, so the two
  platforms genuinely differ in what "a complete call" means for exactly these three
  methods — as they already did before this change.

**2. `DelayedSend` and `CPrefsP2` are deleted.** Neither can have a behavioural test —
the removed code was unreachable on both platforms, which is the whole claim. **The
Windows requirement is a compile:** confirm `doc.cpp`, `doc.h`, `doc_shared.cpp`,
`OtherTypes.h`, `flags.h`, `prefspropertypages.{h,cpp}` and `configuration.cpp` all build
clean, and that World Properties still opens with every page present — `CPrefsP14`
("Output") is the live page that carries `m_new_activity_sound`, and its *Sound file* and
*Beep sound* fields must both still round-trip.

### 4c-terquadragies. Two items with no Windows half, recorded so the reference claim is checkable (2026-08-19)

Neither of these changed canonical source, so **there is no Windows behaviour to
re-confirm.** They are here because each rests on a claim about Windows that was read
from code rather than measured.

**1. Middle-click over the output window (`77c6f8f`, D-19).**
`platform/wx_outputpane.cpp` `COutputPane::OnMiddleUp` no longer pastes the system
clipboard into the command bar on non-GTK platforms. The claim it rests on is that
Windows does nothing with a middle click over the output view:

- `CMUSHView::OnMButtonDown` (`mushview.cpp:2304`) and `OnMButtonUp`
  (`mushview.cpp:2420`) offer the point to `Mouse_Down_MiniWindow` /
  `Mouse_Up_MiniWindow` with `MW_MOUSE_MIDDLE` and otherwise chain to
  `CView::OnMButtonDown` / `CView::OnMButtonUp`, which are the MFC defaults through to
  `DefWindowProc`.
- `CMUSHView::PreTranslateMessage` (`mushview.cpp:4143`) relays `WM_MBUTTONDOWN` /
  `WM_MBUTTONUP` to the tooltip control only.
- `sendvw.cpp` has no `OnMButton*` / `WM_MBUTTON*` handling at all.

**To confirm on Windows:** middle-click over the output window of a world with text on
the clipboard. Nothing should appear in the command bar. Also confirm a plugin hotspot
with a middle-click callback still fires (that path is unchanged on both platforms).

**2. Owner-draw colour combos (`8314538`, D-14).** `CBS_OWNERDRAWFIXED` was ignored by
`platform/wx_dialog_bridge.cpp`, which built every `ComboBox` template as a plain
`wxChoice`/`wxComboBox`, so the five colour drop-downs listed the strings
`Custom1 … Custom16` / `Other ...` instead of painting each row in the colour it stands
for. `CColourComboBox::DrawItem` is correct and Windows runs it; the port never called
it. The complete set of owner-draw templates in `platform/mushclient_dialogs.rc`:

| Dialog | Control | Style | Class |
|---|---|---|---|
| `IDD_EDIT_TRIGGER` | `IDC_TRIGGER_COLOUR` | `0x50210213` | `CTriggerDlg::m_ctlTriggerColour` |
| `IDD_HIGHLIGHT` | `IDC_TRIGGER_COLOUR` | `0x50210213` | `CHighlightPhraseDlg::m_ctlTriggerColour` |
| `IDD_PREFS_P9` | `IDC_ECHO_COLOUR` | `0x50210213` | `CPrefsP9::m_ctlInputColour` |
| `IDD_PREFS_P17` | `IDC_TEXT_COLOUR` | `0x50210213` | `CPrefsP17::m_ctlTextColour` |
| `IDD_TEST_RESULT` | `IDC_TRIGGER_COLOUR` | `0x58210213` | *(orphan resource — no class declares `IDD = IDD_TEST_RESULT`, nothing calls `DoModal` on one)* |

Low word `0x0213` = `CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS`;
`IDD_TEST_RESULT`'s `0x58…` adds `WS_DISABLED`. No `ListBox` template carries
`LBS_OWNERDRAW*` and no combo carries `CBS_OWNERDRAWVARIABLE` — the bridge accepts both
bits anyway, since Windows treats either as "the owner paints".

**To check on macOS (there is no Windows half):** World Properties > Triggers >
add/edit a trigger, open the *Change colour to* drop-down. Each row must be filled with
its custom colour, `(no change)` must be plain black-on-white, and `Other ...` must use
the two "other" swatches beside the combo. Picking a row must still update those
swatches, and picking `Other ...` must still open the colour picker (that is
`CBN_SELCHANGE` -> `CTriggerDlg::OnSelchangeTriggerColour`). Then OK, reopen the trigger,
and confirm the colour you chose is the one selected — the value round-trip is the part
most likely to fail silently.

**The one unmeasured link:** that `wxOwnerDrawnComboBox` raises `wxEVT_COMBOBOX` (rather
than `wxEVT_CHOICE`) when the user picks a row is taken from wx's documented behaviour,
not measured here — a selftest exits before the event loop, so no popup can be opened and
no real pick can be made. The half that lives in our code *is* pinned: the selftest
asserts the control is a child of the dialog and that a `wxEVT_COMBOBOX` from it reaches
the dialog's `CBN_SELCHANGE` binding. If the swatches fail to follow the drop-down in the
manual check above, that assumption is where to look first.

**Known cosmetic gap:** `CDC::DrawFocusRect` is a permanent no-op in the shim
(`platform/mfc_ui.h`), so the dotted focus rectangle the canonical body draws for
`ODS_FOCUS` does not appear. `ODS_FOCUS` *is* passed through faithfully, so this fixes
itself if `DrawFocusRect` is ever implemented.

### 4c-quaterquadragies. Miniwindow font parity: what a Windows run must measure (2026-08-19)

> **ANSWERED (round 2), §10g** — all four measurements taken, and written up in
> `docs/FONT-PARITY-2026-08.md` §9. The size pipeline is exact; the
> `pitchAndFamily` gap is real but far narrower than §2 assumed; and §3's
> proposed `CpuRaster` fix is **refuted** — see §3's update before acting on it.
> Still open: the ascent/descent split needs font-info codes 2 and 3 read for a
> scalable face, which this run did not take.

See `docs/FONT-PARITY-2026-08.md` for the full derivation. Nothing in canonical source
changed — `d02c4b9` deleted a block that was inside `#ifndef _WIN32`, and
`pal::GdiSurface::createFont` is untouched — so **Windows behaviour is unchanged by
construction**. This entry exists because the port's remaining size gap is attributed to
*face substitution*, and that attribution is the one part of the analysis that cannot be
settled from the macOS side.

**What to measure on Windows** (the Lua probe is in `docs/FONT-PARITY-2026-08.md` §5;
paste it into Game > Immediate):

1. **Info code 21 (`GetTextFace`) for a `Dina` request.** If it does not say `Dina`,
   Dina is not installed on that machine and the whole bitmap-strike line of enquiry
   collapses — the difference is then between two *different* substitutes, and the fix is
   to honour `pitchAndFamily` so both platforms substitute by the same rule.
2. **Info code 1 (`tmHeight`) and code 4 (`tmInternalLeading`) for Dina 8 pt.** A raster
   strike should show `tmInternalLeading` ≈ 0; a scalable face shows ~2-3. This is what
   decides whether Windows genuinely renders larger.
3. **A known-present scalable face as the control** — Courier New at 8/9/10/12/14 pt.
   Windows `tmHeight` should be within ±1 px of the macOS figure for the same face and
   size (macOS measures 13/14/15/19/22). **A difference larger than that falsifies the
   claim that the size pipeline is now exact**, and would mean a residual scale factor
   still exists.
4. `WindowTextWidth("MMMMMMMMMM")` for Courier New 10 pt: the port produces ~79 at 100%
   scaling, and Windows should agree.

Do **not** treat the headless `pal::CpuRaster` numbers as the parity target — it reports
the em as `tmHeight` and is short by the internal leading (ratio ~1.18). See
`docs/FONT-PARITY-2026-08.md` §3.

### 4c-quinquadragies. The window-geometry seam, and multi-select file dialogs (2026-08-19)

D-12 and D-15, which landed after the rest of the campaign's documentation was
consolidated. Commits `4e4ce04`, `b29469d`, `37bebec` (D-12) and `5d6345e` (D-15).

**1. Two engine bodies were split into an engine half and a `pal::IWorldView` half**
(`setCommandWindowHeight`, `setOutputWidthPixels`). Both Windows arms live in
`pal/win_worldview.cpp` and were **mechanically diffed** against the deleted originals
with whitespace normalised: 24/24 and 21/21 lines identical. The only substitutions:

| Original | Adapter | Why |
|---|---|---|
| `GetChildFrame ()` | `m_pDoc->GetChildFrame ()` | the adapter's standard doc reach, as every other method there |
| `m_doc->` | `m_pDoc->` | same |
| `return eBadParameter;` | `return false;` | the engine site converts it back to `eBadParameter` |
| `return eOK;` | `return true;` | ditto |
| `(tm.tmAveCharWidth * iColumn) + … + m_iPixelOffset +` | `textWidth +` | the same sum; the engine site computes `aveCharWidth * column + pixelOffset` |

Windows behaviour is therefore unchanged **by construction**. Still worth a run, because
`CSendView` / `CMUSHView` are never instantiated here:

- [ ] `world.SetCommandWindowHeight(n)` still resizes the splitter, and still returns
      `eBadParameter` for `n < 0`, for a too-small top view, and with no child frame.
- [ ] World Properties ▸ Output ▸ **Adjust size to width** still widens the world window
      to the typed column, using the **document's** current font (`m_font[0]`) — *not*
      the font offered on the page. That asymmetry with the sibling **Adjust width to
      size** button (which builds a `CFont` from the page's own pending fields) is
      upstream's, and was preserved deliberately. The two buttons are not exact inverses.
- [ ] One deliberate **non-verbatim** addition on Windows:
      `CMUSHclientDoc::OutputFontCharWidth` returns 0 if `m_font[0]` is NULL rather than
      calling `SelectObject(NULL)`. Upstream reached that code only from behind an "a
      view exists" guard, so this can only change a path that previously crashed.

**Two port-only behaviours introduced, recorded here rather than as divergences** (they
are macOS-vs-Windows differences, which this project keeps in code comments and notes,
not in `UPSTREAM-DIVERGENCES.md`):

* `CWorldChildFrame::setCommandWindowHeight` imposes a **minimum command-bar height of
  one text line plus the control's chrome** (the bar's default height, Menlo 9 is about
  20 px here). Windows has no such floor — it hands the splitter row a 9-pixel `cyMin`
  and relies on the user dragging the sash back. The port has **no sash and no per-world
  geometry persistence**, so a bar shorter than one line would be unrecoverable except by
  editing the world file. A request below the floor is refused with `eBadParameter`
  rather than clamped, matching the shape of the canonical body's own "too small, want to
  see a line at least" refusal.
* `CWorldChildFrame::setOutputWidthPixels` **refuses while maximised or full screen**
  (`eBadParameter`). wxOSX has no `rcNormalPosition` equivalent, so there is no restored
  rect to widen; approximating one would resize the wrong rectangle — the trap
  `wx_geometry_rect_for_save` already exists to avoid.

Neither feature writes `m_nWrapColumn`: `AutoWrapColumnFor` still owns "does the window
width drive the wrap column?", and only when `auto_wrap_window_width` is on. And
`m_pActiveCommandView` / `m_pActiveOutputView` were **not** faked — they are read only
inside the Windows adapter, under the original guard.

**2. Multi-select file dialogs (D-15).** Nothing here needs a Windows run for
correctness — the change is entirely inside the non-Windows shim and
`platform/wx_mfc_impl.cpp`, and no canonical `.cpp` was touched. One behavioural claim
*about* Windows is inferred from MFC's source rather than observed, so it is worth an
eyeball if anyone is in front of a Windows build:

- [ ] Global Preferences ▸ Startup Worlds ▸ Add, select several worlds: all of them are
      added. This is the already-working Windows behaviour the port now matches; nothing
      in the port changes it. The entry exists only to record that the macOS behaviour is
      now claimed identical.

Note what Windows does with the *other* accessors under multi-select, because the port
deliberately does **not** copy it: MFC packs the answer into `m_ofn.lpstrFile` as
`"dir\0name1\0name2\0\0"`, so `GetPathName()` returns the **directory**, and
`GetFileName`/`GetFileExt`/`GetFileTitle` read `lpstrFileTitle`, which the common dialog
does not fill under multi-select (MFC `ASSERT`s against calling them in that state).
None of the three multi-select call sites calls any of the four — all use
`GetStartPosition`/`GetNextPathName` exclusively — so the port makes all four report the
**first** path chosen, documented at the accessors in `platform/mfc_ui.h`. A bare
directory returned from something named `GetPathName` is the *golden adapter,
plausible-wrong* shape.

**Coverage limit, stated:** the panel is a `wxFileDialog` inside `mushgui_adapters`,
which `mushcore` deliberately does not link, and it is modal — so it cannot be shown from
a headless test and an in-app selftest would block on it. The tests drive the
**iteration layer** through the `g_wxhook_file_dialog` seam. Covered: the shim's request
(does it ask for multi-select, and only on open panels), its parse of the returned block,
and the enumeration. **Not** covered: `hook_file_dialog`'s `wxFD_MULTIPLE` style bit and
its packing of `wxFileDialog::GetPaths()` — that half runs only with a real panel on
screen.

### 4c-sexquadragies. The two SQLite surfaces band by value, and agree (2026-08-19)

`docs/UPSTREAM-BUGS.md` 87, and a revision of the D-07 half of §4c-septentricies.
Shared code: `scripting/db_int64.h` and `scripting/lua_methods.cpp` (`e24ab4b`),
`lsqlite/lsqlite3.c` (`fbb6695`). All three are in `MUSHclient.vcxproj`, so both
commits ship to Windows.

**What changed, in one sentence each.** `e24ab4b` moved the four `world.Database*`
column readers' middle band from "magnitude at most 2^53" to "the value survives a
round trip through a double". `fbb6695` fixed the bundled Lua `sqlite3` module's own
round-trip test, which widened both operands and so was vacuously true — its exact
decimal-string branch had never run. The two surfaces now band identically; before
these two commits they disagreed in **both** directions at once.

**What to check on Windows:**
1. `world.DatabaseColumnValue` on `1152921504606846976` (2^60): a **number**, not a
   string. This is the case §4c-septentricies item 4 got wrong, and it is the one
   most likely to be mis-tested.
2. The same column read through the Lua `sqlite3` module (`sqlite3.open_memory()`,
   `db:urows`/`db:rows`/`vm:get_value`): also a **number**, and equal to it.
3. `9007199254740993` (2^53+1) through **both** surfaces: a lossless decimal
   **string** from both. Before `fbb6695` the module returned the Lua number
   `9007199254740992` here — a silently wrong value, which is the defect.
4. `INT64_MAX` (`9223372036854775807`) through both: a **string**. This is the input
   the range guard exists for. `(double) INT64_MAX` rounds *up* to exactly 2^63, and
   casting that back is undefined behaviour — arm64 saturates to `INT64_MAX` and would
   call the value exact, x86-64 yields `INT64_MIN` and would not. **MSVC/x86-64 is the
   platform this was never run on**, so this row is the one that actually needs a
   Windows machine rather than reasoning.
5. `INT64_MIN` (`-9223372036854775808`) through both: a **number** — it is exactly
   `-2^63` and a double holds it perfectly.
6. A 32-bit-range value: unchanged type and value on both surfaces (`VT_I4` / Lua
   number). Nothing that works today may move.
7. `db:last_insert_rowid()` on a table whose rowid exceeds 2^53: the module's
   `lua_pushfstring(L, "%I64d", …)` arm is now reachable for the first time, so this
   is the one place the fix depends on **MSVC's** `%I64d` rather than ours.

**A plugin-visible type change, on Windows too.** Any plugin reading a large integer
through the `sqlite3` module now gets a string where it got a (wrong) number. That is
the point of the fix, but it is the kind of thing to say out loud before a release.

### 4c-septenquadragies. Two canonical edits with nothing to observe (2026-08-19)

Both are behaviour-neutral by construction; they are here so that a Windows build is
known to be needed, not a Windows *run*.

**1. The `m_bUTF_8` seed line is deleted** (`336625d`, `doc_construct.cpp`). The
constructor set `m_bUTF_8 = false` explicitly, two lines after `SetDefaults(false)`
had already set it from `OptionsTable`'s `utf_8` row. The two writes **agreed**, so
nothing was observably wrong — but the table was not actually authoritative, and a
change to its default alone would not have moved a fresh document at all. This is the
*last-writer-wins hides a mutation* shape the project keeps meeting. A fresh document
gets the same value before and after.

- [ ] Compile only. A new world's Special ▸ ... ▸ UTF-8 state is unchanged.

**2. The delayed-alias comment web is deleted** (`893395a`). Seven canonical files
(`OtherTypes.h`, `scripting/methods/methods_aliases.cpp`, `xml/xml_save_world.cpp`,
`dialogs/world_prefs/prefspropertypages.cpp`, `aliasdlg.h`, `aliasdlg.cpp`,
`world_debug.cpp`) lose **comments only** — every member of the web was already
commented out, and was at the 2010 import. Verified dead first by grepping `bDelayed`,
`eDelayed`, `IDC_DELAYED` and `OnUpdateDelayed` across every `.cpp`/`.h`/`.vcxproj`/`.rc`
and the `plugins/` XML samples. Recorded as `docs/UPSTREAM-DIVERGENCES.md` §18's update.

- [ ] Compile only, in all configurations — the point of the check is that seven more
      files now conflict on an upstream merge, not that anything runs differently.

### 4c-duodequinquagies. Port-only changes whose *Windows reference claims* want checking (2026-08-19)

Same shape as §4c-terquadragies: none of these has a Windows half at all — the code is
in `platform/`, which `MUSHclient.vcxproj` does not build — but each rests on a claim
*about* Windows that was read out of the source and never executed. If a claim is
wrong the port's design argument is wrong, so they are worth a spot-check by whoever
next has a Windows build in front of them.

**1. The connected-time pane (D-13, `65fa4ed`).** The port's redesign rests on
`m_tStatusTime` having exactly three writers — the document constructor
(`doc_construct.cpp:364`), Game ▸ Reset Connected Time (`doc.cpp:3607`) and
`world.ResetStatusTime()` (`scripting/methods/methods_output.cpp:87`) — and on nothing
on connect or disconnect touching it.

- [ ] Open a world and do **not** connect. The `ID_STATUSLINE_TIME` pane counts up
      anyway. (If it does not, the "despite its name, Windows measures the window's
      age" reading is wrong.)
- [ ] Connect, wait, disconnect, wait. The pane keeps counting across all four.
- [ ] Game ▸ Reset Connected Time zeroes it *while disconnected*.

**2. The output pane's right-click menu (D-29, `3fe8413`).** The merged menu's
grey-vs-omit decisions were taken from `CMUSHView`'s message map, not from a run.

- [ ] Right-click the output pane with **no** selection: Windows raises its menu
      **without** selecting a word first (`CMUSHView::OnRButtonDown`,
      `mushview.cpp:2283`, falls through to `CView`). The port deliberately differs;
      this confirms what it differs *from*.
- [ ] With no selection, **Go To URL…** and **Send Mail To…** are greyed
      (`OnUpdateNeedSelection`, `mushview.cpp:128`, `:159`), and **Highlight Word…**,
      **Multi-Line Trigger…** and **Bookmark Selection** are **not** — the port copies
      that asymmetry rather than tidying it.
- [ ] The MXP arm (`IDR_MXP_MENU`) really does run *before* `CG_IDR_POPUP_MUSHVIEW`
      (`mushview.cpp:3560`), which is why the port puts the link group first.

**3. An explicitly-passed nil (`8fa8baa`).** `platform/mc_dispatch.cpp` is port-only
and the fix is Lua-parity by construction, so there is nothing to validate — but there
is one question only Windows can answer, and the divergence file's §17 argument brushes
against it:

- [ ] From JScript or VBScript on Windows, call a world method with an explicit
      `null`/`Empty` in a slot that has a documented default — e.g.
      `world.EnableTrigger("t", null)`. Does MFC's `oledisp.cpp` coerce the `VT_EMPTY`
      to 0 (disabling the trigger), or does it supply the default? **Do not assume**:
      `oledisp.cpp` is not in this tree, standard OLE `VariantChangeType` would give 0,
      but that is inference. The port's rule is Lua's, deliberately, and stays Lua's
      either way — this only settles whether the port is *also* matching COM here or
      diverging from it.

### 4c-undequinquagies. A gagging trigger's backwards walk (2026-08-19)

`32d5597` changed `ProcessPreviousLine.cpp`, which `MUSHclient.vcxproj` builds
(line 336) behind no platform ifdef, so **Windows compiles the new code**. The
omit-from-output loop now captures its backward step and its stop condition
*before* `RemoveTail`, where they used to be taken after it:

    delete pLine;
    m_LineList.RemoveTail ();     // unlinks and frees the node `pos` names
    if (pos == prevpos) break;
    m_LineList.GetPrev (pos);     // reads the freed node's prev pointer

On the port that read comes back as whatever libmalloc left in the block —
usually null — so the loop stopped after one line, and a gag on a **wrapped**
paragraph left most of the paragraph on screen. It is the port's bug in effect,
but it is not port-only code.

**The claim this entry exists to check is that Windows was never affected.** It
rests on `CList::FreeNode` pushing the node onto `m_pNodeFree` and leaving
`pPrev` intact rather than returning it to the heap — so the stale read was of a
correct pointer, in a block that is still allocated, which is also why the
`ASSERT (AfxIsValidAddress (...))` inside `GetPrev` passes in a debug build and
nothing upstream ever noticed. That is read out of the MFC sources, not run. If
it holds, stock 5.06 and the new build behave identically and nothing is owed;
if it does not, this is a **live** upstream bug and wants its own row in
`docs/UPSTREAM-BUGS.md` instead of the "latent upstream" reading the commit
message gives it.

Note what that does *not* say. The two versions are not two different walks on
Windows: the only dereference of a freed node was `GetPrev`, and the
`pos == prevpos` test compares addresses without touching the block. So the
checks below test the **premise** — that `pPrev` survived the free — and not the
rewrite, which given that premise is the same traversal, one step earlier.

- [ ] A trigger with **omit from output** set, matching a line long enough to
      wrap into three or more screen rows (narrow the window, or have the MUD
      send a long line). On **stock 5.06** the whole paragraph must vanish —
      every wrapped row of it, not merely the last. Rows left behind mean the
      bug is live on Windows too, and this entry's premise is wrong.
- [ ] The same trigger on the new build: same result, and the lines *in front
      of* the gag identical to stock's.
- [ ] Fire it repeatedly — fifty gagged paragraphs — in a **debug** build, where
      MFC's allocator diagnostics are live, and confirm no assertion, crash or
      corrupted scrollback.
- [ ] A gag whose batch is a **single, unwrapped** line, so `pos == prevpos` on
      the first pass and the loop breaks immediately. Exactly that line goes and
      nothing before it. The port's test wraps its fixture deliberately, so this
      arm is not what it pins.

Covered on the port by `PalWorldView.GagDeletesExactlyItsOwnBatchAndNothingBefore`
(`tests/test_pal_worldview.cpp:2060`), whose fixture asserts the paragraph really
wrapped — a single-line batch never reaches the step under test. The sibling loop
that was given this same fix long ago, and carries a comment describing the
hazard, is `DeleteLines` at `scripting/methods/methods_output_shared.cpp:92`.

### 4d. Everything else (smoke)

> **PARTLY ANSWERED.**  Round one exercised world open/save, prefs dialogs,
> triggers, logging, plugins and Lua at Quow's scale.  Round two adds ~93
> commits of change to smoke over — see §9.

World open/save, prefs dialogs, triggers/aliases/timers on live MUD output,
logging, notepads, plugins list, scripting languages you have available.
These paths were mostly untouched on Windows but the doc grew members and
several TUs changed — a broad smoke matters.

## 5. Deliberately NOT flipped (don't report as missing/broken)

> **Two entries have come off this list since it was written**, and the rest
> was last re-derived on 2026-08-11.  `SetInputFont` was unforked (see the note
> at the end of this section).  **`GetMainWindowPosition` was unforked on
> 2026-08-19** by `6b13a53`, which routed it — and `GetInfo(249)`/`(250)` —
> through `pal::IWorldView::mainClientRect`; its Windows checks are
> §4c-unquadragies item 4, not this list.  Treat the remainder as accurate as
> of 2026-08-11 and re-derive before relying on any single entry.

These sites still run their original Windows code directly; each is a
genuine semantic fork, documented in commits `de9970b`/`a9061ce`:
`PasteCommand`,
`SendTo`→command (plugin-notify delta), `ChangeInputFont`'s view fixup,
`MoveWorldWindowX`/`GetWorldWindowPositionX` (`Which`>1 multi-view), Lua
`GetMainWindowPosition`/`GetWorldWindowPosition` (param variants),
`Activate` (child-frame semantics), `SetScroll` (Visible flag),
`DeleteLines` view resync, `GetInfo(296)`, `GetInfo(257/258)`.  Also the
whole **display family** (line rendering) — unchanged pull model.

**`SetInputFont` was struck off this list on 2026-08-11 and unforked.** The
premise recorded in `de9970b` — "one shared monospace face in the terminal
layout" — was never true here: the command bar has its own font, its own
preference, and its own live seam (`CWorldChildFrame::setInputFont`), which
`ChangeInputFont` already drives through `EmitInputFont`.  The fork made
`world.SetInputFont` restyle the MUD **output** and never touch the command
bar, and because the port's `setOutputFont` also writes the doc's output-font
members, a save then persisted an output font the user never chose.  Both
platforms now run the one canonical body, so nothing here needs a Windows run
— but it is worth an eyeball that the Windows command window still restyles,
since `ChangeInputFont`'s own `CSendView` fixup is the arm that does it.

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

> **Both were settled in round one — §8m.**  The selection math was diffed
> against the four original getters and then confirmed empirically in §8e; the
> message-only window resolved a real hostname (§8d) and survived dozens of
> connect/disconnect cycles, TLS teardowns and an automatic reconnect.  One arm
> stayed untested: `connect()`'s immediate-success path, which MFC can take
> inline on loopback.

---

## 8. Round one — results of the first empirical pass (2026-08-08 … 08-10)

> This section is the **record of what was measured**, and it supersedes any
> §§1–7 prediction it contradicts.  It answers nothing added after 2026-08-10;
> for that, see §9.

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
| ASCII control (wraps at a space) | byte-for-byte identical between the two clients — the space stays at the END of the old line, no continuation begins with one | *(same)* |

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
Ctrl+wheel that "the flags a plugin receives include `MW_MOUSE_CTRL`".  The
flags `OnMouseWheel` *builds* do not: it uses
`(zDelta < 0 ? MW_MOUSE_SCROLL_BACK : 0) | (abs (zDelta) << 16)`
(`mushview.cpp:5160`) and never consults `nFlags`; measured here as `0x780000`
for a 120-unit forward notch with MK_CONTROL held.  The port's forwarder
(`wx_world_child.cpp:1071`) builds the identical expression, so the two agree
about *that* value.

> **⚠ RE-OPENED 2026-08-10 — this correction over-reached, and the macOS side
> now contradicts it.**  The paragraph above concluded "they do not, on either
> platform", and the comment in `platform/wx_outputpane.cpp` was changed to
> match.  But the built value is not the delivered one.  Both frontends hand
> off to the **shared** `MouseWheelMiniWindow`, which calls
> `SendMouseEventToPlugin` (`miniwindow_compose.cpp`) *without* the
> `dont_modify_flags` argument — and it defaults to `false` (`miniwindow.h`) —
> so `Flags |= hooks.KeyboardModifiers ()` runs before the callback is
> dispatched.  Both frontends' `KeyboardModifiers` set `MW_MOUSE_CTRL` from the
> live key state (`mushview.cpp` via `GetKeyState`, `wx_world_child.cpp` via
> `wxGetKeyState`).  A Ctrl+wheel over a hotspot should therefore arrive as
> `0x780002`.
>
> This is now **pinned on macOS** by `platform/miniwindow_selftest.cpp`, which
> drives `MouseWheelMiniWindow` with a hooks object reporting Ctrl and asserts
> the modifiers are consulted, with a control asserting they are *not* when the
> hotspot has no scrollwheel callback (`SendMouseEventToPlugin` returns before
> the OR in that case).  Passing `, true` at that call site fails exactly the
> first assertion — so the test discriminates the two claims rather than merely
> restating one.
>
> **What needs a Windows run:** the `flags=0x780000` in the table above is
> reported as what the *plugin's handler logged*.  If that is accurate, the
> shared code cannot be doing what it plainly reads as doing, so one of the two
> observations is mis-attributed — most likely the value was read at the call
> site rather than inside the callback.  Re-measure by printing the flags
> argument from **inside** a scrollwheel callback with Ctrl held, and record
> `0x780002` or `0x780000`.  Until then the sibling comment in
> `platform/outputpane_selftest.cpp` — which still says Windows delivers
> `MW_MOUSE_CTRL`, and was never touched by the correction — should be treated
> as the open question, not as settled either way.

The *ordering* half of that bullet — hotspot before zoom — is unaffected by any
of this, is confirmed above, and is the part that mattered.

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

Everything that was ever on this list has come off it except one judgement call:
SOCKS, zChat, the mapper's appearance, the hotspot re-detect, the mouse family,
chat file transfer, all four of §4c-quatervicies, the property-page validators,
the spell checker, the four `sendvw.cpp` divergences and §6's five deltas are
done — §8p, §8q, §8r, §8s, §8t, §8u, §8v, §8w, §8x.  What is left:

- **What a real mapper looks like while its own state changes.**  The engine
  half of this is now measured — 400 redraws accumulate nothing, leak nothing,
  and produce a checksum identical to 5.06's (§8x) — and the geometry renders
  correctly in a still capture (§8p).  What is left is the judgement a person
  makes watching Quow's map while walking, which no assertion here can stand
  in for.

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

### 8x. Redraw over time — the measurable half of the mapper question

§8z's last item was "mapper redraw as a character moves through rooms needs
someone playing".  Whether Quow's map *looks* right while walking is a visual
judgement and still does.  What the **engine** can get wrong under sustained
redrawing is not, and none of it needs a MUD: a surface that is not fully
repainted or an op that leaves residue, and handles leaked per frame.  A mapper
redrawing once per room would meet either within an evening.

`conformance/driver_redraw.lua` draws a deliberately mapper-shaped frame — full
background fill, a street grid, nine room rectangles with outlines and circle
markers, labels in a created font, an image blit, and nine hotspots **deleted
and re-added every frame** — then redraws it 400 times with a moving marker,
and finally redraws frame 1 again.  `WindowGetPixel` samples 2451 pixels into a
fingerprint, so nothing is captured from the screen and nothing depends on
focus.  The harness reads `GetGuiResources` around the loop.

| | this build (Release) | stock 5.06 |
|---|---|---|
| frame-1 checksum | **`AB373FA6`** | **`AB373FA6`** |
| same after 400 redraws | yes, exactly | yes, exactly |
| 400 frames took | 0.37 s | 0.39 s |
| GDI objects | 100 → 102 → 102 | 101 → 103 → 103 |
| USER objects | 101 → 101 | 100 → 100 |
| process handles | 488 → 490 → 490 | 446 → 448 → 448 |
| private bytes | 15 MB throughout | 13 MB throughout |
| hotspots left | 9 | 9 |

Three things worth having from that.

**The two clients draw the identical picture.**  The checksum is computed
in-process from the surface itself, so it is comparable across processes: the
same scene through `pal::ISurface` and through 5.06's direct GDI agrees
pixel-for-pixel over fills, outlines, lines, circles, text in a created font,
and an image blit with transparency.  That is a far richer probe of the surface
refactor than any single op in §4b's list.

**Nothing accumulates and nothing leaks.**  The +2 GDI objects are the window's
own bitmap and the created font, and they are stable; 400 frames × ~40 ops ×
9 hotspot deletes/adds add nothing further, in either build.  Nine hotspots at
the end, not 3600.

**No speed regression.**  0.37 s versus 0.39 s for 400 frames. The first run of
this measured 0.46 s and looked like a 2× slowdown — that was the **Debug**
build against 5.06's Release; the comparison is only meaningful Release to
Release.

What this does **not** cover, and should not be read as covering: whether a
real mapper's geometry stays correct as its own state changes — that is
plugin behaviour, and §8p's still capture plus a human are the only ways to it.

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
| `tests/lua/*.lua` | two new conformance probes | not registered with ctest by design — they are diff-against-a-stock-client scripts, run by hand |
| `platform/wx_outputpane.cpp` | comment only (§8q) | no behaviour |
| `pal/itransport.h` + `doc_shared.cpp` | `lastResolveError()` and the message suffix (§8w) | **shared** — but the new method is non-pure and defaults to 0, so the POSIX transport needs no edit and the mac message is unchanged.  Still a header both sides compile |

The Windows-shared files (`chatsock.cpp`, `doc.cpp`, `doc.h`,
`doc_shared.cpp`, `mcdatetime.cpp`, `methods_accelerators.cpp`,
`methods_output_shared.cpp`) all compile on both, but only Windows had run
them at the time this list was written.

**The macOS run asked for above was done on 2026-08-10**, on the fast-forward
of this whole series into the canonical tree. Clean configure + full rebuild
(`doc.h` moved two includes, so nothing was allowed to link against a stale
object), then `ctest` **58/58** on the default set and **5/5** extended:
everything above builds and passes on macOS with no source change required.
Three things the run corrected, none of them a build failure:

- **The accelerator change (`c24926f`) alters nothing on macOS**, and its macOS
  arm is not untested — this paragraph used to claim both. The erase block was
  *already* the `#else` arm before the hoist, so the port ran it verbatim; the
  commit only adds Windows to it. It is covered here by
  `tests/test_accelerators.cpp` — `EmptySendUnregistersTheKeystroke` and
  `AcceleratorListDropsDeletedKeystrokes` in particular, which pin exactly the
  bookkeeping the hoist gives Windows.
- **`ColourToName` gained the regression test its determinism needs**
  (`tests/test_utilities.cpp`, `ColourToNameIsDeterministicForColoursWithTwoNames`).
  It pins all nine two-named RGB values rather than a sample, so dropping or
  flipping a `kCanonicalColourNames` entry fails; three mutations confirmed it,
  including reverting to first-match-wins. This was the file flagged above as
  "the one to check first", and it was the only shared behaviour change that
  reached macOS output.
- **The max-char-width selftest could not fail.** See §8t-bis below.

**`lastResolveError()` on the POSIX transport — done, but NOT the way the
suggestion reads.** The follow-up above proposes returning `getaddrinfo`'s
`EAI_*` code "to make the port's message as informative as Windows'". Doing
only that would have made it *misinformative*, because the description is
rendered by `CMUSHclientDoc::GetSocketError`, whose non-Windows arm is
`strerror` — and `EAI_*` is a different numbering that merely overlaps errno.
Measured on macOS 2026-08-10:

| code | value | `gai_strerror` (right) | `strerror` (what it would have printed) |
|---|---|---|---|
| `EAI_NONAME` | 8 | nodename nor servname provided, or not known | **Exec format error** |
| `EAI_AGAIN` | 2 | Temporary failure in name resolution | **No such file or directory** |
| `EAI_FAIL` | 4 | Non-recoverable failure in name resolution | **Interrupted system call** |
| `EAI_NODATA` | 7 | No address associated with nodename | **Argument list too long** |

`EAI_NONAME` is the commonest resolver failure there is — a mistyped hostname —
so the naive version would have answered a typo with "Exec format error", which
is worse than the terse message it replaced.

Implemented instead as a sibling to `lastResolveError()`, in the same shape and
with the same non-invasiveness: a **non-pure** `lastResolveErrorString()`
defaulting to empty, meaning "use `GetSocketError`". Windows overrides neither,
so its path and its message are byte-identical — its codes *are* Winsock's, and
that table was written for them. The POSIX transport returns `gai_strerror`.
`getaddrinfo`'s code is carried to the main thread through the existing
completion mailbox rather than written from the resolver thread, since the
member it lands in is read on the main thread.

Pinned in `--pal-transport-selftest` against a reserved `.invalid` name (RFC
2606, so it needs no network): the code is non-zero, the description equals
`gai_strerror`'s, and — the assertion that actually discriminates — it is
demonstrably **not** `strerror`'s text for the same number. A code alone proves
nothing here, precisely because the two numberings overlap.

### §8t-bis — the relaxed font-metric assertion had no teeth

`50a1303` correctly relaxed `platform/miniwindow_selftest.cpp` from
`max == average` to `max >= average`, because the Windows measurement (Courier
New 10pt: average 8, max 10) disproves equality. But `>=` is guaranteed by the
implementation rather than tested by it: `CDC::GetTextMetrics` seeds
`int widest = fm.averageWidth` and only ever raises it, so `max >= average`
holds *however the loop behaves, including if it is deleted* — and the original
defect, `averageWidth * 2`, also satisfies it. The commit that introduced the
measurement therefore left it with no assertion that could detect its absence,
which is the shape `CLAUDE.md` rules out ("design each assertion so that 'the
code did nothing' FAILS").

Fixed by measuring what the two fonts actually report here (2026-08-10):

| face | tmAveCharWidth | tmMaxCharWidth |
|---|---|---|
| Courier New 10 (monospace) | 11 | 11 |
| Times New Roman 12 (proportional) | 11 | 20 |

A monospace face **cannot** discriminate on this platform — the port measures
the widest printable advance, and for a monospace face that *is* the average,
so the two are equal and every relation over them is an identity. The
proportional face can: `max > average` strictly, and it collapses to equality
the moment the measurement stops happening. The selftest now asserts the strict
inequality over a proportional font (checking the pitch bit first, so a font
substitution reports itself instead of silently restoring the identity), and
keeps `< average * 2` over the monospace one to exclude the specific historical
invention. Verified by mutation: neutering the measuring loop leaves both
monospace assertions green and fails only the proportional one.

Consequence for the Windows side: this is a `platform/` file and does not
compile there, so nothing is owed. But the same numbers are worth having —
if real GDI reports Times New Roman with `max == average`, the strict assertion
is wrong on that side and the check should stay macOS-only.

## 9. Round two — everything since the first pass

Round one ended on 2026-08-10.  Since then **93 commits have changed lines that
MSVC compiles**, and none of them has been built or run on Windows.  They come
from three campaigns: a whole-tree audit and its fix waves, the 32-item decision
queue, and a memory-safety sweep under a sanitizer.  Most are fixes to *upstream*
defects in canonical source, so they change Windows behaviour by design.

### 9a. How this list was built, and how far to trust it

Mechanically, so that nothing was missed by memory:

1. Take every commit from the last round-one commit (`b621be9`) to HEAD.
2. Keep those touching a file listed as `<ClCompile>` in `MUSHclient.vcxproj`
   — that, not the file's directory, is what decides whether Windows builds it.
3. For each changed file, walk its `#if`/`#else`/`#endif` structure and keep
   only commits with at least one added line that MSVC actually reaches (i.e.
   not inside an `#ifndef _WIN32` arm).

**That yields candidates, not verdicts.**  Two ways it over-counts, both real
here: a commit that *deletes* a fork whose two arms agreed leaves new
unconditional lines that are Windows-live and Windows-identical (`00f634e`);
and a file can be in the project yet unreachable on Windows because the only
thing that includes it is guarded (`platform/output_types.h`).  Each row below
therefore carries a judgement, and the judgement is the part to argue with.

Rows are marked:

- **RUN** — Windows behaviour moves; the check is stated.
- **COMPILE** — Windows behaviour is unchanged by construction, with the
  reason; only the build matters.
- **§…** — already has its own section in this document; go there.

The rows marked COMPILE are *reasoning*, and round one falsified reasoning five
times in one afternoon (§8a).  If a COMPILE row is cheap to check, check it.

### 9b. Predicted build break — `bmp_load_shared.cpp` defines `LoadImage`

> **CONFIRMED 2026-08-20, exactly as written — §10a.**  MSVC gave `C2373`
> ("redefinition; different type modifiers") and `C2491` ("definition of
> dllimport function not allowed") at `:208`, and the `#ifndef _WIN32` around
> that one function is the fix that was applied.  A **second**, unpredicted
> break sat behind it in `pal/win_transport.cpp`; the generalisation is in
> §10a.

Read out of the tree, **not observed**, and offered in that spirit.  `90fc274`
added `bmp_load_shared.cpp` to `MUSHclient.vcxproj` (`:211`) and the file
carries **no `_WIN32` guard anywhere**.  At `:208` it defines

    HANDLE LoadImage (HINSTANCE hInst, LPCTSTR name, UINT type, int, int, UINT flags)

which is the port's stand-in for the Win32 API of that name — it exists because
`::LoadImage` does not, off Windows.  On Windows `winuser.h` both `#define`s
`LoadImage` to `LoadImageA` *and* declares `LoadImageA` as `WINAPI`
(`__stdcall`).  `bmp_load_shared.cpp` includes `stdafx.h`, so the macro is in
scope: the definition should expand to a `__cdecl` redefinition of `LoadImageA`
and fail to compile.

If that is right, the fix is a `#ifndef _WIN32` around that one function (not
around `LoadBmp`, which is harmless dead code there) — and it is worth
recording as the mirror image of §8a's lesson: **a port-only *definition* in a
file the Windows project compiles is as dangerous as a Windows-needed
*declaration* inside an `#ifndef _WIN32` arm.**  Nothing on macOS can see
either.  The other two files added to the project in the same period
(`doc_aliasmenu_shared.cpp`, `scripting/db_int64.h`) define nothing that
collides.

### 9c. The inventory

#### Output, display and logging

| Commit | What changed | Verdict |
|---|---|---|
| `4639762` | `ProcessPreviousLine`'s note-logging and output-logging blocks (two ~35-line near-clones: preamble/postamble, `%n` substitution, `FormatTime`, HTML-vs-plain dispatch) funnel through one `LogFormattedLine` helper. Claimed behaviour-identical. | **RUN** — log a session from a world with a note preamble/postamble containing `%n`, in both HTML and plain modes, and diff the file against 5.06's byte for byte. A dedupe of two hand-synced clones is exactly where a silent divergence hides. |
| `6491c97` | `strnrchr` ignored its length bound (`while (*s && n < count++)` never terminates on `n`), so it scanned to the first NUL. Both callers pass a `CLine::text` that is exactly `len` bytes **with no terminator**. | **RUN** — a long line with no space, in MXP `<P>` mode. Windows carried the same over-read; the visible symptom would be a wrong reflow point or a crash on a line ending at a page boundary. |
| `cb903af` | `world.WriteLog`'s trailing-newline guard compared two characters against a one-character literal, so it never matched and every write appended a newline. `UPSTREAM-DIVERGENCES.md` §10. | **RUN** — `world.WriteLog("x\n")` must now put one newline in the log, not two. Any script that relied on the doubling changes. |
| `8dd87e5`, `6e67cb0` | Status-bar text is now attributed to the world that produced it rather than to the frontmost one — "Connecting to…", the server-closed notice, the queued-command count, `DrawMappingStatusLine`'s "Mapper: …" and `CheckTimers`' "Reconnecting …". | **RUN** — on Windows these all go to the single app-scoped main-frame bar. Confirm the same strings still land there, in the same order, with two worlds open and only one connecting. |
| `88b26eb`, `c511148`, `ee3586a` | Output fade, the nine `timestamp_*` options, and an MXP link with a legacy-charset byte — all in `platform/output_types.h`. | **COMPILE** — the header is a `ClInclude` but `doc.h` includes it only off Windows, so nothing on Windows reaches it. |
| `0656d24`, `aad9e22` | Bold reaching white; tab stops in a UTF-8 world. | §9, *Engine-core upstream bugs* below. |

#### Telnet, MCCP and hostile input

| Commit | What changed | Verdict |
|---|---|---|
| `759d778` | Three security defects in the shared MCCP path, all upstream: an `FDICT` zlib header makes `inflate` return `Z_NEED_DICT`, which consumes nothing and produces nothing, and the loop only tested `< 0` — an endless spin; plus two more. | **RUN** — highest priority in this table. Point the client at a server that sends an MCCP stream with `FDICT` set and confirm it disconnects rather than freezing. Then a normal MCCP world (Discworld) to confirm nothing ordinary regressed. |
| `71c6afd` | `Phase_SUBNEGOTIATION` appended every byte with no bound, so `IAC SB <type>` never followed by `IAC SE` grew a string forever — memory exhaustion, and on the 32-bit Windows build a `terminate()` at ~2 GB. | **RUN** — a stub server that opens a subnegotiation and never closes it. Confirm the client abandons it. Confirm ordinary MCCP/NAWS/TTYPE negotiation is untouched. |
| `f878ff5` | `SendWindowSizes` wrote the four NAWS payload bytes raw; RFC 854 requires a data 255 to be doubled, so a width or height with a 255 byte emitted a bare `IAC` mid-subnegotiation. | **RUN** — set the world so a NAWS dimension is exactly 255 (a wrap column of 255 is an ordinary value) and confirm the MUD's echo of the size is right and the session stays in step. 256 was always fine, which is why only that one value tripped it. |
| `5ee518e`, `0dae192` | Six MXP early-return paths leaked their `CArgumentList` — including a stack-local list that nothing ever reclaimed, reachable from any server. | **RUN** (leak only) — feed malformed MXP tags in a loop and watch the process's private bytes. |
| `00f634e` | Deleted the `#ifdef _WIN32` fork on the TTYPE reply and the MTTS bitvector. | **COMPILE** — the *Windows* arm is the one that survived: it already answered `ANSI` and `MTTS 265`. Cheap to confirm with a TTYPE-logging stub. **Confirmed §10c — but stock 5.06 answers `MTTS 9`, from upstream `d9d45b3` (2025-02-15), which post-dates the 5.06 binary. Do not file that difference.** |

#### Triggers, aliases and timers

| Commit | What changed | Verdict |
|---|---|---|
| `856fa00` | Three upstream logic errors: `GetTrigger` OR'd `eOmitFromOutput` for **both** `bOmitFromOutput` and `omit_from_log`; `GetUdpPort` mishandled single-port ranges; two `InfoTypes` get/lose-focus codes were wrong. | **RUN** — the first is the one that bites: `AddTriggerEx` a trigger with `omit_from_log`, read it back with `GetTrigger`, and confirm the round trip no longer converts it to `omit_from_output`. |
| `30df30b` | `ExecuteAliasMenuItem`'s `%`-substitution computed the `%<digit>` subscript and never read it, so a menu alias always sent `%1` empty. `UPSTREAM-BUGS.md` #14 — upstream's, sitting in `mushview.cpp` before the hoist. | **RUN** — an alias flagged *menu* whose Send is `say %1`, fired from the Ctrl+click word menu and from an MXP action menu. It must send the clicked word. |
| `0a8d1a4` | `CmcDateTime::operator-`'s **`_WIN32` arm** returned a null/invalid span as a bare `double`, which the return type reconstructed as unconditionally valid. | **RUN** — a script subtracting two dates where one is null/invalid must now see the span report invalid. This is one of very few commits whose changed code is Windows-only. |
| `a53232d` | Unlabelled timers report their internal `*timerN` name to scripts and to `Debug("timerlist")`; the port's `SortTimers` was a no-op stub. | **COMPILE** — Windows always built the reverse map. Confirm `*timerN` naming and the Debug listing are unchanged. |
| `a098a70` | Reset All Timers walks `m_PluginList` and resets each enabled plugin's timers; the port had substituted a one-liner that reset only the world map. | **COMPILE** — the canonical body is what Windows already ran. Worth confirming a plugin heartbeat really is reset. |
| `1ad3349`, `ef5466f`, `d2f67e3` | Fractional `at_time`; the colour-trigger matcher. | §9 *Engine-core…* / §4c-quintricies. |

#### Miniwindows and images

| Commit | What changed | Verdict |
|---|---|---|
| `1e03119` | Four upstream image defects that produced wrong output while returning `eOK`: greyscale and palettised PNGs decoded as coloured noise a third of the width, saved `.bmp` files were malformed, and two blend modes were wrong. | **RUN** — `world.WindowLoadImage` on an 8-bit greyscale PNG and on a palettised one; `WindowWrite` a `.bmp` and reopen it in Paint; the two blend modes against 5.06 side by side. |
| `ab0336d`, `70ad82e` | A miniwindow no layout pass has placed reads as occupying the pane's top-left corner and swallowed clicks meant for the text there; and `m_bLaidOut` was set before the "not if hidden" skip, so a hidden-then-shown window hit-tested at that corner. `UPSTREAM-DIVERGENCES.md` §9. | **RUN** — create a miniwindow and add a hotspot before any paint; clicks at the pane's top-left must reach the text. Then `WindowShow` a hidden window and confirm its hotspots respond where it is drawn. |
| `6caf24a` | `WindowAddHotspot`'s re-entrancy guard was a function-local `static`, so it was one flag per **process** rather than per document. | **RUN** — two worlds open; a hotspot `MouseOver` in world A that adds a hotspot to world B. It must now succeed. |
| `ca2fba1` | `SetBackgroundImage` and `SetForegroundImage` were byte-identical 66-line bodies; both now call one `SetFrameImage`. | **RUN** (identity) — both still load PNG and BMP, both still report the same error codes for a missing and an unreadable file. |
| `f807984` | Four bitmap-lifecycle defects in the port's `CBitmap`; `png_load_shared.cpp` gained 10 Windows-compiled lines. | **COMPILE**, believed — the ownership token is the port's shim. Confirm the PNG loader still works. |
| `47896a0` | `WindowAddHotspot` skips its re-detect while the mouse is captured; the guard asked `::GetCapture()` directly, a permanent null stub off Windows. | **COMPILE** — `::GetCapture()` is real on Windows, so the guard already worked there; the change routes it through a seam. |
| `90fc274` | A shared `.bmp` decoder, added to the project. | **See §9b — predicted build break.** |
| `e288d7e`, `1d45c28` | Hotspot self-deletion; cross-window hotspot use-after-free. | §9 *Two miniwindow-layout corrections* / §4c-quadragies. |

#### Scripting API and the Lua binding

| Commit | What changed | Verdict |
|---|---|---|
| `3f6e164` | Five upstream defects in `scripting/lua_methods.cpp` (upstream bugs 36–40). `world.ArrayImport` checked that the *key* was a string then built the value with `lua_tostring` regardless of type — NULL for a boolean, table, function or nil, and a `CString` constructed from NULL. | **RUN** — `world.ArrayImport` with a table holding a boolean value must fail cleanly rather than crash. Lua only; the other four are in the same file. |
| `8d7ab33` | `ArrayExport` escapes a delimiter after a doubled backslash; `ArrayImport`'s un-escape pass matched the wrong half of that pair and ate the real separator. One such field fails the import, two corrupt it silently. | **RUN** — export and re-import an array whose value ends in a literal backslash. |
| `3f3e8ac` | The one-argument `CPlugin::ExecutePluginScript` returned an **uninitialised** local as the plugin's veto, and `ExecuteLua` returns from its "function not found" and error arms without writing it. | **RUN** — a plugin whose `OnPluginSend` raises a run-time error. Whether the line was sent, displayed or suppressed was previously whatever the stack held. |
| `d4112c4` | `world.GetRecentLines` with a `Count` larger than the buffer wrapped an unsigned subtraction into a positive `int`, so the `< 0` clamp never fired and it read past the buffer. | **RUN** — `world.GetRecentLines(3000000000)` with three lines held. |
| `89ee850` | Both `ExecuteLua` overloads saved `m_iNoteStyle`, forced NORMAL, then returned from the "function not found" arm without restoring it. Also restores the script-error log's context block. | **RUN** — a Lua callback naming a routine that does not exist, from inside a handler that had set a note style. |
| `2e54bba` | `world.Help`'s fallback bound `bLua` to the *second* parameter (`nStartChar`, an `int`) instead of the fourth, so the Function List never showed Lua's functions and the bogus offset blanked the filter. `CMainFrame::OnGameFunctionslist` had the identical mistake. | **RUN** — `world.Help` and Game ▸ Function List on a Lua world must list Lua's own functions. |
| `4a0de8f` | The Lua `error_code`/`error_desc` tables keyed `ePluginCouldNotSaveState` on 30037 instead of its own 30038, so `error_desc[30038]` was nil. | **RUN** — provoke a failed `world.SaveState` and confirm a script can name the error. |
| `fbc4161` | `catch (CArchiveException*)` had no parameter name, so `e->Delete()` could never be called. | **RUN** (leak only) — a malformed `world.ImportXML` in a loop. |
| `5c53c3e` | `world.Debug("summary")`'s OS name moved out of `world_debug.cpp` into `OSVersionString()` in `utilities_shared.cpp`. | **RUN** (identity) — the OS line in the Debug summary must be the same string as before on the same machine. |
| `4874426`, `69155bd`, `1537f41` | `world.Debug` listed 0 commands always (it walked for a `CSendView`); `/triggerlist` and friends printed a bare `"..."` at a narrow wrap column; and the truncation clamped its compare width and cut width independently, so a value came back *wider* than its budget. | **RUN** — `world.Debug("commands")` with history, and the four list commands at a wrap column of 30 and of 200. |
| `fc77994` | `GUID::Data1` was `unsigned long` and formatted `%08lX` — a minimum width — so `world.CreateGUID` returned 44 characters on LP64. The member is now a fixed 32-bit type. Also a metaphone stack overflow. | **RUN** — the member's *declared type* changed, which Windows compiles. `world.CreateGUID` must still be 36 characters and the same shape as 5.06's. |
| `daf8a95`, `c05001a` | `Get_XML_number`'s magnitude guard and Insert Unicode Character's range check both used the native `LONG_MAX`. | **COMPILE** — `LONG` is already 32 bits on Windows, so both guards are unchanged there. The XML one is worth a look anyway: it now uses a literal, and it decides which world files load. |
| `47af881`, `33ee87d`, `bb7fa0a` | `world.SpellCheckCommand`, `utils.directorypicker`, `utils.menufontsize` — each dead on the port because a Win32 API is a stub there. | **COMPILE** — all three take the real API on Windows and are unchanged; the edits are in the port's arm. |
| `203c0fe`, `092c1f4`, `43e27b8`, `648622c`, `e24ab4b`, `fbb6695` | The Lua binding trio; the SQLite widening. | §4c-sextricies, §4c-septentricies, §4c-sexquadragies. |

#### Plugins

| Commit | What changed | Verdict |
|---|---|---|
| `28ca804` | `Screendraw` and `PluginListChanged` guarded recursion with a function-local `static`, so the flag was one per **process**. `Screendraw` runs for every completed line, note and command — so one world's callbacks silenced another's. | **RUN** — two worlds, both with a plugin implementing `OnPluginScreendraw`, both receiving output. Both must be called. This is the most user-visible plugin change in the list. |
| `43c4545` | In the plugin wizard's four "remove the exported items" loops, the `SortTriggers()` + `SetModifiedFlag()` block sat **inside** the loop, so a full sort ran once per surviving entry. | **RUN** — run the wizard on a world with a few hundred triggers and confirm it finishes promptly and the world is still marked modified exactly once. |
| `aca2478` | `OnUpdateNeedDescription` read the selected row's `CPlugin*` off the list control with no liveness check, unlike its five sibling handlers. | **RUN** — open the Plugins dialog, remove a plugin from outside it, then hover Show Info. |
| `66e6606` | The plugin language whitelist now gates `jscript`/`perlscript`/`python` on the engine being present, as it already did for Tcl and Ruby. | **COMPILE** — every new arm is `defined(_WIN32) \|\| …`, so Windows accepts exactly what it accepted before. Confirm a VBScript and a JScript plugin still load. |
| `b5d85fb` | Four `#ifdef USE_PYTHON` / `USE_TCL` / `USE_RUBY` gates became `#if defined(USE_X) \|\| !defined(_WIN32)`. | **COMPILE** — the Windows condition is unchanged by construction. |

#### Dialogs and preferences

| Commit | What changed | Verdict |
|---|---|---|
| `44f0737` | **Eight** defects in the world- and global-preferences pages, all the same shape: a value that round-trips through the dialog but is compared, applied or seeded against the wrong thing — so edits were discarded, or changes invented. 79 lines in `prefspropertypages.cpp` alone. | **MOSTLY DONE (§10o)** — four of its five families are now unit tests on Windows (`tests/win/test_prefs_pages.cpp`), three of them revert-checked (the other two are weaker and say so at their sites). What is left needs the GUI: the printer-font picker (modal `CFontDialog`), the two `CListCtrl` column items, and the end-to-end press-OK confirmation this row originally asked for. |
| `c605461` | Seven file dialogs tested `App.platform` to decide whether to suggest a filename; off Windows it was never assigned. Also a long-name overrun guard. | **RUN** in part — `App.platform` **is** assigned on Windows (`MUSHclient.cpp:940`), so the suggestion behaviour is unchanged there; what is new on Windows is the bound on a long name. Try File ▸ Log Session with a 300-character world name. |
| `283124f` | Eight `catch (CMemoryException*)` arms could not match the `std::bad_alloc` a non-MFC `operator new` throws. | **COMPILE**, believed — MFC's `operator new` still throws `CMemoryException*` on Windows and those arms still match. Confirm nothing in the new `catch` ordering shadows the MFC one. |
| `9139076` | `ReadNames` accepted a names file whose section markers are adjacent, leaving an empty array; `GenerateName` then did `rand () % 0`. | **RUN** — a names file with an empty `[middle]` section must be refused, not divide by zero. |
| `3c58886` | Inside `CMUSHclientDoc`, unqualified `GetUniqueID()` resolved to the automation member, which returns an `AllocSysString`'d BSTR the caller owns — leaked once per document since the 2010 import. | **RUN** (leak only) — open and close a hundred worlds. Same string either way. |
| `c3936f4` | `m_iNoteColourFore`/`Back` were never initialised, yet `Tell`'s deferred arm (taken inside a telnet negotiation or a plugin callback) pushes them into `m_OutstandingLines`. | **RUN** — a plugin calling `world.Tell` from `OnPluginTelnetRequest`; the note must come out in real colours. |
| `0b70de7`, `41eb7bd`, `c1a3990` | A compiled regexp leaked on every regexp Recall; a failed `sqlite3_open_v2` leaked the handle it had already been given; a rejected duplicate-label trigger/alias left a freed pointer in the map. | **RUN** — the third is the one with teeth: load a world file with two triggers sharing a label and confirm nothing later dereferences the rejected one. |
| `0e4a775` | `CDocument::SaveModified`/`DoFileSave` were `return TRUE` in the port's shim; six canonical dialog TUs each swapped a raw `SendMessage(WM_SETFONT/EM_LIMITTEXT)` for the MFC wrapper (`SetFont`, `LimitText`); `winplace.cpp`'s body was wrapped in `#ifdef _WIN32`. | **COMPILE** — the wrappers are what MFC does anyway and `winplace.cpp`'s Windows arm is the original. The behavioural half is port-only. |
| `e14c216`, `c13d73a` | Tip of the Day's stack smash; six world-prefs and mapper defects. | §9 *World-prefs dialog bugs* below. |

#### Chat

| Commit | What changed | Verdict |
|---|---|---|
| `364ecd0` | `GetStamp` rebuilt the 4-byte wire stamp through `int` arithmetic, so a top-bit-set stamp sign-extended on LP64 and a genuine self-collision compared unequal about half the time. | **COMPILE** — `long` is 32 bits on Windows, so the sign extension never happened there. |
| `52db0b6` | `ChatNote`'s truncation loop read `p[1]` after `ReleaseBuffer(-1)`. | **COMPILE** — on Windows `p` targets the string's real buffer, which survives the release. |

#### Command bar, accelerators and sound

| Commit | What changed | Verdict |
|---|---|---|
| `eb8c2fe` | **A regression round one introduced.** `c24926f` (§8j) hoisted the accelerator-map erase to both platforms; the erase destroys the entry a later re-bind of the same key looks up, so every unbind/rebind cycle consumed one of the 1000 command ids permanently, and at exhaustion `AcceleratorTo` answered `eBadParameter` for **every** key in that world. Fixed with a free list. | **RUN** — bind and un-bind the same key 1200 times through `AcceleratorTo`, then bind a fresh key. It must still work. A plugin that binds in `OnPluginInstall` and clears in `OnPluginClose` reaches this by being reloaded enough times. |
| `5359254` | `CSendView::OnMacroCommand`'s 52-case `switch` mapping macro command ids to slots moved into `utilities_shared.cpp`; `sendvw.cpp` now calls it. | **RUN** — F2…F12 and the Ctrl/Shift variants must still fire the right macro, from the key, from a menu item, and from `world.DoCommand("MacroF1")`. |
| `bc4d149` | Display ▸ Stop Sound Playing stopped two unrelated things at once (the world's MCI voice and the script's ten buffers); each half gained a command of its own, in `doc.cpp` and `accelerators.cpp`. | **RUN** — new commands in canonical source. Confirm the original combined command still stops both, that the two new ones each stop only their half, and that the menu and accelerator tables are intact. |
| `8468a76` | `ApplyAutoSay` went from `bool` to a tri-state, because "auto-say consumed it" and "auto-say refused it, nothing was sent" must be told apart — a command that was never sent does not belong in the history. Six more command-bar defects alongside. | **RUN** — with auto-say on and the world **disconnected**, type a line: nothing is sent, and it must not enter the command history. Windows' `SendCommand` exits before its history tail in that case, which is the behaviour being matched. |
| `f198bea` | The port's ten sound buffers were process-global; on Windows they are a `CMUSHclientDoc` member. | **COMPILE** — Windows was already per-world. Confirm `world.StopSound(0)` in one world leaves another's sounds playing. |
| `353f7e8` | `world.SetInputFont` unforked. | §5. |

#### Comments and internal-only

`e88ddaf`, `3547525`, `9d0462e` correct stale in-tree comments (`genprint.cpp`,
`platform/gdi_headless.cpp`, `chatsock.cpp`, `scripting/db_int64.h`).  Nothing
to check beyond the build.

### 9d. The dated round-two sections

What follows are the round-two changes that were written up individually when
they landed.  They are the detailed half of the table above; the 2026-08-19
decision-campaign entries live in §4 instead, from §4c-quintricies to
§4c-undequinquagies.

### The Ctrl+click alias-menu hoist (doc_aliasmenu_shared.cpp, 2026-08-13)

CMUSHView::AliasMenu's word scan and menu-list build, and OnMXPMenu's
ACTION_ALIAS arm, moved verbatim into three CMUSHclientDoc methods
(SetWordUnderMenu / BuildAliasMenuItems / ExecuteAliasMenuItem) so the wx
frontend can share them; mushview.cpp now delegates.  Meant to be
behaviour-preserving on Windows, with three deliberate near-identities to
check in a Windows run:

- The list is sorted by std::sort, not qsort.  The original qsort swaps
  elements by raw bytes — fine on MFC's pointer-sized CString, corrupting on
  the port's shim (this bit during the hoist, headless).  Ordering is the
  same comparator; both sorts are unstable, so equal labels may tie-break
  differently than before.
- BuildAliasMenuItems sorts unconditionally; the view previously sorted only
  in its display branch.  Windows' display loop now runs over the pre-sorted
  array (its own qsort was removed as a duplicate).
- Ctrl+click in the output/command bar, an alias flagged "menu" firing from
  the popup (world and plugin arms), and GetInfo(86) after the popup are the
  behaviours to smoke-test.

### Three mushview.cpp corrections nothing here can execute (2026-08-18)

All three are in `CMUSHView`, which the port never constructs, so the macOS
build type-checks them and nothing more.  A Windows run is the only way to
see any of them work, and the third is not even confirmed to be a bug.

- **`OnMXPMenu`'s ACTION_HYPERLINK arm now allows `https://`.**  The
  left-click arm (`mushview.cpp` ~2221) and `OpenBrowser`
  (`methods_utilities.cpp:465`) both allow http/https/mailto; the popup-menu
  arm allowed only http and mailto, so an MXP hyperlink opened from the
  right-click menu was refused with "permission denied" while left-clicking
  the very same link opened it.  **To check:** a world sending an `<a
  href="https://...">` link with a `<send>`-style menu — open it from the
  right-click popup, not by clicking, and confirm the browser opens.  The
  three copies of the allowlist are still three copies; hoisting them into
  one helper in `utilities_shared.cpp` is a follow-up.

- **The double-click word scan no longer reads past the line.**  A completed
  line's `text` buffer is reallocated to exactly `MAX (len, 1)` bytes with no
  terminator (`doc_shared.cpp:3637`), and `calculate_line_and_column` returns
  `col == pLine->len` for a click to the right of the text, so
  `OnLButtonDblClk`'s backward scan read `text [len]` — one byte past the
  allocation.  Whether a word came back at all depended on that byte: a
  space or delimiter there produced no selection, anything else selected the
  last word.  Now clamped the way the hoisted twin already clamps
  (`doc_aliasmenu_shared.cpp` `SetWordUnderMenu`), so it deterministically
  selects the last word.  `OnDisplayTextattributes` had the same shape — a
  selection spanning lines can leave `startcol == pStartLine->len` and the
  character read below it is `text [startcol]` — clamped after the
  valid-selection test so which selections are accepted is unchanged.
  **To check:** double-click well to the right of a short line (expect the
  last word selected, every time), and run Display ▸ Text attributes on a
  selection that starts at the end of one line and ends on a later one.

- **`m_ScrollbarPosition` is written through `.cy` now — and this one is a
  question, not a fix.**  It is a `CSize`, and the three writers assigned a
  plain `int` to it.  Real MFC's `CSize` has a `CSize(DWORD)` constructor
  that splits the value `LOWORD`/`HIWORD`, which would put the scroll
  position's *high* word in `.cy` — the only field ever read
  (`mushview.cpp:3255`, `m_bAtBufferEnd`) — making it 0 for any position
  under 65536 and the "at buffer end" test wrong.  The port's shim declares
  an explicit `operator= (int)` that sets `.cy`, so the port never had the
  problem and cannot demonstrate it either way.  The writes are now
  unambiguous on both platforms.  **To check:** whether the pause/freeze
  status line was ever wrong on Windows before this — if `CSize(DWORD)` does
  not participate here (e.g. the assignment was ambiguous or resolved
  elsewhere), this was a no-op and the change is merely clearer.

### Two miniwindow-layout corrections in shared source (2026-08-18)

Both are in `miniwindow_compose.cpp`, which is `CMUSHView`'s old
`Mouse_Over_Miniwindow` / `Calculate_MiniWindow_Rectangles` hoisted verbatim
and is compiled into the Windows build too (`MUSHclient.vcxproj:316`).  Both
have headless coverage in `tests/test_lua_surface.cpp`, so what a Windows run
adds is confirmation against the real view and a real plugin.

- **A MouseOver callback may now delete its own hotspot**
  (`docs/UPSTREAM-BUGS.md` #20).  The re-lookup that sets `pHotspot = NULL`
  when the callback deleted the hotspot was immediately followed by
  `pHotspot->m_sCancelMouseOver`, so the client died.  **To check:** a plugin
  whose hotspot MouseOver handler calls `WindowDeleteHotspot` (or
  `WindowDeleteAllHotspots`) on itself — a self-redrawing button bar is the
  natural shape — then mouse onto that hotspot.  Stock 5.06 should crash where
  this build does not.  Also confirm the *ordinary* case still arms the
  mouse-leave: a hotspot with a non-empty `CancelMouseOver` that survives its
  MouseOver must still fire CancelMouseOver when the pointer leaves the
  miniwindow entirely (that is `TrackMouseEvent`, which only Windows really
  runs).

- **`m_bTemporarilyHide` survives the other layout pass**
  (`docs/UPSTREAM-BUGS.md` #21).  The two passes run back to back and the flag
  was reset for every window in both, so `WindowInfo (name, 6)` always answered
  false for a draw-underneath window that had no room.  **To check:** a
  `MINIWINDOW_DRAW_UNDERNEATH` window at position 5 (centre top) wider than the
  output pane — `WindowInfo (name, 6)` should now report true, and narrowing /
  widening the world window should flip it both ways.  Worth watching that the
  paint is unaffected: the flag is read by the same loop that sets it, so a
  draw-underneath window that fits must still be painted.

The hit-test half of the same commit is a deliberate divergence rather than a
bug fix and is written up as `docs/UPSTREAM-DIVERGENCES.md` #9; on Windows its
only visible effect should be that a wall-placed miniwindow's hotspots respond
at all (upstream's `PtInRect` could not match a `CRect (start, 0, width,
height)`), and that a window the layout could not fit stops answering at the
pane's top-left corner — which upstream's `PtInRect` also never did.

### Engine-core upstream bugs in shared source (2026-08-19)

Each is unconditional shared source with no `#ifdef` anywhere near it, and each
file is in `MUSHclient.vcxproj`, so Windows carries the bug and the fix changes
Windows behaviour.  Each has headless coverage, and each new test was confirmed
to fail with its fix reverted, so what a Windows run adds is confirmation
against the real view, the real timer wheel and a real MUD.

- **Bold now reaches white** (`docs/UPSTREAM-BUGS.md` #56).  **To check:** a
  world with *Use custom colour 16 as default output colour* ticked, then send
  it `ESC[37m` (white) followed by `ESC[1m`.  The text should brighten to the
  *bold* white swatch and go back to the normal one on `ESC[22m`; stock 5.06
  leaves it at the normal swatch through both.  Set the two white entries to
  obviously different colours on the world's ANSI-colour page first, or the
  change is invisible.  Confirm the other seven colours are unchanged — they
  already worked, and the loop bound is the only thing that moved.

- **Fractional `at_time` timers** (`docs/UPSTREAM-BUGS.md` #57).  **To check:**
  a timer set to *at time* with a second field of, say, 30.75, against the
  world clock.  It should fire three quarters of a second later than stock
  5.06 does — i.e. at the time configured, not at the whole second below it.
  Worth re-running the ordinary *every* timers and `DoAfter` alongside: they go
  through `CmcDateTimeSpan`, not this constructor, and must be unaffected.  The
  range the dialog accepts must also be unchanged — 59.9999 still valid, 60
  still rejected.

- **Tab stops in a UTF-8 world** (`docs/UPSTREAM-BUGS.md` #58).  **To check:**
  a UTF-8 world (*Use UTF-8* on the world's output page), then have the MUD
  send a line of accented or CJK text followed by a tab and a marker
  character — a two-column table is the natural shape.  The marker should line
  up with the same column an all-ASCII line puts it in; stock 5.06 pulls the
  multi-byte row left.  Also worth a look: an ASCII world must be
  byte-identical (the helper returns the byte length when `m_bUTF_8` is off),
  and the `<P>` period-space rule, which shared the same confusion, should
  still give two spaces after a full stop well inside the wrap column.

### World-prefs dialog bugs in canonical source (2026-08-19)

Five upstream defects in `dialogs/world_prefs/configuration.cpp`,
`dialogs/world_prefs/prefspropertypages.cpp`, `dialogs/MapDlg.cpp` and
`dialogs/TipDlg.cpp` — all four in `MUSHclient.vcxproj`, none behind a platform
ifdef, so Windows carries every one.  All five are covered by the
`--gui-bridge-selftest` harness or `tests/test_utilities.cpp`, and every new
assertion was confirmed to fail with its fix reverted; what a Windows run adds
is the real property sheet and the real common dialogs.

- **"Use SSL" now dirties the world** (`docs/UPSTREAM-BUGS.md` #63).
  **To check:** open a saved world, tick *Use SSL* on World Properties ▸
  Connecting, change nothing else, press OK, then close the world.  It must
  prompt to save (or auto-save); reopening it must still have the box ticked.
  Stock 5.06 closes without a prompt and the box is clear again.  Confirm the
  nine fields that were already compared still behave — change only the port,
  and only the proxy password, and check each still marks the world modified.

- **Regexp recompile keeps each trigger's own pattern**
  (`docs/UPSTREAM-BUGS.md` #64).  **To check:** a world with several triggers,
  one of them deliberately broken so its compile fails at load (an unmatched
  `(` in a regular-expression trigger, which reports an error and leaves the
  entry with no compiled expression).  Then toggle *Use UTF-8* on the world's
  output page, which is what calls `RecompileRegularExpressions`.  Afterwards
  every working trigger must still match only its own text; on stock 5.06 the
  broken one starts matching whatever the previous trigger matches, or — if it
  is first in the map — every line that arrives.

- **The beep-sound browser bounds its path** (`docs/UPSTREAM-BUGS.md` #50, now
  eight sites).  **To check:** `world.SetOption ("beep_sound", <a path of 300+
  characters>)`, then World Properties ▸ Output ▸ the sound *Browse* button.
  It must open the file dialog normally; stock 5.06 overruns a 261-byte buffer
  there.  A normal path must still pre-fill the dialog with that name.

- **Tip of the Day from a long install path** (`docs/UPSTREAM-BUGS.md` #65).
  **To check:** install into a directory whose full path is 240+ characters and
  open Help ▸ Tip of the Day (and let the startup tip run).  It must show a tip
  or the "file absent" message; stock 5.06 smashes the stack.  Confirm the
  ordinary install still finds `tips.txt` beside the executable — that is the
  `"\\tips.txt"` arm, which moved with the rest.

- **The mapper's "make alias" keeps every field**
  (`docs/UPSTREAM-BUGS.md` #66).  **To check:** Game ▸ Mapper, then *Make
  alias*; in the alias dialog set *one shot*, *echo alias*, *temporary*, a
  variable name, *keep evaluating*, *omit from command history* and a sequence
  other than 100.  After OK, open the new alias from World Properties ▸ Aliases
  and confirm all seven came through; stock 5.06 shows the defaults.  Also
  confirm the sequence the dialog OPENS on is 100, not 0.

- **Paste/send delays accept the option-table range**
  (`docs/UPSTREAM-BUGS.md` #67).  **To check:** `world.SetOption
  ("paste_delay", 50000)` (and `send_file_delay`), then open World Properties ▸
  Paste and press OK.  The sheet must close; stock 5.06 puts up "Please enter an
  integer between 0 and 10000." and refuses.  Confirm 100001 is still refused
  and that the neighbouring *delay per lines* field (1..100000) is unchanged.

### Frame-routing wave — shared source (2026-09-01)

Port-side command-routing work whose engine half is compiled by MSVC.  Each is
a textual change to a line Windows executes; each is meant to leave Windows
behaviour bit-identical, and each is unrunnable here because the class that
would prove it (`CMUSHView` / `CSendView`) is never constructed on the port.

- **The trigger sound's "only if inactive" test now goes through
  `IsWorldViewActive()`** (`ProcessPreviousLine.cpp:1218`).  The raw
  `m_pActiveCommandView == NULL && m_pActiveOutputView == NULL` pair was
  vacuously true off Windows, so the option suppressed nothing there; on
  Windows `IsWorldViewActive()` is *defined* as that pair (`doc.h:1214`), so
  the new expression is its De Morgan equal.  **To check:** a trigger with a
  sound file and *Only if inactive* ticked, on a world whose window is
  frontmost — no sound; click another world (or another app) and make the same
  line arrive — the sound plays.  Also confirm a trigger with the box
  **un**ticked still sounds in both states, and that Game ▸ *enable trigger
  sounds* off still silences both.  Nothing else reads those two pointers on
  this path, so the only way to see a regression is the sound itself.

- **`CSendView::OnKeypadCommand`'s id → slot switch is hoisted**
  (`utilities_shared.cpp` `KeypadCommandIdToSlot` / `KeypadSlotDigit`;
  `sendvw.cpp:1053` now calls both).  Same shape as `MacroCommandIdToSlot`
  before it, and for the same reason — the port has no `CSendView`, so
  `world.DoCommand ("Keypad1")` reached nothing.  The view's body is otherwise
  untouched: the auto-say save/restore, the `m_keypad_enable` branch, the
  `SendCommand` call and the `ReplaceSel` else-arm all stay where they were.
  **To check:** with *Enable keypad* on and the Keypad page configured, press
  each of the fifteen numpad keys and the fifteen Ctrl+numpad ones — every one
  must send exactly the slot it did before.  Then turn *Enable keypad* off and
  press them again: each must TYPE its own character into the command bar at
  the caret (`0`-`9`, `.`, `/`, `*`, `-`, `+`), which is the arm that reads
  `sValues[]`.  Finally `world.DoCommand ("Keypad1")` and
  `("CtrlKeypadPlus")` from the Immediate window, which is the route that was
  broken on the port and has always worked on Windows.

- **The colour swatch's right-click menu forks off Windows**
  (`dialogs/world_prefs/ButtonColour.cpp`, `docs/UPSTREAM-DIVERGENCES.md` 23).
  The `#ifdef _WIN32` arm is the original code verbatim; the only thing that
  moved on the Windows side is `bool bEnable = GetClipboardColour (...)`, which
  now runs a few lines earlier, before `LoadMenu` rather than after it (nothing
  between them touches the clipboard or the menu).  `ButtonColour.h` gains one
  inline method inside `#ifndef _WIN32`.  **To check:** right-click a colour
  swatch on World Properties ▸ Custom Colours.  The two-item *Copy* / *Paste*
  menu must appear at the pointer exactly as before, *Paste* greyed until a
  colour has been copied, and both items must work.  Also try the keyboard
  context-menu key on a focused swatch — that is the `(-1, -1)` arm, and it is
  the same code on both platforms.

---

## 10. Round two — results of the second empirical pass (2026-08-20)

> This section is the **record of what was measured** on the second Windows
> pass, and it supersedes any §9 *prediction* it contradicts.  It does not
> finish round two: §10h lists what is still owed, and it is most of §4's
> dialog-driven half.

**Environment.**  Windows 11 Pro 26200, Visual Studio **Build Tools** 2022
17.14 (MSVC 14.44.35207), Win32/MBCS, OpenSSL under `openssl\`.  Baseline: the
stock **5.06** installer copy at `C:\Program Files (x86)\MUSHclient` (compiled
Mar 29 2019).  New build: `WinDebug\MUSHclient.exe`, reports **5.07**.  Both
configurations were built and both were exercised (§10b).

**Method.**  The same differential discipline §3 sets out: one Lua file, run in
*both* clients, printing `key = value` lines, so "is this a regression?" is a
diff.  Two new probe scripts were written alongside the two the repo already
had, plus a scripted loopback MUD for the protocol work.  **The MUD stub is now
committed** — `tests/mud_stub.py`, Python 3, stdlib only — because this was the
third time one had been written from scratch (`docs/GUI-DRIVE-PLAN.md` §3.1
used to carry an inline copy; that section now points at the committed file).

**A note on what this pass is for.**  Driving the real client found three
things no headless test could have (§10e, §10f), which is what end-to-end
testing is good at.  It is *not* the regression net: everything it found that a
unit test could pin is called out below with the test that should pin it.

### 10a. Build — broken at HEAD again, in two places

**Round two did not compile**, and the two failures are the same shape §8a
warned about: something the macOS build structurally cannot see.

| Site | Error | Predicted? |
|---|---|---|
| `bmp_load_shared.cpp:208` | `C2373` + `C2491` — `LoadImageA` redefinition | **Yes**, §9b, exactly as written |
| `pal/win_transport.cpp:407` | `C2065` ×2 then `C1903` — `COMPRESS_BUFFER_LENGTH` undeclared | No |

**1. `bmp_load_shared.cpp`.**  §9b called this precisely: `winuser.h` both
`#define`s `LoadImage` to `LoadImageA` and declares it `WINAPI` (`__stdcall`)
dllimport, so the port's stand-in expands to a `__cdecl` redefinition of a
dllimport function.  Fixed with `#ifndef _WIN32` around that one function;
`LoadBmp` stays unconditional (dead but harmless on Windows — its only caller
is the guarded shim).

**2. `pal/win_transport.cpp`.**  Not predicted, and the more interesting of the
two.  `759d778` replaced the literal `9000` with `COMPRESS_BUFFER_LENGTH` in
the Windows transport adapter — but that macro lives in `doc.h`, and a
transport adapter does not include `doc.h`.  The **posix twin does the same
thing with the literal `10000` and a comment naming the macro**
(`platform/posix_transport.cpp:355`), so the two arms had drifted apart in the
one direction only Windows can notice.  Fixed by matching the twin.

**The lesson generalises §8a's, and is worth carrying.**  §8a's rule was "a
shared header's `#ifndef _WIN32` block is the most dangerous place in this tree
to put an include".  The mirror image is now measured too: **a Windows-*only*
file is exactly as invisible to macOS as an `#ifndef _WIN32` arm.**  The
complete set is `pal/win_transport.cpp`, `pal/win_worldview.cpp` and
`pal/gdi_surface.cpp`; five round-two commits touched two of them (`759d778`,
`47896a0`, `4e4ce04`, `b29469d`, `6b13a53`), and one of the five broke the
build.  A cheap standing check for the mac side: after editing any
`pal/win_*.cpp`, grep the identifiers you introduced for a definition that only
`doc.h` supplies.

### 10b. Both configurations build and run clean

After those two fixes, **Debug|Win32 and Release|Win32 both link with no
warnings from our own code** — the remaining warnings are exactly the
pre-existing set §8a recorded (`LNK4099` from OpenSSL's static libs, three
`C4244`s in vendored `scripting/lua_compress.c`, two `GetVersionEx`
deprecations in `sqlite3.c`).

Release was checked against Debug on the whole probe set: **identical output**
for `miniwindow_conformance` (64/64 in both), `round2_conformance` and
`round2b_conformance`, apart from a wall-clock second in a timer readback.  So
nothing below depends on `_DEBUG` or on being unoptimised.

`WinRel` needs the same runtime assets staged beside it that `WinDebug` does
(§2) — worth saying because the first Release run looked like a hang and was a
bare directory.

### 10c. The three §9c rows with the highest stated risk — all confirmed

Driven by `tests/mud_stub.py --scenario …`, each run against **both** clients,
which is what §3 requires of any loopback stub.

**`759d778` MCCP with `FDICT` set — the top of the table, and the worst.**
Sampling CPU, private bytes and `Responding` every 2 s:

| | stock 5.06 | this build |
|---|---|---|
| responding | **False from the first sample and never again** | True throughout |
| CPU over 24 s | 0.03 → **22.25 s** (a full core, pegged) | 0.00 → 1.11 s |
| private bytes | flat at 14 MB | flat at 20 MB |
| outcome | frozen; killed | disconnects, prints the full statistics block, stays usable |

`inflate` answering `Z_NEED_DICT` consumes nothing and produces nothing, and
the old loop only tested `< 0`.  Stock is unusable after one such packet.

**`71c6afd` unterminated subnegotiation.**  Server opens `IAC SB 200` and
streams without ever sending `IAC SE`:

| | stock 5.06 | this build |
|---|---|---|
| bytes accepted | **1,430,841 and still climbing** when the run ended | **269,973** — the 256 KB cap plus what was in flight |
| private bytes | 15.1 → 17.2 MB, climbing monotonically | flat |
| CPU | pegged (the `CString` append is O(n) per byte) | 1.1 s over 24 s |
| outcome | grows until memory is gone (32-bit: `terminate()` at ~2 GB) | drops the connection |

The cap clears legitimate traffic: `--scenario subneg_big_ok` (a properly
terminated 200 KB GMCP payload) is accepted by both clients.  That answers
`docs/UPSTREAM-BUGS.md` row 74's open question.

**But the abort is not one-shot — see §10e, which is a finding.**

**`f878ff5` NAWS and the doubled 255.**  With the world's wrap column at 255,
the server sends `IAC DO NAWS` and logs the reply byte for byte:

```
stock 5.06 :  IAC SB NAWS  0 255      0 26  IAC SE     <- bare IAC mid-subnegotiation
this build :  IAC SB NAWS  0 255 255  0 25  IAC SE     <- doubled, per RFC 854
```

Exactly the defect and exactly the fix, measured on the wire.  (The row counts
differ by one because the two clients restored different window sizes — §3's
"pin both windows" note; both later report 34 rows after `MoveWorldWindow`.)

**`00f634e` TTYPE / MTTS — §9c's COMPILE verdict stands, but read this before
diffing against 5.06.**  Four `IAC SB TTYPE SEND` cycles:

```
both       :  mushclient  /  ANSI
stock 5.06 :  MTTS 9
this build :  MTTS 265
```

That difference is **not** from `00f634e`, whose surviving arm is the Windows
one.  It comes from upstream `d9d45b3` (2025-02-15, "MTTS: mark MushClient as
supporting TRUECOLOR codes"), which post-dates the 5.06 binary.  Anyone
comparing this tree against 5.06 will see it; it is not a finding.

### 10d. The rest of the §9c inventory, measured

Every row below is a differential against stock 5.06 unless it says otherwise.
"same" means the two clients printed the identical line, which for a COMPILE
row is the whole check.

| Commit | What was measured | Verdict |
|---|---|---|
| `cb903af` | `WriteLog("alpha\n")` then `WriteLog("beta")`: stock writes `alpha␍␊␍␊beta␍␊`, this build `alpha␍␊beta␍␊` | fixed as intended (DIVERGENCES §10) |
| `4639762` | A session logged with file/line preamble+postamble carrying `%n`, over notes, output and a direct write, in plain and HTML mode: the two log files are **byte-identical** apart from that one `WriteLog` newline | **dedupe is behaviour-identical** |
| `856fa00` | a trigger with only `omit_from_log` set: `GetTrigger` reports flags **5** on stock (`eEnabled|eOmitFromOutput`) and **3** here (`eEnabled|eOmitFromLog`); the control, a trigger with `omit_from_output`, reports 5 on both | fixed; round trip no longer converts one to the other |
| `856fa00` | `GetUdpPort(5555,5555)`: stock **0**, here **5555**; the multi-port and inverted cases identical | fixed |
| `8d7ab33` | export an array whose value ends in a literal backslash, then re-import: stock `30057` and an empty array, here `0` and both keys byte-exact | fixed |
| `4a0de8f` | `error_desc[30038]`: stock `nil`, here the real text; `error_code.ePluginCouldNotSaveState` 30037 → 30038 | fixed |
| `3f6e164` | `SetRemoveMapReverses(true)`: stock raises "number expected, got boolean", here accepted.  `world:StripANSI(...)` through the `other:Method` form: stock raises "string expected, got userdata", here returns `"red"` | fixed (upstream bugs 38, 40) |
| `203c0fe` | `ReplaceNotepad` from Lua: stock returns `nil`, here a **boolean** | fixed (DIVERGENCES §13).  Both arms answer `true` — a missing title is *created*, which is `AppendToTheNotepad` in replace mode, so that is correct |
| `648622c`, `e24ab4b` | `DatabaseColumnValue` on 2³¹, 2⁵³, 2⁵³+1, 2⁶⁰, `INT64_MAX`, `INT64_MIN`: stock returns `-2147483648`, `0`, `1`, `0`, `-1`, `0` — all wrong; here `2147483648`, the exact double, the decimal **string** `9007199254740993`, the exact double, the string `9223372036854775807`, the exact double | fixed, and **`INT64_MAX` is the row §4c-sexquadragies item 4 said needed an x86-64 Windows machine.  It answers a string.** |
| `fbb6695` | the same values through the bundled Lua `sqlite3` module: stock returns rounded doubles for 2⁵³+1 and `INT64_MAX`, here the same decimal strings the `world.Database*` surface gives | fixed, **and the two surfaces now agree value for value** |
| `fa694a4` | `script_strict_arguments`: stock `GetOption` −1 and `SetOption` 30025; here reads 0, sets to 1, reads back 1, and appears in `GetOptionList` | round-trips, and is inert (no consumer on Windows) |
| `d8de347` | `WindowFilter` blur over a 21×21 window: on stock the **top row of the region is unfiltered** and the rightmost pixel column keeps a different value (`0x333333` vs `0x141414` beside it); here every row including the first is filtered and the right column matches its neighbours | both defects fixed (upstream 78, 79) |
| `ef5466f` | colour-matching triggers — the table below | fixed, with one deliberate removal |
| `f06e369` | `ESC[91m`: stock leaves the colour at the default (`0xC0C0C0`), here it is bright red (`0x0000FF`); `ESC[101m` background 0 → 255.  `ESC ] 0 ; title BEL` prints `0;a window titleOSCBEL` on stock and `OSCBEL` here, same for the `ESC \` terminator | fixed (DIVERGENCES §15, §16) |
| `0656d24` | with *custom 16 as default output colour*, `ESC[37m`+`ESC[1m`: stock stays on the normal white swatch (`0xC0C0C0`), here it reaches bold white (`0xFFFFFF`).  Red and cyan, normal and bold, identical on both | fixed; the other colours did not move |
| `aad9e22` | a tab after `café` and after two CJK characters, UTF-8 world: stock puts the marker at byte 9 both times (tab stop computed in bytes), here at 10 and 13 (computed in characters).  **The ASCII world is byte-identical on both** | fixed |
| `1e03119` | `WindowWrite` a `.bmp`, then reopen it: the two files differ in **exactly one byte** — `biSize` at offset 14, **44 on stock, 40 here** (`sizeof(BITMAPINFOHEADER)`).  Stock's own loader refuses its own file (`30067`); this build's reloads as 16×16.  GDI+ (`System.Drawing`) agrees: it refuses stock's and accepts this build's as `Format24bppRgb` | fixed, root cause measured to the byte |
| `28ca804` | two worlds, both with a plugin implementing `OnPluginScreendraw`; world A's callback writes a note into world B.  **Stock never calls B's callback** for that note; this build does | fixed — see §10f for why the obvious probe misses it |
| `a53232d` | unlabelled timers list as `*timer0000000000`, `*timer0000000001` | same on both, as the COMPILE row predicted |
| `fc77994` | `CreateGUID` is 36 characters, `xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx` | same on both |
| `d4112c4` | `GetRecentLines(3000000000)` with a short buffer | same on both, no crash |
| `43e27b8` | `GetLineInfo(n).time` is a positive number matching `os.time()` | same on both (the 2038 arm is not reachable today) |
| `eb8c2fe` | bind and un-bind one key through `AcceleratorTo` 1200 times, then bind a fresh key | every return code `eOK`, fresh bind works — the id pool is not consumed |
| `5c53c3e` | the Debug summary's OS line | identical string on both |
| `32d5597` | a gagging trigger over a paragraph that wraps to **6** rows: net line change **0** on stock and here, i.e. the whole paragraph goes on both | **§4c-undequinquagies' premise confirmed: the upstream bug is latent on Windows, not live.**  MFC's `CList::FreeNode` really does leave `pPrev` intact |

**The colour matcher (`ef5466f`), in full**, because §4c-quintricies calls it
the highest-regression-risk item in the campaign.  Two triggers, "match text
colour = red" and "= black", both *keep evaluating*; the resolved RGB of each
probe line is read back so a non-match can be explained rather than guessed at:

| line | resolves to | stock | this build | |
|---|---|---|---|---|
| `ESC[31m` | 0x000080 | red | red | regression guard holds |
| `ESC[38;5;1m` | 0x000080 | red | red | index < 8 matched by coincidence before, by appearance now |
| `ESC[38;5;9m`, `ESC[38;5;196m` | 0x0000FF | — | — | correctly neither: this world's *red* is 0x000080 |
| **`ESC[38;2;128;0;0m`** | 0x000080 | **—** | **red** | **the fix**: a 24-bit run that displays as the world's red now matches |
| `ESC[38;2;170;0;0m` | 0x0000AA | — | — | correctly neither |
| `ESC[1;31m` | 0x0000FF | red | red | bold red still matches (HILITE carried into the resolve) |
| `ESC[7;31m` | inverse | black | black | identical, and identical again with `alternative_inverse` on |
| `ESC[30m`, `ESC[38;2;0;0;0m` | 0x000000 | black | black | both still match |
| no SGR | 0xC0C0C0 | — | — | identical |
| **`ESC[38;2;1;0;0m`** | 0x000001 | **red** | **—** | **the one removal**: a near-black red whose COLORREF *is* the integer 1 no longer satisfies a "red" condition |

Nothing that matched before stopped matching except that last row, which is the
coincidence the fix exists to remove.  **Do not read this as a clean bill for
item 6** — a real xterm-256 world with a large existing trigger set for a
session is still owed (§10h).

**One methodological warning, because it nearly produced a false report.**  The
first version of this probe did not send `ESC[0m` between lines, so an earlier
`ESC[7m` was still in force when the "black" probes ran and this build looked
like it had *stopped* matching plain black.  It had not.  Any probe that sets
SGR state must reset it, and any "regression" seen in a colour probe should be
re-run with the reset before it is written down.

### 10e. FIXED — the subnegotiation cap was not one-shot (`71c6afd`)

**Round two introduced this, and this pass found and fixed it.**  Stock 5.06 has
no cap at all, so there is nothing to diff against; the finding is against the
fix's own intent.

**What was wrong.**  Neither guard site reset `m_phase` or cleared
`m_IAC_subnegotiation_data` before returning (`telnet_phases.cpp:619` and
`:641`), and `DisplayMsg`'s byte loop calls both helpers as
`Phase_SUBNEGOTIATION (c); continue;` — a `void` call that cannot stop the
loop.  So **every remaining byte of data already received re-entered the same
branch**, each one calling `OnConnectionDisconnect()` again and raising another
modal `UMessageBox`.  `m_phase` is only reset at *connection setup*
(`doc_shared.cpp:1000`), never at disconnect.

Measured with `tests/mud_stub.py --scenario subneg_overflow_inject` (the cap
plus 16 bytes, then a marker), counting **distinct** dialog handles:

```
before:  distinct modal dialogs raised: 8      x8  Telnet subnegotiation too long - closing connection
after:   distinct modal dialogs raised: 1      x1  Telnet subnegotiation too long - closing connection
```

And with megabytes still in flight, the bytes after the cap were **rendered into
the output window as ordinary MUD text** — 31 lines of the attacker's filler,
after the client had announced it was closing.  After the fix: none.

**The fix, in three parts.**

1. `CMUSHclientDoc::AbandonSubnegotiation()` (new, `telnet_phases.cpp`) does the
   reset *before* the disconnect: erase the collected data, clear
   `m_subnegotiation_type`, `m_phase = NONE`, then `OnConnectionDisconnect()`
   and the message box.  Order matters twice over — it makes the guard
   unreachable a second time, and `UMessageBox` pumps messages, so a re-entrant
   `ReceiveMsg` during that pump would otherwise find the phase machine still
   inside a subnegotiation the client had already abandoned.
2. Both guard sites call it instead of open-coding the disconnect.
3. `doc_shared.cpp`'s two `HAVE_SUBNEGOTIATION` dispatch arms now watch for the
   disconnect and `return`, abandoning the rest of the packet — which is
   precisely what the two MCCP arms beside them already do.  The test is
   before/after (`bWasConnected`), not a bare "are we connected?", because
   `world.Simulate` feeds this loop on a world that is legitimately
   disconnected.

Parts 1 and 3 are deliberately belt-and-braces: this is a hostile-input path
and the two halves fail independently.

**Verified on Windows.**  In **Debug**: the dialog count is 1 (was 8), no filler
reaches the output window (was 31 lines), `--scenario subneg_big_ok` — a
properly terminated 200 KB GMCP payload — is still accepted, `ttype`, `naws`
and `mccp_normal` are byte-for-byte as before, and the whole conformance set
differs from stock 5.06 in **exactly the same set of keys** as it did before the
change: nothing new appeared and nothing stopped differing.  In **Release**:
builds clean, dialog count 1, `subneg_big_ok` accepted.  The remaining Release
checks were run before the fix, where Release and Debug already agreed
key-for-key (§10b), rather than repeated after it.

**Not verified headless**, and this matters: `add_subdirectory(tests)` sits
inside `if(NOT WIN32)` (`CMakeLists.txt:1259`), so `ctest` cannot be built on
Windows at all — see §10i.  **The macOS side must run `ctest` before this
merges.**  Three shared files changed (`telnet_phases.cpp`, `doc_shared.cpp`,
`doc.h`); `doc.h` gains a method declaration, not a member, so there is no
`CMUSHclientDoc` layout change and no stale-archive hazard.

**Two covering tests are added** to `tests/test_telnet.cpp`, and the reason the
existing ones missed this is worth keeping.
`Phase_SUBNEG_UnterminatedSubnegotiationIsCappedAndDisconnects` and its
escaped-IAC sibling both `break` out of their loop the moment the disconnect
appears, so neither ever asks what the *next* byte does.  **A test that stops at
the first sign of the behaviour it is checking cannot tell whether that
behaviour repeats.**  The new pair re-arm the connect phase after the abort and
keep feeding, and assert that the phase machine really did leave the
subnegotiation.

**Worth a look with it, not done here:** the sibling MCCP guards in
`doc_shared.cpp` have the same *shape* — `OnConnectionDisconnect()` then a
message box — but they are inline in the loop and already `return`, so they are
one-shot per packet by construction.  `--scenario mccp_fdict` nonetheless raised
2 dialogs rather than 1, which is unexplained and is the next thing to pull on.

### 10f. Two things the differential method got wrong before it got them right

Both cost a re-run, and both are worth carrying because the mistake is
reusable.

**A count is not a measurement of *which*.**  The first two-world Screendraw
probe made each world print something in turn and reported a call count.  Both
clients answered 2 and 2, and it looked like `28ca804` changed nothing on
Windows.  It changes nothing *there* because the guard only bites on
**re-entrancy** — what the process-global static broke is one world producing
output from **inside** another world's callback, which is what a relay or
chat-bridge plugin does.  Rewritten so world A's `OnPluginScreendraw` writes a
note into world B, and so that the plugin logs the *text* of every callback
rather than a total, the answer is unambiguous:

```
stock 5.06:  TwoWorldA #1 : sequential screendraw probe
             TwoWorldA #2 : armed relay, this note triggers A's Screendraw
             TwoWorldB #1 : sequential screendraw probe
this build:  … same three, plus
             TwoWorldB #2 : relayed from A's Screendraw 1
```

**Ask what the API actually takes.**  Three probes were silently vacuous on the
first run and had to be rewritten: `omit_from_log` has no bit in the
`AddTriggerEx` flags word (it is a trigger *option*), the log preamble strings
are **alpha** options that `SetOption`'s `VTS_I4` signature ignores, and
`GetLineInfo`'s timestamp is type code **9**, not 5.  Each produced a
plausible-looking "identical on both clients" line.  A differential probe that
sets nothing compares nothing, so **every probe that configures something should
read it back** — the rewritten versions all do.

### 10g. §4c-quaterquadragies — the four font measurements, answered

These were gating the remaining miniwindow-font work on macOS.  Measured with
`docs/FONT-PARITY-2026-08.md` §5's script.

**1. Dina.**  A bare `Dina` request realises as **Arial** — MUSHclient does not
register `Dina.fon` at startup; the only caller of `AddFontResourceEx` is
`world.AddFont` (`doc.cpp:3846`).  After `world.AddFont("Dina.fon")` (which
returns `eOK` on Windows) the same request realises as **Dina**.  So Dina *is*
available here, and the bitmap-strike line of enquiry is live — but note what
the unregistered case shows: with `pitchAndFamily` = `FF_DONTCARE`, GDI
substitutes a **proportional** face for a monospace request.  That is direct
corroboration, on the platform whose mapper is the reference, of the
`CFont::CreateFont` gap already commented at its site.

**2. Dina 8 pt.**  `tmHeight` **13**, ascent 10, descent 3, **`tmInternalLeading`
3**, ave/max char width 7/7, pitch 48.  Not the ~0 internal leading a pure
raster strike was expected to show — though it does snap like one: Dina at 10
and 12 pt both report `tmHeight` 16, and 14 pt jumps to 26.

**3. Courier New, the control.**

| pt | Windows `tmHeight` | macOS | Δ |
|---|---|---|---|
| 8 | 14 | 13 | +1 |
| 9 | 15 | 14 | +1 |
| 10 | 16 | 15 | +1 |
| 12 | 18 | 19 | −1 |
| 14 | 21 | 22 | −1 |

Every size within ±1 px.  **The size pipeline is exact — there is no residual
scale factor, and none should be added.**

**4.** `WindowTextWidth("MMMMMMMMMM")` for Courier New 10 pt is **80** on
Windows; the port produces ~79.  Within 1 px.

Full table for six faces × five sizes is in the run artefacts; the four rows
above are the ones the analysis turned on.

### 10h. What round two still owes

Not reached.  All of it is §4's dialog-driven half plus the items needing a
real MUD or a second machine.

- **§4c-duodequadragies** — the MXP atomic-tag veto (`3095c2a`).  **Two of the
  six are now tests (§10q)**, revert-checked; no plugin was needed, since the
  veto is reachable from the world's own OnMXP_OpenTag routine.  Four remain:
  the outstanding-tag warning marker, the close-tag callback, the
  <send>-inside-<a> substitution, and OnPluginMXPopenTag specifically.
- **`44f0737`** — the eight world- and global-preferences defects.  This is the
  largest dialog change in the inventory and the whole page-by-page walk is
  outstanding.
- **`2e54bba`** — `world.Help` and Game ▸ Function List listing Lua's own
  functions.  Both are modal.
- **`8dd87e5`, `6e67cb0`** — status-bar attribution with two worlds open.
- ~~**`30df30b`** — the Ctrl+click alias menu's `%1`.~~  **Done**: covered by
  AliasMenu.ExecuteSubstitutesWordUnderMenuIntoPercentOne in test_aliases.cpp,
  which has been passing on Windows since that suite was enabled (§10q).
- **`5359254`, `8468a76`, `bc4d149`** — the macro `switch`, the auto-say
  tri-state, and the split Stop-Sound commands.  All three need real typed
  input or menu clicks.
- **`43c4545`, `aca2478`, `c605461`, `9139076`** — plugin wizard, plugins
  dialog liveness, long-name file dialogs, the names file with an empty section.
- **`3f3e8ac`, `89ee850`, `c3936f4`** — the plugin-veto uninitialised return,
  the note-style restore, and `m_iNoteColourFore`.  All three need a plugin.
- ~~**`0b70de7`, `41eb7bd`, `c1a3990`, `fbc4161`, `3c58886`, `5ee518e`,
  `0dae192`** — the leak and use-after-free rows; none was exercised.~~
  **All seven are now exercised on Windows (§10p)**: four by tests in suites
  this target already compiles, three by `tests/win/test_gap_coverage.cpp`.
- **The font ascent/descent split** — §10g read font-info codes 1 and 4 and
  answered the size question with them, but never read **2 and 3**, so
  `pal::CpuRaster`'s invented `descent = cellH / 5`
  (`pal/cpu_raster.cpp:519-520`) still has nothing to be compared against and
  no assertion pins it. `tests/win/app_probes/fontmetrics.lua` now takes that
  measurement — a `SPLIT` line per face and size, recording the **realized**
  face beside the requested one — alongside two assertions that hold by the
  Win32 contract without knowing the answer (`tmHeight == tmAscent +
  tmDescent`, and `tmAscent >= tmInternalLeading`). Added on macOS and
  therefore **never run**: it is Lua rather than C++, and its syntax is checked
  against the Lua 5.1 the app embeds, but nothing here can execute it. Run the
  probe and paste the `SPLIT` lines into `docs/FONT-PARITY-2026-08.md` §3.
- **§4c-quintricies item 6** — a real xterm-256 world, live, for a session.
- **§4a's outstanding half** — keepalives and a long idle session.
- **`world.Debug("commands")`** — reports `0 commands.` on **both** clients
  after `world.PushCommand`.  `4874426`'s Windows arm is untouched by
  construction (`#ifndef _WIN32`), so this is not a round-two question, but
  `docs/UPSTREAM-BUGS.md` row 41 asks whether the Windows arm lists anything
  and this does not answer it: only *typed* input fills `CSendView::m_msgList`,
  and injected typing did not send (below).

**One instrument limitation, recorded so it is not re-derived.**  Posting
`WM_CHAR` per character into the command bar works — the text accumulates and
reads back correctly — but a posted plain `WM_KEYDOWN VK_RETURN` **did not
send the command**, on stock 5.06 and on this build alike.  §8v reports that a
posted plain Return *did* send; whatever the difference is (§8v's run drained
the startup dialogs and got real focus first, and used `keybd_event` for the
chorded cases), the posted-Return route did not work here.  Because it failed
identically on both clients it is the instrument, not the build — §3's rule.
Anything needing real typed input should follow §8v: drain every startup
dialog, confirm with `GetGUIThreadInfo` that the edit has the focus, then use
`keybd_event`.


### 10i. `ctest` on Windows: measured, not estimated

Asked directly during this pass, because it is the one thing that would make a
Windows session self-sufficient — every fix made here has to be handed to macOS
for its regression check.

**A survey of the tree said "six to ten engineer-weeks, write a third adapter
set".  A four-hour spike says days.  The spike is right and the survey was
wrong**, and the reason is worth more than the number: the survey reasoned about
`mushcore_testsupport`, the *macOS* adapter set, and concluded you would have to
rebuild it for Windows.  But the B-3 header flip means the tests include the
**real** headers — `tests/test_telnet.cpp` includes `stdafx.h`, `MUSHclient.h`,
`doc.h`, `pal/itransport.h` and nothing from `platform/` — and on Windows the
real headers are **real MFC, which works**.  There is nothing to port.

This is the `nm` discipline the project already uses for stub adoption
(CLAUDE.md: "measured with `nm`: adopting the four families leaves 5 / 1 / 0 / 2
undefined symbols"), applied to the same question.  It corrected the estimate by
an order of magnitude, in the same direction it did last time.

**What was measured.**

1. **Compile.**  `cl.exe` on `tests/test_telnet.cpp`, with the vcxproj's own
   Debug flags plus `/Itests`: **88 errors, which collapse to four causes.**

   | Cause | Errors | What it is |
   |---|---|---|
   | `CMUSHclientDoc`'s constructor is `protected` on Windows | **82** | `doc.h:721`, an **already-existing, already-documented** platform fork — off Windows it is made `public` because the wx layer `new`s the doc; on Windows `DECLARE_DYNCREATE` owns construction |
   | `CLine` has no default constructor on Windows | 1 | `OtherTypes.h:360`, the identical shape |
   | `g_debug` is a file-local `static const` on Windows | 3 | `telnet_phases.cpp:14`; `doc.h` declares it only off-Windows |
   | `EXPECT_EQ(a,b) << "msg"` | 2 | **not a portability problem — a gtest idiom in the two tests §10e added, which would not have compiled on macOS either.**  Fixed. |

   Widen the two constructor guards to `#if !defined(_WIN32) || defined(MUSHCLIENT_TESTS)` — one token each, in the pattern already there — and give `g_debug` a Windows home, and it is **zero errors**.  No header conflicts.  No `windows.h` collisions.  No POSIX problem anywhere in the file.

2. **Link.**  `test_telnet.obj` against the app's own 285 `WinDebug/*.obj`,
   `/SUBSYSTEM:CONSOLE`: 115 unresolved externals, of which **all but 28 were
   simply two missing libraries** (`lua5.1.lib`, `winmm.lib`) and the remaining
   28 were `CMUSHclientApp` members from the one object deliberately excluded.
   Put `MUSHclient.obj` back, recompile the two TUs that see the widened
   constructor guard (`doc.cpp`, `doc_construct.cpp`) so the mangled names
   agree, and it is **zero unresolved symbols — a 9.4 MB test executable.**

3. **Run.**  This is where the remaining work is, and it is MFC *runtime
   initialisation*, not architecture.  Entering through `main()` rather than
   `WinMain()` means MFC never initialises: the first assertion is
   `afxwin1.inl:21`, and `AfxWinInit (::GetModuleHandle (NULL), NULL,
   ::GetCommandLine (), 0)` clears it — the documented "MFC in a console
   application" fix.  The next is `oleunk.cpp:455`, the OLE/automation
   reference counting, which `AfxOleInit` + `AfxSocketInit` did **not** clear.
   `CMUSHclientDoc` is an automation object, so that is the thread to pull.

**So the recommended path, in order.**

- **Add `MUSHclient_tests.vcxproj` to the existing solution**, not a CMake arm.
  `/SUBSYSTEM:CONSOLE`, the same compile settings as `MUSHclient.vcxproj`,
  `MUSHCLIENT_TESTS` defined **project-wide** (the spike proved that a
  half-applied define changes a mangled name and breaks the link), the same
  `<ClCompile>` list plus `tests/`, and the two extra libraries.

  *Why MSBuild rather than extending CMake:* the vcxproj already holds the exact
  settings that make this engine compile with MSVC — the `/D` set, MBCS,
  `/std:c++14`, PCH off for the `pal/` files.  Re-deriving those in CMake means
  two definitions of "how to compile this on Windows", and drift between two
  definitions of the same thing is precisely the bug class this whole document
  exists to catch (§10a is one).  The cost is that "ctest" becomes "run the exes
  and check exit codes", which `mc_test.h` already supports — it is a five-line
  script, and the word `ctest` in the request means the regression suite, not the
  tool.

- **Glob `tests/test_*.cpp` with an explicit exclusion list**, rather than an
  inclusion list.  The default should be that a new test runs on Windows unless
  someone says otherwise; an inclusion list silently rots the moment a test is
  added on macOS.  This inverts the drift, and drift is the thing to design
  against.

- **Exclude the shim tests deliberately, and say so in the exclusion list.**
  About 10,000 lines and ~350 cases test the *port's* substitutes for things
  Windows has natively: `test_mfc_ui.cpp` (6,267 lines, 192 tests — `:3462`
  asserts `App.platform == VER_PLATFORM_WIN32_NT` against the shim's
  *fabricated* OS info), `test_cstring.cpp`, `test_collections.cpp`,
  `test_posix_socket.cpp`, `test_gdi_headless.cpp`, `test_mc_dispatch.cpp`,
  `test_pal_transport.cpp`.  On Windows `CString` is Microsoft's problem.  A
  Windows suite is a **smaller** suite by design, and that is correct rather than
  a compromise.

- **Then work the MFC console-host initialisation** until a first test file runs
  green, starting from `oleunk.cpp:455`.

- **Last and least: the POSIX calls in the tests** — 18 files, ~30 include lines,
  behind them `socketpair`, `dup2`, `getrusage`, `setrlimit`, `mkstemp`,
  `usleep`, `uname`.  Nearly all are scaffolding (timing, temp files), not
  assertions.  A small `tests/posix_compat.h` plus `MC_TEST_SKIP` for the few
  that genuinely cannot work.  `tests/mc_test.h` itself is portable C++17 and
  already guards its one POSIX touch.

**What the payoff is, and the warning that comes with it.**  There are **249
`#if*_WIN32` decision sites across 70 of the 135 TUs the tests link**, and the
Windows arm of every one of them has never been executed under test.  That is
the actual prize — far more than re-running what macOS already runs.  It is also
the warning: expect the first green run to be preceded by a burst of failures
that are *real Windows bugs*, not porting artefacts.  That is what happened the
first time a sanitizer was pointed at this code — a use-after-free on every
gagging trigger, after the code had already survived a 218-finding audit, four
review rounds and 75 green tests.

**And still do the cheap thing first.**  A Linux runner (WSL, or CI) builds and
runs the existing suite unchanged, today, because the code is kept Linux-clean.
It gives back the regression net for shared source — which is the gap a Windows
session actually feels, and it would have covered §10e's fix.  It does not
exercise the Windows arms; the vcxproj test target is what does that.  They are
complements, and the runner is hours while the target is days.

### 10j. The Windows test target — built, and what it found

§10i sized this at days rather than the six-to-ten engineer-weeks a survey
predicted.  It was built the same day.  **`MUSHclient_tests.vcxproj` compiles
286 engine sources and 14 test files into `WinTest\mushclient_tests.exe`, and
it runs.**

```
pwsh tests\win\run_tests.ps1              # build + run, Debug
pwsh tests\win\run_tests.ps1 -NoBuild     # run what is there
pwsh tests\win\gen_test_vcxproj.ps1       # regenerate the project
```

**First run: ~69 tests executed, 18 failures in two suites, then an MFC
collection assertion aborts the process.**  Nothing about those numbers is
final — see *What the failures mean* below, which is the part that matters.

**How it is put together, and why each piece is the way it is.**

- **One executable, not forty.**  An MFC test binary is 19 MB and a full link;
  forty of them is minutes per build.  `MC_TEST_COMBINED` makes each test
  file's `MC_TEST_MAIN()` expand to nothing and `tests/win/all_tests.cpp`
  supplies the one `main`.  No test file needed editing for this.  The cost is
  that the suites share a process, so an `abort()` in one ends the rest — which
  is exactly what happens today.
- **It cannot share the app's object files.**  `MUSHCLIENT_TESTS` turns
  `CMUSHclientDoc`'s constructor from protected into public, and access is part
  of the C++ mangled name, so a half-applied define makes `doc.obj` reference
  `??0CMUSHclientDoc@@IAE@XZ` while `doc_construct.obj` defines
  `??0CMUSHclientDoc@@QAE@XZ` and the link fails on a symbol that looks
  identical.  Hence its own compilation of all 286 TUs, and hence
  **`tests/check_win_test_sources.py`**, which fails `ctest` if the two
  projects' `<ClCompile>` lists ever diverge (with its own four-case fixture
  selftest, as the three lints beside it have).
- **MSBuild, not CMake**, for the reason §10i gives: the vcxproj already holds
  the settings that make this engine compile with MSVC, and a second definition
  of those would drift.
- **An exclusion list, not an inclusion list.**  A new test file is compiled
  into the Windows target by default and has to be argued *out*, with a reason
  recorded next to its name in the generator.

**The host.**  On Windows the engine compiles against real MFC, so the binary
is an "MFC in a console application" host: `AfxWinInit` (without it the first
`AfxGetApp()` asserts in `afxwin1.inl`), `AfxOleInit` (`CMUSHclientDoc` is a
`CCmdTarget` with `EnableAutomation`, and `~CCmdTarget` calls
`CoDisconnectObject`), `AfxSocketInit`.  And CRT/MFC assertions are routed to
**stderr rather than a dialog** — a failed ASSERT in a test process is a test
result, not something to wait for a human on.  That one line is what made the
target possible at all: the first run was wedged behind an invisible modal box.

**Six things the build taught, all of them the same shape.**

| Symptom | Cause |
|---|---|
| 36,810 errors, nearly all inside the Windows SDK | `platform\` on the include path. `tests/CMakeLists.txt` puts it there, which is safe when `platform/` **is** the platform; on Windows `platform/direct.h` masks the CRT's `<direct.h>`. The port's directories stay off the path; tests that want a port header spell the directory. |
| `#error LINK_SIZE must be either 2, 3, or 4` | The generator dropped **per-file** settings. 246 of the 286 entries carry them, and the 36 `pcre\*.c` need `HAVE_CONFIG_H`. It copies child nodes verbatim now, so a future per-file setting is inherited rather than lost. |
| `'binary_function': base class undefined` | The tests need C++17 (`mc_test.h` uses `if constexpr`); C++17 removed `std::binary_function`, which `stdafx.h` derives from. `_HAS_AUTO_PTR_ETC=1` on the test project only, so §2's "no C++17 in code MSVC compiles" stays true of the shipping client. |
| `'(': illegal token on right side of '::'` | `windows.h`'s `min`/`max` macros versus `std::max`. Parenthesised as `(std::max)(...)` in two test headers. |
| `#error MFC requires use of Winsock2.h` | `tests/posix_compat.h` pulled in `windows.h` ahead of `stdafx.h`. Fixed *in that header* — one advertised as safe to include anywhere has to get the order right on its own behalf. |
| `_main already defined` | **The interesting one.**  `test_database.cpp` and `test_lua_scripting.cpp` gate their whole bodies on `MUSHCLIENT_LUA_SCRIPTING`; without it each falls to a "Lua not available" stub *that defines its own main*.  They were not conflicting, they were **silently empty**.  Had the duplicate-symbol error not stopped the build, two large suites would have shipped passing vacuously — the same shape as §10f's three vacuous probes. |

**What is excluded, and the test that decides it.**  28 files, each with a
reason in `tests/win/gen_test_vcxproj.ps1`.  The question is always *does the
Windows product contain the thing this test asserts on?*

- **Permanent (10)** — it does not: `test_mfc_ui`, `test_cstring`,
  `test_collections`, `test_posix_socket`, `test_gdi_headless`,
  `test_mc_dispatch`, `test_pal_transport`, `test_pal_surface`,
  `test_menu_rules`, `test_notepad_convert`.  These test the port's
  *substitutes* — on Windows `CString` is Microsoft's problem.  A Windows suite
  is a smaller suite by design, and that is correct rather than a compromise.
- **Re-enterable (18)** — the suite is fine, it just reaches a port-owned seam:
  `notepad_fixture.h`'s `CTextDocument::m_fn_*` (5 suites), the wx hooks for
  clipboard / status / sound (3), `socketpair` (2), `LoadWorldFromFile` (1),
  port-only globals and the `BSTR`-as-`char*` typedef (4), and
  `test_plugins.cpp`, which is **undiagnosed**: a `#define` at `:125` reads as
  an undeclared identifier at `:1736` with no platform guard anywhere in the
  file.  `test_lua_scripting` (5,000 lines) and `test_triggers` / `test_timers`
  are the ones most worth reclaiming.

**What the failures mean — do not over-read them.**  11 of the 18 are
`Accelerators`, where `AcceleratorTo("F5", "sing", eSendToWorld)` answers
`eBadParameter` (30046) instead of `eOK`.  The accelerator family reaches
`Frame` and an accelerator table, and **this host has no main window**, so that
is at least as likely to be a limitation of the console host as a defect in the
product.  The other 7 are `EvaluateArms`.  **Triage is owed before any of this
is called a bug**, and the first question for each is "does this need a window?"
rather than "is Windows wrong?".

That caveat is the whole reason the count is reported rather than celebrated.
The value delivered here is the *target*, not its first verdict.

**Still owed on the target itself:** the `oleunk.cpp:455` assertion on every
document destruction (69 of them in a run — `CoDisconnectObject` failing
despite `AfxOleInit`), and the `afxcoll.inl:384` assertion that ends the run
early and hides the summary line.  Neither is understood yet, and until the
second is fixed the pass count is a floor rather than a total.

**One trap for whoever iterates next:** MSBuild's up-to-date check did not
notice a changed preprocessor definition across a regenerated project file, and
a stale `test_lua_scripting.obj` kept the old "Lua unavailable" `main` for a
whole cycle.  After changing the defines, delete `WinTest\obj`.

**Superseded by §10k, which triaged all 18.**  The counts above moved with it --
as of §10k the target compiled **13** test files with **29** excluded,
`test_accelerators.cpp` having joined the list for the reason below, and §10l
moved them again -- and two claims above are wrong, corrected here rather than
edited away:

- "this host has no main window" — it is no **view**, and the distinction
  matters because it changes the fix.  `AcceleratorTo` walks
  `GetFirstViewPosition()`, not a window handle.
- "Neither is understood yet" — both are.  `oleunk.cpp:455` is COM never being
  initialised despite `AfxOleInit()` answering TRUE; `afxcoll.inl:384` is a
  `CPtrList` walk of an empty `m_LineList`, which was empty because
  `SetUpOutputWindow` never ran.  Fixed, and the pass count is now a total
  rather than a floor.

The reading that held up is the cautious one: the first question per failure was
"does this need a window?" rather than "is Windows wrong?", and **none of the 18
was a defect in the Windows product**.

### 10k. Triage: 459 passing, 0 failing — and one real bug

§10j reported 18 failures and an abort, and said the count was reported rather
than celebrated because triage was owed.  Triage is done.

```
459 passed, 0 failed          exit 0
```

That is every test the target compiles: 460 `TEST(` across the 13 enabled files,
one of them guarded out below, 459 executed.  The declared count is checked
against the executed one on purpose — §10j's own lesson was two suites that fell
through to a "Lua unavailable" stub and would have passed vacuously.

**Not one of the 18 was a defect in the Windows product.**  There were five
causes, and four of them are the same shape: *the test host is not the app, and
the app does its setup in places a host with no window never reaches.*  The
fifth was in the tests.  The one genuine product bug came from somewhere else
entirely — a test that had been passing on macOS for months.

**1 — `DisplayMsg` returned at its first line, silently, for every document.**
This was the big one: four of the eighteen failures traced straight to it, and so
did the abort that ended the run before ~220 further tests could be reached.

```cpp
// cannot go very far without this - must be called at world loadup ...
if (!m_LinePositions.IsAllocated ())
  return;
```

`m_LinePositions` is allocated in `SetUpOutputWindow`, which Windows runs at
**view creation**, and this host has no view.  So every `Note`, every `Trace`,
every echoed command and every line of MUD text was dropped on the floor and
`m_LineList` stayed empty — and the first test to walk it from
`GetHeadPosition()` took the process down inside `afxcoll.inl`, which is the
`afxcoll.inl(384)` abort §10j could not explain: `CPtrList::GetAt` asserts on a
NULL POSITION, then throws.

The port has answered this since the B-3 flip — `doc_construct.cpp`'s
`MUSHCLIENT_HEADLESS_APP` arm seeds the first line and the index, with a comment
saying exactly why — so **macOS has never seen this and never will**.  The
Windows test build now seeds the same two things under
`_WIN32 && MUSHCLIENT_TESTS`, in `SetUpOutputWindow`'s order, and deliberately
without the headless arm's two port-only choices (the world-doc registration, and
`m_bNotesInRGB`, which is a *port* default — a Windows test that adopted it would
be testing the wrong client).

Measured, not reasoned: a probe printed `lines=0` for a trace, a `USER_INPUT`
echo and a MUD line; after the seed, 2 lines each and the trigger's
`nMatched=1`.

**2 — the app's other InitInstance state was missing too**, and each gap failed
tests that looked unrelated to each other:

| Missing | Symptom |
|---|---|
| `MXP_LoadEntities` (`MUSHclient.cpp:751`) | the XML parser rejects any document containing `&amp;` — and a world's *custom* entity **shadows** a predefined one instead of losing to it |
| `LoadGlobalsFromDatabase` (`:614`) | `App` is merely zero-filled, so every global option whose Windows default is 1 reads as 0.  `RegexpMatchEmpty` is one of them, and it quietly changed what every regexp in the suite matches |

Both are now in the host, and the second is worth the detail: it calls the
**real** loader with **no prefs database open**.  `db_simple_query` returns
`SQLITE_CANTOPEN` and the caller's default when `App.db` is null
(`MUSHclient.cpp:1947`), so all ~77 options take their table defaults from
`globalregistryoptions.cpp` — the real function over the real table, not a
hand-copied set of constants that would drift from it.  It also means the run
*cannot* read the developer's installed preferences, so it is reproducible on any
machine.  `platform/headless_app.cpp` reaches the same state by listing the
non-zero defaults by hand; on Windows the loader itself is available, so use it.

**3 — COM was never initialised, and `AfxOleInit` returning TRUE said otherwise.**
§10j left the 69-per-run `oleunk.cpp(455)` assertion unexplained.  Measured:
after a *successful* `AfxOleInit()`, `CoInitializeEx` returns **`S_OK`, not
`S_FALSE`** — so COM was not up — and both `CoDisconnectObject` (which
`~CCmdTarget` calls for every document) and a bare `CoCreateInstance` answered
`CO_E_NOTINITIALIZED`.  One `CoInitializeEx` in the host took the assertion count
from 69 to 0.  It is written as a hard failure rather than a hopeful extra call:
if a future MFC does initialise COM there, this returns `S_FALSE`, which is not
`FAILED`, and nothing changes.

**4 — `/tmp`.**  15 test files build scratch paths as `/tmp/mc_test_<pid>…`, 120
of them.  On Windows a leading slash means the root of the *current drive*, so
they resolved to `C:\tmp\…` and failed to open; `OpenLog` then returned
`eCouldNotOpenFile` and tests asserting on log **content** failed for a reason
that had nothing to do with logging.  The host creates the directory.  That is
deliberately not 120 edits to 15 files macOS also compiles: this is a property of
the host, and the scaffolding is not what any of those tests is about.

**5 — a BSTR read as `char*`.**  All seven `DatabaseTest` failures were
`EXPECT_STREQ("alpha", (const char*) v.bstrVal)` answering `"a"`: on the port a
`BSTR` **is** a `char*`, on Windows it is UTF-16, and the comparison then turns
on the first character alone.  Routed through `CString`, which is the conversion
on Windows and the identity on the port — the idiom
`tests/variant_test_helpers.h` already used three lines away.

**Two tests assert port-only behaviour and now say so.**

- `EvaluateArms.VariableExpandingTriggerRecompileArms` hands `EvaluateTrigger`
  three triggers with `regexp == nullptr`.  Windows compiles a trigger's regexp
  when it is added, and the recompile arm the test aims at sits *inside*
  `if (trigger_item->regexp)`; what lets a null-regexp trigger reach it is the
  lazy-compile block above it, which is itself `#ifndef _WIN32`.  The test's
  premise, not the product, is what does not hold there.  Guarded.
- The three `RegexpTest` / `RegexpWildcardsTest` cases that force a PCRE runtime
  error go through a new `regexec_erroring` helper.  `regexp.cpp:109` **throws**
  on Windows where `:119` returns false off it — a deliberate, commented
  divergence — and both mean the same thing to tests that are about what happens
  *after* the error (program nulled, code recorded, second exec early-outs).
  Normalising the report keeps the assertions shared; a spurious *match* still
  fails, because that is returned rather than thrown.

**`test_accelerators.cpp` is excluded, and §10j guessed its reason wrongly.**
§10j said "this host has no main window".  It is no **view**, which is a
different and more specific thing: `AcceleratorTo` opens with
`GetFirstViewPosition()` and returns `eBadParameter` before touching the map,
because the live accelerators are an `HACCEL` owned by the world's MDI child
frame.  Giving the host a window would not fix it; giving the document a view
would — and then `App.m_pWorldDocTemplate->m_hAccelTable` needs a doc template
too, which is why this is an exclusion rather than a fixture change.  The map
bookkeeping the suite asserts on **is** shared, and is worth re-entering later.

**One real bug, and it is upstream** — `docs/UPSTREAM-BUGS.md` 88.
`Get_XML_date` builds its result as `CTime (…, iSecond, 0)`, and that trailing
`0` is `nDST`: it tells `mktime` that a wall-clock time recorded during daylight
saving is *not* during daylight saving.  Reading the hour back through
`localtime` then returns it one hour later.  Measured here: `"2020-05-06
07:08:09"` parsed back as hour **8**, and every other hour in the suite was off
by exactly one the same way.  The port never saw it because
`platform/mfc_time.h`'s `CTime` ignores the argument and always passes `-1`, so
the port's answer was right and Windows' was wrong.  Fixed by passing `-1`, which
is a no-op on the port.

This is the target earning its keep on its first green run: the test that caught
it is not new, it had been passing on macOS for months, and nothing but running
it against real MFC could have found the difference.  It is also the **third**
bug in `Get_XML_date` — entries 5 and 6 are the other two.

**Two host changes worth more than the bugs they fixed.**

- **An exception that escapes a test is now a failed test, not a dead run**
  (`mc_test.h`, every platform).  One bad walk was killing the process and taking
  every suite after it down — silently, because the summary line prints after the
  loop.  This single change took the run from 231 tests to 451.  It is
  `catch (...)` rather than `catch (CException*)` because every test file
  includes `mc_test.h` *before* `stdafx.h`, so the MFC exception classes are
  incomplete there and a handler may not name a pointer to an incomplete type.
- **`setvbuf(stdout, NULL, _IONBF, 0)`.**  `[PASS]` goes to buffered stdout and
  MFC's traces to unbuffered stderr, so in a merged log the last line before an
  abort was *not* the last test that ran.  Two rounds of triage were spent
  looking at the wrong suite before that was noticed.

**Method note.**  The 18 failures shared a symptom and had five causes; the three
that looked most alike (`sawTyped`, `sawTrace`, `sawExec`) shared one, the two
that looked identical to them (`OpenLog`) did not, and the two "the script did
not run" ones split between the seeding gap and a port-only premise.  This is the
settings audit's lesson in a new place: *a shared symptom is not evidence of a
shared cause*.  What separated them was a probe printing actual values, not more
reading.

**Shared files changed — macOS must re-verify.**  `tests/mc_test.h` (the
`catch` is unguarded and changes behaviour on both platforms),
`tests/test_database.cpp`, `tests/test_regexp.cpp`, `tests/test_xmlparse.cpp`,
`tests/test_aliases.cpp`, `xml/xmlparse.cpp` (a no-op there, but it is canonical
code), and `platform/wx_mfc_hooks.h`, where `HeadlessMessageBoxAnswer` moved
*outside* the `#ifndef _WIN32` guard so all three headless hosts share one
answer.  Everything in `doc_construct.cpp`, `MUSHclient.h`, `MUSHclient.cpp` and
`Utilities.cpp` is behind `_WIN32 && MUSHCLIENT_TESTS` and is invisible to macOS
and to the shipping client.

**Still owed.**  The run ends with an MFC leak dump (`Detected memory leaks!`,
~80 lines).  Some of it is documents the tests deliberately leak; none of it has
been read yet.  Noise today, possibly a finding tomorrow.

### 10l. Two suites re-entered, and the leak dump made readable

```
TEST() declared by the enabled files: 566
561 passed, 0 failed          exit 0
```

`test_timers.cpp` and `test_triggers.cpp` are in the Windows target: 15 test
files now, 27 excluded, and the suite went from 459 tests to 561.  The gap of
five between declared and executed is the five tests guarded off Windows — one
in `test_aliases`, two in each of the two new arrivals — which is why the runner
prints both numbers.

**Getting in cost two guards each, as §10j predicted, and then one thing it did
not.**  The guards were the easy half: `test_timers` watches a reconnect through
`g_wxhook_set_status` / `_set_status_for` and `test_triggers` drives the
clipboard through `g_wxhook_get_clipboard_text` / `_set_`, and none of those
hooks exists on Windows because the real client writes the status bar through
MFC's message map and talks to the real clipboard.  Two tests each, guarded,
with a note at the guard saying what a Windows-side observable would have to be.

`test_timers` was then green on its first run.  `test_triggers` failed **37 of
its 69**, and all 37 had one cause — in the *fixture*, not in any test:

```cpp
static CTrigger* add_trigger(CMUSHclientDoc& doc, const char* match, ...)
{
    CTrigger* t = new CTrigger();
    t->trigger  = CString(match);
    ...                        // and nothing ever compiles t->regexp
```

Windows compiles a trigger's regexp when the trigger is **added** —
`AddTriggerEx` and the trigger dialog both do — and a `CTrigger` built by hand
never passes through either.  The port survives that because `evaluate.cpp`
compiles lazily on first use, and **that whole arm is `#ifndef _WIN32`**.  So on
Windows every hand-built trigger arrived at `EvaluateTrigger` with a null
regexp and was skipped before a match was even attempted.

**This corrects something §10k got right for the wrong reason.**  §10k guarded
`EvaluateArms.VariableExpandingTriggerRecompileArms` off Windows and called it a
test whose *premise* was port-only.  That is true of that test, but the
diagnosis stopped one level too high: the same dependency is not one test's
premise, it is the shape of every by-hand trigger in the suite, and 37 more
tests were sitting on it.  The signal was there to read — a whole file's worth
of triggers built the same way as the one test already identified — and it was
not read until the build made it unavoidable.

The fix is in the fixture and it compiles the pattern **the way the engine
does**, copied from `evaluate.cpp` rather than approximated, because a fixture
that compiles the pattern differently is testing its own regexp and not the
client's.  It runs at feed time rather than add time — four tests set `bRegexp`
or `ignore_case` *after* adding, and those change what the pattern compiles to,
so add-time compilation would need a "remember to recompile" rule that the next
test to be written would forget.  Only null regexps are touched, so a trigger
added through `AddTriggerEx` keeps exactly the program the engine gave it, and
on the port this simply gets there before the lazy arm does.

That took 37 failures to 1.  The last one was
`NegativeSequencePluginTriggersEvaluateBeforeWorld`, and it is worth its own
line: **"the trigger list" is not one list.**  Plugins keep their own
`m_TriggerArray` and are evaluated from it, so the sweep has to walk
`m_PluginList` as well as the document.  One test out of 561 was the difference
between noticing that and not.

**The leak dump is now a number instead of a wall.**

§10k said the run ends with an "~80-line" MFC leak dump.  It was **2,684
lines** — the estimate was made from the first forty and never checked, which is
the same shape of mistake as reading a build failure off the first error-looking
line (below).  Nearly none of it was a leak: `App` outlives the dump and still
owns the MXP element / entity / colour tables `MC_TEST_WIN_APP_STATE` loads, so
its ~500 `CObject`s and their collection plexes are reported as leaked while
being perfectly alive.  Measured: adding `MC_TEST_WIN_APP_STATE` grew the dump
from 1,618 lines to 2,684 **without leaking anything**, which is what proved it.

So the host takes a `_CrtMemCheckpoint` once its own setup is finished, diffs
against it after the last test, and reports that instead —
`AfxEnableMemoryLeakDump(FALSE)` suppresses the exit dump that made the summary
line unreadable.  What survives the diff was allocated by a test and not freed
by it, which is the only question this binary can usefully answer about memory.
Today that is:

```
80,711 bytes in 2,038 Normal Blocks
27,848 bytes in   556 Client Blocks
```

Not chased.  It is now a number that can move, which it was not before; and it
is emphatically **not** a substitute for a sanitizer, since it sees only blocks
that were never freed and says nothing about use-after-free or overruns.
`docs/SANITIZERS.md` is still the instrument for those, on the macOS side.

**One trap, and it cost a wrong diagnosis.**  `vcvarsall.bat` writes
`'vswhere.exe' is not recognized as an internal or external command` to stderr
and carries on regardless; it is in every run, **including the green ones**.
When the build first failed after re-entering the two suites, that line was at
the top of the log and was read as the failure — the real cause was a
`test_triggers.cpp` compile error 10 lines further down, and the first
"diagnosis" was of a message that had been there all along.  `run_tests.ps1`
now redirects vcvarsall's stderr as well as its stdout, so the line is gone.
The general form: **a message that appears in a passing run cannot be the reason
a run failed** — check it against a green log before believing it.

### 10m. Five suites were behind one `#include`, and behind that, one NULL

`tests/notepad_fixture.h` was not `_WIN32`-guarded, so **five** suites that use
it — `test_connection` (154 tests), `test_mxp` (131), `test_world_load` (105),
`test_lua_surface` (94) and `test_notepad_methods` (18) — could not be compiled
into the Windows target at all.  Four of them use it in **one or two places**.
That is ~480 tests kept off Windows by an `#include`, and the exclusion list
recorded the cause for each as "notepad_fixture.h, as above" without ever
saying how little of the file depended on it.

The header is guarded now, so including it on Windows costs nothing and a use
site that forgets its own guard gets an undeclared-identifier error naming
`FakeNotepad` or `NotepadCapture` — which is the right failure.  The 21 cases
that really do use it are guarded individually, each saying what a Windows-side
observable would have to be.  A notepad on Windows **is** a real `CEditView`
created through the MFC doc/view framework: there is no callback seam to
substitute, and this host has no document template to make one with.

**And then none of them landed.**  Every one hit a *second* blocker, larger
than the recorded one and behind it:

| Suite | What the list said | What was actually in the way |
|---|---|---|
| `test_world_load` | "notepad_fixture.h, as above" | **98 uses of `CMUSHclientDoc::LoadWorldFromFile`**, which `doc.h` declares `#ifndef _WIN32`.  The suite *is* that entry point.  Same shape as `test_lua_scripting`, and the same fix would serve both. |
| `test_connection` | "notepad_fixture.h drives `CTextDocument`'s `m_fn_*`" | **An access violation.**  207 tests pass, then `Tls.CertVerifyFailureBlocksAndRecordsReason` dies: `ConnectionClosed` calls `Frame.OnUpdateBtnWorlds`, which opens with `App.m_pWorldDocTemplate->GetFirstDocPosition()` — and that pointer is NULL because `InitInstance` never ran. |
| `test_lua_surface` | "notepad_fixture.h, as above" | Three failures in the POSIX-shaped and sound arms (`ChangeDirRoundTrip`, `SoundFamilyHeadless`, `UtilityMiscArms`), then an access violation in `LuaSurface.UdpFamily`. |
| `test_mxp` | "notepad_fixture.h, as above; also includes `<malloc/malloc.h>`" | **345 passes, 0 failures, then an access violation.**  Any MXP error or debug message reaches `AppendToTheNotepad`, and `doc.cpp:3043` walks `App.m_pNormalDocTemplate->GetFirstDocPosition()` -- the same NULL again. |

**The lesson, and it cost two build cycles to learn twice: a recorded exclusion
reason that names one blocker is not a promise that there is only one.**  Both
notes were written from the first compile error the author saw, which is the
same trap as reading a build failure off the first error-looking line (§10l).
Every reason in the generator now says what was *measured*, not what was first
noticed.

**The next move is two objects.**  Three of those four rows are the SAME defect:
`App.m_pWorldDocTemplate` and `App.m_pNormalDocTemplate` are NULL because
`InitInstance` never ran, and two engine sites walk them without checking --
`mainfrm.cpp:687` (world) and `doc.cpp:3043` (notepad).  `test_accelerators`
(§10k) is a fourth: its `eBadParameter` comes from the same world template.
Worth noting on its own that `doc.cpp:3043`'s twin at
`notepad_app_shared.cpp:115` DOES guard the same pointer -- two walkers of one
template, one careful and one not.  In the shipping app neither can be NULL
after InitInstance, so it is latent there rather than a live bug.

`platform/headless_app.cpp` answers this off Windows with two file-scope
`CMultiDocTemplate`s.  On Windows the class is real MFC and its constructor
stores runtime classes and loads a string resource without creating any window,
so the same answer should work.  That single change is what stands between this
target and roughly 400 more tests -- `test_connection` (154), `test_mxp` (131),
`test_accelerators` (11) and whatever of `test_lua_surface` is not sound- and
chdir-shaped -- and it is the obvious next task.  It was deliberately not
started in this pass: it is a host feature, not a guard, and it deserves a clean
start rather than being bolted on at the end of a long one.

**A finding that has nothing to do with the notepad.**  `test_connection.cpp`
had a helper returning a `CStyle` by value.  `CStyle` derives from `CObject`,
and **MFC's real `CObject` declares its copy constructor private with no
implementation** — a `CObject` is deliberately not copyable.  The port's shim
(`platform/mfc_object.h`) declares no such thing and so gets an implicit copy
constructor, which is why the helper compiled on macOS for months and does not
compile on Windows at all.

That generalises past this one helper: **any port-authored code that copies a
`CObject` subclass builds on macOS and breaks the Windows build**, and nothing
but a Windows build finds it.  The helper is now fill-in-place; the shim is
*not* changed, because deleting its copy constructor could break the macOS
build in ways that cannot be tested from here.  It is a good candidate for the
macOS side: make the shim match MFC and see what falls out.

**Three smaller things fixed along the way.**  `getcwd` joined
`tests/posix_compat.h` (`_getcwd`).  `live_heap_blocks()` in `test_mxp.cpp` had
an Apple arm and a glibc arm and now has a Windows one over the debug CRT's own
accounting — with a comment noting the three are *not* interchangeable, since
the glibc arm returns bytes where the other two return counts.  And `zlib\` and
`png\` are on the test project's include path, because two test files include
`<zlib.h>` / `<png.h>` and the app reaches those through per-file settings this
project does not inherit; both directories were checked first for a header that
would mask a CRT or SDK one, which is exactly how `platform\direct.h` once
turned a 36-error build into 36,810.

### 10n. The document templates: one of the three predictions held

```
TEST() declared: 697    664 passed, 0 failed    exit 0
```

The host now builds `App.m_pNormalDocTemplate` and `App.m_pWorldDocTemplate`,
the two of InitInstance's four that code with nothing to do with windows walks.
Constructing a `CMultiDocTemplate` creates no window — it stores three runtime
classes and loads a string resource — and `AddDocTemplate` is deliberately not
called, because that hands the template to the app's document manager and makes
File ▸ New and the OLE server reachable, which this host has no business doing.

**§10m predicted this would unblock roughly 400 tests. It unblocked 103.**  The
measurement is worth more than the prediction, so here is what each of the three
actually did:

| Predicted | Measured |
|---|---|
| `test_mxp` (131) | **Right.**  The access violation is gone; `doc.cpp:3043` now walks a real, empty list.  103 of its tests run. |
| `test_accelerators` (17) | **Wrong, and §10k already said so.**  All 11 failures are unchanged.  `AcceleratorTo` returns `eBadParameter` at `GetFirstViewPosition()`, and a view is a *window*, not a registry entry.  §10m read "two NULL pointers" as "one fix" and generalised past its own evidence. |
| `test_connection` (154) | **Wrong for a second reason.**  Still an access violation at the same place, and the template was never the cause: `CMainFrame::OnUpdateBtnWorlds` walks the template *and then* calls `m_wndActivityToolBar.GetButtonInfo` unconditionally (`mainfrm.cpp:712`) on a `CToolBar` that only exists after `CMainFrame::OnCreate`.  This one needs a created main frame, which a console host cannot have. |

The general form of the mistake: **two symptoms with the same-looking NULL are
not the same defect**, and the way to tell is to fix one and re-measure rather
than to reason from the shape.  That is the settings audit's lesson for the
third time in this document — a shared symptom is not evidence of a shared
cause — arrived at from the opposite direction, by assuming a shared cause and
being shown two.

**And the guarding in `test_mxp` is the reverse of what it looks like.**  MXP
writes its errors and debug output to a notepad, so on Windows a test that
produces debug output performs real MDI window creation:
`CMDIChildWnd::Create` looks up `AfxGetThread()->m_pMainWnd` and `ENSURE_VALID`s
it (`winmdi.cpp:490`).  So two disjoint sets are guarded there, for opposite
reasons:

- the 16 cases that install `MxpDebugCapture`, because `NotepadCapture` hooks
  `g_wxhook_create_text_window` and that hook exists only off Windows;
- the 12 that produce debug output **without** installing a capture, because
  nothing intercepts the creation and the framework really tries.

Intercepting the notepad is what makes a test safe, so it was the tests
*without* the harness that broke — which is not the half anyone would guard
first.

Those 12 also show the runner change from §10l earning its keep a second time:
`ENSURE_VALID` throws, so what was an access violation before the template
became twelve clean `[FAIL]` lines naming themselves, in a run that reached its
own summary.

**The count reconciles exactly.**  697 `TEST(` declared across the 16 enabled
files, 33 guarded off Windows — 28 in `test_mxp`, 2 each in `test_timers` and
`test_triggers`, 1 in `test_aliases`, and 0 in `test_telnet`, whose one guard is
*inside* a test body rather than around it — and 664 executed.  Nothing
disappeared silently, which is the only reason the number is worth quoting.

### 10o. `44f0737` — the biggest **RUN** row, mostly answered without a walk

§9c marked `44f0737` **RUN** and said to walk every world-prefs page by hand:
"the largest dialog change in the list", eight defects, 79 lines in
`prefspropertypages.cpp` alone.  §10h then reported it not reached.

Most of it did not need a walk.  The commit says its five families are
reachable only from inside the running app, "because configuration.cpp,
prefspropertypages.cpp and GlobalPrefs.cpp are all `mushgui_adapters` TUs" —
and that is true **on macOS**.  On Windows all three are ordinary members of
the 286 engine sources this target already compiles, so four of the five are an
ordinary unit test.  `tests/win/test_prefs_pages.cpp` is that test, and it
lives under `tests/win/` precisely because the macOS executables cannot link
what it drives.

```
669 passed, 0 failed
[PASS] PrefsP8.TriggerMultiLineIsAChange
[PASS] PrefsChange.DefaultColoursAreNotASpuriousChange
[PASS] PrefsP16.TimerListSortsByContents
[PASS] AdjustWidth.WithNoOutputWindowNothingIsInvented
[PASS] PrefsP9.CommandColoursSurviveAPageChange
```

**All five passing first time is exactly when to distrust them**, so three were
checked the way the original commit checked its own: by reverting the fix
underneath and confirming the test goes red.

| Fix reverted | Test |
|---|---|
| the `bMultiLine` term in `CPrefsP8::CheckIfChanged` | `PrefsP8` → **FAIL** ✓ |
| the `eColumnContents` case in `CPrefsP16::CompareObjects` | `PrefsP16` → **FAIL** ✓ |
| `ChangedPrefsP9`'s encoding comparison | `PrefsChange` → **FAIL** ✓ |

The third one took two attempts, and the first attempt is the instructive part.
Reverting it as

```cpp
(m_echo_colour == SAMECOLOUR ? SAMECOLOUR : m_echo_colour) != (unsigned short)(page9.m_echo_colour - 1)
```

left the test **passing** — because that cast makes `-1` wrap to 65535 and
therefore *fixes* the very bug it was meant to reintroduce.  The real original
is `m_echo_colour != page9.m_echo_colour - 1`, where the `unsigned short` is
promoted to `int` and compared against a genuine `-1`.  **A revert-check has to
reproduce the actual defect, not something the same shape**; a plausible-looking
variant that quietly repairs it reports the test as vacuous when it is not.

**What still needs the GUI, and it is now three things rather than eight:**

- `CGlobalPrefsP4::OnFont` seeding the printer-font picker from the page rather
  than from `App`.  The port substitutes `g_wxhook_font_dialog`; on Windows
  `OnFont` opens a real modal `CFontDialog`, and a test host must never do that
  — §10k's first lesson was a run wedged behind an invisible modal box.
- The fitted last-column width on the alias/timer/variable pages, and
  `LoadPrefsP7`'s wrong-enum indexing.  Both need a created `CListCtrl`.
- The end-to-end confirmation §9c actually asked for: edit one field per page,
  OK, reopen, confirm it stuck; then OK with no edit and confirm the world is
  not marked modified.  The unit tests pin the arithmetic each of those rests
  on, which is the part that was wrong — but they do not press OK.

**Two of the five are weaker than they look, and say so at their sites.**
`AdjustWidth` can only assert that a host with no `CMUSHView` gets 0 and that
nothing is invented from a window that is not there — the port's half of that
fix is in `tests/test_mfc_ui.cpp`, which does not compile here.  And
`PrefsP9`'s `OnKillActive` chains into real MFC's `CPropertyPage::OnKillActive`,
which calls `UpdateData` on a page with no window; the two assignments the fix
is about have already happened by then, so the test is sound, but it is
deliberately the last in the file so that if that chained call ever becomes more
than an assertion on stderr, this is the test that says so first.

### 10p. §10h's leak rows: four were already answered, three more now are

§10h lists seven leak / use-after-free fixes and says "none was exercised".
That was true when it was written and had quietly stopped being true, because
**four of the seven are covered by tests in suites this target compiles**, and
those tests have been passing in every run since:

| Fix | Test | |
|---|---|---|
| `0b70de7` recall leaks its compiled regexp | `RegexpTest.RecallTextDoesNotLeakItsCompiledRegexp` | PASS |
| `41eb7bd` failed database open leaks the handle | `DatabaseTest.OpenFailureClosesTheHandleSqliteAllocated` | PASS |
| `5ee518e` malformed MXP tag leaks its argument list | `MxpR4.CustomElementArgumentAbortDoesNotLeakTheBuiltList` | PASS |
| `0dae192` three more MXP early returns | `MXPDefs.RedefinitionEarlyReturnsDoNotLeakTheArgumentList`, `MxpR4.OpenTagVetoDoesNotLeakTheArgumentList` | PASS |

The lesson is small and boring and worth writing down: **an owed-work list and a
test list drift apart silently.**  Nothing announced that re-entering
`test_mxp` had answered two rows of §10h; the connection had to be looked for.

The other three had no Windows coverage because their tests live in
`test_world_load` and `test_lua_scripting`, both excluded for reasons that have
nothing to do with the fix under test — an artefact of where an assertion
happens to live, not of what the Windows build can reach.
`tests/win/test_gap_coverage.cpp` closes that by driving the same engine code
through `world.ImportXML`, which reaches `Load_World_XML` with `XML_OVERWRITE`
set — the same arm `LoadWorldFromFile` reaches.  All three pass; **672 passed,
0 failed.**

**And one of them was nearly written up as a new bug.**  The first version of
the `ImportXML` test measured live heap blocks across twenty malformed imports
and found **246 blocks per batch, twice running** — 12.3 per call, perfectly
steady, which is exactly the signature of a per-call leak, in a path whose
exception leak `fbc4161` had supposedly fixed.

It is not a leak.  `_CrtMemDumpAllObjectsSince` named the blocks: 80-byte and
20-byte `CObject`s with `plex.cpp` map blocks and a string reading
`"Line    1: Unexp…"`.  Those are `CLine` and `CStyle`.  A failed import is not
silent — `LoadError` writes `[WARNING] <file>` and the detail into the world's
scrollback through `ColourNote` — so a document that survives the loop
accumulates two output lines and their styles per call, **by design**.

Two things to carry out of that:

- **The instrument was right and the interpretation was wrong.**  "Steady growth
  per call" is necessary for a leak and nowhere near sufficient; the object dump
  is what distinguishes a leak from a buffer doing its job, and it cost one
  build to get an answer that no amount of re-reading the code would have given.
- The corrected test constructs the document **inside** the loop, so its output
  dies with it and anything still allocated afterwards is allocated outside the
  document — which is what `fbc4161` is actually about.  That is now written at
  the test, because the obvious "simplification" is to hoist the document out of
  the loop, and doing so would make the test fail for a reason that is not a
  defect.

### 10q. The MXP atomic-tag veto (`3095c2a`) — and two more rows already answered

§10h says this one "needs a plugin implementing `OnPluginMXPopenTag`; the
plugin was not written."  **No plugin is needed.**  The veto is equally
reachable from a world's own `OnMXP_OpenTag` script routine, and `test_mxp.cpp`
has been installing one a few tests above since `0dae192`.  The barrier was in
the note, not in the code.

Two checks of the six are now tests, both revert-checked by disabling the
`if (bVetoed)` unwind in `mxpStart.cpp` and confirming they go red:

| Check | Test |
|---|---|
| the active-tag record itself — the fix's subject | `MxpVetoR5.VetoedBuiltInTagLeavesNoActiveTagEntry` |
| the spurious `StartNewLine` from `</p>` finding the suppressed `<p>` | `MxpVetoR5.VetoedParagraphCloseDoesNotBreakTheLine` |

The **built-in vs custom** distinction is what makes these worth having, and it
is why the veto test that was already there does not cover this:
`MxpR4.OpenTagVetoDoesNotLeakTheArgumentList` vetoes a *custom* `<mytag>`,
which returns before any bookkeeping exists — the path that always worked.
`MXP_StartTag` records a built-in tag as open *before* calling
`MXP_OpenAtomicTag`, which is where the veto is asked, and it cannot be asked
earlier because the argument list handed to the handler is only rebuilt inside
`MXP_OpenAtomicTag`.

The first test puts its control **first**, deliberately: an un-vetoed `<b>`
really does record itself, so a gutted "always empty" would not pass.  The
second compares the vetoed line count against the un-vetoed one rather than
against a literal, so it does not encode how many lines `<p>` makes.

CORRECTION (see section 10w): this paragraph was wrong.  `4f4b667` had already
written all six as `MxpD20.*`, and all eight of those have been green here since
`test_mxp.cpp` was enabled.  What was genuinely missing was the PLUGIN route --
`OnPluginMXPopenTag` -- which is now covered too, and whose control turned up
`docs/UPSTREAM-BUGS.md` 90.

**And two more §10h rows were already answered**, found the same way as §10p's
four — by checking where each fix's test actually lives rather than trusting
the note:

- `30df30b`, the Ctrl+click alias menu's own `%1` —
  `AliasMenu.ExecuteSubstitutesWordUnderMenuIntoPercentOne`, in
  `test_aliases.cpp`, passing here since that suite was enabled.
- `0dae192`'s second test, as recorded in §10p.

That is now **three** separate occasions in this document where an owed-work
list had drifted from the test list without anything saying so.  The check
takes one grep per row — `git show <commit> --stat | grep tests/` — and it is
worth doing before writing any new test, because two of the four rows attempted
this way turned out to need nothing at all.

### 10r. `test_plugins.cpp` was never undiagnosed — it was diagnosed wrongly

The exclusion reason read: *"UNDIAGNOSED — the file preprocesses oddly under
MSVC (a `#define` at :125 reads as undeclared at :1736)."*  Seventy-nine tests
sat out on the strength of it.

There is no preprocessor oddity, and `IDA` at :125 is not involved.  Compiling
the file takes one build and gives five ordinary causes, four of them already
familiar from the suites re-entered in §10l and §10m:

| Cause | Sites |
|---|---|
| `mkdir(path, mode)` — Windows `_mkdir` takes one argument | 4 |
| a `BSTR` in a `? :` against `""` — no common type when `BSTR` is `wchar_t*` | 3 |
| `g_wxhook_directorypicker`, port-only | 1 test |
| the plugin state folder | 4 tests (below) |
| **`const double small = …`** | 1 |

That last one is the whole mystery.  The Windows SDK's `rpcndr.h` contains

```c
#define small char
```

so `const double small = run(400);` becomes `const double char = …`, and MSVC
reports **"'double' followed by 'char' is illegal"** — an error that names
neither the macro nor the header nor the variable.  An SDK macro silently
changing what an identifier means is *exactly* what the old note was trying to
describe; it simply attached the description to the wrong identifier and the
wrong line, and once written down it was never re-checked.  **"Undiagnosed" in a
note means "nobody has looked recently", not "this cannot be diagnosed."**

**The state folder is the interesting half, and it is a real divergence.**
`plugins.cpp` auto-creates a missing plugin-state folder — `mkdir -p`, with a
comment saying it is there "rather than blocking with a modal on a fresh setup"
— and that whole block is `#ifndef _WIN32`.  On Windows the upstream behaviour
stands: warn, then open a real `::SHBrowseForFolder`.

That is a **third** way to raise a modal box, and unlike the other two it cannot
be intercepted at all: `CMUSHclientApp::DoMessageBox` catches `AfxMessageBox`
and `UMessageBox` has its own arm (§10k), but `SHBrowseForFolder` is a Shell
dialog, not a message box.  The run stops dead behind an invisible window — the
same failure mode as §10j's first wedged run, reached a different way.  Four
tests touch it: two now create their folder as their siblings already did, and
two are guarded because they assert the port's non-blocking behaviour.

**Where it stands.**  The file compiles and **420 tests run**, then the process
exits without printing its summary — a crash rather than a block, since the
runner's `WaitForExit` returns on its own.  That is one more thing to chase, and
it is left for its own pass; the fixes above are committed either way, so the
next attempt starts from 420 rather than from zero.  The exclusion reason now
records all of that instead of the word "UNDIAGNOSED".

**Two tests were fixed rather than guarded, and the distinction matters.**
`ReentrantSaveStateRejected` and two others never created the state folder they
named — the other state tests in the same file always did.  On the port the
absence is benign, so the omission was invisible; the folder was never part of
what any of those tests was about.  Creating it is a fix, not an accommodation.
Guarding is reserved for the two that genuinely assert behaviour Windows does
not have.

### 10s. `test_utilities` — the note was right, and still incomplete

Unlike §10r, this exclusion reason was **accurate**: "the `g_wxhook_*sound*` seam
and `AllocateLinePositions`, both port-only. ~10 test cases; guarding those
would bring the rest of a large suite back."  Both named causes are exactly what
the compiler reported.  It was also incomplete in two ways worth recording,
because the same two shapes will recur.

**First, one of the two is not a seam at all.**  Seven tests use
`AllocateLinePositions`, and it is tempting to file that with the wx hooks as
"port-only plumbing in the way of shared behaviour".  It is not: that function
**is** the port's unbounded-scrollback mechanism, the one that raised the cap
from Windows' 500,000 lines to 100,000,000.  `UnlimitedScrollbackNeverEvicts`
and its siblings are testing a feature Windows does not have.  Guarding them is
the correct answer rather than a compromise, and the distinction matters for
whoever reads the guard later: **a guard on port-only plumbing is owed work; a
guard on a port-only feature is finished work.**

The eighth use was not in a test at all but in `make_buffer_doc`, a helper 13
tests share, where it applies `m_maxlines = 0`.  Guarding just that one line
keeps all 13 — for `m_maxlines == 0` the function's job is "trim nothing",
which is also what Windows does by not having it.  Guarding the helper's
*callers* would have cost 13 tests to save one line.

**Second, a third blocker the note did not mention** — and it only appears at
**link** time, so no amount of reading finds it: five `g_ShowFunctionslist*`
globals that `platform/doc_headless_stubs.cpp` defines to record what the
Function List was asked for.  On Windows `ShowFunctionslist` is the real modal
dialog, so there is nothing to record.  That test is §10h's `2e54bba` row, and
it stays owed: it needs a Windows-side observable, not a guard.

**Where it stands.**  The file now compiles and links.  What stops it is a crash
**later in the run**, in MXP tests that pass when this file is absent — so the
suspect is global state `test_utilities` leaves behind, not anything it asserts.
That is its own hunt and is left for its own pass; the fixes are committed, and
the exclusion reason records all of the above.

**Two suites, two stopping points, one rule.**  §10r and this section both end
the same way: the compile problems are solved and committed, a runtime problem
remains, and the suite stays excluded with a reason that says exactly that.
Leaving a suite enabled-but-crashing would take the whole target red for one
file; leaving the reason as it was would waste the diagnosis.  **The unit of
progress is the recorded reason, not the enabled flag.**

### 10t. Six suites, one wall: the combined runner's stated trade, now due

`test_plugins`, `test_utilities`, `test_display`, `test_info`, `test_mcdatetime`
and `test_scriptengine` have all now been made to **compile and link** on
Windows.  Between them that is roughly **370 tests**, and none of them is
enabled, because every one of them ends the same way: a handful of small
portability fixes, a clean build, and then the process **dies partway through
the run** — usually mid-`printf`, in a suite that passes when the file is
absent.

Three attempts is a pattern rather than three coincidences, and the pattern was
predicted in writing.  `tests/win/all_tests.cpp` says of the one-executable
design:

> The trade is that all the suites share one process. … it does mean one suite
> that calls abort() takes the rest down with it, and that suites are no longer
> isolated from each other's global state.  Both are acceptable for a target
> whose purpose is to exercise the Windows arms of shared code; **if a suite
> ever needs isolation, give it its own vcxproj.**

**That trade has come due.**  The suites that fail are exactly the ones that
manipulate process-wide state — `App.*` globals, document templates, the
`m_strDefaultStateFilesDirectory`, hook pointers — and the crash lands in a
*later, unrelated* suite.  On macOS this cannot happen: each suite is its own
executable, so a leaked global dies with the process that leaked it.

**So the next move is architectural, not another guard.**  Two options, and the
choice wants a decision rather than a default:

1. **Per-suite isolation** — a second vcxproj, or one binary invoked with a
   `--suite=` filter and run once per suite by `run_tests.ps1`.  Restores the
   macOS property exactly.  Costs link time (an MFC test binary is ~19 MB) or a
   process launch per suite.
2. **Find the global.** Cheaper if it is one thing, and the bisect is
   mechanical: enable one suite, note where it crashes, and use the fact that
   the crash is *always in a different suite from the one added*.

What is committed either way is the compile work: every one of the six now has
its Windows blockers fixed or guarded, each guard carrying a reason, so
whichever route is taken starts from a clean build rather than from a compiler
error.  The exclusion reasons say "COMPILES AND LINKS now" and point here.

**Two old reasons were wrong or incomplete, in the §10r shape.**

- `test_display` — "socketpair, for the chat-snoop test."  The compiler never
  reaches socketpair: `#include <unistd.h>` at line 20 stops it first.  The
  socketpair use is real but it is *one test*, and 135 others were held behind
  a missing include.
- `test_info` — "g_pWinApp … plus getcwd."  `getcwd` was fixed in §10p; there
  were five tests to guard, not two, and one of them is `CTimeShim`, which
  tests the port's `CTime` **by name**.

That last one is worth its own line, because it is the mirror of a known gap.
`CLAUDE.md`'s verification debt records that the port's `CTime` shim *lacks*
`operator-(const CTimeSpan&)`.  On Windows the problem is the opposite: MFC's
real `CTime` has **both** `operator-(CTimeSpan)` and `operator-(CTime)`, so the
`LONGLONG` from `GetTotalSeconds()` matches both and the call is **ambiguous**.
A shim can diverge by having too few overloads *or* too many, and only building
both sides finds either.

### 10u. One process per suite — and three ways a wait can lie

§10t ended with a choice: give the crashing suites isolation, or hunt the global
they share. **Isolation won**, and not because of this bug. It is the property
macOS already has and gets for free by building one executable per test file: a
suite that corrupts process state takes only itself down, and says so. Hunting
the global would have fixed one crash; isolation fixes the class, including the
ones not yet written.

The implementation is deliberately the *cheap* half of what
`tests/win/all_tests.cpp` proposed ("give it its own vcxproj"). A second vcxproj
means a second ~19 MB MFC link. Instead the one binary learned two flags:

```
mushclient_tests.exe --list-suites      # every TEST() suite name, one per line
mushclient_tests.exe --suite=NAME       # run only that suite
```

and `run_tests.ps1` launches it once per suite. The cost is one process start
per suite and no extra link; `run_all` gained a `const char*` and one `strcmp`.
The runner asks the **binary** for the list rather than keeping its own, because
a second list would drift and a suite missing from it would simply not run,
silently.

That part took twenty minutes. **Getting the runner to report anything at all
took the rest of the afternoon, and all three obstacles were the same shape: a
wait that looked like slowness.**

**1. `Start-Process -PassThru` returns a `Process` with no cached handle.**
`WaitForExit(ms)` and `ExitCode` both misbehave without one — the timeout
silently never fires. The run just sat there, indistinguishable from a slow
suite. The fix is one line, `$null = $j.Handle`, before the wait; nothing in
either signature hints at it. It also explains an empty `run exit=` seen earlier
in this campaign and shrugged off at the time.

**2. `--list-suites` hung, because it exited too early.** It printed its names
and returned from `main` *before* `MC_TEST_WIN_HOST_INIT`, which looked
obviously right: listing strings needs no MFC, so why pay for the init? Because
MFC's OLE teardown asserts in `oleunk.cpp:455` when `AfxOleInit` was never
called — and on this host an assertion is an **invisible modal dialog** (§10q).
The process printed nothing, exited never, and the runner waited on it forever.
Two runs were lost before the window enumeration from §10q was pointed at it and
read the caption back.

> **On this host an early return from `main` is a hang, not a shortcut.** Any
> flag added to `MC_TEST_MAIN_ALWAYS` must run the full host init and leave
> through the same exit as an ordinary run. That is now the comment above the
> macro, and both flags are handled after the init rather than before it.

**3. The list call had no clock**, because it was too trivial to need one — and
it was the thing that hung. Both callers now go through one `Invoke-TestExe`
helper that always takes a timeout. The helper also returns stdout and stderr
**separately**, which is not tidiness: MFC's leak report goes to stderr, and a
runner that concatenated the streams would read `0 bytes in 0 Free Blocks.` as a
suite name and launch a process for it. A shape filter on the names
(`^[A-Za-z_][A-Za-z0-9_]*$`) is the belt to that braces.

**The rule the three share:** a hang and a slow run are indistinguishable
without a clock, and each of these was a wait on something that would never
arrive. §10t gave the *suites* a timeout for exactly this reason and then let
the list call go unguarded — the ordinary way a lesson fails to generalise. It
gets applied to the instance that taught it.

**What it costs and what it bought.** The full run is ~7 minutes rather than
~40 seconds, almost all of it process start plus host init repeated 130 times.
That is the price of the property, and it is worth paying: the very first
isolated run took the target from **16 files / 674 passing** to **22 files /
1042 passing**, and every one of the 19 failures, 2 crashes and 1 hang named its
own suite instead of an innocent bystander thirty tests later.  Triaging those
(section 10v) finished at **1075 passing, 0 failing, across 128 suites**.

Three reporting states now exist, and the distinction is the point:

| state | meaning |
|---|---|
| `N passed, M failed` | the suite ran and disagreed with itself |
| `CRASHED (no summary)` | the process died before printing a summary |
| `BLOCKED (killed after Ns)` | the process was still alive when the clock ran out |

### 10v. What the six re-entered suites found

Isolation made the six suites of §10t runnable; running them produced 19
failures, 2 crashes and 1 hang. Every one is triaged below, because the split
between them is the interesting part: **two were real defects, one was a missing
host capability, one was a host capability that turned out to be expensive, and
the rest were tests that had quietly encoded the port's answer as *the* answer.**

#### The two real defects

**`ExtractDirectory` searched for a backslash only on Windows.** A plugin path
written with forward slashes therefore resolved `$PLUGINDIR` to the current
directory. Full write-up in `docs/UPSTREAM-BUGS.md` 89; the short version is
that the function's own caller (`xml_load_world.cpp`) tests a plugin filename
for **both** separators six lines before handing it to a function that knows
only one. Fixed by using the POSIX arm's search everywhere; the only thing left
under `#ifdef` is the no-separator fallback, which still names the platform's
own separator.

**Three tests were about to `delete` a stack object.** The pattern
`doc.m_pWorldView = &view;` … `doc.m_pWorldView = nullptr;` is harmless on the
port, where the member starts null and nothing owns it. On Windows the document
**constructs its own adapter** (`doc_construct.cpp`: `pal::CreateWinWorldView`)
and its destructor **deletes** it, so the same two lines do one of two wrong
things: nulling the pointer leaks the adapter, and leaving the fake in place
runs `delete` over a stack address. `test_info.cpp`'s
`MainClientRectComesFromTheViewNotTheCharacterGrid` did the second, and had
never fired only because the `GetInfo` crash described below killed the suite
several tests earlier. `mc_test.h` grew a `ScopedWorldView` guard that restores
the doc's own view rather than NULL; `test_info`, `test_commandbar` and
`test_tabcomplete` now use it.

> The general shape is worth keeping: **a test that pokes a raw member has to
> know who owns it, and ownership can differ per platform even when the member
> does not.** A shared test file is a place where two ownership models meet.

#### The host capability that was worth building

**Documents were never registered with the world template.** `world.GetWorld`,
`GetWorldList` and every cross-world call walk `App.m_pWorldDocTemplate`'s
document list and nothing else (`methods_worlds.cpp`). On Windows the framework
fills that list inside `CDocTemplate::OpenDocumentFile`; a bare
`CMUSHclientDoc doc;` never goes near it, so every world was invisible to every
other one. That reads as "the plugin callback did not fire", which is exactly
what `PluginGuards.ScreendrawGuardIsPerDocument` reported.
`doc_construct.cpp`'s `_WIN32 && MUSHCLIENT_TESTS` arm now calls `AddDocument`,
mirroring the headless arm a few lines above it. Symmetric for free:
`AddDocument` sets `m_pDocTemplate` and `~CDocument` removes it again.

#### The host capability that was not — and the measurement that said so

**There is no main frame, and `Frame` is a macro.** `stdafx.h` defines it as
`*((CMainFrame*)App.m_pMainWnd)`, so with `m_pMainWnd` NULL every
`Frame.something` in the engine is a **null dereference**, not a call that
quietly does nothing. `GetInfo` codes 251-260 take `&Frame.m_wndToolBar` and
friends; 249/250 get there one level down, through `GetMainClientRect` into the
seam adapter's `mainClientRect`, which is `Frame.GetClientRect`
(`pal/win_worldview.cpp`). They segfaulted the whole `GetInfo` suite from inside
a loop over ninety other codes — no assertion, nothing naming the culprit, just
a suite that stopped. Code 125 (`IsClientFullscreen` → `Frame.IsFullScreen()`)
is the same thing one family earlier.

The obvious fix looks cheap and is not. Constructing a `CMainFrame` **object**
without ever `Create`ing it should be free: the constructor sets scalars and
zeroes a `NOTIFYICONDATA`, the destructor is empty, and every window inside it is
created in `OnCreate`. Each embedded `CWnd` would then exist with
`m_hWnd == NULL`, and MFC's own guards would answer 0 — which is what Windows
reports for a bar that is not there.

**Measured: it took the target from 1053 passed / 1 failed to 1006 / 71.**
Plugin loading failed across the board, on an MFC assertion in `winocc.cpp:242`
followed by a `CArchive exception: badSchema`, and it dragged down suites with
no connection to frames at all — `Mapper`, `RegexpTest`, `TelnetTest`,
`MXPOpenAtomic`. It was reverted, undiagnosed, and the codes are excluded on
Windows instead with the measurement written beside them.

> Two things worth keeping. **A null `App.m_pMainWnd` is not "no frame", it is a
> landmine**, because `Frame` is a dereference rather than a pointer — every use
> site is undefined behaviour, and the one that fired did so from inside a loop
> that named ninety innocent codes. And **the cheap-looking host capability was
> the expensive one**: registering documents with a template was five lines and
> fixed four tests; giving the host a frame object was three lines and cost
> seventy. Neither cost was predictable from the size of the change, which is
> the argument for measuring each one on its own rather than landing them
> together.

Tests that need a *created* toolbar or a *created* view remain owed (§10m), and
a main frame is now on that list rather than off it.

#### The rest: tests that encoded the port's answer as the answer

Five causes, and each guard now carries its reason:

- **Note colours start in different modes.** `m_bNotesInRGB` is `true` on the
  port by deliberate design (headless has no palette to resolve an index
  against) and `false` from the Windows constructor. Three tests in
  `test_display.cpp` read the doc's colours, called `ColourTell`/`AnsiNote`, and
  asserted they were restored — but restoring is *conditional on the mode*, so
  each was testing whichever branch the platform happened to pick. Fixed by
  saying which mode is meant (`NoteColourRGB`) rather than guarding: the tests
  are stronger on both platforms now.
- **Logs are CRLF here.** The log file is opened in text mode (`fopen "w"` /
  `"a+"`), so the CRT expands every LF. That is a recorded, deliberate parity
  gap; the helper that reads the file back now collapses the pair, which keeps
  every assertion about content rather than encoding and still catches a doubled
  line break, because the substitution is one-for-one.
- **`TipsFileName`'s suffix is platform-specific, and the test hard-coded the
  Mac one.** Rewritten against a per-platform tail. Its length assertion is the
  more interesting half: 250 characters plus `/../Resources/tips.txt` clears
  `MAX_PATH`, but plus `\tips.txt` is 259 and does not. **A test whose point is
  "longer than the platform's constant" must not pick a length close to it** —
  the assertion had quietly stopped meaning anything.
- **`CScriptEngine::Execute`'s DISPPARAMS entry is the port's path for Lua.**
  Windows checks `if (L)` first and returns through `ExecuteLua` with empty
  parameter lists, never reaching the `Invoke` that carries `rgvarg`. That is
  not a defect: every caller in the product asks `UsesEmbeddedCallback()` first
  and routes Lua elsewhere (`plugins.cpp` ×6, `CallPlugin`). The port needs the
  marshalling because five *more* languages are embedded rather than
  COM-hosted. Three tests guarded, with that reasoning written down — the naive
  reading, "Windows hands rgvarg to Invoke, so it must work", is true and still
  gets the answer wrong.
- **The rest asserted port-only behaviour outright**: the whole
  `PluginDispatch` family (a "vbscript" plugin is a real COM language on
  Windows, and with `m_pDispatch` NULL the first test took the process down
  before it could print), the unsupported-language message (VBScript *is*
  supported on Windows), the headless plugin wizard (`DoModal` degrades to
  `IDCANCEL` only on the port), the inert headless preferences database, and
  `SetScroll`'s scrollbar flag — whose Windows arm does record it, but from
  inside the loop over the document's views, so a document with no view records
  nothing.

#### One modal with no way out

`PluginState.StateFolderIsAFileFails` points the state folder at a file. Windows
answers with `AfxMessageBox` (which this host intercepts) and then
`::SHBrowseForFolder` — a **Shell** dialog, not a message box, with no hook and
no window a test can find or close. It is the one modal in this tree that
nothing can answer, and it hung the suite until the runner's clock killed it.
Without §10u's timeout it would have hung the whole run instead.

`plugins.cpp` now skips that browse under `_WIN32 && MUSHCLIENT_TESTS`. This is
not a fabricated outcome: a NULL `pidl` is exactly what pressing Cancel
produces, and the code below already handles it by leaving the folder alone and
letting the save fail — which is the arm the test asserts. Skipping the dialog
keeps the assertion running on Windows instead of guarding it away.

### 10w. §10h, re-checked row by row — and the bug the last row was hiding

§10q recorded a rule and then did not apply it to the rest of the list: *check
where a fix's test actually lives before writing a new one*, one grep per row,
`git show <commit> --stat | grep tests/`. Running it over all of §10h's
remaining rows takes about a minute and moves five of them.

**Answered, by tests that are live and green on this target:**

| §10h row | test |
|---|---|
| `5359254` the 52 macro command ids | `MacroCommandIds.EveryMacroCommandIdMapsToItsOwnSlot` |
| `8468a76` the auto-say tri-state | `AutoSayTest.RefusedWhenNotConnectedIsDistinctFromSent` |
| `43c4545` the plugin wizard's re-sort | `WizardTest.RemovingSelectedItemsSortsOncePerCategory` |
| `c3936f4` `m_iNoteColourFore` on a deferred note | `NotingTest.DeferredSameColourNoteCarriesConstructedNoteColours` (+ the palette twin) |
| `3f3e8ac` the plugin-veto uninitialised return | `PluginCallback.RuntimeErrorLatchesCallbackOff` |

Every one of these was listed as needing typed input, a menu click or a plugin.
None of them does. Three became reachable only because §10u let
`test_plugins.cpp` and `test_utilities.cpp` into the target at all; the other
two were reachable all along.

**Answered on macOS, and correctly NOT on Windows** — recorded so they are not
re-checked a third time. Each sits inside an `#ifndef _WIN32` block whose reason
is at the guard:

| §10h row | test | why it is port-only |
|---|---|---|
| `2e54bba` `world.Help` / Function List | `Utilities.HelpPassesBLuaAndZeroOffsets…` | the `g_ShowFunctionslist*` observation globals exist only in the port's headless stubs; on Windows this is a real modal |
| `6e67cb0` background-world status attribution | `TimerTest.ReconnectStatusIsAttributedToTheWorldThatDropped` | drives the port's status hook |
| `bc4d149` the split Stop-Sound commands | `Sounds.TheTwoHalvesOfStopSoundPlayingActSeparately` | drives the `g_wxhook_sound_*` family |

**Not applicable to Windows at all:** `8dd87e5`, the background world's status
text. It reads as a shared-code fix because it touches `doc_shared.cpp`, but all
three of its hunks there are `#ifdef _WIN32` / `#else` pairs whose Windows arm is
byte-for-byte the original `Frame.SetStatusMessage (str)`; everything else it
touches is under `platform/`. Windows has one app-scoped status bar, where these
strings are unambiguous, and the attribution exists precisely because the port
gives each world its own. Its coverage is `world_frame_selftest.cpp`, which is
where it belongs. **Close the row rather than carrying it.**

**Genuinely owed, with no test anywhere:** `aca2478`, the Plugins dialog reading
a row's stale `CPlugin*`. That one IS in shared dialog code and does apply here,
but reaching `OnUpdateNeedDescription` wants the dialog and its list control.

**`9139076` was owed and is now done** — see §10x.

**And the MXP atomic-tag veto row (`3095c2a`) was stale twice over.** §10h said
four of its six checks remained; §10q repeated it. In fact `4f4b667` had already
written **all six** as `MxpD20.*` — including the outstanding-tag warning marker,
the close-tag callback and the `<send>`-inside-`<a>` substitution — and all eight
of them have been green here since `test_mxp.cpp` was enabled.

What was genuinely missing was the **plugin route**. All eight drive the world's
own `OnMXP_OpenTag`; a plugin vetoes through a different call
(`SendToAllPluginCallbacks(ON_PLUGIN_MXP_OPENTAG, …, bStopOnFalse)`) and with the
opposite polarity — a world handler vetoes by returning **true**, a plugin by
returning **false**. Two tests now cover it, and the second pins something
neither side can observe alone: the plugin broadcast is asked **first** and
short-circuits the world's handler entirely.

> **Writing the missing test is what found the bug.** Building the control for
> it — "the same two handlers, with the plugin declining to veto" — meant writing
> a world handler that returns nothing, which no existing test had ever done.
> Every test of these callbacks returns an explicit boolean. The control failed,
> and `docs/UPSTREAM-BUGS.md` 90 is why: a Lua `OnMXP_OpenTag` that falls off the
> end suppresses **every** tag, and a Lua `OnMXP_Error` that falls off the end
> silences every plugin's error callback along with the display. The COM arms of
> both functions get this right, because a handler that returns nothing leaves
> them a `VT_EMPTY` they can recognise.
>
> The general form is worth keeping: **a control that has never been written is
> a case that has never been run.** The controls in this suite are usually the
> same call with one flag flipped; this one needed a handler shaped differently
> from any that existed, and that is exactly why it found something.

The same commit closes the third instance of a defect that has now been fixed
twice and missed twice: `a39785e` initialised the result flag at three of
`ExecutePluginScript`'s overloads, `3f3e8ac` did the fourth, and both passed over
`mxpStart.cpp` and `mxpError.cpp`, where the identical bare `bool result;` was
read after `ExecuteLua`'s four early returns.

### 10x. The names file: the first row only Windows can close

`9139076` fixed a crash — `ReadNames` checked that each of `[start]`,
`[middle]`, `[end]` and `[stop]` was **present** but never that anything was
collected between them, so a file whose markers are adjacent set
`bNamesRead = true` with an empty `CStringArray`, and `GenerateName` then
evaluated `rand () % FirstNames.GetSize ()`. Division by zero, followed by a
subscript of an empty array. `world.ReadNamesFile` answered `eOK` for the file
that caused it, which is what made the crash arrive later and somewhere else.
It is `docs/UPSTREAM-BUGS.md` 42.

Its own commit message closed with:

> No headless coverage: `NameGeneration.cpp` is a `mushgui_adapters` TU, and the
> test executables link `platform/windows_stubs.cpp`'s `ReadNames`, which always
> throws — a test written against that stub would pass with and without this
> change. Wants a GUI (or **Windows**) run against a hand-made names file.

**That is the first row in this whole campaign that Windows can close and macOS
structurally cannot.** Everything else the Windows target has found so far was
either a shared-code defect the port happened to get right, or a test whose home
suite was excluded. This one is different in kind: the macOS test executables
cannot reach the code at all, because the real TU is not in `mushcore`, and the
stub they get instead has the opposite behaviour from every input. A test there
would be green either way — the worst kind of green.

Three tests, in `tests/win/test_gap_coverage.cpp` because they are Windows-only
by necessity rather than by exclusion:

| test | what it pins |
|---|---|
| `NamesFileR5.WellFormedFileIsAcceptedAndGeneratesAName` | the control, first: a good file is accepted, and twenty draws all match one-of-each-section |
| `NamesFileR5.AnEmptySectionIsRefusedAndLeavesNamesUnread` | all three empty-section shapes, each preceded by a known-good read |
| `NamesFileR5.GenerateNameAfterABadFileAnswersNullInsteadOfDividingByZero` | the consequence: `VT_NULL`, not a fault |

**Revert-checked**, by deleting the three `GetSize () == 0` checks and
rebuilding: the control stays green, and the other two go red on the
*precondition* — `ReadNamesFile` answers `eOK` (0) where `eCouldNotOpenFile`
(30013) belongs, and `bNamesRead` comes back true — for all three sections.

Two details worth keeping:

- **Each empty-section case starts from a known-good read.** `bNamesRead`,
  `FirstNames` and friends are file-scope statics, so "the flag is false" is
  only evidence if this file is what made it false. Seeding it true first is
  what turns the assertion from an observation into a measurement.
- **The test deliberately stops at the precondition rather than at the fault.**
  `ASSERT_FALSE (bNamesRead)` aborts the body before `GenerateName` when the
  flag is wrong, so the reverted build reports a failure rather than taking the
  process down. Per-suite isolation (§10u) means a crash there would only cost
  this one suite — but a named failing expectation says more than
  `CRASHED (no summary)`, and it says it in the same place either way.

The doc method's own `if (!bNamesRead) return VT_NULL` is **not** the fix and
never was: with the flag wrongly true it does not fire. Worth stating because it
is the guard a reader notices first, and mistaking it for the fix would make all
three tests look redundant.

### 10y. The two biggest excluded suites, unblocked but not yet entered

> **Superseded by §10z**, which enters both (and `test_connection.cpp` with
> them). Everything below is the state as measured before that pass; the
> "why they are still excluded" section at the end no longer holds.

`test_world_load.cpp` (105 tests) and `test_lua_scripting.cpp` (174) were
excluded for the same stated reason, and the generator said so: "Same shape as
test_lua_scripting, and the same fix would serve both." Both were blocked on
`CMUSHclientDoc::LoadWorldFromFile`, which `doc.h` declares `#ifndef _WIN32`.

**The recorded reason was accurate for one of them and badly overstated for the
other** — the §10r/§10s pattern again, now for the fourth time.
`test_world_load.cpp` uses `LoadWorldFromFile` 105 times, once per test: that
suite genuinely *is* the entry point. `test_lua_scripting.cpp` uses it **twice
in 5,028 lines**. A reason that reads identically for both hides a hundredfold
difference in how much it actually explains.

#### What the entry point is, and what it is not

The port's `LoadWorldFromFile` (`platform/doc_port_glue.cpp`) transcribes
`CMUSHclientDoc::Serialize`'s loading branch, because the port has no
CArchive-driven document framework to reach it through. Its own comment calls
itself "the test suite's canonical loader **until the Stage-7 Serialize
rewire**".

On Windows that rewire is available today, so the host does not get a second
copy of the body. It gets a dozen-line **driver** (`tests/win/all_tests.cpp`,
declared in `doc.h` under `_WIN32 && MUSHCLIENT_TESTS`) that opens the file,
hands `Serialize` a load archive, and then runs the OpenSession scripting steps
`doc.cpp` performs once a world is open. The loading is the shipping loader.

One engine line had to move: `Serialize` calls
`Frame.SetStatusMessageNow (strMsg)`, and `Frame` is
`*((CMainFrame*)App.m_pMainWnd)` — a null dereference in a host with no main
frame (§10v). It is now `#ifndef MUSHCLIENT_TESTS`. The message is still built;
nothing below reads it.

Three portability fixes went with it, all in `test_lua_scripting.cpp`:
`<utime.h>` is `<sys/utime.h>` here (and `utime`/`utimbuf` wear underscores, now
aliased in `tests/posix_compat.h`), and two `CString::c_str ()` calls became
`(LPCTSTR)` — `c_str` is the port shim's addition, not MFC's. Two tests are
guarded: one drives `g_wxhook_message_box`, the port's seam for the reload
prompt, and one reads `CArchiveException::s_iLiveCount`, an instrument the
port's shim adds so a leaked exception can be counted at all.

#### Measured, with both suites in

**1,130 passing across 141 suites** — up from 1,082 — with:

| suite | state |
|---|---|
| `WorldLoad`, `WorldLoadR4` | **BLOCKED** at the 60 s clock |
| `LuaScripting`, `SweepR4`, `LuaR5` | **CRASHED** |
| 10 individual tests | failed |

The failures are `LuaScripting.WorldVersion`,
`ProgressDialogLibraryIsRegistered`, `GetLineInfoTimeUsesLockedLocalFormat`,
`PaletteAccessorsRoundTripAndRejectOutOfRange`, `Utf8DecodeBadStringErrors`,
`Utf8EncodeTableNonNumberErrors`, `LuaMarshal.SaveWritesWorldFile`,
`SpellCheckTest.SpellCheckCommandAppliesCorrectionThroughView`,
`LuaR5.HardenedStateDebugAndStandins`, `WorldLoadR5.PluginHeaderValidationArms`
and `PluginLoad.SourcePathRecorded`.

One is already understood and fixed: **`world.Version ()` answered `""`**,
because `MUSHCLIENT_VERSION` is a `CString` global that
`CMUSHclientApp::InitInstance` assigns rather than a compile-time constant, and
this host does not run `InitInstance`. `MC_TEST_WIN_APP_STATE` now sets it, for
the same reason it loads the option table and the MXP tables.

**Two hangs are the first job**, because a hang is the one state that costs more
than it reports: `WorldLoad` and `WorldLoadR4` sit until the clock kills them,
which on this host means a modal nothing can answer. §10r's `SHBrowseForFolder`
and a `CFileDialog` from a missing plugin file are both plausible; neither is
confirmed.

#### Why they are still excluded

The suites stay out and the infrastructure lands, which is the split this
document has settled on before: **the unit of progress is the recorded reason.**
Entering a suite that hangs would take the whole target red for one file, and
triaging eleven failures across five suites is its own pass. Both exclusion
reasons now carry the measured list rather than a guess, so the next pass starts
from data.

The infrastructure is **not** left without a caller, which is the failure mode
that would otherwise follow: `tests/win/test_gap_coverage.cpp` gains
`WorldLoadHost.RealSerializeLoadsAWorldFileEndToEnd` and
`WorldLoadHost.ANonWorldFileIsRefused`. Between them they drive
`Serialize` → `IsArchiveXML` → `Serialize_World_XML` → `Load_World_XML` end to
end against real MFC and a real `CArchive`, assert that `SetPathName` happened
(a driver that skipped it would pass everything else and then break
`%WORLDFILEDIR%` and Save for all 279 tests), and assert that a non-world file
is refused rather than half-loaded — without which the positive test would be
satisfied by a driver that swallowed every error.

### 10z. The three biggest excluded suites, entered — and what one macro was costing

`test_world_load.cpp` (105 tests), `test_lua_scripting.cpp` (174) and
`test_connection.cpp` (154) are all IN. The target went from **1,084 passing
across 130 suites** to **1,508 across 175**, with nothing crashed, nothing
blocked and nothing failing.

Almost all of it came from one fact, stated four different ways in four
different exclusion reasons before anyone noticed it was the same fact.

#### `Frame` is a dereference

```c
#define Frame (*((CMainFrame*)App.m_pMainWnd))      // stdafx.h:271
```

`App.m_pMainWnd` is NULL in a console host, so **every** `Frame.Anything()` is a
null dereference, not an inert call. That single line was behind:

| symptom | where |
|---|---|
| `CRASHED` — `SweepR4.DebugListingsCoverEachAction` | `GetInfo` 249 → `GetMainClientRect` → `mainClientRect` → `Frame.GetClientRect` |
| `CRASHED` — `LuaScripting.SetTraceFlushesAHalfBuiltLine` | `OnBytesReceived` → `Frame.CheckTimerFallback` → `ProcessTimers` |
| `CRASHED` — `LuaR5.WindowPositionsWithWorldView` | `L_GetMainWindowPosition` → `Frame.GetWindowPlacement` |
| `ACCESS VIOLATION` — `test_connection.cpp`, excluded for it | `ConnectionClosed` → `Frame.OnUpdateBtnWorlds` |
| `GetInfo` codes 251–260, guarded off in `test_info.cpp` | `&Frame.m_wndToolBar` and friends |

The fix is three lines in `mc_test.h`: **construct** a `CMainFrame` and never
`Create()` it.

```cpp
if (App.m_pMainWnd == NULL)
    App.m_pMainWnd = new CMainFrame;
```

Its constructor is fourteen assignments and a `ZeroMemory` — no window, no
toolbars, no timers — so every embedded control has `m_hWnd == NULL`, which is
exactly the state MFC's own guards (`if (pWnd->m_hWnd)`) are written for. The
engine's existing checks start working instead of faulting one line before they
are read.

#### Why that was tried before and recorded as a failure

§10v tried precisely this and measured a **regression**: 1053/1 → 1006/71, with
`winocc.cpp:242` assertions and `CArchive badSchema` breaking plugin loading in
suites that had nothing to do with the change. It was reverted undiagnosed, and
the measurement was written down next to the exclusion — which is the only
reason it was cheap to re-open.

Re-measured here: 1206/4 → **1142/72**. Same regression, same shape.

The cause is one guard that checks the wrong thing:

```cpp
void CMainFrame::SetStatusMessage (const CString & msg)
  {
  if (App.m_pMainWnd)              // <-- a frame OBJECT exists
    {
    m_wndStatusBar.SetWindowText (msg);   // <-- ...says nothing about the BAR
```

`m_pMainWnd` answers "is there a frame object", and the line beneath it needs
"does the status bar have a window". Those were the same question for as long as
the only way to get a frame object was to create one. Give the host a
constructed-but-uncreated frame and they come apart: `CWnd::SetWindowText`
asserts, and it does so **inside `Load_World_XML`**, which is why the visible
damage was plugin loading rather than the status bar.

Two callers, five lines apart, both fixed the same way — ask the control:

```cpp
if (m_wndStatusBar.m_hWnd == NULL)
  return;
```

`SetStatusMessageNow` alone took the target from 1142/72 to **1346/14**.
`SetStatusMessage` took `test_connection` from 37 failures to 15.

**The generalisation is worth more than the fix.** A guard that has to
dereference the thing it is guarding is not a guard. `pal/win_worldview.cpp` had
six of them in a row — every infobar body opens with
`if (!Frame.m_wndInfoBar.m_hWnd) return;`, and that read is itself through the
null pointer. They all now go through one accessor that answers the real
question:

```cpp
static CMainFrame * frame ()
  {
  CMainFrame * f = (CMainFrame *) App.m_pMainWnd;
  return (f && f->m_hWnd) ? f : NULL;      // an OBJECT is not a WINDOW
  }
```

That file's header comment used to end "CANNOT be compiled here (no MSVC) — by-
reasoning parity". It can now, and the first thing compiling it found was this.

#### The two hangs were one `CFileDialog`

`WorldLoad` and `WorldLoadR4` both sat until the clock killed them. Enumerating
the hung process's windows named it immediately: a visible `#32770` titled
**"Trigger file name"**, i.e. `CMUSHclientDoc::Load_Set`'s file dialog
(`evaluate.cpp:668`), raised by the one test in each suite that calls
`Load_Set (TRIGGER, "", NULL)`.

`::TMessageBox` and `::AfxMessageBox` already have an answer on this host
(`McTestMessageBox`, two hooks because they do not funnel — see §10q). A common
dialog cannot use it: a `CFileDialog` is a stack object whose `DoModal` the
caller owns, so the decision has to be taken at the call site. One `#ifdef` per
site, each naming the dialog it declines, answering `McTestCancelDialog`'s
`IDCANCEL`.

Cancel is the only answer that needs no file, no user and no state — and every
caller of a file dialog already has a cancel path, because a user can always
press Escape. What that leaves under test is the arm the two suites actually
assert: `if (nResult != IDOK) return TRUE;`, the Windows cancel convention the
port's headless `Load_Set` copies.

There are 31 `CFileDialog` sites in the tree and exactly one was reachable.
Finding that out took one run of a probe that closes every dialog the process
raises and records the last `[PASS]` line at the moment each appears — cheaper
than N build cycles, and it proved there was no second modal behind the first.

#### A crash now names its frame

The single highest-leverage thing added in this pass is not a fix. This target
compiles `/EHsc`, where the runner's `catch (...)` does not see an SEH
exception, so an access violation killed the process and `run_tests.ps1` could
report only `CRASHED (no summary)`. The last `[PASS]` on stdout named the test
*before* the one that died and nothing at all about where in the engine it went.

`tests/win/all_tests.cpp` now installs an unhandled-exception filter that walks
the stack from the faulting `CONTEXT` with `StackWalk64` + `SymFromAddr`,
symbolised from the PDB the Debug build already writes. It prints the exception,
the faulting address, the test name, and forty frames with file and line. Every
crash above was diagnosed from its first output.

The same walker is wired to `_CrtSetReportHook`, opt-in behind
`MC_TEST_ASSERT_STACKS=1`, because an MFC assertion has the identical problem
from the other direction: `winocc.cpp(242) : Assertion failed!` names a line
inside MFC, which is never the interesting half. Off by default — one suite can
raise fifty — and capped at twelve stacks per run. It is what turned
`doccore.cpp(293)` into `docs/UPSTREAM-BUGS.md` 91.

This is the Windows counterpart of the in-process
`backtrace()`/`backtrace_symbols_fd` handler the macOS side reached for when
`lldb` hung on the notepad crash, and it is worth having for the same reason:
the instrument that is always installed beats the one that has to be attached to
a reproduction.

#### The finding that outlives this pass: two test-only reimplementations

`scripting/lua_utils_portable.cpp` and `scripting/world_debug_portable.cpp` are
in **`mushcore_testsupport` only** (`CMakeLists.txt:870-877`). The product — on
both platforms — uses `scripting/lua_utils.cpp` and `world_debug.cpp`. So the
macOS suite has been asserting the behaviour of bodies **nothing ships**, and
the Windows target, which links the real ones, disagrees with them in at least
five measured places:

| what | fixture (`*_portable.cpp`) | shipping |
|---|---|---|
| `utils.utf8decode ('\255')` | raises `Bad UTF-8 string. Error at column 2` | returns **no results**, no error |
| `utils.utf8encode ({72,'x'})` | `Index [2] of table arg #1 must be a number` | `Index [2] of table at argument #1 to 'utf8encode' is not a number` |
| `world.Debug ('triggerlist')` | lists the triggers | prints the **help**: Windows' vocabulary is `triggers`, `aliases`, … and `triggerlist` belongs to `DebugHelper`, the hyperlink handler |
| trigger-listing layout | `%-14s␠␠%s`, budget `wrap - 44` | `%*s␠%s`, budget `wrap - 42` — one column narrower |
| the alias-count line | `PLURAL` → "2 alias**s**." | `PLURALES` → "2 alias**es**." |

The `utf8decode` divergence is the sharpest: the fixture reimplemented a version
of the function that is **commented out** in the shipping file, two definitions
above the live one.

None of this can reach a plugin, because none of it is in the product. What it
reaches is the test suite's own credibility: a test that passes against a body
nothing runs is the worst kind of green, which is the same lesson §10x recorded
about `ReadNames`. Every affected test now carries **both** arms as positive
assertions rather than one arm and a skip, so neither platform's behaviour is
merely stepped over. The change that removes the fork — hoisting the
pure-computation half of `lua_utils.cpp` into `mushcore` — is its own piece of
work and is not attempted here.

#### The rest of the triage, by cause

Fourteen failures survived the frame fix in the two Lua/world-load suites, and
fifteen in `test_connection`. Grouped:

- **A `long` is 32 bits here.** `world.SetNormalColour (3, 0xFF123456)` masks to
  `0x123456` on the 64-bit port and to **0** on Win32: the value is past
  `LONG_MAX`, the double→long conversion saturates to `0x80000000`, and
  `& 0x00FFFFFF` takes it to zero. Upstream MUSHclient is a 32-bit Windows app,
  so Windows is the reference and the *test constant* was the problem. Now
  `0x7F123456`, which is in range everywhere and still has a high byte for the
  mask to drop.
- **Locale.** `GetLineInfo(n).timestr` is zero-padded English 12-hour on the
  port because `lua_utils_portable.cpp` renders `%m/%d/%Y %I:%M:%S %p` to lock
  it; Windows renders through `COleDateTime::Format (0, 0)`, i.e.
  `VarBstrFromDate` with the user's locale. The strict pattern is a port
  assertion; Windows asserts a date and a time are both present.
- **Text-mode log files.** The log is `fopen`ed `"w"`/`"a+"`, so on Windows every
  `\n` the engine writes lands as CRLF, and six assertions read the bytes back
  in binary. Already a recorded, deliberate parity gap (CLAUDE.md, macOS notes);
  the expected separator is now `MC_LOG_NL`.
- **Two error-code spaces.** `GetSocketError` switches on `WSAE*` on Windows and
  calls `strerror` on the port. `ECONNREFUSED` is 107 and `WSAECONNREFUSED` is
  10061 — not a near miss, a different space. Each arm now asks its own platform
  for a code that platform can deliver, and the Windows arm additionally asserts
  that a POSIX errno really is unknown, which is what makes the other two mean
  something.
- **Things that genuinely need a view.** `SendWindowSizes` (NAWS rows come from a
  `CMUSHView`'s text rectangle), `SendTo (eSendToCommand)` and
  `SpellCheckCommand` (the command bar is a `CSendView`),
  `GetWorldWindowPositionX`. Each keeps a Windows arm asserting the *right*
  answer for a world with no window — nothing announced, nothing typed, `-1`,
  an empty string — rather than being skipped. Where the port has a second
  reason for the same number, it is now visible: the NAWS guard sets
  `m_bNAWS_wanted` and a non-zero `m_FontHeight` first, so the missing view is
  the only remaining explanation for the silence.
- **A path is fully qualified when it is stored.** `CPlugin::m_strSource` comes
  from `CFile::GetFilePath`, so `/tmp/x.xml` is recorded as `C:\tmp\x.xml`. The
  test now asserts the two things true of both platforms: the file is named, and
  the recorded path reopens.
- **Five languages Windows whitelists unconditionally.** `stdafx.h` defines
  `USE_PYTHON` / `USE_TCL` / `USE_PHP` / `USE_RUBY` inside its `#ifdef _WIN32`
  block, and `xml_load_world.cpp:2538` lets `perlscript` and `jscript` through
  on `_WIN32` alone. So `rubyscript` and `perlscript` plugin headers **load**
  here and are refused on a port build without those engines. The expectation
  flips rather than the cases disappearing.
- **One resource this target does not link.** `progress.new` creates a real
  `CProgressDlg` from `IDD_PROGRESS`; the test binary compiles the engine
  sources, not the `.rc`, so `_AfxCheckDialogTemplate` fails
  (`dlgcore.cpp:210`). What the test is *named* for — the library being
  registered at all, which was `#ifdef _WIN32` and therefore missing from the
  port until 2026-08-12 — is asserted on both.
- **`utils.menufontsize()`** reports `nil` on the port because
  `SystemParametersInfo` is an unconditional `FALSE` there (D-25). Here the
  metrics ARE readable, so the premise in the test's name does not hold and
  `nil` would be the bug; the Windows arm asserts a real size instead. It is
  deliberately **read-only**: `utils.menufontsize(n)` calls
  `SPI_SETNONCLIENTMETRICS`, which changes the machine's menu font, and a unit
  test has no business doing that even for the moment before it puts it back.

#### And one defect in the tests themselves

`SendToTest.CommandDestinationFillsEmptyBarOnly` deleted a heap `CMUSHclientDoc`
while a stack `FakeWorldView` was still installed in `m_pWorldView`, and
`~CMUSHclientDoc` **deletes** that pointer. The old code nulled it instead,
which merely leaked the adapter the document had built for itself; putting
`mc_test::ScopedWorldView` in turned the leak into `operator delete` over a
stack object and a crash inside the debug heap's own reporting. `ScopedWorldView`
is right when the document outlives the guard — a stack document declared
first — and wrong when the document is deleted inside the guard's scope. Here it
is a hand-written save/restore placed before the `delete`.

Two more suites still assign `m_pWorldView` directly and are now guarded:
`LuaR5.WindowPositionsWithWorldView` and
`SpellCheckTest.SpellCheckCommandAppliesCorrectionThroughView`, both with stack
documents, both correct with `ScopedWorldView`.

#### Still out, with the reason re-measured

`test_accelerators.cpp` is unaffected by the main frame: `AcceleratorTo` fails at
its first line because `GetFirstViewPosition()` is NULL, and a `CView` attached
to a document is a window, not a registry entry. The frame does not supply one.
The other fifteen exclusions are unchanged.

### 10aa. `test_lua_surface.cpp`, and the two host settings that were quietly wrong

The fourth big suite is in: 94 tests, **1,597 passing across 176 suites**. Its
stated exclusion reason — an access violation in `LuaSurface.UdpFamily` after
three earlier failures — was already stale when §10z landed, because the
`Frame` fix removed that whole class of crash. What was left was 41 failures and
two modals, and almost all of it came from two things the host was not doing.

#### `InstallGdiSurfaceBridge` is an InitInstance step, and nobody ran it

`pal::g_surface` is NULL until `MUSHclient.cpp:254` runs. Without it every
`CMiniWindow` drawing operation — the whole `world.Window*` family, both image
loaders, the DIB store, text measurement — fails at the seam rather than at
anything it is being asked about. Adding one call to `MC_TEST_WIN_APP_STATE`
cleared **thirty** failures at once and, more to the point, made the miniwindow
tests run against **real GDI**: the image ops, the alpha merges, the filters,
the blends and the PNG/BMP round-trips now have a second surface to disagree
with, which `pal::CpuRaster` has never had.

This is the third InitInstance step this host has had to be taught, after
`LoadGlobalsFromDatabase` and `MUSHCLIENT_VERSION`. The pattern is worth naming:
**anything `InitInstance` does before the first document exists is something the
engine afterwards assumes has happened**, and a console host that skips it fails
somewhere else entirely.

#### The test binary was DPI-aware and the client is not

`MUSHclient.vcxproj` sets `<EnableDpiAwareness>false</EnableDpiAwareness>` in
both configurations, and its manifest declares no `dpiAware` element. The
shipping client is therefore DPI-**unaware**: Windows lies to it about the
display and `GetDeviceCaps (LOGPIXELSY)` answers 96 whatever the monitor is
doing. MSBuild's default for a VC project is DPI-**aware**, and the generator
had never said otherwise — so the test binary read this machine's real 216 dpi.

`CMiniWindow::Font` computes `MulDiv (points, srf->dpi (), 72)`, so an 8-point
font produced a 24-pixel cell in the tests and 11 in the client, and D-21's
font-parity pin failed against a client that was never wrong.

**The generalisation:** the generator's whole purpose is that there are not two
definitions of "how this builds on Windows", and until now it copied the source
list and the preprocessor defines. Manifest settings are equally part of that,
and this one silently changes what every *pixel* measurement in the suite means.
`tests/check_win_test_sources.py` pins the `<ClCompile>` lists; nothing pins
this, and the next such setting will go the same way.

#### A test that could not put the working directory back

`LuaSurface.ChangeDirRoundTrip` built its restore path into a Lua literal:

```cpp
"assert(world.ChangeDir('" + std::string(orig) + "') == 1)\n"
```

`getcwd` returns backslashes here, and **Lua 5.1 does not reject an unknown
escape — it silently drops the backslash**, so `C:\Users\shive` became
`C:Usersshive` and the restore failed. That is not a local failure: the test
left the **process** at `C:\`, and later tests in the file failed for reasons
that had nothing to do with them. It now escapes the path and `chdir`s back in C
regardless, so a failure costs one test instead of the rest of the suite.

Worth carrying: a test that mutates process-wide state must restore it in a way
that cannot itself fail, and a path is never safe to paste into a string
literal.

#### Font parity: the size half is now closed exactly, and the face half measured

`docs/FONT-PARITY-2026-08.md` left two things open. Both moved.

**Size.** The pin asserted `MulDiv (pt, 96, 72) == tmHeight`, and on real GDI
that is the wrong quantity. `pal/gdi_surface.cpp` hands the em to
`CreateFont` as a **negative** `lfHeight`, which asks the mapper to match the
character height; the `tmHeight` it reports back is that em **plus the font's
internal leading**. Measured at 96 dpi: 8pt → a 14-pixel `tmHeight` over an
11-pixel em, and the same three-pixel gap at every size.

`pal::CpuRaster` reports the em *as* `tmHeight` and leaves `internalLeading` at
its zero default — which is exactly the "under-constrains the headless parity
assertions" note in CLAUDE.md, now measured rather than suspected. The pin is
now `tmHeight - tmInternalLeading`, an **exact** equality on both surfaces (zero
leading on CpuRaster, three-ish on GDI), and it pins the arithmetic D-21 was
about instead of a quantity that legitimately differs.

**Face.** `WindowFontInfo (w, f, 21)` returns the *realised* face, so GDI can be
asked directly what it substitutes. Measured: **`"NoSuchFaceAtAll"` resolves to
`"Arial"`** — a proportional face. CLAUDE.md's note at `CFont::CreateFont` says
the port's shim drops `pitchAndFamily` "so an unknown face does not fall back to
monospace as GDI's mapper does". With a zero pitch/family hint GDI does not fall
back to monospace either; it falls back to Arial. The hint is what would change
that, and the port drops it — so the gap is real, but its consequence is not the
one the comment states.

The same measurement is why the test now names `Courier New` on Windows: `Menlo`
is a macOS font, GDI substitutes Arial for it, and every width assertion below
was therefore measuring a *proportional* face while claiming to measure a
fixed-pitch one.

#### A stretch test that asserted a colour it was told not to

`pal/cpu_raster.cpp`'s stretch branch carries this, in as many words:

> This will NOT match the wx adapter's HALFTONE (`BlitQuality::Smooth`) output,
> and it is not meant to: the shipping rasteriser is the parity bearer for
> interpolated colour. […] Tests over a stretch must assert those and never an
> interpolated colour value.

`DrawImageStretchModeDrawsAndHonoursItsSourceRect` asserted interpolated colour
values, and passed only because CpuRaster does not interpolate.
`world.WindowDrawImage` mode 2 requests `BlitQuality::Smooth`, which is
`::StretchBlt` with `HALFTONE` — the mode the call site's own comment records
upstream using — so shrinking nine source columns into three **blends** on
Windows. Measured: the leftmost destination cell is `0x0048D0`, a colour that is
not in the image.

Measured too, and recorded because it is the shape of the difference: the
horizontal magnify came out `GGGGGGGGGGGGGGGBBBB` where CpuRaster's
`floor (X * srcW / dstW)` gives sixteen greens, and the vertical band split
identically on both. So the divergence is one pixel of boundary on magnify and
genuine interpolation on minify.

The Windows arm now asserts placement and extent — the bands are contiguous and
in the right order, all three minified cells are painted, nothing spills — and
the port keeps its exact nearest-neighbour columns, which is legitimate there
and is not a parity claim. **A comment that tells you what a test may not assert
is worth reading before writing the test**; this one had been there all along.

#### The rest, by cause

- **`world.AddFont` fails differently, and the difference is the macOS gap read
  from the other side.** Windows resolves `AddFontResourceExA`, calls it, and
  gets 0 back for a missing file → `eFileNotFound`. The port's
  `LoadLibrary ("gdi32")` answers nullptr, so it never reaches the file →
  `eNoSuchCommand`, i.e. `world.AddFont` there can never register anything.
- **`world.GetRecentLines (3000000000)`** is the second instance of the 32-bit
  `long` class (§10z's palette constant was the first). The value does not fit,
  the double→long conversion saturates to `LONG_MIN`, `Count > 0` is false and
  the method returns empty — so the out-of-bounds read the test exists to guard
  is never reached. Two billion is positive in 32 bits, still past the buffer,
  and still makes `size() - Count` wrap. The out-of-range value is asserted
  separately on Windows, where the answer must be a harmless empty string.
- **`world.Open` reaches `CWinApp::OpenDocumentFile`,** whose first act is
  `ASSERT_VALID (m_pDocManager)`. This host has the document *templates* but
  deliberately never calls `AddDocTemplate` — that is what creates the manager,
  and it also makes File▸New and the OLE server reachable. A bare
  `new CDocManager` with no templates registered would make the line answer
  `false` without any of that reach; noted, not done, because it changes a
  framework global for the whole target.
- **Accelerators need a `CView`, not a frame.** `world.Accelerator` refuses at
  its first line for the same reason `tests/test_accelerators.cpp` stays out,
  and the CMainFrame did not change that — measured.
- **Sound is real here, and audible.** `CMainFrame::PlaySoundFile` hands a `.wav`
  to `::PlaySound`, which reports success for a file that does not exist *and
  plays the system default sound while it is at it*; the rest of the family
  opens DirectSound. The parameter check (`Sound ("")`) runs before any device
  and is asserted on both; the backend assertions are the port's and stay there.
  A unit test does not make a noise on the machine it runs on.
- **`utils.glyph_available` and `utils.menufontsize`** read the real system here
  and get real answers; both arms are asserted rather than either being skipped.
- **`utils.*` dialogs are the one thing with no Windows answer at all.**
  `utils.filepicker` is a `CFileDialog` and `utils.directorypicker` is
  `::SHBrowseForFolder`, both measured putting up a window nothing can answer.
  Unlike every other view-bound case in this campaign, "what does it answer with
  no GUI" has no answer — it does not return until a user acts. That test is the
  port's null-hook seam and is skipped here, explicitly.
- **`world.ChatPasteEverybody` reads the machine's clipboard**, so its answer
  depends on what the user last copied. The assertion now accepts either
  `eClipboardEmpty` or `eNoChatConnections` and still rules out `eOK`. Clearing
  the real clipboard to force the empty case is not something a unit test may do.

### 10ab. A `CMUSHView` can be constructed here, and the first thing it refuted

`docs/UPSTREAM-BUGS.md` 86 — the two UTF-8 click fix-ups walking in opposite
directions — had sat marked *"not fixed; needs a Windows run"* since it was
written, on the grounds that `CMUSHView` is never constructed on the port and
nothing there can observe the defect. It turns out not to need a GUI at all,
and once measured it is **not a defect**. The entry has been rewritten as
refuted; the measurement and the table are there.

The part worth carrying is the technique, because "needs a view" is the reason
attached to `test_accelerators.cpp`, to `SendWindowSizes`' NAWS rows, to
`GetWorldWindowPositionX`, to `SpellCheckCommand` and to `world.Open`, and it
has been treated throughout this campaign as a wall.

**It is a low wall.** Three facts, each measured:

- `CMUSHView`'s constructor is twenty member assignments (`mushview.cpp:275`).
  It is `protected: // create from serialization only`, like every MFC doc/view
  class — and `RUNTIME_CLASS(CMUSHView)->CreateObject()` is the framework's own
  way past that, and it is public.
- `CDocument::AddView` attaches a windowless view without complaint. Its
  `OnChangedViewList` → `UpdateFrameCounts` walk asks each view for its parent
  frame, and `CWnd::GetParentFrame` answers NULL for a null `m_hWnd` rather than
  asserting.
- `CClientDC` given a **NULL** `CWnd*` is the **screen DC**. Anything that wants
  a `CClientDC&` to measure text with can have one.

And one hazard, which is the reason the fixture holds both objects by pointer:
**the document must be destroyed before the view.** `~CDocument` calls
`DisconnectViews`, which nulls each view's back-pointer and empties the list
without deleting anything. `RemoveView` is the tidy-looking call and the wrong
one — removing the **last** view runs `OnCloseDocument`, and for an auto-delete
document that is `delete this`.

What this does *not* give is a view with a **window**: `m_hWnd` stays NULL, so
anything that paints, scrolls, captures the mouse or reads its own client
rectangle is still out of reach — which is why `test_accelerators.cpp` stays
excluded (`AcceleratorTo` walks to a view and then wants its accelerator table).
The line is not "no view" but "no window", and a good many of the reasons in
this document say the former when they mean the latter. Each is worth
re-reading against `ClickColumnR6`'s fixture before it is believed again.

**On the refutation itself.** The bug entry's reasoning was careful and its
proposed fix was one token, and both were wrong: the two walks *compose* — each
normalises the byte the step before it produced, in the direction that step
went. Applying the proposed `col++` → `col--` was measured as a regression (a
selection anchored in the left half of an accented character would start one
character too far left). The general form is worth stating, because this project
keeps meeting it: **an asymmetry in code is evidence of nothing until the two
halves are measured against a control.** The control here was the single-byte
character next door, and it took eight lines.

### 10ac. Why this host has no dialog templates, and why that is load-bearing

Two comments written during §10z and §10aa said the test binary "links no
resources" and that `CG_IDD_PROGRESS` is "a resource this console target does
not link". Both were reasoning, not measurement, and both are wrong in the
letter — which matters, because the correct explanation points somewhere useful.

**The target does compile `MUSHclient.rc`.** The generator has emitted a
`<ResourceCompile Include="MUSHclient.rc" />` all along. That file is 144 lines
and carries a cursor, three icons and a version block — *nothing else*.
Confirmed by enumerating the built `MUSHclient.exe`'s resource types:
`CURSOR`, `ICON`, `GROUP_CURSOR`, `GROUP_ICON`, `VERSION`, and no others. No
`MENU`, no `DIALOG`, no `STRING`, no `ACCELERATOR`.

That is not damage and it is not the port's doing — `git log --follow` shows the
file has been 143 lines since the first commit in this repository. **On Windows
MUSHclient keeps its dialogs, menus, string table and accelerators in
`locale\EN.dll`** and loads them at runtime: `CMUSHclientApp::I18N_Setup` ends in
`AfxSetResourceHandle (m_hInstDLL)` (`MUSHclient.cpp:1832`). It is the same DLL
`tools/decompile_dialogs.py` recovered `platform/mushclient_dialogs.rc` from for
the wx bridge, and that file's own header says so in its second paragraph.

#### The experiment, and its result

Six lines will give the host those resources: `LoadLibrary ("locale\en.dll")` —
`run_tests.ps1` already sets the working directory to the staged runtime assets,
which is exactly where `ExtractDirectory` of an unset `m_strMUSHclientFileName`
points the real code — then `AfxSetResourceHandle`. `I18N_Setup` itself is not
callable, because its failure arms are plain `::MessageBox`, which nothing
routes.

Measured: **four suites went from green to `BLOCKED` at the runner's clock** —
`LifecycleArms`, `LuaScripting`, `SpellCheckTest`, `SweepR4`.

The reason is worth stating plainly, because it inverts an assumption this
target has been running on:

> This host's safety from modal dialogs has never come from routing them. It
> comes from there being **no dialog template to instantiate**.

`McTestMessageBox` covers `AfxMessageBox` and `UMessageBox` — §10q worked out
that those are two funnels and hooked both. `McTestCancelDialog` covers the
common dialogs, one call site at a time (§10z). **Nothing covers a `CDialog`**,
and until now nothing had to, because `CDialog::Create` could not find a
template and failed.

So the resources are worth having only together with a way to decline every
`DoModal`, and that is a change to the shim rather than to this host. The
experiment is reverted and the finding recorded at both sites. A missing dialog
template is load-bearing; anything that would supply one has to bring a policy
for answering dialogs with it.

#### What it costs, stated so the next attempt starts here

Without resources this target cannot reach: `CProgressDlg` and therefore the
Lua `progress` library's create path; anything `AfxMessageBox (nIDPrompt)` says;
`CMultiDocTemplate`'s title/filter strings (one assertion per template on every
run, and the reason `world.Open` cannot report a proper failure); the
accelerator tables `AcceleratorTo` copies from; and every dialog in
`dialogs/**`, which is most of what §10h still owes.

The route in is **not** the one that first suggests itself. "Give the MFC shim a
`MUSHCLIENT_TESTS` arm on `CDialog::DoModal`" is what the port would do and is
meaningless here: on Windows `CDialog` is real MFC, in a static library nobody
in this tree edits. There is no single place every modal passes through.

So the only honest route is the one `McTestCancelDialog` already takes — a
`#ifdef MUSHCLIENT_TESTS` arm at the call site — and the size of that job is
now measured rather than guessed: **159 `DoModal ()` call sites across 52 files**
in the engine and `dialogs/**`. Which is to say the dialog half of §10h is not
going to be unit-tested into existence one `#ifdef` at a time; the ones worth
doing are the handful a specific test actually reaches, exactly as `Load_Set`'s
was.

A cheaper alternative exists and is untried: a `WH_CBT` hook posted for the
duration of a test, cancelling any dialog the process creates. That is one
mechanism rather than 159 edits, it needs no change to shipping source, and it
would let the resource DLL be loaded. It also silently changes what every
existing green test is running against, so it wants its own measurement before
anyone relies on it.

### 10ad. One hook answers every dialog, so the host can have its resources

§10ac ended by ruling out the obvious route — a `MUSHCLIENT_TESTS` arm on
`CDialog::DoModal`, which is the *port's* answer and meaningless on Windows,
where `CDialog` is real MFC in a static library — and recorded a measured
alternative as untried: **a `WH_CBT` hook, cancelling any dialog the process
creates**. It has now been tried, and it works.

```cpp
static LRESULT CALLBACK McTestCbtProc (int code, WPARAM wParam, LPARAM lParam)
{
  if (code == HCBT_ACTIVATE)
    {
    char cls [64] = "";
    ::GetClassNameA ((HWND) wParam, cls, sizeof cls);
    if (strcmp (cls, "#32770") == 0)          // the dialog window class
      {
      ++s_dialogsCancelled;
      ::PostMessageA ((HWND) wParam, WM_CLOSE, 0, 0);
      }
    }
  return ::CallNextHookEx (s_cbtHook, code, wParam, lParam);
}
```

Three properties make it the right shape:

- **It is the funnel Windows itself provides.** Every window the process creates
  passes through the hook before it is shown, so there is one place instead of
  the 159 `DoModal ()` call sites §10ac counted.
- **`WM_CLOSE`, not `WM_COMMAND`/`IDCANCEL`.** `DefDlgProc` turns `WM_CLOSE` into
  `IDCANCEL` for a dialog that has a Cancel button and closes one that does not,
  so property sheets and the common dialogs are covered by the same line.
- **No shipping source changes.** It lives entirely in
  `tests/win/all_tests.cpp`.

With it installed, `MC_TEST_WIN_RESOURCE_DLL` is safe, and the two land as one
change: `LoadLibrary ("locale\\en.dll")` then `AfxSetResourceHandle`. The run is
**1,599 passing across 177 suites**, exactly as before — and the four suites the
first attempt at the DLL had blocked (`LifecycleArms`, `LuaScripting`,
`SpellCheckTest`, `SweepR4`) are green.

The second measurement is the better one. `--suite=LuaScripting` used to print
several MFC assertions per run — the two document templates failing to load
their title/filter strings, `dlgcore.cpp:210` failing the progress dialog's
template lookup. It now prints **zero**. The host is not merely quieter; it is
closer to the app, because those assertions were the sound of resources the real
client has and this one did not.

#### What it costs, and the one thing to remember

Every dialog is cancelled, including one a test might want to complete. That is
the trade, and it is the right way round for a unit-test host — but it means a
code path whose *outcome* depends on the user pressing OK cannot be asserted
here, only that it does not hang. `CGlobalPrefsP4::OnFont` is the standing
example and remains untestable for a different reason anyway: the subject of its
fix is the `LOGFONT` seeded before `DoModal`, and its only outputs are
`UpdateData` and `SetWindowText`, both of which need a window.

`McTestDialogsCancelled ()` exports the count, because "a dialog was raised" is
the only observable a test has for a path that ends in one.

`McTestCancelDialog`'s per-site `#ifdef` (§10z, `Load_Set`) stays. The hook is a
net, not a replacement: declining at the call site never creates the window at
all, which is both cheaper and deterministic, and it documents at the site what
the test is choosing not to do.

### 10ae. `test_accelerators.cpp` is in, and "needs a view" turned out to be three things

§10ab said the line was "no window", not "no view", and that several exclusion
reasons in this document say the latter when they mean the former. This is the
first one cashed in. `test_accelerators.cpp` is IN: 17 tests over the Win32
`HACCEL` machinery, which had **no test on either platform** — the port's suite
covers the map bookkeeping, and the table manipulation is `#ifdef _WIN32`.

Its reason had been re-measured twice and both times the number was 11 of 17.
It took three changes, not one, and the useful part is that none of them is what
the reason named:

1. **A `CMUSHView` attached to the document.** `AcceleratorTo` opens by walking
   the view list for the MDI child frame that owns the table, and refuses with
   `eBadParameter` if the walk finds nothing.
   `RUNTIME_CLASS (CMUSHView)->CreateObject ()` gets past the protected
   constructor and `CDocument::AddView` accepts a windowless view — the
   `ClickColumnR6` technique, applied for real.
2. **`CMultiDocTemplate::LoadTemplate ()` in the host.** With no window on the
   view, `GetParentFrame ()` is NULL and the fallback chain ends at
   `App.m_pWorldDocTemplate->m_hAccelTable`. That is populated by
   `LoadTemplate`, which the framework calls from `AddDocTemplate` — which this
   host deliberately does not call. Without it the table is NULL,
   `CopyAcceleratorTable` answers 0, and the body then evaluates `&vAccel [0]`
   on an **empty vector**: an out-of-range subscript that the debug STL turns
   into an abort.
3. **The resources from §10ad**, because `LoadTemplate` loads the accelerator
   table from `locale\en.dll`. That DLL does carry `RT_ACCELERATOR`; the exe
   does not.

So the dependency chain runs view → template → resources → dialog hook, and
removing any link puts the suite back where it was. That is why the earlier
measurements ("giving the host the document templates did NOT fix this", "the
resource DLL made no difference") were each true and each incomplete.

#### The conversion, and the one that got away

Every test declared `CMUSHclientDoc doc;`. They now declare an `AccelDoc`
fixture and bind `CMUSHclientDoc& doc` from it, so each body is unchanged; the
fixture is a plain `CMUSHclientDoc` member off Windows. Fourteen sites converted
mechanically — and **one did not**, because it declares `doc2` rather than
`doc`, and a pattern that matches on the variable name matches only the name.
It was the single remaining failure at 1,615/1, which is the cheap kind of
mistake: a regex conversion that leaves one site behind fails loudly at exactly
that site.

The fixture destroys the **document first and the view second**, and that
ordering is the whole reason both are pointers rather than members. `~CDocument`
runs `DisconnectViews`, which detaches without deleting; `RemoveView` is the
tidy-looking call and the wrong one, because removing the last view runs
`OnCloseDocument` — `delete this` on an auto-delete document.

#### Cost

Attaching a view is noisy: `CDocument::AddView` and its `UpdateFrameCounts` walk
raise a handful of MFC assertions per document, routed to stderr as usual. In a
suite that constructs one document per test that is a few hundred lines of
noise for 17 tests' worth of coverage over code nothing else reaches. Accepted,
and worth knowing before the technique is applied to a bigger suite.

### 10af. What round two owes macOS, file by file

The whole of round two — `ab34492~1..HEAD`, 84 commits — was written and
measured on Windows, and nothing in it has been compiled by clang. This is the
list the macOS `ctest` run has to clear before any of it merges, sorted by how
much attention each needs.

**This section is the handoff.** It was written when the round reached §10ae
and has been extended three times since, which is why the sub-headings below
read as a chronology rather than a taxonomy. If you are picking this up on
macOS, read this section and `docs/UPSTREAM-BUGS.md` 88-109, in that order —
§10af says what to check, the bug entries say why each change is what it is.

**The shipping-code lists are derived; the test list is not.** Every shared
hunk in the tables below was found by computing the preprocessor nesting at each
changed line of `git diff ab34492~1..HEAD` **and reading the guard text at each
depth** — not by reading back through §10, which twice had silently stopped
tracking files that reach macOS. Re-derive it the same way after further work.

The depth number alone is not the answer, and assuming it was is a mistake worth
not repeating: it cannot tell an `#ifdef _WIN32` from an include guard or an
`#if ALIASES_USE_UTF8`, so it scores a Windows-only hunk and a shared one inside
any conditional identically. Read what is open at that depth. On the test files
it is not worth doing at all — see below for why.

#### One behavioural change to shared engine code

`scripting/lua_methods.cpp` — `L_Save` no longer restores an **empty** path name
after a Save-As (`docs/UPSTREAM-BUGS.md` 91). On Windows that call threw
`CFileException::badPath` out of the Lua binding; on the port the `CDocument`
shim accepted it silently. The visible change on macOS is that
`world.Save (name, true)` on a world that has never been saved now **keeps** the
name it just wrote, where before it set the path name back to `""`. Nothing in
the suite asserts the old behaviour, but this is the one hunk that can change
what a macOS test sees.

#### Nothing else in shipping code can reach macOS at all

Verified by reading each hunk's guard rather than by assuming:

| file | why it cannot affect the port |
|---|---|
| `mainfrm.cpp` | both hunks are inside the `#else` (i.e. `_WIN32`) arm of an existing `#ifndef _WIN32` |
| `evaluate.cpp` | `Load_Set` is inside `#ifdef _WIN32` (`:599`); the `MUSHCLIENT_TESTS` arm nests inside it |
| `pal/win_worldview.cpp` | the whole file is `#ifdef _WIN32` |
| `platform/wx_mfc_hooks.h` | the addition sits inside the existing `#if defined(_WIN32) && defined(MUSHCLIENT_TESTS)` block |
| `tests/win/*`, `MUSHclient_tests.vcxproj` | Windows-only files. CMake never sees them — `tests/CMakeLists.txt` names each executable explicitly and globs nothing. The vcxproj is kept honest by the `lint_win_test_sources` ctest entry, which DOES run on macOS: if it goes red, the generator and the project have drifted, not the port. |

#### Shared test files: what changed for BOTH platforms

**26 shared test files changed in this round, and `ctest` is itself the check** —
unlike shipping code, where a mis-guarded hunk changes behaviour silently, a
test either compiles and passes there or it does not. So this list is a
head start on a red run, not a proof of anything: it names the edits known to
carry a macOS-visible semantic change. It was written by hand, and an attempt to
verify it mechanically failed — counting preprocessor depth cannot tell an
`#ifdef _WIN32` from an include guard or an `#if ALIASES_USE_UTF8`, so it reports
a fully-guarded file and a fully-shared one identically. Trust the run:

- **`mc_test.h` — the framework itself, and this row used to say the opposite.**
  It was listed as "every hunk is inside the file's `#ifdef _WIN32` block". Eight
  of its fifteen are not, and one of those changes what a macOS run DOES: `run_all`
  now wraps `tc.fn ()` in a `try`, so an exception escaping a test is a **failed
  test** rather than a dead process with no summary line. Also new and shared:
  `ScopedWorldView` (restores the doc's OWN view, never NULL — on Windows the doc
  constructs and deletes its own adapter, so the old `= nullptr` idiom leaked it),
  `list_suites ()`, a `run_all (const char * only)` filter, and an
  `#include <exception>`. None of it is conditional. If `ctest` fails to build,
  start here — it is included by every test file.
- **`test_lua_surface.cpp`, `MiniWindowFontIsWindowsSized`** — the pin is now
  `tmHeight - tmInternalLeading` rather than `tmHeight`. Equivalent on
  `pal::CpuRaster` *because it leaves `internalLeading` at its zero default*
  (`pal/isurface.h`), which is exactly the thing this change makes explicit. If
  CpuRaster ever starts reporting a real internal leading, this is the test that
  says so — which is the point.
- **`test_lua_surface.cpp`, `ChangeDirRoundTrip`** — the restore path is now
  escaped before it goes into a Lua literal, and `chdir` is called again in C
  regardless. A no-op on a platform whose paths contain no backslashes; the
  belt-and-braces `chdir` is new.
- **`test_lua_surface.cpp`, `GetRecentLinesClamps…`** — the discriminator is
  2,000,000,000 rather than 3,000,000,000, because the latter does not fit the
  32-bit `long` the API takes. Still past the buffer, still wraps
  `size () - Count`.
- **`test_lua_scripting.cpp`, `PaletteAccessors…`** — `0x7F123456` rather than
  `0xFF123456`, for the same reason.
- **`test_lua_scripting.cpp`, the two `SweepR4` debug-listing tests** — they
  concatenate the output lines before searching instead of scanning line by
  line, so a match spanning a wrap boundary is still found. Strictly weaker, and
  deliberately: the per-line form was asserting the wrap column.
- **`test_world_load.cpp`, `PluginLoad.SourcePathRecorded`** — asserts that the
  recorded source names the file and reopens, rather than string-equalling the
  path it was handed. `CFile::GetFilePath` fully qualifies.
- **`test_world_load.cpp`, `PluginHeaderValidationArms`** — split into a
  refusal loop and a language loop whose expectation is a named constant. On the
  port the constant is `false` and the assertions are the originals.
- **`test_connection.cpp`** — `MC_LOG_NL` is `"\n"` off Windows and
  `kNawsNeedsAView` is `false`, so both are inert there.
  `CommandDestinationFillsEmptyBarOnly` now saves and restores
  `m_pWorldView` instead of nulling it, which on the port stops leaking the
  adapter the document built for itself.
- **`test_accelerators.cpp`** — every test binds a `CMUSHclientDoc&` from an
  `AccelDoc` fixture, which off Windows is a plain `CMUSHclientDoc` member. The
  bodies are unchanged.

#### Port source changed on Windows, and NOT compiled here (§10am)

The Functions List fix is three edits to macOS-only code, made on this machine
and therefore built by nothing:

| file | change |
|---|---|
| `platform/menu_ui_rules.cpp` | `ID_GAME_FUNCTIONSLIST` removed from the world-only list |
| `platform/wx_mfc_impl.cpp` | a no-active-child arm for 33046 calling `::ShowFunctionslist (noContext, 0, 0, true)` |
| `tests/test_menu_rules.cpp` | 33046 moved from `kWorldOnly` to `kGlobal` |

`ShowFunctionslist` is declared in `stdafx.h` and defined in
`scripting/functionlist.cpp`, which is a `mushgui_adapters` TU, so the call
should link from `wx_mfc_impl.cpp` — but that is reasoning, not a build. The
behavioural check on macOS is: with **no world open**, Game ▸ Functions List must
be selectable and must open the list.

#### Shared code changed on Windows since §10af was written

Every hunk below is in a file macOS compiles, and none of them has been built
here except by the Windows target. Listed with what a macOS run should check.
The list grows as the round does — do not trust its length, trust the last row:

| file | change | what to check on macOS |
|---|---|---|
| `mxp/mxpClose.cpp` | `MXP_CloseTag` re-finds its line after the dispatch and returns if a gagging trigger deleted it (`UPSTREAM-BUGS` 94) | `MXPCloseR6.ParagraphCloseSurvivesAGaggingTrigger` is a **shared** test and runs there too. Worth one ASan run: this was a heap-use-after-free on both platforms, and ASan is the instrument that names it. |
| `dialogs/world_prefs/configuration.cpp` | `LoadPrefsP23` defaults into locals, not onto the document (`UPSTREAM-BUGS` 95) | World prefs ▸ **Cancel** on a world with no chat name must leave the file alone; **OK** must still write `chat_name`. |
| `dialogs/world_prefs/prefspropertypages.cpp` | `CPrefsP11::OnSaveNotes` builds a SAVE dialog (`UPSTREAM-BUGS` 93) | The Notes page's Save… must still offer a save panel and must now warn before overwriting. This is the call site the port's `openOrCreate` shim heuristic existed for. |
| `platform/mfc_ui.h` | comment only — records that the `openOrCreate` arm is now unreachable | nothing to run; the arm is left in place deliberately |
| `tests/test_mxp.cpp` | the new `MXPCloseR6` case and its local gagging-trigger helper | compiles against the port's `CTriggerArray` / `regcomp` / `ConvertToRegularExpression`, all shared, but it has only ever been built by MSVC |
| `pal/iworldview.h`, `doc.h` | the new `hasMainWindow ()` predicate and the accessor that gates on it (§10ar) | header-only; every adapter that does not override it keeps the old behaviour by construction |
| `platform/wx_world_child.cpp` / `.h` | `hasMainWindow ()` returning true | **blind edit** — three lines, and it is **correct**: measured on macOS 2026-08-22, `selftest_world_frame` green. The row's "the failure mode is quiet" is wrong twice over, and the correction is worth keeping. *Missing* is loud: `platform/world_frame_selftest.cpp:3941-3944` asserts `GetInfo(249)/(250)` against the frame's real client rect, after a nudge loop at `:3910` that resizes until the character grid and the client rect **differ** — so the fallback fails the test rather than passing quietly. *Wrong* cannot be silent either: the declaration carries `override` (`wx_world_child.h:303`), so a signature mismatch is a compile error, not a new function. And "which is what it did before" names the wrong before — the grid answer is what macOS did before `6b13a53`, three days earlier; between `6b13a53` and `e6fcbee` it reported the real rect, so a missing override would regress that feature rather than restore a status quo |
| `tests/fake_worldview.h`, `tests/test_info.cpp`, `tests/test_pal_worldview.cpp` | the fake gained `hasMainWin`; both suites now distinguish "no window" from "a window measuring nothing" | `test_pal_worldview.cpp` is excluded here, so its two new cases have never been compiled |
| `pal/win_worldview.cpp` | `saveWorld ()` now returns true on success and false for no path, matching the contract and the wx adapter (§10as) | `#ifdef _WIN32` — cannot reach macOS. Nothing calls the seam yet on either side, so nothing there changes. |
| `chatsock.cpp` | `Process_File_block` refuses a block shorter than the size the peer promised (`UPSTREAM-BUGS` 96) | shared, and the defect ships there too. Worth one **ASan** run of the chat suite: this is a heap-buffer-overflow read, and `tests/test_chat_sockets.cpp` runs on macOS where the Windows target excludes it. |
| `scripting/scriptengine.cpp` | the `Trace` format-string fix (`UPSTREAM-BUGS` 97) | inside the file's own `#ifdef _WIN32`, so it cannot reach macOS — the Lua path there was already correct |
| `chatsock.cpp` | `Process_File_start` refuses a second offer while one is running, and nulls `m_pFile` after deleting it (`UPSTREAM-BUGS` 98) | shared, and the defect ships there too. The same ASan run that covers 96 covers this. |
| `OtherTypes.h` | `bEnabled = TRUE` in `CTimer`'s constructor (`UPSTREAM-BUGS` 99) | shared header; the defect ships there too, and the reachable route is plugin-authored `<timer defaults="y">` XML |
| `scriptingoptions.cpp` | the `wrap_column` re-allocation now uses the UTF-8 byte count and updates `iMemoryAllocated` (`UPSTREAM-BUGS` 100) | shared, and **more likely to bite on macOS**, where a UTF-8 world is the common case. Worth an ASan run with `world.SetOption ("wrap_column", …)` on a busy world. |
| `xml/xml_save_world.cpp` | a zero is written whenever the option's default is non-zero (`UPSTREAM-BUGS` 101) | shared. **World files saved by this build gain up to four attributes** (`pixel_offset`, `fade_output_opacity_percent`, `tool_tip_visible_time`, `tool_tip_start_time`) when any is 0 — older clients read them fine, and the §5 byte-identical check no longer holds. |
| `methods_triggers.cpp`, `methods_aliases.cpp`, `methods_timers.cpp` | `eReplace` refuses an item whose script is on the stack (`UPSTREAM-BUGS` 102) | shared, and the defect ships there too. A plugin that re-registers itself from its own handler is the reproduction; worth an ASan run of the trigger suite. |
| `plugins.cpp`, `dialogs/world_prefs/prefspropertypages.cpp` | the archive is closed **inside** the `try`, so a write that fails stops being reported as success over an already-truncated file (`UPSTREAM-BUGS` 103) | shared, and the defect ships there too. The third site, `evaluate.cpp`'s `Save_Set`, is inside that file's `#ifdef _WIN32` (`:599`-`:944`) and cannot reach macOS. `PluginState.SaveStateWritesVariablesToStateFile` pins the success path and runs there; the failure path has no test on either platform, and 103 says why. |
| `scriptingoptions_alpha.inc` | `send_to_world_line_preamble` and `_postamble` gained `OPT_KEEP_SPACES` (`UPSTREAM-BUGS` 104) | shared table, so the defect ships there too — a saved `"say "` reloaded as `"say"` and every line went out as `sayhello`. `WorldLoad.ASendPreamblesTrailingSpaceSurvivesTheLoad` is a **shared** test and will run there. |
| `scripting/methods/methods_timers.cpp` | `SetTimerOption` refuses to zero the last non-zero component of an `every` timer (`UPSTREAM-BUGS` 105) | shared, unguarded — **and this is the one row of the seven that changes what an existing script sees**: `world.SetTimerOption (t, "second", 0)` on a timer whose hour and minute are already 0 now answers `eTimeInvalid` where it used to answer `eOK`. Nothing in the macOS suite asserts the old answer; check by grep before assuming. |
| `stdafx.h`, `xml/xml_save_world.cpp` | `Save_XML_double` takes a decimal count (defaulted to 2, so every other caller is unchanged) and the two timer-second sites pass 4 (`UPSTREAM-BUGS` 106) | shared. **Worlds saved by this build write timer seconds at up to four decimals**, which moves the §5 byte-identical baseline a second time on top of 101's four attributes. Sub-centisecond intervals previously round-tripped through the saver into a value the loader itself refuses. |
| `doc_shared.cpp` | the SOCKS 5 connect reply's length is derived from its ATYP byte instead of assuming IPv4 (`UPSTREAM-BUGS` 107) | shared by definition (one body, every build). Only the **ATYP half** of the filed claim is fixed; a reply split across TCP segments is still mishandled, and 107 records that closing it needs a persistent proxy-phase buffer and a "need more data" return. |
| `dialogs/world_prefs/configuration.cpp` | `LoadPrefsP23` shows the *preference* for accepting chat calls, not whether a socket happens to be listening (`UPSTREAM-BUGS` 108) | shared, and it goes wrong in **both** directions there too. Check: with "accept incoming calls" ticked and no chat server running, World prefs ▸ OK must leave the tick set — before, merely opening the pages and pressing OK rewrote the setting. Distinct from the 95 row above, which is the same function's *defaulting*. |
| `OtherTypes.h`, `ProcessPreviousLine.cpp` | `ClipboardWildcardIndex` maps the trigger clipboard dropdown's `10` onto the whole matching line (`UPSTREAM-BUGS` 109) | shared, and the dropdown offers the same entry there. Check: a trigger with *copy to clipboard: 10* must put the whole matched line on the clipboard, where before it put nothing at all. The bound is **remapped, not widened** — `wildcards [10]` is still never read. |

`pal/win_transport.cpp`'s `flushSend` guard is inside the file's own
`#ifdef _WIN32` and cannot reach macOS.

**NINE new tests are in the shared suite, not two — and all nine now compile and
pass on macOS.** This paragraph used to name only the two in
`tests/test_world_load.cpp`, `WorldLoad.ASendPreamblesTrailingSpaceSurvivesTheLoad`
(104) and `WorldLoad.AZeroSurvivesOnEveryOptionWhoseDefaultIsNotZero` (101), and
say that "everything else added this round is under `tests/win/`". It is not:
`git diff ab34492~1..HEAD -- tests/ ':(exclude)tests/win' | grep '^+TEST'` returns
nine. The other seven are the three `MxpVetoR5` cases and `MXPCloseR6` in
`tests/test_mxp.cpp` and three in `tests/test_plugins.cpp` — which the table two
rows above contradicts this sentence about, its `tests/test_mxp.cpp` row saying
the new case "has only ever been built by MSVC". Re-derive such a count; do not
re-read the narrative for it. None is a port assertion — each pins shipping
behaviour that was wrong on both platforms — so a failure there is a real signal,
not a platform difference to paper over.

**The third Windows surface has no macOS counterpart, and needs none.**
`tests/win/run_app_probes.ps1` and `tests/win/app_probes/*.lua` (§11) drive the
shipping GUI exe. Its macOS equivalent already exists and is older: the in-app
selftests behind `mushgui`'s hidden flags. The probes were written to reach what
neither headless suite can — real GDI, real font metrics, the real command bar —
which on macOS is exactly what `--pal-surface-selftest` and its siblings cover.
Do **not** port the runner; if a probe assertion turns out to be worth having on
both sides, its home there is a selftest harness.

#### Shared code changed BEFORE §10z, which this section used to exclude

The scope line above said "§10z onwards" for most of the round, so the shared
hunks from the round's *first* half — §10j to §10y, `UPSTREAM-BUGS` 88, 89 and
90 — were never listed here at all. Found by computing the preprocessor nesting
at every changed line in `ab34492~1..HEAD` rather than by re-reading the
narrative, which is the only way that finds a file nobody thought to mention.

| file | change | what to check on macOS |
|---|---|---|
| `xml/xmlparse.cpp` | `Get_XML_date` passes `nDST = -1` ("work it out") instead of 0 ("standard time") (`UPSTREAM-BUGS` 88) | **provably a no-op there** — `platform/mfc_time.h:83` and `:97` hardcode `tm.tm_isdst = -1` and ignore the `nDST` argument entirely, which is *why* the port had been getting this right for months. Compile it; nothing else. |
| `utilities_shared.cpp` | `ExtractDirectory` accepts both separators on both platforms, later one wins (`UPSTREAM-BUGS` 89) | behaviour-identical there **by construction**: the POSIX arm's own both-separator logic is what became the shared body, and the only line still `#ifdef`-split is the no-separator fallback, which still answers `"./"`. This fix moves Windows to what macOS always did. |
| `mxp/mxpStart.cpp`, `mxp/mxpError.cpp`, `scripting/scripting.h` | `ExecuteLua` gained `pbReturnedAValue`, so the two MXP sites can tell "said nothing" from "said true"; `result` is seeded as well (`UPSTREAM-BUGS` 90) | **the one hunk of this block that changes live macOS behaviour, and it changes it for the engine macOS uses most.** A Lua `OnMXPopenTag` that falls off the end no longer vetoes the tag, and a Lua `OnMXP_Error` that merely logs no longer suppresses the `ON_PLUGIN_MXP_ERROR` broadcast to every other plugin. Seeding `result` also stops a handler that *throws* leaving an uninitialised read — `ExecuteLua` has four early returns that never reach the assignment. Check with the MXP suite and `--engines-selftest`. |
| `serialize.cpp` | `Frame.SetStatusMessageNow` is skipped under `MUSHCLIENT_TESTS`, because `Frame` is a macro that dereferences a null `m_pMainWnd` in the Windows test host | **inert there** — `MUSHCLIENT_TESTS` is never defined by the CMake build (it occurs in `tests/CMakeLists.txt` only inside a comment), so the `#ifndef` body always compiles and the call runs exactly as before. |
| `MUSHclient.cpp`, `MUSHclient.h`, `Utilities.cpp`, `doc_construct.cpp` | Windows test-host plumbing | every hunk is inside `#if defined(_WIN32) && defined(MUSHCLIENT_TESTS)`, measured; cannot reach macOS. |

Two findings from the same round are **macOS-only and deliberately not fixed
here** — `platform/mfc_collections.h`'s leaked walk cursor and `timers.cpp`'s
dropped status-line refresh. Both are described with proposed fixes in §10ap;
neither can be compiled on this machine, and the first is the container shim the
whole port stands on.

#### And the one thing Windows found that only macOS can fix — DONE

`docs/UPSTREAM-BUGS.md` 90's remaining half is closed. `ExecuteLua` now passes
`pbReturnedAValue` down through `DispatchEmbedded` and all five embedded engines
set it from the idiom each already used to spot a no-return handler, so a Python
`OnMXP_OpenTag` returning `None` no longer vetoes every tag. Proved by a new
`battery_returned_a_value` in `--engines-selftest`, green in all six languages
and measured against three mutations of the Python body.

**One thing this section had wrong, and it mattered:** the Tcl pointer said "the
non-`TCL_OK` arm". That is not where Tcl's no-return case lands. A proc that
falls off the end returns the empty string with `rc == TCL_OK`; the boolean
conversion then fails and leaves the seeded 1, so the one-armed veto arrived at
"keep processing" through the *success* path. Fixing only the arm named here
would have left Tcl broken while every other language looked fixed.

#### What the macOS run found (2026-08-22)

Round two merged into the port and was compiled by clang for the first time.
**It built clean on the first try** — 95 commits, no errors — and `ctest` came
back **76 of 77**, ASan **70 of 71**, with the same single failure in both.

- **`lint_doc_sections` was red, and only macOS could see it.** Round two added
  `## 89` … `## 109` to `docs/UPSTREAM-BUGS.md` without their index rows. The
  checker is a registered ctest entry, and §10i's Windows spike never reached
  the lint entries, so nothing there could have said so. Twenty-one rows
  restored; two of them disagree with their write-up and say why in the commit.
- **`docs/UPSTREAM-BUGS.md` 90's remaining half is closed** — see the amended
  subsection above. One correction landed with it: the Tcl pointer named the
  wrong arm.
- **Both blind edits are correct.** Functions List is not greyed with no world
  open, and the dispatch arm links and is reached; `hasMainWindow ()` is
  overridden and `GetInfo(249)/(250)` report the real client rect. Neither had a
  test, and neither would have been noticed breaking — putting `33046` back into
  `kNeedsWorld` left the whole suite green — so `menubar_selftest.cpp` now pins
  the first, and `world_frame_selftest.cpp` already pinned the second.
- **No sanitizer findings** from the round's five memory fixes (94, 96, 98, 100,
  102). `test_mxp`, `test_triggers`, `test_chat_sockets` and `test_chat_protocol`
  all ran under ASan. Read that as "the instrument found nothing", not as proof
  of absence — `docs/SANITIZERS.md` says why, and Guard Malloc rather than ASan
  is what made the last heap bug here deterministic.
- **Three guard-table rows are literally false and harmlessly so**, re-derived
  rather than re-read: `pal/win_worldview.cpp`'s header comment is outside its
  `#ifdef _WIN32` (and CMake never opens the file anyway);
  `platform/wx_mfc_hooks.h`'s `HeadlessMessageBoxAnswer` sits at depth 0, having
  been *moved* out of `#ifndef _WIN32` rather than added, byte-identical; and
  `MUSHclient.cpp` / `MUSHclient.h` / `doc_construct.cpp` each have one added
  **blank** line past the `#endif`. No token reaches macOS in any of them. The
  claims about `mainfrm.cpp`, `evaluate.cpp`, `scriptengine.cpp`,
  `win_transport.cpp` and `Utilities.cpp` are exactly right.
- **§10ap's "cannot be compiled here" blocker has expired.**
  `platform/mfc_collections.h`'s leaked walk cursor was left unfixed on the
  grounds that the header could not be compiled on the Windows host — true
  there, false here: it builds into `mushcore`, both adapter sets and all 43
  test executables, and `tests/test_collections.cpp` already includes it. The
  *other* half of that caution stands (11 map instantiations, 122
  `GetStartPosition` sites, and `5cb6681`'s history of a complexity regression
  in the sibling class), so the fix wants the allocation-free O(1) form and a
  timing pin, not the `find ()` form on its own. §10ap's rate is also an
  overstatement: it is one leak per *fired, matching* alias, not one per user
  command.

**And one new defect, found while checking a round-two fix rather than by
running it.** `docs/UPSTREAM-BUGS.md` 105 closed the route by which
`world.SetTimerOption` could leave an "every" timer at 0/0/0.0 — but it guards
only the `hour` / `minute` / `second` rows. `at_time` is a boolean mapped
straight onto `iType`, and setting it to 0 on a midnight at-timer runs
`SetBaseOptionItem`, flips the type to `eInterval`, and then *copies*
`iAtHour`/`iAtMinute`/`fAtSecond` into the `every` fields
(`methods_timers.cpp:896-911`) — arriving at exactly the 0/0/0.0 state the new
guard exists to prevent, and answering `eOK`. Verified by reading the code at
HEAD, not run. No test on either platform covers it: the two macOS `at_time`
sites (`test_lua_scripting.cpp:2441`, `test_timers.cpp:340`) both set it to 1.
Recorded rather than fixed, because closing it changes what an existing script
sees from a *second* method — that is a decision, not a follow-up.

### 10ag. `aca2478`, the last §10h row with no test anywhere

`CPluginsDlg::OnUpdateNeedDescription` read the selected row's `CPlugin*`
straight off the list control's item data with no `find ()`-in-`m_PluginList`
liveness check, unlike all five sibling handlers. §10w recorded it as
**"genuinely owed, with no test anywhere"**, needing "the dialog and its list
control". It now has one, and it is a unit test rather than the GUI walk that
row asked for — for a reason worth stating.

**No sequence of clicks arranges the hazard.** It needs a plugin to leave
`m_PluginList` between `LoadList ()` and the next idle, and the dialog's own
Remove button calls `LoadList ()` afterwards. What produces it is
`world.UnloadPlugin` or `ReloadPlugin` from a timer firing during the dialog's
modal loop. A test can arrange that in three lines; a person driving the GUI
cannot arrange it at all.

The port covers it in `platform/gui_bridge_selftest.cpp` against a
wxListCtrl-backed `CListCtrl`. The same shape works here against a **real Win32
list view** — a console process may create windows, and nothing about this needs
the application. `tests/win/test_gap_coverage.cpp`'s `PluginsDlgR6` creates a
hidden `STATIC` parent and a `WC_LISTVIEW` child, `Attach ()`es it to the
dialog's `CListCtrl` member and `Detach ()`es before it goes away, so MFC's
handle map never outlives the window.

**Two of the port's helpers do not exist in real MFC**, and the substitutes are
better. `AfxDispatchUpdateUI` and `CCmdUI::m_pCaptureEnable` are both
`platform/mfc_ui.h`'s additions. The framework's own equivalents are
`CCmdUI::DoUpdate` — the public entry the idle pump goes through — and the fact
that `CCmdUI::Enable` is **virtual**, so a two-field subclass records not only
what the handler answered but *whether it answered at all*. A captured `bool`
cannot tell "disabled" from "never called"; `sawEnable` can, and both assertions
are made.

**Revert-checked.** With the liveness check deleted and nothing else changed,
`stale_ui.enabled` stays true — the handler reads the erased plugin's
description and enables Show Info. The erased plugin is deliberately kept
*alive* (only removed from `m_PluginList`), so the test itself never touches
freed memory; only the check under test would.

That closes the row. It also settles the shape of the remaining §10h dialog
work: **"needs the dialog" is not the same as "needs a GUI session"**, and the
list-control barrier that this row and two of `44f0737`'s leftovers share — the
fitted last-column width, and `LoadPrefsP7`'s wrong-enum indexing, both recorded
in `tests/win/test_prefs_pages.cpp` as needing "a created `CListCtrl`" — is the
same twelve lines of fixture away.

### 10ah. `2e54bba` run on the real client, and how the GUI was driven

`tests/test_utilities.cpp` guards its `ShowFunctionslist` test `#ifndef _WIN32`
with a note that says exactly what was missing: *"re-entering the test below
needs a Windows-side observable, not a guard. This is §10h's 2e54bba row, and
it stays owed."* This is that observable.

**The fix.** `Help`'s fallback called `ShowFunctionslist (m_strFilter, bLua)`,
binding `bLua` to the second parameter (`nStartChar`, an `int`) instead of the
fourth — so the fourth kept its default `false` and the Function List never
showed Lua's own functions, while the bogus start offset also blanked the
filter. `CMainFrame::OnGameFunctionslist` had the identical mistake.

**The observable turned out to be a count.** `ShowFunctionslist` fills the
dialog from `InternalFunctionsTable` always, and adds `LuaFunctionsSet` only
when `bLua`. `LVM_GETITEMCOUNT` needs no buffer, so it reads cleanly across the
process boundary where item *text* would need `WriteProcessMemory`. Measured:

| world | route | items |
|---|---|---|
| `enable_scripts="y"`, Lua | Game ▸ Function List | **650** |
| `enable_scripts="n"` | Game ▸ Function List | **418** |
| `enable_scripts="y"`, Lua | `world.Help ("")` | **650** |

The 232-entry difference is `LuaFunctionsSet`, and 418 is precisely what the
defect produced. Both call sites the commit touched are covered: the menu
handler and the scripting method.

#### How the app was driven, since §10h's instrument note is now out of date

§10h records that posted `WM_CHAR` works but a posted `VK_RETURN` does not send,
and concludes that anything needing typed input must follow §8v — drain the
startup dialogs, confirm focus with `GetGUIThreadInfo`, then `keybd_event`.
**None of that was needed, and two of its assumptions did not hold.**

- **Menu commands do not need clicking.** `PostMessage (frame, WM_COMMAND, id, 0)`
  with the id from `resource.h` runs the same message-map handler a click would,
  with no focus race and nothing to synchronise. `GetMenu`/`GetSubMenu`/
  `GetMenuItemID` enumerate the live menu bar, so a command can be found by name
  and driven by number.
- **Dialogs are read, not photographed.** `EnumChildWindows` plus
  `GetDlgCtrlID` gives every control's id, class and text, which is evidence a
  screenshot is not. Buttons are pressed the same way as menu items —
  `WM_COMMAND` with `IDOK`/`IDCANCEL`.
- **Cross-process `WM_SETTEXT` does NOT work**, and neither did posted `WM_CHAR`
  here. Both hand the target process a pointer into the *sender's* address
  space, or arrive at a control that is not accepting them. This is the one
  place §10h's note is confirmed rather than superseded — and the answer is not
  a better keystroke.
- **Let the world drive itself.** A one-shot timer in the world file with
  `send_to="12"` (`eSendToScript`) runs arbitrary Lua three seconds after the
  world opens, with no input synthesis of any kind. That is how `world.Help ("")`
  was invoked, and it is the general answer for any scripting-reachable
  behaviour: put it in a timer, open the world, read the result.

What still needs real typed input is a much shorter list than §10h implies:
only the things with no scripting entry point at all — the macro `switch`
(`5359254`), the auto-say tri-state (`8468a76`), and tab-completion.

#### Housekeeping, recorded so the next session repeats it

The client keeps its preferences in the **working directory** first
(`MUSHclient.cpp:328`), so running it from `WinDebug` uses
`WinDebug\mushclient_prefs.sqlite` and cannot reach the installed copy under
`Documents\MUSHclientx64\`. Both were fingerprinted before the run: the
installed one verified unchanged afterwards, and the scratch one was restored
from a backup, since dismissing the "Thanks for upgrading" screen writes to it.

### 10ai. Two helpers hoisted to be testable, untested on the platform they ship for

`44f0737` hoisted `FittedLastColumnWidth` and `OutputBufferBytesNeeded` out of
the property pages and into `dialogs/world_prefs/prefspropertypages.h`, with a
comment saying exactly why: *"in the header so the headless suite can drive them
without linking a property page … Both were bugs precisely because they were
buried in a dialog nothing tests."*

They are tested — in `tests/test_mfc_ui.cpp`, which this target **excludes**,
for a reason that has nothing to do with them ("tests `platform/mfc_ui.h`, the
108 fake MFC classes"). So two functions in a shipping header, hoisted expressly
to be testable, had no coverage at all on the platform they ship for. The
assertions are now duplicated into `tests/win/test_gap_coverage.cpp` — no
fixture, no window, no document, the cheapest instance of this file's whole
premise. If `test_mfc_ui.cpp` is ever split so its shim half and its engine half
can be excluded separately, they become redundant and should go.

**`44f0737`'s eighth defect has no runtime observable and never will.**
`LoadPrefsP7` indexed the alias page's column arrays with the trigger page's
`CPrefsP8::eColumnSequence` — "two independent enums that happen to agree", so
the fix corrected an expression without changing a value. No test can watch it.
What *can* be pinned is the coincidence the old code rested on, and the right
instrument is a `static_assert`: the hazard is at edit time, and a failing build
is the only report that arrives before the damage.

With that, `44f0737` is as closed as it can be from a test: six of its eight are
covered here, the seventh is the static assertion, and the eighth
(`CGlobalPrefsP4::OnFont`'s seeding) needs a modal font dialog whose only
outputs are `UpdateData` and `SetWindowText` — both of which need a window even
with the dialog answerable.

### 10aj. Two of this document's own instructions cannot be followed on this machine

Recorded because both look like negligence otherwise, and the next session will
hit them in the first ten minutes.

**"`tests/mud_stub.py` is the loopback server; do not write another."** There is
no Python on this machine. `python`, `python3` and `py` all resolve to the
Microsoft Store *alias stubs* under
`%LOCALAPPDATA%\Microsoft\WindowsApps\`, which print an install advertisement
and exit non-zero; there is no interpreter in `C:\Python*`, `Program Files` or
`%LOCALAPPDATA%\Programs\Python`. So the instruction cannot be honoured as
written, and a PowerShell equivalent is the alternative — kept in the session
scratchpad rather than committed, precisely so it does not become the second
loopback server the instruction warns about. **If Python is ever installed here,
delete it and use `mud_stub.py`.**

**The `tests/check_*.py` lints have not run once during this campaign.** They
are `ctest` entries, and `ctest` is the macOS side; on Windows nothing invokes
them. That matters most for `check_win_test_sources.py`, whose whole job is to
notice when `MUSHclient.vcxproj` and `MUSHclient_tests.vcxproj` stop compiling
the same engine sources — the guard against exactly the drift this round has
been editing the generator to avoid. Its invariant was therefore checked by
hand at the end of this round instead: **286 `<ClCompile>` entries each, and the
two lists are identical.** Regenerating after every change achieves the same
thing, but "the generator was re-run" is not the same claim as "the lint
passed", and only the second one is a guard.

### 10ak. xterm-256 live on Windows, and the technique that got it

§4c-quintricies item 6 asked for "a real xterm-256 world, live, for a session".
Done, and the evidence is exact rather than visual.

A loopback stub sent SGR `38;5;196` (foreground), `48;5;21` (background) and
`0` (reset) around three marker lines; a one-shot timer in the world file then
walked `GetLinesInBufferCount` / `GetLineInfo` / `GetStyleInfo` and wrote every
line's style runs to a file. Measured:

| line | `GetStyleInfo` fore | back |
|---|---|---|
| `XTERM256-FOREGROUND-196` | **255** = `0x0000FF` | 0 |
| `XTERM256-BACKGROUND-21` | 12632256 (default) | **16711680** = `0xFF0000` |
| `PLAIN-LINE-AFTER-RESET` | 12632256 = `0xC0C0C0` | 0 |

`COLORREF` is `0x00BBGGRR`, so `0x0000FF` is pure **red** and `0xFF0000` is pure
**blue** — which is exactly xterm-256's 196 (`#FF0000`) and 21 (`#0000FF`). Both
directions resolve correctly and the reset restores the world's defaults. That
is more than a session's eyeballing would have shown.

#### The technique, because it generalises to most of what §10h has left

Three pieces, none of which needs a human or a synthesised keystroke:

1. **A loopback stub** that plays a script of `TEXT` / `RAW <hex>` / `WAIT`
   directives at the client and logs whatever the client sends back. Anything
   that arrives over the wire — ANSI, xterm-256, MXP, telnet negotiation,
   MCCP — can be put in front of the real client this way.
2. **A world file with a one-shot timer** (`send_to="12"`, `active_closed="y"`)
   that runs arbitrary Lua a few seconds after the world opens. No input
   synthesis at all.
3. **The world writes its own evidence.** `io.open` plus the `GetLineInfo` /
   `GetStyleInfo` family turns "what did the client actually display" into a
   file this side can diff. A screenshot could not have distinguished
   `0x0000FF` from `0xFF0000`; this does.

The timing is the one thing to get right: the timer is relative to world OPEN,
not to the connection, so it must be set later than the `ID_CONNECTION_CONNECT`
that follows. Six seconds was too early on the first attempt and the dump
contained only MUSHclient's own banner.

### 10al. The notepad family, over the real MFC doc/view

`test_notepad_methods.cpp` was the largest remaining exclusion — 18 tests, and
the only one of the seven "needs a port-only seam" rows whose reason named a
*seam* rather than a hostility. The reason was right and the conclusion drawn
from it was wrong.

It said the fixture needs `CTextDocument`'s twelve `m_fn_*` callbacks, which
`TextDocument.h` declares `#ifndef _WIN32`, so guarding the users would leave an
empty file. All true. What it implied — that the coverage therefore cannot exist
here — does not follow, because the callbacks are not a *seam to the same code*.
They are one of **two implementations**.
`scripting/methods/methods_notepad.cpp` is `#ifndef _WIN32 / #else` eleven times
over, and the `#else` half — the half every Windows user runs — had never had a
test on any platform.

So the right move was not to port the file. It was
`tests/win/test_notepad_win.cpp`: 19 tests, the same behaviours, over the real
`CTextView` in a real frame. **19 passed, 0 failed** on the first complete run;
the target is now **1638 passed, 0 failed, 181 suites**.

#### The observables are strictly better here

| port fixture | this suite |
|---|---|
| `applyFontCalls` counter | the `HFONT` the EDIT control answers `WM_GETFONT` with, and its `LOGFONT` face, weight and height |
| `applyColourCalls` counter | the frame caption `CTextView::OnUpdate` rewrites when `UpdateAllViews` reaches it |
| `readOnly` bool | the control's `ES_READONLY` style bit |
| `lastMove[4]` | the frame's real screen rectangle |
| `saveCalls` / `lastSavePath` | the bytes on disk |
| `closeCalls` | `::IsWindow(frame)` going FALSE, and the document leaving the template |

A counter fails only when nothing was called. Every entry on the right fails
additionally when the *wrong* thing was called, or the right thing with the
wrong argument. `NotepadFont`'s keep-current arm is the clearest case: on the
port "it re-applied" is `applyFontCalls` going up, and here it is the control
wearing a **different `HFONT` handle** — because `CTextView::SetTheFont` builds
a new `CFont` every time — while the face and size stay put.

#### What a console process needs in order to own a notepad

Four things, and the third is the one that is easy to get backwards:

1. **The document** through `RUNTIME_CLASS(CTextDocument)->CreateObject()`. The
   constructor is protected and `CreateInstance` — the port's way past it — is
   itself `#ifndef _WIN32`. `DECLARE_DYNCREATE` is the Windows way past it, the
   same route `test_accelerators.cpp` uses for `CMUSHView`.
2. **A real `CFrameWnd`.** Not optional and not cosmetic: `CTextView::OnUpdate`
   does `GetParentFrame ()->GetWindowText (...)` with no null check, and
   `ActivateNotepad`, `MoveNotepadWindow` and `GetNotepadWindowPosition` all go
   through `GetParentFrame ()` too. A windowless view is not an option here the
   way it was for the accelerator tests.
3. **`AddView` AFTER `Create`, not before.** `CView::OnCreate` asserts
   `m_pDocument == NULL`; the framework wires the document through a
   `CCreateContext` in `lpCreateParams`, which a hand-made view has none of.
   MFC traces "Creating a pane with no CDocument" for each one, which is the
   TRACE for exactly that and is expected output here.
4. **`App.m_pNormalDocTemplate`**, which `MC_TEST_WIN_DOC_TEMPLATES` already
   builds and `LoadTemplate`s.

Teardown is `CTextDocument::OnCloseDocument` — the same call `world.CloseNotepad`
makes — and nothing in the fixture deletes anything by hand: the frame destroys
the view, `~CView` removes it from the document, and the document, being out of
views, closes and deletes itself. A fixture that unpicks that by hand ends up
testing its own bookkeeping instead of MFC's.

The single access-control snag is that `~CView` is **protected** (the framework
deletes a view from `PostNcDestroy`, which never runs when `Create` failed and
there is no window). `delete (CObject *) view` is the same deletion — `CObject`'s
destructor is public and virtual — by a route the access specifier allows.

#### The one thing that does not work, and what it is worth anyway

**Creation.** `CreateTextWindow` goes through
`CMultiDocTemplate::OpenDocumentFile`, whose `CTextChildFrame` is a
`CMDIChildWnd` and needs the main frame's MDI client window — and this host
constructs `CMainFrame` without ever creating it (§10z). So it fails, loudly,
with three MFC assertions and a "Failed to create empty document" that the
message-box hook routes to stderr.

That does not cost the two tests that reach it. `SendToNotepad` and
`AppendToNotepad`-with-an-unknown-title are both asserting a *negative* — that
the call did not write into the notepad that already exists — and a creation
that fails cannot fake that. The drift the port's version of this test pins (a
twin that forwarded `SendToNotepad` to the append path) would fail here exactly
as it does there.

#### Two findings from writing it

- **`CDocument::SetPathName` canonicalises.** `SaveNotepad(title, "/tmp/x.txt",
  TRUE)` leaves `GetPathName()` reading `C:\tmp\x.txt`, because `SetPathName`
  runs `AfxFullPath`. The first version of the test asserted the string that
  went in, and was asserting that this does not happen. It now asserts the
  property that matters — the document points at a file, and that file is the
  one just written — which is true under any canonicalisation.
- **A failing save is reported, not swallowed.** `CDocument::OnSaveDocument`
  reports the `CFileException` through `AfxMessageBox`
  ("... contains an invalid path") and returns FALSE, so `SaveNotepad` answers
  FALSE. Only the message-box hook makes that assertable rather than a hang.

#### What is left of the exclusion list

Fourteen, of which eight are correct-by-design (they test the port's
substitutes: `CString`, the collections, `mfc_ui.h`, `mc_dispatch`,
`posix_socket`, the headless CDC, the posix transport, and the framework's own
`dup2` test). The other six are `test_sound.cpp`, `test_chat_sockets.cpp`,
`test_notepad_convert.cpp`, `test_pal_worldview.cpp`, `test_menu_rules.cpp` and
`test_pal_surface.cpp`. `test_notepad_methods.cpp` stays excluded on purpose and
its reason now names this file, because two copies of the same 18 assertions in
two dialects would be two places to keep in step — the same rule the header of
`tests/win/test_gap_coverage.cpp` states.

**The generalisable check**, and the third time this campaign has produced one:
*a reason that names a missing seam is a reason to ask whether the other side is
the same code.* When it is — `test_accelerators.cpp`, `test_lua_surface.cpp` —
the fix is a guard or a host change and the tests come across whole. When it is
not, as here, porting them would be the wrong move and writing the other half is
the right one. The exclusion reason cannot tell you which; only reading the
`#else` can.

### 10am. The port's menu-rule table, asked of MFC — and the one row that was wrong

`platform/menu_ui_rules.cpp` is a table that **restates another system's
behaviour**: wx leaves a menu item enabled when nobody handles it, MFC greys it,
so the port writes down which commands MFC greys. `tests/test_menu_rules.cpp`
pins the table faithfully — and a table restating a foreign system is only as
good as the reading it was written from, which had never been checked against
the system.

`tests/win/test_menu_rules_win.cpp` checks it. Ten tests, and it found one row
wrong on the first run.

#### The finding: Functions List is live on Windows and was greyed here

`ID_GAME_FUNCTIONSLIST` (33046) was in the port's world-only list, so with no
world open **Game ▸ Functions List is greyed on macOS**. On Windows it is not.
There are three handlers for that id, not one:

| handler | context it uses |
|---|---|
| `CSendView::OnGameFunctionslist` (`sendvw.cpp:2467`) | the input bar's text and selection |
| `CTextView::OnGameFunctionslist` (`TextView.cpp:1482`) | the notepad's text and selection |
| **`CMainFrame::OnGameFunctionslist`** (`scripting/functionlist.cpp:602`) | **none** — `ShowFunctionslist ("", 0, 0, true)` |

The frame's is the fallback, and `ShowFunctionslist` needs no world at all: it
fills a dialog from `InternalFunctionsTable` plus `LuaFunctionsSet` and jumps to
a help topic. So on Windows the item works with nothing open whatsoever.

Fixed in three places, all of them the port's: the id is out of
`menu_ui_rules.cpp`'s list, `wx_mfc_impl.cpp`'s dispatch grew a no-child arm
calling the same context-free `ShowFunctionslist`, and `test_menu_rules.cpp`
moves it from `kWorldOnly` to `kGlobal`. **None of that is compiled or run
here** — it is macOS code — so it is on the re-integration list with everything
else in §10af.

**Why the port's own reading missed it:** the sweep that built the table went
looking for the handler, found `sendvw.cpp`, and stopped. One id, three
`ON_COMMAND` entries in three files. The check that finds this shape is the one
`docs/UPSTREAM-DIVERGENCES.md` §16 already states for a different family —
*grep every occurrence, not the first* — and it is the same lesson as D-31's
"grep every overload in a set, not just the most visible definition".

#### The catch-all that makes "is it handled?" the wrong question

`mainfrm.cpp:238` is `ON_UPDATE_COMMAND_UI_RANGE (0, 0xFFFF, OnFixMenus)`, and
`doc.cpp:205` is the same on the document. **CMainFrame claims the update-UI
side of every command id there is.** Both handlers call `ContinueRouting`, so at
run time they strip an accelerator from the caption and step aside — but a probe
that asks "is there an update-UI handler for this id" is answered *yes* by the
catch-all, because a non-NULL `AFX_CMDHANDLERINFO` short-circuits
`_AfxDispatchCmdMsg` **before the handler runs** and therefore before
`ContinueRouting` can be observed.

The first draft of this suite asked exactly that question and got "all 24
world-only commands ARE handled with no world open", which is true and useless.
Worse, the mirror-image test — "every world-only id is claimed somewhere in the
world chain" — passed **vacuously**, because the document carries a catch-all
too. The only honest question in this app is what `DoUpdate` *enables*.
`MenuRulesWin.EveryIdHasAnUpdateUiHandlerAndAlmostNoneHasACommandOne` pins that
fact against id 31111, which is in no menu and no message map and still has an
update-UI handler.

#### What `CCmdUI::DoUpdate` actually returns

Worth writing down, because a second wrong assertion came from guessing it:

```
BOOL CCmdUI::DoUpdate (CCmdTarget * pTarget, BOOL bDisableIfNoHndler)
  m_bEnableChanged = FALSE;
  bResult = pTarget->OnCmdMsg (m_nID, CN_UPDATE_COMMAND_UI, this, NULL);
  if (bDisableIfNoHndler && !m_bEnableChanged)
    Enable (pTarget->OnCmdMsg (m_nID, CN_COMMAND, this, &info));
  return bResult;
```

The **return value** is "an update-UI handler ran", not "the command is
handled", and the two differ for every command with an `ON_COMMAND` and no
`ON_UPDATE_COMMAND_UI` — which is most of them. Those come back `FALSE` and
`enabled`. Assert on the outcome, never on the return.

#### A third shape the table did not have a name for

`ID_FILE_OPENWORLDSINSTARTUPLIST` (32954) is neither global nor world-only: it
is live or dead by a piece of *global* state,
`CMainFrame::OnUpdateFileOpenworldsinstartuplist` → `Enable
(!App.m_strWorldList.IsEmpty ())`. The first draft put it in the always-live
list and it failed, because this host's `LoadGlobalsFromDatabase` runs over no
database and the startup list is empty.

That one is **not** a divergence — `platform/menubar_selftest.cpp:708-713`
already drives both arms of the same condition on the port. But it is a reminder
that a two-bucket classification will mis-sort anything that belongs in a third
bucket, and the failure looks exactly like a bug until it is read.

#### What the suite covers, and what a bare view can be asked

Ten tests, no windows anywhere: the frame is the host's windowless `CMainFrame`
and the views are bare `DYNCREATE` instances, which is enough because a message
map is a static table and `OnCmdMsg` walks it whether or not the object owns an
`HWND`. Beyond the two lists, it runs the canonical handlers the port's table
*dispatches to* rather than restates — `OnUpdateConnectionConnect` across every
connect phase and its `0.0.0.0` placeholder test, `OnUpdateConnectionDisconnect`
staying live through all eight in-progress phases (the rule that has shipped
broken before), the Trace and Debug-packets check marks per document, and
`CSendView::OnUpdateSessionOpen` behind Quit From This World.

That last one is the first time `CSendView` has been **instantiated** in this
campaign — the class the port compiles and never constructs, and the home of
four of `docs/UPSTREAM-DIVERGENCES.md`'s entries.

Two mechanical notes for anyone extending it:

- **`RUNTIME_CLASS(T)` cannot be used in a template.** It token-pastes to
  `T::classT`, so inside `template <class T>` it names a member called `classT`
  on every class and nothing compiles. Pass the `CRuntimeClass *` in.
- **Never `RemoveView` a fixture's last view.** `CDocument::RemoveView` runs
  `OnChangedViewList`, which closes and *deletes* the document — fatal for one
  on the stack, a double delete for one on the heap. `~CDocument` calls
  `DisconnectViews` instead, which detaches without deleting, so deleting the
  document first is both simpler and correct. `tests/test_accelerators.cpp` had
  already learned this.

`tests/test_menu_rules.cpp` stays excluded: two thirds of it drives
`PortMenuRuleUiState` / `PortMenuActionForCommandId` / `kPortMenuId*`, which are
the port's own inventions with no Windows meaning. The lists are copied
literally into the Windows file rather than shared, deliberately — two files
asserting the same claim about two different subjects **is** the cross-check,
and deriving either from the other would make both agree with anything.

**1648 passed, 0 failed, across 182 suites.**

### 10an. The real menu, dumped — and why an id diff is the wrong diff

The port builds its menu bar by hand in `platform/wx_mfc_impl.cpp`. The Windows
client loads its menus from a resource. Nobody has ever compared the two, and
`tests/win/dump_menu_resource.ps1` now makes the Windows half printable:

| resource | menu | commands |
|---|---|---|
| 128 `IDR_MAINFRAME` | with no world open | 44 |
| 129 `IDR_MUSHCLTYPE` | a world | **147** |
| 130 `IDR_NORMALTYPE` | a notepad | 79 |

(`MUSHclient.rc` has no menus — they are in `locale\en.dll`, §10ac.
`LOAD_LIBRARY_AS_DATAFILE` is enough for `LoadMenu`.)

**The first attempt at the comparison was the wrong one, and the failure is
worth recording because it is tempting.** Extracting every id the port appends
and set-subtracting it from the 147 reports **67 missing commands** — and almost
all 67 are phantoms. The port renumbers: Always On Top is
`ID_MENU_VIEW_ALWAYSONTOP`, the Immediate window is `ID_MENU_GAME_LUA_EVAL`, a
new notepad is the port-local `40001`, and every standard File/Edit verb is a
`wxID_*` that never equals its `57xxx` MFC twin. An id is not the identity of a
menu command across these two builds; **the path and the caption are.** A real
audit therefore needs the port's menu dumped the same way and diffed by
`File > &Open World…`, not by `57601` — which is macOS-side work.

Two leads did survive checking against the port's source, and both are recorded
as **leads, not findings**: they are macOS UI, nothing here can run them, and
the port's menu builder has conventions of its own that a grep does not see.

- **View ▸ Info Bar (`ID_VIEW_INFOBAR`, 32988) has no port menu item.** The
  string "Info Bar" does not occur in any `platform/*.cpp`. The feature itself
  is fully implemented — `CWorldChildFrame::infobarShow/Text/...`
  (`wx_world_child.cpp:6522`) backs the whole seven-method `pal::IWorldView`
  infobar API — so a script can raise the bar and the user has no way to toggle
  it. Windows has the toggle — **confirmed on the live menu bar** (2026-08-21,
  `WinDebug\MUSHclient.exe` with a world open): id 32988, a checkable `View`
  item, ticked by default. This is no longer a lead about the resource; it is
  what the running client offers and the port does not.
- **Edit ▸ Notepad's `Ctrl+Alt+W` is bound nowhere.** Windows'
  `ID_EDIT_NOTESWORKAREA` (32333) is `&Notepad\tCtrl+Alt+W`; the port's nearest
  item is `40001 "New Notepad"` with no accelerator, and `Alt+W` appears in no
  `platform/*.cpp`.

Neither is in the *Settings that are deliberately NOT coming back* table, so
neither has been triaged before.

**The reusable part is the script**, not the two leads. Any question of the form
"does the port's menu match the client's" now starts with a text file instead of
a screenshot, and the same three resources carry the captions and accelerator
text that a caption-level diff needs.

### 10ao. `pal/win_transport.cpp` runs for the first time

The file's own header says what it needed:

> CANNOT be compiled or run in the macOS port environment: correctness is
> by-construction/reasoning (each mechanic is the exact original call
> sequence), the same discipline as `pal/gdi_surface.cpp`. **Windows CI is the
> eventual empirical arbiter.**

Every world connection on Windows goes through it. It replaced `CWorldSocket`
(worldsock.cpp), `CMUSHclientDoc::ConnectSocket`, and the
`LookupHostName`/`HostNameResolved` pair, and until now nothing had executed a
line of it. It is also the least forgiving place for by-reasoning parity: a
transport that neither connects nor reports a failure is a client that hangs on
"Connecting…", and there is no test elsewhere in the suite that touches it.

`tests/win/test_win_transport.cpp` is 14 tests over 127.0.0.1 against a listener
the file creates itself — resolve (success, failure, cancel, destroy-mid-lookup),
connect (success, refused, non-reentrant delivery), bytes both ways, the big
buffered send, peer close, local close, delete-from-inside-a-callback, and the
two engine-seam tests below. `tests/mud_stub.py` is deliberately not used: it is
a separate process, and these need a socket the assertion can drive byte by
byte. **All 14 green; the target is 1662 passed, 0 failed, 183 suites.**

#### The finding: a send during the DNS lookup makes an illegal MFC call

`ConnectSocket` calls `CreateTransport ()` and then, for a hostname that is not
already an IP, sets `eConnectMudNameLookup` and returns to wait for DNS. For the
whole of that window the doc has `m_pTransport != NULL` and the adapter's
`CAsyncSocket` has `m_hSocket == INVALID_SOCKET`, because the adapter creates
its socket in `connect ()`, not in the factory. Anything that reaches
`SendPacket` in that window reaches `flushSend`, whose error branch calls
`AsyncSelect` / `ShutDown` / `Close` on a socket that does not exist.

Measured, not reasoned: **two `_CRT_ASSERT`s per call**, `sockcore.cpp:386`
(`ASSERT (m_hSocket != INVALID_SOCKET)`) and `:389` (`ASSERT
(pState->m_hSocketWindow != NULL)`) — both from the single `AsyncSelect`. In a
Debug build of the product that is a modal Abort/Retry/Ignore box during an
ordinary connect; in Release the calls fail harmlessly and `m_outgoing.clear ()`
**silently discards** what the engine just sent.

**Upstream could not reach it.** `ConnectSocket` at `249534f^` did
`m_pSocket = new CWorldSocket (this)` followed immediately by
`m_pSocket->Create (…)`, before the phase was set and before the lookup — so a
non-NULL `m_pSocket` always had a real handle and `SendPacket`'s NULL check was
sufficient. Creating the socket lazily is what opened the window.

Fixed by giving `flushSend` the guard `close ()` five lines away already has and
documents (*"real MFC's AsyncSelect ASSERTs on an invalid handle"*). The bytes
stay queued rather than being discarded, so they go out on the first `FD_WRITE`
once connected — which is what "append and it drains on writability" promises,
and the test now asserts that they arrive.


#### What actually reaches it: a timer with "Send to: World"

This took three tries to get right, and the two wrong answers are as useful as
the right one, so all three are here.

**Not a script.** `Send`, `SendImmediate`, `SendNoEcho`
(`scripting/methods/methods_sending.cpp:23`, `:41`, `:54`) and `SendPkt`
(`methods_utilities.cpp:479`, plus its Lua twin at `lua_methods.cpp:5290`) all
open with `if (m_iConnectPhase != eConnectConnectedToMud) return eWorldClosed;`.
Measured in the running client: `world.Send` during a lookup returns **30002**
(`eWorldClosed`) and sends nothing.

**Not typed input either.** A command from the command bar, an alias, or
`world.Execute` goes through `EvaluateCommand`, which opens with
`CheckConnected ()` (`evaluate.cpp:118`, `:205`, `:1085`). That function tests
`m_iConnectPhase != eConnectConnectedToMud || !ConnectionOpen ()` and, mid-connect,
puts up **"The connection to *world* is currently being established."**
(`doc.cpp:2792`) and refuses. The message box turning up unbidden in a scripted
GUI run is how this was found — it is the client saying no.

**A timer is.** `eSendToWorld` — send_to 0, the default for every trigger,
alias and timer — dispatches at `doc_shared.cpp:5472` straight to `SendMsg`,
which has no gate at all, into `DoSendMsg`, whose only gate is
`ConnectionOpen ()`. `CheckConnected` is nowhere on that path. And a timer with
**Active when world is closed** ticked (`active_closed="y"`) is *designed* to
fire while there is no connection.

Measured in the running client, `WinDebug\MUSHclient.exe`, with a world whose
site is a name that cannot resolve and a one-shot timer at 1.0 s sending
`hello-from-a-timer` to the world:

```
connect phase (227): 1        <- eConnectMudNameLookup
connecting?  (107): true
bytes out    (217): 20        <- 18 characters + CRLF
```

Twenty bytes handed to `SendPacket` while the client was still resolving the
name, i.e. straight into `flushSend` with `m_hSocket == INVALID_SOCKET`. Before
the fix that is two `_CRT_ASSERT`s and the bytes discarded; after it they are
queued and go out if the connect completes.

#### The underlying cause: `ConnectionOpen ()` stopped meaning what it says

```cpp
// The "socket exists" half of the classic connectivity test
// (m_iConnectPhase == eConnectConnectedToMud && the connection object).
bool ConnectionOpen (void) const { return m_pTransport != NULL; }
```

Upstream it was `m_pSocket != NULL` (`doc.h:656` at `249534f^`), and `m_pSocket`
was assigned only after `Create ()` had succeeded — so **"the connection object
exists" and "a socket exists" were the same statement, and the comment was
true.** A factory that builds the transport ahead of its socket pulled them
apart. Nothing at `ConnectionOpen ()` had to be edited for it to become wrong.

`DoSendMsg` is the only Windows-reachable caller where the difference bites. The
other five all pair it with an explicit
`m_iConnectPhase == eConnectConnectedToMud` (`doc.cpp:2122` and `:2782`,
`doc_shared.cpp:4811`, `mainfrm.cpp:740`, `dialogs/SendToAllDlg.cpp:62` and
`:87`) — which is itself the tell: five sites already distrust the predicate
enough to add the phase check beside it, and the sixth is the one that did not.

**The check that finds this shape: when a lifetime moves, grep every predicate
that tests whether the thing exists, and read its comment as a claim rather than
as documentation.** A rename would have caught it; a relocation did not have to
touch the line at all.

The `flushSend` fix stands on its own — the adapter must not make an illegal
call whoever provokes it. Tightening `DoSendMsg` is **flagged, not taken**: it
decides whether a timer's output during a lookup is dropped or queued and
delivered on connect, and that is a behaviour choice rather than a repair. The
`flushSend` fix currently makes it "queued and delivered", which is new — the
bytes used to be discarded on both platforms.

**The general shape, and it is the mirror of one already recorded.**
`pal/win_transport.cpp` says "Structure mirrors `platform/posix_transport.cpp`
1:1 (that adapter is the live-verified reference; reviewing this file is a diff
exercise against it)". The two `flushSend` bodies **are** identical, character
for character in the error branch. They behave differently because the
`CAsyncSocket` underneath is not the same class: the port's shim
(`platform/posix_socket.h`) tolerates an invalid handle, real MFC asserts on it.
CLAUDE.md already records *"a shim can satisfy an API's signature and not its
protocol"* from the multi-select file dialog; this is that in reverse — **the
shim is more permissive than the real thing, so parity-by-structure against the
shim cannot see the difference.** Any file whose correctness argument is "it
matches the POSIX adapter" inherits this, which is `pal/win_worldview.cpp` and
`pal/gdi_surface.cpp` as well.

#### The second finding: the bytes-sent statistic was doubled upstream

Reading `CWorldSocket::OnSend` out of history to check the one-`Send`-per-
`FD_WRITE` structure — which the adapter **does** reproduce faithfully, partial
sends and all — turned up the line beside it: `pDoc->m_nBytesOut += count` for
every chunk drained, on top of `SendPacket`'s `m_nBytesOut += nBufLen` for the
whole buffer, with `SendPacket` calling `OnSend (0)` itself. Stock MUSHclient
reports exactly double the bytes sent, on the Info page, in `world.GetInfo (217)`
and in `world.Debug`. Received bytes are counted once and are right, so the
statistics page shows an inflated out:in ratio that looks plausible.

The relocation dropped the wire-side count deliberately and nothing recorded
that, so it is now `docs/UPSTREAM-BUGS.md` 92 and
`docs/UPSTREAM-DIVERGENCES.md` § 20, with
`WinTransport.BytesSentAreCountedOncePerSendPacket` pinning it.

#### `McTestAssertionsRaised ()`

New in `tests/win/all_tests.cpp`, and the reason the first finding is a test
rather than a note. MFC asserts where a Win32 handle is required and absent, so
"did this call reach a state MFC considers illegal" is an observable — but until
now it only reached stderr, where it is one line among hundreds and only found
if somebody happens to read them. The counter makes it a question a test can
put directly, the same way `McTestDialogsCancelled ()` did for modal dialogs.
It counts `_CRT_ASSERT` reports only.

#### What is NOT covered, and why

- **TLS.** `startTls` wants a server with a certificate chain; a self-signed
  loopback server exercises the failure arm and not the success one, which is
  the arm that matters. Left out rather than half-done. This is now the largest
  untested part of the Windows connection path.
- **Keepalives.** `connect` applies `SIO_KEEPALIVE_VALS` when
  `options.keepalives` is set. There is no getter, and the adapter's `SOCKET` is
  private, so `SO_KEEPALIVE` — which that ioctl does set, and which *is*
  readable — cannot be reached from outside. A visibility gap, not an
  untestable mechanic.
- **The buffered send queue, still.** `ASendTooBigForTheSocketBufferIsQueued
  AndFullyDelivered` delivers 8 MB correctly and in order, and reports that the
  queue **was not used**: Windows auto-tunes the send buffer on loopback and
  absorbed the whole payload in one `send ()` even with the peer's `SO_RCVBUF`
  pinned to 1 KB and the peer not reading for half a second. The test asserts
  `onWritable` fired only when it can see the queue was needed, and says so when
  it was not — an assertion that cannot distinguish "correct" from "never ran"
  is worse than none. Forcing a partial send on Windows loopback needs a lever
  this test does not have.

### 10ap. A read-only agent sweep, adversarially verified — 13 claims, 5 survived

Six independent readers over six areas (`pal/win_transport.cpp`,
`pal/win_worldview.cpp`, the uninitialised-read family, the input parsers,
lifetimes/ownership, and shared files with `#ifndef _WIN32 / #else` pairs), each
followed by a checker told to **refute** its findings. Read-only throughout — no
edits, no builds, no running the app — so it could run alongside GUI work.

**13 claims, 8 refuted, 5 confirmed.** The refutations are the interesting half
and are summarised below, because a sweep that reports everything it thinks of
is worth less than one that discards two thirds.

Of the five survivors, **two were reproduced and fixed**, one was reproduced and
**deliberately not fixed**, and two are macOS-side and recorded for the
re-integration run.

#### Reproduced and fixed

Both have full entries in `docs/UPSTREAM-BUGS.md`:

- **92→94, MXP `</p>` uses the line a gagging trigger just freed.**
  `MXP_CloseTag` caches `pLine`/`pStyle` from a backwards scan, dispatches, and
  uses them again unchecked; `</p>` dispatches to `StartNewLine (true)`, which
  runs `ProcessPreviousLine`, where "omit from output" does
  `delete pLine; m_LineList.RemoveTail ()`. The line held IS the tail.
  Reproduced as an access violation writing `0xDDDDDDDD` inside
  `CPtrList::RemoveAt`, one frame under `mxpClose.cpp:337`. Shared code, so the
  same defect ships on macOS.
- **93, "Save notes to file" is an Open dialog.** `CFileDialog (TRUE, …)` with
  `OFN_OVERWRITEPROMPT`, then `CFile::modeCreate`. Measured on the running
  client: default button `&Open` before, `&Save` after.

#### Reproduced, and fixed the next day (§10ar)

**`GetInfo (249)` and `(250)` report a fabricated size while the client is
minimised.** `pal/win_worldview.cpp`'s `mainClientRect ()` returns
`Rect{0,0,0,0}` for "there is no main window" **and** for "there is one and it
measures empty", and `CMUSHclientDoc::GetMainClientRect` treats `w <= 0 || h <= 0`
as the former, so `methods_info.cpp` falls back to
`(m_gui_rows + 2) * m_gui_font_height` and `m_gui_cols * m_gui_font_width`.
Those four members default to 24/80/18/10 and **nothing on Windows ever writes
them** — their only writers are under `platform/`, which the Windows project
does not compile. Upstream had no fallback at all: `Frame.GetClientRect (&rect);
SetUpVariantLong (vaResult, rect.bottom);`.

Measured in the running client, one timer reading and asking to minimise and a
second reading afterwards:

```
restored   h=931  w=1616
minimised  h=468  w=800
(the no-window fallback is h=468 w=800)
```

`IsIconic` on the frame was TRUE and `GetClientRect` from another process gave
0 × 0, so the state is real and the numbers are invented. A plugin laying
miniwindows out against the main window gets a plausible-looking rectangle where
upstream gave it a 0 it would have skipped on.

**Left unfixed when this section was written**, on the grounds that the honest
fix needs the seam to express three states rather than two — which touches
`pal/iworldview.h`, both adapters, the test fake and a pinned assertion, none of
which except the Windows adapter can be compiled here. That was the right
reading of the cost and the wrong conclusion: the change is additive, the new
predicate defaults to the old behaviour, and the two blind edits are three lines
each. It is fixed; §10ar is the change and the measurement.

**The first draft of this measurement was wrong, and the reason generalises.**
`world.DoCommand ("MinimiseProgram")` **posts**; reading `GetInfo` in the same
script reported the restored size and looked like a refutation. Anything driven
through `DoCommand` needs the message loop to run before its effect is
observable, so the read has to be in a *later* timer.

#### macOS-side, recorded not fixed

- **`platform/mfc_collections.h`, `CTypedPtrMap::GetStartPosition` leaks a
  heap cursor on any walk that stops early.** The token is `new`ed by
  `GetStartPosition` and `delete`d only when `GetNextAssoc` reaches `end()`, so
  the canonical MFC idiom — walk until you find it, then `break` — leaks one
  token per walk. Real MFC allocates nothing (`BEFORE_START_POSITION` is a
  sentinel). `CAliasMap`, `CTriggerMap`, `CTimerMap` and `CVariableMap` are all
  this shim off Windows. `evaluate.cpp`'s alias lookup has exactly this shape,
  so it is **one leak per user command** for the life of the session, plus the
  early exits in `xml/xml_load_world.cpp` and `dialogs/plugins/PluginWizard.cpp`.
  A fix that needs no allocation: make the POSITION a pointer to the map node
  and re-find it with `map_.find (key)` to advance — allocation-free and
  leak-free, at `O(log n)` per step. **Not applied**: this is the container shim
  the whole port stands on, and it cannot be compiled here.
- **`timers.cpp`'s non-Windows arm drops the 5-second status-line refresh.**
  The Windows arm re-runs `ShowStatusLine ()` when `m_tStatusDisplayed` is more
  than five seconds old; the `#else` arm has only the log flush, and its comment
  does not mention the omission. The tell is that `m_tStatusDisplayed` is
  written three times off Windows and read **zero** times — a member with
  writers and no readers is a dropped consumer. The port's status bar is a
  different mechanism (`CWorldChildFrame::UpdateStatusBar`, driven by events),
  so the fix is not the Windows line verbatim, which is why it is recorded
  rather than pasted across.

#### What killed the other eight

Worth keeping, because most of them are shapes that will be claimed again:

| claim | why it fell |
|---|---|
| `flushSend` skips the `INVALID_SOCKET` guard | **The guard is on the two lines immediately above the cited line** — the finder read the file as it was before `eed6351`, which had landed hours earlier. |
| `flushSend`'s hard-error arm relies on an `FD_CLOSE` that never comes for a local shutdown | line numbers stale by 18 for the same reason, and the arm it describes is the TLS one |
| two claims that a failed `CFile::GetStatus` leaves an **uninitialised `CTime`** | v143's `CTime()` is `m_time (0)`, not the empty body the claim quoted from an older header; the project pins v143 in both configurations |
| a short `CFile::Read` publishes uninitialised heap | `CFile::Read` throws on error rather than returning short |
| `Phase_UTF8` writes one past `m_UTF8Sequence[5]` | the arithmetic is right and the write is in padding on every ABI this builds for; the claim conceded it in its own last sentence |
| `addToCommandHistory` fails to reset `CSendView::m_HistoryPosition` | not a relocation defect — the Windows code it replaced never reset it either |
| `ChangeInputFont` forwards a raw `nHeight` | that is the documented seam contract, and the arm has no caller |

Two lessons in that table. **Half the refutations turned on the tree having
moved under the reader** — a sweep run against a branch being actively edited
must have its line numbers re-checked, and the verifier catching its own
sibling's staleness is the mechanism working. And **an MFC or CRT claim needs
the toolset's actual header**, not a remembered one: two claims died on
`CTime`'s default constructor differing between MFC versions.

### 10aq. Driving the world-properties sheet, and the invariant that found a bug

The settings audits so far have asked "does anything read this setting?". This
asks a different question, of the running client, with a checkable answer:

> **Opening a world-properties page and pressing OK must not change the world
> file.**

`tests/win/drive_world_prefs.ps1` opens a world, saves it twice to establish
that a no-op save is stable, then for each of the twenty-two Game ▸ Configure
pages posts the menu command, presses a button, saves again and diffs. Any diff
is a page whose load and save halves disagree — asked here of real MFC DDX
rather than of the port's dialog bridge.

It found `docs/UPSTREAM-BUGS.md` 95 on the first page it tried:
`LoadPrefsP23` fills the Chat page's defaults into **the document** instead of
the page, so merely opening the sheet — at any page, since the sheet builds all
twenty-three — writes `chat_name`, `m_IncomingChatPort` and the chat colours,
and **Cancel cannot take them back**. No modified flag is set either, so the
values wait quietly in the document until something else saves.


#### The result, once the driver stopped lying

All twenty-two pages, opened and OK'd on a world with default settings:

| page | result |
|---|---|
| MUD name/IP address (32295) | `chat_name="Name-not-set"` added |
| the other twenty-one | **clean** |

The one non-clean row is **expected and is not the bug**. `SavePrefsP23` runs
unconditionally on OK, so the first OK commits whatever the load put in the Chat
page — which, on a world that has never had a chat name, is the default. That is
the design: the sheet loads every page, shows the user sensible values, and
commits them. What was wrong, and is fixed, is that **Cancel** committed them
too. Every subsequent page reports clean because the value is now in the file.

**An intermediate version of this run reported twenty of those rows as
"clean" when it had tested nothing at all** — the Logging page's confirmation
was still up, so the sheet never closed and the next twenty menu commands went
nowhere. The driver now checks for a leftover dialog before believing its own
diff, and prints `UNTESTED - a modal is stuck` instead. Then it learned to
**answer** the box (IDYES, else IDOK) rather than close it: WM_CLOSE means "no,
that is not intentional", which sends the user back to the page and is exactly
how the sheet got wedged. A driver that reports a pass for work it did not do is
worse than one that stops, and this one did both in turn.
#### Four things the driver had to learn, all of which make a run lie

Each of these produced a plausible, wrong result first:

- **`date_saved` and the `<!-- Saved on … -->` comment differ between two
  identical saves.** Without normalising them the invariant is a clock. The
  first run reported eighteen lines of pure timestamp; the second still tripped
  on the comment when a minute boundary fell mid-run.
- **A posted `WM_COMMAND(IDOK)` closes a property sheet, but `DoModal` returns
  IDCANCEL.** The sheet vanishes, the run looks orderly, and the entire save
  half never executes — so every page reports "clean" for the wrong reason.
  `GetDlgItem(sheet, IDOK)` and a `BM_CLICK` on the button is the difference
  between testing OK and testing Cancel twice. This one is worth remembering
  for anything driving MFC dialogs: **the sheet closing is not evidence that OK
  happened.**
- **A validation message box wedges the rest of the sweep.** The Logging page's
  OK asks *"You are not logging output from the MUD — is this intentional?"*
  (by design). A driver that closes stray dialogs by title learns nothing;
  reporting the longest static in each leftover dialog turns "something is
  stuck" into the actual sentence.
- **Start-up modals do not all arrive at once.** With fresh preferences the
  client shows an upgrade splash *and* Tip of the Day, the second of which
  turned up after the property sheet on one run and swallowed the click. The
  driver now closes dialogs until two seconds pass with none.

#### And two the world files taught

Both cost a run each, and both are silent-until-modal:

- **An unknown world attribute is a modal.** `auto_connect="n"` is not a world
  attribute, and the client stops with *"Line 11: Attribute not used"* before
  anything else happens.
- **`<script>` is not a world tag, and `--` inside an XML comment is not legal
  XML.** The second gives *"Invalid '--' inside comment (Cannot load)"*. Inline
  Lua belongs in a `send_to="12"` timer's `<send>`, which is §10ak's technique
  and remains the only route that works.

`world.DoCommand` **posts**, as §10ap records: any effect it has is invisible
until the message loop runs, so a probe must read it from a *later* timer.

### 10ar. Giving the worldview seam a third state, and the minimised client

§10ap measured `GetInfo (249)`/`(250)` answering a fabricated **468 × 800**
while the client was minimised, and left it, reasoning that the fix needed the
PAL seam changed on both sides at once. That was the right cost estimate and the
wrong call — the change is *additive*, so the two edits that cannot be compiled
here are three lines each and cannot alter existing behaviour if they are wrong.

#### The defect, in one sentence

`pal::IWorldView::mainClientRect ()` answered `Rect{0,0,0,0}` for **two
different things** — "there is no main window" and "there is one and its client
area measures nothing" — and `CMUSHclientDoc::GetMainClientRect` read the empty
rect as the first:

```cpp
pal::Rect r = m_pWorldView->mainClientRect ();
if (r.w <= 0 || r.h <= 0) return false;        // <- cannot tell them apart
```

A minimised main frame is the second. `GetClientRect` on it really does report
0 × 0 — measured from another process on the live client, with `IsIconic` TRUE —
so `methods_info.cpp` fell through to
`(m_gui_rows + 2) * m_gui_font_height` and `m_gui_cols * m_gui_font_width`.
Those four members are written only under `platform/`, which the Windows project
does not compile, so on Windows they are permanently 24/80/18/10. Upstream had
no fallback at all — `Frame.GetClientRect (&rect); … rect.bottom` — and answered
0.

#### The fix: `hasMainWindow ()`

A new non-pure predicate on the seam, **defaulting to `false`**. That default is
the load-bearing part: an adapter that does not override it behaves exactly as
it did before the predicate existed, so the change cannot regress an implementer
that is not updated.

| | |
|---|---|
| `pal/iworldview.h` | `virtual bool hasMainWindow () const { return false; }` |
| `pal/win_worldview.cpp` | `return frame () != NULL;` — `frame ()` already answers exactly this, being NULL unless `App.m_pMainWnd` is both present and *created* |
| `platform/wx_world_child.cpp` | `return true;` — the adapter *is* the window |
| `doc.h` | gate on `hasMainWindow ()`, not on the empty rect |

Nothing else changed: `mainClientRect ()` still reports what it measures.

#### Measured, three ways

**In the running client**, the same two-timer probe as §10ap (the first reads
and asks to minimise, the second reads after the message loop has run, because
`world.DoCommand` posts):

```
                    before          after
restored            931 x 1616      931 x 1616
minimised           468 x 800       0 x 0        <- upstream's answer
```

**In the suite**, `MainClientRectR6` in `tests/win/test_gap_coverage.cpp` — three
tests, and the third is the whole regression in one flag: two blocks with an
identical empty rect, differing only in `hasMainWin`, which must now answer
differently. Before the fix the second block answered the fallback. The first
test drives the **real** Windows adapter and pins the arm the fix had to leave
alone: the host's `CMainFrame` has no window, `hasMainWindow ()` is false, and
`GetInfo (249)` still falls back.

**On the port's side**, `tests/test_info.cpp` and `tests/test_pal_worldview.cpp`
each gained the same distinction. Their existing "a view with no window must
fall back" cases used to express that by zeroing the rect; they now set the flag,
which is what they always meant, and each gained a case for the state the flag
made expressible.


#### The rest of the sentinels, checked

Having found two, the obvious question is how many more there are. Every
non-pure default across the three PAL seams was checked against *"can a real
measurement also produce this value, and does any caller care?"*:

| seam value | verdict |
|---|---|
| `mainClientRect () -> {0,0,0,0}` | **the bug** — a minimised window measures exactly that |
| `isPaused () -> false`, `mouseCaptured () -> false` | a real adapter answers false too, and "not paused" / "not captured" is the safe reading either way |
| `setCommandWindowHeight () -> false`, `setOutputWidthPixels () -> false` | false means "did not happen", which is true of both the default and a refusal |
| `outputRect ()` returning zeros | its only two callers are `#ifndef _WIN32`, so it is unused on Windows entirely |
| `outputAreaChars ()` returning zeros | "dormant on every platform" by its own comment; the engine reads `m_gui_cols`/`m_gui_rows` instead |
| `ITransport::lastResolveError () -> 0` | a Winsock failure code is never 0, so 0 unambiguously means "nothing to report" |
| `ISurface::contentGeneration () -> 0` | the hazard is stated at the declaration *and* honoured at the one call site (`gen != 0 && …` in the miniwindow mask cache) |

So: one real instance, one near-miss that its author had already written a
warning for, and the rest sound. The check is cheap enough to be worth running
again whenever a seam gains a non-pure default.

#### Why this is the third time the same shape has come up

`docs/UPSTREAM-DIVERGENCES.md` and CLAUDE.md already record two: *a shim can
satisfy an API's signature and not its protocol* (multi-select file dialogs), and
its mirror in §10ao (*a shim more permissive than the real thing hides a
difference*). This is a third: **a seam that encodes two different facts in one
value.** `Rect{0,0,0,0}` was chosen as a sentinel because "no window" and "empty"
seemed like the same answer, and they are — right up until a real window can be
empty. The check that finds more: for any seam value doing double duty as a
sentinel, ask whether the sentinel is *reachable as a real measurement*. Here it
took one keystroke to reach.

### 10as. Every `IWorldView` method with nothing behind it — one test, one bug

`pal/win_worldview.cpp` is ~1200 lines of port-introduced code shipping to every
Windows user: about fifty methods forwarding to `CMUSHView`, `CSendView`,
`CChildFrame` and `CMainFrame`. `tests/test_pal_worldview.cpp` drives the
**port's** adapter and is excluded here, so the Windows one had no test at all.

`tests/win/test_win_worldview.cpp` calls **every method** on the real adapter
with no main-frame window and no view, with deliberately unhelpful arguments
(negative line numbers, empty strings, out-of-range window-status codes), and
asserts two things: the process survives, and **`McTestAssertionsRaised ()` does
not move.**

The second is the point. A null dereference is an access violation and gets
noticed. What does *not* get noticed is a method that "works" by calling
`CWnd::SetWindowText` on a null `m_hWnd` — real MFC asserts
`ASSERT (::IsWindow (m_hWnd))`, which in a Debug build of the product is a modal
Abort/Retry/Ignore during ordinary use and in Release is a silent no-op the
author never intended. §10ao added the counter for one such case; this is what
it is for.

This also finally pins the six null-guards added to this file earlier in the
round. §10v's lesson — *"a guard that has to dereference the thing it is
guarding is not a guard"* — cost three crashing suites and two exclusions, and
nothing had been pinning the fix.

#### The bug it found: `saveWorld ()` was inverted

The first run of the blanket sweep did not report a missing guard. It threw:
`CWinApp::DoPromptFileName`'s `ENSURE (m_pDocManager != NULL)`, out of
`saveWorld ()`, which read

```cpp
// Save with no name (methods_utilities.cpp): !DoSave == success.
return !m_pDoc->DoSave (m_pDoc->GetPathName (), TRUE);
```

Two defects in one line.

**It is inverted.** The comment is quoting `CMUSHclientDoc::Save`
(`methods_utilities.cpp`), whose own comment says *"returns zero if no error"* —
that is an **error code**, and `world.Save` hands it straight to the script.
Pasting the expression into a seam whose return is a **boolean** flips it:

| | on a successful save |
|---|---|
| `pal/iworldview.h` contract — *"false = no path"* | true |
| `platform/wx_world_child.cpp` — `DoSave (...) != FALSE` | true |
| `tests/fake_worldview.h` | true |
| **`pal/win_worldview.cpp`** — `!DoSave (...)` | **false** |

**And it ignores the "no path" clause.** With an empty path name `DoSave`
prompts, which is not what *"save the world file to its current path"* means —
and prompting is what threw here, because this host has no document manager.
The wx adapter returns false early; the Windows one did not.

**Latent, not live.** Nothing calls `CMUSHclientDoc::SaveWorldViaView` in
production — `world.Save` still reaches `DoSave` directly — so the two adapters
have been disagreeing at rest. Which is exactly why it is worth fixing now: the
cost lands entirely on whoever wires the seam up, at which point `world.Save`
starts reporting failure for every successful save on Windows and success on
macOS, and the test fake agrees with macOS so a shared test would not catch it.

Fixed to match the contract and the other adapter, and pinned by
`WinWorldView.SaveWorldAnswersTrueForASaveThatHappened`, which asserts both
halves: no path → false with no assertion raised, and a real path → true with
the file on disk.

#### The general shape

**A seam is a place where two conventions meet, and one of them has to lose.**
`CMUSHclientDoc::Save` returns an error code; `pal::IWorldView::saveWorld`
returns a boolean. The adapter author copied a working expression across that
boundary along with a comment asserting it was right. The comment is the tell —
*"!DoSave == success"* is true of the function it was copied from and false of
the function it was pasted into, and nothing checks a comment.

Same family as §10ar's sentinel-that-is-also-a-measurement and §10ao's
shim-more-permissive-than-the-real-thing: **all three are a value crossing a
boundary while its meaning quietly does not.**

### 10at. An MFC-assertion census, and what it is and is not evidence of

`McTestAssertionsRaised ()` (§10ao) counted `_CRT_ASSERT`s so that one test could
ask a question. It costs nothing to ask it of all 186 suites: the host now prints
`MFC assertions: N` at the end of every run, and `run_tests.ps1` aggregates it.

```
1670 passed, 0 failed, across 186 suites

MFC assertions: 169, from 11 of 186 suites
     90  Accelerators
     14  LuaSurface
     12  ClickColumnR6
     12  SmallArms
     12  MenuRulesWin
     11  ConnectionClosedR5
      6  NotepadWin
      5  PrefsP9
      4  LuaR5
      2  Tls
      1  TransportEventsR5
```

#### What it turned out to mean

Every one of the 169 was triaged, with `MC_TEST_ASSERT_STACKS=1` for the stacks.
**None is a product defect**, and saying so plainly matters more than the census
looking impressive:

| suite | asserts at | reached from | verdict |
|---|---|---|---|
| `Accelerators` (90) | `CDocument::UpdateFrameCounts` — `ASSERT (::IsWindow (pView->m_hWnd))` then `IsWindowVisible` | the suite's own `AccelDoc` fixture calling `AddView` on a windowless view | fixture; 45 tests × 2 |
| `MenuRulesWin` (12), `NotepadWin` (6), `ClickColumnR6` (12) | the same, plus the MDI-child failure `NotepadWin` documents | my own fixtures | fixture, already commented at each site |
| `LuaSurface` (14) | `CWnd::InvalidateRect` | `SetFrameBackgroundColour` doing `Frame.InvalidateRect (NULL)` unguarded | real code, unreachable state: the main frame always has a window once the app is up |
| `SmallArms` (12), `LuaR5` (4) | `CMDIFrameWnd::MDIGetActive` — `ASSERT (m_hWndMDIClient != NULL)` | `CreateTextWindow` → `OpenDocumentFile` → `CreateNewFrame` | same shape, same verdict |
| `ConnectionClosedR5` (11) | a toolbar assertion in `bartool.cpp` | `ConnectionClosed` updating button state | same |
| `PrefsP9` (5), `Tls` (2), `TransportEventsR5` (1) | `CWnd` accessors on pages/frames this host never creates | — | same |

So the honest summary is: **the census found no bugs today.** It is worth having
anyway, for two reasons.

#### Why keep it

**It is a baseline, and the signal is movement.** A suite whose count changes
after an unrelated edit is a suite that started reaching a window-dependent path
it did not reach before — which is precisely how the `flushSend` defect
announced itself in §10ao, as two assertions where there had been none. Without
a recorded number, "two assertions" is indistinguishable from the ambient 169.

**And it is a map.** The table's middle rows are a list of shipping call sites
that dereference a window without checking — `Frame.InvalidateRect (NULL)` being
the clearest. None is reachable *today*, because the main frame outlives every
caller. Each becomes reachable the moment something is constructed earlier or
destroyed later than it is now, which is exactly what the transport relocation
did to `ConnectionOpen ()` (§10ao). This is the list to re-read after any change
to when a window is created.

#### The one caveat, learned while triaging

The stack walker prints innermost-first, so "the first frame in our tree" is the
**caller of the MFC function that asserted** — but a naive grep for the repo path
also matches `TRACE` output, which MFC interleaves on the same stream. Two of the
eleven were misattributed that way on the first pass (`Tls` and
`TransportEventsR5` appeared to assert inside `OnConnectionDisconnect`, which was
just the trace line above the stack). Match on the frame shape, not on the path.

### 10au. A second sweep, and a chat file block that wrote heap into a file

A second read-only hunt over six areas the first had not covered: the world
loader and saver, the matching engines, everything that writes a file, the
VARIANT boundary, a second pass at `pal/win_worldview.cpp`, and telnet/MCCP.
**17 confirmed, 4 refuted**, listed in §10av.

**This section first said the sweep had died, and that was wrong.** Its output
file existed while the run was still going, and reading it early gave
`confirmed: 0, refuted: 0, byArea: []` — which reads exactly like a finished
sweep that found nothing. The journal agreed (seven agents started, one result),
because it too was mid-run. The correction is the useful part: **a background
run's output file existing is not the same as the run having finished**, and the
completion notification is the only signal that is. Both of the things that
looked like evidence of death were just snapshots of a live run.

The one finding read from that early snapshot was acted on immediately, and the
sweep later confirmed it independently. Its verifier had not run at the time, so
it was checked by hand instead:

**`CChatSocket::Process_File_block` writes the block size the peer PROMISED, not
the number of bytes that arrived.** On the zChat transport the block length
comes from two bytes of the peer's own message header and is never compared with
`m_iFileBlockSize`, so a one-byte `CHAT_FILE_BLOCK` makes the receiver
`Write (pBuff, 1024)` out of a one-byte allocation — 1023 bytes of heap into the
user's file, and into the SHA sumcheck. The MudMaster arm of the same switch
already has the guard (`if (iLength < (m_iFileBlockSize + 2)) return;`); the
zChat arm simply never got one.

Confirmed by reading both arms and both size fields before touching anything,
and pinned by `ChatFileBlockR6` in `tests/win/test_gap_coverage.cpp`, which
drives the method directly — the socket-level suite is excluded here for using
`socketpair`, and every member the setup needs is public.

#### Two things the test got wrong first, both worth keeping

- **`StopFileTransfer` does `delete m_pFile`.** The first fixture pointed it at
  a stack `CFile` and the debug heap failed instantly with
  `is_block_type_valid`. A fixture that hands a member to code which owns it has
  to hand it something owned the same way.
- **The diagnostic printed a number it had not measured.** It reported
  "0 bytes on disk" for a file that had just had 1024 written into it: the file
  is still open on the failing path, and a second handle cannot read the size of
  a `CFile` opened `shareCompat`. The assertion was right and the explanation
  beside it was false, which is the worse of the two failures — it now reports
  the file's *existence*, which is what it can actually see.

### 10av. The second sweep's 17, and which of them I actually checked

The sweep in §10au returned **17 confirmed, 4 refuted** across six areas. That
ratio is much softer than the first sweep's 5-of-13, and softer is a warning,
not a result: the same verification harness produced both, so either these areas
are richer or these verifiers were less sceptical. **A "confirmed" here means one
agent argued for it and a second failed to refute it. It does not mean I checked
it.**

So the list below is split by what I did, and nothing in the second half should
be treated as established.

#### Checked by hand, and fixed (seven)

| finding | outcome |
|---|---|
| `chatsock.cpp` — zChat file block written at the promised size, not the received one | `UPSTREAM-BUGS` 96. Independently re-found by this sweep after I had already fixed it from the early snapshot, which is a reassuring cross-check. |
| `chatsock.cpp` — `Process_File_start` has no in-progress guard, and its catch deletes `m_pFile` without nulling | `UPSTREAM-BUGS` 98. Verified by reading the whole function for the guard the claim said was missing; it was. The sharper half is mine, not the agent's: `m_iFileSize = atol (strSize)` runs *before* the accept prompt, so a **refused** second offer still corrupts the running transfer's size. |
| `scripting/scriptengine.cpp` — a script routine name used as a printf format | `UPSTREAM-BUGS` 97. Verified by reading `Trace`'s declaration (`MC_PRINTF_FORMAT(2, 3)`) and body (`FormatV`), and by finding the Lua twin already fixed with the reasoning spelled out at three sites. A tree-wide grep for the same shape now returns only the two legitimate `va_list` forwarders in `exceptions.cpp`. |
| `OtherTypes.h` — `CTimer`'s constructor never initialises `bEnabled` | `UPSTREAM-BUGS` 99. Verified by reading all three constructors (the two siblings do set it) and `Get_XML_boolean`, which leaves its output untouched when the attribute is absent and defaults are wanted. |
| `scriptingoptions.cpp` — `wrap_column` re-allocates the line buffer without the UTF-8 ×4 factor and leaves `iMemoryAllocated` stale | `UPSTREAM-BUGS` 100, and worse than the claim: the stale record is what `DisplayMsg`'s only overrun guard tests against. Measured — a UTF-8 line at wrap 80 keeps `iMemoryAllocated == 320` while its buffer becomes 200. |
| `xml/xml_save_world.cpp` — a numeric option set to 0 reloads as its default | `UPSTREAM-BUGS` 101, and broader than the claim: **four** rows, not one. The claim's real value was refuting `docs/AUDIT-2026-08.md`'s own closing sentence, that only *future* options could have the trap. Both that file and §5's write-up are corrected. |
| `methods_triggers.cpp` / `methods_aliases.cpp` / `methods_timers.cpp` — `eReplace` frees an item whose script is running | `UPSTREAM-BUGS` 102. Verified against the matching `Delete*` guard in each of the three, and against `ProcessPreviousLine`'s `// cannot be deleted now` comment two lines above the write that lands in freed memory. |

#### Checked by hand, and deliberately not fixed

**`pal/win_worldview.cpp`'s `commandSelection ()` cannot say "no command
view".** It returns `Range{0, 0}`, which is also a real caret at offset 0, so
`GetInfo (236)` answers **1** and `GetInfo (237)` answers **0** where upstream
answered **nil** — verified against `f9b47d6^`, where `SetUpVariantLong` is
called only inside the `CSendView` branch and an absent view leaves `vaResult`
at `VT_EMPTY`.

This is precisely §10ar's shape, and the fix would be precisely §10ar's fix: a
second predicate on the seam. It is **not** taken, because unlike the minimised
main frame I cannot show the state is reachable in the product — a world's
`CSendView` exists for as long as its window does. Adding a seam predicate for a
state nobody can reach is how a seam grows a method per hypothesis. Recorded
here so that if a "no command view" window ever becomes reachable, the answer is
already written down.

#### The leads, and what became of them

The sweep produced thirteen leads beyond the seven I had already checked.
**Eight of them have since been confirmed and fixed**, each with a
non-vacuous test — the fix reverted, the test seen to fail, the fix restored:

| file | outcome |
|---|---|
| `scriptingoptions_alpha.inc` | **confirmed, fixed** — `UPSTREAM-BUGS` 104 |
| `xml/xml_save_world.cpp` (option set to 0 omitted by the saver) | **confirmed, fixed** — `UPSTREAM-BUGS` 101 |
| `scripting/methods/methods_triggers.cpp` (×2, and the alias/timer twins) | **confirmed, fixed** — `UPSTREAM-BUGS` 102 |
| `plugins.cpp` (archive flushed after success is reported) | **confirmed, fixed** — `UPSTREAM-BUGS` 103, which turned out to be three sites, not one |
| `scripting/methods/methods_timers.cpp` (zero interval) | **confirmed, fixed** — `UPSTREAM-BUGS` 105 |
| `xml/xml_save_world.cpp` (timer seconds at 2 decimals) | **confirmed, fixed** — `UPSTREAM-BUGS` 106, and it is the same end state as 105 reached from the other side |
| `doc_shared.cpp` (SOCKS5 reply length) | **confirmed, fixed** — `UPSTREAM-BUGS` 107, but only the ATYP half of the claim; see below |
| `dialogs/world_prefs/configuration.cpp` (accept-chat tick) | **confirmed, fixed** — `UPSTREAM-BUGS` 108, and worse than filed: it goes wrong in both directions |

The zero-interval one is worth a note on method, because the argument and the
proof came apart. The claim as filed was an asymmetry — `AddTimer` refuses a
zero "every" interval and `SetTimerOption` does not — which is true but is only
evidence of an *inconsistency*. What makes it a bug is the consequence, and the
consequence is measurable: three back-to-back `CheckTimers` calls with no time
allowed to pass, three matches on the zero timer against at most one on a
five-second control. The dialog turned out to carry the same refusal
(`CTimerDlg::DoDataExchange`, "The timer interval must be greater than zero"),
which makes it two entry points guarding and one not — an omission rather than
a judgement. **Look for the third caller before calling an asymmetry a bug: it
decides which of the two sides is the wrong one.**

And the *fourth* caller is what turned 106 from a precision nicety into a bug.
Rounding a timer's seconds from four declared decimals to two is inconsequential
against a 100 ms poll — until you notice that the loader **refuses** the value
the rounding produces, so a legal timer silently disappears from a file the app
itself wrote. Two entry points guarding, one not, and one *manufacturing* the
forbidden state out of a legal one. **When several places disagree about what
is legal, enumerate all of them before deciding which is wrong** — the count
here was four, and the first two leads each found only two of them.

#### Checked, true, and deliberately NOT changed

**`outputSelection ()` answers the first `CMUSHView`, not the one you selected
in.** `Window ▸ New Window` (`ID_WINDOW_NEW`, present in the menu with no
handler of the app's own, so MFC's `CMDIFrameWnd::OnWindowNew` serves it) gives
one world two `CMUSHView`s, each with its own
`m_selstart_line`/`m_selend_col`. `GetSelectionStart/EndLine/Column` walk the
document's view list and answer the first `CMUSHView` they find, while
`CMUSHView::OnEditCopy` copies **`this`** view's selection through
`GetSelection`. Select in the second window and a script is told there is no
selection.

The lead named `pal/win_worldview.cpp`, which is port-introduced code — but
this is not the port's doing. `git show e362b37:scripting/methods/methods_info.cpp`
has the identical loop; the adapter transcribed it and added only an
`m_LineList.GetCount () > 0` guard that upstream lacks.

**Left as it is, on purpose.** "Which view is authoritative" is a real design
question for a document-level API that can only report one answer, and changing
it would alter a documented return value on both platforms to no clear benefit —
the port has exactly one view per world, so a fix would be Windows-only in
effect. Pinned instead by
`WinWorldViewR6.OutputSelectionAnswersTheFirstViewNotTheSelectedOne`, so that a
future change to it is a decision rather than an accident. The test moves the
same selection onto the first view and watches it appear, which is what keeps
the zeros from being a dead reader.

It costs **18 MFC assertions** — `CDocument::UpdateFrameCounts` re-walks the
view list on each `AddView`, asserting `::IsWindow` on views that have none —
so the suite's assertion baseline moves from 169 to 187. That is expected
noise of the same kind `tests/win/test_menu_rules_win.cpp` already documents,
not a regression.

**Two hypotheses raised while reading nearby code, both refuted**, recorded so
they are not re-raised:

- *The timer poll interval is really the activity-window refresh interval.*
  It is not mislabelled: `IDD_GLOBAL_PREFSP6` is captioned "Timers", the field
  is "&Timer interval (seconds):", and the page even carries the hint "Set to
  zero for timers that can fire up to 1/10 of a second apart." The identifier
  `ACTIVITY_TIMER_ID` is misleading; the user-facing contract is correct.
- *`world.Debug ("timers")` hardcodes 0.1 s.* It does not —
  `world_debug.cpp:1189` already branches on `App.m_nTimerInterval` and reports
  the real figure.

#### No leads left

The last one — "World Properties clears the persisted *accept chat
connections* tick when the listen socket failed to bind" — was **confirmed and
fixed**: `UPSTREAM-BUGS` 108. It was worse than the claim: `LoadPrefsP23` filled
the tick from `m_pChatListenSocket != NULL` and `SavePrefsP23` writes the tick
back into the persisted flag, so OK on an untouched page rewrote the preference
to match runtime state in **both** directions — a failed bind turned it off,
and a script's `world.ChatAcceptCalls` turned it on in a world that had never
asked for it. The second needs no failure at all, only a plugin.

**Tally, counting the rows of the two tables above rather than the sweep's own
numbering** (it grouped some sites and split others, and I have not gone back
to reconcile it): eight confirmed and fixed, one confirmed and deliberately
left alone, one refuted outright, and one — the SOCKS5 row — half of each.

The refuted one is the MCCP claim: the decompression loop does keep going after
a script-initiated disconnect, but nothing is freed on that path
(`OnConnectionDisconnect` nulls `m_pTransport` and the buffers live until the
document's destructor), the connect path resets `m_bCompress` so nothing leaks
into the next session, and what it displays is data that legitimately arrived.
The SOCKS5 row's second clause — "and to arrive whole in one packet" — is also
refuted as *stated*: a short reply is not over-read, because the caller guards
with `count <= iBytesToMove`. It is dropped instead, which is a different and
lesser bug, and `UPSTREAM-BUGS` 107 records why closing it needs a change to
the phase machinery.

Set that beside the first sweep, where **eight of thirteen did not survive
contact**. The difference is not that this batch was better reasoned. It is
that each was re-derived from the source by hand and given a test that fails
without the fix — the same standard the first batch was held to, and mostly
failed. **"Confirmed" from an agent sweep means one agent argued for it and a
second failed to refute it, and that is all it means.**


#### One unreproduced crash, recorded rather than dismissed

`MXPCloseR6` reported `CRASHED (no summary)` on one full run, and only that one.
It passed **12/12** in a loop immediately afterwards and clean on a full re-run,
so there is nothing to chase yet — but the code under it is a use-after-free
path, and this project's own record (the notepad selftest, 2-in-12 before Guard
Malloc made it deterministic) is that a flake in that neighbourhood is worth a
line rather than a shrug. If it recurs, the loop-and-count is the first
instrument and `MC_TEST_ASSERT_STACKS=1` the second.

### §10aw — Driving the real GUI app, and what it said

The unit suite links `mushcore` and never constructs a `CMUSHView`. To reach the
code that only exists when a frame, a view and a command bar do, the app itself
was driven — not by clicking, but by a **world script**, so that every claim
below is a file on disk rather than a screenshot.

The rig (`scratchpad/gui/`, not committed — it is three files and a listener):
a minimal `.mcl` naming a Lua `script_filename`, a one-shot `active_closed`
timer that defers the battery until after the view exists, and a PowerShell
`TcpListener` on 127.0.0.1 so the world genuinely connects and receives.

**Four instrument lessons, each of which cost a run:**

- **`script_file` is not the attribute; `script_filename` is.** The app says so
  in a message box naming the line — read the box before guessing.
- **A hand-written world file needs `defaults="y"`.** Without it every omitted
  boolean loads as FALSE, *including* `enable_timers`, so the deferred battery
  never ran and every timer sat overdue with `nMatched = 0`. This is deliberate
  and the reason is commented at `xml/xml_load_world.cpp:1046`: the saver omits
  FALSE booleans, so honouring ctor defaults for omitted ones would silently
  re-enable every true-by-default option the user had turned off, on every
  save/load round trip. **Four hypotheses were burned before reading that
  comment** — the option table's default (`true`) is not what a minimal file
  gets, and `world.GetOption ("enable_timers")` from inside the running app is
  what settled it. Ask the app, not the table.
- **Never `SendMessage (BM_CLICK)` into a modal from outside the process.** It
  ends the modal loop *inside* the send, and MFC's `CWnd::RunModalLoop` then
  trips `ASSERT(ContinueModal())` (`wincore.cpp:4659`) on its next pump. That
  assertion is the instrument, not the app: posting `WM_LBUTTONDOWN`/`UP`
  instead produced zero assertions across every later run. A Debug-build
  assertion that appears the moment you start automating is yours until proven
  otherwise.
- **Read a dialog before dismissing it.** The first-run "Thanks for upgrading"
  and "Tip of the Day" boxes are ordinary; clicking blind would have hidden the
  `script_filename` error that was sitting behind them.

**What the battery found: nothing wrong.** 82 probes over `GetInfo` 1-296, the
selection and command-bar family, and the miniwindow ops against **real GDI**.
Everything was internally consistent — `WindowGetPixel (10,10)` returned the
blue of the diagonal line drawn over the green fill; `WindowTextWidth` for ten
`M`s was exactly ten times `aveCharWidth` on a monospaced face and larger on a
proportional one; `GetInfo (288)` (timers fired this session) read 1 at the
moment the one-shot timer's own routine was running; `GetInfo (280/281)` and
`(292/293)` agreed pairwise. **`GetInfo (249)/(250)` reported 931 × 1616
against a 1616 × 1022 screen** — real client geometry, which is the fix for the
minimised-client fabrication holding up in the live app.

**And one open item closed by measurement**, which is the real return on the
rig: `docs/FONT-PARITY-2026-08.md` §9. GDI's substitute for an unavailable face
is decided by the `pitchAndFamily` hint the port drops — `FIXED_PITCH` gives
Courier New, `FF_DONTCARE` gives Arial — but **only when the face is unknown**;
a face Windows has beats the hint every time, and the default hint is the one
whose answer the port already matches. `tmHeight - tmInternalLeading` was
confirmed equal to `MulDiv (points, 96, 72)` exactly, across four sizes and
five faces, which quantifies `pal::CpuRaster`'s under-report at 3 px in 16 —
about 19% — at 10 pt Courier New.

### §10ax — The five unasserted miniwindow image ops, measured against real GDI

`CLAUDE.md` lists five image operations with no assertions anywhere:
`WindowMergeImageAlpha`'s mask variant, `BlendImage`, `Filter`,
`TransformImage`, `ImageOp`. The GUI rig from §10aw drives all five over known
pixels and reads the result back with `WindowGetPixel`, so "returned `eOK` and
did nothing" fails. `COLORREF` is `0x00BBGGRR` throughout.

**All five work.** No defect found. The reference values, from a source window
that is pure red (`0x0000FF`) on the left and pure blue (`0xFF0000`) on the
right:

| op | driven as | result |
|---|---|---|
| `DrawImage` (control, already covered) | mode 1 over white | `0x0000FF` / `0xFF0000` — exact copy |
| `BlendImage` | mode 1, opacity 0.5, over white | `0x8080FF` / `0xFF8080` — exactly half-way to white on each channel |
| `TransformImage` | identity | `0x0000FF` at x=5, `0xFF0000` at x=25 |
| `TransformImage` | 0.5 scale | image occupies x 0-20, so red at x=5 and blue at x=15 |
| `ImageOp` | action 1, rect 0,0-20,20, image as brush | `0x0000FF` inside, `0xFFFFFF` outside |
| `MergeImageAlpha` | mask black left / white right, over a green ground | `0x00FF00` where the mask is black, `0xFF0000` where it is white |

The `MergeImageAlpha` row is the one worth keeping: black mask = fully
transparent, white mask = fully opaque, which is exactly the alpha-mask
contract and had never been asserted anywhere.

**`Filter`'s `Options` argument means two different things, and reading it as
one cost a false positive.** For `Noise`, `Brightness`, `Contrast`, `Gamma` and
the per-channel variants it is a strength. For the five convolutions — blur,
sharpen, edge detect, emboss and the two at cases 20-21 — it is a **direction
selector**: `GeneralFilter` guards its horizontal pass with
`if (Options != 2) // 2 = vertical only` (`miniwindow.cpp:2668`) and its
vertical pass with `if (Options != 1) // 1 = horizontal only` (`:2716`).

That was first read as a strength, which made blur, sharpen and emboss look
like they silently no-opped at `Options = 2.0` while working at `0.5` — a
"returns `eOK` and does nothing" bug, reported by a test field whose only edge
ran *vertically*. A vertical-only convolution across a vertical edge correctly
changes nothing. **Before calling a filter inert, check what its parameter
selects.**

The full 19-operation table over a mid-grey field (`0x808080`, chosen so
nothing clamps and nothing sits on a gamma fixed point) with a black block whose
edge runs down x=30, sampling a flat pixel at (10,10) and an edge-adjacent one
at (29,10):

- **Noise** perturbs both samples, more at 2.0 than 0.5; **MonoNoise** is
  sub-unit at 0.5 and visible at 2.0.
- **blur** leaves the flat pixel alone and pulls the edge pixel toward the black
  block (`0x808080` → `0x4C4C4C`); **sharpen** pushes it away (`→ 0xD5D5D5`);
  **emboss** saturates it (`→ 0xFFFFFF`). All three at `Options = 0.5` (both
  directions); all three correctly inert at `2.0` (vertical only).
- **Contrast** and all three **ColourContrast** ops leave mid-grey untouched at
  every strength — correct, since contrast pivots about mid-grey. The first
  pass proved they are not inert: on pure red, `Contrast 0.5` gave
  `0x0000FF → 0x4040BF`.
- **Gamma** is the cleanest signal: `0.5 → 0xB4B4B4`, `2.0 → 0x404040`, and the
  three `ColourGamma` ops move exactly one channel each (`0x8080B4`, `0x80B480`,
  `0xB48080` at 0.5) — which also confirms the channel-to-index mapping
  (2 = red, 1 = green, 0 = blue) that `miniwindow.cpp:3153-3161` asserts in
  comments only.
- **greyscale** leaves mid-grey unchanged, as it must.

One method note, recorded because the first run of this probe produced a file
that *looked* like a run: in Lua, `out:write (..., "\n")` truncates `...` to its
**first** value, so every line came out as a bare label with no data. An output
helper that silently drops its arguments is indistinguishable from a probe that
found nothing.

### §10ay — "Copy wildcard 10 to the clipboard" selected a wildcard the clipboard code could not reach

Found while checking whether the saver can write a `clipboard_arg` its own
loader rejects (it cannot — the option table and the loader both say 0..10, and
they agree). What does not agree is the *third* place, the one that uses it.

The Trigger dialog's clipboard dropdown (`IDC_WILDCARD_CLIPBOARD`) is populated
from `DLGINIT` with **eleven** entries: `(none)`, then `1` … `10`, so the
selected index is the wildcard number. `ProcessPreviousLine.cpp:1196-1199`
then does

```cpp
if (trigger_item->iClipboardArg &&
    trigger_item->iClipboardArg >= 0 &&
    trigger_item->iClipboardArg < MAX_WILDCARDS)
   putontoclipboard (trigger_item->wildcards [trigger_item->iClipboardArg]...);
```

with `MAX_WILDCARDS == 10`, so the usable range is **1…9**. Choosing `10` in
the dialog stores 10, fails `< MAX_WILDCARDS`, and silently copies nothing.

What makes it more than an off-by-one in a dropdown is that **wildcard 10 does
mean something everywhere else**. The array handed to scripts is built
1-relative with `MAX_WILDCARDS` elements, and its last one is deliberately not
a capture group:

```cpp
// i should be MAX_WILDCARDS (10) now ;)
COleVariant v (trigger_item->wildcards [0].c_str ()); // the whole matching line
```

So a script's `wildcards[10]` is the whole matching line — and that is the
natural reading of what the dropdown's `10` offers. The clipboard code can
reach it by neither index: 10 fails the upper bound, and 0 (where the whole
line actually lives in `trigger_item->wildcards`) fails the truthiness guard
that means "(none)".

The `iClipboardArg >= 0` term is dead in any case: the member is
`unsigned short`.

**Fixed** — `UPSTREAM-BUGS` 109. This section originally ended "recorded for a
decision rather than guessed at", listing three conceivable fixes and arguing
none was wholly available: the dialog lives in the prebuilt `locale\EN.dll`
rather than a resource this tree compiles, and lowering the option table's
maximum to 9 would leave a value a world file can still hold and the script API
can no longer set — the shape `UPSTREAM-BUGS` 106 warns about.

The third option was the right one and the objection to it was weaker than the
other two: make `10` mean what scripts already mean by it. `ClipboardWildcardIndex`
(`OtherTypes.h`) maps 0 to "copy nothing", 1…9 to `%1`…`%9`, and 10 to
`wildcards [0]`, the whole matching line. It **remaps** rather than widening the
bound, so the ten-element vector is still never indexed at 10 — the old guard's
actual job is kept.

Worth noting how the two levels of test divide: the unit test asserts the
mapping and deliberately does not drive the call site, because
`putontoclipboard` would clobber the machine's real clipboard; the in-app probe
(`tests/win/app_probes/clipboard.lua`, §11) drives it through a real trigger
match and can afford to, because `world.GetClipboard`/`SetClipboard` let it save
and restore. Against the app built before the fix it failed on exactly the `10`
assertion and passed the other three — which is the end-to-end evidence the unit
test cannot supply.

### §10az — OWED: does GDI really shrink a NULL-pen Rectangle?

Not a result — a question this side cannot answer, recorded so the next
Windows run can settle it.

`docs/PAL.md` records as a TAKEN decision that `shape` with a null pen draws
the figure one pixel smaller on right/bottom ("GDI's documented null-pen
shrink — normative, pinned by the golden tests"), and `pal::CpuRaster`
implements it (`inset = stroked ? 1 : 0`) with a golden in
`tests/test_pal_surface.cpp`. The **shipping** wx adapter did not: it filled
the full w x h, so the two `ISurface` adapters disagreed about a rule the
design doc calls normative and only the one nobody ships was pinned. The wx
side has been changed to match, in `CDC::Rectangle`.

The direction rests on PAL.md's recorded decision plus the fact that wxMSW
compensates for exactly this quirk in `DoDrawRectangle` when the pen is
transparent. **No macOS-side observation can confirm it.** Run this on the
Windows build:

```lua
WindowCreate ("t", 0, 0, 64, 48, 0, 0, ColourNameToRGB ("blue"))
WindowCircleOp ("t", 2, 50, 30, 53, 32,
                ColourNameToRGB ("white"), 5 --[[PS_NULL]], 1,
                ColourNameToRGB ("red"), 0, 0, 0, 0, 0)
print (WindowGetPixel ("t", 51, 30), WindowGetPixel ("t", 52, 30),
       WindowGetPixel ("t", 50, 31))
```

- `(52,30)` and `(50,31)` **blue** → GDI shrinks; PAL.md, CpuRaster and the
  new wx behaviour are all right, and this row can be closed.
- either of them **red** → GDI does NOT shrink, the wx adapter was right all
  along, and the things to change are `pal/cpu_raster.cpp`,
  `tests/test_pal_surface.cpp` (`ShapeRectStrokesAndFills`), `docs/PAL.md`
  and the new `CDC::Rectangle` arm — in that case revert the arm rather than
  keeping two wrong adapters agreeing.

Worth reading `(51,30)` in the same breath: it must be red either way, so a
run where it is blue means the fixture is wrong, not the platform.

Only `ShapeKind::Rect` was changed. `CpuRaster::shape` bails on every other
kind, so RoundRect/Chord/Pie/Ellipse have no reference to disagree with and
were deliberately left alone; if this run confirms the shrink, the same
question should be asked of those four before extending it.

### §10ba — OWED: PS_INSIDEFRAME's exact geometry

Another question, not a result. `PS_INSIDEFRAME` (`miniwin.pen_inside_frame`)
was rendering exactly like `PS_SOLID` on macOS — the shim maps the style to an
ordinary solid pen and wx strokes centred on the path, so a wide pen overflowed
the bounding rectangle by half its width on every side. Real GDI performs the
shrink inside `CreatePen`, so `pal/gdi_surface.cpp` gets it free. The wx
adapter now insets the shape rectangle by `width / 2` before drawing
(`insideFrameRect`, `platform/wx_surface.cpp`), and
`--pal-miniwindow-selftest` pins the invariant that matters — no ink outside
the rectangle, with a `PS_SOLID` control proving a centred pen really does
overflow.

What the macOS side **cannot** settle, and what a Windows run should measure
with `WindowGetPixel` over a `WindowCircleOp` at pen widths 8 and 9:

1. **Rounding for EVEN pen widths.** `width / 2` is exact for odd widths; for
   an even one GDI has to put the extra half-pixel somewhere, and which side it
   lands on is not derivable here.
2. **Does the brush FILL move with the border?** The inset shrinks the whole
   figure, so the fill's extent shifts too. That is an inference from "the
   dimensions of the figure are shrunk", not a measurement.
3. **Does RoundRect's corner ellipse shrink with the figure?** `a.x`/`a.y` (the
   corner ellipse width/height) are passed through unshrunk here, which is a
   guess.

Also unmeasured, and deliberately so: `pal::CpuRaster::shape` ignores pen width
entirely (documented S1 gap — a 1px border), so the headless reference can
neither disagree with this nor be used to check it. At width 1
`PS_INSIDEFRAME` is a no-op, so the two adapters do not currently conflict.

### §10bb — OWED: the 1-bpp WindowCreateImage pattern's reported geometry

`world.WindowImageInfo(win, id, 6)` used to answer 32 for every image on
macOS, because the wx shim materialises everything as `new wxBitmap(img, 32)`
and reported `wxBitmap::GetDepth()`; `InfoType` 4 answered a flat 0, because
nothing ever computed a stride. Both now come from a per-`CBitmap` record of
what the source DIB or `CreateBitmap` call actually declared.

Two of the three answers are safe from this side: a 24-bit RGB PNG reports 24
and a 4-byte-aligned stride, which is what the loaders themselves pass to
`CreateDIBSection` (`png_load_shared.cpp`, `bmp_load_shared.cpp`) and what
`platform/headless_surface.cpp` and `tests/test_gdi_headless.cpp` have always
asserted. The third is an inference:

- **`world.WindowCreateImage`'s 8x8 pattern.** It is
  `CBitmap::CreateBitmap (8, 8, 1, 1, bits)` — a 1-bpp DDB on Windows — so
  `GetObject` should report `bmBitsPixel == 1`. That follows from the argument,
  but has never been measured. `--pal-miniwindow-selftest` asserts it.
- **And that pattern's `bmWidthBytes`.** Windows pads a DDB scan line to a
  WORD, not a DWORD, so 8 pixels at 1 bpp should be **2**, where the DIB rule
  would give 4. The port implements the WORD rule for `CreateBitmap` and the
  DWORD rule for DIB sections, but nothing pins the DDB half — deliberately,
  since the number is unmeasured. Read `WindowImageInfo (win, id, 4)` for a
  `WindowCreateImage` pattern on Windows and record it here.

**Behaviour change worth knowing about while validating.** The depth fix makes
`WindowDrawImageAlpha` and `WindowGetImageAlpha` refuse a 24-bit image with
`eImageNotInstalled` (30056) on macOS, as they always have on Windows. That is
parity by definition, but from a mac user's seat it is a regression: a plugin
that today hands an RGB PNG to either op gets a drawn image and will now get
nothing. `WindowMergeImageAlpha` has no such gate on either platform and is
unaffected.

### §10bc — OWED: how oleaut rounds a fraction into an integer parameter

Not a result — a question this side cannot answer, recorded so the next
Windows run can settle it.

`platform/mc_dispatch.cpp`'s `var_to_long` used to hand an integer parameter
`(long) a->dblVal` and `atol (a->bstrVal)`, i.e. truncation toward zero. It
now rounds to nearest with **ties to even** (`round_to_long`), on the reading
that Windows reaches the same methods through the real `IDispatch::Invoke`,
whose `VariantChangeType` coercion is `VarI4FromR8` / `VarI4FromStr` — both
documented as round-to-nearest, banker's ties.

That reading is **documented oleaut behaviour, not a measurement**, and this
project's own rule is never to assert the other platform's result from this
side. `doc_dispatch_map.inc` declares no `VTS_VARIANT`, so every parameter is
typed and the coercion definitely happens; what is unmeasured is only which
way it breaks a tie. Run this on the Windows build, in a **JScript** world
(all JScript arithmetic is double, so `10.5` crosses as `VT_R8`):

```javascript
var v = [10.5, 11.5, 10.7, 10.2, 0.5, 1.5];
for (var i = 0; i < v.length; i++) {
  world.SetOption("paste_delay", v[i]);
  world.Note(v[i] + " -> " + world.GetOption("paste_delay"));
}
```

`paste_delay` is chosen because its row in `scriptingoptions_numeric.inc` is
`0, 100000` with no `OPT_FIX_*` flag, so nothing rewrites the value on the way
in or out.

- `10.5 -> 10` **and** `11.5 -> 12` → ties to even; the fix is right and this
  row closes.
- `10.5 -> 11` and `11.5 -> 12` → ties away from zero; change `round_to_long`
  to `std::round` and flip the two tie rows in
  `McDispatch.FractionalNumbersRoundHalfToEvenIntoIntegerParameters`.
- `10.5 -> 10` and `11.5 -> 11` → no rounding at all; revert the fix and
  restore the C cast, since the port was right and the finding was wrong.

`10.7 -> 11` and `10.2 -> 10` must hold in the first two cases and fail in the
third, so they say which of the three answers you are looking at without
relying on the ties alone. `0.5`/`1.5` are the same tie question one step down,
where half-away and half-even differ on the FIRST of the pair — a fixture that
only ever asked about 10.5 could not tell a rule from an off-by-one in it.

**The string arm needs its own run**, because it is a different oleaut entry
point (`VarI4FromStr`, not `VarI4FromR8`) and it is the one Tcl always takes —
`tcl_engine.cpp`'s `tcl_to_variant` marshals *every* argument as `VT_BSTR`, so
a Tcl plugin's numbers all arrive as text. In a **Tcl** world:

```tcl
foreach n {10.5 11.5 10.7 42 abc 0x10} {
  world SetOption paste_delay $n
  world Note "$n -> [world GetOption paste_delay]"
}
```

- `10.5 -> 10`, `11.5 -> 12`, `10.7 -> 11` → matches the new `str_to_long`.
- `abc` and `0x10`: the port answers **0** for both, deliberately — `atol` did,
  and widening what counts as a number is a separate decision (D-24 /
  `docs/UPSTREAM-DIVERGENCES.md` §17 keep strict checking to arity precisely
  because Tcl sends everything as a string). If Windows instead raises
  `DISP_E_TYPEMISMATCH` — a visible script error — record that here; it is a
  divergence worth deciding on, not something to fix silently.

Also worth reading in the same run: what an **out-of-range** double does.
`world.SetOption("paste_delay", 1e300)` should be `DISP_E_OVERFLOW` on
Windows — a script error. The port has no channel to report that from
`var_to_long`, so it clamps to `LONG_MAX` and the option's own range check
then refuses it with `eOptionOutOfRange`. Both end in a refusal, but only one
of them is catchable, and that gap is recorded rather than fixed.

### §10bd — OWED: what a script's `true` looks like to a non-boolean parameter

Not a result — the second half of §10bc's question, and the same caveat: the
Windows side is documented COM behaviour, never measured here.

`py_engine` / `js_engine` / `ruby_engine` marshalled a language `true` as
`VT_BOOL` with `boolVal = 1`, and `mc_dispatch` coerced that to `1` for a
numeric parameter and `"1"` for a string one. They now send **VARIANT_TRUE**,
which is `-1`, and the substrate passes the number through — `VarI4FromBool`
and `VarBstrFromBool` both render the value, and MFC's coercion passes no
`VARIANT_ALPHABOOL`, so a string parameter should receive `"-1"` rather than
`"True"`.

A `VTS_BOOL` parameter is deliberately **not** part of this: `VARIANT_BOOL` is
16-bit where MFC's `BOOL` is 32-bit, so that conversion cannot be a copy, and
every shipping method with a `BOOL` parameter normalises for itself anyway
(`Enabled != 0`, `bNewValue != 0`). `var_to_bool` keeps those at 0/1 so `-1`
can never reach a `BOOL` member that is stored raw — `m_bMapping` and
`m_bRemoveMapReverses` are written straight from the property setter.

Run this on the Windows build, in a **Python** world (JScript and Ruby should
agree; Perl has no native boolean at this boundary and Tcl sends everything as
a string, so neither can be asked):

```python
world.SetVariable("bv", True)
world.Note("string param: " + world.GetVariable("bv"))

world.SetOption("paste_delay", 5)
world.SetOption("paste_delay", True)
world.Note("after boolean: %s" % world.GetOption("paste_delay"))
world.SetOption("paste_delay", 1)
world.Note("after plain 1: %s" % world.GetOption("paste_delay"))
```

- `string param: -1` → the string arm is right and this half closes. `1`
  means revert `var_to_cstr`'s `VT_BOOL` arm and the three engine
  marshallers; `True` means MFC does pass `VARIANT_ALPHABOOL` after all, which
  nothing here predicted, and the arm should format the word instead.
- `after boolean: 5` → the numeric arm is right: `-1` is outside every boolean
  option's `0..1` range, so Windows refuses the call and leaves the option
  alone. `after boolean: 1` means the number did not arrive as `-1` and the
  numeric arm should go back to `boolVal ? 1 : 0`.
- `after plain 1: 1` is the control. If that fails, the fixture is wrong and
  neither of the two readings above means anything.

**This is a user-visible behaviour change on macOS, and it is the one worth
watching for.** A port-only Python/JS/Ruby plugin that says
`world.SetOption (<a boolean option>, True)` used to succeed here and now gets
`eOptionOutOfRange`. That is Windows parity — the same line has always failed
silently on Windows — but from a mac user's seat it reads as a regression, so
if the run above says the port is right, it is worth a release note rather
than only a commit message. The port-side pin is
`McDispatch.ALanguageBooleanReachesANumericParameterAsMinusOne`, and the
engine-side one is the `bools` row of `--engines-selftest`, which asks the
real interpreter for the boolean and keeps Lua as a live control (Lua does not
travel `mc_dispatch`, and `my_checkstring` rejects a boolean outright, so it
raises where the other three store `-1`).

## §11 — The in-app probe suite

```
powershell -File tests\win\run_app_probes.ps1              # build + run all
powershell -File tests\win\run_app_probes.ps1 -NoBuild
powershell -File tests\win\run_app_probes.ps1 -Probe imageops
```

**99 assertions across 4 probes, all passing.**

There are now **three** Windows test surfaces, not two:

| surface | what it links | what it can reach |
|---|---|---|
| `ctest --test-dir build` | `mushcore` + `mushcore_testsupport` | the engine, headless, on macOS/Linux |
| `tests\win\run_tests.ps1` | the same 286 engine sources as the app, real MFC, real COM | the engine, headless, **on Windows** — 1683 assertions, 194 suites |
| `tests\win\run_app_probes.ps1` | **the shipping `MUSHclient.exe`** | everything that needs a `CMUSHView`, a `CSendView`, a `CMainFrame` and real GDI *at the same time* |

The third exists because the second cannot reach any of it. `run_tests.ps1`
constructs a `CMUSHclientDoc` with no window — deliberately, and
`tests/win/test_win_worldview.cpp` pins the "no view" answers as a contract —
so the miniwindow drawing ops, the `GetInfo` window family, the command bar and
the clipboard have no coverage there at all. This is the Windows counterpart of
the macOS in-app selftests, and it is kept **separate from `run_tests.ps1` on
purpose**: that runner drives a headless console binary whose failure mode is a
bad assertion, while this one launches a windowed app whose failure mode is a
stuck modal. Folding them together would let one hung dialog take down 194
healthy suites.

### How a probe works

`tests/win/app_probes/<name>.lua` defines a global `probeMain` and nothing
else. The runner generates a world file and a bootstrap script; the bootstrap
`dofile`s the probe and registers a **one-shot `active_closed` timer** that
calls `probeMain` two seconds later — after the view exists, which is the whole
point. The probe writes `PASS`/`FAIL` lines and one `SUMMARY`, and the runner
counts them. **A missing `SUMMARY` is a crash, not a pass.**

`harness.lua` carries the assertion helpers. Write every assertion so that "the
code did nothing" FAILS: the clipboard probe seeds the clipboard with
`SENTINEL` before each match, and the image probe paints a green ground under
its alpha-mask test so that "failed to draw" and "correctly transparent" cannot
look alike.

### The four probes

- **`imageops`** (29) — the five operations `CLAUDE.md` lists as having no
  assertions anywhere, against real GDI. Includes `MergeImageAlpha`'s mask
  variant and the `Filter` direction-selector pair from §10ax.
- **`fontmetrics`** (36) — the two facts §9 of `docs/FONT-PARITY-2026-08.md`
  established by measurement: `tmHeight − tmInternalLeading == MulDiv(pt,96,72)`
  exactly, and the `pitchAndFamily` substitution rule *including* the half that
  bounds it (a known face beats the hint).
- **`worldview`** (22) — the `GetInfo` window family and the command bar,
  asserted on internal consistency and bounds rather than absolute pixels so it
  passes on any screen.
- **`clipboard`** (12) — `clipboard_arg` end to end through a real trigger
  match, saving and restoring the machine's clipboard around itself. This is
  the call-site coverage `tests/win/test_gap_coverage.cpp` deliberately does
  not attempt.

### Five hazards, each of which cost a run

All five are commented at their sites in the runner, because every one of them
presents as an unexplained timeout:

1. **The world attribute is `script_filename`, not `script_file`.**
2. **A hand-written world file must say `defaults="y"`** or every omitted
   boolean loads FALSE, `enable_timers` included, so the deferred probe never
   runs. Deliberate; reason at `xml/xml_load_world.cpp:1046`.
3. **Never `SendMessage(BM_CLICK)` into a modal from outside the process** — it
   ends the modal loop inside the send and MFC trips `ASSERT(ContinueModal())`
   at `wincore.cpp:4659`. Post `WM_LBUTTONDOWN`/`UP`.
4. **No BOM on generated Lua.** Windows PowerShell 5.1's
   `Set-Content -Encoding utf8` writes one and Lua's loader rejects the file.
   This surfaces *only* as a "Scripting Error" box whose child controls expose
   the field labels (`Error number:`, `Description:`) and never the message —
   so the runner now prints the full contents of any dialog whose title is not
   on a benign list, and the generated bootstrap `pcall`s its `dofile` and
   **writes a report on failure** instead of leaving a silent timeout.
5. **Preferences.** This is the only mode that runs the app far enough to write
   them. The runner backs them up before the first launch and restores them in
   a `finally`, so a failing probe cannot leave them modified.

### What building it found

Nothing broken in the app — but two contracts that a reader would get wrong,
both caught because a probe asserted the opposite and failed:

- **`SetCommand` refuses a command bar that is not empty**, returning
  `eCommandNotEmpty`. Upstream, not a port quirk: the
  `if (strCurrent.IsEmpty ())` guard is in `e362b37`. The first draft of
  `worldview` read the refusal as a stale selection.
- **`Filter`'s `Options` is a direction selector for the convolutions**, not a
  strength (§10ax).

Both are now pinned, which is the point: a contract that surprised the person
writing the test is exactly the one worth a test.
