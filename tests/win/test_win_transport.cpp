// tests/win/test_win_transport.cpp — the native Windows pal::ITransport
// adapter, run for the first time.
//
// `pal/win_transport.h` says, of itself:
//
//     CANNOT be compiled or run in the macOS port environment: correctness is
//     by-construction/reasoning (each mechanic is the exact original call
//     sequence), the same discipline as pal/gdi_surface.cpp.  Windows CI is
//     the eventual empirical arbiter.
//
// This is that arbiter.  The file replaced the shipping Windows network path —
// `CWorldSocket` (worldsock.cpp), `CMUSHclientDoc::ConnectSocket`, and the
// `LookupHostName` / `HostNameResolved` pair — so every world connection on
// Windows goes through code that until now had been reasoned about and never
// executed.  It is also the least forgiving place for that: a transport that
// neither connects nor reports a failure is a client that hangs on connect,
// and no unit test elsewhere in the suite touches it.
//
// Everything here runs over 127.0.0.1 against a listener this file creates, so
// no network, no MUD, and no fixture server.  `tests/mud_stub.py` is the tree's
// loopback server and is deliberately NOT used: it is a separate process, and
// what these tests need is a socket they can drive byte by byte from inside the
// assertion.
//
// ── Why a message pump ─────────────────────────────────────────────────────
// The adapter is `WSAAsyncSelect`-driven by design (that is the mechanic it
// relocated), so every event — connect, readable, writable, close — arrives as
// a window message to MFC's socket window, and DNS completions arrive at the
// adapter's own message-only window.  Nothing is delivered until somebody
// pumps.  `Pump` below is that somebody, with a deadline, so a lost event is a
// failed assertion after N milliseconds rather than a hung suite.
//
// ── What is NOT covered, and why ───────────────────────────────────────────
//   * TLS.  `startTls` wants a TLS server with a certificate chain; a
//     self-signed loopback server would exercise the failure arm and not the
//     success one, which is the arm that matters.  Left out deliberately
//     rather than half-done.
//   * Keepalives.  `connect` applies SIO_KEEPALIVE_VALS to its own socket when
//     `options.keepalives` is set, and there is no getter for it and no way to
//     reach the adapter's private SOCKET from out here.  `SO_KEEPALIVE` would
//     be observable if the handle were, so this is a visibility gap rather
//     than an untestable mechanic.

#include "mc_test.h"

#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "pal/itransport.h"
#include "pal/win_transport.h"

#include <string>
#include <vector>

namespace {

// ── Pump ───────────────────────────────────────────────────────────────────
// Dispatch queued messages until `done` is true or the deadline passes.
// Returns whether the condition came true; every caller asserts on that rather
// than on a bare timeout, so a lost event names itself.
template <class Done>
bool Pump(Done done, int timeoutMs = 5000)
{
    const DWORD deadline = ::GetTickCount() + (DWORD) timeoutMs;
    for (;;) {
        MSG msg;
        while (::PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
        }
        if (done())
            return true;
        if (::GetTickCount() >= deadline)
            return false;
        ::Sleep(1);
    }
}

// ── The events the engine would receive ────────────────────────────────────
//
// `reentrant` is the one non-obvious member.  pal/itransport.h's contract says
// events are "delivered on the main thread and never re-entrantly", and the
// adapter's header repeats it for connect specifically ("onConnect for an
// immediately-successful connect is deferred via a self-posted message, never
// delivered from inside connect()").  A depth counter is the only way to
// notice a violation; without it a re-entrant delivery looks identical to a
// correct one.
struct Recorder : public pal::ITransportEvents {
    int resolves, connects, closes, writables, tlsOk, tlsFail;
    pal::ResolveToken lastToken;
    unsigned int      lastAddr;
    bool              lastResolveOk;
    int               lastConnectErr, lastCloseErr;
    std::string       bytes;

    int  depth;           // callbacks currently on the stack
    bool reentered;       // a callback arrived while another was running

    // Set to have onBytes delete the transport out from under the adapter,
    // which pal/win_transport.h explicitly promises is safe.
    pal::ITransport** suicideSlot;

