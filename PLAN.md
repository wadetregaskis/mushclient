# MUSHclient macOS Port — Project Plan

## Goal
Make MUSHclient build and run natively on macOS (targeting macOS 12+) while preserving full Windows compatibility. Minimise invasiveness of changes to existing source files.

## Approach: Platform Compatibility Layer

MUSHclient is built on MFC (Microsoft Foundation Classes) and uses a wide range of Windows-specific APIs throughout the codebase. Rather than rewrite the application from scratch with a new GUI framework (which would be hugely disruptive), we will:

1. **Create a platform compatibility layer** (`src/platform/`) that provides MFC-like type definitions and stubs on non-Windows platforms, allowing the existing code to compile with minimal `#ifdef` changes.
2. **Create a CMake build system** that works on both macOS and Windows.
3. **Extract and test the core logic** (networking, telnet/ANSI/MXP protocol, trigger/alias evaluation, scripting) through unit tests that run on macOS.
4. **Add a minimal macOS UI** (Phase 2) — starting with a terminal/ncurses interface, then a full GUI using wxWidgets.

This approach keeps changes to existing files minimal and focused; most new code lives in new files (`platform/`, `tests/`, `CMakeLists.txt`).

## Architecture Overview

```
MUSHclient
├── Existing Windows/MFC source (unchanged as much as possible)
│   ├── doc.cpp / doc.h          – World document (core state machine)
│   ├── telnet_phases.cpp        – Telnet protocol parser
│   ├── ansi.cpp                 – ANSI colour/style parser
│   ├── mxp/*.cpp                – MUD eXtension Protocol parser
│   ├── scripting/*.cpp          – Lua scripting layer
│   ├── xml/*.cpp                – World file serialisation
│   ├── worldsock.cpp            – Networking
│   └── dialogs/*.cpp            – GUI dialogs (Windows only)
├── platform/                    – NEW: cross-platform abstraction layer
│   ├── compat.h                 – Master platform header (included by stdafx.h)
│   ├── mfc_types.h              – CString, CObject, COLORREF, BOOL, etc.
│   ├── mfc_collections.h        – CTypedPtrList, CTypedPtrMap, CTypedPtrArray
│   ├── mfc_exceptions.h         – CException stubs
│   ├── mfc_time.h               – CTime stub
│   └── posix_socket.h           – CAsyncSocket stub using POSIX sockets
├── tests/                       – NEW: unit tests (Google Test)
│   ├── CMakeLists.txt
│   ├── test_cstring.cpp         – CString compatibility layer tests
│   ├── test_regexp.cpp          – PCRE regex wrapper tests
│   ├── test_ansi.cpp            – ANSI parser tests
│   ├── test_telnet.cpp          – Telnet protocol parser tests
│   ├── test_mxp.cpp             – MXP parser tests
│   ├── test_mcdatetime.cpp      – Date/time class tests
│   └── test_triggers.cpp        – Trigger matching tests
└── CMakeLists.txt               – NEW: top-level CMake build
```

## Windows/MFC Dependencies Matrix

