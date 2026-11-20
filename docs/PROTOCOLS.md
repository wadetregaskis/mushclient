# MUD protocols in the client

What the client now does for itself: telnet negotiation guards, NEW-ENVIRON (MNES), the MSSP
TLS hint, OSC 8 links, MXP 1.0, MSP, and a GMCP helper for plugins. The code is
`src/protocols*.cpp`, `src/msp.cpp` and `src/gmcp.cpp`; `src/protocols.h` says how it is laid out.

## World options

All of these go with the world file and are set with `SetOption` and `SetAlphaOption`, or
`world.SetOption ("osc8_links", 0)` in the Immediate window. None has a page in the World
Configuration dialog yet.

| Option | Default | Means |
|---|---|---|
| `osc8_links` | 1 | OSC 8 links from the server are clickable. Off, the text stays and the link does not. |
| `mtts_screen_reader` | 0 | Tell servers a screen reader is in use (MTTS bit 64, `SCREEN_READER` in MNES). It discloses a disability, so it is the player's choice. |
| `mssp_tls_hint` | 1 | Offer to reconnect with TLS if the server advertises a TLS port. |
| `mxp_windows` | 1 | Show MXP frames, gauges, stats and images in miniwindows. Off, text stays in the main window and nothing appears. |
| `msp_native` | 1 | The client plays MSP sounds itself. |
| `msp_without_negotiation` | 0 | Parse `!!SOUND` lines even if the server never said `IAC WILL 90`, for old servers with a command to turn MSP on. |
| `msp_sound_directory` | "" | Where this world's sounds are. Looked in before the others. |
| `msp_downloads` | 0 | Fetch missing sounds named by `U=`. |
| `msp_allow_http` | 0 | ... over plain http as well as https. |
| `msp_allowed_hosts` | "" | Hosts the player has agreed to download from, separated by spaces. |

A plugin cannot change `msp_downloads`, `msp_allow_http` or `msp_allowed_hosts`, and nor can a
server through `<RECOMMEND_OPTION>`.

## Telnet

**Renegotiation.** Ten negotiations of one option in a minute is more than a real server needs, so
past that the client ignores that option's negotiation for ten seconds, and says so once. Echo is
still tracked while ignored, so that a looping server cannot make a password show.

**Repeated requests and plugins.** A `WILL` repeated when the client has already said `DO` gets no
reply, and no longer runs `OnPluginTelnetRequest (n, "SENT_DO")` again either, if that ran in the
last minute. Later than that it does run, because that is what a server that copies over looks like:
it keeps its sockets, forgets everything and says `WILL` again, and the plugin's handshake is the
only answer it can get. The same goes for `DO` and `SENT_WILL`.

**CHARSET.** Names compare without regard to case, the server hears its own spelling back, and the
client takes the first on the server's list it can honestly do: a UTF-8 world says UTF-8, a Latin-1
world says ISO-8859-1 and no longer claims to be US-ASCII, and either will take US-ASCII if that is
all there is. It never changes a world's encoding.

**NEW-ENVIRON (MNES).** Answered unless a plugin claims option 39. Sent: `CLIENT_NAME`,
`CLIENT_VERSION`, `CHARSET`, `MTTS`, `TERMINAL_TYPE`, and as Mudlet does `ANSI`, `256_COLORS`,
`TRUECOLOR`, `UTF-8`, `TLS`, `OSC_HYPERLINKS`, `OSC_HYPERLINKS_SEND`, `OSC_HYPERLINKS_PROMPT`,
`SCREEN_READER`, `WORD_WRAP`. `IPADDRESS` is never sent: asked for, it comes back with no value,
which is how RFC 1572 says "not defined". A name the server asked for is echoed as it asked, so in
names and values alike the bytes 0 to 3 go with an `ESC` before them and `IAC` is doubled.

**MTTS** now includes 512 (MNES), and 64 if `mtts_screen_reader` is on.

**MSSP.** Parsed only for a port under `SSL` or `TLS`, and only once the client has said `DO`. If
there is one and the world is not using TLS, a note in the output window offers to reconnect. It
never switches by itself. The other link in the note stops it asking for this world. Not on a
connection a RELOCATE made, whose ports are another host's.

