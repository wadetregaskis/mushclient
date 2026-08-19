// tests/test_posix_socket.cpp
// Intent-comment tests for the POSIX CAsyncSocket shim
// (platform/posix_socket.h): the Poll() snapshot/IsLive machinery, EOF
// close-once semantics, listening-socket OnAccept routing, non-blocking
// connect errno normalisation, and SIGPIPE suppression.
//
// These use real loopback sockets (bind to 127.0.0.1 port 0).

#include "mc_test.h"
#include "platform/windows_types.h"
#include "platform/posix_socket.h"

#include <string>

// ── helpers ───────────────────────────────────────────────────────────────

// A socket that records its callbacks.
struct Recorder : CAsyncSocket {
    int receives = 0, closes = 0, accepts = 0, connects = 0, oobs = 0;
    void OnReceive(int) override { ++receives; }
    void OnClose(int) override   { ++closes; }
    void OnAccept(int) override  { ++accepts; }
    void OnConnect(int) override { ++connects; }
    void OnOutOfBandData(int) override { ++oobs; }
};

// A listening socket on 127.0.0.1:<ephemeral>; port() reports the port.
struct Listener : Recorder {
    UINT port_ = 0;
    bool open(UINT wantPort = 0)
    {
        // MFC style: Create() itself binds, so do NOT call Bind() afterwards —
        // a second bind(2) on the same socket fails with EINVAL.
        if (!Create(wantPort, SOCK_STREAM,
                    FD_READ | FD_WRITE | FD_OOB |
                    FD_ACCEPT | FD_CONNECT | FD_CLOSE,
                    "127.0.0.1"))
            return false;
        SOCKADDR_IN addr{}; int len = sizeof addr;
        if (!GetSockName((SOCKADDR*)&addr, &len)) return false;
        port_ = ntohs(addr.sin_port);
        return Listen(5) != FALSE;
    }
};

// Poll until pred() or ~2 s passes.
template <typename F>
static bool poll_until(F pred)
{
    for (int i = 0; i < 200 && !pred(); i++)
        CAsyncSocket::Poll(10);
    return pred();
}

// ── tests ─────────────────────────────────────────────────────────────────

// posix_socket.h Create: "Creates a non-blocking socket and BINDS it,
// exactly as MFC's CAsyncSocket::Create does".  Create() used to drop
// nSocketPort entirely, so a server socket ended up on a kernel-assigned
// ephemeral port -- world.ChatAcceptCalls announced the configured chat
// port while listening somewhere else, and world.UdpListen sat on port 0.
TEST(PosixSocket, CreateBindsTheRequestedPortAndAddress)
{
    // Take an ephemeral port, note it, then release it so we can ask for
    // that exact number back.
    UINT wanted = 0;
    {
        Listener probe;
        ASSERT_TRUE(probe.open());
        wanted = probe.port_;
        probe.Close();
    }
    ASSERT_TRUE(wanted != 0);

    Listener lis;
    ASSERT_TRUE(lis.open(wanted));
    // The whole point: the socket is on the port we asked for, not a
    // kernel-chosen one.
    EXPECT_EQ(wanted, lis.port_);

    // And it is really reachable there -- an unbound listen() would have
    // succeeded too, which is exactly why this bug was invisible.
    Recorder client;
    ASSERT_TRUE(client.Create());
    client.Connect("127.0.0.1", wanted);
    EXPECT_TRUE(poll_until([&] { return lis.accepts > 0; }));
    client.Close();
    lis.Close();
}

// posix_socket.h Connect: "errno = EWOULDBLOCK; // normalise to
// WSAEWOULDBLOCK for callers — Non-blocking connect. Returns FALSE with
// WSAEWOULDBLOCK when in progress (which is normal)".
TEST(PosixSocket, InProgressConnectNormalisesErrnoToWouldBlock)
{
    Listener lis;
    ASSERT_TRUE(lis.open());

    Recorder client;
    ASSERT_TRUE(client.Create());
    BOOL rc = client.Connect("127.0.0.1", lis.port_);
    if (rc) {
        // Immediate loopback connect: nothing to normalise on this kernel.
        fprintf(stderr, "  (connect completed immediately; skipping)\n");
    } else {
        EXPECT_EQ(WSAEWOULDBLOCK, CAsyncSocket::GetLastError());
        // and the connect genuinely completes afterwards
        EXPECT_TRUE(poll_until([&] { return client.connects > 0; }));
    }
    client.Close();
    lis.Close();
}

