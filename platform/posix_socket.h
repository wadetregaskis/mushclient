// platform/posix_socket.h
//
// POSIX implementation of MFC's CAsyncSocket.
//
// On Windows, CAsyncSocket is part of MFC and uses WSAAsyncSelect() to deliver
// socket events (FD_READ, FD_WRITE, FD_CONNECT, FD_CLOSE) as Windows messages.
//
// On macOS/Linux we provide a compatible class backed by POSIX sockets.
// Event dispatch is triggered by calling CAsyncSocket::Poll() from the
// application's main loop — it runs select(2) with a short timeout and calls
// the appropriate On* virtual method for each socket that has a pending event.
//
// Guard: this header is only included on non-Windows (see platform/compat.h).

#ifndef _WIN32
#ifndef POSIX_SOCKET_H
#define POSIX_SOCKET_H

// Bring in BOOL, DWORD, FALSE/TRUE etc. if not yet defined.
// When included via compat.h these are already present; when included
// standalone (e.g. utilities_portable.cpp) we need them directly.
#include "platform/windows_types.h"
#include "platform/mfc_object.h"   // CRuntimeClass + SHIM_RUNTIME_CLASS_ROOT (RTTI)

#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <algorithm>   // std::remove
#include <cstring>
#include <cstdio>      // snprintf
#include <vector>
#include <string>

// ── Windows socket constants mapped to POSIX ─────────────────────────────────

typedef int                SOCKET;
typedef struct sockaddr    SOCKADDR;
typedef struct sockaddr_in SOCKADDR_IN;

static const SOCKET INVALID_SOCKET_VAL = -1;
#define INVALID_SOCKET  (-1)
#undef  SOCKET_ERROR
#define SOCKET_ERROR    (-1)

// Winsock error code aliases
#define WSAEWOULDBLOCK  EWOULDBLOCK
#define WSAECONNREFUSED ECONNREFUSED
#define WSAEHOSTUNREACH EHOSTUNREACH
#define WSAENETUNREACH  ENETUNREACH
#define WSAETIMEDOUT    ETIMEDOUT
#define WSAECONNRESET   ECONNRESET
#define WSAEADDRINUSE   EADDRINUSE

// MFC FD_ event flags (used with AsyncSelect)
#define FD_READ       0x01
#define FD_WRITE      0x02
#define FD_OOB        0x04
#define FD_ACCEPT     0x08
#define FD_CONNECT    0x10
#define FD_CLOSE      0x20

// ── CAsyncSocket ──────────────────────────────────────────────────────────────

class CAsyncSocket {
    SHIM_RUNTIME_CLASS_ROOT(CAsyncSocket)   // for IMPLEMENT_DYNAMIC(CWorldSocket, CAsyncSocket)
public:
    // ── Types ─────────────────────────────────────────────────────────────────
    enum ShutdownHow { receives = 0, sends = 1, both = 2 };

    // ── Public data ───────────────────────────────────────────────────────────
    SOCKET  m_hSocket = INVALID_SOCKET; // raw socket descriptor

    // ── Construction / destruction ────────────────────────────────────────────
    CAsyncSocket()  = default;
    virtual ~CAsyncSocket() { if (m_hSocket != INVALID_SOCKET) Close(); }

    // ── Creation ──────────────────────────────────────────────────────────────
    // Creates a non-blocking TCP socket.  nSocketPort is ignored (bound by OS
    // at connect time).  nSocketType is SOCK_STREAM or SOCK_DGRAM.
    // lpszSocketAddress and lEvent are accepted for API compatibility.
    BOOL Create(UINT /*nSocketPort*/ = 0,
                int  nSocketType     = SOCK_STREAM,
                long /*lEvent*/      = FD_READ | FD_WRITE | FD_OOB |
                                       FD_ACCEPT | FD_CONNECT | FD_CLOSE,
                LPCSTR /*lpszSocketAddress*/ = nullptr)
    {
        if (m_hSocket != INVALID_SOCKET) Close();

        m_hSocket = ::socket(AF_INET, nSocketType, 0);
        if (m_hSocket == INVALID_SOCKET) return FALSE;

        // Set non-blocking
        int flags = ::fcntl(m_hSocket, F_GETFL, 0);
        if (flags == -1 || ::fcntl(m_hSocket, F_SETFL, flags | O_NONBLOCK) == -1) {
            Close(); return FALSE;
        }

        // Disable Nagle for TCP
        if (nSocketType == SOCK_STREAM) {
            int opt = 1;
            ::setsockopt(m_hSocket, IPPROTO_TCP, TCP_NODELAY,
                         &opt, sizeof(opt));
        }

        Register(this);
        return TRUE;
    }