**Locale.** The client sets the C library's locale from the system's, and in many of them
`isalnum` and `tolower` say yes to bytes above 127. What a server sends is checked byte by byte
against ASCII instead (names, hosts, GMCP packages, frame sizes), so it means the same on every
machine.

## OSC 8 links

`ESC ] 8 ; params ; URI ST` opens a link and `ESC ] 8 ; ; ST` closes it. ST is BEL, `ESC \`, or the
byte 0x9C except in UTF-8. A link ends with its line.

| Target | Does |
|---|---|
| `http:`, `https:` | Opens the browser, as an MXP `<A>` does. The hover shows the URL. |
| `send:command` | Sends it, as an MXP `<SEND>` does. The hover shows it first. Percent-encoded. |
| `prompt:command` | Puts it in the command window, as `<SEND PROMPT>` does. |
| anything else | The text stays and is not a link. |

A relayed ESC can forge one of these, which is why `send:` says what it will send, and is refused
if it decodes to a control character or a `|`.

What was taken for a URI goes back to being text if a space or control character shows it was not
one, if a newline arrives first, or past 4 KB. Bytes split across packets make no difference. In a
UTF-8 world that text is repaired as the rest of the line is: a byte that is not part of a valid
character is taken as Windows-1252.

## MXP

The client says it is MXP 1.0. `<SUPPORT>` answers truthfully, including for arguments.

| Tag | Here |
|---|---|
| Line tags 20-99, `<!TAG>` | The element with `TAG=n` applies to the line. `<!TAG>` takes `FORE`, `BACK`, `GAG`, `ENABLE`, `DISABLE`, `WINDOWNAME`. |
| Modes 10, 11, 12, 19 | The line is tagged: see below. They do not change the security mode. Welcome text (19) is hidden after a RELOCATE. |
| `FLAG=RoomName` etc | As before it sets `mxp_roomname`. Now it also tags the line and tells plugins. |
| `EXPIRE`, `expire=` | Kills the links of that name, or with no name every link that has one. 256 names are remembered; past that the one used longest ago is forgotten, and its links stay. |
| Entities | `ADD` and `REMOVE` treat the value as a `|` list. `DELETE`, `DESC`, `PUBLISH`, `PRIVATE`. Names stay case-insensitive, because plugins have always been told them in lower case and QuowMinimap depends on that. A value is at most 16 KB, there are at most 1024 entities, and 1 MB of values in all; a definition (or `<VAR>`) past those leaves the entity as it was, says so as an MXP error, and does not call `OnPluginMXPsetEntity`. |
| `&#n;` | In a UTF-8 world, that character in UTF-8, up to U+10FFFF. Otherwise the byte up to 255, and beyond that what the world's code page calls it. |
| `S`, `STRIKE`, `STRIKEOUT` | Struck out. |
| `NOBR`, `SBR` | As the spec says. `SBR` is a space, which is where wrapping prefers to break. |
| `<A hint= expire=>` | The hint shows on hover. |
| `FONT` | `COLOR` and `BACK`. `FACE` and `SIZE` are accepted and ignored, and `<SUPPORT>` says `-font.face`: the output window is a fixed grid with one font. |
| `H1` to `H6` | Bold. |
| `SMALL`, `TT` | The grid cannot make text smaller, so `SMALL` is dimmer, the opposite of `HIGH`. Everything is already `TT`. |
| `GAUGE`, `STAT` | Recorded, and shown in a small miniwindow at the top right that appears only once a server asks. |
| `FRAME`, `DEST` | Miniwindows holding a grid of text. See below. |
| `IMAGE` | Shown in a miniwindow if the file is already in this world's cache folder and is PNG or BMP. Otherwise the `[url]` link it always was. Nothing is ever fetched. |
| `SOUND`, `MUSIC` | The MSP player below. |
| `RELOCATE` | A note with a link. See below. |
| `FILTER` | Not done, and `<SUPPORT>` says so. zMUD never did it either. |

**For a mapper.** A plugin can define

    function OnPluginMXPflag (mode, text)      -- eg. 10, "roomname=The Square"

which is called for each line tagged by a mode or a line tag, and for each element with a `FLAG`.
`mode` is 10 room name, 11 description, 12 exits, 13 room number, 14 prompt, 19 welcome, 20-99 the
server's own, or 0 for a `FLAG` with no meaning to a mapper (`FLAG="Set hp"`). The same number
stays on the line: `GetLineInfo (line, 14)`.