    Recorder()
      : resolves(0), connects(0), closes(0), writables(0), tlsOk(0), tlsFail(0),
        lastToken(0), lastAddr(0), lastResolveOk(false),
        lastConnectErr(-1), lastCloseErr(-1),
        depth(0), reentered(false), suicideSlot(NULL) {}

    struct Guard {
        Recorder* r;
        explicit Guard(Recorder* rr) : r(rr) { if (r->depth++) r->reentered = true; }
        ~Guard() { --r->depth; }
    };

    void onResolved(pal::ResolveToken token, unsigned int addr, bool ok) override
    {
        Guard g(this);
        ++resolves; lastToken = token; lastAddr = addr; lastResolveOk = ok;
    }
    void onConnect(int err) override
    {
        Guard g(this);
        ++connects; lastConnectErr = err;
    }
    void onBytes(const char* data, size_t n) override
    {
        Guard g(this);
        bytes.append(data, n);
        if (suicideSlot && *suicideSlot) {
            pal::ITransport* t = *suicideSlot;
            *suicideSlot = NULL;
            delete t;          // the documented "may destroy the transport" case
        }
    }
    void onWritable() override                   { Guard g(this); ++writables; }
    void onClosed(int err) override              { Guard g(this); ++closes; lastCloseErr = err; }
    void onTlsEstablished(const char*, const char*, const char*) override
                                                 { Guard g(this); ++tlsOk; }
    void onTlsFailed(pal::TlsFailure, const char*) override
                                                 { Guard g(this); ++tlsFail; }
};

// ── A loopback listener the test drives by hand ────────────────────────────
struct Listener {
    SOCKET listening;
    SOCKET peer;              // the accepted end, INVALID_SOCKET until accepted
    unsigned short port;

    // rcvbuf > 0 pins SO_RCVBUF on the accepted socket (it is inherited from
    // the listener), which is the only lever out here on how much the SENDER
    // can push before its own buffer fills.  Windows auto-tunes the send
    // buffer on loopback and will otherwise swallow tens of megabytes without
    // ever refusing, which makes the adapter's send queue unreachable.
    explicit Listener(int rcvbuf = 0)
      : listening(INVALID_SOCKET), peer(INVALID_SOCKET), port(0)
    {
        listening = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listening == INVALID_SOCKET) return;

        if (rcvbuf > 0)
            ::setsockopt(listening, SOL_SOCKET, SO_RCVBUF,
                         (const char*) &rcvbuf, sizeof rcvbuf);

        sockaddr_in sa;
        memset(&sa, 0, sizeof sa);
        sa.sin_family      = AF_INET;
        sa.sin_addr.s_addr = ::inet_addr("127.0.0.1");
        sa.sin_port        = 0;                       // let the stack choose
        if (::bind(listening, (sockaddr*) &sa, sizeof sa) != 0) { fail(); return; }

        int len = sizeof sa;
        if (::getsockname(listening, (sockaddr*) &sa, &len) != 0) { fail(); return; }
        port = ::ntohs(sa.sin_port);

        if (::listen(listening, 4) != 0) { fail(); return; }
        u_long nb = 1;
        ::ioctlsocket(listening, FIONBIO, &nb);
    }

    ~Listener()
    {
        if (peer      != INVALID_SOCKET) ::closesocket(peer);
        if (listening != INVALID_SOCKET) ::closesocket(listening);
    }

    bool ok() const { return listening != INVALID_SOCKET && port != 0; }

    // Non-blocking; call from inside a Pump predicate.
    bool acceptOnce()
    {
        if (peer != INVALID_SOCKET) return true;
        SOCKET s = ::accept(listening, NULL, NULL);
        if (s == INVALID_SOCKET) return false;
        u_long nb = 1;
        ::ioctlsocket(s, FIONBIO, &nb);
        peer = s;
        return true;
    }

    // Everything readable right now, appended.  Non-blocking.
    size_t drain(std::string& into)
    {
        if (peer == INVALID_SOCKET) return 0;
        char buf[8192];
        size_t total = 0;
        for (;;) {
            int n = ::recv(peer, buf, sizeof buf, 0);
            if (n <= 0) break;
            into.append(buf, (size_t) n);
            total += (size_t) n;
        }
        return total;
    }

