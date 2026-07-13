// tests/test_pal_transport.cpp
// ITransport migration step T1: characterization tests pinning the SOCKS
// proxy phase machine and its exact wire bytes BEFORE T2/T3 rewire the
// engine onto pal::ITransport and single-source the doc-level twins.
//
// The tests drive the engine's real entry points (OnConnect, the
// ProcessProxyResponse* parsers in doc_shared.cpp) and inspect the bytes the
// engine queued into the transport seam — SendPacket sends via m_pTransport
// there and, headless, nothing drains it, so the queue IS the wire capture.
//
// Byte-width pins matter: the SOCKS4 and SOCKS5 CONNECT packets carry the
// address as exactly 4 bytes (unsigned int, not LP64 unsigned long) — both
// builds shipped that bug at least once, so the packet LENGTHS are asserted
// exactly.
//
// pal/itransport.h itself is compile-checked here via a minimal fake
// implementing the full contract.

#include "mc_test.h"
#include "platform/doc_stub.h"
#include "worldsock.h"
#include "pal/itransport.h"

#include <cstring>
#include <string>

// ── contract compile-check ──────────────────────────────────────────────────
struct FakeTransport : pal::ITransport {
    std::string sent;
    bool tlsStarted = false, closed = false;
    pal::ResolveToken lastToken = -1;

    void resolve(std::string_view, pal::ResolveToken token) override { lastToken = token; }
    void cancelResolve() override { lastToken = -1; }
    void connect(unsigned int, unsigned short, const pal::TransportOptions&) override {}
    void send(const char* data, size_t n) override { sent.append(data, n); }
    void startTls(const pal::TlsParams&) override { tlsStarted = true; }
    void close() override { closed = true; }
    std::string lastErrorString() const override { return "fake"; }
};

struct FakeTransportEvents : pal::ITransportEvents {
    int connects = 0, closes = 0;
    std::string bytes;
    void onResolved(pal::ResolveToken, unsigned int, bool) override {}
    void onConnect(int) override { ++connects; }
    void onBytes(const char* data, size_t n) override { bytes.append(data, n); }
    void onWritable() override {}
    void onClosed(int) override { ++closes; }
    void onTlsEstablished(std::string_view, std::string_view,
                          std::string_view) override {}
    void onTlsFailed(pal::TlsFailure, std::string_view) override {}
};

// ── helpers ─────────────────────────────────────────────────────────────────
static CMUSHclientDoc* make_proxy_doc(int socksType,
                                      const char* user, const char* pass)
{
    CMUSHclientDoc* doc = new CMUSHclientDoc();
    doc->m_iSocksProcessing  = socksType;
    doc->m_strProxyUserName  = user;
    doc->m_strProxyPassword  = pass;
    // MUD endpoint 10.11.12.13:4000 (values chosen so every byte is distinct)
    doc->m_sockAddr.sin_addr.s_addr = htonl(0x0A0B0C0Du);
    doc->m_sockAddr.sin_port        = htons(4000);
    // The doc sends through the PAL transport seam (T3b single-sourced
    // SendPacket); a FakeTransport captures the bytes.  The doc destructor's
    // TeardownTransport() frees it.
    doc->m_pTransport = new FakeTransport();
    return doc;
}

static std::string& queued(CMUSHclientDoc* doc)
{
    return static_cast<FakeTransport*>(doc->m_pTransport)->sent;
}

// The SOCKS5 CONNECT request SendProxyConnectSequence must emit:
// 05 01 00 01 + 4-byte address (network order) + 2-byte port (network order).
static const char kSocks5Connect[] =
    { 5, 1, 0, 1, 0x0A, 0x0B, 0x0C, 0x0D, 0x0F, (char)0xA0 };

// ── tests ───────────────────────────────────────────────────────────────────

TEST(PalTransport, FakeImplementsTheContract)
{
    FakeTransport t;
    FakeTransportEvents ev;
    pal::ITransport& seam = t;

    seam.resolve("mud.example.com", 42);
    EXPECT_TRUE(t.lastToken == 42);
    seam.send("abc", 3);
    EXPECT_STREQ("abc", t.sent.c_str());
    seam.startTls(pal::TlsParams{ "mud.example.com", false });
    EXPECT_TRUE(t.tlsStarted);
    seam.close();
    EXPECT_TRUE(t.closed);

    ev.onBytes("xy", 2);
    EXPECT_STREQ("xy", ev.bytes.c_str());
}

