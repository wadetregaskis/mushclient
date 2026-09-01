# Bug hunt and fix campaign — 2026-09-01

A full-tree hunt over the port/shim/adapter surface against the canonical
Windows source, then a fix campaign over everything it confirmed. Recorded so
none of it is re-triaged from scratch (the backlog-goes-stale rule).

**Method.** 12 finder lenses -> 79 raw findings -> 76 after dedup -> one
adversarial verifier per finding (re-derive at HEAD, attempt refutation, check
the knowns registry and git log). Verdicts: **70 CONFIRMED, 4 ALREADY_KNOWN,
2 REFUTED.** All 70 confirmed findings were then fixed across two waves of
worktree-isolated agents grouped by root cause (11 clusters), each finding
carrying a regression test, merged serially with the suite green between
merges. Final state: `ctest` 78/78 (extended on), ASan suite 77/77.

## Not fixed, and why — do not re-file these

- **REFUTED: Ctrl+Home/End inside the output pane** — the pane never has focus
  (see the outputpane-never-focused record); the handlers are unreachable, not
  broken. Ctrl+Home/End work from the command bar.
- **REFUTED: View menu Info Bar wiring** — the "missing" caller sits behind
  `CMainFrame::OnCreate`'s port early-return; the port's info bar is separate
  and live.
- **ALREADY_KNOWN (4)**: per-character `DrawText` (the batch optimisation is
  refuted BY MEASUREMENT — Core Text fractional advances; see
  AUDIT-2026-08.md, do not re-attempt), the output-fade repaint timer, and two
  perf items already recorded in AUDIT-2026-08.md.

## Deliberate residuals from the fix wave

- `DrawEdge`'s **BF_SOFT** is a recorded gap (UPSTREAM-DIVERGENCES.md §8): it
  changes only a bevel grey no macOS observation can check; a plausible-wrong
  shade would be worse than an acknowledged gap.
- **MudMaster file transfer stays ~25 KB/s**: the Poll drain loop cannot help a
  lockstep protocol; speeding it up (adaptive tick or a nudge-Poll after
  CHAT_FILE_BLOCK_REQUEST) is its own decision.
- ~~Windows' `OnTestEnd` also clears the output selection~~ — **closed later the
  same day**: `FireScrollCommand`'s End arm now clears it (Start pinned as the
  arm that must not).
- ~~`FireMacro`'s SEND_NOW arm bypasses unpause_on_send~~ — **closed later the
  same day**, mirroring `SendCommand`'s tail (sendvw.cpp:756 via SendMacro).
- `WindowDrawImageAlpha`/`WindowGetImageAlpha` now **refuse 24-bit images with
  eImageNotInstalled on macOS as they always did on Windows** (gdi#1) — a
  user-visible change that is parity, flagged in WINDOWS-VALIDATION.md §10bb.
- ~~`dialogs/Splash.cpp` reads an uninitialised `DIBSECTION` field through
  `CBitmap::GetObject`~~ — **closed later the same day**: `GetObject` honours
  the caller's declared buffer size (latent until the RESOURCE `LoadImage` form
  is ever served — `CSplashWnd::Create` bails on its null today).

## What ASan added

Run per the sanitizer trigger after each wave. Wave 1's run caught two brand-new
TEST fixture bugs (an uninitialised `m_pPage2` dereference in the wizard-DDV
test; an lpstrFile fixture violating OPENFILENAME's NUL-terminated-input
contract) — both fixture bugs, no shipping-code defects. Wave 2's run was clean.

## Windows validation owed

Each item has its script/checklist in docs/WINDOWS-VALIDATION.md (2026-09-01
sections) or UPSTREAM-DIVERGENCES.md (§22 for the entity-escape symmetry, §17
addendum for coercion): xml-options#2, dispatch-variant#1/#2, sockets#1,
twin-drift#0, frame-routing#0/#1, stub-inventory#1, and the gdi geometry items
whose Windows numbers are inferred rather than measured (§10az/§10ba/§10bb).

## Disposition, by cluster