    bool sendAll(const char* data, size_t n)
    {
        if (peer == INVALID_SOCKET) return false;
        size_t sent = 0;
        while (sent < n) {
            int k = ::send(peer, data + sent, (int) (n - sent), 0);
            if (k > 0) { sent += (size_t) k; continue; }
            if (::WSAGetLastError() != WSAEWOULDBLOCK) return false;
            ::Sleep(1);
        }
        return true;
    }

    void closePeer()
    {
        if (peer != INVALID_SOCKET) { ::closesocket(peer); peer = INVALID_SOCKET; }
    }

private:
    void fail()
    {
        if (listening != INVALID_SOCKET) { ::closesocket(listening); listening = INVALID_SOCKET; }
        port = 0;
    }

    Listener(const Listener&);
    Listener& operator=(const Listener&);
};

// A port nothing is listening on: bind one, learn its number, drop it.
// Racy in principle, reliable in practice on loopback within one test.
unsigned short dead_port()
{
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return 0;
    sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = ::inet_addr("127.0.0.1");
    sa.sin_port        = 0;
    ::bind(s, (sockaddr*) &sa, sizeof sa);
    int len = sizeof sa;
    ::getsockname(s, (sockaddr*) &sa, &len);
    ::closesocket(s);
    return ::ntohs(sa.sin_port);
}

const unsigned int kLoopback = 0x0100007F;   // 127.0.0.1, network byte order

// Connect a fresh transport to `l` and return it connected, or NULL.
pal::ITransport* connect_to(Recorder& rec, Listener& l)
{
    pal::ITransport* t = pal::CreateWinTransport(rec);
    if (!t) return NULL;
    pal::TransportOptions opt;
    opt.keepalives = true;
    t->connect(kLoopback, l.port, opt);
    const bool up = Pump([&] { l.acceptOnce(); return rec.connects > 0; });
    if (!up || rec.lastConnectErr != 0 || l.peer == INVALID_SOCKET) { delete t; return NULL; }
    return t;
}

}   // namespace

// ── resolve ────────────────────────────────────────────────────────────────

TEST(WinTransport, ResolveLocalhostAnswersTheLoopbackAddressAndEchoesTheToken)
{
    Recorder rec;
    pal::ITransport* t = pal::CreateWinTransport(rec);
    ASSERT_TRUE(t != NULL);

    t->resolve("localhost", 4242);
    // The contract's non-reentrancy rule, at its most visible: the completion
    // must not have been delivered from inside resolve().
    EXPECT_EQ(0, rec.resolves);

    EXPECT_TRUE(Pump([&] { return rec.resolves > 0; }));
    EXPECT_EQ(1, rec.resolves);
    EXPECT_TRUE(rec.lastResolveOk);
    EXPECT_EQ(4242, (int) rec.lastToken);          // the staleness guard's token
    EXPECT_EQ((long) kLoopback, (long) rec.lastAddr);
    EXPECT_FALSE(rec.reentered);

    delete t;
}

TEST(WinTransport, ResolveAnUnknownNameFailsAndKeepsTheWinsockCode)
{
    Recorder rec;
    pal::ITransport* t = pal::CreateWinTransport(rec);
    ASSERT_TRUE(t != NULL);

    // .invalid is reserved by RFC 2606 and must never resolve.
    t->resolve("mushclient-no-such-host.invalid", 9);
    EXPECT_TRUE(Pump([&] { return rec.resolves > 0; }, 15000));
    EXPECT_EQ(1, rec.resolves);
    EXPECT_FALSE(rec.lastResolveOk);
    EXPECT_EQ(9, (int) rec.lastToken);
    EXPECT_EQ(0u, rec.lastAddr);

    // lastResolveError exists because upstream's "Unable to resolve host name"
    // carried the WSA code and the seam nearly lost it (pal/itransport.h).
    // A failure that reports 0 is that loss.
    EXPECT_NE(0, t->lastResolveError());

    delete t;
}

TEST(WinTransport, CancelResolveDropsTheCompletion)
{
    Recorder rec;
    pal::ITransport* t = pal::CreateWinTransport(rec);
    ASSERT_TRUE(t != NULL);

    t->resolve("localhost", 1);
    t->cancelResolve();
    Pump([&] { return false; }, 400);       // give a stray completion time to land
    EXPECT_EQ(0, rec.resolves);

    // ...and a cancel must not have broken the next one.
    t->resolve("localhost", 2);
    EXPECT_TRUE(Pump([&] { return rec.resolves > 0; }));
    EXPECT_EQ(2, (int) rec.lastToken);

    delete t;
}