// SOCKS4: OnConnect(0) sends 04 01 + port + 4-BYTE address + username + NUL.
// Length pin: 2 + 2 + 4 + 4("fred") + 1 = 13 (an LP64 unsigned long address
// would make it 17 — the historical malformed packet).
TEST(PalTransport, Socks4ConnectPacket)
{
    CMUSHclientDoc* doc = make_proxy_doc(eProxyServerSocks4, "fred", "");
    doc->OnConnect(0);

    EXPECT_TRUE(doc->m_iConnectPhase == eConnectAwaitingProxyResponse3);
    const std::string& q = queued(doc);
    ASSERT_EQ(13u, (unsigned)q.size());
    const char expected[] = { 4, 1, 0x0F, (char)0xA0,           // ver, connect, port 4000
                              0x0A, 0x0B, 0x0C, 0x0D,            // address, 4 bytes exactly
                              'f', 'r', 'e', 'd', 0 };           // user + NUL
    EXPECT_TRUE(memcmp(q.data(), expected, sizeof expected) == 0);
    delete doc;
}

// SOCKS5 without auth: the opener offers exactly one method (no-auth).
TEST(PalTransport, Socks5OpenerNoAuth)
{
    CMUSHclientDoc* doc = make_proxy_doc(eProxyServerSocks5, "", "");
    doc->OnConnect(0);

    EXPECT_TRUE(doc->m_iConnectPhase == eConnectAwaitingProxyResponse1);
    const std::string& q = queued(doc);
    ASSERT_EQ(3u, (unsigned)q.size());
    const char expected[] = { 5, 1, 0 };
    EXPECT_TRUE(memcmp(q.data(), expected, sizeof expected) == 0);
    delete doc;
}

// SOCKS5 with a username: the opener offers no-auth + username/password.
TEST(PalTransport, Socks5OpenerWithAuth)
{
    CMUSHclientDoc* doc = make_proxy_doc(eProxyServerSocks5, "fred", "pw");
    doc->OnConnect(0);

    EXPECT_TRUE(doc->m_iConnectPhase == eConnectAwaitingProxyResponse1);
    const std::string& q = queued(doc);
    ASSERT_EQ(4u, (unsigned)q.size());
    const char expected[] = { 5, 2, 0, 2 };
    EXPECT_TRUE(memcmp(q.data(), expected, sizeof expected) == 0);
    delete doc;
}

// Server picks no-auth (method 0): the CONNECT request goes out immediately.
// Length pin: 10 bytes (4 header + 4 address + 2 port).
TEST(PalTransport, Socks5NoAuthAccepted)
{
    CMUSHclientDoc* doc = make_proxy_doc(eProxyServerSocks5, "", "");
    doc->OnConnect(0);
    queued(doc).clear();

    char reply[] = { 5, 0 };
    int consumed = doc->ProcessProxyResponse1(reply, 2);
    EXPECT_TRUE(consumed == 2);
    EXPECT_TRUE(doc->m_iConnectPhase == eConnectAwaitingProxyResponse3);
    const std::string& q = queued(doc);
    ASSERT_EQ(10u, (unsigned)q.size());
    EXPECT_TRUE(memcmp(q.data(), kSocks5Connect, sizeof kSocks5Connect) == 0);
    delete doc;
}

// Server demands username/password (method 2): the auth sub-negotiation goes
// out (01, len, user, len, pass) and the phase advances to response 2.
TEST(PalTransport, Socks5AuthRequested)
{
    CMUSHclientDoc* doc = make_proxy_doc(eProxyServerSocks5, "fred", "pw");
    doc->OnConnect(0);
    queued(doc).clear();

    char reply[] = { 5, 2 };
    int consumed = doc->ProcessProxyResponse1(reply, 2);
    EXPECT_TRUE(consumed == 2);
    EXPECT_TRUE(doc->m_iConnectPhase == eConnectAwaitingProxyResponse2);
    const std::string& q = queued(doc);
    ASSERT_EQ(9u, (unsigned)q.size());
    const char expected[] = { 1, 4, 'f', 'r', 'e', 'd', 2, 'p', 'w' };
    EXPECT_TRUE(memcmp(q.data(), expected, sizeof expected) == 0);
    delete doc;
}

// Auth accepted: the CONNECT request follows.
TEST(PalTransport, Socks5AuthAccepted)
{
    CMUSHclientDoc* doc = make_proxy_doc(eProxyServerSocks5, "fred", "pw");
    doc->OnConnect(0);
    char reply1[] = { 5, 2 };
    doc->ProcessProxyResponse1(reply1, 2);
    queued(doc).clear();

    char reply2[] = { 1, 0 };
    int consumed = doc->ProcessProxyResponse2(reply2, 2);
    EXPECT_TRUE(consumed == 2);
    EXPECT_TRUE(doc->m_iConnectPhase == eConnectAwaitingProxyResponse3);
    const std::string& q = queued(doc);
    ASSERT_EQ(10u, (unsigned)q.size());
    EXPECT_TRUE(memcmp(q.data(), kSocks5Connect, sizeof kSocks5Connect) == 0);
    delete doc;
}