Merge commits: wave 1 `41a0979`..`f91e6e9` (+ `6d54c3e` seam-space test fix),
wave 2 `..` through the sockets/twins merges. Every finding below is at
severity as CORRECTED by its verifier, with per-finding fix/test detail in the
merged commits themselves.

### Cluster `xml-crlf`
- `0178cf7` fix: multi-line DDX fields translate CRLF at the wx seam instead of doubling it
- `a92bdf1` fix: Display > Multi-Line Trigger can no longer store a raw line break
- `0a3cd30` fix: an attribute value with a control character saves as an entity, not raw
- `17b8f56` fix: a save-a-copy notepad save no longer re-points the document
  - **xml-options#0** (high, fixed) DDX multi-line fields double CRLF on display and save back LF-only
  - **xml-options#1** (high, fixed) Display > Multi-Line Trigger stores raw LFs in the match text, producing a world file that will not reload
  - **xml-options#2** (high, fixed) The port-only &#10; / &#13; entity widening admits attribute values the saver cannot re-emit — **needs a Windows run**
  - **xml-options#3** (medium, fixed) Notepad save ignores bReplace and always re-points the document at the new file

### Cluster `engines`
- `ff093c6` fix: a Ruby handler returning an Integer beyond 64 bits no longer kills the client
- `c3979ce` fix: Tcl and Ruby rich-callback arguments no longer truncate at the first NUL byte
- `1241342` fix: Tcl_FindExecutable is handed the real executable path, so [info nameofexecutable] works
- `56916d5` fix: a Python script error carrying non-UTF-8 text is reported, not blanked
- `88a1887` fix: a Python return value whose truthiness raises is a script error, not a leak
- `093bf5a` fix: the Python module namespace carries __name__ = "__main__"
- `b942ac3` fix: a Python int beyond double range marshals as its digits, not -1 plus a leaked exception
- `b8e1ee2` fix: the Python GIL is released between callbacks, so script threads run
  - **engines#0** (high, fixed) Ruby callback returning an Integer outside long long range kills the process
  - **engines#1** (low, fixed) Python script errors carrying non-UTF-8 text are reported with an empty description
  - **engines#3** (low, fixed) A Python handler whose return value is not cleanly truthy leaves an exception pending for the NEXT callback
  - **engines#4** (medium, fixed) The Python module namespace has no __name__, so common module-level idioms fail at script load
  - **engines#5** (low, fixed) Tcl_FindExecutable(NULL) does not do what the comment beside it claims
  - **engines#6** (low, fixed) The Python GIL is held between callbacks, so a script's own threads never run
  - **dispatch-variant#3** (low, fixed) Tcl and Ruby rich-callback arguments truncate at the first NUL byte
  - **dispatch-variant#4** (low, fixed) py_to_variant leaves a pending OverflowError for ints beyond double range

### Cluster `dialog-bridge`
- `bec5224` fix: the caption close of a bridged dialog runs the canonical OnCancel
- `f20fad9` fix: LBS_SORT / CBS_SORT reach the bridged list boxes and combos
- `700905c` fix: ES_NUMBER and ES_UPPERCASE reach the bridged edit controls
- `0158f21` fix: a right-click on a bridged list dispatches NM_RDBLCLK
- `cd8b4a6` fix: a DDV failure returns focus to the offending control
- `6317db6` fix: DDV_MaxChars enforces maximum string lengths
- `7f31278` fix: the template's BS_DEFPUSHBUTTON becomes the bridged dialog's default item
- `ccfe270` fix: Esc in a bridged dialog runs the canonical Cancel arm
  - **dialog-bridge#0** (medium, fixed) Bridged dialogs lose the default button and wx's Esc-to-Cancel routing
  - **dialog-bridge#1** (low, fixed) Window-close of a bridged dialog bypasses canonical OnCancel overrides
  - **dialog-bridge#2** (low, fixed) DDV_MaxChars is a permanent no-op, so every maximum-length validation is silently skipped
  - **dialog-bridge#4** (low, fixed) NM_RDBLCLK never synthesized: PluginsDlg right-double-click state-file editor unreachable
  - **dialog-bridge#5** (low, fixed) LBS_SORT / CBS_SORT are dropped: four sorted controls display in insertion order
  - **dialog-bridge#6** (low, fixed) ES_NUMBER and ES_UPPERCASE edit styles are dropped
  - **dialog-bridge#7** (low, fixed) DDV/DDX failure does not return focus to (or select) the offending control

