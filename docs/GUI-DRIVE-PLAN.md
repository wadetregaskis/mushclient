# GUI Drive Plan — full-app verification by system control

A step-by-step operating plan for an agent (target: Sonnet-class) to verify every
user-reachable feature of the macOS MUSHclient port by **actually driving the
running app** — real clicks, real keystrokes, real screenshots — via the
computer-use MCP. This is the coverage the in-app selftests structurally cannot
give (they exit from `OnInit`, so paint, modals, focus, and real typing never
happen there). A 2026-08-10 session that did exactly this by hand found three
bugs that 63 green tests and three audits had missed; this plan systematizes it.

**Your job is VERIFICATION ONLY.** Find and record; never fix, never commit,
never touch git state. Every claim in your report must be backed by evidence
you captured (screenshot, clipboard read, server log line, or file artifact).

---

## 0. Non-negotiable rules

Read these twice. Each one is a scar.

1. **Never synthesize a global ⌘Q** (it once killed the controlling session).
   Quit MUSHclient only by (a) clicking its own app-menu ▸ Quit with MUSHclient
   confirmed frontmost in a screenshot taken immediately before, or (b)
   `pkill -9 -f MUSHclient.app/Contents/MacOS/MUSHclient` from Bash.
2. **Only ever run and kill `build/MUSHclient.app`.** Never launch, quit, or
   touch anything under `dist/` — that is the user's installed copy.
3. **Never open, save over, or let a file dialog land in
   `~/Library/Application Support/MUSHclient/worlds/`.** The user's world file
   must stay byte-identical; verify at setup AND teardown:
   ```bash
   shasum "/Users/robot/Library/Application Support/MUSHclient/worlds/Discworld.mcl"
   ```
   must print `299af2670fb8cc51ea0837993a7225d5a008c630`. If it ever differs,
   STOP everything and report immediately.
4. **Never connect to a real MUD.** All connections go to the local test server
   (§3.1) on 127.0.0.1. Test worlds use only that address.
5. **Preferences are the user's real preferences.** wxConfig writes to the
   `mushclient Preferences` domain in the user's real `~/Library/Preferences`
   — `$HOME` games do NOT redirect it. Back up before the first launch,
   restore at teardown unconditionally (§2.2, §8).
6. **Screenshot before every type/click batch and confirm MUSHclient is
   frontmost.** Keystrokes go wherever focus is. If anything else is frontmost,
   re-activate the app (`open ./build/MUSHclient.app` re-fronts a running
   instance) and re-screenshot before proceeding.
7. **The Escape key does not reach this app through the automation harness**
   (proven 2026-08-11: every other key worked; the user's own Escape press
   worked; the synthesized one never arrived). Never depend on Escape — close
   every dialog with its own Cancel/OK/close button. If any OTHER key seems
   dead, suspect the instrument before the app (§7).
8. **Numeric-keypad keys cannot be synthesized** on this hardware/harness.
   Keypad checks are recorded BLOCKED, not attempted.
9. **No code changes, no `git add/commit`, no pushes.** `git status --short`
   in the repo must be as clean at teardown as at setup.
10. **One instance only.** Before every launch:
    ```bash
    pkill -9 -f MUSHclient.app/Contents/MacOS/MUSHclient; sleep 1
    pgrep -f MUSHclient.app/Contents/MacOS/MUSHclient | wc -l   # must print 0
    ```
11. **Never click a web link in the output pane** (safety policy: links open the
    real browser). Verify link affordance visually only.
12. **All shell work through the Bash tool**, never by driving Terminal's GUI
    (Terminal is click-tier anyway; typing into it is blocked).
13. Run artifacts live in a **run directory OUTSIDE the repository**:
    `~/mushclient-gui-drive/run-<YYYYMMDD>/` (create it first thing). Nothing
    of yours goes inside `/Users/robot/Documents/mushclient`.

---

## 1. Deliverables and evidence discipline

In the run directory, maintain:

- **`RESULTS.md`** — the ledger. One row per check, **updated immediately after
  each check** (the run must survive a dead session; a resuming agent starts
  from this file). Format:
  ```
  | ID | Title | Status | Evidence | Notes |
  ```
  Status is `PASS`, `FAIL`, `BLOCKED` (couldn't attempt, with reason), or
  `SKIP` (explicitly out of scope, with reason). Evidence is a filename in the
  run dir (screenshot, log excerpt) — **a PASS with no evidence is invalid**.
- **`BUGS.md`** — one entry per FAIL:
  ```
  ## BUG-<n>: <one-line summary>
  Check: <ID>  Severity: critical|high|medium|low|cosmetic
  Steps: <numbered, exactly what you did>
  Expected: ...   Actual: ...
  Evidence: <files>
  Instrument re-verified: <what you did to rule out the harness — §7 step 1>
  ```
- **`MENUS.md`** — the menu census (§P9).
- **`shots/`** — numbered screenshots `NNN-<checkid>.png`.
- **`server.log`** — written by the test server; this is the "what did the app
  actually send" oracle.

Evidence rules:

- An **action** check (something should happen) FAILS if nothing observable
  happened — "no visible change" is never a PASS for an action.
- A **state** check (something should be shown) needs the screenshot that shows
  it; use the `zoom` action for small text before judging legibility.
- When a check reads text from the screen, quote the text you read into the
  Notes column — that is what makes the row auditable later.

---

## 2. Phase 0 — setup and instrument calibration

Do these in order. **If any CAL check fails, stop and report an instrument
problem — do not proceed to feature checks with a broken instrument.**

### 2.1 Environment

```bash
mkdir -p ~/mushclient-gui-drive/run-$(date +%Y%m%d)/shots
cd /Users/robot/Documents/mushclient
git status --short          # record output in RESULTS.md header (expect clean)
cmake --build build -j6     # never unbounded -j
shasum "/Users/robot/Library/Application Support/MUSHclient/worlds/Discworld.mcl"
```
Record the sha (must be `299af267…`, rule 3).

### 2.2 Preferences backup + fresh-run state

**Corrected 2026-08-12, the hard way: `defaults export/import` does NOT
control this app's real settings.** `defaults domains | grep -i mush` finds
`au.com.gammon.MUSHclient` — but that domain is empty of app settings; it's
just AppKit's own per-bundle-ID bookkeeping (NSOpenPanel/NSColorPanel/
NSFontPanel frame positions, `NSAutomaticPeriodSubstitutionEnabled`). This
build's actual settings live in **two** other places, neither a `defaults`
domain:

- `~/Library/Preferences/mushclient Preferences` — a plain wxFileConfig ini
  file (recent-files list, window geometry, find state, toolbar visibility,
  display font).
- `~/Library/Preferences/MUSHclient/mushclient_prefs.sqlite` — a SQLite store
  (`control`/`prefs`/`worlds` tables) holding the real global registry,
  **including `WorldList` and `AutoConnectWorlds`**. This is the one that
  matters most: on a first run after this session's earlier work, launching
  with the ini file reset but this DB untouched auto-reopened and
  **auto-connected to the user's real, live Discworld account** — a rule-4
  violation caught only because the very first post-launch screenshot showed
  it. No commands were sent and the connection was killed within seconds
  (`pkill -9`), but treat this as the load-bearing lesson of this section.

Back up and reset BOTH, by moving them aside (not `defaults delete`, which
silently no-ops here since neither is a real defaults domain):
```bash
RUN=~/mushclient-gui-drive/run-$(date +%Y%m%d)
cp "$HOME/Library/Preferences/mushclient Preferences" "$RUN/prefs-backup-real.ini"
cp -R "$HOME/Library/Preferences/MUSHclient" "$RUN/prefs-backup-MUSHclient-dir"
sqlite3 "$RUN/prefs-backup-MUSHclient-dir/mushclient_prefs.sqlite" "PRAGMA integrity_check;"  # expect "ok" before trusting the backup
mv "$HOME/Library/Preferences/mushclient Preferences" "$HOME/Library/Preferences/mushclient Preferences.SESSIONBACKUP"
mv "$HOME/Library/Preferences/MUSHclient/mushclient_prefs.sqlite" "$HOME/Library/Preferences/MUSHclient/mushclient_prefs.sqlite.SESSIONBACKUP"
```
Renaming (not deleting) means a crash mid-run still leaves a recoverable
original one `mv` away, on top of the run-dir copy. Restoring both at
teardown is **mandatory** even if the run aborts (§8). Also worth a check:
`ls ~/Library/Saved\ Application\ State/ | grep -i mush` — empty is normal
(no AppKit window-restoration bundle); if one ever exists, it's a third thing
that could reopen a world and should be moved aside too.

**After resetting, take the very first post-launch screenshot before touching
anything else** and confirm no world is open / nothing is connected. That
screenshot is what caught the incident above — don't skip it, and don't
assume the reset worked without looking.

Note: with fresh state, `AutoConnectWorlds` defaults to ON — a world connects
the moment it opens. **Start the server (§3.1) before creating any world.**

### 2.3 Test server

Start the server from §3.1 in the background:
```bash
cd "$RUN" && nohup python3 server.py server.log > server.stdout 2>&1 &
sleep 1 && printf 'ping\r\n' | nc -w 2 127.0.0.1 4000   # expect WELCOME banner + "echo: ping"
```
The `nc` round-trip must succeed before anything else.

### 2.4 App launch

```bash
pkill -9 -f MUSHclient.app/Contents/MacOS/MUSHclient; sleep 1
cd /Users/robot/Documents/mushclient
open ./build/MUSHclient.app --args -ApplePersistenceIgnoreState YES
```
`-ApplePersistenceIgnoreState YES` suppresses the "reopen windows?" modal that
otherwise appears after repeated automated launches.

Then `request_access` for **MUSHclient** (full tier expected). Take a
screenshot; the app should be frontmost with an empty MDI frame and the
no-world menubar (File / Edit / View / Connection / Game / Window / Help).
If any first-run dialog appears, screenshot it, close it via its buttons,
record it in RESULTS.md.

### 2.5 Calibration checks

| ID | Check | How |
|---|---|---|
| CAL-1 | Screenshot legibility | Screenshot; `zoom` on the menubar; menu titles readable. |
| CAL-2 | Menu click works | Click Help ▸ About MUSHclient; the About box opens; close via its OK/close **button** (not Escape). |
| CAL-3 | Typing works | Open File ▸ New World (leave it open); click into the world-name field; type `CALTEST`; zoom-screenshot shows `CALTEST` in the field. |
| CAL-4 | Arrow keys work | In the same field, press Left twice then type `X`; field reads `CALTEXST`. Clear the field afterwards. |
| CAL-5 | Modifier chords work | With the dialog open, select the field text via triple-click, press ⌘C, then use the `read_clipboard` tool; the clipboard holds the field text. |
| CAL-6 | Cancel path | Close the dialog with its Cancel button; the dialog closes; no world was created. |

