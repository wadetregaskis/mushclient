# Plugin sandboxing: not done, to revisit

Decided 2026-10-31: we should tackle this, but not yet. This note holds the
research so that it need not be repeated.

## Where things stand

No sandbox is active by default. A plugin can run `os.execute` or
`utils.shellexecute`, read and write anything the user can, load DLLs, open
sockets, and read everything typed through `OnPluginSend`.

- `luaL_openlibs` opens everything (`src/scripting/lua_scripting.cpp`).
- `DisableDLLs` always stubs `os.exit`, `io.popen` and `debug.debug`, and removes
  `loadlib` and the C loaders only if `AllowLoadingDlls` is off, which it isn't by
  default.
- `lua/sandbox.lua` is opt-in, trusts a plugin's self-declared ID, and leaves
  `utils.shellexecute`, luacom, `sqlite3.open`, debug and `loadstring` open.
- "Can you trust your plugins?" in the welcome text is only a link.

## The honest limit

Plugins run inside the client, so this can be a seatbelt, not a security boundary.
A real boundary means out-of-process hosts brokering some 430 world functions,
synchronous triggers and miniwindows, which is effectively a rewrite.

| Language | What is realistic |
|---|---|
| Lua | Practical, but must also close debug, `string.dump`, binary chunks, luacom, C loaders and SQL `ATTACH` |
| Tcl | Safe interpreters are real, but we use a full one today |
| JavaScript | QuickJS has no std or os module here, so it is nearly confined already |
| Python, Ruby, Perl | Nothing reliable in-process |
| COM engines | Perhaps block `CreateObject`, which breaks some stock plugins |

## The proposal

Don't break plugins silently, which rules out default-on confinement.

1. **Audit, on by default, blocking nothing.** Record per plugin: files outside its
   folder, exec, native code, network, COM. Show it in the plugin list.
2. **Optional `<permissions>` element.** Plugins that declare it are enforced at C
   level. Undeclared ones stay allowed but audited.
3. **Allow, ask or deny per plugin**, keyed on plugin ID plus SHA-256 of the file (not
   the self-declared ID alone), asking again when the file changes.
4. **Picker grants.** A file chosen in a file picker becomes a grant.
5. **Strict mode as an explicit choice**, also closing debug, bytecode, luacom and C
   loaders.
6. **Other languages** are labelled "can't be restricted".

It gives visibility and protection from buggy or casual plugins, and in strict Lua
mode a meaningful file and exec barrier. It does not stop in-game abuse, keylogging
through the normal API, resource exhaustion, or anything the user approved.

QMud's path confinement (its `LuaCallbackEngine.cpp`, `Development.txt`) wraps `io.*`,
`os.remove`, `os.rename`, `loadfile`, `dofile` and `sqlite3.open`. It leaves
`os.execute`, `utils.shellexecute` and C modules open, and plugins can still read each
other's files.

## What confinement would break

- `QuowMinimap`: sound paths from a file picker, its map database in a picked folder,
  `loadfile` from the plugin folder, and a deliberate `os.exit()` as an abort, so keep
  the raising stub.
- `QuowMissionHelper`: `loadfile`.
- `ATCP_Mapper`, `MUSHclient_Help`, `Version_Check`, `Global_Option_Updater`: SQLite
  files outside the plugin folder.
- `Installer_sumcheck`, `GB2312_converter`, `encrypted_chat`, `Text_To_Speech`:
  `io.open`, `dofile`, `loadlib`, luacom.
- `Calendar`, `MudDatabase`, `AOD_EQ`: VBScript COM.
