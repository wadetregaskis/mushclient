// tests/test_connection.cpp
// Tests for the doc_shared.cpp connection family: SOCKS proxy error paths,
// connect-failure notification, DNS-completion arms, TLS outcome policy,
// MCCP decompression, logging (preamble/postamble/colour), the connect-time
// login sequences (ConnectionEstablished), ConnectSocket validation, and
// GetStyleRGB's inverse/custom/xterm arms.
//
// All of it drives the REAL engine (the same doc_shared.cpp the products
// link) against a recording pal::ITransport fake — the seam the product's
// POSIX/Win adapters implement.  Error notes are asserted by reading the
// doc's real line list (App.m_bErrorNotificationToOutputWindow routes the
// notifications through Note()).

#include "mc_test.h"
#include "platform/doc_stub.h"
#include "pal/itransport.h"
#include "scripting/scripting.h"

#include <zlib.h>

#include <cstring>
#include <string>
#include <unistd.h>

// telnet bytes (numeric to avoid macro clashes)
static const unsigned char T_IAC = 255, T_WILL = 251, T_SB = 250, T_SE = 240;

// ── recording transport ─────────────────────────────────────────────────────

struct RecTransport : pal::ITransport {
    std::string sent;
    int  connectCalls = 0;
    unsigned int lastAddr = 0;
    unsigned short lastPort = 0;
    bool lastKeepalives = false;
    int  resolveCalls = 0;
    std::string lastResolveName;
    pal::ResolveToken lastToken = -1;
    bool tlsStarted = false;
    std::string tlsServerName;
    bool tlsAllowInvalid = false;
    bool closed = false;

    void resolve(const char* name, pal::ResolveToken token) override {
        ++resolveCalls;
        lastResolveName = name ? name : "";
        lastToken = token;
    }
    void cancelResolve() override {}
    void connect(unsigned int addr, unsigned short port,
                 const pal::TransportOptions& opt) override {
        ++connectCalls;
        lastAddr = addr;
        lastPort = port;
        lastKeepalives = opt.keepalives;
    }
    void send(const char* data, size_t n) override { sent.append(data, n); }
    void startTls(const pal::TlsParams& p) override {
        tlsStarted = true;
        tlsServerName = p.serverName;
        tlsAllowInvalid = p.allowInvalidCert;
    }
    void close() override { closed = true; }
    std::string lastErrorString() const override { return "rec"; }
};

// ── helpers ────────────────────────────────────────────────────────────────

// Route can't-connect notifications into the output window (the Note path we
// can read back), and restore the App flags afterwards.
struct NotifyToOutput {
    BOOL savedNotify, savedToOutput;
    NotifyToOutput(bool notify = true, bool toOutput = true) {
        savedNotify   = App.m_bNotifyIfCannotConnect;
        savedToOutput = App.m_bErrorNotificationToOutputWindow;
        App.m_bNotifyIfCannotConnect          = notify ? TRUE : FALSE;
        App.m_bErrorNotificationToOutputWindow = toOutput ? TRUE : FALSE;
    }
    ~NotifyToOutput() {
        App.m_bNotifyIfCannotConnect          = savedNotify;
        App.m_bErrorNotificationToOutputWindow = savedToOutput;
    }
};

static CMUSHclientDoc* make_doc()
{
    CMUSHclientDoc* doc = new CMUSHclientDoc();
    doc->m_mush_name = "TestWorld";
    doc->m_server = "mud.example.com";
    doc->m_port = 4000;
    doc->m_iConnectPhase = eConnectConnectedToMud;
    doc->m_wrap = FALSE;          // keep long notification notes on one line
    doc->m_nWrapColumn = 500;     // (hard wrap fires at the column even so)
    doc->m_pTransport = new RecTransport();
    doc->Note("");   // retire the ctor-time 80-column line; fresh lines use 500
    return doc;
}

static RecTransport& wire(CMUSHclientDoc* doc)
{
    return *static_cast<RecTransport*>(doc->m_pTransport);
}

// Everything currently in the doc's line list, concatenated.
static std::string all_text(CMUSHclientDoc* doc)
{
    std::string out;
    for (POSITION pos = doc->m_LineList.GetHeadPosition(); pos; ) {
        CLine* l = doc->m_LineList.GetNext(pos);
        out.append(l->text, l->len);
        out += '\n';
    }
    return out;
}

static bool saw(CMUSHclientDoc* doc, const char* needle)
{
    return all_text(doc).find(needle) != std::string::npos;
}

// ── SOCKS proxy error paths ────────────────────────────────────────────────

TEST(Proxy, Response1GssapiRefusalNotesAndDisconnects)
{
    NotifyToOutput toOutput;
    CMUSHclientDoc* doc = make_doc();
    doc->m_iConnectPhase = eConnectAwaitingProxyResponse1;
    char buff[2] = { 5, 1 };   // version 5, method 1 = GSSAPI
    EXPECT_EQ(-1, doc->ProcessProxyResponse1(buff, 2));
    EXPECT_TRUE(saw(doc, "Proxy server cannot authenticate"));
    EXPECT_TRUE(saw(doc, "GSSAPI method not supported"));
    EXPECT_EQ((int)eConnectNotConnected, (int)doc->m_iConnectPhase);
    delete doc;
}

TEST(Proxy, Response1UnknownMethodRefusal)
{
    NotifyToOutput toOutput;
    CMUSHclientDoc* doc = make_doc();
    char buff[2] = { 5, (char)0xFF };   // 0xFF = "no acceptable methods"
    EXPECT_EQ(-1, doc->ProcessProxyResponse1(buff, 2));
    EXPECT_TRUE(saw(doc, "no acceptable methods"));
    delete doc;
}

TEST(Proxy, Response1WrongVersionByteIsUnexpectedResponse)
{
    NotifyToOutput toOutput;
    CMUSHclientDoc* doc = make_doc();
    char buff[2] = { 4, 0 };   // SOCKS5 reply must start with 5
    EXPECT_EQ(-1, doc->ProcessProxyResponse1(buff, 2));
    EXPECT_TRUE(saw(doc, "Unexpected proxy server response 4, expected 5"));
    delete doc;
}

TEST(Proxy, Response1AuthRequiredSendsUserPassPacket)
{
    CMUSHclientDoc* doc = make_doc();
    doc->m_strProxyUserName = "user";
    doc->m_strProxyPassword = "pw";
    char buff[2] = { 5, 2 };   // username/password required
    EXPECT_EQ(2, doc->ProcessProxyResponse1(buff, 2));
    EXPECT_EQ((int)eConnectAwaitingProxyResponse2, (int)doc->m_iConnectPhase);
    // RFC 1929 packet: 01 ulen "user" plen "pw"
    const std::string& s = wire(doc).sent;
    ASSERT_EQ((int)(2 + 4 + 1 + 2), (int)s.size());
    EXPECT_EQ(1, (int)s[0]);
    EXPECT_EQ(4, (int)s[1]);
    EXPECT_EQ(0, (int)s.compare(2, 4, "user"));
    EXPECT_EQ(2, (int)s[6]);
    delete doc;
}

TEST(Proxy, Response2RefusedAuthenticationNotes)
{
    NotifyToOutput toOutput;
    CMUSHclientDoc* doc = make_doc();
    char buff[2] = { 1, 5 };   // version 1 reply, nonzero status = refused
    EXPECT_EQ(-1, doc->ProcessProxyResponse2(buff, 2));
    EXPECT_TRUE(saw(doc, "Proxy server refused authentication"));
    delete doc;
}

TEST(Proxy, Response2SuccessAdvancesToPhase3)
{
    CMUSHclientDoc* doc = make_doc();
    doc->m_iSocksProcessing = eProxyServerSocks5;
    doc->m_sockAddr.sin_addr.s_addr = htonl(0x01020304u);
    doc->m_sockAddr.sin_port = htons(4000);
    char buff[2] = { 1, 0 };
    EXPECT_EQ(2, doc->ProcessProxyResponse2(buff, 2));
    EXPECT_EQ((int)eConnectAwaitingProxyResponse3, (int)doc->m_iConnectPhase);
    EXPECT_TRUE(wire(doc).sent.size() >= 10);   // SOCKS5 CONNECT went out
    delete doc;
}

TEST(Proxy, Response3Socks4ErrorReasonsAreSpelledOut)
{
    struct { int code; const char* text; } cases[] = {
        { 91, "request rejected or failed" },
        { 92, "cannot connect to ident server" },
        { 93, "client and identd report different user ids" },
        { 94, "unknown error reason" },
    };
    for (auto& c : cases) {
        NotifyToOutput toOutput;
        CMUSHclientDoc* doc = make_doc();
        doc->m_iSocksProcessing = eProxyServerSocks4;
        char buff[8] = { 0, (char)c.code };
        EXPECT_EQ(-1, doc->ProcessProxyResponse3(buff, 8));
        EXPECT_TRUE(saw(doc, "Proxy server refused connection"));
        EXPECT_TRUE(saw(doc, c.text));
        delete doc;
    }
}

TEST(Proxy, Response3Socks4ShortReplyBecomesRequestFailed)
{
    NotifyToOutput toOutput;
    CMUSHclientDoc* doc = make_doc();
    doc->m_iSocksProcessing = eProxyServerSocks4;
    char buff[8] = { 0 };   // count < 2 -> treated as code 91
    EXPECT_EQ(-1, doc->ProcessProxyResponse3(buff, 1));
    EXPECT_TRUE(saw(doc, "request rejected or failed"));
    delete doc;
}

TEST(Proxy, Response3Socks5ErrorReasonsAreSpelledOut)
{
    struct { int code; const char* text; } cases[] = {
        { 1, "general SOCKS server failure" },
        { 2, "connection not allowed by ruleset" },
        { 3, "Network unreachable" },
        { 4, "Host unreachable" },
        { 5, "Connection refused" },
        { 6, "TTL expired" },
        { 7, "Command not supported" },
        { 8, "Address type not supported" },
        { 9, "unknown error reason" },
    };
    for (auto& c : cases) {
        NotifyToOutput toOutput;
        CMUSHclientDoc* doc = make_doc();
        doc->m_iSocksProcessing = eProxyServerSocks5;
        char buff[10] = { 5, (char)c.code };
        EXPECT_EQ(-1, doc->ProcessProxyResponse3(buff, 10));
        EXPECT_TRUE(saw(doc, c.text));
        delete doc;
    }
}

TEST(Proxy, Response3Socks4SuccessEstablishesConnection)
{
    CMUSHclientDoc* doc = make_doc();
    doc->m_iSocksProcessing = eProxyServerSocks4;
    doc->m_iConnectPhase = eConnectAwaitingProxyResponse3;
    char buff[8] = { 0, 90 };   // SOCKS4 granted
    EXPECT_EQ(8, doc->ProcessProxyResponse3(buff, 8));
    EXPECT_EQ((int)eConnectConnectedToMud, (int)doc->m_iConnectPhase);
    delete doc;
}

