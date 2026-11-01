# Accessibility

With a screen reader running, MUSHclient reads new output to it, with no setup and no plugin.
Without one, nothing is different: nothing is spoken, nothing is shown, no key changes.

The code is `src/a11y.cpp` (the document's half), `src/speech_router.cpp` (what to say and when),
`src/a11y_shims.cpp` (plugins the client now does itself), `src/pal/ispeech.h` (the seam),
`src/pal/win_speech.cpp`, `src/platform/wx_osx_speech.mm` and `src/platform/linux_speech.cpp` (the
backends), and `src/pal/win_a11y_pane.cpp` and `src/platform/wx_a11y_pane.cpp` (the text pane).

## What speaks

| | Screen reader | Pacing | The system voice |
|---|---|---|---|
| Windows | NVDA directly, JAWS directly, UI Automation notifications for Narrator and anything else that sets the screen reader flag | Exact on NVDA 2026.3 and later, otherwise estimated | SAPI, paced exactly |
| macOS | VoiceOver announcements | Estimated, and VoiceOver can skip under load | AVSpeechSynthesizer, paced exactly |
| Linux | Orca through ATK notifications (ATK 2.46 or later) | Estimated | speech-dispatcher, loaded with dlopen, estimated |

Global Preferences has an Accessibility page. *Speak output* is Never, When a screen reader is
running (the default), or Always, which uses the system voice if there is no reader.

A script that asks to be heard, with `world.Speak` or Send To: Speech, is heard with no reader as
well, through the system voice, unless *Speak output* is Never. The player asked for it by
installing that script.

## The keys

They work in the command box and the text pane while speech is in use, and are otherwise whatever
they always were. All but the first are Alt+Shift, which nothing else in the client uses.

| Key | Does |
|---|---|
| Ctrl+Shift+F12 | Mute this world, or unmute it. Also Alt+Shift+M. |
| Up, Down | Previous and next line. This moves the review cursor, and new lines wait while it is away. |
| Left, Right | Previous and next word |
| Comma, Period | Previous and next character |
| S | Spell the word |
| W, L | Say the word, say the line |
| Home, End | Oldest line; newest line, which also ends reviewing and lets what waited be heard |
| 1 to 9, 0 | The newest line, up to the tenth newest |
| Page Up, Page Down | Previous and next category: review reads only those lines. A category is a history of its own, such as tells. |
| K | Skip to the latest: drop what is waiting and say only the newest line |
| H, J | Next and previous miniwindow hotspot |
| Return | Activate that hotspot |
| O | Between the command box and the text pane. F6 does this too, unless the world has given F6 a macro. |

Pressing Enter in the command box silences speech, drops what is waiting and ends reviewing, since
it is no longer news (`speech_interrupt_on_enter`).

A plugin can put any of these on other keys: `world.SpeechReview (action)`, where action is one of
`previous_line next_line first_line last_line current_line previous_word next_word current_word
spell_word previous_char next_char skip_to_latest previous_category next_category next_hotspot
previous_hotspot activate_hotspot toggle_mute toggle_pane recent_1` ... `recent_10`.

## The text pane

A real read-only text control holding what the output window shows, so that the reader's own caret,
review and braille work on it as on any other edit box. It exists only while output is being read,
or if *Text pane for review* says Always.

On Windows it is a RichEdit the same size as the output window and behind it. Nothing on the
screen moves, and miniwindows stay visible to anyone who can see them. On macOS and Linux it is
a short strip under the output window.

F6 goes there, at the newest line. Tab, Enter or Escape go back. Typing goes to the command box. New
lines arrive without moving the caret, and without the reader saying so. While it has focus, new
lines wait to be spoken, and on leaving you hear how many there were. It keeps the last 5000 lines.

## Flooding

Lines that arrive together are said together. If more than `speech_flood_lines` (40) are waiting,
the middle goes, and you hear "N lines skipped" and the newest five. What has a category with
priority 2, as triggers, aliases and timers sending to speech do by default, is never dropped.

## For a script

    world.Speak (text [, category [, priority [, interrupt]]])
    world.StopSpeech ()
    world.SpeechCategory (name, option, value)
    world.SpeechInfo (type)
    world.SpeechReview (action)
    world.AccessibleHotspots ()           -- window<tab>id<tab>label, a line each
    world.ActivateHotspot (window, id)
    world.WindowHotspotLabel (window, id, label)

Priority is 0 polite, 1 normal, 2 assertive, or -1 for whatever the category says.

`SpeechCategory` options are `enabled`, `priority`, `cue` (a sound file played as a line arrives),
`cue_volume` (0 to -100) and `cue_pan` (-100 to 100). The client's own categories are `output`,
`note`, `input` (off, since you just typed it), `prompt`, `trigger`, `alias`, `timer`, `script` and
`system`. Any other name makes a new one.

`SpeechInfo`: 1 the screen reader, 2 what is speaking, 3 is output being read, 4 lines waiting, 5
busy, 6 this world muted, 7 the backend's capabilities, 8 the mode, 9 how many things have been
said, 10 the last, 11 lines skipped, 12 the system voices separated by `|`, 13 is there a text pane,
14 reviewing.

Send To: Speech is value 15, `sendto.speech`, on triggers, aliases and timers.

An older client refuses a world file with a trigger of 15, as it would any send-to it does not know.

## World options

All go with the world file, and are set with `SetOption`.

| Option | Default | Means |
|---|---|---|
| `speech_mute` | 0 | This world says nothing. |
| `speech_background` | 0 | Read this world's lines when it is not the active one. What a script asks for is always said. |
| `speech_read_all` | 0 | Read output even with no screen reader. |
| `speech_braille` | 1 | What is said also goes to a braille display. NVDA only, since it is the only one with a way there; elsewhere the pane is how. |
| `speech_interrupt_on_enter` | 1 | |
| `speech_hold_while_reviewing` | 1 | |
| `speech_flood_lines` | 40 | 0 is never. |
| `speech_categories` | | Set by `SpeechCategory`. |
| `mtts_screen_reader` | 0 | Tell servers a screen reader is in use. It discloses a disability, so finding a reader never turns this on. See PROTOCOLS.md. |

## Existing plugins

**MushReader** (`925cdd0331023d9f0b8f05a7`) and **LuaAudio** (`aedf0cb0be5bf045860d54b7`) are replaced
by ID: a plugin file with one of those IDs loads as a small Lua plugin with the same functions instead,
and the file is left alone. Their DLLs are 32-bit. `CallPlugin`, `PluginSupports` and `ppi.Load`
all answer as they did, so soundpacks work. Loading MushReader sets `speech_read_all`, since the
original read everything. Sounds in Ogg play only if a file of the same name in WAV, MP3 or FLAC is
beside them: the client has no Ogg decoder.

The commands `tts`, `tts_note`, `tts_interrupt`, `tts_stop` and `subst` work while speech is in use
or that plugin is loaded. To anyone else they are words to send to the MUD. `subst` keeps
MushReader's `substitutions.mush`: a whole line that matches is said differently, or if replaced with
`!skip` not at all.

The bundled `Text_To_Speech.xml` keeps working, through real SAPI on Windows and a small `luacom` shim
elsewhere. If it is enabled, or another plugin whose name says it is a screen reader, the client
leaves reading lines to it, so nothing is said twice.

## Hotspots

Miniwindows are pixels, so a plugin's buttons cannot be reached by a screen reader. H and J read
those that do something when clicked, and Return clicks: mouse-over, down, up, as a mouse would.

A hotspot says its tooltip; or what the plugin gave `WindowHotspotLabel`; or whatever `WindowText` last
drew inside it, while it has not been drawn over; or its ID. Hotspots that only work by dragging
cannot be reached.

## Dialogs

`tests/check_dialog_labels.py` requires every edit, combo and list in `resources/en.rc` to follow a
label, since that is where Windows gets its name, and owner-drawn buttons to have words in their
captions. Where nothing shows, the label has no size. A colour swatch's caption is its name, and it
paints only the number in it. On the port the same order gives the name, in `wx_dialog_bridge.cpp`.

## Testing with NVDA

No install and no system changes: unpack `nvda_<version>.exe` from nvaccess.org with 7-Zip, and run
`$PLUGINSDIR\app\nvda_noUIAccess.exe -m -c <config> -f <log> -l 10 --disable-addons`, where the
config's `nvda.ini` says `synth = silence`. Lines in the log starting `Speaking [` are what it said.
`tests/win/uia_probe` is a native UI Automation client that walks the pane by line, word and
character.

These prove what is exposed, not that it is usable. No blind player has tried any of this yet.

## Not done

**Trigger lists and the preference tree on the port.** On macOS `wxListCtrl` is generic, and the
preference sheets use a generic tree, so those are probably invisible to VoiceOver and Orca. The
fix is `wxDataViewCtrl`, costed in `LIST-CONTROL-DECISION.md`, whose section 7 trigger this now meets.
It touches thirteen dialogs, and doing that with no Mac to compile or look at it would be guessing. To
do it: start with the trigger list, which is the one players use, check it with VoiceOver and Orca, and
only then the others; the preference tree can go to a native `wxTreeCtrl` on GTK and an outline view
on macOS separately.

**The info bar** is a disabled RichEdit on Windows, so it cannot be reached. A plugin writing there
can also speak.

**Exact pacing on Linux.** speech-dispatcher's end-of-speech callback needs its struct's layout,
which could not be checked from where this was written.