### Cluster `gdi`
- `d41ccce` fix: CDC::Chord traces the exclusive bounding rectangle, like every other shape
- `cea7462` fix: CDC::Pie's radials stop on the ellipse instead of shooting to the raw points
- `8d5c1e7` fix: CDC::DrawEdge's rings stay inside the rectangle they were given
- `2ad6ca7` fix: CDC::DrawEdge honours BF_DIAGONAL, BF_MONO and BF_FLAT
- `6c3938f` fix: a NULL-pen Rectangle performs GDI's one-pixel right/bottom shrink
- `7d5f03e` fix: PS_INSIDEFRAME keeps the whole pen inside the bounding rectangle
- `9b00e0a` fix: mono pattern brushes key transparency on BrushColour, not PenColour
- `2c7c33f` fix: the wx CBitmap::GetBitmap/GetObject zero the caller's BITMAP before failing
- `3f96b91` fix: an image reports the DIB's bit count and stride, not the wxBitmap's
- `404c9fd` perf: a miniwindow font's metrics are measured once, not once per query
- `88748fb` test: mirror the new geometry probes into the cross-platform conformance script
  - **gdi#0** (medium, fixed) CDC::Pie strokes the two radials to the caller's raw ray points, not to the arc endpoints on the ellipse
  - **gdi#1** (low, fixed) Every loaded miniwindow image reports bmBitsPixel 32, so 24-bit images misreport their depth and the alpha ops — **needs a Windows run**
  - **gdi#2** (low, fixed) Mono pattern brushes test the wrong colour for GDI's transparent-gap case (PenColour instead of BrushColour) — **needs a Windows run**
  - **gdi#3** (low, fixed) CDC::DrawEdge's rings are one pixel too long on every side, painting outside the caller's rectangle and over t — **needs a Windows run**
  - **gdi#4** (low, fixed) PS_INSIDEFRAME is mapped to an ordinary centred solid pen, so wide-pen figures overflow their bounding rectang — **needs a Windows run**
  - **gdi#5** (low, fixed) The wx adapter does not implement the null-pen one-pixel shrink that PAL.md calls normative and CpuRaster's go — **needs a Windows run**
  - **gdi#6** (low, fixed) CBitmap::GetBitmap / GetObject never fill bmWidthBytes, so world.WindowImageInfo(win, id, 4) always returns 0 — **needs a Windows run**
  - **gdi#7** (low, fixed) CDC::Chord builds its ellipse over the inclusive rectangle, so a chord is one pixel larger than an Ellipse/Pie
  - **gdi#8** (low, fixed) The wx CBitmap::GetBitmap returns FALSE without zeroing the BITMAP, breaking the documented zero-first contrac
  - **gdi#9** (low, fixed) CDC::DrawEdge silently ignores BF_DIAGONAL, BF_FLAT, BF_MONO and BF_SOFT, all of which WindowRectOp lets throu — **needs a Windows run**
  - **perf#1** (low, fixed) CDC::GetTextMetrics measures 95 glyphs on every call, where GDI returns cached metrics in O(1)