TEST(WinTransport, DestroyingTheTransportMidLookupDeliversNothing)
{
    // The header promises destruction "cancels the DNS lookup and destroys the
    // message window before anything else".  If it did not, the completion
    // would arrive at a freed window procedure.
    Recorder rec;
    pal::ITransport* t = pal::CreateWinTransport(rec);
    ASSERT_TRUE(t != NULL);
    t->resolve("localhost", 5);
    delete t;

    Pump([&] { return false; }, 500);
    EXPECT_EQ(0, rec.resolves);
}

// ── connect ────────────────────────────────────────────────────────────────

TEST(WinTransport, ConnectToALoopbackListenerReportsSuccessAsynchronously)
{
    Listener l;
    ASSERT_TRUE(l.ok());
    Recorder rec;
    pal::ITransport* t = pal::CreateWinTransport(rec);
    ASSERT_TRUE(t != NULL);

    pal::TransportOptions opt;
    opt.keepalives = true;
    t->connect(kLoopback, l.port, opt);
    EXPECT_EQ(0, rec.connects);        // never from inside connect()

    EXPECT_TRUE(Pump([&] { l.acceptOnce(); return rec.connects > 0; }));
    EXPECT_EQ(1, rec.connects);
    EXPECT_EQ(0, rec.lastConnectErr);
    EXPECT_TRUE(l.peer != INVALID_SOCKET);
    EXPECT_FALSE(rec.reentered);
    EXPECT_EQ(0, rec.closes);          // connecting is not closing

    delete t;
}

TEST(WinTransport, ConnectToAPortNothingListensOnReportsTheError)
{
    const unsigned short dead = dead_port();
    ASSERT_NE(0, (int) dead);

    Recorder rec;
    pal::ITransport* t = pal::CreateWinTransport(rec);
    ASSERT_TRUE(t != NULL);

    pal::TransportOptions opt;
    t->connect(kLoopback, dead, opt);

    // The failure that matters most in this adapter is the one that arrives as
    // NOTHING: a connect that neither succeeds nor reports is a client hung on
    // "Connecting…" forever.  So the assertion is that an event came at all.
    EXPECT_TRUE(Pump([&] { return rec.connects > 0 || rec.closes > 0; }));
    EXPECT_EQ(1, rec.connects);
    EXPECT_NE(0, rec.lastConnectErr);

    delete t;
}

// ── bytes ──────────────────────────────────────────────────────────────────

TEST(WinTransport, SendReachesThePeerAndPeerBytesArriveAsOnBytes)
{
    Listener l;
    ASSERT_TRUE(l.ok());
    Recorder rec;
    pal::ITransport* t = connect_to(rec, l);
    ASSERT_TRUE(t != NULL);

    t->send("hello\r\n", 7);
    std::string got;
    EXPECT_TRUE(Pump([&] { l.drain(got); return got.size() >= 7; }));
    EXPECT_EQ(std::string("hello\r\n"), got);

    ASSERT_TRUE(l.sendAll("world\r\n", 7));
    EXPECT_TRUE(Pump([&] { return rec.bytes.size() >= 7; }));
    EXPECT_EQ(std::string("world\r\n"), rec.bytes);
    EXPECT_FALSE(rec.reentered);

    delete t;
}