// posix_socket.h Poll: "A pending inbound connection makes a listening
// socket readable — hand off to OnAccept (which calls Accept())" — never
// OnReceive/OnClose on the listen fd, and Accept() yields a usable socket.
TEST(PosixSocket, PendingConnectionFiresOnAcceptNotOnReceive)
{
    Listener lis;
    ASSERT_TRUE(lis.open());

    Recorder client;
    ASSERT_TRUE(client.Create());
    client.Connect("127.0.0.1", lis.port_);

    ASSERT_TRUE(poll_until([&] { return lis.accepts > 0; }));
    EXPECT_EQ(0, lis.receives);              // listen fd never recv()'d
    EXPECT_EQ(0, lis.closes);

    Recorder server;
    EXPECT_TRUE(lis.Accept(server));         // yields a connected socket
    ASSERT_TRUE(server.m_hSocket != INVALID_SOCKET);

    // the accepted socket really is wired to the client
    ASSERT_TRUE(poll_until([&] { return client.connects > 0 ||
                                        client.m_hSocket != INVALID_SOCKET; }));
    EXPECT_EQ(4, client.Send("ping", 4));
    ASSERT_TRUE(poll_until([&] { return server.receives > 0; }));
    char buf[8] = {};
    EXPECT_EQ(4, server.Receive(buf, sizeof buf));
    EXPECT_STREQ("ping", buf);

    server.Close();
    client.Close();
    lis.Close();
}

// posix_socket.h Poll: "EOF — notify and close. Without Close() the fd
// stays readable-with-EOF on every subsequent poll, firing OnClose again
// and again until the fd is actually closed."
TEST(PosixSocket, EofFiresOnCloseOnceThenSocketIsClosed)
{
    Listener lis;
    ASSERT_TRUE(lis.open());
    Recorder client;
    ASSERT_TRUE(client.Create());
    client.Connect("127.0.0.1", lis.port_);
    ASSERT_TRUE(poll_until([&] { return lis.accepts > 0; }));
    Recorder server;
    ASSERT_TRUE(lis.Accept(server));

    client.Close();                          // peer EOF for `server`

    ASSERT_TRUE(poll_until([&] { return server.closes > 0; }));
    EXPECT_EQ(1, server.closes);             // exactly one OnClose
    EXPECT_TRUE(server.m_hSocket == INVALID_SOCKET);   // auto-closed

    for (int i = 0; i < 10; i++) CAsyncSocket::Poll(1);
    EXPECT_EQ(1, server.closes);             // never fires again

    lis.Close();
}

// posix_socket.h Poll: the readable-event handler's MSG_PEEK-returns-0-means-
// EOF heuristic is TCP-specific.  A SOCK_DGRAM socket (world.UdpListen) has no
// concept of a peer closing the connection, and a legitimate zero-length UDP
// datagram peeks as 0 bytes exactly like TCP EOF — Poll() used to treat it as
// the peer going away and permanently close the listening socket.
namespace {
struct UdpRecorder : CAsyncSocket {
    int receives = 0, closes = 0;
    std::string last;
    void OnReceive(int) override
    {
        ++receives;
        char buf[64] = {};
        int n = Receive(buf, sizeof buf);
        last = (n > 0) ? std::string(buf, n) : std::string();
    }
    void OnClose(int) override { ++closes; }
};
} // namespace

TEST(PosixSocket, ZeroLengthUdpDatagramIsDataNotEof)
{
    UdpRecorder udp;
    ASSERT_TRUE(udp.Create(0, SOCK_DGRAM, FD_READ | FD_WRITE, "127.0.0.1"));
    SOCKADDR_IN addr{}; int len = sizeof addr;
    ASSERT_TRUE(udp.GetSockName((SOCKADDR*)&addr, &len));
    UINT port = ntohs(addr.sin_port);

    SOCKET sender = ::socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_TRUE(sender != INVALID_SOCKET);
    SOCKADDR_IN dst{};
    dst.sin_family      = AF_INET;
    dst.sin_port        = htons((uint16_t)port);
    dst.sin_addr.s_addr = inet_addr("127.0.0.1");

    // A zero-length datagram used to be indistinguishable from TCP EOF and
    // made Poll() close the socket on it.
    EXPECT_EQ(0, (int)::sendto(sender, "", 0, 0, (SOCKADDR*)&dst, sizeof dst));
    ASSERT_TRUE(poll_until([&] { return udp.receives > 0; }));
    EXPECT_EQ(1, udp.receives);
    EXPECT_EQ(0, udp.closes);
    ASSERT_TRUE(udp.m_hSocket != INVALID_SOCKET);   // socket survives

    // ...and the listener keeps working afterwards: a following non-empty
    // datagram is still delivered with the right bytes.
    EXPECT_EQ(4, (int)::sendto(sender, "ping", 4, 0, (SOCKADDR*)&dst, sizeof dst));
    ASSERT_TRUE(poll_until([&] { return udp.receives > 1; }));
    EXPECT_EQ(2, udp.receives);
    EXPECT_EQ("ping", udp.last);

    ::close(sender);
    udp.Close();
}

