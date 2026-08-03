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
// B-3 atomic flip: real headers (the headless stub is gone; tests are FULL_BUILD).
#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "scripting/errors.h"
#include "pal/itransport.h"

#include <cstring>
#include <string>

// ── contract compile-check ──────────────────────────────────────────────────
struct FakeTransport : pal::ITransport {
    std::string sent;
    bool tlsStarted = false, closed = false;
    pal::ResolveToken lastToken = -1;

    void resolve(const char*, pal::ResolveToken token) override { lastToken = token; }
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
    void onTlsEstablished(const char*, const char*,
                          const char*) override {}
    void onTlsFailed(pal::TlsFailure, const char*) override {}
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

// Audit §4: fold the repeated ASSERT_EQ(size)+memcmp pair (~6 SOCKS sites) that pins
// the exact bytes queued on the wire.  The size check ASSERTs (returns from the helper
// on mismatch) before memcmp, so a wrong-size buffer never over-reads.
static void expect_queued(CMUSHclientDoc* doc, const char* bytes, size_t n)
{
    const std::string& q = queued(doc);
    ASSERT_EQ((unsigned)n, (unsigned)q.size());
    EXPECT_TRUE(memcmp(q.data(), bytes, n) == 0);
}

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
    const char expected[] = { 4, 1, 0x0F, (char)0xA0,           // ver, connect, port 4000
                              0x0A, 0x0B, 0x0C, 0x0D,            // address, 4 bytes exactly
                              'f', 'r', 'e', 'd', 0 };           // user + NUL
    expect_queued(doc, expected, sizeof expected);
    delete doc;
}

// SOCKS5 without auth: the opener offers exactly one method (no-auth).
TEST(PalTransport, Socks5OpenerNoAuth)
{
    CMUSHclientDoc* doc = make_proxy_doc(eProxyServerSocks5, "", "");
    doc->OnConnect(0);

    EXPECT_TRUE(doc->m_iConnectPhase == eConnectAwaitingProxyResponse1);
    const char expected[] = { 5, 1, 0 };
    expect_queued(doc, expected, sizeof expected);
    delete doc;
}