TEST(WinTransport, ASendTooBigForTheSocketBufferIsQueuedAndFullyDelivered)
{
    // The buffered-send path — `m_outgoing` plus the FD_WRITE drain, which is
    // `CWorldSocket::OnSend`'s `m_outstanding_data` relocated.
    //
    // Forcing it takes care.  A first attempt sent a megabyte while the peer
    // was being drained inside the pump, and every byte arrived without
    // `onWritable` ever firing — because Windows' send buffer auto-tunes and
    // simply took the whole megabyte in one `send()`, so the queue was never
    // used and the test proved nothing about it.  The queue only comes into
    // play when the socket refuses bytes, so the peer must NOT read until the
    // window is shut.
    Listener l(1024);          // a 1 KB receive window on the accepted socket
    ASSERT_TRUE(l.ok());
    Recorder rec;
    pal::ITransport* t = connect_to(rec, l);
    ASSERT_TRUE(t != NULL);

    const size_t kSize = 8 * 1024 * 1024;
    std::string payload;
    payload.reserve(kSize);
    for (size_t i = 0; i < kSize; ++i)
        payload.push_back((char) ('A' + (i % 26)));

    t->send(payload.data(), payload.size());

    // Pump WITHOUT reading, so anything the socket could not take is stuck in
    // m_outgoing and any FD_WRITE the stack has to give is consumed now.
    Pump([&] { return false; }, 500);

    std::string got;
    l.drain(got);
    const size_t firstDrain = got.size();

    const bool all = Pump([&] { l.drain(got); return got.size() >= kSize; }, 60000);
    EXPECT_TRUE(all);
    EXPECT_EQ(kSize, got.size());
    EXPECT_TRUE(got == payload);      // in order, not merely the right length

    // Only assert what was actually exercised.  If the stack swallowed the
    // whole payload up front there was no queue to drain and no writability to
    // report, and demanding onWritable would be demanding a spurious event.
    if (firstDrain < kSize) {
        if (rec.writables == 0)
            printf("      (%u of %u bytes were queued, and onWritable never fired)\n",
                   (unsigned) (kSize - firstDrain), (unsigned) kSize);
        EXPECT_GT(rec.writables, 0);
    } else {
        printf("      (the socket absorbed all %u bytes; the queue was not used)\n",
               (unsigned) kSize);
    }

    delete t;
}

// ── close ──────────────────────────────────────────────────────────────────

TEST(WinTransport, ThePeerClosingDeliversOnClosed)
{
    Listener l;
    ASSERT_TRUE(l.ok());
    Recorder rec;
    pal::ITransport* t = connect_to(rec, l);
    ASSERT_TRUE(t != NULL);

    l.closePeer();
    EXPECT_TRUE(Pump([&] { return rec.closes > 0; }));
    EXPECT_EQ(1, rec.closes);
    EXPECT_FALSE(rec.reentered);

    delete t;
}

TEST(WinTransport, ALocalCloseIsSilent)
{
    // pal/itransport.h: "onClosed is NOT fired for a locally requested close."
    // The engine runs its own disconnect bookkeeping on that path, so a stray
    // onClosed would run it twice.
    Listener l;
    ASSERT_TRUE(l.ok());
    Recorder rec;
    pal::ITransport* t = connect_to(rec, l);
    ASSERT_TRUE(t != NULL);

    t->close();
    Pump([&] { return false; }, 400);
    EXPECT_EQ(0, rec.closes);

    // ...and closing twice, or closing a transport that never connected, is
    // routine (Disconnect during a lookup) and must not fault.
    t->close();
    delete t;

    Recorder rec2;
    pal::ITransport* fresh = pal::CreateWinTransport(rec2);
    ASSERT_TRUE(fresh != NULL);
    fresh->close();
    EXPECT_EQ(0, rec2.closes);
    delete fresh;
}

TEST(WinTransport, DeletingTheTransportFromInsideOnBytesIsSafe)
{
    // pal/win_transport.h: "Any event handler may destroy the transport; the
    // internal socket's forwarding callbacks touch nothing after the forward."
    // That is a claim about what happens after the return, so the test is that
    // the process survives the rest of the pump.
    Listener l;
    ASSERT_TRUE(l.ok());
    Recorder rec;
    pal::ITransport* t = connect_to(rec, l);
    ASSERT_TRUE(t != NULL);

    rec.suicideSlot = &t;
    ASSERT_TRUE(l.sendAll("boom", 4));
    EXPECT_TRUE(Pump([&] { return rec.bytes.size() >= 4; }));
    EXPECT_EQ(std::string("boom"), rec.bytes);
    EXPECT_TRUE(t == NULL);            // the handler really did delete it

    l.closePeer();
    Pump([&] { return false; }, 300);  // anything still queued for the dead socket
    EXPECT_EQ(0, rec.closes);          // ...and nothing arrived at freed memory
}