TEST(Proxy, Response3SuccessWithSslStartsHandshake)
{
    CMUSHclientDoc* doc = make_doc();
    doc->m_iSocksProcessing = eProxyServerSocks5;
    doc->m_bUseSSL = TRUE;
    char buff[10] = { 5, 0 };
    EXPECT_EQ(10, doc->ProcessProxyResponse3(buff, 10));
    EXPECT_TRUE(wire(doc).tlsStarted);
    EXPECT_STREQ("mud.example.com", wire(doc).tlsServerName.c_str());
    EXPECT_EQ((int)eConnectAwaitingSSLHandshake, (int)doc->m_iConnectPhase);
    delete doc;
}

TEST(Proxy, NotificationSuppressedWhenDisabled)
{
    NotifyToOutput quiet(false, true);   // notifications off
    CMUSHclientDoc* doc = make_doc();
    char buff[2] = { 5, 1 };
    EXPECT_EQ(-1, doc->ProcessProxyResponse1(buff, 2));
    EXPECT_FALSE(saw(doc, "Proxy server"));   // silent
    delete doc;
}

// ── connection status / socket errors ──────────────────────────────────────

TEST(ConnectStatus, AllPhasesHaveNames)
{
    EXPECT_STREQ("Closed", (LPCTSTR)GetConnectionStatus(eConnectNotConnected));
    EXPECT_STREQ("Look up world name", (LPCTSTR)GetConnectionStatus(eConnectMudNameLookup));
    EXPECT_STREQ("Look up proxy name", (LPCTSTR)GetConnectionStatus(eConnectProxyNameLookup));
    EXPECT_STREQ("Connecting to world", (LPCTSTR)GetConnectionStatus(eConnectConnectingToMud));
    EXPECT_STREQ("Connecting to proxy", (LPCTSTR)GetConnectionStatus(eConnectConnectingToProxy));
    EXPECT_STREQ("Awaiting proxy response (1)", (LPCTSTR)GetConnectionStatus(eConnectAwaitingProxyResponse1));
    EXPECT_STREQ("Awaiting proxy response (2)", (LPCTSTR)GetConnectionStatus(eConnectAwaitingProxyResponse2));
    EXPECT_STREQ("Awaiting proxy response (3)", (LPCTSTR)GetConnectionStatus(eConnectAwaitingProxyResponse3));
    EXPECT_STREQ("Performing TLS handshake", (LPCTSTR)GetConnectionStatus(eConnectAwaitingSSLHandshake));
    EXPECT_STREQ("Open", (LPCTSTR)GetConnectionStatus(eConnectConnectedToMud));
    EXPECT_STREQ("Disconnecting", (LPCTSTR)GetConnectionStatus(eConnectDisconnecting));
    EXPECT_STREQ("<unknown>", (LPCTSTR)GetConnectionStatus(999));
}

TEST(ConnectStatus, SocketErrorUsesStrerror)
{
    CMUSHclientDoc* doc = make_doc();
    EXPECT_STREQ(strerror(ECONNREFUSED), (LPCTSTR)doc->GetSocketError(ECONNREFUSED));
    EXPECT_STREQ(strerror(ETIMEDOUT), (LPCTSTR)doc->GetSocketError(ETIMEDOUT));
    delete doc;
}

// ── OnConnect failure notification ─────────────────────────────────────────

TEST(ConnectFail, NotifiesWithPhaseAndAddress)
{
    NotifyToOutput toOutput;
    CMUSHclientDoc* doc = make_doc();
    doc->m_iConnectPhase = eConnectConnectingToMud;
    doc->m_sockAddr.sin_addr.s_addr = inet_addr("10.11.12.13");
    doc->OnConnect(ECONNREFUSED);
    EXPECT_EQ((int)eConnectNotConnected, (int)doc->m_iConnectPhase);
    EXPECT_TRUE(saw(doc, "Unable to connect to \"TestWorld\""));
    EXPECT_TRUE(saw(doc, strerror(ECONNREFUSED)));
    EXPECT_TRUE(saw(doc, "Connecting to world"));
    EXPECT_TRUE(saw(doc, "10.11.12.13"));
    EXPECT_TRUE(saw(doc, "port 4000"));
    EXPECT_TRUE(saw(doc, "?id=8369"));   // forum-help pointer
    delete doc;
}

TEST(ConnectFail, ProxyPhaseReportsProxyAddress)
{
    NotifyToOutput toOutput;
    CMUSHclientDoc* doc = make_doc();
    doc->m_iConnectPhase = eConnectConnectingToProxy;
    doc->m_iProxyServerPort = 1080;
    doc->m_ProxyAddr.sin_addr.s_addr = inet_addr("192.168.1.99");
    doc->OnConnect(ETIMEDOUT);
    EXPECT_TRUE(saw(doc, "Connecting to proxy"));
    EXPECT_TRUE(saw(doc, "192.168.1.99"));
    EXPECT_TRUE(saw(doc, "port 1080"));
    delete doc;
}

TEST(ConnectFail, MessageBoxPathAndSuppressedPath)
{
    {   // message-box branch (headless UMessageBox -> stderr) still resets phase
        NotifyToOutput toBox(true, false);
        CMUSHclientDoc* doc = make_doc();
        doc->m_iConnectPhase = eConnectConnectingToMud;
        doc->OnConnect(ECONNREFUSED);
        EXPECT_EQ((int)eConnectNotConnected, (int)doc->m_iConnectPhase);
        EXPECT_FALSE(saw(doc, "Unable to connect"));   // not in output window
        delete doc;
    }
    {   // notifications disabled entirely
        NotifyToOutput quiet(false, true);
        CMUSHclientDoc* doc = make_doc();
        doc->m_iConnectPhase = eConnectConnectingToMud;
        doc->OnConnect(ECONNREFUSED);
        EXPECT_EQ((int)eConnectNotConnected, (int)doc->m_iConnectPhase);
        EXPECT_FALSE(saw(doc, "Unable to connect"));
        delete doc;
    }
}

TEST(ConnectFail, SuccessWithSslGoesToHandshake)
{
    CMUSHclientDoc* doc = make_doc();
    doc->m_iSocksProcessing = eProxyServerNone;
    doc->m_bUseSSL = TRUE;
    doc->m_bSSLAllowInvalidCert = TRUE;
    doc->OnConnect(0);
    EXPECT_TRUE(wire(doc).tlsStarted);
    EXPECT_TRUE(wire(doc).tlsAllowInvalid);
    EXPECT_EQ((int)eConnectAwaitingSSLHandshake, (int)doc->m_iConnectPhase);
    delete doc;
}

// ── DNS completion ─────────────────────────────────────────────────────────

TEST(Lookup, FailureNotesUnresolvedHostName)
{
    NotifyToOutput toOutput;
    CMUSHclientDoc* doc = make_doc();
    doc->m_iConnectPhase = eConnectMudNameLookup;
    struct in_addr none = {};
    doc->LookupHostNameComplete(none, eConnectMudNameLookup, false);
    EXPECT_EQ((int)eConnectNotConnected, (int)doc->m_iConnectPhase);
    EXPECT_TRUE(saw(doc, "Unable to resolve host name for \"mud.example.com\""));
    delete doc;
}

TEST(Lookup, ProxyFailureNamesTheProxy)
{
    NotifyToOutput toOutput;
    CMUSHclientDoc* doc = make_doc();
    doc->m_strProxyServerName = "proxy.example.com";
    doc->m_iConnectPhase = eConnectProxyNameLookup;
    struct in_addr none = {};
    doc->LookupHostNameComplete(none, eConnectProxyNameLookup, false);
    EXPECT_TRUE(saw(doc, "Unable to resolve host name for \"proxy.example.com\""));
    delete doc;
}

TEST(Lookup, StaleCompletionIsDropped)
{
    NotifyToOutput toOutput;
    CMUSHclientDoc* doc = make_doc();
    doc->m_iConnectPhase = eConnectConnectedToMud;   // user reconnected already
    struct in_addr none = {};
    doc->LookupHostNameComplete(none, eConnectMudNameLookup, false);   // stale
    EXPECT_EQ((int)eConnectConnectedToMud, (int)doc->m_iConnectPhase); // untouched
    EXPECT_FALSE(saw(doc, "Unable to resolve"));
    delete doc;
}

TEST(Lookup, MudSuccessInitiatesConnection)
{
    CMUSHclientDoc* doc = make_doc();
    doc->m_iSocksProcessing = eProxyServerNone;
    doc->m_iConnectPhase = eConnectMudNameLookup;
    doc->m_bSendKeepAlives = TRUE;
    struct in_addr addr;
    addr.s_addr = inet_addr("10.0.0.1");
    doc->LookupHostNameComplete(addr, eConnectMudNameLookup, true);
    EXPECT_EQ((int)eConnectConnectingToMud, (int)doc->m_iConnectPhase);
    EXPECT_EQ(1, wire(doc).connectCalls);
    EXPECT_EQ(addr.s_addr, wire(doc).lastAddr);
    EXPECT_EQ((unsigned short)4000, wire(doc).lastPort);
    EXPECT_TRUE(wire(doc).lastKeepalives);
    delete doc;
}

TEST(Lookup, MudSuccessWithUnresolvedProxyResolvesProxyNext)
{
    CMUSHclientDoc* doc = make_doc();
    doc->m_iSocksProcessing = eProxyServerSocks5;
    doc->m_strProxyServerName = "proxy.example.com";   // not a dotted quad
    doc->m_iProxyServerPort = 1080;
    doc->m_iConnectPhase = eConnectMudNameLookup;
    doc->m_ProxyAddr.sin_family = AF_INET;
    doc->m_ProxyAddr.sin_addr.s_addr = INADDR_NONE;
    struct in_addr addr;
    addr.s_addr = inet_addr("10.0.0.1");
    doc->LookupHostNameComplete(addr, eConnectMudNameLookup, true);
    // proxy name is non-numeric -> async re-resolve through the transport
    EXPECT_EQ((int)eConnectProxyNameLookup, (int)doc->m_iConnectPhase);
    EXPECT_EQ(1, wire(doc).resolveCalls);
    EXPECT_STREQ("proxy.example.com", wire(doc).lastResolveName.c_str());
    EXPECT_EQ((pal::ResolveToken)eConnectProxyNameLookup, wire(doc).lastToken);
    delete doc;
}

TEST(Lookup, ProxySuccessConnectsToProxy)
{
    CMUSHclientDoc* doc = make_doc();
    doc->m_iSocksProcessing = eProxyServerSocks5;
    doc->m_iProxyServerPort = 1080;
    doc->m_iConnectPhase = eConnectProxyNameLookup;
    struct in_addr addr;
    addr.s_addr = inet_addr("172.16.0.1");
    doc->LookupHostNameComplete(addr, eConnectProxyNameLookup, true);
    EXPECT_EQ((int)eConnectConnectingToProxy, (int)doc->m_iConnectPhase);
    EXPECT_EQ(1, wire(doc).connectCalls);
    EXPECT_EQ((unsigned short)1080, wire(doc).lastPort);
    delete doc;
}