### Cluster `outputpane`
- `9e99acd25` fix: Copy breaks a selection only where the LINE ended, not at the engine's wrap
- `b042a67df` fix: the MXP link menu labels items with the MUD's hints, and drops "Send All"
- `e1381ea0e` fix: a drag past the edge keeps scrolling while the mouse is still
- `df06ed555` fix: Find Next visits every occurrence on a line, not just the first
- `5ef00e3de` fix: Go To Bookmark scans from the selection, not from its own call history
- `61394962b` fix: an action still carrying "&text;" is not a link anywhere
- `0dc6cb16f` fix: a left-click inside the selection leaves it intact
- `4c20c59c7` fix: new output clears the selection when the view is not frozen
  - **outputpane-parity#0** (medium, fixed) Copy inserts newlines at engine soft-wrap boundaries — **needs a Windows run**
  - **outputpane-parity#3** (low, fixed) Context-menu 'Send All (in order)' reopens the chooser instead of sending
  - **outputpane-parity#9** (low, fixed) Multi-action link menu items are labeled with the MUD's hints, not raw actions (same commit and test as #3)
  - **outputpane-parity#4** (medium, fixed) Drag-select auto-scroll stalls while the mouse is stationary past the edge
  - **outputpane-parity#6** (low, fixed) Find Next skips additional matches on the same line
  - **outputpane-parity#7** (low, fixed) Go To Bookmark cycles from a private member, ignoring the current selection
  - **outputpane-parity#8** (low, fixed) Links with an unsubstituted &text; are clickable on the port; Windows refuses them
  - **outputpane-parity#10** (low, fixed) Left-click inside the selection collapses it; Windows leaves it intact
  - **outputpane-parity#11** (low, fixed) New output does not clear the selection when the view is not frozen

### Cluster `shim-protocol`
- `aaa5bd5` fix: CTypedPtrList::Find resumes strictly after the position it is handed
- `3a4ddf7` fix: the DDV minimum-length prompt says what it wants, not just a number
- `e513723` fix: text-mode reads collapse CRLF, so Tip of the Day is not CR-terminated
- `aa9c969` fix: the Colour Picker's Other colour... button opens a colour panel
- `d3a537a` fix: CFileDialog writes the chosen path back into m_ofn.lpstrFile
- `65021c6` fix: CWnd's geometry-mapping overloads answer for real, in every spelling
- `0947380` fix: the Complete Word popup is anchored at the caret again
  - **shim-protocol#0** (low, fixed) CColorDialog never bridges: Colour Picker's 'Other colour...' button and swatch click silently do nothing
  - **shim-protocol#1** (low, fixed) Complete Word/Function popup positioning chain is fully inert: dialog opens far from the caret
  - **shim-protocol#2** (low, fixed) CRect&-reference geometry overloads on base CWnd still zero/no-op after the 5cfd0e5 overload fix
  - **shim-protocol#3** (low, fixed) CTypedPtrList::Find(value, startAfter) searches from AT the given position; MFC and both sibling shims search 
  - **shim-protocol#4** (low, fixed) CFileDialog never writes the chosen path back into m_ofn.lpstrFile (OPENFILENAME write-back protocol)
  - **shim-protocol#5** (low, fixed) Text-mode CR stripping is absent: CStdioFile::ReadString and the _fgetts alias keep '\r', so Tip of the Day te
  - **shim-protocol#6** (low, fixed) AfxFormatString1 drops the format string: DDV minimum-length validation shows a message box containing only a 

### Cluster `dispatch`
- `f51af6836` fix: the array delimiter's Lua default reaches the five embedded engines
- `3b48b3999` fix: a fraction reaching an integer parameter rounds the way oleaut does
- `d23d0433e` fix: a script's true is VARIANT_TRUE where a number or string was declared
  - **dispatch-variant#0** (low, fixed) Lua string-typed argument defaults are inexpressible in kArgDefaults (Array delimiter)
  - **dispatch-variant#1** (medium, fixed) VT_R8/VT_R4/numeric-string to integer coercion truncates where Windows oleaut rounds — **needs a Windows run**
  - **dispatch-variant#2** (low, fixed) VT_BOOL crossing into numeric/string parameters yields 1/"1", not VARIANT_TRUE's -1 — **needs a Windows run**

### Cluster `worldchild-links`
- `8629f40` fix: a link click unpauses the output window when unpause_on_send says so
- `6f9d435` fix: a click on a multi-alternative MXP link sends the default action
- `f480c24` fix: a link's flyover names the command the click will send
- `a90c810` fix: no tooltip in the empty area below the last output line
- `3d58310` fix: jumping to the end of the buffer obeys keep_pause_at_bottom
- `5bdbaea` fix: the Display menu carries the six scroll commands; Ctrl+Up/Down nudge a line
  - **outputpane-parity#1** (medium, fixed) unpause_on_send is dead on the hyperlink-click path
  - **outputpane-parity#2** (low, fixed) Left-click on a multi-action send-link opens a chooser instead of sending the default action
  - **outputpane-parity#5** (low, fixed) Tooltip for a hint-less link shows line info; multi-hint links show the raw pipe string
  - **outputpane-parity#12** (low, fixed) Line-info tooltip appears in the empty area below the last line
  - **outputpane-parity#15** (low, fixed) Ctrl+End ignores keep_pause_at_bottom's latch
  - **outputpane-parity#13** (low, fixed) Display menu scroll commands missing; Line Up / Line Down have no port equivalent