TEST(WinTransport, LastErrorStringNamesAWinsockCode)
{
    Recorder rec;
    pal::ITransport* t = pal::CreateWinTransport(rec);
    ASSERT_TRUE(t != NULL);
    const std::string s = t->lastErrorString();
    EXPECT_TRUE(s.find("winsock error") != std::string::npos);
    delete t;
}

// ── the engine's own seam onto the adapter ─────────────────────────────────
//
// CMUSHclientDoc::CreateTransport is the single place the concrete adapter is
// chosen, and CMUSHclientDoc::SendPacket is the only thing that calls send().
// Driving the doc rather than the adapter is what makes these two tests about
// the product rather than about the interface.

TEST(WinTransport, BytesSentAreCountedOncePerSendPacket)
{
    // Stock MUSHclient counts m_nBytesOut TWICE on Windows: once in SendPacket
    // for the whole buffer, and again in CWorldSocket::OnSend for every chunk
    // that drains (both bodies visible at 249534f^).  Every world's Info page
    // "Sent: N bytes", world.GetInfo(217) and the Debug "Sent:" line therefore
    // reported roughly double.  The transport relocation dropped the wire-side
    // count on purpose (doc_shared.cpp:1785 says so), which fixes the number
    // and makes this build disagree with stock 5.06 — docs/UPSTREAM-BUGS.md 92
    // and docs/UPSTREAM-DIVERGENCES.md 18.
    Listener l;
    ASSERT_TRUE(l.ok());

    CMUSHclientDoc doc;
    doc.CreateTransport();
    ASSERT_TRUE(doc.m_pTransport != NULL);
    ASSERT_EQ(0, (int) doc.m_nBytesOut);

    pal::TransportOptions opt;
    doc.m_pTransport->connect(kLoopback, l.port, opt);
    ASSERT_TRUE(Pump([&] { l.acceptOnce(); return l.peer != INVALID_SOCKET; }));

    doc.SendPacket("hello", 5);
    EXPECT_EQ(5, (int) doc.m_nBytesOut);

    doc.SendPacket("world!", 6);
    EXPECT_EQ(11, (int) doc.m_nBytesOut);

    // ...and the bytes really went, so this is not "counted once because it
    // sent nothing".
    std::string got;
    EXPECT_TRUE(Pump([&] { l.drain(got); return got.size() >= 11; }));
    EXPECT_EQ(std::string("helloworld!"), got);
    EXPECT_EQ(11, (int) doc.m_nBytesOut);   // the drain must not add any more
}

TEST(WinTransport, SendingBeforeTheConnectCompletesIsNotAnIllegalSocketCall)
{
    // CreateTransport runs at the START of a connect attempt, so between it and
    // a successful connect the doc has a transport whose socket does not exist
    // yet.  SendPacket's only guard is `m_pTransport == NULL`, which is
    // satisfied — and a script can reach it (world.Send during a DNS lookup).
    //
    // The hazard is specific and the adapter already knows about it elsewhere:
    // close() guards its AsyncSelect/ShutDown/Close because "real MFC's
    // AsyncSelect ASSERTs on an invalid handle".  flushSend's error branch
    // makes the same three calls with no such guard.  This asks whether that
    // branch is reachable, by measurement rather than by reading.
    CMUSHclientDoc doc;
    doc.CreateTransport();
    ASSERT_TRUE(doc.m_pTransport != NULL);

    const long before = McTestAssertionsRaised();
    doc.SendPacket("premature", 9);
    const long raised = McTestAssertionsRaised() - before;
    if (raised != 0)
        printf("      (SendPacket on an unconnected transport raised %ld MFC assertion(s))\n",
               raised);
    EXPECT_EQ(0L, raised);

    // ...and the bytes were not thrown away: once the connect completes they
    // drain, which is what leaving them queued buys over the error branch.
    Listener l;
    ASSERT_TRUE(l.ok());
    pal::TransportOptions opt;
    doc.m_pTransport->connect(kLoopback, l.port, opt);
    std::string got;
    EXPECT_TRUE(Pump([&] { l.acceptOnce(); l.drain(got); return got.size() >= 9; }));
    EXPECT_EQ(std::string("premature"), got);
}