// ── TLS outcomes ───────────────────────────────────────────────────────────

TEST(Tls, EstablishedNotesVersionAndCipher)
{
    CMUSHclientDoc* doc = make_doc();
    doc->m_iConnectPhase = eConnectAwaitingSSLHandshake;
    doc->OnTlsEstablished("TLSv1.3", "TLS_AES_256_GCM_SHA384", nullptr);
    EXPECT_TRUE(doc->m_bSSL_Connected);
    EXPECT_TRUE(saw(doc, "--- Connected via TLSv1.3 (TLS_AES_256_GCM_SHA384) ---"));
    EXPECT_EQ((int)eConnectConnectedToMud, (int)doc->m_iConnectPhase);
    delete doc;
}

TEST(Tls, EstablishedWithUnverifiedCertWarns)
{
    CMUSHclientDoc* doc = make_doc();
    doc->OnTlsEstablished("TLSv1.2", "AES128-SHA", "self signed certificate");
    EXPECT_TRUE(saw(doc, "WARNING: proceeding with UNVERIFIED TLS certificate"));
    EXPECT_TRUE(saw(doc, "self signed certificate"));
    EXPECT_TRUE(doc->m_bSSL_Connected);
    delete doc;
}

TEST(Tls, CertVerifyFailureBlocksAndRecordsReason)
{
    CMUSHclientDoc* doc = make_doc();
    doc->OnTlsFailed(true, "unable to get local issuer certificate");
    EXPECT_TRUE(saw(doc, "TLS certificate for \"mud.example.com\" could NOT be verified"));
    EXPECT_STREQ("unable to get local issuer certificate",
                 (LPCTSTR)doc->m_strSSLLastError);
    EXPECT_EQ((int)eConnectNotConnected, (int)doc->m_iConnectPhase);
    delete doc;
}

TEST(Tls, HandshakeFailureNotes)
{
    CMUSHclientDoc* doc = make_doc();
    doc->OnTlsFailed(false, "wrong version number");
    EXPECT_TRUE(saw(doc, "--- TLS handshake failed: wrong version number ---"));
    delete doc;
}

// ── ConnectSocket validation ───────────────────────────────────────────────

TEST(ConnectSocket, RefusesBlankConfiguration)
{
    CMUSHclientDoc* doc = make_doc();
    doc->m_server = "0.0.0.0";
    EXPECT_FALSE(doc->ConnectSocket());   // silent refusal

    doc->m_server = "mud.example.com";
    doc->m_mush_name = "";
    EXPECT_FALSE(doc->ConnectSocket());   // no world name

    doc->m_mush_name = "TestWorld";
    doc->m_server = "";
    EXPECT_FALSE(doc->ConnectSocket());   // no address

    doc->m_server = "mud.example.com";
    doc->m_port = 0;
    EXPECT_FALSE(doc->ConnectSocket());   // no port
    delete doc;
}

TEST(ConnectSocket, RefusesBadProxyConfiguration)
{
    CMUSHclientDoc* doc = make_doc();
    doc->m_iSocksProcessing = eProxyServerSocks5;
    doc->m_strProxyServerName = "";
    EXPECT_FALSE(doc->ConnectSocket());   // blank proxy address

    doc->m_strProxyServerName = "proxy.example.com";
    doc->m_iProxyServerPort = 0;
    EXPECT_FALSE(doc->ConnectSocket());   // no proxy port

    doc->m_iSocksProcessing = 99;
    EXPECT_FALSE(doc->ConnectSocket());   // unknown proxy type
    delete doc;
}

// ── SendPacket ─────────────────────────────────────────────────────────────

TEST(SendPacket, CountsBytesAndPacketsThroughTransport)
{
    CMUSHclientDoc* doc = make_doc();
    long packetsBefore = doc->m_iOutputPacketCount;
    doc->SendPacket("hello", 5);
    doc->SendPacket((const unsigned char*)"!", 1);
    EXPECT_STREQ("hello!", wire(doc).sent.c_str());
    EXPECT_EQ(packetsBefore + 2, doc->m_iOutputPacketCount);
    EXPECT_EQ((LONGLONG)6, (LONGLONG)doc->m_nBytesOut);
    // Null transport: silently dropped
    delete doc->m_pTransport;
    doc->m_pTransport = nullptr;
    doc->SendPacket("dropped", 7);
    EXPECT_EQ((LONGLONG)6, (LONGLONG)doc->m_nBytesOut);
    delete doc;
}

// ── ConnectionEstablished login sequences ──────────────────────────────────

TEST(Login, ConnectMushSendsConnectLine)
{
    CMUSHclientDoc* doc = make_doc();
    doc->m_connect_now = eConnectMUSH;
    doc->m_name = "Gandalf";
    doc->m_password = "mellon";
    doc->ConnectionEstablished();
    EXPECT_TRUE(wire(doc).sent.find("connect Gandalf mellon") != std::string::npos);
    EXPECT_EQ((int)eConnectConnectedToMud, (int)doc->m_iConnectPhase);
    delete doc;
}

TEST(Login, ConnectDikuSendsNameThenPassword)
{
    CMUSHclientDoc* doc = make_doc();
    doc->m_connect_now = eConnectDiku;
    doc->m_name = "Frodo";
    doc->m_password = "ring";
    doc->ConnectionEstablished();
    size_t namePos = wire(doc).sent.find("Frodo");
    size_t passPos = wire(doc).sent.find("ring");
    EXPECT_TRUE(namePos != std::string::npos);
    EXPECT_TRUE(passPos != std::string::npos);
    EXPECT_TRUE(namePos < passPos);
    delete doc;
}

TEST(Login, ConnectTextSubstitutesNameAndPassword)
{
    CMUSHclientDoc* doc = make_doc();
    doc->m_connect_now = eNoAutoConnect;   // "do nothing" arm of the switch
    doc->m_name = "Sam";
    doc->m_password = "potato";
    doc->m_connect_text = "login %name% %password%";
    doc->ConnectionEstablished();
    EXPECT_TRUE(wire(doc).sent.find("login Sam potato") != std::string::npos);
    delete doc;
}

TEST(Login, NawsOfferSentWhenEnabled)
{
    CMUSHclientDoc* doc = make_doc();
    doc->m_bNAWS = TRUE;
    doc->ConnectionEstablished();
    char naws[3] = { (char)T_IAC, (char)T_WILL, (char)31 /*TELOPT_NAWS*/ };
    EXPECT_TRUE(wire(doc).sent.find(std::string(naws, 3)) != std::string::npos);
    delete doc;
}

TEST(Login, AutoLogWritesPreambleAndWorldName)
{
    char path[128];
    snprintf(path, sizeof path, "/tmp/mc_test_conn_%d_auto.log", (int)getpid());
    CMUSHclientDoc* doc = make_doc();
    doc->m_strAutoLogFileName = path;
    doc->m_strLogFilePreamble = "PREAMBLE%nLINE2";
    doc->m_bWriteWorldNameToLog = TRUE;
    doc->ConnectionEstablished();
    EXPECT_TRUE(doc->m_logfile != nullptr);
    EXPECT_EQ((long)eOK, doc->CloseLog());
    FILE* f = fopen(path, "rb");
    ASSERT_TRUE(f != nullptr);
    char buf[4096] = {};
    fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    std::string log = buf;
    EXPECT_TRUE(log.find("PREAMBLE\nLINE2") != std::string::npos);
    EXPECT_TRUE(log.find("TestWorld - ") != std::string::npos);
    EXPECT_TRUE(log.find("----") != std::string::npos);   // hyphen underline
    unlink(path);
    delete doc;
}

TEST(Login, AutoLogHtmlModeUsesBreaks)
{
    char path[128];
    snprintf(path, sizeof path, "/tmp/mc_test_conn_%d_auto.html", (int)getpid());
    CMUSHclientDoc* doc = make_doc();
    doc->m_strAutoLogFileName = path;
    doc->m_bLogHTML = TRUE;
    doc->m_bWriteWorldNameToLog = TRUE;
    doc->ConnectionEstablished();
    EXPECT_EQ((long)eOK, doc->CloseLog());
    FILE* f = fopen(path, "rb");
    ASSERT_TRUE(f != nullptr);
    char buf[4096] = {};
    fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    std::string log = buf;
    EXPECT_TRUE(log.find("<br>") != std::string::npos);
    EXPECT_TRUE(log.find("<br><br>") != std::string::npos);
    unlink(path);
    delete doc;
}

// ── logging: CloseLog postamble + LogCommand colour spans ──────────────────

TEST(Logging, ClosePostambleAndDoubleCloseCode)
{
    char path[128];
    snprintf(path, sizeof path, "/tmp/mc_test_conn_%d_close.log", (int)getpid());
    CMUSHclientDoc* doc = make_doc();
    ASSERT_EQ((long)eOK, doc->OpenLog(path, FALSE));
    doc->m_strLogFilePostamble = "END%nOF LOG";
    EXPECT_EQ((long)eOK, doc->CloseLog());
    EXPECT_EQ((long)eLogFileNotOpen, doc->CloseLog());   // already closed
    FILE* f = fopen(path, "rb");
    ASSERT_TRUE(f != nullptr);
    char buf[512] = {};
    fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    EXPECT_TRUE(strstr(buf, "END\nOF LOG") != nullptr);
    unlink(path);
    delete doc;
}

TEST(Logging, LogCommandStripsEndlineAndAppliesPreamble)
{
    char path[128];
    snprintf(path, sizeof path, "/tmp/mc_test_conn_%d_cmd.log", (int)getpid());
    CMUSHclientDoc* doc = make_doc();
    ASSERT_EQ((long)eOK, doc->OpenLog(path, FALSE));
    doc->m_log_input = 1;   // LoggingInput()
    doc->m_strLogLinePreambleInput = ">> ";
    doc->m_strLogLinePostambleInput = " <<";
    doc->LogCommand("look\r\n");
    doc->CloseLog();
    FILE* f = fopen(path, "rb");
    ASSERT_TRUE(f != nullptr);
    char buf[512] = {};
    fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    EXPECT_TRUE(strstr(buf, ">> look <<\n") != nullptr);
    unlink(path);
    delete doc;
}