// posix_socket.h Poll: "callbacks may modify s_sockets — and may even
// DELETE a socket ... Re-check IsLive(s) after every callback so we never
// dereference a pointer in the snapshot that a callback has since freed."
namespace {
bool g_victim_fired = false;
struct Victim : CAsyncSocket {
    void OnReceive(int) override { g_victim_fired = true; }
    void OnClose(int) override   { g_victim_fired = true; }
};
struct Killer : CAsyncSocket {
    CAsyncSocket** ppVictim = nullptr;
    void OnReceive(int) override
    {
        char buf[16];
        Receive(buf, sizeof buf);
        if (ppVictim && *ppVictim) { delete *ppVictim; *ppVictim = nullptr; }
    }
    void OnClose(int) override {}
};
} // namespace

TEST(PosixSocket, CallbackDeletingSocketDuringPollDoesNotFireFreedSocket)
{
    Listener lis;
    ASSERT_TRUE(lis.open());

    // two client connections; the server side of the FIRST (registered
    // earlier, so processed earlier in Poll's snapshot) deletes the second.
    Recorder c1, c2;
    ASSERT_TRUE(c1.Create());
    c1.Connect("127.0.0.1", lis.port_);
    ASSERT_TRUE(poll_until([&] { return lis.accepts >= 1; }));
    Killer* killer = new Killer();
    ASSERT_TRUE(lis.Accept(*killer));

    ASSERT_TRUE(c2.Create());
    c2.Connect("127.0.0.1", lis.port_);
    ASSERT_TRUE(poll_until([&] { return lis.accepts >= 2; }));
    CAsyncSocket* victim = new Victim();
    ASSERT_TRUE(lis.Accept(*victim));
    killer->ppVictim = &victim;

    g_victim_fired = false;
    // make BOTH server sockets readable in the same Poll pass
    EXPECT_EQ(1, c1.Send("a", 1));
    EXPECT_EQ(1, c2.Send("b", 1));
    for (int i = 0; i < 50 && victim != nullptr; i++)
        CAsyncSocket::Poll(10);

    EXPECT_TRUE(victim == nullptr);          // the killer really ran
    EXPECT_FALSE(g_victim_fired);            // freed socket never fired

    delete killer;
    c1.Close();
    c2.Close();
    lis.Close();
}

// posix_socket.h Create: "Don't let a write to a peer-closed socket raise
// SIGPIPE and kill the process" — send() reports an error instead.
TEST(PosixSocket, SendAfterPeerCloseReturnsErrorNotSigpipe)
{
    Listener lis;
    ASSERT_TRUE(lis.open());
    Recorder client;
    ASSERT_TRUE(client.Create());
    client.Connect("127.0.0.1", lis.port_);
    ASSERT_TRUE(poll_until([&] { return lis.accepts > 0; }));
    Recorder server;
    ASSERT_TRUE(lis.Accept(server));

    // Make the peer go away with an RST, not a graceful FIN.  A plain close
    // sends FIN, and writes after a FIN are still accepted into the local send
    // buffer -- they only fail once the peer's stack answers with RST, which on
    // loopback is not guaranteed to happen within any bounded number of small
    // writes.  That is why this test used to fail about 14 runs in 25.
    // SO_LINGER with a zero timeout makes close(2) send RST immediately, so the
    // client's next write fails deterministically, which is what the test is
    // actually about: an error return rather than a SIGPIPE death.
    {
        struct linger lg;
        lg.l_onoff  = 1;
        lg.l_linger = 0;
        setsockopt(server.m_hSocket, SOL_SOCKET, SO_LINGER, &lg, sizeof lg);
    }
    server.Close();                          // peer side goes away (RST)

    // Keep writing until the failure surfaces; the point of the test is that
    // the process SURVIVES (an error return, not a SIGPIPE death).
    //
    // Timing note: closing the peer only sends FIN.  Writes after a FIN are
    // still accepted into the local send buffer and only fail once the peer's
    // stack answers with RST, so the error is not immediate.  This loop used
    // to be 50 writes of 4 bytes with Poll(1) between them -- ~200 bytes and
    // ~50ms, far short of both the socket buffer and the RST round trip, so on
    // a loopback socket every write could succeed and the test failed roughly
    // one run in three.  Give it the same budget as poll_until (200 x 10ms)
    // and it fails only if the error genuinely never arrives.
    int rc = 0;
    for (int i = 0; i < 200; i++) {
        rc = client.Send("xxxx", 4);
        if (rc < 0) break;
        CAsyncSocket::Poll(10);
    }
    EXPECT_LT(rc, 0);                        // an error, not a dead process

    client.Close();
    lis.Close();
}

