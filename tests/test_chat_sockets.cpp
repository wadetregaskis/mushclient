// tests/test_chat_sockets.cpp
//
// The chat SOCKET LIFECYCLE, driven end to end over loopback.
//
// docs/UNREACHABLE-PATHS-2026-08.md finding 23 split this family in two and
// recommended covering only the cheap half: hoist the pure protocol parsing into
// pal/chat_protocol.h (done — tests/test_chat_protocol.cpp covers it) and
// "leave Accept/AsyncSelect/m_ChatList ownership uncovered and record it as
// accepted debt", on the grounds that a loopback harness plus untangling the
// stub was "a multi-day job".
//
// That estimate was for untangling the stub.  The stub is gone — chatsock.cpp
// and chatlistensock.cpp are compiled into mushcore
// (docs/STUB-ADOPTION-2026-08.md) — so the harness is all that was left, and the
// pieces it needs were already real: CAsyncSocket::Listen/Accept/AsyncSelect are
// fully implemented in platform/posix_socket.h, and its Poll() is the same pump
// platform/posix_transport.cpp:637 already drives in its own selftest.
//
// So this closes the other half of finding 23.  What runs below is the whole
// chain the finding named:
//
//   ChatAcceptCalls -> bind/listen -> Poll -> OnAccept -> Accept ->
//   AsyncSelect re-arm -> m_ChatList ownership -> the handshake ->
//   ON_PLUGIN_CHAT_ACCEPT veto -> the timers.cpp reap
//
// Everything here talks to 127.0.0.1 only, and each test picks its own port.

#include "mc_test.h"
#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "chatsock.h"
#include "chatlistensock.h"
#include "scripting/errors.h"
#include "platform/wx_mfc_hooks.h"   // g_wxhook_message_box (the accept prompt)

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

// ── helpers ─────────────────────────────────────────────────────────────────

// Ports are per-test so two tests in the same binary never collide, and high
// enough not to need privileges.  If one is somehow in use the Create() below
// fails and the test says so rather than hanging.
static short kPort = 25170;
static short next_port() { return kPort++; }

// A plain client socket connected to 127.0.0.1:port.  Not a CAsyncSocket — it
// stands in for the REMOTE end, which in reality is another MUD client.
struct LoopbackClient {
    int fd = -1;

    bool connect_to(short port)
    {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return false;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port   = htons((unsigned short) port);
        a.sin_addr.s_addr = inet_addr("127.0.0.1");
        if (::connect(fd, (sockaddr*) &a, sizeof a) != 0) { ::close(fd); fd = -1; return false; }
        ::fcntl(fd, F_SETFL, O_NONBLOCK);
        return true;
    }
    void send(const std::string& s) const { ::write(fd, s.data(), s.size()); }
    std::string recv_some() const
    {
        char buf[4096];
        const ssize_t n = ::read(fd, buf, sizeof buf);
        return n > 0 ? std::string(buf, (size_t) n) : std::string();
    }
    ~LoopbackClient() { if (fd >= 0) ::close(fd); }
};

// Drive the socket pump until `pred` holds or we run out of patience.  Polling
// on a condition rather than for a fixed duration, per the recorded lesson from
// the input selftest: a fixed sleep is a flake waiting for a loaded machine.
template <typename Pred>
static bool pump_until(Pred pred, int max_ms = 2000)
{
    for (int waited = 0; waited < max_ms; ++waited) {
        if (pred()) return true;
        CAsyncSocket::Poll(1);
        if (pred()) return true;
        ::usleep(1000);
    }
    return pred();
}

static long chat_count(CMUSHclientDoc& doc) { return doc.m_ChatList.GetCount(); }

// ── the accept chain ────────────────────────────────────────────────────────