// SOCKS5 with a username: the opener offers no-auth + username/password.
TEST(PalTransport, Socks5OpenerWithAuth)
{
    CMUSHclientDoc* doc = make_proxy_doc(eProxyServerSocks5, "fred", "pw");
    doc->OnConnect(0);

    EXPECT_TRUE(doc->m_iConnectPhase == eConnectAwaitingProxyResponse1);
    const char expected[] = { 5, 2, 0, 2 };
    expect_queued(doc, expected, sizeof expected);
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
    expect_queued(doc, kSocks5Connect, sizeof kSocks5Connect);
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
    const char expected[] = { 1, 4, 'f', 'r', 'e', 'd', 2, 'p', 'w' };
    expect_queued(doc, expected, sizeof expected);
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
    expect_queued(doc, kSocks5Connect, sizeof kSocks5Connect);
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
// the connection is torn down (transport destroyed, phase reset).
TEST(PalTransport, Socks5BadVersionAborts)
{
    CMUSHclientDoc* doc = make_proxy_doc(eProxyServerSocks5, "", "");
    doc->OnConnect(0);

    char reply[] = { 4, 0 };
    int consumed = doc->ProcessProxyResponse1(reply, 2);
    EXPECT_TRUE(consumed == -1);
    EXPECT_TRUE(doc->m_pTransport == nullptr);   // the transport is torn down
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
    EXPECT_TRUE(doc->m_pTransport == nullptr);   // the transport is torn down
    EXPECT_TRUE(doc->m_iConnectPhase == eConnectNotConnected);
    delete doc;
}

// ── End-to-end headless lifecycle (T3e) ─────────────────────────────────────
// Drives the REAL single-sourced connection path over a real loopback socket:
// ConnectSocket() → async DNS (getaddrinfo thread + completion mailbox) →
// connect → OnConnect → ConnectionEstablished → server bytes →
// OnBytesReceived → DisplayMsg → server close → ConnectionClosed → phase
// reset + transport destroyed.  This is the coverage the twins never had.

#include "platform/posix_socket.h"      // CAsyncSocket::Poll (the pump)
#include "platform/posix_transport.h"   // pal::PumpTransportCompletions
#include <sys/socket.h>
#include <unistd.h>

template <typename Pred>
static bool pump_until(Pred pred, int maxTicks = 2500)
{
    for (int i = 0; i < maxTicks; ++i) {
        if (pred()) return true;
        CAsyncSocket::Poll(10);
        pal::PumpTransportCompletions();
        ::usleep(2000);
    }
    return pred();
}

TEST(PalTransport, EndToEndLoopbackLifecycle)
{
    // in-process server on an ephemeral loopback port
    int srv = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_TRUE(srv >= 0);
    sockaddr_in sa{};
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port        = 0;
    ASSERT_TRUE(::bind(srv, (sockaddr*)&sa, sizeof sa) == 0);
    ASSERT_TRUE(::listen(srv, 1) == 0);
    socklen_t salen = sizeof sa;
    ASSERT_TRUE(::getsockname(srv, (sockaddr*)&sa, &salen) == 0);

    CMUSHclientDoc* doc = new CMUSHclientDoc();
    doc->m_mush_name   = "e2e";
    doc->m_server      = "localhost";            // exercises the resolver thread
    doc->m_port        = ntohs(sa.sin_port);
    doc->m_iFlags      = COLOUR_ANSI;            // display-pipeline setup
    doc->m_iForeColour = WHITE;
    doc->m_iBackColour = BLACK;
    doc->m_pCurrentLine = new CLine();
    doc->AddStyle(COLOUR_ANSI, WHITE, BLACK, 0, static_cast<CAction*>(nullptr));

    EXPECT_TRUE(doc->ConnectSocket());

    // resolve + connect + ConnectionEstablished
    EXPECT_TRUE(pump_until([&] {
        return doc->m_iConnectPhase == eConnectConnectedToMud;
    }));
    EXPECT_TRUE(doc->ConnectionOpen());

    int client = ::accept(srv, nullptr, nullptr);
    ASSERT_TRUE(client >= 0);

    // server line → transport onBytes → OnBytesReceived → DisplayMsg
    long linesBefore = (long)doc->m_LineList.GetCount();
    const char kLine[] = "welcome-e2e\r\n";
    ASSERT_TRUE(::send(client, kLine, sizeof kLine - 1, 0) == (long)sizeof kLine - 1);
    EXPECT_TRUE(pump_until([&] {
        return (long)doc->m_LineList.GetCount() > linesBefore;
    }));
    EXPECT_TRUE(doc->m_nBytesIn >= (long)(sizeof kLine - 1));

    // typed-side send: through the shared SendPacket into the real socket
    doc->SendPacket("ping\r\n", 6);
    char buf[64] = {};
    long got = 0;
    EXPECT_TRUE(pump_until([&] {
        long n = ::recv(client, buf + got, sizeof buf - 1 - got, MSG_DONTWAIT);
        if (n > 0) got += n;
        return got >= 6;
    }));
    EXPECT_TRUE(memcmp(buf, "ping\r\n", 6) == 0);

    // server closes → onClosed → ConnectionClosed bookkeeping + teardown
    ::close(client);
    EXPECT_TRUE(pump_until([&] {
        return doc->m_iConnectPhase == eConnectNotConnected;
    }));
    EXPECT_TRUE(doc->m_pTransport == nullptr);   // bridge destroyed it

    ::close(srv);
    delete doc;
}

// ── Intent-comment tests (candidate mining, 2026-07) ─────────────────────

// doc_shared.cpp DoSendMsg: "double IAC to IAC IAC (unless
// m_bDoNotTranslateIACtoIACIAC)" — telnet escaping of 0xFF in outgoing
// commands, observable on the fake wire.
TEST(PalTransport, SentCommandDoublesIacBytes)
{
    CMUSHclientDoc* doc = new CMUSHclientDoc();
    doc->m_iConnectPhase = eConnectConnectedToMud;
    doc->m_display_my_input = FALSE;
    doc->m_pTransport = new FakeTransport();

    doc->SendMsg(CString("cmd\xFFx"), false, false, false);
    const char expected[] = { 'c','m','d', (char)0xFF, (char)0xFF, 'x', '\r', '\n' };
    ASSERT_EQ(sizeof expected, queued(doc).size());
    EXPECT_TRUE(memcmp(queued(doc).data(), expected, sizeof expected) == 0);

    // the doubling can be suppressed
    queued(doc).clear();
    doc->m_bDoNotTranslateIACtoIACIAC = 1;
    doc->SendMsg(CString("cmd\xFFx"), false, false, false);
    const char raw[] = { 'c','m','d', (char)0xFF, 'x', '\r', '\n' };
    ASSERT_EQ(sizeof raw, queued(doc).size());
    EXPECT_TRUE(memcmp(queued(doc).data(), raw, sizeof raw) == 0);

    delete doc;
}

// doc_shared.cpp SendMsg: "strip trailing endline - that would trigger an
// extra blank line" — a command ending in CRLF transmits exactly one line.
TEST(PalTransport, TrailingEndlineDoesNotSendExtraBlankLine)
{
    CMUSHclientDoc* doc = new CMUSHclientDoc();
    doc->m_iConnectPhase = eConnectConnectedToMud;
    doc->m_display_my_input = FALSE;
    doc->m_pTransport = new FakeTransport();

    doc->SendMsg(CString("hello\r\n"), false, false, false);
    EXPECT_STREQ("hello\r\n", queued(doc).c_str());   // one line, not two
    EXPECT_EQ(1L, doc->m_nTotalLinesSent);

    delete doc;
}

// doc_portable.cpp CheckConnected: "MUSHclient convention: returns 0 (FALSE)
// if connected, non-zero if not" — the inverted convention callers rely on
// ("if (CheckConnected()) return true;" bails early when NOT connected).
TEST(PalTransport, CheckConnectedReturnsNonzeroWhenDisconnected)
{
    CMUSHclientDoc* doc = new CMUSHclientDoc();

    doc->m_iConnectPhase = eConnectNotConnected;
    EXPECT_TRUE(doc->CheckConnected() != 0);        // not connected → non-zero

    // B-3: the adopted doc.cpp CheckConnected requires BOTH phase==connected AND
    // ConnectionOpen() (m_pTransport != NULL); the old portable twin checked only
    // the phase.  Give it an open transport (the dtor's TeardownTransport frees it).
    doc->m_iConnectPhase = eConnectConnectedToMud;
    doc->m_pTransport = new FakeTransport();
    EXPECT_EQ((BOOL)0, doc->CheckConnected());      // connected + open → 0

    delete doc;
}

// platform/posix_transport.cpp: the full ITransport conformance selftest —
// async DNS (+cancel), a real loopback TCP round trip, TLS establish/refuse
// against an in-process self-signed server, and destroy-inside-onBytes
// safety.  The same suite the app runs under --pal-transport-selftest;
// here it exercises the REAL POSIX adapter inside the unit binary.
TEST(PalTransport, PosixAdapterConformanceSelftest)
{
    EXPECT_EQ(0, RunPalTransportSelfTest());   // returns the failure count
}



// ════════════════════════════════════════════════════════════════════════════════
// TLS encrypted-path coverage (gaps in platform/posix_transport.cpp):
//   (1) flushSend() SSL_write — an encrypted send round-trips (server decrypts).
//   (2) handleReadable() SSL_read<=0 clean shutdown → teardownTls + onClosed(0).
//   (3) stepTls() protocol failure → onTlsFailed(TlsFailure::Handshake).
// These drive the REAL PosixTransport (mushcore, built with OPENSSL_FOUND)
// against tiny in-process OpenSSL servers spun up here — no posix_transport.cpp
// or CMake change: OpenSSL's include dir + libs already propagate PUBLIC from
// mushcore, and OPENSSL_VERSION_TEXT (defined only by the REAL <openssl/*.h>,
// per the fallback stub's own `#if !defined(OPENSSL_VERSION_TEXT)` guard) gates
// the whole block, so a no-OpenSSL build — which ships only stub
// ssl.h/err.h/x509.h and whose PosixTransport::startTls fails immediately —
// compiles it out.  (Registration is by static init, so defining these after
// MC_TEST_MAIN() is fine — same TU as main().)
#include <openssl/ssl.h>            // real header → defines OPENSSL_VERSION_TEXT
#if defined(OPENSSL_VERSION_TEXT)
#include <openssl/x509.h>
#include <openssl/evp.h>
#include <thread>

namespace {

// Records the TLS-relevant events the file's FakeTransportEvents ignores.
struct TlsRecorder : pal::ITransportEvents {
    int  connects = 0, closes = 0;
    int  firstCloseErr = -999;                // captured once, on the first close
    bool tlsEstablished = false, tlsFailed = false;
    pal::TlsFailure tlsFailKind = pal::TlsFailure::CertVerify;  // != expected Handshake
    std::string bytes, verifyFailure;

    void onResolved(pal::ResolveToken, unsigned int, bool) override {}
    void onConnect(int) override { ++connects; }
    void onBytes(const char* d, size_t n) override { bytes.append(d, n); }
    void onWritable() override {}
    void onClosed(int err) override {
        if (firstCloseErr == -999) firstCloseErr = err;
        ++closes;
    }
    void onTlsEstablished(const char*, const char*, const char* vf) override {
        tlsEstablished = true; verifyFailure.assign(vf ? vf : "");
    }
    void onTlsFailed(pal::TlsFailure kind, const char*) override {
        tlsFailed = true; tlsFailKind = kind;
    }
};

// A throwaway self-signed "localhost" identity + server SSL_CTX (the
// StartTlsServer recipe in posix_transport.cpp).  Caller frees the out params.
static SSL_CTX* make_tls_server_ctx(EVP_PKEY*& pkey, X509*& x)
{
    pkey = EVP_RSA_gen(2048);
    x = X509_new();
    X509_set_version(x, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
    X509_gmtime_adj(X509_getm_notBefore(x), 0);
    X509_gmtime_adj(X509_getm_notAfter(x), 3600);
    X509_set_pubkey(x, pkey);
    X509_NAME* name = X509_get_subject_name(x);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               (const unsigned char*)"localhost", -1, -1, 0);
    X509_set_issuer_name(x, name);
    X509_sign(x, pkey, EVP_sha256());
    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    SSL_CTX_use_certificate(ctx, x);
    SSL_CTX_use_PrivateKey(ctx, pkey);
    return ctx;
}

static int make_loopback_listener(sockaddr_in& sa)
{
    int srv = ::socket(AF_INET, SOCK_STREAM, 0);
    sa = sockaddr_in{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = 0;
    ::bind(srv, (sockaddr*)&sa, sizeof sa);
    socklen_t len = sizeof sa;
    ::getsockname(srv, (sockaddr*)&sa, &len);
    ::listen(srv, 1);
    return srv;
}

// TLS server: handshake, greet "TLSHELLO", then DECRYPT one client packet and
// echo it back as "ECHO:<plaintext>" through the encrypted channel.
static unsigned short start_tls_echo_server(std::thread& out)
{
    sockaddr_in sa;
    int srv = make_loopback_listener(sa);
    out = std::thread([srv]() {
        EVP_PKEY* pkey = nullptr; X509* x = nullptr;
        SSL_CTX* ctx = make_tls_server_ctx(pkey, x);
        int c = ::accept(srv, nullptr, nullptr);
        if (c >= 0) {
            SSL* ssl = SSL_new(ctx);
            SSL_set_fd(ssl, c);
            if (SSL_accept(ssl) == 1) {
                SSL_write(ssl, "TLSHELLO", 8);
                char buf[128];
                int n = SSL_read(ssl, buf, (int)sizeof buf);
                if (n > 0) {
                    std::string reply = "ECHO:" + std::string(buf, (size_t)n);
                    SSL_write(ssl, reply.data(), (int)reply.size());
                }
                SSL_read(ssl, buf, (int)sizeof buf);   // block until client closes
                SSL_shutdown(ssl);
            }
            SSL_free(ssl);
            ::close(c);
        }
        SSL_CTX_free(ctx); X509_free(x); EVP_PKEY_free(pkey);
        ::close(srv);
    });
    return ntohs(sa.sin_port);
}

// TLS server: handshake, greet, then a CLEAN one-way SSL_shutdown (close_notify)
// while lingering with TCP open — so the client sees the close via SSL_read (the
// gap-2 path), not a TCP FIN.
static unsigned short start_tls_shutdown_server(std::thread& out)
{
    sockaddr_in sa;
    int srv = make_loopback_listener(sa);
    out = std::thread([srv]() {
        EVP_PKEY* pkey = nullptr; X509* x = nullptr;
        SSL_CTX* ctx = make_tls_server_ctx(pkey, x);
        int c = ::accept(srv, nullptr, nullptr);
        if (c >= 0) {
            SSL* ssl = SSL_new(ctx);
            SSL_set_fd(ssl, c);
            if (SSL_accept(ssl) == 1) {
                SSL_write(ssl, "TLSHELLO", 8);
                SSL_shutdown(ssl);          // send close_notify (clean TLS shutdown)
                ::usleep(400000);           // linger: keep TCP open (SSL_read, not FIN)
            }
            SSL_free(ssl);
            ::close(c);
        }
        SSL_CTX_free(ctx); X509_free(x); EVP_PKEY_free(pkey);
        ::close(srv);
    });
    return ntohs(sa.sin_port);
}

// Plain (non-TLS) server: wait for the ClientHello, answer with garbage, so the
// client's handshake fails at the protocol level (wrong version number) —
// distinct from a completed-handshake CertVerify failure.
static unsigned short start_garbage_server(std::thread& out)
{
    sockaddr_in sa;
    int srv = make_loopback_listener(sa);
    out = std::thread([srv]() {
        int c = ::accept(srv, nullptr, nullptr);
        if (c >= 0) {
            char drain[512];
            ::recv(c, drain, sizeof drain, 0);       // wait for the ClientHello
            const char junk[] =
                "NOT-A-TLS-SERVER wrong version number, this is plain text\r\n\r\n";
            ::send(c, junk, sizeof junk - 1, 0);
            ::usleep(100000);                         // let the client read it first
            ::shutdown(c, 2);
            ::close(c);
        }
        ::close(srv);
    });
    return ntohs(sa.sin_port);
}

} // namespace

// (1) encrypted send: plaintext handed to send() is SSL_write-encrypted, the
// server decrypts it, and the round-trip echo returns — exercising flushSend()'s
// SSL_write arm (lines ~358-376), never reached by the existing selftest.
TEST(PalTransport, TlsEncryptedSendRoundTrips)
{
    std::thread server;
    unsigned short port = start_tls_echo_server(server);

    TlsRecorder ev;
    pal::ITransport* t = pal::CreatePosixTransport(ev);
    t->connect(htonl(INADDR_LOOPBACK), port, pal::TransportOptions{ false });
    EXPECT_TRUE(pump_until([&]{ return ev.connects > 0; }));

    pal::TlsParams params;
    params.serverName = "localhost";
    params.allowInvalidCert = true;             // self-signed: consent to proceed
    t->startTls(params);
    EXPECT_TRUE(pump_until([&]{ return ev.tlsEstablished || ev.tlsFailed; }));
    ASSERT_TRUE(ev.tlsEstablished);
    ASSERT_TRUE(!ev.tlsFailed);

    t->send("PINGTLS\n", 8);
    EXPECT_TRUE(pump_until([&]{
        return ev.bytes.find("ECHO:PINGTLS") != std::string::npos;
    }));
    EXPECT_TRUE(ev.bytes.find("ECHO:PINGTLS") != std::string::npos);

    delete t;
    server.join();
}
#endif // OPENSSL_VERSION_TEXT


#if defined(OPENSSL_VERSION_TEXT)
// (2) server-initiated CLEAN TLS shutdown: SSL_read returns 0 (ZERO_RETURN) →
// handleReadable tears the session down and reports onClosed(0) (lines ~311-323).
TEST(PalTransport, TlsServerCleanShutdownReportsClosedZero)
{
    std::thread server;
    unsigned short port = start_tls_shutdown_server(server);

    TlsRecorder ev;
    pal::ITransport* t = pal::CreatePosixTransport(ev);
    t->connect(htonl(INADDR_LOOPBACK), port, pal::TransportOptions{ false });
    EXPECT_TRUE(pump_until([&]{ return ev.connects > 0; }));

    pal::TlsParams params;
    params.serverName = "localhost";
    params.allowInvalidCert = true;
    t->startTls(params);
    EXPECT_TRUE(pump_until([&]{ return ev.tlsEstablished || ev.tlsFailed; }));
    ASSERT_TRUE(ev.tlsEstablished);
    EXPECT_TRUE(pump_until([&]{ return ev.bytes.find("TLSHELLO") != std::string::npos; }));

    EXPECT_TRUE(pump_until([&]{ return ev.closes > 0; }));
    EXPECT_TRUE(ev.closes >= 1);
    EXPECT_EQ(0, ev.firstCloseErr);             // clean shutdown → onClosed(0)

    delete t;
    server.join();
}
#endif // OPENSSL_VERSION_TEXT


#if defined(OPENSSL_VERSION_TEXT)
// (3) handshake HARD failure: a protocol-level error (garbage instead of a
// ServerHello) surfaces as TlsFailure::Handshake — NOT CertVerify (which needs a
// COMPLETED handshake with an unverifiable chain, already covered by the
// selftest).  Exercises stepTls()'s error tail (lines ~423-432).
TEST(PalTransport, TlsHandshakeProtocolFailure)
{
    std::thread server;
    unsigned short port = start_garbage_server(server);

    TlsRecorder ev;
    pal::ITransport* t = pal::CreatePosixTransport(ev);
    t->connect(htonl(INADDR_LOOPBACK), port, pal::TransportOptions{ false });
    EXPECT_TRUE(pump_until([&]{ return ev.connects > 0; }));

    pal::TlsParams params;
    params.serverName = "localhost";
    params.allowInvalidCert = true;             // consent is irrelevant here
    t->startTls(params);
    EXPECT_TRUE(pump_until([&]{ return ev.tlsFailed || ev.tlsEstablished; }));

    EXPECT_TRUE(ev.tlsFailed);
    EXPECT_TRUE(!ev.tlsEstablished);
    EXPECT_TRUE(ev.tlsFailKind == pal::TlsFailure::Handshake);

    delete t;
    server.join();
}
#endif // OPENSSL_VERSION_TEXT

MC_TEST_MAIN()

// ── TLS trust anchors ─────────────────────────────────────────────────────
// (Registered by static init, so defining it after MC_TEST_MAIN() is fine —
// same pattern as the TLS block above.)
//
// SSL_CTX_set_default_verify_paths() alone looks in the OPENSSLDIR compiled
// into whichever libcrypto we linked.  For a Homebrew build that is
// /opt/homebrew/etc/openssl@3 — a path that exists on the BUILD machine and on
// essentially no user's machine.  Measured against the linked library: with
// that directory present the store gets 184 anchors; with it absent, ZERO.  A
// packaged app would then fail verification for every TLS world and raise the
// "connect anyway?" prompt every single time, which is worse than useless — it
// teaches the user to click through certificate warnings.
//
// This forces exactly that situation (SSL_CERT_FILE / SSL_CERT_DIR point at
// nothing, which is what OpenSSL consults ahead of its compiled-in default) and
// requires a usable trust store regardless.
#if defined(OPENSSL_VERSION_TEXT)
#include <cstdlib>
TEST(PalTransport, TrustStoreSurvivesWithoutTheBuildMachinesOpensslDir)
{
    setenv("SSL_CERT_FILE", "/nonexistent/cert.pem", 1);
    setenv("SSL_CERT_DIR",  "/nonexistent/certs",    1);

    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    ASSERT_NE(nullptr, ctx);
    const int anchors = pal::ConfigureTrustAnchors(ctx);
    SSL_CTX_free(ctx);

    unsetenv("SSL_CERT_FILE");
    unsetenv("SSL_CERT_DIR");

    // Any real store holds dozens; the failure being pinned is exactly 0.
    EXPECT_GT(anchors, 20);
    if (anchors <= 20)
        printf("    only %d trust anchors - a packaged app would prompt on "
               "every TLS connection\n", anchors);
}
#endif
