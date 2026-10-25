// utilities_portable.cpp
//
// The one function from Utilities.cpp that is genuinely platform-tied and so
// cannot be single-sourced: ShutDownSocket drives a CAsyncSocket (a POSIX shim
// here vs MFC/WinSock on Windows) and the Windows original also touches the
// status bar / TRACE.  Compiled only on non-Windows (CMakeLists.txt); the full
// Utilities.cpp provides the Windows version.
//
// Everything else that used to live here (IsStringNumber, myAtoF, the base64
// family, EditDistance) has been single-sourced into utilities_shared.cpp, which
// every build compiles.

#ifndef _WIN32

#include "platform/posix_socket.h"

// ── ShutDownSocket ────────────────────────────────────────────────────────────
// Gracefully shuts down and closes a socket.  On Windows this lives in
// Utilities.cpp; on macOS/Linux we provide the equivalent here.
void ShutDownSocket(CAsyncSocket& s)
{
    if (s.m_hSocket != INVALID_SOCKET) {
        s.AsyncSelect(0);           // stop interest in events
        s.ShutDown(CAsyncSocket::both);
        s.Close();
    }
}

#endif // !_WIN32