TEST(ChatSockets, AcceptCallsListensAndAcceptsAnIncomingConnection)
{
    const short port = next_port();
    CMUSHclientDoc doc;

    ASSERT_EQ(eOK, doc.ChatAcceptCalls(port));
    ASSERT_NE(nullptr, doc.m_pChatListenSocket);
    EXPECT_EQ(0L, chat_count(doc));            // nothing accepted yet

    // A second call must be refused rather than leaking a second listener —
    // the control that makes "always create a listener" fail.
    EXPECT_EQ(eChatAlreadyListening, doc.ChatAcceptCalls(port));

    LoopbackClient client;
    ASSERT_TRUE(client.connect_to(port));

    // THE CHAIN: Poll sees the listener readable, dispatches OnAccept, which
    // Accepts into a fresh CChatSocket, re-arms AsyncSelect and takes ownership
    // by adding it to m_ChatList.  None of this was reachable before the
    // adoption — windows_stubs.cpp supplied an inert CChatSocket.
    ASSERT_TRUE(pump_until([&] { return chat_count(doc) == 1; }));

    CChatSocket* peer = doc.m_ChatList.GetHead();
    ASSERT_NE(nullptr, peer);
    // Accepted, but not yet trusted: the handshake has not arrived.
    EXPECT_EQ((int) eChatAwaitingConnectionRequest, (int) peer->m_iChatStatus);
    EXPECT_TRUE(peer->m_bIncoming);
    EXPECT_EQ(std::string("127.0.0.1"), std::string((const char*) peer->m_strServerName));

    doc.ChatStopAcceptingCalls();
    EXPECT_EQ(nullptr, doc.m_pChatListenSocket);
    doc.ChatDisconnectAll();
}

TEST(ChatSockets, StopAcceptingClosesTheListenerSoNewCallsFail)
{
    const short port = next_port();
    CMUSHclientDoc doc;
    ASSERT_EQ(eOK, doc.ChatAcceptCalls(port));
    doc.ChatStopAcceptingCalls();

    // The discriminator for "stop actually closed the socket": a connect to the
    // port must now fail.  Without the close it would still be accepted, and a
    // test that only checked m_pChatListenSocket == nullptr would not notice.
    LoopbackClient client;
    const bool connected = client.connect_to(port);
    if (connected) {
        // Some stacks complete the TCP handshake from the backlog even after
        // close; the load-bearing assertion is that nothing is ACCEPTED.
        CAsyncSocket::Poll(10);
        CAsyncSocket::Poll(10);
    }
    EXPECT_EQ(0L, chat_count(doc));
}

// ── the handshake, and the plugin veto ──────────────────────────────────────

// A well-formed MudMaster CHAT: handshake, as chatsock.cpp's parser expects.
static std::string handshake(const char* who = "remote")
{
    return std::string("CHAT:") + who + "\n127.0.0.10000";
}

TEST(ChatSockets, HandshakePromotesThePeerToConnected)
{
    const short port = next_port();
    CMUSHclientDoc doc;
    doc.m_bValidateIncomingCalls = false;     // no modal ask
    ASSERT_EQ(eOK, doc.ChatAcceptCalls(port));

    LoopbackClient client;
    ASSERT_TRUE(client.connect_to(port));
    ASSERT_TRUE(pump_until([&] { return chat_count(doc) == 1; }));
    CChatSocket* peer = doc.m_ChatList.GetHead();

    client.send(handshake("someone"));
    ASSERT_TRUE(pump_until([&] {
        return peer->m_iChatStatus == eChatConnected;
    }));

    EXPECT_EQ(std::string("someone"),
              std::string((const char*) peer->m_strRemoteUserName));
    // The peer is now addressable by the scripting API, which is the point of
    // the whole chain: GetChatList walks m_ChatList.
    EXPECT_NE(0, (int) doc.ChatGetID("someone"));

    doc.ChatStopAcceptingCalls();
    doc.ChatDisconnectAll();
}

