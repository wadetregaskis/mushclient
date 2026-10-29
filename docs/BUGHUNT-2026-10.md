# Bug hunt, GUI pass and performance survey — 2026-10-29 (Windows)

An overnight pass on the Windows ARM64 VM, recorded so none of it is
re-triaged from scratch. Three parts: a review of the 6.00 work done on Windows
in the preceding sessions (data folder, 5.x import, bundled-plugin upgrades,
system spell checkers, toolbars); a broad hunt across the application, aimed at
what is Windows-specific (the PAL transport and world view, the embedded script
engines, the version 6 common controls the manifest now brings) and done
largely by using the client against a live MUD (Discworld, as a guest); and a
performance survey, measured in the running client.

**Method.** Reviewers read each area adversarially and reported findings with
failure scenarios; every finding below was re-read against the code before it
was fixed, and each Windows-reachable fix carries a test or was checked in the
running client. Tests: 1735 pass on ARM64, x64 and Win32 (after the JIT, below); the packaged ARM64
build's engine selftest passes 471/471 (19 skipped).

## Fixed

### This session's own work
- **Data folder:** a non-ASCII (or code-page-unspellable) profile path stopped an
  installed copy starting — ANSI bytes went to `sqlite3_open`, which takes
  UTF-8. Folder via `SHGetFolderPathW` (8.3 name when unspellable); every
  database opened through `sqlite_path.h`. Portable copies keep relative folder
  preferences.
- **Bundled plugins:** asked about a changed plugin even when the version had
  nothing new; asked about plugin data (map databases); overwrote an earlier
  `.backup`; replaced the user's copy even if the backup failed; wrote in place;
  never re-synced a new build of the same version; synced plugin state from a
  5.x program folder.
- **5.x import:** crashed on a file name the ANSI code page cannot spell; skipped
  a Discworld player's own world in favour of the seeded stock one; pointed
  paths at files it never copied; ran a path in a world file to the end of the
  line past `&quot;`; missed forward-slash and doubled-backslash forms; kept
  "Worlds" as written; accepted 6.00's own folder as a 5.x; dropped long
  registry values; Wine encodings and fonts; the warning text understated what
  is replaced.
- **Spell checker:** built-in path ignored new userdict words; appends ran on to
  an unterminated last line; COM checker released after `CoUninitialize`;
  migration repeated every load (and an unreadable old database disabled
  checking); a refused "Add to System" looked added; Windows chose by regional
  format rather than display language; Enchant locale precedence; macOS with no
  dictionaries; UTF-8 words split off Windows.
- **Toolbars:** Reset Toolbar Locations could leave the last bar half off a
  ~1000 px window (MFC wraps only a bar that starts past the edge). Port: a drag
  in progress was adopted as a drop; a hidden bar lost its row through a drag.

### Found by using the client
- **A new world printed "Your chat name changed from <no name> to
  Name-not-set"** (UPSTREAM-BUGS 95 follow-up).
- **Clearing the output paused it** (`clearOutput` scrolled home against the
  emptied buffer's old scroll sizes, so auto-pause read it as scrolling back;
  new output then stayed off the bottom). Upstream code; also Display ▸ Clear
  Output. `win_worldview.cpp`.
- **Closing a new world asked "Save changes to Untitled?"** (a world never has
  MFC's title); it names the world now.
- **Relative sound names ignored the user's sounds folder**: an import (or the
  user) puts sounds in the data folder, `PlaySound` looked only beside the exe.
  It looks in the data folder first; `GetInfo (74)` names it.

### Windows networking (`pal/win_transport.cpp`)
- TLS sends could drop the connection: `SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER`
  was set on the POSIX transport only (its comment said Windows had no TLS).
- A send while the TCP connect was pending (an active-when-closed timer) took
  `WSAENOTCONN` for a hard error, replaced the event mask and left the world
  "Connecting" for good; now queued.
- An immediate connect could deliver `onConnect` twice (posted message plus
  `FD_CONNECT`); now once.
- A TLS write that wanted to read was never retried until the next send.
- A host name that would not resolve left the transport in place, so a timer
  running while closed queued sends into it without limit; it is freed once the
  failure is reported (checked: an unresolvable host, then a reconnect).

### Script engines (64-bit embedded)
- **JScript's `VBArray(wildcards).toArray()` gave `[whole, %1, …]`**, so every
  5.x JScript callback (`exampscript.js` included) read its wildcards one off.
  It now gives 5.x's shape (`%1…%9`, then the whole line, 1-based); direct
  indexing is unchanged. Selftest added.