#ifdef SO_NOSIGPIPE
// posix_socket.h Accept: the accepted socket gets "the same per-socket setup
// Create() does, because accept(2) does NOT inherit either of these from the
// listening socket".
//
// SO_NOSIGPIPE is per-descriptor on macOS/BSD.  Accept() used to set only
// O_NONBLOCK, so every accepted socket -- i.e. every incoming chat connection
// via world.ChatAcceptCalls -- could kill the process with SIGPIPE on a write
// to a departed peer.  The app masked it with a global signal(SIGPIPE, SIG_IGN)
// in platform/wx_app.cpp, so this was invisible in the GUI and only showed up
// as an intermittent test-runner death.
//
// Asserted with getsockopt rather than by provoking a real SIGPIPE, because
// the test runner now ignores the signal (mc_test.h) and so could not observe
// the difference.
TEST(PosixSocket, AcceptedSocketGetsNoSigpipeLikeCreatedOne)
{
    Listener lis;
    ASSERT_TRUE(lis.open());
    Recorder client;
    ASSERT_TRUE(client.Create());
    client.Connect("127.0.0.1", lis.port_);
    ASSERT_TRUE(poll_until([&] { return lis.accepts > 0; }));

    Recorder server;
    ASSERT_TRUE(lis.Accept(server));
    ASSERT_TRUE(server.m_hSocket != INVALID_SOCKET);

    int on = 0; socklen_t len = sizeof on;
    EXPECT_EQ(0, ::getsockopt(server.m_hSocket, SOL_SOCKET, SO_NOSIGPIPE, &on, &len));
    EXPECT_NE(0, on);                        // accepted socket is protected

    // ...and the created socket still is, so the two paths agree.
    on = 0; len = sizeof on;
    EXPECT_EQ(0, ::getsockopt(client.m_hSocket, SOL_SOCKET, SO_NOSIGPIPE, &on, &len));
    EXPECT_NE(0, on);

    server.Close();
    client.Close();
    lis.Close();
}
#endif

// ── out-of-band data ──────────────────────────────────────────────────────
//
// MFC routes urgent data to its own no-op virtual, OnOutOfBandData.  This shim
// used to send it to OnReceive(MSG_OOB) instead, and the transport's OnReceive
// ignores its argument -- so an ordinary recv() ran with nothing normal to
// read, returned EWOULDBLOCK, which Receive reports as SOCKET_ERROR, which the
// transport treats as the connection closing.  A MUD sending a telnet SYNCH
// (IAC DM as urgent data, the standard reply to IP/AO/AYT) disconnected the
// world mid-session.
TEST(PosixSocket, UrgentDataGoesToOnOutOfBandDataNotOnReceive)
{
    Listener lis;
    ASSERT_TRUE(lis.open());
    Recorder client;
    ASSERT_TRUE(client.Create());
    client.Connect("127.0.0.1", lis.port_);
    ASSERT_TRUE(poll_until([&] { return lis.accepts > 0; }));
    Recorder server;
    ASSERT_TRUE(lis.Accept(server));
    ASSERT_TRUE(poll_until([&] { return client.connects > 0; }));

    const int before = server.receives;

    // One byte of TCP urgent data, and nothing in the normal stream — exactly
    // the shape that made recv() return EWOULDBLOCK.
    ASSERT_EQ(1, (int)::send(client.m_hSocket, "!", 1, MSG_OOB));

    ASSERT_TRUE(poll_until([&] { return server.oobs > 0; }));
    EXPECT_EQ(0, server.closes);              // the world stays connected
    EXPECT_EQ(before, server.receives);       // and it is NOT a normal read

    // The urgent byte is drained, so select() stops reporting the condition:
    // without that, level-triggered select fires this callback on every poll.
    const int settled = server.oobs;
    for (int i = 0; i < 20; i++) CAsyncSocket::Poll(1);
    EXPECT_EQ(settled, server.oobs);

    // Normal traffic still flows afterwards.
    ASSERT_EQ(2, (int)::send(client.m_hSocket, "hi", 2, 0));
    EXPECT_TRUE(poll_until([&] { return server.receives > before; }));
    EXPECT_EQ(0, server.closes);

    client.Close();
    lis.Close();
}

MC_TEST_MAIN()
