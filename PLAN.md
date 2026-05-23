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
| `CString` | `std::string`-backed wrapper class | TODO |
| `CObject` | Empty base class | TODO |
| `COLORREF` | `typedef uint32_t COLORREF` | TODO |
| `BOOL`, `TRUE`, `FALSE` | `typedef int BOOL` etc. | TODO |
| `LONGLONG`, `__int64` | `typedef int64_t` | TODO |
| `LARGE_INTEGER` | struct stub | TODO |
| `LPCTSTR`, `LPTSTR`, `TCHAR` | `const char*`, `char*`, `char` | TODO |
| `CTypedPtrList` | `std::list<T*>` wrapper | TODO |
| `CTypedPtrMap` | `std::map<std::string, T*>` wrapper | TODO |
| `CTypedPtrArray` | `std::vector<T*>` wrapper | TODO |
| `DECLARE_DYNAMIC` / `IMPLEMENT_DYNAMIC` | Empty macros | TODO |
| `ASSERT` | `assert()` | TODO |
| `CTime` | `time_t` wrapper | TODO |
| `CmcDateTime` | Already mostly portable | TODO |
| `DISPID` / `DISPID_UNKNOWN` | `int` stubs | TODO |
| `CAsyncSocket` | POSIX socket wrapper | TODO |
| `CEvent`, `CMutex`, `CCriticalSection` | `pthread` / `std::mutex` | TODO |
| `AFX_MANAGE_STATE` | No-op macro | TODO |
| `CFormat` | Reimplement without CString base | TODO |
| `CException` | std::exception wrapper | TODO |
| `PCRE` | Vendored (already in tree) | OK |
| Lua 5.1 | Vendored (already in tree) | OK |
| OpenSSL | System (Homebrew: `brew install openssl`) | TODO |
| SQLite3 | Vendored (already in tree) | OK |
| zlib | System (macOS ships it) | TODO |
| libpng | System or vendored | TODO |
| COM/ActiveX scripting | Stubbed (not supported on macOS) | TODO |
| LuaCOM | Stubbed on macOS (COM-specific) | TODO |
| GDI (`CDC`, `CPen`, etc.) | Stubbed (Phase 2: wxWidgets) | TODO |
| MFC GUI (`CWnd`, `CDialog`, etc.) | Stubbed (Phase 2: wxWidgets) | TODO |

## Phased Roadmap

### Phase 1: Build Infrastructure ✅ In Progress
- [x] Explore and understand codebase
- [ ] Create `CMakeLists.txt` (macOS + Windows)
- [ ] Create `platform/compat.h` — master cross-platform header
- [ ] Create `platform/mfc_types.h` — basic types + CString
- [ ] Create `platform/mfc_collections.h` — collection templates
- [ ] Create `platform/mfc_exceptions.h`
- [ ] Create `platform/mfc_time.h`
- [ ] Modify `stdafx.h` to include `platform/compat.h` on non-Windows
- [ ] First macOS compile attempt on a subset of files

### Phase 2: Core Logic Compilation
- [ ] Make `mcdatetime.cpp` compile on macOS
- [ ] Make `regexp.cpp` + `pcre/` compile on macOS
- [ ] Make `Color.cpp` compile on macOS
- [ ] Make `xml/*.cpp` compile on macOS
- [ ] Make `scripting/md5.c`, `sha256.c`, `shs.c` compile on macOS
- [ ] Make `scripting/lrexlib.c`, `lua_compress.c`, `bits.c` compile on macOS
- [ ] Make `scripting/lua_scripting.cpp` compile on macOS (with LuaCOM stubbed)
- [ ] Make `mxp/*.cpp` compile on macOS
- [ ] Make `telnet_phases.cpp` + `ansi.cpp` compile on macOS (CMUSHclientDoc stub)
- [ ] Make `Utilities.cpp` compile on macOS

### Phase 3: Networking
- [ ] Create `platform/posix_socket.h` — CAsyncSocket POSIX implementation
- [ ] Make `worldsock.cpp` compile on macOS
- [ ] Test TCP connection to a MUD server

### Phase 4: Minimal macOS "Run" Capability
- [ ] Create a test harness / CLI driver that exercises core logic
- [ ] OR integrate ncurses for a basic terminal UI
- [ ] Connect, receive text, process triggers, send commands

### Phase 5: Full GUI (Long-term)
- [ ] Evaluate wxWidgets vs Qt for GUI layer
- [ ] Implement macOS GUI wrapping existing doc/view logic

### Ongoing: Tests
- [ ] `tests/test_cstring.cpp`
- [ ] `tests/test_regexp.cpp`
- [ ] `tests/test_mcdatetime.cpp`
- [ ] `tests/test_ansi.cpp`
- [ ] `tests/test_telnet.cpp`
- [ ] `tests/test_mxp.cpp`
- [ ] `tests/test_triggers.cpp`
- [ ] `tests/test_colour.cpp`

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