TEST(Logging, LogCommandHtmlColourSpan)
{
    char path[128];
    snprintf(path, sizeof path, "/tmp/mc_test_conn_%d_html.log", (int)getpid());
    CMUSHclientDoc* doc = make_doc();
    ASSERT_EQ((long)eOK, doc->OpenLog(path, FALSE));
    doc->m_log_input = 1;
    doc->m_bLogHTML = TRUE;
    doc->m_bLogInColour = TRUE;
    doc->m_echo_colour = 0;                       // custom colour slot 0
    doc->m_customtext[0] = RGB(0x12, 0x34, 0x56);
    doc->LogCommand("n<&>\r\n");                  // needs HTML fixing too
    doc->CloseLog();
    FILE* f = fopen(path, "rb");
    ASSERT_TRUE(f != nullptr);
    char buf[512] = {};
    fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    std::string log = buf;
    EXPECT_TRUE(log.find("<font color=\"#123456\">") != std::string::npos);
    EXPECT_TRUE(log.find("</font>") != std::string::npos);
    EXPECT_TRUE(log.find("&lt;&amp;&gt;") != std::string::npos);   // FixHTMLString
    unlink(path);
    delete doc;
}

TEST(Logging, LogCommandSkippedWhenInputLoggingOff)
{
    char path[128];
    snprintf(path, sizeof path, "/tmp/mc_test_conn_%d_off.log", (int)getpid());
    CMUSHclientDoc* doc = make_doc();
    ASSERT_EQ((long)eOK, doc->OpenLog(path, FALSE));
    doc->m_log_input = 0;
    doc->LogCommand("secret\r\n");
    doc->CloseLog();
    FILE* f = fopen(path, "rb");
    ASSERT_TRUE(f != nullptr);
    char buf[128] = {};
    fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    EXPECT_TRUE(strstr(buf, "secret") == nullptr);
    unlink(path);
    delete doc;
}

// ── MCCP v2 decompression ──────────────────────────────────────────────────

// Deflate `plain` (Z_SYNC_FLUSH; optionally finished) for feeding the doc.
static std::string deflate_bytes(const std::string& plain, bool finish)
{
    z_stream z = {};
    deflateInit(&z, Z_DEFAULT_COMPRESSION);
    std::string out;
    out.resize(plain.size() + 512);
    z.next_in = (Bytef*)plain.data();
    z.avail_in = (uInt)plain.size();
    z.next_out = (Bytef*)&out[0];
    z.avail_out = (uInt)out.size();
    deflate(&z, finish ? Z_FINISH : Z_SYNC_FLUSH);
    out.resize(out.size() - z.avail_out);
    deflateEnd(&z);
    return out;
}

// Negotiate MCCP2 on `doc` and switch the stream to compressed mode.
static void start_mccp2(CMUSHclientDoc* doc)
{
    unsigned char will[3] = { T_IAC, T_WILL, 86 /*COMPRESS2*/ };
    doc->OnBytesReceived((char*)will, 3);
    // client must have agreed: IAC DO COMPRESS2
    unsigned char expectDo[3] = { T_IAC, 253, 86 };
    EXPECT_TRUE(wire(doc).sent.find(std::string((char*)expectDo, 3)) != std::string::npos);
    unsigned char sb[5] = { T_IAC, T_SB, 86, T_IAC, T_SE };
    doc->OnBytesReceived((char*)sb, 5);
    EXPECT_TRUE(doc->m_bCompress);
}

TEST(Mccp, NegotiatesAndDecompressesStream)
{
    CMUSHclientDoc* doc = make_doc();
    start_mccp2(doc);
    std::string z = deflate_bytes("Hello from the compressed MUD\r\n", false);
    doc->OnBytesReceived(&z[0], (int)z.size());
    EXPECT_TRUE(saw(doc, "Hello from the compressed MUD"));
    EXPECT_TRUE(doc->m_bCompress);              // still streaming
    EXPECT_GT((long)doc->m_nTotalCompressed, 0L);
    EXPECT_GT((long)doc->m_nTotalUncompressed, 0L);
    delete doc;
}

TEST(Mccp, CompressedDataInTheSamePacketAsSE)
{
    // The special case the DisplayMsg arms handle: compressed bytes arrive in
    // the SAME packet, immediately after IAC SB COMPRESS2 IAC SE.
    CMUSHclientDoc* doc = make_doc();
    unsigned char will[3] = { T_IAC, T_WILL, 86 };
    doc->OnBytesReceived((char*)will, 3);
    std::string z = deflate_bytes("inline start\r\n", false);
    std::string packet;
    unsigned char sb[5] = { T_IAC, T_SB, 86, T_IAC, T_SE };
    packet.append((char*)sb, 5);
    packet += z;
    doc->OnBytesReceived(&packet[0], (int)packet.size());
    EXPECT_TRUE(doc->m_bCompress);
    EXPECT_TRUE(saw(doc, "inline start"));
    delete doc;
}

TEST(Mccp, StreamEndTurnsCompressionOff)
{
    CMUSHclientDoc* doc = make_doc();
    start_mccp2(doc);
    std::string z = deflate_bytes("last compressed line\r\n", true);   // Z_FINISH
    z += "plain after stream\r\n";   // MCCP contract: plain text follows the end
    doc->OnBytesReceived(&z[0], (int)z.size());
    EXPECT_FALSE(doc->m_bCompress);
    EXPECT_TRUE(saw(doc, "last compressed line"));
    EXPECT_TRUE(saw(doc, "plain after stream"));
    delete doc;
}

TEST(Mccp, LargePayloadDecompressesAcrossManyPasses)
{
    // A very compressible payload much larger than the output buffer drives
    // the decompression loop through many display/reset passes.  (The
    // Z_BUF_ERROR grow-the-buffer arm itself is defensive: the loop resets
    // avail_out after every pass, so inflate always has room — skip list.)
    CMUSHclientDoc* doc = make_doc();
    start_mccp2(doc);
    std::string big(300000, 'A');
    big += "\r\n";
    std::string z = deflate_bytes(big, false);
    doc->OnBytesReceived(&z[0], (int)z.size());
    EXPECT_EQ((long long)big.size(), (long long)doc->m_nTotalUncompressed);
    delete doc;
}

TEST(Mccp, CorruptStreamDisconnectsWithMessage)
{
    NotifyToOutput toOutput;
    CMUSHclientDoc* doc = make_doc();
    start_mccp2(doc);
    char garbage[] = "this is definitely not a zlib stream!!";
    doc->OnBytesReceived(garbage, (int)sizeof garbage - 1);
    EXPECT_EQ((int)eConnectNotConnected, (int)doc->m_iConnectPhase);   // disconnected
    delete doc;
}

TEST(Mccp, RawLoggingCapturesDecompressedText)
{
    char path[128];
    snprintf(path, sizeof path, "/tmp/mc_test_conn_%d_raw.log", (int)getpid());
    CMUSHclientDoc* doc = make_doc();
    ASSERT_EQ((long)eOK, doc->OpenLog(path, FALSE));
    doc->m_bLogRaw = TRUE;
    start_mccp2(doc);
    std::string z = deflate_bytes("raw logged\r\n", false);
    doc->OnBytesReceived(&z[0], (int)z.size());
    doc->m_bLogRaw = FALSE;
    doc->CloseLog();
    FILE* f = fopen(path, "rb");
    ASSERT_TRUE(f != nullptr);
    char buf[512] = {};
    fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    EXPECT_TRUE(strstr(buf, "raw logged") != nullptr);
    unlink(path);
    delete doc;
}

TEST(Mccp, DataDiscardedWhenNotConnected)
{
    CMUSHclientDoc* doc = make_doc();
    doc->m_iConnectPhase = eConnectConnectingToMud;   // not connected yet
    char text[] = "should vanish\r\n";
    long packetsBefore = doc->m_iInputPacketCount;
    doc->OnBytesReceived(text, (int)sizeof text - 1);
    EXPECT_EQ(packetsBefore, doc->m_iInputPacketCount);
    EXPECT_FALSE(saw(doc, "should vanish"));
    delete doc;
}

// ── ShiftTabCompleteItem ───────────────────────────────────────────────────

TEST(TabComplete, ShiftTabCompleteItemContract)
{
    CMUSHclientDoc* doc = make_doc();
    EXPECT_EQ((long)eOK, doc->ShiftTabCompleteItem("candidate"));
    EXPECT_EQ((size_t)1, doc->m_ExtraShiftTabCompleteItems.size());
    EXPECT_EQ((long)eBadParameter, doc->ShiftTabCompleteItem(""));
    EXPECT_EQ((long)eBadParameter,
              doc->ShiftTabCompleteItem("0123456789012345678901234567890"));  // 31 chars
    EXPECT_EQ((long)eBadParameter, doc->ShiftTabCompleteItem("bad name!"));
    EXPECT_EQ((long)eOK, doc->ShiftTabCompleteItem("<functions>"));
    EXPECT_TRUE(doc->m_bTabCompleteFunctions);
    EXPECT_EQ((long)eOK, doc->ShiftTabCompleteItem("<nofunctions>"));
    EXPECT_FALSE(doc->m_bTabCompleteFunctions);
    EXPECT_EQ((long)eOK, doc->ShiftTabCompleteItem("<clear>"));
    EXPECT_TRUE(doc->m_ExtraShiftTabCompleteItems.empty());
    delete doc;
}

// ── GetStyleRGB ────────────────────────────────────────────────────────────

static CStyle make_style(unsigned short flags, COLORREF fore, COLORREF back)
{
    CStyle s;
    s.iFlags = flags;
    s.iForeColour = fore;
    s.iBackColour = back;
    return s;
}

TEST(StyleRgb, CustomColourAndInverse)
{
    CMUSHclientDoc* doc = make_doc();
    doc->m_customtext[3] = RGB(1, 2, 3);
    doc->m_customback[3] = RGB(4, 5, 6);
    CStyle s = make_style(COLOUR_CUSTOM, 3, 0);
    COLORREF fg = 0, bg = 0;
    doc->GetStyleRGB(&s, fg, bg);
    EXPECT_EQ(RGB(1, 2, 3), fg);
    EXPECT_EQ(RGB(4, 5, 6), bg);
    s.iFlags |= INVERSE;
    doc->GetStyleRGB(&s, fg, bg);
    EXPECT_EQ(RGB(4, 5, 6), fg);   // swapped
    EXPECT_EQ(RGB(1, 2, 3), bg);
    delete doc;
}

TEST(StyleRgb, RgbColourAndInverse)
{
    CMUSHclientDoc* doc = make_doc();
    CStyle s = make_style(COLOUR_RGB, RGB(10, 20, 30), RGB(40, 50, 60));
    COLORREF fg = 0, bg = 0;
    doc->GetStyleRGB(&s, fg, bg);
    EXPECT_EQ(RGB(10, 20, 30), fg);
    EXPECT_EQ(RGB(40, 50, 60), bg);
    s.iFlags |= INVERSE;
    doc->GetStyleRGB(&s, fg, bg);
    EXPECT_EQ(RGB(40, 50, 60), fg);
    EXPECT_EQ(RGB(10, 20, 30), bg);
    delete doc;
}

