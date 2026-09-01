// platform/posix_socket.h
//
// POSIX implementation of MFC's CAsyncSocket.
//
// On Windows, CAsyncSocket is part of MFC and uses WSAAsyncSelect() to deliver
// socket events (FD_READ, FD_WRITE, FD_CONNECT, FD_CLOSE) as Windows messages.
//
// On macOS/Linux we provide a compatible class backed by POSIX sockets.
// Event dispatch is triggered by calling CAsyncSocket::Poll() from the
// application's main loop — it runs poll(2) (read/write/accept/connect) plus
// select(2) (out-of-band data only — see the comment in Poll()) with a short
// timeout and calls the appropriate On* virtual method for each socket that
// has a pending event.
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
#include <sys/ioctl.h>   // FIONREAD (the Poll() drain loop's progress check)
#include <poll.h>
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
#include <utility>     // std::pair (Poll snapshot)

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
    // Creates a non-blocking socket and BINDS it, exactly as MFC's
    // CAsyncSocket::Create does (Socket() then Bind(nSocketPort,
    // lpszSocketAddress), returning FALSE if the bind fails).
    // nSocketType is SOCK_STREAM or SOCK_DGRAM.
    //
    // The bind used to be missing here, with a comment claiming the port was
    // "ignored (bound by OS at connect time)".  That is true for an OUTBOUND
    // socket and false for a server one, and it silently broke both server
    // paths:
    //   * world.ChatAcceptCalls (scripting/methods/methods_chat.cpp) passes the
    //     configured chat port to Create() and then calls Listen().  listen(2)
    //     on an unbound TCP socket succeeds on BSD/macOS — the kernel just
    //     auto-assigns an ephemeral port — so the world cheerfully reported
    //     "Listening for chat connections on port 4050" and advertised 4050 in
    //     the CHAT handshake while actually listening on a random high port.
    //     Nothing could ever call in.
    //   * world.UdpListen (methods_udp.cpp) passes both a port and a local IP,
    //     and lost both, leaving the socket on port 0 where no datagram arrives.
    //
    // lEvent is still accepted and ignored: this shim is Poll()-driven and
    // dispatches every event type regardless of the requested mask.
    BOOL Create(UINT nSocketPort     = 0,
                int  nSocketType     = SOCK_STREAM,
                long /*lEvent*/      = FD_READ | FD_WRITE | FD_OOB |
                                       FD_ACCEPT | FD_CONNECT | FD_CLOSE,
                LPCSTR lpszSocketAddress = nullptr)
    {
        if (m_hSocket != INVALID_SOCKET) Close();

        m_hSocket = ::socket(AF_INET, nSocketType, 0);
        if (m_hSocket == INVALID_SOCKET) return FALSE;
        m_sockType = nSocketType;

        // Don't let a write to a peer-closed socket raise SIGPIPE and kill the
        // process.  macOS: per-socket SO_NOSIGPIPE; Linux relies on the global
        // signal(SIGPIPE, SIG_IGN) installed at startup (send() returns EPIPE,
        // which the world socket already treats as a disconnect).
#ifdef SO_NOSIGPIPE
        { int on = 1; ::setsockopt(m_hSocket, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on)); }
#endif

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

        // MFC binds unconditionally — port 0 with no address just asks the
        // kernel for an ephemeral port, which is what an outbound socket wants
        // anyway, so this is safe for the connect-side callers too.
        if (!Bind(nSocketPort, lpszSocketAddress)) {
            int saved = errno;
            Close();
            errno = saved;
            return FALSE;
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
        // Same per-socket setup Create() does, because accept(2) does NOT
        // inherit either of these from the listening socket.
        //
        // SO_NOSIGPIPE is per-file-descriptor on macOS/BSD, so an accepted
        // socket started out unprotected: writing to it after the peer went
        // away raised SIGPIPE and killed the process.  The GUI app happened to
        // survive that only because platform/wx_app.cpp installs a global
        // signal(SIGPIPE, SIG_IGN) at startup -- anything embedding this shim
        // without that (every test binary) was exposed, and the shim was
        // internally inconsistent.  The reachable product path is
        // world.ChatAcceptCalls, whose incoming connections all arrive here.
#ifdef SO_NOSIGPIPE
        { int on = 1; ::setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on)); }