**Entities.** `MXPEntityInfo (name, type)`: 1 the value (NULL if `PRIVATE`), 2 the `DESC`, 3 is it
`PUBLISH`ed, 4 is it `PRIVATE`, 5 every `PUBLISH`ed name separated by `|`, 6 the `GAUGE` and `STAT`
bindings, a line each, as `gauge|entity|max|caption|colour`. `OnPluginMXPsetEntity` is as it was, and
`GetEntity` still answers for a `PRIVATE` entity, since it always has.

**Frames.** A frame's window is `mxp_frame_<name>`; it sits over the output window, whose text does
not move aside. `LEFT`, `TOP`, `WIDTH` and `HEIGHT` take pixels, `%` or `c`, `INTERNAL` frames go where
`ALIGN` says, `ACTION=REDIRECT` sends everything there until `<FRAME _top ACTION=REDIRECT>`. `DEST` takes
`X`, `Y`, `EOL`, `EOF`, and with nothing to write erases as the spec says. Not done: moving and resizing
them, and `FLOATING`, since a miniwindow is on top of the output anyway. Limits: eight frames, 200 rows,
256 columns. Frames are painted at most four times a second (what changes in between waits for the
next, at most a quarter of a second), and a paint draws at most 2048 runs of one colour: past that
the rest of each row is drawn in the colour it starts with, so a server cannot make painting cost
more than that by giving every cell a colour of its own.

A plugin can do any of it differently by returning false from `OnPluginMXPopenTag`, as for any tag.

**RELOCATE.** A note says where the server wants to send you, with a link to go. It never goes
by itself. From that moment `<USER>` and `<PASSWORD>` get nothing for the rest of that connection,
and a connection made this way sends no name, no password and no connect text, and gets none from
`<USER>` and `<PASSWORD>` either.

The world's own address is never changed: the destination is used for the one connection the link
makes, and saving the world while it is up (autosave included) saves the world's own host and port.
When that connection closes, or fails to connect, the next connection goes to the world's own
address, as does any connection the link did not make. A link works only for its own offer on its
own connection: a later offer replaces it, and a new connection forgets it. On a relocated connection
there is no MSSP TLS offer, and a failed TLS handshake or certificate there gives no prompt, since
what the prompt changes (TLS off, or any certificate accepted) is the world's own setting.

## MSP

Server `IAC WILL 90`, client `IAC DO 90`, and from then on lines starting `!!SOUND(name ...)` or
`!!MUSIC(name ...)` play, and appear nowhere: not the output, not triggers, not the log. A line where
that is not the start, such as "Bob says, !!SOUND(...)", is only text.

**Other players' text.** The client cannot tell a line the server wrote from one a player made it
write. A line can start with `!!SOUND(` because of `@emit`, a room description someone wrote, or the
server wrapping a long `say` at just that point, and the MSP spec leaves it to the server to filter
`!!` out of what players send. So:

- With MSP negotiated, such a line is honoured as the server's, as the spec says: another player on a
  server that does not filter can play any sound already on your machine (by a safe name, as below),
  stop yours, and with `U=` cause the one-time note asking to allow a host. That note names the host,
  and nothing is fetched unless you click it and `msp_downloads` is on.
- With `msp_without_negotiation`, which reads lines from a server that never said `WILL 90`, a line
  gets what the old `msp.xml` gave it: a sound already here. `U=` and `Off U=` are ignored, with no
  note, so no download and no question about one can come from it.
- `<SOUND>` and `<MUSIC>` are secure-mode MXP tags, the server's own, and are treated as negotiated.

`V=` volume, `L=` repeats (-1 for ever), `P=` priority, `C=` continue, `T=` type, `U=` where to
download it. `Off` stops, `Off U=...` sets the default URL. `*` and `?` work in the last part of a
name, with a random pick among matches and a new pick on each repeat. A name with no extension is
`.wav`, or `.mid` for music. By the spec, a sound replaces a playing one only if its priority is higher,
and the same music with `C=1` carries on.