TEST(StyleRgb, AnsiClassicInverseAndHilite)
{
    CMUSHclientDoc* doc = make_doc();
    doc->m_bAlternativeInverse = FALSE;
    doc->m_normalcolour[1] = RGB(100, 0, 0);
    doc->m_normalcolour[2] = RGB(0, 100, 0);
    doc->m_boldcolour[1] = RGB(200, 0, 0);
    COLORREF fg = 0, bg = 0;

    CStyle plain = make_style(COLOUR_ANSI, 1, 2);
    doc->GetStyleRGB(&plain, fg, bg);
    EXPECT_EQ(RGB(100, 0, 0), fg);
    EXPECT_EQ(RGB(0, 100, 0), bg);

    CStyle bold = make_style(COLOUR_ANSI | HILITE, 1, 2);
    doc->GetStyleRGB(&bold, fg, bg);
    EXPECT_EQ(RGB(200, 0, 0), fg);   // bold palette for hilite fg

    CStyle inverse = make_style(COLOUR_ANSI | INVERSE, 1, 2);
    doc->GetStyleRGB(&inverse, fg, bg);
    EXPECT_EQ(RGB(0, 100, 0), fg);   // swapped: fg from back colour
    EXPECT_EQ(RGB(100, 0, 0), bg);

    // xterm-256 indices bypass the palettes entirely
    CStyle xterm = make_style(COLOUR_ANSI, 196, 21);
    doc->GetStyleRGB(&xterm, fg, bg);
    EXPECT_EQ(xterm_256_colours[196], fg);
    EXPECT_EQ(xterm_256_colours[21], bg);
    delete doc;
}

TEST(StyleRgb, AnsiAlternativeInverse)
{
    CMUSHclientDoc* doc = make_doc();
    doc->m_bAlternativeInverse = TRUE;
    doc->m_normalcolour[1] = RGB(100, 0, 0);
    doc->m_normalcolour[2] = RGB(0, 100, 0);
    doc->m_boldcolour[1] = RGB(200, 0, 0);
    COLORREF fg = 0, bg = 0;

    // alternative inverse keeps the fore colour boldable while swapping roles
    CStyle inv = make_style(COLOUR_ANSI | INVERSE | HILITE, 1, 2);
    doc->GetStyleRGB(&inv, fg, bg);
    EXPECT_EQ(RGB(200, 0, 0), bg);   // hilited fore colour becomes background
    EXPECT_EQ(RGB(0, 100, 0), fg);   // back colour becomes foreground

    // xterm indices on the alternative-inverse path
    CStyle invx = make_style(COLOUR_ANSI | INVERSE, 196, 21);
    doc->GetStyleRGB(&invx, fg, bg);
    EXPECT_EQ(xterm_256_colours[196], bg);
    EXPECT_EQ(xterm_256_colours[21], fg);

    // non-inverse alternative path
    CStyle plain = make_style(COLOUR_ANSI | HILITE, 1, 21);
    doc->GetStyleRGB(&plain, fg, bg);
    EXPECT_EQ(RGB(200, 0, 0), fg);
    EXPECT_EQ(xterm_256_colours[21], bg);
    delete doc;
}

TEST(StyleRgb, RememberStyleStoresFlagsAndColours)
{
    CMUSHclientDoc* doc = make_doc();
    CStyle s = make_style(COLOUR_RGB | HILITE, RGB(9, 9, 9), RGB(7, 7, 7));
    doc->RememberStyle(&s);
    EXPECT_EQ((int)(COLOUR_RGB | HILITE), (int)(doc->m_iFlags & STYLE_BITS));
    EXPECT_EQ(RGB(9, 9, 9), (COLORREF)doc->m_iForeColour);
    EXPECT_EQ(RGB(7, 7, 7), (COLORREF)doc->m_iBackColour);
    doc->RememberStyle(nullptr);   // null-safe
    delete doc;
}

// ═══ SendTo — the trigger/alias/timer send-destination switch ═══════════════

TEST(SendToTest, OutputStatusVariableAndLog)
{
    CMUSHclientDoc* doc = make_doc();
    CString out;
    doc->SendTo(eSendToOutput, "to output", false, false, "desc", "", out);
    EXPECT_TRUE(out.Find("to output") == 0);   // ENDLINE appended after text

    doc->SendTo(eSendToStatus, "status text", false, false, "desc", "", out);

    CString out2;
    doc->SendTo(eSendToVariable, "value!", false, false, "desc", "myvar", out2);
    CVariable* v = nullptr;
    ASSERT_TRUE(doc->GetVariableMap().Lookup("myvar", v));
    EXPECT_STREQ("value!", (LPCTSTR)v->strContents);

    char path[128];
    snprintf(path, sizeof path, "/tmp/mc_test_conn_%d_sendto.log", (int)getpid());
    ASSERT_EQ((long)eOK, doc->OpenLog(path, FALSE));
    doc->SendTo(eSendToLogFile, "logged line", false, false, "d", "", out2);
    doc->CloseLog();
    FILE* f = fopen(path, "rb");
    ASSERT_TRUE(f != nullptr);
    char buf[128] = {};
    fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    EXPECT_TRUE(strstr(buf, "logged line\n") != nullptr);
    unlink(path);
    delete doc;
}

TEST(SendToTest, WorldAndCommandQueue)
{
    CMUSHclientDoc* doc = make_doc();
    CString out;
    doc->SendTo(eSendToWorld, "kill rat", false, false, "d", "", out);
    EXPECT_TRUE(wire(doc).sent.find("kill rat") != std::string::npos);

    // with the default speed-walk delay of 0 the queue drains immediately,
    // so the queued command lands on the wire (SendMsg queue-then-drain)
    doc->SendTo(eSendToCommandQueue, "queued thing", false, false, "d", "", out);
    EXPECT_TRUE(wire(doc).sent.find("queued thing") != std::string::npos);
    delete doc;
}

TEST(SendToTest, EmptyTextDoesNothingForMostDestinations)
{
    CMUSHclientDoc* doc = make_doc();
    CString out;
    doc->SendTo(eSendToWorld, "", false, false, "d", "", out);
    EXPECT_TRUE(wire(doc).sent.empty());
    EXPECT_TRUE(out.IsEmpty());
    delete doc;
}

static std::string s_notepadTitle, s_notepadText;
static bool s_notepadReplace;

TEST(SendToTest, NotepadDestinations)
{
    g_wxhook_append_to_notepad = [](const char* title, const char* text,
                                    bool replace) -> bool {
        s_notepadTitle = title ? title : "";
        s_notepadText = text ? text : "";
        s_notepadReplace = replace;
        return true;
    };
    static std::string s_createdTitle, s_createdBody;
    g_wxhook_create_text_window = [](const char* body, const char* title,
                                     CMUSHclientDoc*, long long, const char*,
                                     long, long, unsigned int, unsigned long,
                                     unsigned long, const char*, const char*,
                                     bool, bool, bool, bool, bool, int, int) -> void* {
        s_createdTitle = title ? title : "";
        s_createdBody = body ? body : "";
        return (void*)1;
    };
    CMUSHclientDoc* doc = make_doc();
    CString out;
    doc->SendTo(eSendToNotepad, "fresh contents", false, false,
                "Trigger: pad", "", out);
    EXPECT_STREQ("Trigger: pad", s_createdTitle.c_str());
    EXPECT_TRUE(s_createdBody.find("fresh contents") == 0);

    doc->SendTo(eAppendToNotepad, "appended", false, false, "pad", "", out);
    EXPECT_STREQ("pad", s_notepadTitle.c_str());
    EXPECT_TRUE(s_notepadText.find("appended") == 0);
    EXPECT_FALSE(s_notepadReplace);

    doc->SendTo(eReplaceNotepad, "replaced", false, false, "pad", "", out);
    EXPECT_TRUE(s_notepadText.find("replaced") == 0);
    EXPECT_TRUE(s_notepadReplace);

    g_wxhook_append_to_notepad = nullptr;
    g_wxhook_create_text_window = nullptr;
    delete doc;
}

TEST(SendToTest, SpeedwalkEvaluationAndExecute)
{
    CMUSHclientDoc* doc = make_doc();
    CString out;
    // A valid speedwalk expands and goes to the world (queued send)
    doc->SendTo(eSendToSpeedwalk, "2n", false, false, "d", "", out);
    EXPECT_TRUE(wire(doc).sent.find("north") != std::string::npos);
    delete doc;
}

// ═══ GetProcedureDispid diagnostics ════════════════════════════════════════

TEST(ProcedureDispid, NoEngineOrEmptyNameIsUnknown)
{
    CMUSHclientDoc* doc = make_doc();
    CString err;
    EXPECT_EQ((DISPID)DISPID_UNKNOWN,
              doc->GetProcedureDispid("", "trigger", "lbl", err));
    EXPECT_EQ((DISPID)DISPID_UNKNOWN,
              doc->GetProcedureDispid("fn", "trigger", "lbl", err));
    EXPECT_TRUE(err.IsEmpty());   // no engine: silent
    delete doc;
}

#ifdef MUSHCLIENT_LUA_SCRIPTING
TEST(ProcedureDispid, MissingRoutineBuildsDiagnostics)
{
    CMUSHclientDoc* doc = make_doc();
    doc->m_ScriptEngine = new CScriptEngine(doc, "Lua");
    doc->m_ScriptEngine->CreateScriptEngine();
    CString err;
    // no script file configured
    EXPECT_EQ((DISPID)DISPID_UNKNOWN,
              doc->GetProcedureDispid("no_such_fn", "trigger", "", err));
    EXPECT_TRUE(err.Find("You have not specified a script file name:") == 0);
    EXPECT_TRUE(err.Find("The trigger subroutine named \"no_such_fn\"") != -1);
    // labelled variant + script-file variant append to the SAME message
    doc->m_strScriptFilename = "world.lua";
    CString err2;
    EXPECT_EQ((DISPID)DISPID_UNKNOWN,
              doc->GetProcedureDispid("gone", "alias", "mylabel", err2));
    EXPECT_TRUE(err2.Find("problem in script file \"world.lua\"") != -1);
    EXPECT_TRUE(err2.Find("The alias (mylabel) subroutine named \"gone\"") != -1);
    delete doc;
}
#endif

// ═══ CalculateSpeedWalkString (auto-mapper) ════════════════════════════════

TEST(Mapping, SpeedWalkStringGroupsAndComments)
{
    CMUSHclientDoc* doc = make_doc();
    doc->m_strMapList.AddTail("n");
    doc->m_strMapList.AddTail("n");
    doc->m_strMapList.AddTail("n");
    doc->m_strMapList.AddTail("s");
    doc->m_strMapList.AddTail("{the plaza}");
    doc->m_strMapList.AddTail("sw");
    doc->m_strMapList.AddTail("sw");
    CString with = doc->CalculateSpeedWalkString(false);
    EXPECT_TRUE(with.Find("3n") != -1);       // runs are counted
    EXPECT_TRUE(with.Find("s ") != -1);
    EXPECT_TRUE(with.Find("{the plaza}") != -1);
    EXPECT_TRUE(with.Find("2(sw)") != -1);    // multi-char dirs in parens
    CString without = doc->CalculateSpeedWalkString(true);
    EXPECT_TRUE(without.Find("{the plaza}") == -1);
    EXPECT_TRUE(without.Find("3n") != -1);
    delete doc;
}