// ── the MudMaster message framer, and MFC's NUL-bounded Find ────────────────
//
// A MudMaster frame ends at CHAT_END_OF_COMMAND (0xFF), and the framer locates
// that byte with what used to be CString::Find.  MFC's Find(char) is _tcschr:
// it gives up at the first embedded NUL even though GetLength() counts past it.
// The port's CString is std::string-backed and does not stop, so a peer that
// puts a NUL before its terminator was framed and DISPATCHED here while the
// same bytes left a Windows client's m_outstanding_input growing forever.
//
// The NULs genuinely survive to this point on both platforms -- ChatAppendBytes
// exists precisely so they do -- so the divergence was the search alone, and
// pal::chatFindChar (already used by the handshake splits) is what removes it.
//
// Both directions, because either half alone proves nothing: a framer that
// dispatched nothing at all would pass the NUL half, and the pre-fix code
// passes the clean half.
TEST(ChatSockets, AnEmbeddedNulHidesTheMudMasterTerminatorTheWayMfcDoes)
{
    const short port = next_port();
    CMUSHclientDoc doc;
    doc.m_bValidateIncomingCalls = false;     // no modal ask
    ASSERT_EQ(eOK, doc.ChatAcceptCalls(port));

    LoopbackClient client;
    ASSERT_TRUE(client.connect_to(port));
    ASSERT_TRUE(pump_until([&] { return chat_count(doc) == 1; }));
    CChatSocket* peer = doc.m_ChatList.GetHead();

    client.send(handshake("someone"));
    ASSERT_TRUE(pump_until([&] { return peer->m_iChatStatus == eChatConnected; }));
    ASSERT_EQ(std::string("someone"),
              std::string((const char*) peer->m_strRemoteUserName));

    // CONTROL: a clean frame IS framed and dispatched.  CHAT_NAME_CHANGE is the
    // oracle because its handler writes m_strRemoteUserName, so "dispatched" is
    // observable without a script engine, a plugin or a view.
    std::string clean;
    clean += (char) CHAT_NAME_CHANGE;
    clean += "renamed";
    clean += (char) (unsigned char) CHAT_END_OF_COMMAND;
    client.send(clean);
    ASSERT_TRUE(pump_until([&] {
        return std::string((const char*) peer->m_strRemoteUserName) == "renamed";
    }));
    EXPECT_EQ(0, peer->m_outstanding_input.GetLength());   // frame fully consumed

    // THE DIVERGENCE: the same frame with a NUL in the middle of its payload.
    // strchr never reaches the 0xFF, so Windows sees no terminator -- and now
    // neither do we.
    std::string hidden;
    hidden += (char) CHAT_NAME_CHANGE;
    hidden += "ab";
    hidden += '\0';
    hidden += "cd";
    hidden += (char) (unsigned char) CHAT_END_OF_COMMAND;
    ASSERT_EQ(7u, hidden.size());
    client.send(hidden);

    // The bytes really did reach the framer.  This is the load-bearing
    // assertion, and the one that fails on the pre-fix code: std::string::find
    // located the terminator inside the very OnReceive that appended these
    // bytes, so m_outstanding_input went 0 -> 0 and never held all seven.
    ASSERT_TRUE(pump_until([&] {
        return peer->m_outstanding_input.GetLength() == (int) hidden.size();
    }));

    // ...and they stay put.  A framer that consumed them would empty the buffer
    // and rename the peer to "ab\0cd" (which reads as "ab" through LPCTSTR).
    for (int i = 0; i < 20; ++i)
        CAsyncSocket::Poll(1);

    EXPECT_EQ((int) hidden.size(), peer->m_outstanding_input.GetLength());
    EXPECT_EQ(std::string("renamed"),
              std::string((const char*) peer->m_strRemoteUserName));

    doc.ChatStopAcceptingCalls();
    doc.ChatDisconnectAll();
}

// The VALIDATE-INCOMING-CALLS arm, in both directions.
//
// Finding 23 called the accept veto out as "the one arm with a security shape
// (a plugin's refusal being ignored gives the peer live snooping and file
// transfer)".  The plugin half of that decision is covered as a pure function in
// tests/test_chat_protocol.cpp (pal::chatAcceptAfterPlugins), and driving it
// through a socket needs a live script engine.  The USER half needs neither, and
// it reaches the same rejection path through pal::chatAcceptAfterUser — so this
// is where the socket-level consequence of a refusal gets pinned.
//
// Both directions, because a one-way test here is worth very little: a body that
// accepted unconditionally and a body that rejected unconditionally would each
// pass one of these two.

