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
running client. Tests: 1730 pass on ARM64, x64 and Win32; the packaged ARM64
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
The client-side cure is **PCRE's JIT**, typically several times faster on such
patterns — but the bundled `pcre/` has no `pcre_jit_compile.c` or sljit, so it
means vendoring them (and confirming sljit's Windows ARM64 support) and a
fallback to the interpreter on `PCRE_ERROR_JIT_STACKLIMIT`. Left for a
decision. 400 µs a line is still 2,500 lines a second, well above what a MUD
sends.

Not done, measured as not worth the risk now: per-trigger overhead is ~110 ns
(two `QueryPerformanceCounter` reads for the statistics, a `strlen`, the
`pcre_exec`), i.e. 1000 triggers ≈ 9,000 lines/s; PCRE JIT (not compiled in;
see above); per-run double measurement in `OnDraw`; plugin
callback string copies; `SortTriggers` per add.

## Not fixed, and why

- **Python wildcard shape** — the decision is the user's: 5.x ActivePython (via
  pywin32) gave `wildcards[0] == %1`; the embedded engine gives Lua's
  `[0] == whole line`, which the port's scripts already use, and
  `exampscript.pys` expects the former. Queued.
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
- **Window tabs at the bottom** use `TCS_BOTTOM`, which Common Controls v6 does
  not support: they draw as top tabs on a bottom bar, and `CMDITabs`' classic
  edge painting mismatches the themed border (top too). Cosmetic, tabs off by
  default; a proper fix is owner-drawn bottom tabs.
- **Port-only toolbar selftest** "a hidden bar does not collide…" became
  vacuous when `GetPlacement` moved to the stored arrangement.
- Nothing port-side in this pass has been compiled (Mac/Enchant spell engines,
  the toolbar drag guard, the bundled-files dialog, the importer's Wine search).