// ═══ MCCP v1 (TELOPT_COMPRESS 85: IAC SB COMPRESS WILL SE) ═════════════════

TEST(Mccp, Version1NegotiationAlsoCompresses)
{
    CMUSHclientDoc* doc = make_doc();
    unsigned char will[3] = { T_IAC, T_WILL, 85 /*COMPRESS v1*/ };
    doc->OnBytesReceived((char*)will, 3);
    unsigned char expectDo[3] = { T_IAC, 253, 85 };
    EXPECT_TRUE(wire(doc).sent.find(std::string((char*)expectDo, 3)) != std::string::npos);
    // v1 subnegotiation ends WILL SE (not IAC SE) with compressed data inline
    std::string z = deflate_bytes("v1 compressed\r\n", false);
    std::string packet;
    unsigned char sb[5] = { T_IAC, T_SB, 85, T_WILL, T_SE };
    packet.append((char*)sb, 5);
    packet += z;
    doc->OnBytesReceived(&packet[0], (int)packet.size());
    EXPECT_TRUE(doc->m_bCompress);
    EXPECT_TRUE(saw(doc, "v1 compressed"));
    delete doc;
}

// ═══ Beep + Pueblo start detection in the stream ═══════════════════════════

TEST(StreamArms, BeepCharacterIsConsumed)
{
    CMUSHclientDoc* doc = make_doc();
    char text[] = "ding\adong\r\n";
    doc->OnBytesReceived(text, (int)sizeof text - 1);
    EXPECT_TRUE(saw(doc, "dingdong"));   // BEL consumed, not displayed
    delete doc;
}

// ═══ Round-4 coverage: ConnectSocket / proxy dispatch / MCCP growth /
//      DNS / scripts-at-connect / SendTo tail arms ═══════════════════════════

#include "fake_worldview.h"

// ── ConnectSocket validation + proxy address arms ──────────────────────────

TEST(ConnectSocketTest, UnknownProxyTypeRefused)
{
    CMUSHclientDoc* doc = make_doc();
    doc->m_iSocksProcessing = 99;
    EXPECT_FALSE(doc->ConnectSocket());
    delete doc;
}

TEST(ConnectSocketTest, Socks4RequiresProxyNameAndPort)
{
    CMUSHclientDoc* doc = make_doc();
    doc->m_iSocksProcessing = eProxyServerSocks4;
    doc->m_strProxyServerName = "";
    EXPECT_FALSE(doc->ConnectSocket());

    doc->m_strProxyServerName = "proxy.example.com";
    doc->m_iProxyServerPort = 0;
    EXPECT_FALSE(doc->ConnectSocket());
    delete doc;
}

// world IP known, proxy given as dotted IP: the proxy conversion arm runs and
// the connection is initiated to the PROXY address/port.
TEST(ConnectSocketTest, ProxyByIpInitiatesConnectionToProxy)
{
    CMUSHclientDoc* doc = make_doc();
    doc->m_server = "10.1.2.3";
    doc->m_iSocksProcessing = eProxyServerSocks5;
    doc->m_strProxyServerName = "10.9.9.9";
    doc->m_iProxyServerPort = 1080;
    EXPECT_TRUE(doc->ConnectSocket());
    // ConnectSocket replaced the transport with the real POSIX adapter and
    // pointed the connect at the proxy
    EXPECT_EQ((int)eConnectConnectingToProxy, (int)doc->m_iConnectPhase);
    EXPECT_EQ(inet_addr("10.9.9.9"), (unsigned int)doc->m_ProxyAddr.sin_addr.s_addr);
    delete doc;
}

// proxy given as a NAME: ConnectSocket parks in the proxy name-lookup phase
// (resolution is async through the transport) and reports "waiting".
TEST(ConnectSocketTest, ProxyNameParksInProxyLookupPhase)
{
    CMUSHclientDoc* doc = make_doc();
    doc->m_server = "10.1.2.3";
    doc->m_iSocksProcessing = eProxyServerSocks5;
    doc->m_strProxyServerName = "localhost";
    doc->m_iProxyServerPort = 1080;
    EXPECT_TRUE(doc->ConnectSocket());
    EXPECT_EQ((int)eConnectProxyNameLookup, (int)doc->m_iConnectPhase);
    delete doc;
}

// world given as a NAME that cannot resolve: with no transport the
// synchronous-fallback resolver runs, fails, and notes the failure.
TEST(ConnectSocketTest, UnresolvableWorldNameNotesFailure)
{
    NotifyToOutput toOutput;
    CMUSHclientDoc* doc = make_doc();
    doc->TeardownTransport();               // force the sync-fallback arm
    doc->m_server = "no-such-host.invalid";
    doc->m_iConnectPhase = eConnectMudNameLookup;
    EXPECT_TRUE(doc->LookupHostName("no-such-host.invalid"));   // true = error
    EXPECT_TRUE(saw(doc, "Unable to resolve"));
    EXPECT_EQ((int)eConnectNotConnected, (int)doc->m_iConnectPhase);
    delete doc;
}

// world given as a resolvable NAME: the sync-fallback resolver fills in the
// MUD address and initiates the connection.
TEST(ConnectSocketTest, ResolvableWorldNameProceedsToConnect)
{
    CMUSHclientDoc* doc = make_doc();
    doc->TeardownTransport();               // force the sync-fallback arm
    doc->m_iConnectPhase = eConnectMudNameLookup;
    EXPECT_FALSE(doc->LookupHostName("localhost"));   // false = resolved
    EXPECT_EQ(inet_addr("127.0.0.1"), (unsigned int)doc->m_sockAddr.sin_addr.s_addr);
    delete doc;
}

// MUD address resolved with a named proxy still pending: the completion
// chains straight into the proxy lookup.
TEST(ConnectSocketTest, MudLookupChainsIntoProxyLookup)
{
    CMUSHclientDoc* doc = make_doc();
    doc->TeardownTransport();
    doc->m_iSocksProcessing = eProxyServerSocks5;
    doc->m_strProxyServerName = "localhost";
    doc->m_iProxyServerPort = 1080;
    doc->m_ProxyAddr.sin_family = AF_UNSPEC;    // proxy not resolved yet
    doc->m_iConnectPhase = eConnectMudNameLookup;
    struct in_addr a; a.s_addr = inet_addr("10.4.5.6");
    doc->LookupHostNameComplete(a, eConnectMudNameLookup, true);
    // both addresses now known
    EXPECT_EQ(inet_addr("10.4.5.6"),  (unsigned int)doc->m_sockAddr.sin_addr.s_addr);
    EXPECT_EQ(inet_addr("127.0.0.1"), (unsigned int)doc->m_ProxyAddr.sin_addr.s_addr);
    delete doc;
}

// LookupHostNameComplete called in a phase that is not a name-lookup phase:
// the unexpected-phase arm.
TEST(ConnectSocketTest, LookupCompleteInWrongPhaseIsRejected)
{
    CMUSHclientDoc* doc = make_doc();
    struct in_addr a; a.s_addr = inet_addr("127.0.0.1");
    doc->LookupHostNameComplete(a, eConnectConnectedToMud, true);
    // phase untouched — the arm bails out
    EXPECT_EQ((int)eConnectConnectedToMud, (int)doc->m_iConnectPhase);
    delete doc;
}

// ── OnBytesReceived proxy-phase dispatch ───────────────────────────────────

// A packet arriving in AwaitingProxyResponse1 is routed to
// ProcessProxyResponse1; an error result discards the packet.
TEST(ProxyDispatch, Response1ErrorConsumesPacket)
{
    NotifyToOutput toOutput;
    CMUSHclientDoc* doc = make_doc();
    doc->m_iConnectPhase = eConnectAwaitingProxyResponse1;
    char buff[] = { 5, 1, 'x', 'y' };   // GSSAPI refusal + trailing bytes
    doc->OnBytesReceived(buff, 4);
    EXPECT_EQ((int)eConnectNotConnected, (int)doc->m_iConnectPhase);
    delete doc;
}

// A successful response1 with NO trailing data takes the exact-size return.
TEST(ProxyDispatch, Response1SuccessExactPacket)
{
    CMUSHclientDoc* doc = make_doc();
    doc->m_iConnectPhase = eConnectAwaitingProxyResponse1;
    char buff[] = { 5, 0 };   // no-auth accepted
    doc->OnBytesReceived(buff, 2);
    // moved on to phase 3 (connect request sent to the proxy)
    EXPECT_EQ((int)eConnectAwaitingProxyResponse3, (int)doc->m_iConnectPhase);
    EXPECT_TRUE(wire(doc).sent.size() >= 4);   // the CONNECT request went out
    delete doc;
}

// A successful response1 WITH trailing data: the response bytes are consumed
// and the residue is shifted down (memmove arm) — then discarded because we
// are not yet connected-to-mud.
TEST(ProxyDispatch, Response1SuccessWithResidueShiftsBuffer)
{
    CMUSHclientDoc* doc = make_doc();
    doc->m_iConnectPhase = eConnectAwaitingProxyResponse1;
    char buff[] = { 5, 0, 'A', 'B', 'C' };
    doc->OnBytesReceived(buff, 5);
    EXPECT_EQ((int)eConnectAwaitingProxyResponse3, (int)doc->m_iConnectPhase);
    // residue was shifted to the front of the buffer by the memmove
    EXPECT_EQ('A', buff[0]);
    EXPECT_EQ('C', buff[2]);
    delete doc;
}

// Response2 and response3 dispatch arms.
TEST(ProxyDispatch, Response2And3Dispatch)
{
    NotifyToOutput toOutput;
    {
        CMUSHclientDoc* doc = make_doc();
        doc->m_iConnectPhase = eConnectAwaitingProxyResponse2;
        char buff[] = { 1, 1 };   // auth refused
        doc->OnBytesReceived(buff, 2);
        EXPECT_EQ((int)eConnectNotConnected, (int)doc->m_iConnectPhase);
        delete doc;
    }
    {
        CMUSHclientDoc* doc = make_doc();
        doc->m_iConnectPhase = eConnectAwaitingProxyResponse3;
        char buff[] = { 5, 1, 0, 1, 0,0,0,0, 0,0 };   // general SOCKS failure
        doc->OnBytesReceived(buff, 10);
        EXPECT_EQ((int)eConnectNotConnected, (int)doc->m_iConnectPhase);
        delete doc;
    }
}

// ── MCCP: output-buffer growth (Z_BUF_ERROR) and input-buffer overflow ─────