Record all six in RESULTS.md. Known instrument limits (do NOT re-test, just
respect them): Escape (rule 7), numpad (rule 8).

---

## 3. Fixtures

Write all of these into the run directory before Phase 1.

### 3.1 `server.py` — deterministic local test MUD

```python
#!/usr/bin/env python3
# Minimal deterministic test MUD for GUI-drive verification. Stdlib only.
import socket, threading, time, sys, datetime

HOST, PORT = "127.0.0.1", 4000
LOG = sys.argv[1] if len(sys.argv) > 1 else "server.log"
lock = threading.Lock()

def log(line):
    with lock:
        with open(LOG, "a") as f:
            f.write("%s %s\n" % (datetime.datetime.now().isoformat(), line))

def send(conn, text):
    conn.sendall(text.replace("\n", "\r\n").encode("utf-8"))

def ansi_block():
    out = []
    for base, label in ((30, "FG"), (90, "FG-BRIGHT")):
        out.append(label + ": " + " ".join(
            "\x1b[%dm#%d\x1b[0m" % (base + c, c) for c in range(8)))
    out.append("BG: " + "".join(
        "\x1b[%dm %d \x1b[0m" % (40 + c, c) for c in range(8)))
    out.append("STYLES: \x1b[1mbold\x1b[0m \x1b[4munderline\x1b[0m "
               "\x1b[7minverse\x1b[0m \x1b[3mitalic\x1b[0m")
    out.append("X256: \x1b[38;5;196mred196\x1b[0m \x1b[38;5;46mgreen46\x1b[0m "
               "\x1b[38;5;21mblue21\x1b[0m")
    return "\n".join(out) + "\n"

def handle(conn, addr):
    log("CONNECT %s" % (addr,))
    send(conn, "WELCOME-GUIDRIVE\nType commands; unknown lines are echoed back.\n")
    buf = b""
    try:
        while True:
            data = conn.recv(4096)
            if not data:
                break
            buf += data
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.rstrip(b"\r").decode("utf-8", "replace")
                log("RECV %r" % line)
                if line == "ansi":
                    send(conn, ansi_block())
                elif line == "long":
                    send(conn, "[wraptest]" + "abcdefghij" * 50 + "\n")
                elif line == "utf8":
                    send(conn, "UTF8: café naïve 世界 "
                               "—dash— tail\n")
                elif line == "url":
                    send(conn, "Link: http://example.invalid/guidrive\n")
                elif line.startswith("stream"):
                    try:
                        n = int(line.split()[1])
                    except Exception:
                        n = 20
                    for i in range(n):
                        send(conn, "stream-line-%03d\n" % i)
                        time.sleep(0.2)
                elif line == "quit":
                    send(conn, "BYE\n")
                    return
                else:
                    send(conn, "echo: %s\n" % line)
    finally:
        log("DISCONNECT %s" % (addr,))
        conn.close()

srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind((HOST, PORT))
srv.listen(4)
print("listening on %s:%d log=%s" % (HOST, PORT, LOG), flush=True)
while True:
    c, a = srv.accept()
    threading.Thread(target=handle, args=(c, a), daemon=True).start()
```

`server.log` is your primary "what did the app transmit" oracle: `RECV '...'`
lines are exactly what arrived, byte-faithful via `%r`.

### 3.2 `testplugin.xml` — miniwindow + alias + trigger probe

```xml
<!DOCTYPE muclient>
<muclient>
<plugin
   name="GuiDriveProbe"
   author="guidrive"
   id="0f9a8b7c6d5e4f3a2b1c0d9e"
   language="Lua"
   purpose="GUI drive verification probe"
   save_state="n"
   version="1.0"
>
</plugin>

<aliases>
  <alias match="pling" enabled="y" send_to="12" sequence="100">
    <send>
world.Note("PLUGIN-ALIAS-OK")
world.Send("plugin-sent")
    </send>
  </alias>
</aliases>

<triggers>
  <trigger match="echo: TRIGPLUG*" enabled="y" send_to="12" sequence="100">
    <send>
world.Note("PLUGIN-TRIGGER-OK")
world.Send("plugin-triggered")
    </send>
  </trigger>
</triggers>

<script>
<![CDATA[
local W = "guidriveprobe"

function OnPluginInstall ()
  WindowCreate (W, 0, 0, 140, 60, 6, 0, ColourNameToRGB ("blue"))
  WindowRectOp (W, 2, 4, 4, 136, 56, ColourNameToRGB ("red"))
  WindowFont (W, "f", "Menlo", 14, false, false, false, false)
  WindowText (W, "f", "MW-OK", 20, 20, 0, 0, ColourNameToRGB ("white"))
  WindowShow (W, true)
end

function OnPluginClose ()
  WindowDelete (W)
end
]]>
</script>
</muclient>
```

The miniwindow (position 6 = top-right of the output) is the **real paint-path
probe**: if "MW-OK" in white on red on blue is visible in a screenshot, the
whole miniwindow create/draw/text/composite/paint chain works in the shipping
GUI — the one thing no selftest can show.

### 3.3 `test.lua` — world script file

Initial content (the reload check appends to it later):
```lua
function OnWorldConnect ()
  world.Note ("SCRIPT-CONNECT-OK")
end
```

### 3.4 `beep.wav`