| Dependency | macOS Strategy | Status |
|---|---|---|
| `CString` | `std::string`-backed wrapper class | ✅ Done |
| `CObject` | Empty base class | ✅ Done |
| `COLORREF` | `typedef uint32_t COLORREF` | ✅ Done |
| `BOOL`, `TRUE`, `FALSE` | `typedef int BOOL` etc. | ✅ Done |
| `LONGLONG`, `__int64` | `typedef int64_t` | ✅ Done |
| `LARGE_INTEGER` | struct stub | ✅ Done |
| `LPCTSTR`, `LPTSTR`, `TCHAR` | `const char*`, `char*`, `char` | ✅ Done |
| `CTypedPtrList` | `std::list<T*>` wrapper | ✅ Done |
| `CTypedPtrMap` | `std::map<std::string, T*>` wrapper | ✅ Done |
| `CTypedPtrArray` | `std::vector<T*>` wrapper | ✅ Done |
| `DECLARE_DYNAMIC` / `IMPLEMENT_DYNAMIC` | Empty macros | ✅ Done |
| `ASSERT` | `assert()` | ✅ Done |
| `CTime` | `time_t` wrapper | ✅ Done |
| `CmcDateTime` | POSIX implementation added | ✅ Done |
| `DISPID` / `DISPID_UNKNOWN` | `int` stubs | ✅ Done |
| `CAsyncSocket` | POSIX socket wrapper | 🔲 Phase 3 |
| `CEvent`, `CMutex`, `CCriticalSection` | `pthread` / `std::mutex` | 🔲 Phase 2 |
| `AFX_MANAGE_STATE` | No-op macro | ✅ Done |
| `CFormat` | Reimplement without CString base | ✅ Done |
| `CException` | std::exception wrapper | ✅ Done |
| `PCRE` | Vendored (all .c files in CMake) | ✅ Done |
| Lua 5.1 | Optional; warn if absent | ✅ Done |
| OpenSSL | System (Homebrew: `brew install openssl`) | ✅ Optional |
| SQLite3 | Vendored (already in tree) | ✅ Done |
| zlib | System (`ZLIB::ZLIB`) | ✅ Done |
| libpng | System or vendored | 🔲 Phase 2 |
| COM/ActiveX scripting | Stubbed on macOS (COM-specific; all methods are no-ops) | ✅ Done |
| LuaCOM | Stubbed on macOS (`#ifdef _WIN32` in lua_scripting.cpp) | ✅ Done |
| GDI (`CDC`, `CPen`, etc.) | Stubbed (Phase 5: full GUI) | 🔲 Phase 5 |
| MFC GUI (`CWnd`, `CDialog`, etc.) | Stub in compat.h (Phase 5: full GUI) | 🔲 Phase 5 |

## Phased Roadmap

### Phase 1: Build Infrastructure ✅ COMPLETE
- [x] Explore and understand codebase
- [x] Create `CMakeLists.txt` (macOS/Linux; Windows keeps .sln/.vcxproj)
- [x] Create `platform/compat.h` — master cross-platform header
- [x] Create `platform/windows_types.h` — basic types (BOOL, COLORREF, LARGE_INTEGER, etc.)
- [x] Create `platform/mfc_object.h` — CObject + DECLARE/IMPLEMENT_DYNAMIC macros
- [x] Create `platform/mfc_string.h` — CString backed by std::string
- [x] Create `platform/mfc_collections.h` — CTypedPtrList/Map/Array
- [x] Create `platform/mfc_exceptions.h` — CException stubs
- [x] Create `platform/mfc_time.h` — CTime backed by time_t
- [x] Modify `stdafx.h` to include `platform/compat.h` on non-Windows
- [x] Make `mcdatetime.cpp`, `regexp.cpp`, `Color.cpp` compile on macOS
- [x] Add `utilities_portable.cpp` (IsStringNumber extracted from Utilities.cpp)
- [x] First macOS build passes with 0 errors

**Test status: 177/177 passing** (`test_cstring` 45, `test_regexp` 14, `test_mcdatetime` 14, `test_colour` 12, `test_hashing` 12, `test_ansi` 23, `test_telnet` 37, `test_mxp` 20)
Lua scripting compiles and links. With `brew install lua@5.1`, the full Lua VM + LPeg + lsqlite3 build in automatically.