- **Python callbacks got Lua's `[whole, %1, …]`** where 5.x ActivePython
  (pywin32) gave a tuple with `wildcards[0] == %1` — which `exampscript.pys`
  assumes. Decided by the user after this pass: match 5.x. Python now gets a
  tuple of ten, `%1…%9` then the whole line, missing captures `""`, on every
  platform (the port's Python shares `py_engine.cpp`). Selftest updated.
- A missing or broken `python\` aborted the client (`Py_InitializeEx` →
  `Py_FatalError`); now an error and no engine.
- Python source in the ANSI code page was a SyntaxError at its first accented
  byte (an author's name in a comment will do); decoded from that code page.
  Checked in the client: a Windows-1252 world script with an accented comment
  loads and runs.
- Ruby raised (→ `rb_bug` → abort) on a non-ASCII callback name.
- `round_to_long` clamped at ±2^63, leaving a number past 2^31 undefined on
  Windows (`LONG_MIN` on x64, `LONG_MAX` on ARM64).

### Display and miniwindows
- `WindowText` with `Unicode=false` drew short GBK/Big5 strings as UTF-8 when
  they happened to be valid UTF-8 (the probe assumed that case unreachable).
- `WindowRectOp` frame drew a mirrored frame for an inverted rectangle
  (`FrameRect` draws nothing).
- Page Down / line down / wheel with less than a screenful set a negative
  scroll offset (the text dropped down, then snapped back). Upstream.
- A miniwindow deleted under the mouse left a stale id that stopped tooltips
  and selection until the next click. Upstream.
- The GDI adapter's 32-bit surface ids wrapped after 2^32 image draws and could
  reuse a live miniwindow's; ids in use are skipped.

### Chat (raw 5.x `CAsyncSocket` code, reachable by a remote peer)
- A socket could be reaped (from `WM_TIMER`, which modal loops deliver) while
  one of its own prompts was up — accept this call / file / snoop, or
  `ChatSendFile`'s picker — and was used when the box closed. Sockets inside a
  prompt are kept (`m_iInModal`) and checked afterwards; one offer at a time.
- A peer's file named `NUL` crashed the client (`CFile::Remove` threw from the
  destructor). Device names, `:` streams and dot-only names are refused, and the
  delete is exception-safe.
- Auto-saved files replaced existing ones (and a cancel deleted them); the next
  free `name (2).ext` is used.
- Unparsed input and unsent output grew without limit (a frozen UI, quadratic
  copying); capped at 1 MB / 16 MB, then the session is closed.
- Chat sockets were reaped only while the world's timers were enabled.
- An unsolicited `CHAT_CONNECTION_LIST` made the client dial every host in it;
  only an answer to our own request is acted on, at most 100 calls, each once.
- Lookups that completed during a modal loop were dropped (handled only in
  `PreTranslateMessage`); a real message handler now.
- Sending a file of 2 GB or more announced its size modulo 2^32; refused.
- Chat ports ≥ 32768 went negative (`short`).
Tests: `ChatFileBlockR6.AnAutoSavedFileNeverReplacesOneAlreadyThere`,
`…AConnectionListNobodyAskedForIsNotDialled`.

### Windows UI shell
- A notepad's "file changed on disk" message shared WM_USER + 1001 with the
  script-file one and nothing handled it: the reload question never came.
  Upstream. Checked in the client.
- The Activity List, sorted by anything but Seq ascending, rebuilt itself on
  every update (rows looked up by view number), losing its selection; and
  Popup ▸ Close left the freed world's row selected. Upstream.
- With the icon in the tray, an Explorer restart removed it for good
  (`TaskbarCreated` was not handled). Upstream. Not exercised here: restarting
  Explorer on this machine was not something to do unasked.

## Performance (measured in the running client, ARM64, Release)

`tools/bench.lua`, run in Game ▸ Immediate, times `Simulate`, triggers, notes,
repaints and miniwindow drawing. Lines per second unless stated:

| | before | after |
|---|---:|---:|
| plain lines (no plugins) | 618 | 105,000 |
| plain lines (4 plugins, 2 VBScript) | 359 | 98,000 |
| `Note` | 662 | 134,000 |
| +200 non-matching regex triggers | 694 | 31,800 |
| full-window repaint | 5.4 ms | 3.8 ms |

- **The scroll bar was updated, and redrawn, for every line.** `SetScrollInfo`
  and `SetScrollPos` cost ~2.9 ms per line with visual styles (which the
  embedded manifest turned on): 2000 lines took 5.85 s with the bar shown and
  0.05 s hidden. The range and position are now recorded per line and given to
  the bar once per burst, by a posted message (handled before `WM_PAINT`), and
  first by anything that reads the bar back. This was the whole of the
  difference; with it fixed, plugins cost almost nothing.
- **A screen-sized back buffer per paint** is now one kept buffer (a paint
  inside a paint gets its own).
- **"Expand variables" triggers recompiled their regex on every line**; they
  keep it until the expanded pattern changes (failed compiles still retry).

**With the Discworld edition's plugins** (Quow's six, 752 triggers, 604 of them
in the Cow Bar) a plain line costs ~400 µs (2,484 lines/s; notes, which skip
triggers, 114,649/s). GetTriggerInfo 37 puts it in the plugins' own patterns:
about fifteen near-identical Cow Bar regexes built on
`(?:\w+\b\W*?){1,8}` with look-aheads, ~6 µs each per line of backtracking.
**PCRE's JIT is now built in on Windows** (added after this pass, at the
user's request). It is PCRE 8.45's own `pcre_jit_compile.c` and `sljit/`,
byte-identical to the release archive, and sljit handles MSVC on x86, x64 and
ARM64. Every trigger and alias pattern (`regexp.cpp`) and every Lua `rex`
pattern (`lrexlib.c`) is compiled through `src/pcre_jit.h`. Same client, same
Quow set, before and after:

| | interpreter | JIT |
|---|---:|---:|
| plain lines | 2,610/s | 5,486/s |
| +200 non-matching regex triggers | 2,461/s | 4,557/s |
| slowest Cow Bar trigger, 30,000 attempts | 0.198 s | 0.036 s |
| all 752 plugin triggers' matching | 8.2 s | 3.3 s |

The JIT also removes a crash. The interpreter recurses on the machine stack
about once per repeat of a group, so one long line against a pattern such as
`^(?:a|b)*c$` overflowed the stack. In the client, a 20,001-character line
through `rex` killed the Release build (0xC000041D); the JIT build matched it.
The JIT backtracks on a stack of its own: up to 8 MB per thread, committed as
used. A match that outgrows even that is `PCRE_ERROR_JIT_STACKLIMIT`, reported
("JIT Stack Limit") and handled like the match limit. It is deliberately not
rerun in the interpreter, which is shallower still.

Not on macOS or Linux: 8.45's sljit maps its code `MAP_JIT` but never calls
`pthread_jit_write_protect_np`, so under the hardened runtime on Apple silicon
writing the code would fault. The port keeps the interpreter, and with it the
deep-line crash.

Not done, measured as not worth the risk now: per-trigger overhead is ~110 ns
(two `QueryPerformanceCounter` reads for the statistics, a `strlen`, the
`pcre_exec`), i.e. 1000 triggers ≈ 9,000 lines/s; per-run double
measurement in `OnDraw`; plugin callback string copies; `SortTriggers` per add.

## Not fixed, and why

- **Python threads calling world methods off the UI thread** are unsynchronised;
  marshalling them is a design change.
- **Text crossing world methods on multi-byte (CJK) or UTF-8 system code pages**
  round-trips through the ANSI code page (`mc_dispatch.cpp`); Tcl re-encodes
  Latin-1 bytes. Design-level.
- **Scrollback beyond ~97M lines** overflows `int` pixel arithmetic in
  `mushview.cpp` (~20 GB of buffer); `pal/scroll_geometry.h` fixed it port-side.
- **Miniwindow brush background mode** (dashed outlines inherit the last mode),
  **DNS cancel under Wine**
  (`WSACancelAsyncRequest` is a stub there), **MCCP + reconnect from a trigger**,
  **IPv4 only**, **no TLS handshake timeout** — recorded, low impact, or 5.x's.
- **Chat, lower priority:** a zChat frame over 64 KB keeps a truncated length
  header (a MudMaster peer's huge text relayed desyncs zChat peers); a plugin
  stopping the listen socket from its `ON_PLUGIN_CHAT_DISPLAY` callback inside
  `OnAccept`; `Process_Stamp` broadcasting a decimal stamp; no handshake
  timeout or connection cap for incoming calls; UDP listeners (a handler
  re-listening on its own port, negative `short` ports, not released on plugin
  unload, >999-byte datagrams dropped silently).
- **Window tabs at the bottom** use `TCS_BOTTOM`, which Common Controls v6 does
  not support: they draw as top tabs on a bottom bar, and `CMDITabs`' classic
  edge painting mismatches the themed border (top too). Cosmetic, tabs off by
  default; a proper fix is owner-drawn bottom tabs.
- **Port-only toolbar selftest** "a hidden bar does not collide…" became
  vacuous when `GetPlacement` moved to the stored arrangement.
- Nothing port-side in this pass has been compiled (Mac/Enchant spell engines,
  the toolbar drag guard, the bundled-files dialog, the importer's Wine search).