```bash
cd "$RUN" && python3 - <<'EOF'
import wave, struct, math
w = wave.open("beep.wav", "w")
w.setnchannels(1); w.setsampwidth(2); w.setframerate(22050)
for i in range(4410):
    w.writeframes(struct.pack("<h", int(12000 * math.sin(i * 2 * math.pi * 440 / 22050))))
w.close()
EOF
```

### 3.5 The `/` script oracle

Once scripting is enabled with prefix `/` (done during world creation, P1-02),
typing `/world.Note("TAG")` in the command bar prints `TAG` in the output pane
— your general-purpose in-app assertion. Use unique tags per check
(`/world.Note("P2-07-OK")`) so screenshots are self-labelling. **The prefix
default is EMPTY** (`scriptingoptions_alpha.inc:61`), so this only works after
P1-02 sets it.

### 3.6 File-dialog navigation

Every save/open panel: press ⌘⇧G (Go to folder), type the run directory path,
press Return once, verify the panel now shows the run dir, then type the
filename and click the Save/Open **button**. Never accept a panel whose
location you have not verified (rule 3).

---

## 4. Known absences — do NOT file these as bugs

Deliberately absent / already documented (record SKIP with the reason if a
check would touch them):

| Item | Why |
|---|---|
| Activity button-bar style | Blocked on artwork (16×15@4bpp upstream strips). |
| Timer/alias/trigger list column ORDER persistence | wx `SetColumnsOrder` is a stub on macOS. Widths/sort DO persist — those are testable. |
| Tree View for aliases/triggers/timers | Forced off on the port. |
| Spell-checker locale | Known gap. |
| "Confirm before replacing typing" | Deliberately not wired. |
| "All typing goes to command window" | Vacuously on; cannot be disabled. |
| Window tabs "Bottom" | AppKit draws tabs on top only. |
| Default notepad font setting | Retired upstream; notepads take the default *input* font. |
| `fade_output_buffer_*` | Known-dead on the port (needs a repaint timer; a feature, not a wiring fix). |
| `world.AddFont` | Documented product bug (registers nothing). |
| `world.ChatCall("hostname",…)` | Documented upstream bug (dotted-IP only). |
| VBScript / PHP as world languages | Unsupported; the GUI is EXPECTED to show a clear note — that note is itself a PASS condition (P7-08). |
| Minor combo/label placement variance on dense prefs pages | Known cosmetic consequence of dialog-unit templates + native metrics. Gross overlap IS reportable. |