Looked for in `msp_sound_directory`, then this world's cache folder, then your `sounds` folder, then
the program's. In each, the name's subdirectory (or `T=`) first, then the top.

WAV, MP3 and FLAC play on every platform. `.mid` and `.ogg` do not. A file with the same name in a
kind that does will play instead, and otherwise there is one note and silence. (Ogg's decoder is a
separate file that is not in the tree.)

The buffers used are the highest numbered that are free, so a script that picks its own is not
often disturbed.

**With a plugin.** A plugin that says yes to `OnPluginTelnetRequest (90, "WILL")` has MSP to itself,
as does the old `plugins/msp.xml` if loaded (and the client then says `DO` for it, which it could not
before). The line then stays, for their triggers.

**Downloads,** off by default. If it is turned on, and a sound is missing, and `U=` says where:

- The first time for a host, a note asks, with a link to allow it. Nothing is fetched until then.
- https only, unless `msp_allow_http`.
- Every address the name resolves to must be public: not loopback, private, link-local, or those
  in disguise in IPv6. The connection goes to the address that was checked.
- No redirects to another host, or from https to http.
- 16 MB at most, on a thread of its own, and nothing waits for it.
- `.wav`, `.mp3`, `.flac` only, and the bytes must look like what the name says.
- Names, and `T=` by the same rule, get past a check that refuses absolute paths, drive letters,
  `..`, `:` (NTFS streams), device names (`NUL`, `COM0` to `COM9`, `LPT0` to `LPT9`, `CONIN$` and the
  rest, with any extension, and with the spaces and dots Windows drops before it, as in `nul .wav`),
  and anything not plain ASCII, whether reading or writing.
- A name is percent-encoded into the URL: a space is `%20`.
- It lands in `msp_cache/<world id>/` in the data folder and nowhere else, checked with links
  resolved, and never replaces a file.
- The cache is kept to 256 MB for a world and 1 GB for every world together. Past that the oldest
  downloads go, but only `.wav`, `.mp3` and `.flac` files, so anything else you put there stays (and
  is not counted). A `.part` file a crash left behind goes once it is an hour old.
- Not at all for a world using a proxy, which is there to hide your address.

Music plays when it arrives. A sound effect is kept for next time, since a sword's clang
five seconds late is not what anyone wants.

## GMCP

A helper, so a plugin need not handle the telnet option. Plugins using `OnPluginTelnetRequest`,
`OnPluginTelnetSubnegotiation` and `SendPkt` see exactly what they did.

    GMCP_Subscribe (package [, version])    -- returns eOK or eBadParameter
    GMCP_Unsubscribe (package)
    GMCP_Send (package, json)               -- eOK, eWorldClosed, or eOptionOutOfRange if GMCP is not on
    GMCP_Get (package)                      -- raw JSON text of the last message, or NULL

    function OnPluginGMCP (package, rawText)

`lua/gmcp.lua` wraps them with JSON decoding. In languages other than Lua pass `""` as the
version if you have none; Lua can leave it out.

**Who does the handshake.** Plugins are asked first. If one says yes to `WILL`, the handshake is its
own, and the client sends no `Core.Hello` and no `Core.Supports.Set`. If none does, and there is a
subscriber, the client says `DO`, `Core.Hello` and `Core.Supports.Add`. If none does and nobody has
subscribed, it says `DONT` as it always has; a subscriber arriving later gets a late `DO`.

**What the client sends.** Only `Core.Supports.Add` and `.Remove`, and only after the plugins'
`SENT_DO` chain has run, because a plugin's `Set` replaces everything before it. For the same reason
it watches outgoing data for a `Core.Supports.Set` and sends its `Add` again after one.

**Names.** Sent as spelled, since Discworld's are lower case and IRE's are not. A version is sent only
if given, because Discworld's plugin has never given one; an IRE game wants `"Char 1"`. Incoming
messages match without regard to case, and a subscription to `Char` gets `Char.Vitals`.

**Who gets what.** `OnPluginGMCP` goes to a plugin with a subscription that covers the message, and to
one with no subscriptions at all, which has said nothing about what it wants. `GMCP_Get` knows every
message since connecting, whoever did the handshake, so it works beside Quow's plugin.

A plugin's subscriptions go when it does. Subscriptions outlive a connection, and last values do not.