**AppleClang 17 fixes applied:**
- Removed `register` keyword from `regexp.cpp` / `regexp.h` (C++17 hard error)
- Guarded `std::binary_function` inheritance in `stdafx.h` with `#if __cplusplus < 201703L`
- Moved `#include <vector>` to top of `platform/mfc_string.h` (libc++ doesn't transitively include it)
- Added `-Wno-non-pod-varargs` to `mushcore` compile options (CString passed to TFormat)

### Phase 2: Core Logic Compilation ✅ Complete
- [x] `scripting/md5.c`, `sha256.c`, `shs.c` — portable after LP64 type-width fix
- [x] `ansi.cpp` — ANSI colour parser — ported via platform/doc_stub.h
- [x] `telnet_phases.cpp` — telnet protocol — ported; OutputBadUTF8characters wrapped with `#ifdef _WIN32`
- [x] `mxp/*.cpp` — MUD eXtension Protocol parser (all 14 files)
- [ ] `xml/*.cpp` — world file serialisation (deferred; deeply MFC-coupled: CArchive, CMUSHview, dialogs)
- [x] `scripting/bits.c`, `lrexlib.c`, `lua_compress.c`, `lbc.c`, `mt19937ar.c`, `number.c` — Lua C extensions ported
- [x] `scripting/lua_scripting.cpp` — ported; LuaCOM + `CScriptErrorDlg` + `QueryPerformanceCounter` guarded with `#ifdef _WIN32`
- [x] `scripting/scriptengine.cpp` — COM body guarded `#ifdef _WIN32`; macOS stubs for COM + Lua methods; `MUSHCLIENT_LUA_SCRIPTING` define controls Lua stub compilation
- [x] `scripting/lpeg/{lptree,lpvm,lpcap,lpcode}.c` + `lptypes.h`, `lpcode.h` — Lua include guards fixed
- [x] `lsqlite/lsqlite3.c` — Lua include guard fixed
- [x] `scripting/lua_utils_portable.cpp` (new) — portable `MakeLuaState`, `MakeTableItem`×3, `GetNestedFunction`, `FindLuaFunction`, `MakeTableItemBool`, `RegisterLuaRoutines` (minimal), `DisableDLLs`
- [x] `Utilities.cpp` — excluded from macOS build; portable parts in `utilities_portable.cpp`
- [x] `worldsock.cpp` — fully compiles on macOS; `#ifdef _WIN32` guard; POSIX CAsyncSocket; doc_stub.h extended with socket/connection/scripting fields

**CMUSHclientDoc stub strategy:** Rather than porting the full 16,000-line `doc.cpp`,
create a thin stub class in `platform/` that satisfies the dependencies of the
protocol files (telnet, ansi, mxp) without any GUI or MFC.

### Phase 3: Networking ✅ Complete
- [x] Create `platform/posix_socket.h` — CAsyncSocket implemented with POSIX sockets
- [x] Make `worldsock.cpp` compile on macOS

### Phase 4: CLI Driver & Protocol Validation ✅ Complete
- [x] Real `ReceiveMsg()` — reads from socket, handles MCCP zlib decompression, calls DisplayMsg
- [x] Real `DisplayMsg()` — full telnet/ANSI/MXP Phase_* state machine; output to stdout
- [x] Real `SendPacket()` — queues to CWorldSocket::m_outstanding_data for async send
- [x] Real `OnConnect()` — marks eConnectConnectedToMud, resets session counters
- [x] Real `InitZlib()` — initialises zlib inflate stream for MCCP
- [x] Real `SSLCleanup()` / `ContinueSSLHandshake()` — SSL teardown; handshake stub
- [x] Real `Note()` / `UpdateAllViews()` — console output
- [x] `platform/cli_main.cpp` — CLI driver: argv host/port, readline line editing, event loop
- [x] readline integration — command history, Ctrl-A/E, blank-line send fix, before/after hooks
- [x] `mushcli` target in CMakeLists.txt; optional readline detected by CMake
- [x] MXP tables initialised on startup (`App.MXP_LoadElements/Entities/Colours()`)
- [x] Crash fix: `m_pScratchLine` ensures `m_pCurrentLine` is always non-null
- [x] **ANSI ESC phase fix** — ESC now transitions to `HAVE_ESC` instead of `NONE`; prevents `[31m` etc. printing literally
- [x] 256-colour output: `TermSyncStyle()` maps RGB → nearest xterm-256 palette; `TermResetStyle()` at line end
- [x] Bright colour codes (90–97 fg, 100–107 bg) added to `InterpretANSIcode` for both ANSI and RGB modes
- [x] UTF-8: `m_bUTF_8 = 1`; `TermSyncStyle()` only fires on lead bytes, not continuation bytes
- [x] NAWS: real `SendWindowSizes()` using `ioctl(TIOCGWINSZ)`; `SIGWINCH` handler resends on resize
- [x] MTTS capabilities: advertise `XTERM-256COLOR` + bits 1+8 (ANSI + 256 colour); no false truecolor claim
- [x] Debug logging: `-d` / `--debug` flag; `dbg()` + `telopt_name()` in all protocol handlers
- [x] GMCP/subnegotiation logging: every SB payload logged (text-safe preview or byte count)
- [x] Guard: MXP command-tag path in `mxpStart.cpp` repopulates scratch line if emptied
- [x] Live test: `mushcli batmud.bat.org 23` and DiscworldMUD — colours, MXP, MCCP all working

### Phase 5: Full GUI
**Framework chosen: wxWidgets** — native Cocoa backend on macOS; MFC-like Document/View model; same source builds on Windows and Linux.

#### Phase 5.1: Scaffold ✅ Complete
- [x] `platform/wx_app.cpp` — `CMUSHApp : wxApp`; parses `-d`/`--debug` + host/port args; opens `CWorldFrame`
- [x] `platform/wx_frame.h` / `wx_frame.cpp` — `CWorldFrame : wxFrame` with:
  - Monospace `wxTextCtrl` (rich-text, black background) as scrollback output
  - Single-line `wxTextCtrl` for user input; `EVT_TEXT_ENTER` sends to MUD
  - `wxTimer` (20 ms) calls `CAsyncSocket::Poll(0)` — all I/O on the GUI thread, no locking
  - `AppendLine(SOutputLine&&)` — maps `SOutputRun` spans to `wxTextAttr` (colour, bold, underline)
  - `AppendNote(string)` — italic grey text for status / connection messages
  - `wxStatusBar` (connection state + byte counters)
- [x] `SOutputRun` / `SOutputLine` types in `doc_stub.h`
- [x] `CMUSHclientDoc::m_fn_line_ready` + `m_fn_note` callbacks:
  - When set, `DisplayMsg()` accumulates `SOutputRun` spans into `m_gui_line_buf`; fires callback on `\n`
  - When null (CLI mode), stdout / `TermSyncStyle()` path unchanged
  - `Note()` routes through `m_fn_note` when set
- [x] `CMakeLists.txt` — `mushgui` target (`MACOSX_BUNDLE`); `find_package(wxWidgets QUIET)` (skipped gracefully if absent)
- [x] Install: `brew install wxwidgets` on macOS, then `cmake .. && make mushgui`

#### Phase 5.2: Connect dialog / world file loading ✅ Complete
- [x] Load `.mcl` (XML) world files for host/port/options (`OnMenuOpenWorld`)
- [x] "Connect to…" dialog (host, port fields; Enter confirms) (`OnMenuConnect`)
- [x] Reconnect menu item (`OnMenuReconnect`)
- [x] Recent worlds list / menu — `wxConfig`-backed MRU (up to 9), `World > Recent Worlds` submenu, persists across sessions (session 10)

#### Phase 5.3: Output window polish — partial
- [x] Scrollback limit — `COutputPane` capped at 5000 lines, oldest trimmed on overflow
- [x] Copy selection to clipboard — right-click context menu + Ctrl+C in output pane
- [x] Select All — right-click context menu
- [ ] URL detection → clickable hyperlinks
- [ ] MXP clickable elements (already parsed; need GUI action routing)
- [x] Font size preference / Ctrl+scroll zoom (session 11: Ctrl+scroll, Ctrl+=/-/0 keyboard shortcuts; m_fn_status status bar wiring)

#### Phase 5.4: Input bar enhancements ✅ Complete
- [x] Command history — Up/Down arrows cycle through sent commands; draft text preserved
- [x] Multi-line paste — `wxEVT_TEXT_PASTE` intercept; splits on `\n`; sends all but last line; last fragment stays in input bar; existing typed prefix prepended to first pasted line (session 10)

#### Phase 5.5: Full application chrome
- [ ] Menu bar (File → Connect / Disconnect / Quit; Edit → Preferences)
- [ ] Multiple world tabs
- [ ] World settings dialog (full options from `doc.h`)
- [ ] Trigger / alias editor UI

### Ongoing: Tests — **248 tests, 0 failures** (session 9)
- [x] `tests/test_cstring.cpp` — 45 tests
- [x] `tests/test_regexp.cpp` — 14 tests
- [x] `tests/test_mcdatetime.cpp` — 14 tests
- [x] `tests/test_colour.cpp` — 12 tests
- [x] `tests/test_hashing.cpp` — 12 tests (MD5, SHA-256, SHA-0)
- [x] `tests/test_ansi.cpp` — 32 tests (incl. bright colours, DisplayMsg end-to-end, ESC phase regression)
- [x] `tests/test_telnet.cpp` — 42 tests (incl. NAWS, MXP negotiation, terminal type sequence)
- [x] `tests/test_mxp.cpp` — 20 tests
- [x] `tests/test_triggers.cpp` — 11 tests (trigger matching, regex, enabled/disabled, sequences)
- [x] `tests/test_aliases.cpp` — 11 tests (EvaluateCommand, alias matching/substitution, send-to)
- [x] `tests/test_world_load.cpp` — 26 tests (XML world file round-trip)
- [x] `tests/test_timers.cpp` — 10 tests (timer enable/disable, sequence, fire-count)
- [ ] `tests/test_lua_scripting.cpp` — requires Lua 5.1 at link time

## Key Constraints
- **Do not break Windows build.** All changes to existing files must be guarded with `#ifndef _WIN32` or `#ifdef _WIN32` as appropriate.
- **Minimise changes to existing files.** Prefer adding new files over modifying existing ones.
- **Small, focused commits.** Each commit should do one thing.
- **Tests for everything new.** Every new platform-abstraction component gets unit tests.

## Notes on CString Implementation
`CString` is used ~1304 times in the main source tree. The key interface needed:
- Constructors from `const char*`, `char`, `int` (char repeat), `CString`
- `operator const char*()` (implicit conversion to C string — this is the trickiest part)
- Comparison: `==`, `!=`, `<`, `>`, `<=`, `>=` (both `CString op CString` and `CString op const char*`)
- Concatenation: `+`, `+=`
- `.IsEmpty()`, `.GetLength()`, `.GetAt(i)`
- `.Left(n)`, `.Right(n)`, `.Mid(pos)`, `.Mid(pos, len)`
- `.TrimLeft()`, `.TrimRight()`, `.Trim()`
- `.MakeLower()`, `.MakeUpper()`
- `.Find(sub)`, `.Find(sub, start)`, `.ReverseFind(c)`
- `.Replace(from, to)`
- `.Format(fmt, ...)` — like `sprintf`
- `.GetBuffer(n)` / `.ReleaseBuffer()`
- `LoadString(nID)` — load from resources (stub: return empty)
- Coercion from `std::string`

## Notes on MFC Collection Types
The codebase uses these MFC collection templates:
- `CTypedPtrList<CPtrList, T*>` — doubly-linked list of `T*`
- `CTypedPtrMap<CMapStringToPtr, CString, T*>` — `CString`-keyed map of `T*`
- `CTypedPtrArray<CPtrArray, T*>` — array of `T*`

MFC uses `POSITION` (a non-null opaque cursor) for iteration. Our implementation will use raw pointers to list nodes.

The key collection operations seen in the codebase:
- List: `AddTail`, `AddHead`, `RemoveTail`, `RemoveHead`, `GetTail`, `GetHead`, `GetCount`, `IsEmpty`, `RemoveAll`, `GetHeadPosition`, `GetTailPosition`, `GetNext`, `GetPrev`, `GetAt`, `SetAt`, `Find`, `InsertBefore`, `InsertAfter`, `RemoveAt`
- Map: `SetAt`, `Lookup`, `RemoveKey`, `GetCount`, `IsEmpty`, `RemoveAll`, `GetStartPosition`, `GetNextAssoc`
- Array: `GetAt`, `SetAt`, `Add`, `GetSize`, `GetUpperBound`, `RemoveAll`, `SetSize`