Unreachable by this harness (record BLOCKED, don't attempt):

- Any Escape-key behaviour (rule 7).
- Numeric-keypad macros (rule 8).
- Chat-system peer checks (needs a second app instance; out of scope).
- Following hyperlinks (rule 11 — affordance check only).

---

## 5. Feature matrix

Work through phases in order — later phases assume earlier fixtures (TestWorld,
scripting prefix, server). After each check, update RESULTS.md before moving on.
General cadence: act → wait ~0.5 s → screenshot → judge. Batch predictable
sequences with `computer_batch`, always ending in a screenshot.

### P1 — App lifecycle & world files

- **P1-01 New World dialog.** File ▸ New World. Fill: name `TestWorld`, address
  `127.0.0.1`, port `4000`. Expect: dialog accepts values (zoom-verify).
- **P1-02 Scripting setup (prerequisite for the `/` oracle).** In the world's
  configuration (Game ▸ Configure ▸ Scripting, or the corresponding page of the
  same dialog if still open): enable scripting, language **Lua**, set **script
  prefix** to `/`, and set the script file to `$RUN/test.lua` (⌘⇧G, §3.6). OK
  the dialog.
- **P1-03 Auto-connect on open.** With fresh prefs `AutoConnectWorlds` is ON;
  after P1-01/02 the world should connect by itself: output shows
  `WELCOME-GUIDRIVE` **and** `SCRIPT-CONNECT-OK` (script file fired). If it did
  not auto-connect, use Connection ▸ Connect, record which happened.
- **P1-04 Oracle proof.** Type `/world.Note("ORACLE-OK")` + Return in the
  command bar. Expect `ORACLE-OK` in the output. **Everything downstream leans
  on this; if it fails, debug config (screenshot the Scripting page) before
  continuing.**
- **P1-05 Save World Details As.** File ▸ Save World Details As → run dir →
  `TestWorld.mcl` (§3.6). Verify from Bash the file exists and is non-trivial
  (`wc -c` > 2000).
- **P1-06 Quit prompts for unsaved changes.** Make a trivial change (Configure ▸
  Output, toggle something, OK), then app menu ▸ Quit. Expect a save prompt
  (the macOS quit path is hooked; this verifies it). **Click Cancel** — do not
  quit yet. Undo the toggled setting.
- **P1-07 Close & reopen world.** File ▸ Close World (answer any prompt with
  "don't save"), then File ▸ Open World → `TestWorld.mcl`. Expect reconnect +
  `WELCOME-GUIDRIVE` again.
- **P1-08 Window geometry persistence.** Resize the world window to a
  distinctive size (drag its corner). Quit via app menu ▸ Quit (save if
  prompted). Verify 0 processes (rule 10). Relaunch (§2.4), reopen
  `TestWorld.mcl` via File ▸ Open World (or the MRU list if shown — record
  which). Expect the window to come back at the resized geometry (compare
  screenshots). Note: the *child* is the visible window on this port.
- **P1-09 DNS failure message.** File ▸ New World: name `BadHost`, address
  `no.such.host.invalid`, port `4000`, OK. Expect a connection-failure message
  in its output that **names the real resolver reason** (not a bare error
  code). Screenshot, then close this world without saving.

### P2 — Connection & output display

All in TestWorld, connected. "Send `x`" means: click into the command bar,
type `x`, press Return.

- **P2-01 Echo round-trip.** Send `hello`. Output shows the command echo
  (`hello`) and `echo: hello`; `server.log` has `RECV 'hello'`.
- **P2-02 Disconnect/reconnect.** Connection ▸ Disconnect: output notes the
  disconnect. Connection ▸ Connect: banner again. server.log shows
  DISCONNECT/CONNECT pair.
- **P2-03 ANSI colours.** Send `ansi`. Zoom-screenshot the block: 8 FG colours
  visibly distinct, bright row distinct from normal, BG row filled, bold /
  underline / inverse / italic each rendered, X256 row shows red/green/blue.
- **P2-04 UTF-8 glyphs.** Send `utf8`. Expect `café naïve 世界 —dash— tail`
  rendered with no tofu/mojibake (zoom).
- **P2-05 Long-line wrap.** Send `long`. The 510-char line wraps across
  multiple rows with no horizontal truncation; the last chars (`…abcdefghij`)
  are present (zoom the final wrapped row).
- **P2-06 Wrap column honoured.** Game ▸ Configure ▸ Output (or the page
  carrying wrap column): set wrap column to 40. Send `long` again — rows now
  visibly narrower (~40 chars). Restore the original value after.
- **P2-07 Scrollback.** Send `stream 60`; after it finishes, scroll the output
  up (mouse wheel / scrollbar drag) to see `stream-line-000`, then back down to
  the latest line. Both screenshots.
- **P2-08 Freeze-on-selection.** Send `stream 40`; while lines are arriving,
  drag-select a word a few lines up. Expect: output stops scrolling while the
  selection exists (two screenshots ~2 s apart show the same bottom line), and
  after clicking elsewhere to clear the selection, the pane catches up with the
  buffered lines. This validates the 2026-08 freeze behaviour on macOS.
- **P2-09 Copy from output.** Select a whole visible line (drag), Edit ▸ Copy
  (must be enabled — see P2-10), then `read_clipboard`: the clipboard holds the
  selected text.
- **P2-10 Copy enablement.** With NO selection, open the Edit menu: Copy is
  greyed. With a selection: enabled. (This pins the macOS menu-id hijack fix.)
  Screenshot both menu states.
- **P2-11 Select All.** Edit ▸ Select All, then Copy, then `read_clipboard`:
  clipboard contains `WELCOME-GUIDRIVE` (from the top of the buffer).
- **P2-12 Find in output.** Edit ▸ Find: search for `stream-line-005`. Expect
  the view to scroll to/highlight it. Close the dialog via its button.
- **P2-13 Line-info tooltip.** Hover the mouse over an output line (mouse_move,
  wait 2–3 s, screenshot). Expect a tooltip with line information (time/line
  number). This pins the `line_information` fix.
- **P2-14 Hyperlink affordance.** Send `url`. Expect the URL rendered with link
  affordance (colour/underline). Do NOT click it (rule 11).
- **P2-15 Line spacing.** Configure ▸ Output: set line spacing to a larger
  value; OK. Screenshot: visibly larger vertical pitch (compare with earlier
  screenshot; fewer lines fit). Restore. This pins the `line_spacing` fix.
- **P2-16 Timestamps.** If the configuration offers per-line timestamps /
  line-preamble display (look on the Output/Appearance pages), enable it and
  verify timestamps render at line starts. If no such option is found in the
  GUI, record BLOCKED-CONFIG with a screenshot of the pages checked.

### P3 — Command bar

- **P3-01 History recall.** Send `alpha`, `beta`, `gamma`. Press Up: bar shows
  `gamma`; Up: `beta`; Down: `gamma`. (Zoom each.) Clear the bar.
- **P3-02 Typed text is not rewritten.** Type (do not send):
  `it's a "test" -- twice  done` (note the double space). Zoom: the bar shows
  exactly that — straight apostrophe, straight quotes, two hyphens, two spaces,
  no auto-period. Then send it; `server.log` must show the same bytes
  (`RECV 'it\'s a "test" -- twice  done'`). This pins the smart-quote /
  em-dash / double-space fixes AND the "macOS was rewriting commands as you
  typed" fix.
- **P3-03 Multi-line paste stays in the bar.** `write_clipboard` with
  `line-one\nline-two`, click into the bar, Edit ▸ Paste. Expect BOTH lines in
  the (grown) bar and **nothing new in server.log** — paste must not auto-send
  (a port-invented behaviour that was removed). Then press Return and confirm
  both lines transmit.
- **P3-04 Input bar auto-grows.** Type 3 lines (Shift-Return or paste); the bar
  is visibly taller than one line. Clear it; it shrinks back.
- **P3-05 wrap_input toggle.** Configure ▸ the page carrying "wrap input"
  (Commands/Input family): enable it; OK. Type 200 chars of `x` — the bar
  wraps and grows by display rows instead of scrolling horizontally. Disable
  it again — the same content stays on one (scrolling) line. Screenshot each
  state. (The toggle rebuilds the control in place; text should survive —
  verify the typed text is still present after each toggle.)
- **P3-06 Tab completion.** Send `echo zephyrword`. Type `zeph` and press Tab.
  Expect the bar to complete to `zephyrword`. If Tab instead moves focus,
  check the configuration for a tab-completion enable first; if none found,
  record the observed behaviour honestly.
- **P3-07 Command stacking.** Configure ▸ Commands: enable command stacking,
  character `;`. Send `echo one;echo two`. server.log shows two separate RECV
  lines (`echo one`, `echo two`). Disable after.
- **P3-08 Auto-say.** Configure ▸ the Auto-say page: enable, say-string `say `.
  Send `justwords` — server.log shows `RECV 'say justwords'`. Disable after.
- **P3-09 Speedwalk.** Configure ▸ Commands: enable speed walking, prefix `#`.
  Send `#2n3e` — server.log shows the expansion (n, n, e, e, e as separate
  commands). Disable after.
- **P3-10 Spell-check smoke.** If a "spell check before sending" style option
  exists, enable, send `zzzqqq`, record what happens (dialog/pass-through).
  BLOCKED-CONFIG if not found. Disable after.

### P4 — Aliases, triggers, timers (dialog-driven)

Use Game ▸ Configure ▸ Aliases / Triggers / Timers. Every add/edit goes
through the real dialogs (that's the point — headless tests already cover the
engine).

- **P4-01 Add alias via dialog.** New alias: match `aa`, send `echo alias-fired`.
  OK, OK. Send `aa`. The alias's send goes to the MUD: server.log shows
  `RECV 'echo alias-fired'` and the output shows `echo: echo alias-fired`.
- **P4-02 Edit alias.** Change its send to `echo alias-edited`; verify the new
  behaviour.
- **P4-03 Alias list state.** The list dialog shows the alias with its match
  and enabled tick. Screenshot.
- **P4-04 Menu alias + Ctrl+click word menu.** Add another alias: match
  `greet *`, send `echo greeting %1`, label `Greet this`, and tick its
  **menu** option. OK out. Ctrl+click (hold ctrl, left-click — if no menu
  appears, try plain right-click; macOS delivers Ctrl+click as right-click) on
  a word in the output pane. Expect a popup listing `Greet this`; click it;
  server.log shows `RECV 'echo greeting <word>'` where `<word>` is the word
  under the click. Screenshot the open menu.
- **P4-05 Command-bar Ctrl+click arm.** Type a word into the command bar
  (don't send); Ctrl+click ON the command bar over that word. Expect the same
  alias menu keyed to the bar's bottom row. (New 2026-08 feature.)
- **P4-06 Delete alias.** Delete `aa` via the dialog; send `aa`; server.log
  shows literal `RECV 'aa'` (no expansion).
- **P4-07 Add trigger via dialog.** New trigger: match `echo: TRIGGERME*`,
  send `echo trigger-fired`, and set a distinctive text colour for the matched
  line. Send `TRIGGERME now` → output line `echo: TRIGGERME now` appears in
  the trigger colour (zoom), and server.log shows `RECV 'echo trigger-fired'`.
- **P4-08 Disable trigger.** Untick it; send `TRIGGERME again`; no colour, no
  fired command. Re-enable, delete it.
- **P4-09 Add timer via dialog.** New timer: every 5 seconds, send
  `echo timer-tick`. Within ~12 s server.log gains at least two
  `RECV 'echo timer-tick'` lines. Delete the timer; confirm ticks stop
  (watch 10 s).
- **P4-10 at_time timer persists.** New timer: "at time" a few minutes ahead;
  OK; reopen its edit dialog — the hour AND minute fields still show what you
  entered. (Pins the upstream HOUR→MINUTE copy bug fix.) Delete it.

### P5 — World configuration sweep

The world-prefs dialog is the biggest surface. Census-first:

- **P5-01 Page census.** Open Game ▸ Configure. Screenshot the page list;
  transcribe every page name into MENUS.md. For EVERY page: select it,
  screenshot, judge "renders: controls laid out, no gross overlap, no empty
  panel" (minor placement variance is known — §4). One RESULTS row per page:
  `P5-01-<pagename>`.
- **P5-02 Persistence spot-checks.** On each of ~6 representative pages, change
  one setting, OK out, save the world (File ▸ Save World Details), reopen the
  dialog and verify the value stuck. Suggested: output font size, a colour
  swatch on the colours page, wrap column, an input-style option, a logging
  option, note colour.
- **P5-03 Output font live effect.** Configure ▸ Output font: change to a
  clearly different size (e.g. 18). OK. The output pane text is visibly larger
  WITHOUT restarting. Restore.
- **P5-04 Note colour live effect.** Change the note colour; `/world.Note("P5-04")`
  shows the new colour (zoom). Restore.
- **P5-05 Input font/colour live effect.** Change input font size and input
  text colour; the command bar changes accordingly. Restore.
- **P5-06 ANSI colour remap.** On the ANSI colours page, remap one ANSI slot to
  a wild colour; send `ansi`; the corresponding swatch shows the remap.
  Restore.
- **P5-07 Global preferences census.** Find the global prefs (app menu ▸
  Preferences…, or File ▸ Global Preferences — record where). Same treatment
  as P5-01: every page screenshot + render judgement; then two persistence
  spot-checks (e.g. default output font, a confirmation checkbox).
- **P5-08 Default output font reaches an open world.** In global prefs set the
  default output font size up; verify the open world's output updates (the
  ApplyDefaultFonts path). Restore.

### P6 — Notepad

Open a notepad (Edit ▸ Notepad or File equivalent — record which item). The
notepad has its own menubar (File / Edit / View / Search / Convert / Window /
Help).

- **P6-01 Typing + glyphs.** Click into the notepad body; type:
  `needle one\nplain two\nneedle three\n`. Zoom: crisp glyphs, correct text.
  (This closes the 2026-07 "notepad glyph rendering not eyeballed" debt —
  note that in RESULTS.)
- **P6-02 Find.** Search ▸ Find (⌘F): find `needle`. Expect selection/caret on
  the first `needle`. Find Next (⌘G): moves to line 3's `needle`. Close the
  find dialog via its button.
- **P6-03 Replace.** Search ▸ Replace: replace all `needle` → `thread`. Body
  now shows `thread one` / `thread three`.
- **P6-04 Go To Line.** Search ▸ Go To Line (⌘L): line 2. Caret lands on
  `plain two` (type `<-HERE` and zoom to prove caret position).
- **P6-05 Word count.** The word-count item reports plausible counts for the
  current body (record the numbers).
- **P6-06 Insert Date/Time.** ⌘D inserts a timestamp at the caret (zoom).
- **P6-07 Send to World.** Select one line; Send To World (⇧⌘S); server.log
  receives it.
- **P6-08 Save As.** File ▸ Save As → run dir → `notepad-test.txt` (§3.6).
  From Bash: file exists and contains `thread one`.
- **P6-09 Convert menu probe.** Try one Convert item (e.g. upper case) on a
  selection; record effect.
- **P6-10 Notepad font follows default input font.** (Known: the retired
  notepad-font pref is gone.) In global prefs change the default INPUT font
  size; open a NEW notepad; its text uses the new size. Restore.

### P7 — Scripting & plugins

- **P7-01 Immediate dialog.** Game ▸ Immediate: enter
  `world.Note("IMM-OK")`, run it; `IMM-OK` in output. Close via its button.
- **P7-02 Script reload.** Append to `$RUN/test.lua` from Bash:
  ```lua
  function note2 () world.Note ("SCRIPT-RELOAD-OK") end
  ```
  Game ▸ Reload Script File. A confirm dialog may appear (the reload option
  defaults to confirm) — answer Yes. Then `/note2()` → `SCRIPT-RELOAD-OK`.
- **P7-03 Trace.** Game ▸ Trace on; send `hello`; TRACE lines appear in the
  output; Trace off; they stop.
- **P7-04 Plugin install + miniwindow paints.** File ▸ Plugins ▸ Add →
  `$RUN/testplugin.xml`. Expect: plugin listed in the dialog, and — decisive —
  the red-on-blue "MW-OK" miniwindow visible in the top-right of the output
  pane (zoom-screenshot). **This is the only real-paint miniwindow evidence
  in the whole project; capture it well.**
- **P7-05 Plugin alias.** Send `pling` → output `PLUGIN-ALIAS-OK`, server.log
  `RECV 'plugin-sent'`.
- **P7-06 Plugin trigger.** Send `TRIGPLUG go` → output `PLUGIN-TRIGGER-OK`,
  server.log `RECV 'plugin-triggered'`.
- **P7-07 Plugin remove.** Remove the plugin via the dialog; the miniwindow
  disappears (screenshot); `pling` no longer expands.
- **P7-08 Unsupported-language note.** Configure ▸ Scripting: switch language
  to VBScript; expect the clear "unsupported" note (per §4); switch back to
  Lua (re-verify `/world.Note("P7-08-OK")` still works afterwards).
- **P7-09 Sound.** `/world.Note(world.Sound("<RUN>/beep.wav"))` (absolute
  path). Expect `0` (eOK) noted. (Audibility can't be asserted; the return
  code can.)
- **P7-10 Accelerator.** `/world.Accelerator("F6", "echo accel-fired")`, then
  press F6 with the world focused. server.log shows `RECV 'echo accel-fired'`.
  Then `/world.Accelerator("F6", "")` to unbind; press F6; nothing new.
- **P7-11 SetStatus.** `/world.SetStatus("STATUS-P7-11")` — the status bar
  shows it (zoom).
- **P7-12 (Optional, if time) Second engine smoke.** Set world language to
  Python (if this build embeds it): re-run P1-04's Note oracle in Python
  syntax (`/world.Note("PY-OK")`). Exit-77-style absence → SKIP. Restore Lua.

### P8 — Windows, printing, logging

- **P8-01 Second world.** Save a copy: `cp TestWorld.mcl TestWorld2.mcl` (Bash,
  in the run dir), then File ▸ Open World → `TestWorld2.mcl`. Two worlds open;
  Window menu lists both; switching activates each (title changes).
- **P8-02 Activity window.** Open the activity list (View or Window menu —
  record which). It lists both worlds; send `echo act` in one; its new-lines
  count changes. Close the second world after (don't save).
- **P8-03 Logging.** Start logging via the File menu's log item (or Configure ▸
  Logging) to `$RUN/session.log`; send `echo logged-line`; stop logging. From
  Bash: `grep logged-line "$RUN/session.log"` succeeds.
- **P8-04 Print World to PDF.** File ▸ Print World → in the macOS print
  dialog use the PDF dropdown ▸ Save as PDF → run dir → `print-world.pdf`
  (§3.6). From Bash: file exists, size > 5 KB. (Content check is manual; note
  that.)
- **P8-05 Print Selection.** Select a few output lines; the macOS-specific
  Print Selection item must be enabled; print to `print-selection.pdf` the
  same way; expect a smaller file than P8-04. With NO selection, the item is
  greyed (screenshot).
- **P8-06 Toolbar/status toggles.** View menu: toggle each bar off and on;
  screenshots show them disappear/reappear.
- **P8-07 Paste To World.** `write_clipboard` with two short lines; Edit ▸
  Paste To World (if present; record). Expect both lines in server.log and
  possibly a brief progress dialog. 
- **P8-08 Send File.** If the File/Edit menu has a send-file item: send a
  3-line text file from the run dir; server.log receives all three lines.

### P9 — Menu census & residue probes

The closing sweep that makes coverage systematic rather than anecdotal.

- **P9-01 Census.** For EACH menubar state — (a) no world open (close all
  worlds first, save prompts answered "don't save"), (b) world open, (c)
  notepad focused, (d) activity window focused — open every top-level menu and
  every submenu, screenshot each, and transcribe all items with their
  enabled/disabled state into MENUS.md. Expected top-levels with a world open:
  File, Edit, View, Connection, Input, Display, Game (with a Configure
  submenu), Window, Help.
- **P9-02 Reconciliation.** Mark every menu item in MENUS.md as: covered by a
  check above (note the ID), or residue.
- **P9-03 Residue probes.** For each residue item: click it once, observe,
  record one line (what happened / dialog title / no-op), close any dialog via
  its buttons. Rules: skip Quit/Close-type items until the very end; skip
  anything that would leave the machine (web links, "check for updates",
  email); never probe with unsaved real data at risk. A residue item that
  visibly does NOTHING when clicked is a finding (record FAIL) — "silent
  no-op" is the classic dead-reader symptom this port has a history of.
- **P9-04 Disabled-state spot checks.** Verify a few contextual enablements
  beyond P2-10: Connection ▸ Connect greyed while connected, Disconnect greyed
  while disconnected; Print Selection greyed without selection (from P8-05);
  paste-family items greyed with an empty clipboard (clear via
  `write_clipboard` with empty string if supported, else skip).

---

## 6. Scale expectations

Roughly 95 checks. P1–P4 are the critical path (~40 checks) — if time is
constrained, complete phases in order and stop cleanly; a partial ledger of
verified rows beats a complete ledger of guesses. Expect several hours of
wall-clock; screenshots dominate. The phases after P1 are mostly independent,
so a resumed session (§8) can pick up at any phase boundary.

## 7. Failure protocol

When a check fails:

1. **Verify the instrument first** (the 2026-08-11 lesson: a "dead" Escape key
   read as an app bug and cost an hour). Re-prove the failing input class
   against a framework-guaranteed behaviour: a menu click against Help ▸
   About; typing against a dialog text field; a key chord against ⌘C-then-
   read_clipboard. Only if the instrument passes does the app get the blame.
   Record what you re-proved in the BUGS entry.
2. **Retry once, standalone**: quit the app (rule 1), relaunch (§2.4), reopen
   TestWorld, repeat only the failing check. Accumulated state is a known
   false-positive source.
3. **Crash**: if `pgrep` shows the app died, collect the newest
   `~/Library/Logs/DiagnosticReports/MUSHclient*.ips` into the run dir,
   relaunch, continue. Severity: critical.
4. **Hang** (no repaint, menus unresponsive > 30 s): screenshot, then
   `sample MUSHclient 5 -file "$RUN/hang-<checkid>.txt"`, then pkill,
   relaunch, mark FAIL with the sample attached.
5. **Never fix.** Even an obvious one-liner. Record and move on.
6. A check that cannot be attempted for config/instrument reasons is BLOCKED
   with the reason — never silently skipped.

## 8. Teardown (mandatory, even on abort)

```bash
# 1. Quit the app properly (app menu ▸ Quit, answer prompts), then enforce:
pkill -9 -f MUSHclient.app/Contents/MacOS/MUSHclient
# 2. Stop the server:
pkill -f "python3 server.py"
# 3. Restore the user's real preferences (§2.2 — NOT `defaults`, plain files):
rm -f "$HOME/Library/Preferences/mushclient Preferences"
mv "$HOME/Library/Preferences/mushclient Preferences.SESSIONBACKUP" "$HOME/Library/Preferences/mushclient Preferences"
rm -f "$HOME/Library/Preferences/MUSHclient/mushclient_prefs.sqlite"
mv "$HOME/Library/Preferences/MUSHclient/mushclient_prefs.sqlite.SESSIONBACKUP" "$HOME/Library/Preferences/MUSHclient/mushclient_prefs.sqlite"
# 4. Integrity checks — paste results into RESULTS.md:
shasum "/Users/robot/Library/Application Support/MUSHclient/worlds/Discworld.mcl"
#    → must be 299af2670fb8cc51ea0837993a7225d5a008c630
cd /Users/robot/Documents/mushclient && git status --short   # must match setup
```

Finish RESULTS.md with a summary header: counts of PASS/FAIL/BLOCKED/SKIP, the
list of BUG IDs by severity, and the three most important findings in plain
sentences.

## 9. Resuming a dead run

A fresh agent resuming this plan: read `RESULTS.md` in the newest
`~/mushclient-gui-drive/run-*` directory. Re-run §2.3 (server), §2.4 (launch),
CAL-1..CAL-3 only, reopen `TestWorld.mcl`, re-prove the oracle (P1-04), then
continue from the first row that is not PASS/SKIP. Never re-run the
`defaults delete` if a backup already exists in the run dir — the backup from
the FIRST session is the user's real state and must be the one restored at
teardown.