// When inflate reports Z_BUF_ERROR (output buffer full, no progress), the
// buffer is grown by COMPRESS_BUFFER_LENGTH and decompression continues —
// and next_out must survive the realloc.
TEST(MccpArms, OutputBufferGrowsOnZBufError)
{
    CMUSHclientDoc* doc = make_doc();
    start_mccp2(doc);
    ASSERT_TRUE(doc->m_bCompress);

    // ONE persistent deflate stream, delivered in two sync-flushed chunks
    // (the MCCP contract: the whole session is a single zlib stream)
    z_stream dz{};
    deflateInit(&dz, Z_DEFAULT_COMPRESSION);
    auto chunk = [&](const std::string& plain) {
        std::string out;
        char buf[4096];
        dz.next_in = (Bytef*)plain.data();
        dz.avail_in = (uInt)plain.size();
        do {
            dz.next_out = (Bytef*)buf;
            dz.avail_out = sizeof buf;
            deflate(&dz, Z_SYNC_FLUSH);
            out.append(buf, sizeof buf - dz.avail_out);
        } while (dz.avail_out == 0);
        return out;
    };

    // chunk 1 flows through normally (consumes the zlib header)
    std::string c1 = chunk("first chunk\r\n");
    doc->OnBytesReceived(&c1[0], (int)c1.size());
    EXPECT_TRUE(saw(doc, "first chunk"));

    // now manufacture the documented Z_BUF_ERROR state — output buffer full,
    // mid-stream.  Delivery A (2 bytes) lets inflate consume the block-header
    // bits (Z_OK, no output); delivery B arrives with the output buffer full
    // again and ONLY mid-block literal data pending → no progress possible →
    // Z_BUF_ERROR → the grow-and-retry arm.
    std::string c2 = chunk("grown-buffer payload\r\n");
    auto fill_output = [&]() {
        memset(doc->m_CompressOutput, 'x', doc->m_nCompressionOutputBufferSize);
        doc->m_zCompress.next_out =
            doc->m_CompressOutput + doc->m_nCompressionOutputBufferSize;
        doc->m_zCompress.avail_out = 0;
    };
    fill_output();
    std::string a = c2.substr(0, 2);
    doc->OnBytesReceived(&a[0], (int)a.size());

    fill_output();
    long sizeBefore = doc->m_nCompressionOutputBufferSize;
    std::string b = c2.substr(2);
    doc->OnBytesReceived(&b[0], (int)b.size());
    deflateEnd(&dz);

    EXPECT_TRUE((long)doc->m_nCompressionOutputBufferSize >= sizeBefore + COMPRESS_BUFFER_LENGTH);
    EXPECT_TRUE(saw(doc, "grown-buffer payload"));
    delete doc;
}

// A packet larger than the remaining compression input buffer disconnects
// with the insufficient-memory message.
TEST(MccpArms, InputBufferOverflowDisconnects)
{
    CMUSHclientDoc* doc = make_doc();
    start_mccp2(doc);
    ASSERT_TRUE(doc->m_bCompress);

    // pretend the input buffer is nearly full
    doc->m_zCompress.next_in = doc->m_CompressInput;
    doc->m_zCompress.avail_in = COMPRESS_BUFFER_LENGTH - 2;

    char packet[16] = { 0 };
    doc->OnBytesReceived(packet, 16);   // 16 > 2 remaining
    EXPECT_EQ((int)eConnectNotConnected, (int)doc->m_iConnectPhase);
    delete doc;
}

// The performance-counter timing arms run when the app publishes a counter
// frequency.
TEST(MccpArms, CompressionTimingCountedWhenFrequencyKnown)
{
    LONGLONG saved = App.m_iCounterFrequency;
    App.m_iCounterFrequency = 1000000;
    CMUSHclientDoc* doc = make_doc();
    start_mccp2(doc);
    std::string z = deflate_bytes("timed payload\r\n", false);
    doc->OnBytesReceived(&z[0], (int)z.size());
    EXPECT_TRUE(saw(doc, "timed payload"));
    App.m_iCounterFrequency = saved;
    delete doc;
}

// Existing compressed input is shuffled to the buffer start when a new
// packet arrives (the avail_in memmove arm).
TEST(MccpArms, PendingInputShuffledToBufferStart)
{
    CMUSHclientDoc* doc = make_doc();
    start_mccp2(doc);

    // split one deflate stream into two 1-byte-overlapping deliveries so the
    // second OnBytesReceived call finds avail_in != 0 pending
    std::string z = deflate_bytes("split delivery text\r\n", false);
    ASSERT_TRUE(z.size() > 4);
    std::string first = z.substr(0, 3);       // too short to decompress fully
    std::string rest  = z.substr(3);
    doc->OnBytesReceived(&first[0], (int)first.size());
    // some input is still pending in the zlib buffer
    doc->OnBytesReceived(&rest[0], (int)rest.size());
    EXPECT_TRUE(saw(doc, "split delivery text"));
    delete doc;
}

// ── Logging arms ───────────────────────────────────────────────────────────

// CloseLog writes a formatted postamble (%n newline + strftime codes).
TEST(LoggingArms, ClosePostambleFormatted)
{
    CMUSHclientDoc* doc = make_doc();
    char path[128];
    snprintf(path, sizeof path, "/tmp/mc_test_r4_%d_post.log", (int)getpid());
    ASSERT_EQ((long)eOK, doc->OpenLog(path, FALSE));
    doc->m_strLogFilePostamble = "end%nyear=%Y";
    EXPECT_EQ((long)eOK, doc->CloseLog());

    FILE* f = fopen(path, "rb");
    ASSERT_TRUE(f != nullptr);
    char buf[512] = {};
    fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    unlink(path);
    EXPECT_TRUE(strstr(buf, "end\nyear=2") != nullptr);   // %n and %Y expanded
    delete doc;
}

// LogCommand: '%' in the input preamble/postamble goes through FormatTime;
// HTML colour mode wraps the command in a <font> pair.
TEST(LoggingArms, LogCommandPreambleAndHtmlColour)
{
    CMUSHclientDoc* doc = make_doc();
    char path[128];
    snprintf(path, sizeof path, "/tmp/mc_test_r4_%d_cmd.log", (int)getpid());
    ASSERT_EQ((long)eOK, doc->OpenLog(path, FALSE));
    doc->m_log_input = TRUE;
    doc->m_bLogHTML = TRUE;
    doc->m_bLogInColour = TRUE;
    doc->m_echo_colour = 1;              // not SAMECOLOUR → colour arm
    doc->m_strLogLinePreambleInput = "[%Y] ";
    doc->m_strLogLinePostambleInput = " <done>";
    doc->LogCommand("kill rat\r\n");
    doc->CloseLog();

    FILE* f = fopen(path, "rb");
    ASSERT_TRUE(f != nullptr);
    char buf[512] = {};
    fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    unlink(path);
    EXPECT_TRUE(strstr(buf, "[2") != nullptr);            // %Y expanded
    EXPECT_TRUE(strstr(buf, "<font color=") != nullptr);  // colour arm
    EXPECT_TRUE(strstr(buf, "</font>") != nullptr);
    EXPECT_TRUE(strstr(buf, "kill rat") != nullptr);
    EXPECT_TRUE(strstr(buf, "&lt;done&gt;") == nullptr);  // postamble not escaped
    delete doc;
}

// WriteToLog on an unwritable stream closes the log and clears m_logfile.
TEST(LoggingArms, WriteFailureClosesLog)
{
    CMUSHclientDoc* doc = make_doc();
    char path[128];
    snprintf(path, sizeof path, "/tmp/mc_test_r4_%d_ro.log", (int)getpid());
    FILE* mk = fopen(path, "w"); ASSERT_TRUE(mk != nullptr); fclose(mk);
    doc->m_logfile = fopen(path, "r");   // READ-only stream: fwrite must fail
    ASSERT_TRUE(doc->m_logfile != nullptr);
    doc->m_logfile_name = path;
    doc->WriteToLog("this cannot be written", 22);
    EXPECT_TRUE(doc->m_logfile == NULL);
    unlink(path);
    delete doc;
}

// OpenLog with an empty name derives the file name from the auto-log format
// (FormatTime arm); with no auto-log name either, it fails.
TEST(LoggingArms, OpenLogAutoNameAndEmptyFailure)
{
    CMUSHclientDoc* doc = make_doc();
    EXPECT_EQ((long)eCouldNotOpenFile, doc->OpenLog("", FALSE));

    char pattern[128];
    snprintf(pattern, sizeof pattern, "/tmp/mc_test_r4_%d_auto_%%Y.log", (int)getpid());
    doc->m_strAutoLogFileName = pattern;
    EXPECT_EQ((long)eOK, doc->OpenLog("", FALSE));
    CString name = doc->m_logfile_name;
    EXPECT_TRUE(name.Find("%Y") == -1);          // year was substituted
    doc->CloseLog();
    unlink(name);
    delete doc;
}

// ── Connection lifecycle arms ──────────────────────────────────────────────

// ConnectionEstablished with "use MXP always" turns MXP on at connect.
TEST(LifecycleArms, EstablishTurnsOnMxpWhenAlways)
{
    CMUSHclientDoc* doc = make_doc();
    doc->m_iUseMXP = eUseMXP;
    doc->ConnectionEstablished();
    EXPECT_TRUE(doc->m_bMXP);
    delete doc;
}

// ConnectionEstablished with an auto-log name that cannot be opened takes
// the could-not-open arm (no log file afterwards).
TEST(LifecycleArms, EstablishAutoLogFailureLeavesNoLog)
{
    CMUSHclientDoc* doc = make_doc();
    doc->m_strAutoLogFileName = "/nonexistent-dir-r4/x.log";
    doc->ConnectionEstablished();
    EXPECT_TRUE(doc->m_logfile == NULL);
    delete doc;
}

// ConnectionEstablished auto-log success writes the file preamble and the
// world-name banner (plain and HTML arms).
TEST(LifecycleArms, EstablishAutoLogWritesPreambleAndBanner)
{
    CMUSHclientDoc* doc = make_doc();
    char path[128];
    snprintf(path, sizeof path, "/tmp/mc_test_r4_%d_autolog.log", (int)getpid());
    doc->m_strAutoLogFileName = path;
    doc->m_strLogFilePreamble = "PRE%nAMBLE";
    doc->m_bWriteWorldNameToLog = TRUE;
    doc->m_mush_name = "BannerWorld";
    doc->ConnectionEstablished();
    ASSERT_TRUE(doc->m_logfile != NULL);
    doc->CloseLog();

    FILE* f = fopen(path, "rb");
    ASSERT_TRUE(f != nullptr);
    char buf[1024] = {};
    fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    unlink(path);
    EXPECT_TRUE(strstr(buf, "PRE\nAMBLE") != nullptr);
    EXPECT_TRUE(strstr(buf, "BannerWorld - ") != nullptr);
    EXPECT_TRUE(strstr(buf, "----") != nullptr);   // hyphen banner line
    delete doc;
}

