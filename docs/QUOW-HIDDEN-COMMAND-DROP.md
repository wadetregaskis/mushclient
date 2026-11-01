# A command-dropping handler in Quow's Cow Bar

This note is about `QuowMinimap.xml` ("CowBar"), version 4.96, dated 2026-05-17, as
downloaded from <https://quow.co.uk/MUSHclient.zip> on 2026-10-31. Line numbers are for
that file. It is written to be forwarded to Quow, whose plugins are the main reason many
people use this client, and who is best placed to say what this code is for.

Sections 1 to 3 and 5 are facts that anyone can check against the file. Section 6 is
inference and is labelled as such.

## 1. What the code does

The plugin installs an `OnPluginSend` handler. MUSHclient calls that for every command on
its way to the MUD, and a command is not sent if the handler returns false. This handler
drops a command when all of these hold:

- The command did not come from the user. `GetInfo(239)` (the reason the current script is
  running) is above 5: a trigger, timer, input from the server, a world event, the Lua
  sandbox, or a miniwindow hotspot. Typing, macros, the keypad, accelerators and the alias
  menu are 1 to 5, and are never dropped.
- At least 38 minutes have passed since the plugin loaded.
- At least 59 seconds have passed since the last command it dropped.
- A random draw weighted by the date passes. Since 29 October 2020 at the latest it always
  does.

So at any time now, once a session is 38 minutes old, the first command sent by a trigger,
timer or similar in each minute or so is discarded. Nothing is shown to the user, nothing
is logged, and `OnPluginSent` isn't called. The MUD simply never receives it.

### The code

Declared far from where they are used (lines 4278 to 4280):

```lua
local GI, IAC, PPM, SB, SE, FCR, ANV, DO, ACR, TTP = GetInfo, 0xFF, 0x8AE, 0xFA, 0xF0, 0xEF, false, 0xFD, 0x3E9, 'T25Q'
local MR, LC, GMCP, ATM = math.random, (OST() + PPM), 201, AddTimer
local TTA = 'A3aCcHVnaW5TZW5c'
```

The handler (lines 14341 to 14361):

```lua
-- Fix GMCP lag from outgoing command interrupts
if (TSA == true) then
  TTA = TTP
  TTP = TTP .. 'bHVnaW5TZW5k'
else
  TTP = string.sub(TTP, 1, 4) .. 'bHVnaW5TZW5k'
end
_G[BDC(TTP)] = function(CM)
  local LM = GI(FCR)
  -- Clear 60s TTL overflow buffer
  if (LM > 5) then
    TACR = TACR + 1
    if (MR(1, ACR) < TCBL and LC < (OST() - 0x3A)) then
      LC = OST()
      return ANV
    end
  else
    ACR = 0x3E8 - TCBL
    if (ACR < 0x0002) then ACR = 0x0002 end
  end
end
```

The date (line 23573 to 23577, in `OnPluginInstall`):

```lua
if ((OST() - 0x5B3C38B9) < 0) then
  TCBL = 0x1
else
  TCBL = MFL((OST() - 0x5D05FC2C) / 0xA8C0)
end
```

### Decoding it

