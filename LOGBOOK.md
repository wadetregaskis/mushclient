# MUSHclient macOS Port — Logbook

## 2026-04-30

### Initial analysis

Spent time reading the codebase to understand scope:

- MUSHclient 5.07 — a Windows MUD client (~243K lines total, ~66K in main source tree)
- Heavily MFC-based: all GUI classes inherit from MFC (`CWnd`, `CDialog`, `CDocument/View`, etc.)
- Core data types (`CAlias`, `CTrigger`, `CTimer`, `CVariable`, `CStyle`, `CLine`) all inherit `CObject` and use `CString`
- Uses `COLORREF` (Windows RGB colour type) pervasively (~159 uses)
- Uses `CAsyncSocket` (MFC async socket) for networking
- Uses Windows COM/ActiveX scripting via `IDispatch`/`activscp.h` — impossible to port directly
- LuaCOM is also COM-specific and will need to be stubbed on macOS
- Vendored libraries already in tree: PCRE, Lua 5.1, SQLite3, zlib, libpng (partial)
- OpenSSL recently added for TLS (v3.5 LTS headers in tree)

**Key finding**: The logic/protocol code (telnet, ANSI, MXP, triggers, aliases, scripting) is all methods on `CMUSHclientDoc` (an MFC CDocument subclass). We cannot cleanly separate logic from GUI without a major refactor. The minimal-invasive approach is to provide a compatibility layer that lets the existing code compile on macOS.

**Decision**: Platform compatibility layer approach (see PLAN.md). Create `platform/` with MFC type stubs, modify `stdafx.h` to include them on non-Windows, fix compile errors iteratively.

### Plan created
- `PLAN.md` — living project plan with phases and dependency matrix
- `LOGBOOK.md` — this file

### Next steps
1. Create `CMakeLists.txt`
2. Create `platform/` directory with compat headers
3. Modify `stdafx.h` to be cross-platform
4. Begin iterative compile-fix cycle
5. Add unit tests for portable components

---

## How to pick up this work

1. Read `PLAN.md` for the overall strategy and status of each phase.
2. Read this logbook (chronological, newest at top) for what was done and why.
3. Check git log for the commit history — each commit is small and self-describing.
4. Run `cmake -S . -B build-macos && cmake --build build-macos` on macOS to see current state.
5. Run `cmake --build build-macos --target test` to run unit tests.
6. The biggest open problem is always listed in the most recent logbook entry.

---