    // ── Connection ────────────────────────────────────────────────────────────
    // Non-blocking connect.  Returns FALSE with WSAEWOULDBLOCK when in progress
    // (which is normal — OnConnect will be called when it completes).
    BOOL Connect(const SOCKADDR* lpSockAddr, int nSockAddrLen)
    {
        int rc = ::connect(m_hSocket, lpSockAddr, (socklen_t)nSockAddrLen);
        if (rc == 0) {
            // Immediate connect (loopback etc.)
            m_state = kConnected;
            return TRUE;
        }
        if (errno == EINPROGRESS || errno == EWOULDBLOCK) {
            m_state = kConnecting;
            errno = EWOULDBLOCK;  // normalise to WSAEWOULDBLOCK for callers
            return FALSE;  // caller should check GetLastError() == WSAEWOULDBLOCK
        }
        return FALSE;
    }

    // Convenience overload with host string + port (not in MFC but useful for macOS tests)
    BOOL Connect(const char* lpszHostAddress, UINT nHostPort)
    {
        struct addrinfo hints{}, *res = nullptr;
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%u", nHostPort);
        if (::getaddrinfo(lpszHostAddress, port_str, &hints, &res) != 0 || !res)
            return FALSE;
        BOOL ok = Connect(res->ai_addr, (int)res->ai_addrlen);
        ::freeaddrinfo(res);
        return ok;
    }

    // ── Send / Receive ────────────────────────────────────────────────────────
    int Send(const void* lpBuf, int nBufLen, int nFlags = 0)
    {
        int n = (int)::send(m_hSocket, lpBuf, (size_t)nBufLen, nFlags);
        if (n < 0 && errno == EWOULDBLOCK) return SOCKET_ERROR;
        return n;
    }

    int Receive(void* lpBuf, int nBufLen, int nFlags = 0)
    {
        int n = (int)::recv(m_hSocket, lpBuf, (size_t)nBufLen, nFlags);
        if (n < 0 && errno == EWOULDBLOCK) return SOCKET_ERROR;
        return n;
    }