| In the code | Means |
| --- | --- |
| `BDC`, `BEC`, `OST`, `MFL` (lines 1047 to 1049, 1237) | `utils.base64decode`, `utils.base64encode`, `os.time`, `math.floor` |
| `GI`, `MR`, `ATM` | `GetInfo`, `math.random`, `AddTimer` |
| `'T25Q' .. 'bHVnaW5TZW5k'` | base64 of `OnPluginSend`. The name appears nowhere in the live code in plain text; lines 13908 to 13911 mention it only in a comment saying it is unused |
| `TSA` (lines 610 to 613) | Read from the plugin variable `TSA`, but both branches build the same name, so it changes nothing. It isn't an off switch |
| `TTA` | Assigned and never read. `'A3aCcHVnaW5TZW5c'` decodes to three stray bytes and `puginSen\` |
| `FCR` = 0xEF = 239 | `GetInfo(239)`, the action source |
| `ANV` = false | The value that stops the command |
| `PPM` = 0x8AE = 2222 | Seconds. `LC` starts at load time + 2222, and `LC < now - 58` first holds 2281 seconds after load: 38 minutes |
| `0x3A` = 58 | Seconds between drops, so at least 59 in practice |
| `0x5B3C38B9` = 1530673337 | 4 July 2018. A clock set before this gives `TCBL` = 1, which never passes |
| `0x5D05FC2C` = 1560673324 | 16 June 2019, 08:22 UTC. `TCBL` is negative before this, so it never passes |
| `0xA8C0` = 43200 | Twelve hours. `TCBL` rises by 2 a day, and was 5389 on 31 October 2026 |
| `ACR` | Starts at 1001, and each user command sets it to `1000 - TCBL`, at least 2. The draw is `math.random(1, ACR) < TCBL`, so its odds rose from nothing in June 2019, to a certainty after a typed command from about February 2020 (`TCBL` above 500), and to a certainty in every case from 29 October 2020 (`TCBL` of 1002) |
| `TACR` | A count of every non-user command the handler has seen. It is saved between sessions (line 149, `core_options` in `_quowmap_userdata.db`, and read back at lines 1287 to 1290) |

### The variables are also required elsewhere

Other code only runs if these variables exist:

| Line | Code | What depends on it |
| --- | --- | --- |
| 4311 | `if (ACR and TACR and BDC) then` | Sending `Core.Supports.Set`, without which the MUD sends no room or vitals GMCP. `SMC = BEC(sSupports)` and `BDC(SMC)` around it decode to what they started with, so have no other effect |
| 5539 | `if (not ACR) then return end` | The whole `minimap` command. The comment says it is about the connection |
| 13567 | `if (TACR and sQuowMapfiles[iUseMap][8] == false) then` | Loading map images |
| 23505 | `TCBL = TCBL + 0xFF` | `OnPluginInstall`, which raises a Lua error at this line if `TCBL` is missing |
| 23506, 23544 | `if (bShowCommsTabs == true and TCBL) then` | The comms tabs |

Removing the whole mechanism therefore stops GMCP, the map and the `minimap` command. That is
why our patch (section 4) leaves every variable in place.

### The comments

These describe something other than what the code does:

| Line | Comment | The code |
| --- | --- | --- |
| 14341 | `Fix GMCP lag from outgoing command interrupts` | Installs the handler. GMCP is not sent through `OnPluginSend`, so dropping a command can't affect GMCP's timing |
| 14350 | `Clear 60s TTL overflow buffer` | Decides whether to drop a command. No buffer exists |
| 1257 | `GMCP-TTL` | `TCBL`, the date value |
| 1286 | `GMDbg` | `TACR`, the count |
| 14208 | `ACR = core gmcp enable` | On its own line among room handling, near nothing that uses `ACR` |
| 23504 | `Try smoother handling of the comms-tabs` | `TCBL = TCBL + 0xFF`. The value is overwritten at line 23576 |
| 6785 | `Paste this output to Quow to help with some types of error, version numbers, GMCP buffer-counts etc.` | `minimap debug` prints `{MM DEBUG\|TACR\|TCBL\|client version\|plugin version}`. `TACR` is the count of non-user commands |

## 2. What it affects

Any command that reaches the client's send path from a non-user source, from any plugin or
from the world's own triggers and timers, not only this plugin's. Reading the code, that
includes the Cow Bar's own sends from miniwindow hotspots. For example, `QuowRouteToRoomID`
sends `alias MooMooQuowsRun ...` and then `MooMooQuowsRun` (lines 4224 and 4247). From a map
menu those are hotspot callbacks. If the first is the one dropped, the second runs whichever
route that alias held before, so the character walks an old route. We have not seen this
happen; it follows from the code.

GMCP isn't affected, because `SendPkt` doesn't go through `OnPluginSend`.

## 3. How to see it for yourself

Without waiting 38 minutes:

1. Copy `QuowMinimap.xml` and, at line 4278, change `0xFF, 0x8AE,` to `0xFF, 0x1,`. The first
   drop now comes about a minute after loading rather than after 38.
2. Add a plugin with a timer that sends a numbered command to the MUD every 5 seconds, and
   connect to anything that logs what it receives.
3. Numbers go missing from the log, about one a minute, while the client's output window
   shows nothing unusual. `minimap debug` shows the count rising.

Unmodified, the same thing happens after 38 minutes.

What we saw doing this in MUSHclient 6.00 against a loopback server, with a timer sending
`tick N` every 5 seconds, is in section 7.

## 4. What this client does about it

We don't want to ship this behaviour, and hope the note is taken as it's meant: we would much
rather simply use Quow's plugins as published.

**The Discworld edition.** `packaging/fetch-discworld-plugins.sh` changes one line of the
staged copy, and nothing else in any of Quow's files:

```diff
-      return ANV
+      return true -- MUSHclient packaging: was "return ANV", see docs/QUOW-HIDDEN-COMMAND-DROP.md
```

The handler still runs, still counts and still sets its variables, so GMCP, maps and the
`minimap` command work as before. If the handler isn't the exact text above, or there's
anything else like it, the packaging stops so that the new version gets read.

**Other copies.** People install the plugin themselves, and our importer copies an existing
MUSHclient 5 setup. So the client ignores an `OnPluginSend` refusal from plugin ID
`bfe35205f026786ea1d56e3b` at version 4.96 or below, and says once per session that it has
done so. A later version is presumed fixed, and is treated like any other plugin. The plugin
has no other use for `OnPluginSend`: the name appears in the file only in that comment.

**All plugins.** Stopping a command is a legitimate thing for a plugin to do, but it shouldn't
be invisible. The client now writes a Trace message whenever a plugin stops a command
through `OnPluginSend`, `OnPluginCommand` or `OnPluginCommandEntered`, and counts them per
plugin: `GetPluginInfo(id, 26)` for commands stopped, `GetPluginInfo(id, 27)` for refusals
ignored, and both in `Debug "plugins"`.

## 5. Everything else the review found

This was a static review of all seven plugins in the package, plus the world file and the map
database. It read closely the areas listed here, and searched the rest: `QuowMinimap.xml`
has 285 functions, and they were not all read line by line. So this is good evidence that a
thing is absent, not proof. Every route to the date (`os.time`, `os.date`, `OST`), to randomness (`math.random`, `MR`), to
indirect code (`_G[`, `load*`, `dofile`, `require`, `setfenv`, `string.dump`) and to the MUD
(`Send*`, `Execute`, `Simulate`, `DoAfter*`, `SendPkt`) was listed and looked at.

**Nothing further that degrades behaviour on purpose was found, in any of the plugins.**

Other things worth knowing, none of which we have changed:

| Where | What | Our view |
| --- | --- | --- |
| Lines 23601, 23673 to 23677 | A one-shot timer `timcache`, at a random delay of up to 14 hours, calls `OnPluginConnected`, which does `_G[BDC("c1Jvb21EZXNjcmlwdEhhc2g=")] = {}` if that global exists. The name decodes to `sRoomDescriptHash`. Nothing else in the plugin uses it, `OnPluginConnected` isn't a MUSHclient callback, and `room_descriptions` in the map database is empty | Obfuscated, and has no effect in this version. Probably left over from something else |
| `user_data` table in `_quowmap_database.db`, lines 707 to 714, 6131, 6155 | Base64 data decoded by `minimap cow` and `minimap quote`. We decoded all 14 rows: 13 ASCII cows, and 102 quotes from the Discworld books | Harmless fun |
| Line 26505, and line 921 of `QuowMissionHelper.xml` | Runs `_profiler_minimap.lua` / `_profiler_missions.lua` from the plugin's folder if there is one, and says so | A debugging aid. Not shipped |
| Lines 306, 320, 334 | Turns triggers, aliases and timers back on if they are off | Says so, and explains why |
| Line 373 | Disables the old MapDoorText plugin | Says so |
| Line 3484 | Sets `play_sounds_in_background` on every load, without saying so | Minor, and fits the sound features |
| Lines 6524, 6530 to 6580 | `minimap discworldconfig` and `minimap mushconfig` change many settings, on the MUD and in the client | Only when asked, and they say so |
| Lines 16160 to 16196 | Another group member's `group say CowBarHotspotSync:...` can reset your hotspot times. A malformed time raises a Lua error rather than being ignored | A documented feature with a small bug. It carries numbers only, and is limited to group members |
| Line 625 | `os.exit()` when the map database is missing, deliberately, to stop the plugin with an error | MUSHclient's `os.exit` only raises that error. As its comment says |

**What goes to the MUD.** We read every `Send`, `SendNoEcho` and `SendPkt` in all seven
plugins. Each `Send` is the direct result of a command typed, a menu choice, a click,
or an editing mode only Quow would use. The GMCP sent is `Core.Hello` (the client's name and
version), `Core.Supports.Set`, and `core.request char.skills` when `mindspace skills` asks for
it. Nothing sends information about the user to the MUD, or to anywhere else: no sockets, HTTP
or files outside the plugin's own data.

**`{MM DEBUG`.** Printed only by `minimap debug` (or `mm debug`) typed by the user, and only to
their own window. Nothing that arrives from the MUD can cause it, and it isn't in the plugin's
help. It reaches anyone only if the user pastes it, which the comment on line 6785 invites.

**`TACR`.** Written only by the handler, read only by the debug line and the checks above, and
saved only in the user's own database.

**Character names.** Nothing is keyed to any particular character. The names the plugins
use come from GMCP or from the user's own settings: combat stat sets, alt-specific data, and
recognising group members. (`sAcronymToSpell["quow"]` in `QuowMindspace.xml` is a spell.)

**The other six plugins.** `QuowEFFStats`, `QuowKlein`, `QuowMindspace`, `QuowMissionHelper`,
`QuowQuota` and `QuowTriggers` contain no base64, no `_G[`, no `OnPluginSend` and no
date-derived behaviour beyond their own stated mission and quota timers. `QuowEFFStats` and
`QuowMindspace` send `skills` commands when you run their own commands, and `QuowMissionHelper`
and `QuowMindspace` have aliases that pass your typed line on with `Send(sLine)`.

**The world file.** `Discworld.mcl` holds settings and includes the plugins. It contains no
triggers, aliases or scripts of its own.

## 6. What it might be for (inference)

The rest of this note is our reading, not fact. We have not asked Quow, and he may well know of
reasons we don't.

- It looks deliberate rather than accidental: the name is hidden, the variables are declared
  thousands of lines away and required elsewhere, and the comments describe something else.
  It is hard to see how that comes about by chance.
- Its target is automation. It leaves alone everything a person does at the keyboard, and
  affects what triggers and timers do. The most natural reading is a discouragement of
  scripted or unattended play, which is a sensible thing for a MUD community to care about.
  We can't tell whether that was the purpose.
- The delay, the rate and the slow rise from June 2019 would make it hard to notice and to
  connect with the plugin: a lost command looks like the kind of thing that happens on a MUD.
- The count in the debug line might be a way to see how much automation someone is running
  when they ask for help. That is only a guess from what it counts.

Whatever the intent, its cost also falls on people with legitimate triggers, such as a timer
that keeps a connection open or a trigger for a screen-reader user. They can't tell why a
command they relied on didn't happen.

If the aim is to discourage automation, we would be glad to talk about ways of doing that in
the open. Removing the handler, and the checks on its variables, would let us remove both
the patch and the guard.

## 7. What we observed

In MUSHclient 6.00 on Windows, connected to a loopback server that offers GMCP and reports one
room, with the six plugins that the package's world file loads, plus `QuowQuota` in the 4.97
and patched, short runs. A separate test plugin's timer sent `tick N` to the
server every 5 seconds, and we compared what it sent with what the server logged. To avoid
waiting 38 minutes, the runs marked "short" had `0x8AE` changed to `0x1`, as in section 3.

| Copy of the plugin | Client's treatment | Ticks the server got | Shown to the user |
| --- | --- | --- | --- |
| Upstream text, short, version raised to 4.97 so that the guard doesn't apply. This is how the plugin behaves in any other client | Refusals honoured | 1 to 27, **without 12 and 24**, which were 60 seconds apart | Nothing at all |
| Upstream text, short, version 4.96 | Refusals of 12 and 24 ignored | 1 to 24, none missing | The note, once |
| Patched, short | No refusals to ignore | 1 to 22, none missing, past the point where 12 was dropped above | Nothing |
| Patched, not short | No refusals to ignore | 1 to 7, none missing, in a run of under a minute | Nothing |

In each run the client negotiated GMCP and sent `Core.Hello` and `Core.Supports.Set`, the map
window showed Ankh-Morpork from the `room.info` the server sent, and commands typed into the
client reached the server. `minimap debug` printed `{MM DEBUG|18|5389|6.00|4.96}` after 18 ticks
of the patched, short run, so the count carries on as before.

Not observed: the real 38 minutes, the map-menu example in section 2, or any of this on the
real Discworld.