static int  g_answer   = IDNO;
static int  g_askCount = 0;
static int  FakeAsk(const char*, const char*, unsigned) { ++g_askCount; return g_answer; }

struct AskHook {
    int (*saved)(const char*, const char*, unsigned);
    AskHook(int answer) : saved(g_wxhook_message_box)
      { g_answer = answer; g_askCount = 0; g_wxhook_message_box = FakeAsk; }
    ~AskHook() { g_wxhook_message_box = saved; }
};

TEST(ChatSockets, RefusingAnIncomingCallDropsIt)
{
    const short port = next_port();
    CMUSHclientDoc doc;
    doc.m_bValidateIncomingCalls = true;      // ask before accepting
    AskHook refuse(IDNO);

    ASSERT_EQ(eOK, doc.ChatAcceptCalls(port));
    LoopbackClient client;
    ASSERT_TRUE(client.connect_to(port));
    ASSERT_TRUE(pump_until([&] { return chat_count(doc) == 1; }));
    CChatSocket* peer = doc.m_ChatList.GetHead();

    client.send(handshake("intruder"));

    // Refused: the socket must be marked for collection and must NOT reach
    // eChatConnected, which is the state that would grant snooping and file
    // transfer.
    ASSERT_TRUE(pump_until([&] { return peer->m_bDeleteMe; }));
    EXPECT_NE((int) eChatConnected, (int) peer->m_iChatStatus);
    EXPECT_GT(g_askCount, 0);                 // it really did ask

    doc.ChatStopAcceptingCalls();
    doc.CheckTimers();                        // reap the refused socket
    doc.ChatDisconnectAll();
}

TEST(ChatSockets, AcceptingAnIncomingCallConnectsIt)
{
    const short port = next_port();
    CMUSHclientDoc doc;
    doc.m_bValidateIncomingCalls = true;
    AskHook accept(IDYES);

    ASSERT_EQ(eOK, doc.ChatAcceptCalls(port));
    LoopbackClient client;
    ASSERT_TRUE(client.connect_to(port));
    ASSERT_TRUE(pump_until([&] { return chat_count(doc) == 1; }));
    CChatSocket* peer = doc.m_ChatList.GetHead();

    client.send(handshake("friend"));

    ASSERT_TRUE(pump_until([&] { return peer->m_iChatStatus == eChatConnected; }));
    EXPECT_FALSE(peer->m_bDeleteMe);
    EXPECT_GT(g_askCount, 0);

    doc.ChatStopAcceptingCalls();
    doc.ChatDisconnectAll();
}

// ── ownership: the timers.cpp reap ──────────────────────────────────────────

// m_bDeleteMe is how a chat socket asks to be collected; timers.cpp:75-87 is
// what collects it, and that loop is guarded on MUSHCLIENT_FULL_BUILD, which
// mushcore defines.  Before the adoption there was no real socket to mark, so
// the guard's non-Windows arm had never run.
TEST(ChatSockets, MarkedSocketsAreReapedByTheTimerSweep)
{
    const short port = next_port();
    CMUSHclientDoc doc;
    ASSERT_EQ(eOK, doc.ChatAcceptCalls(port));

    LoopbackClient client;
    ASSERT_TRUE(client.connect_to(port));
    ASSERT_TRUE(pump_until([&] { return chat_count(doc) == 1; }));

    CChatSocket* peer = doc.m_ChatList.GetHead();
    ASSERT_FALSE(peer->m_bDeleteMe);

    // A sweep must NOT collect a live socket — the control.  Without it, "reap
    // everything" would pass the assertion below.
    doc.CheckTimers();
    EXPECT_EQ(1L, chat_count(doc));

    peer->m_bDeleteMe = true;
    doc.CheckTimers();
    EXPECT_EQ(0L, chat_count(doc));

    doc.ChatStopAcceptingCalls();
}

MC_TEST_MAIN()