#endif
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
    // MFC has this as a separate no-op virtual, and CWorldSocket never
    // overrode it -- urgent data is simply ignored.  Routing it into
    // OnReceive instead (what this shim used to do) turned a telnet SYNCH
    // into a disconnect: with SO_OOBINLINE off the urgent byte is NOT in the
    // normal stream, so the recv that followed returned EWOULDBLOCK, which
    // Receive reports as SOCKET_ERROR, which the transport treats as the
    // connection closing.
    virtual void OnOutOfBandData (int /*nErrorCode*/) {}

    // ── Polling ───────────────────────────────────────────────────────────────
    // Call this from the application event loop.  It checks all registered
    // sockets for pending events (up to timeout_ms milliseconds) and calls
    // the appropriate On* callback.
    static void Poll(int timeout_ms = 10)
    {
        auto& s_sockets = AllSockets();
        if (s_sockets.empty()) return;

        // poll(2), unlike select(2)'s fd_set, has no FD_SETSIZE-style ceiling
        // on the descriptor number, so it replaces select(2) for the ordinary
        // read/write/accept/connect events below -- in a long, many-world+
        // plugin+db+log session the process fd count can exceed 1024, and a
        // socket fd past that point is now polled correctly.
        //
        // Out-of-band (urgent) data stays on select(2)'s exceptfds: measured
        // on this platform, poll(2)'s POLLPRI never fires for TCP urgent data
        // at all -- a lone MSG_OOB byte left poll() timed out with revents ==
        // 0, while select()'s exceptfds saw it immediately.  A poll()-only
        // rewrite would silently break telnet SYNCH handling (see the OOB
        // test below).  That narrow path keeps the FD_SETSIZE guard it always
        // had, so the ceiling this fixes only remains for a socket that is
        // BOTH >= 1024 AND receiving urgent data -- doubly rare, and no
        // narrower than before this change.
        std::vector<struct pollfd> fds;
        fds.reserve(s_sockets.size());

        fd_set ex;
        FD_ZERO(&ex);
        int maxfd = -1;

        for (CAsyncSocket* s : s_sockets) {
            if (s->m_hSocket == INVALID_SOCKET) continue;
            int fd = s->m_hSocket;
            short events = 0;
            if (s->m_events & (FD_READ  | FD_CLOSE | FD_ACCEPT)) events |= POLLIN;
            if (s->m_events & (FD_WRITE | FD_CONNECT)) events |= POLLOUT;
            if (events != 0) fds.push_back({ fd, events, 0 });

            if ((s->m_events & FD_OOB) && fd < FD_SETSIZE) {
                FD_SET(fd, &ex);
                if (fd > maxfd) maxfd = fd;
            }
        }
        if (fds.empty() && maxfd < 0) return;

        bool ready = false;
        if (!fds.empty()) {
            ready = ::poll(fds.data(), fds.size(), timeout_ms) > 0;
            if (maxfd >= 0) {
                // poll() already did the waiting above, so this is just a
                // non-blocking peek at the urgent-data set -- a real wait
                // here would double the worst-case latency of every call.
                struct timeval tv{0, 0};
                if (::select(maxfd + 1, nullptr, nullptr, &ex, &tv) > 0) ready = true;
            }
        } else {
            // No socket wants ordinary read/write events -- only OOB is
            // being watched (not something any caller does today, but kept
            // correct) -- so select() alone does the waiting, exactly as
            // before this change.
            struct timeval tv{ timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
            ready = ::select(maxfd + 1, nullptr, nullptr, &ex, &tv) > 0;
        }
        if (!ready) return;

        // Look up the revents poll(2) reported for a given fd -- the
        // replacement for FD_ISSET(fd, &rd/&wr) against the old fd_sets.
        auto revents_for = [&fds](int fd) -> short {
            for (auto& p : fds) if (p.fd == fd) return p.revents;
            return 0;
        };

        // Take a snapshot: callbacks may modify s_sockets — and may even DELETE
        // a socket (e.g. a TLS certificate-verification failure tears the
        // connection down from inside OnReceive, freeing the CAsyncSocket).
        // Re-check IsLive(s) after every callback so we never dereference a
        // pointer in the snapshot that a callback has since freed/unregistered.
        std::vector<std::pair<CAsyncSocket*, unsigned long>> snap;
        snap.reserve(s_sockets.size());
        for (CAsyncSocket* s : s_sockets) snap.push_back({s, s->m_generation});

        for (auto& pr : snap) {
            CAsyncSocket* s = pr.first;
            const unsigned long gen = pr.second;
            if (!IsLive(s, gen) || s->m_hSocket == INVALID_SOCKET) continue;
            int fd = s->m_hSocket;

            // A refused/failed non-blocking connect is where poll() and
            // select() part ways: select()'s writefds marks an errored
            // socket ready to write (a write would return the error without
            // blocking), but poll() on this platform reports the same
            // condition as POLLHUP (sometimes POLLERR), NOT POLLOUT --
            // measured directly: a refused loopback connect produced
            // revents == POLLIN|POLLHUP, no POLLOUT bit at all.  POLLERR and
            // POLLHUP are always reported when they occur regardless of the
            // requested event mask, so checking for them here costs nothing
            // on the ordinary POLLOUT path and is what makes the SO_ERROR
            // check below actually run for a refused connect.
            if (revents_for(fd) & (POLLOUT | POLLERR | POLLHUP)) {
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
            if (!IsLive(s, gen) || s->m_hSocket == INVALID_SOCKET) continue;  // callback may have torn it down
            fd = s->m_hSocket;   // re-read: a callback may have closed/reopened this slot, so never reuse the stale fd

            if (revents_for(fd) & POLLIN) {
                if (s->m_state == kListening) {
                    // A pending inbound connection makes a listening socket
                    // readable — hand off to OnAccept (which calls Accept()).
                    // This is how CChatListenSocket receives incoming MUD chat
                    // calls; without it the listen fd would be recv()'d below
                    // and the connection silently dropped.
                    s->OnAccept(0);
                } else if (s->m_sockType == SOCK_DGRAM) {
                    // SOCK_DGRAM has no concept of a peer closing the
                    // connection -- a zero-length UDP datagram is a legitimate
                    // (if unusual) piece of data, not EOF, and peeks
                    // indistinguishably from the TCP EOF case below.  Skipping
                    // the peek entirely and always calling OnReceive is what
                    // keeps world.UdpListen alive across one.
                    s->OnReceive(0);
                } else {
                    // Peek to distinguish data vs. EOF
                    char peek;
                    int n = (int)::recv(fd, &peek, 1, MSG_PEEK | MSG_DONTWAIT);
                    if (n == 0) {
                        // EOF — notify and close.  Without Close() the fd stays
                        // readable-with-EOF on every subsequent poll, firing OnClose
                        // again and again until the fd is actually closed.
                        s->OnClose(0);
                        if (IsLive(s, gen) && s->m_hSocket != INVALID_SOCKET)
                            s->Close();
                    } else {
                        // Drain the backlog inside THIS pass, the way Winsock
                        // does.  FD_READ is edge-triggered on recv(): the socket
                        // window re-records and re-posts it whenever recv() is
                        // called and data still remains, so a Windows message
                        // loop consumes a burst within one turn.  Here the only
                        // pump is a 20 ms wxTimer (wx_mfc_impl.cpp), so one
                        // OnReceive per pass was a throughput ceiling Windows
                        // does not have: the world socket reads 9999 bytes per
                        // call (posix_transport.cpp) and a chat socket 999, i.e.
                        // ~500 KB/s and ~50 KB/s at 50 passes a second.
                        //
                        // The re-arm condition is Winsock's, and it is NOT
                        // "there is still data": the callback must have CONSUMED
                        // something.  A callback that is notified and reads
                        // nothing gets no FD_READ re-post on Windows either, and
                        // demanding progress here is what stops such a callback
                        // burning the whole budget on every pass.  FIONREAD is
                        // the only instrument that can tell the two apart --
                        // MSG_PEEK answers "is there data", never "did you take
                        // any".
                        //
                        // The budget bounds one busy socket's share of a pass so
                        // it cannot starve the other sockets or the UI: Poll()
                        // runs on the GUI thread.
                        constexpr int kMaxReceivesPerPass = 64;
                        for (int calls = 0; ; ) {
                            int before = 0;
                            if (::ioctl(fd, FIONREAD, &before) != 0)
                                before = -1;   // unmeasurable: one call, as before

                            s->OnReceive(0);
                            ++calls;

                            if (calls >= kMaxReceivesPerPass) break;
                            if (before <= 0) break;
                            // A callback may have closed, freed or reopened the
                            // slot — same discipline as the arms around this one.
                            if (!IsLive(s, gen) || s->m_hSocket == INVALID_SOCKET)
                                break;
                            fd = s->m_hSocket;
                            int after = 0;
                            if (::ioctl(fd, FIONREAD, &after) != 0) break;
                            if (after <= 0)     break;   // nothing left to drain
                            if (after >= before) break;  // consumed nothing
                        }
                    }
                }
            }
            if (!IsLive(s, gen) || s->m_hSocket == INVALID_SOCKET) continue;  // callback may have torn it down
            fd = s->m_hSocket;   // re-read: a callback may have closed/reopened this slot, so never reuse the stale fd

            if (fd < FD_SETSIZE && FD_ISSET(fd, &ex)) {
                s->OnOutOfBandData(0);
                // Then consume the urgent byte.  Winsock's FD_OOB is
                // edge-triggered -- it fires once per arrival -- but select()
                // is LEVEL-triggered, so an undrained urgent byte would mark
                // this socket in exceptfds on every poll from now on, spinning
                // the callback ~50x a second.  Discarding matches the effective
                // Windows behaviour: with SO_OOBINLINE off the byte is not part
                // of the normal stream there either, so the telnet phase
                // machine never sees it on either platform.
                if (IsLive(s, gen) && s->m_hSocket != INVALID_SOCKET) {
                    char oob;
                    (void) ::recv(s->m_hSocket, &oob, 1, MSG_OOB);
                }
            }
        }
    }

private:
    enum State { kIdle, kConnecting, kConnected, kListening };
    State m_state  = kIdle;
    long  m_events = FD_READ | FD_WRITE | FD_OOB | FD_ACCEPT | FD_CONNECT | FD_CLOSE;
    int   m_sockType = SOCK_STREAM;  // set by Create(); Poll() needs this to tell
                                      // a peer-closed TCP stream from a legitimate
                                      // zero-length UDP datagram (both peek as 0
                                      // bytes).  Accepted sockets never change
                                      // this from the SOCK_STREAM default -- accept(2)
                                      // only ever hands back stream sockets.
    unsigned long m_generation = 0;   // bumped on each Register; lets Poll detect
                                      // an ABA reuse of a freed socket's address

    // ── Static registry of all live sockets ──────────────────────────────────
    // Uses a function-local static to avoid static-initialisation order issues.
    static std::vector<CAsyncSocket*>& AllSockets()
    {
        static std::vector<CAsyncSocket*> v;
        return v;
    }

    static unsigned long& GenCounter() { static unsigned long g = 0; return g; }

    static void Register(CAsyncSocket* s)
    {
        auto& v = AllSockets();
        for (auto* p : v) if (p == s) return;
        s->m_generation = ++GenCounter();   // unique tag for this registration
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
    // As IsLive, but also require the registration generation to match the one
    // captured in Poll's snapshot, so a freed socket whose heap address is reused
    // by a new socket within the same poll pass is correctly treated as gone.
    static bool IsLive(CAsyncSocket* s, unsigned long gen)
    {
        auto& v = AllSockets();
        auto it = std::find(v.begin(), v.end(), s);
        return it != v.end() && (*it)->m_generation == gen;
    }
};

// ── ShutDownSocket helper ─────────────────────────────────────────────────────
// Defined in Utilities.cpp on Windows; declared here so worldsock.cpp can call it.
// The implementation lives in utilities_portable.cpp on macOS.
void ShutDownSocket(CAsyncSocket& s);

#endif  // POSIX_SOCKET_H
#endif  // !_WIN32