    // ── Server-side: Bind / Listen / Accept ──────────────────────────────────
    BOOL Bind(UINT nSocketPort, LPCTSTR lpszSocketAddress = nullptr)
    {
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons((uint16_t)nSocketPort);
        if (lpszSocketAddress && *lpszSocketAddress)
            addr.sin_addr.s_addr = inet_addr(lpszSocketAddress);
        else
            addr.sin_addr.s_addr = INADDR_ANY;
        int opt = 1;
        ::setsockopt(m_hSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        return (::bind(m_hSocket, (struct sockaddr*)&addr, sizeof(addr)) == 0)
               ? TRUE : FALSE;
    }

    BOOL Listen(int nConnectionBacklog = 5)
    {
        if (::listen(m_hSocket, nConnectionBacklog) != 0) return FALSE;
        m_state = kListening;   // Poll() turns readability into OnAccept
        return TRUE;
    }

    // Accept an incoming connection into rConnectedSocket.
    // lpSockAddr / lpSockAddrLen receive the peer address (may be nullptr).
    BOOL Accept(CAsyncSocket& rConnectedSocket,
                SOCKADDR* lpSockAddr = nullptr, int* lpSockAddrLen = nullptr)
    {
        socklen_t addrLen = lpSockAddrLen ? (socklen_t)*lpSockAddrLen : 0;
        SOCKET s = ::accept(m_hSocket,
                            lpSockAddr,
                            lpSockAddr ? &addrLen : nullptr);
        if (lpSockAddrLen) *lpSockAddrLen = (int)addrLen;
        if (s == INVALID_SOCKET) return FALSE;
        // Hand off to the caller's socket object
        if (rConnectedSocket.m_hSocket != INVALID_SOCKET)
            ::close(rConnectedSocket.m_hSocket);
        rConnectedSocket.m_hSocket = s;
        // Set non-blocking on accepted socket
        int flags = ::fcntl(s, F_GETFL, 0);
        if (flags != -1) ::fcntl(s, F_SETFL, flags | O_NONBLOCK);
        Register(&rConnectedSocket);
        return TRUE;
    }

    // ── Shutdown / close ──────────────────────────────────────────────────────
    BOOL ShutDown(ShutdownHow nHow = sends)
    {
        return (::shutdown(m_hSocket, (int)nHow) == 0) ? TRUE : FALSE;
    }

    void Close()
    {
        if (m_hSocket != INVALID_SOCKET) {
            Unregister(this);
            ::close(m_hSocket);
            m_hSocket = INVALID_SOCKET;
            m_state   = kIdle;
        }
    }

    // ── Socket options ────────────────────────────────────────────────────────
    BOOL SetSockOpt(int nOptionName, const void* lpOptionValue,
                    int nOptionLen, int nLevel = SOL_SOCKET)
    {
        return (::setsockopt(m_hSocket, nLevel, nOptionName,
                             lpOptionValue, (socklen_t)nOptionLen) == 0)
               ? TRUE : FALSE;
    }

    BOOL GetSockName(SOCKADDR* lpSockAddr, int* lpSockAddrLen)
    {
        socklen_t len = (socklen_t)*lpSockAddrLen;
        int rc = ::getsockname(m_hSocket, lpSockAddr, &len);
        *lpSockAddrLen = (int)len;
        return (rc == 0) ? TRUE : FALSE;
    }

    // AsyncSelect: on POSIX the polling is done via Poll(); this just records
    // which events are wanted.  Pass 0 to deregister all events.
    BOOL AsyncSelect(long lEvent = FD_READ | FD_WRITE | FD_OOB |
                                   FD_ACCEPT | FD_CONNECT | FD_CLOSE)
    {
        m_events = lEvent;
        return TRUE;
    }

    // IOCtl stub (used in commented-out code in worldsock.cpp — safe no-op)
    BOOL IOCtl(long /*lCommand*/, DWORD* /*lpArgument*/) { return TRUE; }

    // ── Error ─────────────────────────────────────────────────────────────────
    static int GetLastError() { return errno; }

    // ── Virtual event callbacks (override in CWorldSocket) ───────────────────
    virtual void OnReceive (int /*nErrorCode*/) {}
    virtual void OnSend    (int /*nErrorCode*/) {}
    virtual void OnClose   (int /*nErrorCode*/) {}
    virtual void OnConnect (int /*nErrorCode*/) {}
    virtual void OnAccept  (int /*nErrorCode*/) {}

    // ── Polling ───────────────────────────────────────────────────────────────
    // Call this from the application event loop.  It checks all registered
    // sockets for pending events (up to timeout_ms milliseconds) and calls
    // the appropriate On* callback.
    static void Poll(int timeout_ms = 10)
    {
        auto& s_sockets = AllSockets();
        if (s_sockets.empty()) return;

        fd_set rd, wr, ex;
        FD_ZERO(&rd); FD_ZERO(&wr); FD_ZERO(&ex);
        int maxfd = -1;

        for (CAsyncSocket* s : s_sockets) {
            if (s->m_hSocket == INVALID_SOCKET) continue;
            int fd = s->m_hSocket;
            if (s->m_events & (FD_READ  | FD_CLOSE | FD_ACCEPT)) FD_SET(fd, &rd);
            if (s->m_events & (FD_WRITE | FD_CONNECT)) FD_SET(fd, &wr);
            if (s->m_events & FD_OOB)                  FD_SET(fd, &ex);
            if (fd > maxfd) maxfd = fd;
        }
        if (maxfd < 0) return;

        struct timeval tv { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
        int rc = ::select(maxfd + 1, &rd, &wr, &ex, &tv);
        if (rc <= 0) return;

        // Take a snapshot: callbacks may modify s_sockets — and may even DELETE
        // a socket (e.g. a TLS certificate-verification failure tears the
        // connection down from inside OnReceive, freeing the CAsyncSocket).
        // Re-check IsLive(s) after every callback so we never dereference a
        // pointer in the snapshot that a callback has since freed/unregistered.
        std::vector<CAsyncSocket*> snap(s_sockets);

        for (CAsyncSocket* s : snap) {
            if (!IsLive(s) || s->m_hSocket == INVALID_SOCKET) continue;
            int fd = s->m_hSocket;

            if (FD_ISSET(fd, &wr)) {
                if (s->m_state == kConnecting) {
                    // Check whether non-blocking connect succeeded
                    int err = 0;
                    socklen_t len = sizeof(err);
                    ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
                    if (err == 0) {
                        s->m_state = kConnected;
                        s->OnConnect(0);
                    } else {
                        errno = err;
                        s->m_state = kIdle;
                        s->OnConnect(err);
                    }
                } else {
                    s->OnSend(0);
                }
            }
            if (!IsLive(s) || s->m_hSocket == INVALID_SOCKET) continue;  // callback may have torn it down
            fd = s->m_hSocket;   // re-read: a callback may have closed/reopened this slot, so never reuse the stale fd

            if (FD_ISSET(fd, &rd)) {
                if (s->m_state == kListening) {
                    // A pending inbound connection makes a listening socket
                    // readable — hand off to OnAccept (which calls Accept()).
                    // This is how CChatListenSocket receives incoming MUD chat
                    // calls; without it the listen fd would be recv()'d below
                    // and the connection silently dropped.
                    s->OnAccept(0);
                } else {
                    // Peek to distinguish data vs. EOF
                    char peek;
                    int n = (int)::recv(fd, &peek, 1, MSG_PEEK | MSG_DONTWAIT);
                    if (n == 0) {
                        // EOF — notify and close.  Without Close() the fd stays
                        // readable-with-EOF on every subsequent poll, firing OnClose
                        // again and again until the fd is actually closed.
                        s->OnClose(0);
                        if (IsLive(s) && s->m_hSocket != INVALID_SOCKET)
                            s->Close();
                    } else {
                        s->OnReceive(0);
                    }
                }
            }
            if (!IsLive(s) || s->m_hSocket == INVALID_SOCKET) continue;  // callback may have torn it down
            fd = s->m_hSocket;   // re-read: a callback may have closed/reopened this slot, so never reuse the stale fd

            if (FD_ISSET(fd, &ex)) {
                s->OnReceive(MSG_OOB);  // out-of-band data
            }
        }
    }

private:
    enum State { kIdle, kConnecting, kConnected, kListening };
    State m_state  = kIdle;
    long  m_events = FD_READ | FD_WRITE | FD_OOB | FD_ACCEPT | FD_CONNECT | FD_CLOSE;

    // ── Static registry of all live sockets ──────────────────────────────────
    // Uses a function-local static to avoid static-initialisation order issues.
    static std::vector<CAsyncSocket*>& AllSockets()
    {
        static std::vector<CAsyncSocket*> v;
        return v;
    }

    static void Register(CAsyncSocket* s)
    {
        auto& v = AllSockets();
        for (auto* p : v) if (p == s) return;
        v.push_back(s);
    }
    static void Unregister(CAsyncSocket* s)
    {
        auto& v = AllSockets();
        v.erase(std::remove(v.begin(), v.end(), s), v.end());
    }
    // True iff `s` is still a live, registered socket.  A callback may delete a
    // socket (the destructor Unregisters it), so Poll() uses this to avoid
    // dereferencing a pointer in its snapshot that has since been freed.
    static bool IsLive(CAsyncSocket* s)
    {
        auto& v = AllSockets();
        return std::find(v.begin(), v.end(), s) != v.end();
    }
};

// ── ShutDownSocket helper ─────────────────────────────────────────────────────
// Defined in Utilities.cpp on Windows; declared here so worldsock.cpp can call it.
// The implementation lives in utilities_portable.cpp on macOS.
void ShutDownSocket(CAsyncSocket& s);

#endif  // POSIX_SOCKET_H
#endif  // !_WIN32