// SOCKS5 final reply granted (status 0, 10 bytes): connected to the MUD.
TEST(PalTransport, Socks5TunnelEstablished)
{
    CMUSHclientDoc* doc = make_proxy_doc(eProxyServerSocks5, "", "");
    doc->OnConnect(0);
    char reply1[] = { 5, 0 };
    doc->ProcessProxyResponse1(reply1, 2);

    char reply3[] = { 5, 0, 0, 1, 0, 0, 0, 0, 0, 0 };
    int consumed = doc->ProcessProxyResponse3(reply3, 10);
    EXPECT_TRUE(consumed == 10);
    EXPECT_TRUE(doc->m_iConnectPhase == eConnectConnectedToMud);
    delete doc;
}

// ── OnBytesReceived: the T2b/c connected-data seam ──────────────────────────
// Drives the receive tail directly (no socket): counters advance and the bytes
// reach the display pipeline (a completed line lands in m_LineList).

static CMUSHclientDoc* make_connected_doc()
{
    CMUSHclientDoc* doc = new CMUSHclientDoc();
    doc->m_iConnectPhase = eConnectConnectedToMud;
    doc->m_iFlags        = COLOUR_ANSI;
    doc->m_iForeColour   = WHITE;
    doc->m_iBackColour   = BLACK;
    doc->m_pCurrentLine  = new CLine();
    doc->AddStyle(COLOUR_ANSI, WHITE, BLACK, 0, static_cast<CAction*>(nullptr));
    return doc;
}

TEST(PalTransport, OnBytesReceivedCountsAndDisplays)
{
    CMUSHclientDoc* doc = make_connected_doc();
    long linesBefore = (long)doc->m_LineList.GetCount();

    char pkt[] = "alpha\r\n";
    doc->OnBytesReceived(pkt, (int)strlen(pkt));

    EXPECT_TRUE(doc->m_nBytesIn == (long)strlen("alpha\r\n"));
    EXPECT_TRUE(doc->m_iInputPacketCount == 1);
    // A newline-terminated packet commits at least one line to the buffer.
    EXPECT_TRUE((long)doc->m_LineList.GetCount() > linesBefore);
    delete doc;
}

TEST(PalTransport, OnBytesReceivedAccumulatesAcrossPackets)
{
    CMUSHclientDoc* doc = make_connected_doc();

    char p1[] = "one\r\n";
    char p2[] = "two\r\n";
    doc->OnBytesReceived(p1, (int)strlen(p1));
    doc->OnBytesReceived(p2, (int)strlen(p2));

    EXPECT_TRUE(doc->m_iInputPacketCount == 2);
    EXPECT_TRUE(doc->m_nBytesIn == (long)(strlen(p1) + strlen(p2)));
    delete doc;
}

// SOCKS4 final reply granted (0, 90; 8 bytes): connected to the MUD.
TEST(PalTransport, Socks4TunnelEstablished)
{
    CMUSHclientDoc* doc = make_proxy_doc(eProxyServerSocks4, "", "");
    doc->OnConnect(0);

    char reply[] = { 0, 90, 0, 0, 0, 0, 0, 0 };
    int consumed = doc->ProcessProxyResponse3(reply, 8);
    EXPECT_TRUE(consumed == 8);
    EXPECT_TRUE(doc->m_iConnectPhase == eConnectConnectedToMud);
    delete doc;
}

// A wrong SOCKS version in the method reply aborts: parser returns -1 and
// the connection is torn down (socket deleted, phase reset).
TEST(PalTransport, Socks5BadVersionAborts)
{
    CMUSHclientDoc* doc = make_proxy_doc(eProxyServerSocks5, "", "");
    doc->OnConnect(0);

    char reply[] = { 4, 0 };
    int consumed = doc->ProcessProxyResponse1(reply, 2);
    EXPECT_TRUE(consumed == -1);
    EXPECT_TRUE(doc->m_pSocket == nullptr);
    EXPECT_TRUE(doc->m_iConnectPhase == eConnectNotConnected);
    delete doc;
}

// A rejected CONNECT (SOCKS5 status != 0) likewise aborts.
TEST(PalTransport, Socks5ConnectRefusedAborts)
{
    CMUSHclientDoc* doc = make_proxy_doc(eProxyServerSocks5, "", "");
    doc->OnConnect(0);
    char reply1[] = { 5, 0 };
    doc->ProcessProxyResponse1(reply1, 2);

    char reply3[] = { 5, 5, 0, 1, 0, 0, 0, 0, 0, 0 };   // 5 = refused
    int consumed = doc->ProcessProxyResponse3(reply3, 10);
    EXPECT_TRUE(consumed == -1);
    EXPECT_TRUE(doc->m_pSocket == nullptr);
    EXPECT_TRUE(doc->m_iConnectPhase == eConnectNotConnected);
    delete doc;
}

MC_TEST_MAIN()