### Cluster `frame-routing`
- `b38e3f5` fix: a trigger's "sound only if inactive" could never suppress anything
- `6f73471` fix: View > Always On Top answers its canonical command id
- `2444133` fix: Game > Activity List ticks while the activity window is open
- `e6936ff` fix: the menu bar greys World Properties while a connect is in flight
- `fc64c10` fix: the world toolbar's check buttons could show the opposite of the truth
- `a0ed77e` fix: the 30 keypad command ids did nothing on the port
- `7a75d35` fix: the scroll, caret and window command ids were silent no-ops
- `147a71b` fix: a colour swatch's right-click Copy/Paste menu was wholly unreachable
  - **frame-routing#0** (medium, fixed) Trigger option "sound only if window inactive" is vacuously true on the port — the sound always plays — **needs a Windows run**
  - **frame-routing#1** (low, fixed) Keypad command ids have no port route — world.DoCommand("Keypad1") is a silent no-op — **needs a Windows run**
  - **frame-routing#2** (low, fixed) View ▸ Always On Top is built on a private wx id, so world.DoCommand("AlwaysOnTop") does nothing
  - **frame-routing#3** (medium, fixed) World toolbar's check buttons are never re-synchronised — Freeze Output can show the opposite of the truth
  - **frame-routing#4** (low, fixed) Commands whose only handler is CMUSHView/CSendView are unreachable by command id — scroll, caret and window co
  - **frame-routing#5** (low, fixed) Game ▸ Activity List is a plain menu item where Windows ticks it while the Activity window is open
  - **frame-routing#6** (low, fixed) Menu-bar Game ▸ World Properties stays enabled while connecting, where Windows greys it
  - **stub-inventory#1** (low, fixed) Colour-swatch right-click Copy/Paste menu is completely dead (no event wiring + no menu-resource loader) — **needs a Windows run**

### Cluster `sockets`
- `68d1dfe` fix: the MudMaster framer stops at an embedded NUL, the way MFC's Find does
- `a471ab2` perf: the Poll pump drains a socket's backlog within one pass
- `eaf7ab7` fix: the POSIX shim stops inventing socket options MFC never sets
  - **sockets#1** (low, fixed) MudMaster chat framing finds a terminator past an embedded NUL that MFC's CString::Find cannot see — **needs a Windows run**
  - **sockets#2** (low, fixed) The Poll pump delivers at most one OnReceive per socket per 20 ms; Winsock's FD_READ re-arms immediately
  - **sockets#3** (low, fixed) The POSIX shim disables Nagle on every TCP socket; MFC and the Windows adapter do not

### Cluster `twins`
- `55566e3` fix: SendNAWS's port-only twin escapes a dimension byte of exactly 255
- `8ff8ebc` fix: Set_Up_Set_Strings' port twin drops the per-set suggested-name suffix
- `c27de9b` fix: UMessageBox's port twin drops Windows' default-icon fill
- `9c93e57` fix: GetNestedFunction's portable twin skips the null guard and trims
  - **twin-drift#0** (medium, fixed) SendNAWS twin still sends an unescaped IAC — the 255-byte NAWS fix (f878ff5) missed the port's own packet buil — **needs a Windows run**
  - **twin-drift#1** (low, fixed) Set_Up_Set_Strings port twin drops the per-set suffix on the suggested export filename (and the STRING set typ
  - **twin-drift#2** (low, fixed) UMessageBox port twin drops Win32's default-icon fill, so every icon-less prompt loses its severity cue
  - **twin-drift#4** (low, fixed) Portable Lua twin: GetNestedFunction lacks the empty/NULL-name guard and the whitespace-trimming split the rea