// Diku-style auto-login with an empty password: name is sent, the headless
// no-password-prompt arm leaves the password blank so nothing else goes out.
TEST(LifecycleArms, DikuLoginEmptyPasswordSendsNameOnly)
{
    CMUSHclientDoc* doc = make_doc();
    doc->m_connect_now = eConnectDiku;
    doc->m_name = "Conan";
    doc->m_password = "";
    doc->ConnectionEstablished();
    EXPECT_TRUE(wire(doc).sent.find("Conan") != std::string::npos);
    delete doc;
}

#ifdef MUSHCLIENT_LUA_SCRIPTING
// The world-connect script handler runs at ConnectionEstablished (the
// ExecuteScript dispid path), and the world-disconnect handler runs at
// ConnectionClosed.
TEST(LifecycleArms, WorldConnectAndDisconnectScriptsRun)
{
    CMUSHclientDoc* doc = make_doc();
    CScriptEngine* engine = new CScriptEngine(doc, "lua");
    ASSERT_TRUE(!engine->CreateScriptEngine());
    ASSERT_TRUE(!engine->Parse(CString(
        "function OnWorldConnect() world.SetVariable('conn', 'yes') end\n"
        "function OnWorldDisconnect() world.SetVariable('disc', 'yes') end\n"),
        "<test>"));
    doc->m_ScriptEngine = engine;
    doc->m_strWorldConnect = "OnWorldConnect";
    doc->m_strWorldDisconnect = "OnWorldDisconnect";
    doc->ResolveAllEntryPoints();      // resolve the handler dispids

    doc->ConnectionEstablished();
    CVariable* v = nullptr;
    ASSERT_TRUE(doc->GetVariableMap().Lookup("conn", v));
    EXPECT_STREQ("yes", (LPCTSTR)v->strContents);

    doc->ConnectionClosed(0);
    ASSERT_TRUE(doc->GetVariableMap().Lookup("disc", v));
    EXPECT_STREQ("yes", (LPCTSTR)v->strContents);
    EXPECT_EQ((int)eConnectNotConnected, (int)doc->m_iConnectPhase);
    delete doc;
}

// GetProcedureDispid: found → dispid; missing sub appends the script-file
// diagnostics to the error string.
TEST(LifecycleArms, GetProcedureDispidDiagnostics)
{
    CMUSHclientDoc* doc = make_doc();
    CScriptEngine* engine = new CScriptEngine(doc, "lua");
    ASSERT_TRUE(!engine->CreateScriptEngine());
    ASSERT_TRUE(!engine->Parse(CString("function known() end\n"), "<test>"));
    doc->m_ScriptEngine = engine;

    CString err;
    EXPECT_NE(DISPID_UNKNOWN, doc->GetProcedureDispid("known", "alias", "lbl", err));
    EXPECT_TRUE(err.IsEmpty());

    EXPECT_EQ(DISPID_UNKNOWN, doc->GetProcedureDispid("missing_sub", "alias", "lbl", err));
    EXPECT_TRUE(err.Find("missing_sub") != -1);
    EXPECT_TRUE(err.Find("You have not specified a script file name") != -1);

    // with a script file name the other preamble arm is used
    CString err2;
    doc->m_strScriptFilename = "myscript.lua";
    EXPECT_EQ(DISPID_UNKNOWN, doc->GetProcedureDispid("missing_sub", "trigger", "", err2));
    EXPECT_TRUE(err2.Find("myscript.lua") != -1);
    delete doc;
}

// SendTo eSendToScript parses the text through the engine (and the after-omit
// variant does too).
TEST(SendToTest, ScriptDestinationsParseThroughEngine)
{
    CMUSHclientDoc* doc = make_doc();
    CScriptEngine* engine = new CScriptEngine(doc, "lua");
    ASSERT_TRUE(!engine->CreateScriptEngine());
    doc->m_ScriptEngine = engine;

    CString out;
    doc->SendTo(eSendToScript, "world.SetVariable('sts', 'ran')", false, false,
                "trigger: t", "", out);
    CVariable* v = nullptr;
    ASSERT_TRUE(doc->GetVariableMap().Lookup("sts", v));
    EXPECT_TRUE(doc->m_bInSendToScript);
    doc->m_bInSendToScript = false;

    doc->SendTo(eSendToScriptAfterOmit, "world.SetVariable('stao', 'ran')",
                false, false, "trigger: t", "", out);
    ASSERT_TRUE(doc->GetVariableMap().Lookup("stao", v));
    delete doc;
}
#endif // MUSHCLIENT_LUA_SCRIPTING

// SendTo eSendToScript with NO engine notes the cannot-execute message.
TEST(SendToTest, ScriptDestinationWithoutEngineNotes)
{
    CMUSHclientDoc* doc = make_doc();
    CString out;
    doc->SendTo(eSendToScript, "print(1)", false, false, "d", "", out);
    EXPECT_TRUE(saw(doc, "scripting is not enabled"));
    delete doc;
}

// SendTo eSendToCommand fills the command bar through the world-view seam —
// but only when the bar is empty.
TEST(SendToTest, CommandDestinationFillsEmptyBarOnly)
{
    CMUSHclientDoc* doc = make_doc();
    FakeWorldView view;
    doc->m_pWorldView = &view;
    CString out;
    doc->SendTo(eSendToCommand, "go north", false, false, "d", "", out);
    EXPECT_STREQ("go north", view.cmd.c_str());
    doc->SendTo(eSendToCommand, "go south", false, false, "d", "", out);
    EXPECT_STREQ("go north", view.cmd.c_str());   // bar not empty: unchanged
    doc->m_pWorldView = nullptr;
    delete doc;
}

// SendTo eSendToStatus keeps only the first line of a multi-line status.
TEST(SendToTest, StatusDestinationTruncatesAtNewline)
{
    CMUSHclientDoc* doc = make_doc();
    CString out;
    doc->SendTo(eSendToStatus, "first line\r\nsecond line", false, false, "d", "", out);
    EXPECT_STREQ("first line", (LPCTSTR)doc->m_strStatusMessage);
    delete doc;
}

// SendTo eSendToSpeedwalk with a malformed speedwalk takes the error arm.
TEST(SendToTest, SpeedwalkErrorArm)
{
    CMUSHclientDoc* doc = make_doc();
    CString out;
    doc->SendTo(eSendToSpeedwalk, "3(unclosed", false, false, "d", "", out);
    EXPECT_TRUE(wire(doc).sent.empty());   // nothing was sent
    delete doc;
}

// SendTo eSendImmediate bypasses the queue and hits the wire directly.
TEST(SendToTest, ImmediateDestinationSendsDirectly)
{
    CMUSHclientDoc* doc = make_doc();
    CString out;
    doc->SendTo(eSendImmediate, "now!", false, false, "d", "", out);
    EXPECT_TRUE(wire(doc).sent.find("now!") != std::string::npos);
    delete doc;
}

// SendTo with an unknown destination takes the default (do-nothing) arm.
TEST(SendToTest, UnknownDestinationDoesNothing)
{
    CMUSHclientDoc* doc = make_doc();
    CString out;
    doc->SendTo(200, "text", false, false, "d", "", out);
    EXPECT_TRUE(wire(doc).sent.empty());
    EXPECT_TRUE(out.IsEmpty());
    delete doc;
}

// ── Assorted small arms ────────────────────────────────────────────────────

TEST(SmallArms, GetSocketErrorUnknownCode)
{
    // non-Windows: strerror() supplies the text, even for unknown codes
    CMUSHclientDoc* doc = make_doc();
    CString s = doc->GetSocketError(999999);
    EXPECT_TRUE(!s.IsEmpty());
    EXPECT_TRUE(s.Find("nknown error") != -1);
    delete doc;
}

// SendPacket with packet-debugging enabled takes the Debug_Packets arm.
TEST(SmallArms, SendPacketDebugArm)
{
    CMUSHclientDoc* doc = make_doc();
    doc->m_bDebugIncomingPackets = true;
    doc->SendPacket("ping", 4);
    EXPECT_TRUE(wire(doc).sent.find("ping") != std::string::npos);
    delete doc;
}

// Incoming packet-debug arm in DisplayMsg.
TEST(SmallArms, IncomingPacketDebugArm)
{
    CMUSHclientDoc* doc = make_doc();
    doc->m_bDebugIncomingPackets = true;
    char text[] = "debugged\r\n";
    doc->OnBytesReceived(text, (int)sizeof text - 1);
    EXPECT_TRUE(saw(doc, "debugged"));
    delete doc;
}

// SendWindowSizes emits the NAWS subnegotiation from the port's cols/rows.
TEST(SmallArms, SendWindowSizesEmitsNaws)
{
    CMUSHclientDoc* doc = make_doc();
    doc->m_bNAWS_wanted = TRUE;
    doc->m_nWrapColumn = 80;
    doc->m_gui_rows = 30;
    doc->SendWindowSizes(80);
    const std::string& s = wire(doc).sent;
    // IAC SB NAWS 0 80 0 30 IAC SE
    std::string expect = { '\xFF', '\xFA', '\x1F', 0, 80, 0, 30, '\xFF', '\xF0' };
    EXPECT_TRUE(s.find(expect) != std::string::npos);

    // and without NAWS negotiated, nothing goes out
    wire(doc).sent.clear();
    doc->m_bNAWS_wanted = FALSE;
    doc->SendWindowSizes(80);
    EXPECT_TRUE(wire(doc).sent.empty());
    delete doc;
}

// TranslateColour: opacity scaling with and without a translation map.
TEST(SmallArms, TranslateColourOpacityArms)
{
    CMUSHclientDoc* doc = make_doc();
    COLORREF c = RGB(100, 200, 50);
    // empty map, opacity 1.0: identity
    EXPECT_EQ(c, doc->TranslateColour(c, 1.0));
    // empty map, half opacity: scaled
    EXPECT_EQ(RGB(50, 100, 25), doc->TranslateColour(c, 0.5));

    // mapped colour
    doc->m_ColourTranslationMap[c] = RGB(10, 20, 30);
    EXPECT_EQ(RGB(10, 20, 30), doc->TranslateColour(c, 1.0));
    EXPECT_EQ(RGB(5, 10, 15), doc->TranslateColour(c, 0.5));
    // unmapped colour with a non-empty map
    COLORREF other = RGB(1, 2, 3);
    EXPECT_EQ(other, doc->TranslateColour(other, 1.0));
    EXPECT_EQ(RGB(0, 1, 1), doc->TranslateColour(other, 0.5));
    delete doc;
}

// OnGameTrace with a partially-built line pushes the pending text onto a new
// line first.
TEST(SmallArms, GameTraceOnPartialLine)
{
    CMUSHclientDoc* doc = make_doc();
    char text[] = "partial-prompt> ";
    doc->OnBytesReceived(text, (int)sizeof text - 1);
    doc->OnGameTrace();
    EXPECT_TRUE(doc->m_bTrace);
    EXPECT_TRUE(saw(doc, "partial-prompt>"));
    delete doc;
}

MC_TEST_MAIN()
