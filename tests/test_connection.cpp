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
// B-3 atomic flip: real headers (portable stub deleted; tests are FULL_BUILD).
#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "scripting/errors.h"
#include "pal/itransport.h"
#include "fake_transport.h"
#include "scripting/scripting.h"
#include "platform/wx_mfc_hooks.h"   // g_wxhook_set_status (queued-command status seam)
                                     // + g_wxhook_file_dialog / g_run_mfc_dialog,
                                     // the two modals ID_FILE_LOGSESSION puts up
#include "resource.h"                // ID_FILE_LOGSESSION / IDD_LINE_COUNT
#include "dialogs/logdlg.h"          // CLogDlg — seeded by the handler, answered
                                     // by the hook below
#include "notepad_fixture.h"   // FakeNotepad / NotepadCapture

#include <zlib.h>

#include <cstring>
#include <string>
#include "posix_compat.h"

// telnet bytes (numeric to avoid macro clashes)
static const unsigned char T_IAC = 255, T_WILL = 251, T_SB = 250, T_SE = 240;

// ── recording transport ─────────────────────────────────────────────────────
// Derives from the shared RecordingTransport (tests/fake_transport.h) for the
// send()/close()/lastErrorString() boilerplate and the sent/closed/tlsStarted/
// lastToken fields; only the connect/resolve/TLS detail this suite actually
// asserts on gets its own override.

struct RecTransport : RecordingTransport {
    int  connectCalls = 0;
    unsigned int lastAddr = 0;
    unsigned short lastPort = 0;
    bool lastKeepalives = false;
    int  resolveCalls = 0;
    std::string lastResolveName;
    std::string tlsServerName;
    bool tlsAllowInvalid = false;

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
    void startTls(const pal::TlsParams& p) override {
        RecordingTransport::startTls(p);
        tlsServerName = p.serverName;
        tlsAllowInvalid = p.allowInvalidCert;
    }
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

// ── OpenSession: who decides whether opening a world dials ────────────────
//
// One setting decides, the global "connect to worlds automatically"
// (App.m_bAutoConnectWorlds), and OpenSession is where it is read -- the last
// thing OnOpenDocument does.  Pinned here because the wx port used to dial a
// SECOND time from its open-world hook, gated on the world's m_connect_now,
// which is a different setting entirely: the auto-LOGIN method (none / MUSH /
// Diku / MXP).  A world that sent a MUSH login therefore connected on open even
// with auto-connect switched off.
//
// The observable is m_iOutputPacketCount: ConnectSocket zeroes it once past its
// validation guards, so a sentinel value survives exactly when it did not dial.

TEST(OpenSession, DoesNotDialWhenAutoConnectIsOff)
{
    CMUSHclientDoc* doc = make_doc();
    doc->m_server = "127.0.0.1";           // dotted quad: no name lookup
    doc->m_port = 1;
    doc->m_bEnableScripts = FALSE;
    doc->m_connect_now = eConnectMUSH;     // an auto-LOGIN method is set...
    doc->m_iOutputPacketCount = 99;

    const unsigned int save = App.m_bAutoConnectWorlds;
    App.m_bAutoConnectWorlds = FALSE;      // ...but auto-connect is off
    EXPECT_FALSE(doc->OpenSession());
    App.m_bAutoConnectWorlds = save;

    EXPECT_EQ(99L, doc->m_iOutputPacketCount);   // ConnectSocket never ran
    delete doc;
}

TEST(OpenSession, DialsWhenAutoConnectIsOn)
{
    CMUSHclientDoc* doc = make_doc();
    doc->m_server = "127.0.0.1";
    doc->m_port = 1;
    doc->m_bEnableScripts = FALSE;
    doc->m_connect_now = eNoAutoConnect;   // no auto-login configured at all...
    doc->m_iOutputPacketCount = 99;

    const unsigned int save = App.m_bAutoConnectWorlds;
    App.m_bAutoConnectWorlds = TRUE;       // ...but auto-connect is on
    doc->OpenSession();
    App.m_bAutoConnectWorlds = save;

    EXPECT_EQ(0L, doc->m_iOutputPacketCount);    // ConnectSocket ran
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

// ═══════════════════════════════════════════════════════════════════════════
// ID_FILE_LOGSESSION: the ONE session-log command
// ═══════════════════════════════════════════════════════════════════════════
//
// There used to be two logging implementations on the port.  The menu bar ran
// the canonical CMUSHclientDoc::OnFileLogsession; the output pane's context
// menu ran CWorldChildFrame::StartLogging, a raw fopen(path, "a") onto a second
// FILE* that knew nothing about m_bLogOutput / m_log_input / m_bLogNotes, the
// preamble, the postamble or append-vs-truncate.  Two commands both called
// "Start Logging", two files, two formats.
//
// Every entry point now dispatches id 32117 to the document, so this is the
// route they share, driven the way they drive it -- through the message map,
// not by calling the (protected) handler.  The GUI-side proof that the two
// menus really do produce the same BYTES is menubar_selftest.cpp; this is the
// headless proof that the id reaches the canonical handler at all and that the
// handler consults the world's options.
//
// Both modals are answered by hooks: the Save panel names the file, and CLogDlg
// accepts whatever OnFileLogsession seeded it with -- which is the world's own
// logging options, so accepting is what "the user pressed OK" means here.

static std::string g_logSessionPath;

// Answer the Save panel with g_logSessionPath.
static char* LogSessionFileHook(bool, const char*, const char*, const char*,
                                const char*, bool, bool)
{
    return strdup(g_logSessionPath.c_str());
}

// Read a whole file (empty string if it is not there).
static std::string SlurpFile(const std::string& path)
{
    std::string out;
    if (FILE* f = fopen(path.c_str(), "rb")) {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0)
            out.append(buf, n);
        fclose(f);
    }
    return out;
}

// Open a log through the canonical command, write one line of each kind, close
// it through the same command, and hand back the bytes.
static std::string RunLogSession(CMUSHclientDoc* doc, const std::string& path)
{
    g_logSessionPath = path;
    unlink(path.c_str());

    EXPECT_TRUE(AfxDispatchCommand(doc, 0 /*ON_COMMAND*/, ID_FILE_LOGSESSION));
    EXPECT_TRUE(doc->m_logfile != nullptr);

    doc->Note("a note");
    doc->LogCommand("a command\r\n");
    doc->DisplayMsg("some output\n", 12, 0);

    EXPECT_TRUE(AfxDispatchCommand(doc, 0 /*ON_COMMAND*/, ID_FILE_LOGSESSION));
    EXPECT_TRUE(doc->m_logfile == nullptr);

    const std::string bytes = SlurpFile(path);
    unlink(path.c_str());
    return bytes;
}

// Seed a doc with %-free markers, so every byte the log gets is deterministic
// and each logged CATEGORY is identifiable by its line preamble.
static void SetUpMarkedLogging(CMUSHclientDoc* doc)
{
    doc->m_strLogFilePreamble        = "PREAMBLE";
    doc->m_strLogFilePostamble       = "POSTAMBLE";
    doc->m_strLogLinePreambleOutput  = "OUT:";
    doc->m_strLogLinePreambleInput   = "IN:";
    doc->m_strLogLinePreambleNotes   = "NOTE:";
    doc->m_strLogLinePostambleOutput = "";
    doc->m_strLogLinePostambleInput  = "";
    doc->m_strLogLinePostambleNotes  = "";
    doc->m_bWriteWorldNameToLog      = 0;   // it embeds the wall clock
    doc->m_bLogHTML                  = 0;
    doc->m_bLogRaw                   = FALSE;
    doc->m_strAutoLogFileName        = "";
}

struct LogSessionHooks {
    char* (*savedFile)(bool, const char*, const char*, const char*,
                       const char*, bool, bool) = g_wxhook_file_dialog;
    intptr_t (*savedDlg)(void*, unsigned)       = g_run_mfc_dialog;
    unsigned savedConfirm                       = App.m_bConfirmLogFileClose;

    LogSessionHooks()
    {
        g_wxhook_file_dialog = LogSessionFileHook;
        // Accept the seeded options; take NO scrollback dump, because the
        // buffer grows between runs and this is about live logging.
        g_run_mfc_dialog = [](void* self, unsigned idd) -> intptr_t {
            if (idd != IDD_LINE_COUNT) return IDCANCEL;
            ((CLogDlg*) self)->m_lines = 0;
            return IDOK;
        };
        App.m_bConfirmLogFileClose = 0;   // no modal on the close half
    }
    ~LogSessionHooks()
    {
        App.m_bConfirmLogFileClose = savedConfirm;
        g_run_mfc_dialog     = savedDlg;
        g_wxhook_file_dialog = savedFile;
    }
};

// The command opens a log, writes the preamble, and closes it again writing the
// postamble -- reached through the message map, which is how every menu reaches
// it.  This is the whole of what the port's second implementation replaced.
TEST(LogSession, TheCanonicalCommandOpensAndClosesWithPreambleAndPostamble)
{
    LogSessionHooks hooks;
    char path[128];
    snprintf(path, sizeof path, "/tmp/mc_test_logsession_%d.log", (int)getpid());

    CMUSHclientDoc* doc = make_doc();
    SetUpMarkedLogging(doc);
    doc->m_bLogOutput = 1;
    doc->m_log_input  = 1;
    doc->m_bLogNotes  = 1;

    const std::string log = RunLogSession(doc, path);

    EXPECT_NE(std::string::npos, log.find("PREAMBLE"));
    EXPECT_NE(std::string::npos, log.find("POSTAMBLE"));
    EXPECT_NE(std::string::npos, log.find("NOTE:"));
    EXPECT_NE(std::string::npos, log.find("IN:"));
    EXPECT_NE(std::string::npos, log.find("OUT:"));
    delete doc;
}

// ...and it consults the world's three logging options rather than writing
// whatever it is handed.  Each is turned off ALONE, so a handler that ignored
// one of them (as the port's own implementation ignored all three) fails on
// that option only.
TEST(LogSession, TheCanonicalCommandHonoursLogOutputInputAndNotes)
{
    LogSessionHooks hooks;
    char path[128];
    snprintf(path, sizeof path, "/tmp/mc_test_logsession_opt_%d.log",
             (int)getpid());

    {   // output off
        CMUSHclientDoc* doc = make_doc();
        SetUpMarkedLogging(doc);
        doc->m_bLogOutput = 0; doc->m_log_input = 1; doc->m_bLogNotes = 1;
        const std::string log = RunLogSession(doc, path);
        EXPECT_EQ(std::string::npos, log.find("OUT:"));
        EXPECT_NE(std::string::npos, log.find("IN:"));
        EXPECT_NE(std::string::npos, log.find("NOTE:"));
        delete doc;
    }
    {   // input off
        CMUSHclientDoc* doc = make_doc();
        SetUpMarkedLogging(doc);
        doc->m_bLogOutput = 1; doc->m_log_input = 0; doc->m_bLogNotes = 1;
        const std::string log = RunLogSession(doc, path);
        EXPECT_NE(std::string::npos, log.find("OUT:"));
        EXPECT_EQ(std::string::npos, log.find("IN:"));
        EXPECT_NE(std::string::npos, log.find("NOTE:"));
        delete doc;
    }
    {   // notes off
        CMUSHclientDoc* doc = make_doc();
        SetUpMarkedLogging(doc);
        doc->m_bLogOutput = 1; doc->m_log_input = 1; doc->m_bLogNotes = 0;
        const std::string log = RunLogSession(doc, path);
        EXPECT_NE(std::string::npos, log.find("OUT:"));
        EXPECT_NE(std::string::npos, log.find("IN:"));
        EXPECT_EQ(std::string::npos, log.find("NOTE:"));
        delete doc;
    }
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

// Deflate `plain` after installing a preset dictionary, which sets the FDICT
// bit in the zlib header.  A decoder that lacks the dictionary answers the
// header with Z_NEED_DICT, consuming no input and producing no output.
static std::string deflate_with_dictionary(const std::string& plain)
{
    z_stream z = {};
    deflateInit(&z, Z_DEFAULT_COMPRESSION);
    static const char dict[] = "a-preset-dictionary-the-decoder-does-not-have";
    deflateSetDictionary(&z, (const Bytef*)dict, (uInt)(sizeof dict - 1));
    std::string out;
    out.resize(plain.size() + 512);
    z.next_in = (Bytef*)plain.data();
    z.avail_in = (uInt)plain.size();
    z.next_out = (Bytef*)&out[0];
    z.avail_out = (uInt)out.size();
    deflate(&z, Z_FINISH);
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

// The three notepad SendTo destinations, against the REAL bodies.
//
// This used to install g_wxhook_append_to_notepad and assert on what the hook
// received.  That hook belonged to the headless twin, which is gone now that
// the notepad family is adopted (docs/STUB-ADOPTION-2026-08.md), and asserting
// on it never distinguished append from replace in terms of RESULTING CONTENT —
// it only checked which flag was passed.  Driving the real bodies over a real
// registered CTextDocument makes the distinction observable, which is the point.
TEST(SendToTest, NotepadDestinations)
{
    NotepadCapture cap;
    CMUSHclientDoc* doc = make_doc();
    CString out;

    // eSendToNotepad always CREATES — it never looks for an existing window
    // (methods_notepad.cpp:181).  So this makes a notepad even though none
    // existed, and would make a second one if called again.
    doc->SendTo(eSendToNotepad, "fresh contents", false, false,
                "Trigger: pad", "", out);
    ASSERT_EQ(1u, (unsigned) cap.count());
    EXPECT_EQ(std::string("Trigger: pad"),
              std::string((const char*) cap.pads[0]->doc->m_strTitle));
    EXPECT_EQ(0u, cap.pads[0]->text.find("fresh contents"));

    // Now a notepad called "pad" exists, so append and replace find it.
    FakeNotepad pad(doc, "pad", doc->m_iUniqueDocumentNumber);
    pad.text = "seed:";

    doc->SendTo(eAppendToNotepad, "appended", false, false, "pad", "", out);
    EXPECT_EQ(0u, pad.text.find("seed:"));                      // kept the seed
    EXPECT_NE(std::string::npos, pad.text.find("appended"));

    // Replace must DISCARD what append kept — asserting both halves is what
    // separates the two modes.  A body that appended in both cases passes the
    // first assertion and fails this one.
    doc->SendTo(eReplaceNotepad, "replaced", false, false, "pad", "", out);
    EXPECT_EQ(std::string::npos, pad.text.find("seed:"));
    EXPECT_EQ(std::string::npos, pad.text.find("appended"));
    EXPECT_EQ(0u, pad.text.find("replaced"));

    // …and neither of those created a second window.
    EXPECT_EQ(1u, (unsigned) cap.count());
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

// A server-crafted zlib header with the FDICT bit set makes inflate() return
// Z_NEED_DICT (+2): no input consumed, no output produced.  The old error test
// (iCompressResult < 0) let it through, so the decompression loop re-inflated
// the same bytes forever — a remote 100%-CPU denial of service.  The fix treats
// any result that is neither Z_OK nor Z_STREAM_END as a decompress failure, so
// this disconnects immediately instead of hanging.  (Reverting the fix makes
// this test time out rather than pass.)
TEST(MccpArms, DictionaryRequestDisconnectsInsteadOfSpinning)
{
    NotifyToOutput toOutput;
    CMUSHclientDoc* doc = make_doc();
    start_mccp2(doc);
    ASSERT_TRUE(doc->m_bCompress);
    std::string z = deflate_with_dictionary("secret payload that needs a dict\r\n");
    doc->OnBytesReceived(&z[0], (int)z.size());
    EXPECT_EQ((int)eConnectNotConnected, (int)doc->m_iConnectPhase);   // did not spin
    delete doc;
}

// At Z_STREAM_END the loop copies the post-stream residue (real telnet data,
// up to avail_in) back into the caller's receive buffer.  A crafted stream can
// leave avail_in above 9000 — larger than the transports' old char buff[9000]
// — so those buffers are now COMPRESS_BUFFER_LENGTH.  Drive avail_in past 9000
// and confirm the whole residue survives (a fix that merely clamped the memcpy
// to 9000 would drop the tail marker).
TEST(MccpArms, LargeStreamEndResidueIsPreservedNotTruncated)
{
    CMUSHclientDoc* doc = make_doc();
    unsigned char will[3] = { T_IAC, T_WILL, 86 /*COMPRESS2*/ };
    doc->OnBytesReceived((char*)will, 3);

    // one packet: SB IAC SE, a finished compressed stream, then a plain residue
    // bigger than 9000 with a marker at its very end.
    std::string compressed = deflate_bytes("start of stream\r\n", true);   // Z_FINISH
    std::string residue(9600, 'X');
    residue += "TAILMARK\r\n";
    std::string packet;
    unsigned char sb[5] = { T_IAC, T_SB, 86, T_IAC, T_SE };
    packet.append((char*)sb, 5);
    packet += compressed;
    packet += residue;

    doc->OnBytesReceived(&packet[0], (int)packet.size());

    EXPECT_EQ((int)eConnectConnectedToMud, (int)doc->m_iConnectPhase);   // not disconnected
    EXPECT_FALSE(doc->m_bCompress);                                      // stream ended
    EXPECT_GT((long)doc->m_zCompress.avail_in, 9000L);                   // exercised the >9000 path
    EXPECT_TRUE(saw(doc, "TAILMARK"));                                   // full residue preserved
    delete doc;
}

// The two compression-start memmoves copy the post-SE remainder into
// m_CompressInput (COMPRESS_BUFFER_LENGTH bytes) with no bound of their own; a
// plugin or world.Simulate that grows the packet past that would overflow the
// heap.  A packet whose post-SE remainder exceeds the buffer must disconnect,
// exactly as the ReceiveMsg append path already does.  (Reverting the guard
// heap-overflows m_CompressInput.)
TEST(MccpArms, OversizeCompressionStartDisconnects)
{
    CMUSHclientDoc* doc = make_doc();
    unsigned char will[3] = { T_IAC, T_WILL, 86 /*COMPRESS2*/ };
    doc->OnBytesReceived((char*)will, 3);   // negotiate + allocate m_CompressInput

    std::string packet;
    unsigned char sb[5] = { T_IAC, T_SB, 86, T_IAC, T_SE };
    packet.append((char*)sb, 5);
    packet.append(COMPRESS_BUFFER_LENGTH + 1000, 'Z');   // remainder > input buffer

    doc->OnBytesReceived(&packet[0], (int)packet.size());
    EXPECT_EQ((int)eConnectNotConnected, (int)doc->m_iConnectPhase);
    // The guard returns before the memmove and its accounting, so no compressed
    // bytes were ever taken.  Without the guard the remainder is memmove'd (the
    // overflow) and counted here, and the disconnect above still happens on the
    // following inflate error — so this counter, not the phase, is what proves
    // the guard fired.
    EXPECT_EQ(0L, (long)doc->m_nTotalCompressed);
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
    doc->FindAllEntryPoints();      // resolve the handler dispids

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

// UPSTREAM-BUGS #15: a NAWS dimension byte of exactly 255 IS an IAC, and the
// four payload bytes sit between IAC SB NAWS and IAC SE where RFC 854 requires
// a 255 sent as data to be doubled.  Unescaped, the MUD reads it as the start
// of a command, eats the byte after it, and the subnegotiation goes out of
// step.  Only the single value 255 per byte trips it, which is why it survived.
TEST(SmallArms, NawsEscapesADimensionByteOfExactly255)
{
    CMUSHclientDoc* doc = make_doc();
    doc->m_bNAWS_wanted = TRUE;
    doc->m_gui_rows = 30;

    // 255 columns: the LOW byte is 0xFF and must go out doubled.  Unfixed, the
    // wire carries the 9-byte unescaped form and this find() fails.
    doc->SendWindowSizes(255);
    const std::string colsEscaped =
        { '\xFF', '\xFA', '\x1F', 0, '\xFF', '\xFF', 0, 30, '\xFF', '\xF0' };
    EXPECT_TRUE(wire(doc).sent.find(colsEscaped) != std::string::npos);

    // ...and the unescaped form must NOT be there, so a build that appends the
    // doubled byte as well as the raw one cannot pass on the assertion above.
    const std::string colsRaw =
        { '\xFF', '\xFA', '\x1F', 0, '\xFF', 0, 30, '\xFF', '\xF0' };
    EXPECT_TRUE(wire(doc).sent.find(colsRaw) == std::string::npos);

    // The same for the ROWS pair, which the old code wrote from a different
    // slot of the same array — 255 rows, 80 columns.
    wire(doc).sent.clear();
    doc->m_gui_rows = 255;
    doc->SendWindowSizes(80);
    const std::string rowsEscaped =
        { '\xFF', '\xFA', '\x1F', 0, 80, 0, '\xFF', '\xFF', '\xFF', '\xF0' };
    EXPECT_TRUE(wire(doc).sent.find(rowsEscaped) != std::string::npos);

    // Control: only 0xFF is doubled.  256 columns is 0x01 0x00 and 30 rows is
    // 0x00 0x1E — nothing here may be escaped, so a fix that doubled every byte
    // (or every high bit) fails this while still passing the two above.
    wire(doc).sent.clear();
    doc->m_gui_rows = 30;
    doc->SendWindowSizes(256);
    const std::string plain =
        { '\xFF', '\xFA', '\x1F', 1, 0, 0, 30, '\xFF', '\xF0' };
    EXPECT_TRUE(wire(doc).sent.find(plain) != std::string::npos);
    EXPECT_EQ(9, (int)wire(doc).sent.size());   // exactly the 9 unescaped bytes
    delete doc;
}

// world.TextRectangle must renegotiate the window size with the MUD.
//
// Reserving a text rectangle shrinks the area the output is laid out in, so the
// real body ends with `SendWindowSizes (m_nWrapColumn); Redraw ();`
// (scripting/methods/methods_output.cpp:577-578).  The headless twin in
// platform/doc_gui_method_stubs.cpp dropped both for years, on the grounds that
// the method is "view-bound at both ends" — true of the CMUSHView walk in the
// middle, false of these two statements: SendWindowSizes is doc_shared.cpp:4570,
// a mushcore body.  So the engine silently skipped a NAWS renegotiation the
// product performs, and no test could see a regression through this path.
TEST(SmallArms, TextRectangleRenegotiatesTheWindowSize)
{
    CMUSHclientDoc* doc = make_doc();
    doc->m_bNAWS_wanted = TRUE;
    doc->m_nWrapColumn = 80;
    doc->m_gui_rows = 30;
    wire(doc).sent.clear();

    EXPECT_EQ(eOK, doc->TextRectangle(10, 10, 200, 200, 1, 0, 1, 0, 0));

    // The discriminator: against the old body nothing at all goes on the wire.
    const std::string expect = { '\xFF', '\xFA', '\x1F', 0, 80, 0, 30, '\xFF', '\xF0' };
    EXPECT_TRUE(wire(doc).sent.find(expect) != std::string::npos);

    // Control, so that "always send NAWS regardless" also fails: with NAWS not
    // negotiated the call must stay silent.  This is SendWindowSizes' own gate,
    // and asserting it here pins that TextRectangle goes THROUGH that gate
    // rather than hand-rolling a packet.
    wire(doc).sent.clear();
    doc->m_bNAWS_wanted = FALSE;
    EXPECT_EQ(eOK, doc->TextRectangle(10, 10, 200, 200, 1, 0, 1, 0, 0));
    EXPECT_TRUE(wire(doc).sent.empty());
    delete doc;
}

// ── Auto-wrap-to-window-width ────────────────────────────────────────────────
// The wx port used to overwrite m_nWrapColumn from the window width on every
// resize, font change and plugin text-rectangle change, with no regard for the
// world's auto_wrap_window_width option — so "wrap output at column N" did
// nothing, and the window-derived value was written back into the world file on
// the next save, destroying the user's setting.  It also clamped to 1..65535
// rather than the 20..MAX_LINE_WIDTH the option itself declares.

TEST(AutoWrap, OptionOffLeavesTheConfiguredWrapColumnAlone)
{
    CMUSHclientDoc* doc = make_doc();
    doc->m_bAutoWrapWindowWidth = FALSE;          // the default
    EXPECT_EQ(0, doc->AutoWrapColumnFor(155));    // 0 == "don't touch it"
    delete doc;
}

TEST(AutoWrap, OptionOnFollowsTheWindow)
{
    CMUSHclientDoc* doc = make_doc();
    doc->m_bAutoWrapWindowWidth = TRUE;
    EXPECT_EQ(155, doc->AutoWrapColumnFor(155));
    delete doc;
}

TEST(AutoWrap, ResultIsClampedToTheOptionsOwnRange)
{
    CMUSHclientDoc* doc = make_doc();
    doc->m_bAutoWrapWindowWidth = TRUE;
    // A very narrow window, or a huge font: Windows floors at 20 columns.
    EXPECT_EQ(20, doc->AutoWrapColumnFor(3));
    // A 5K display with a small font: MAX_LINE_WIDTH is the ceiling, and it is
    // also the maximum the wrap_column option accepts, so exceeding it would
    // persist a value the loader rejects.
    EXPECT_EQ(MAX_LINE_WIDTH, doc->AutoWrapColumnFor(MAX_LINE_WIDTH + 1));
    EXPECT_EQ(MAX_LINE_WIDTH, doc->AutoWrapColumnFor(9999));
    delete doc;
}

TEST(AutoWrap, UnmeasuredPaneIsNotAnExcuseToChangeTheWrapColumn)
{
    CMUSHclientDoc* doc = make_doc();
    doc->m_bAutoWrapWindowWidth = TRUE;
    EXPECT_EQ(0, doc->AutoWrapColumnFor(0));
    EXPECT_EQ(0, doc->AutoWrapColumnFor(-4));
    delete doc;
}

TEST(AutoWrap, ClampWrapColumnPassesLegalValuesThrough)
{
    EXPECT_EQ(20,  CMUSHclientDoc::ClampWrapColumn(20));
    EXPECT_EQ(80,  CMUSHclientDoc::ClampWrapColumn(80));
    EXPECT_EQ(MAX_LINE_WIDTH, CMUSHclientDoc::ClampWrapColumn(MAX_LINE_WIDTH));
}

// NAWS must report the wrap column the client will actually wrap at, which is
// what CMUSHView::OnSize does (SendWindowSizes (pDoc->m_nWrapColumn)).  With
// auto-wrap off that is the configured column, NOT the window width.
TEST(AutoWrap, NawsReportsTheConfiguredColumnWhenAutoWrapIsOff)
{
    CMUSHclientDoc* doc = make_doc();
    doc->m_bNAWS_wanted = TRUE;
    doc->m_bAutoWrapWindowWidth = FALSE;
    doc->m_nWrapColumn = 100;
    doc->m_gui_rows = 40;
    doc->SendWindowSizes(doc->m_nWrapColumn);
    std::string expect = { '\xFF', '\xFA', '\x1F', 0, 100, 0, 40, '\xFF', '\xF0' };
    EXPECT_TRUE(wire(doc).sent.find(expect) != std::string::npos);
    delete doc;
}

// ...but the width NAWS reports is the caller's argument, not m_nWrapColumn.
// CMUSHclientDoc::Configure announces the new wrap column BEFORE storing it
// (configuration.cpp), which is the one caller where the two disagree — so a
// port arm that read the member announced the OLD width every time the user
// changed "wrap column" in World Properties.
TEST(AutoWrap, NawsReportsTheWidthItWasGivenNotTheStoredOne)
{
    CMUSHclientDoc* doc = make_doc();
    doc->m_bNAWS_wanted = TRUE;
    doc->m_nWrapColumn = 80;        // what the world is still configured for
    doc->m_gui_rows = 25;
    doc->SendWindowSizes(120);      // what Configure is about to change it to
    std::string expect = { '\xFF', '\xFA', '\x1F', 0, 120, 0, 25, '\xFF', '\xF0' };
    EXPECT_TRUE(wire(doc).sent.find(expect) != std::string::npos);
    delete doc;
}

// Widths and heights above 255 need both bytes; NAWS is big-endian (RFC 1073).
TEST(AutoWrap, NawsSplitsWideDimensionsAcrossBothBytes)
{
    CMUSHclientDoc* doc = make_doc();
    doc->m_bNAWS_wanted = TRUE;
    doc->m_nWrapColumn = 80;                      // deliberately not the width
    doc->m_gui_rows = 300;                        // 0x012C
    doc->SendWindowSizes(500);                    // 0x01F4
    std::string expect = { '\xFF', '\xFA', '\x1F',
                           '\x01', '\xF4', '\x01', '\x2C', '\xFF', '\xF0' };
    EXPECT_TRUE(wire(doc).sent.find(expect) != std::string::npos);
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

// OnGameTrace is a protected afx_msg handler; reach it for tests via a derived
// using-declaration (legal member-pointer access, no doc.h change).
static void call_OnGameTrace(CMUSHclientDoc& d)
{
    struct Access : CMUSHclientDoc { using CMUSHclientDoc::OnGameTrace; };
    (d.*(&Access::OnGameTrace))();
}

// OnGameTrace with a partially-built line pushes the pending text onto a new
// line first.
TEST(SmallArms, GameTraceOnPartialLine)
{
    CMUSHclientDoc* doc = make_doc();
    char text[] = "partial-prompt> ";
    doc->OnBytesReceived(text, (int)sizeof text - 1);
    call_OnGameTrace(*doc);
    EXPECT_TRUE(doc->m_bTrace);
    EXPECT_TRUE(saw(doc, "partial-prompt>"));
    delete doc;
}

// ── round 5: proxy message-box arms, transport-events bridge, misc ────────

// Every proxy refusal has a UMessageBox arm taken when error notification is
// NOT routed to the output window; headless that box is a recorded no-op.
TEST(ProxyR5, RefusalsViaMessageBoxArm)
{
    NotifyToOutput toBox(true, /*toOutput=*/false);

    {   // Response1 GSSAPI refusal
        CMUSHclientDoc* doc = make_doc();
        char buff[2] = { 5, 1 };
        EXPECT_EQ(-1, doc->ProcessProxyResponse1(buff, 2));
        EXPECT_FALSE(saw(doc, "Proxy server cannot authenticate"));  // box, not Note
        delete doc;
    }
    {   // Response2 refused authentication
        CMUSHclientDoc* doc = make_doc();
        char buff[2] = { 1, 5 };
        EXPECT_EQ(-1, doc->ProcessProxyResponse2(buff, 2));
        EXPECT_FALSE(saw(doc, "refused authentication"));
        delete doc;
    }
    {   // Response3 SOCKS4 refusal
        CMUSHclientDoc* doc = make_doc();
        doc->m_iSocksProcessing = eProxyServerSocks4;
        char buff[2] = { 0, 91 };
        EXPECT_EQ(-1, doc->ProcessProxyResponse3(buff, 2));
        EXPECT_FALSE(saw(doc, "refused connection"));
        delete doc;
    }
    {   // Response3 SOCKS5 refusal
        CMUSHclientDoc* doc = make_doc();
        doc->m_iSocksProcessing = eProxyServerSocks5;
        char buff[2] = { 5, 1 };
        EXPECT_EQ(-1, doc->ProcessProxyResponse3(buff, 2));
        EXPECT_FALSE(saw(doc, "refused connection"));
        delete doc;
    }
    {   // unexpected version byte
        CMUSHclientDoc* doc = make_doc();
        char buff[2] = { 4, 0 };
        EXPECT_EQ(-1, doc->ProcessProxyResponse1(buff, 2));
        EXPECT_FALSE(saw(doc, "Unexpected proxy server response"));
        delete doc;
    }
}

// Responses 2/3 with the wrong version byte reject via the shared check.
TEST(ProxyR5, LaterResponsesWrongVersionByte)
{
    NotifyToOutput toOutput;
    {
        CMUSHclientDoc* doc = make_doc();
        char buff[2] = { 9, 0 };            // Response2 expects version 1
        EXPECT_EQ(-1, doc->ProcessProxyResponse2(buff, 2));
        delete doc;
    }
    {
        CMUSHclientDoc* doc = make_doc();
        doc->m_iSocksProcessing = eProxyServerSocks5;
        char buff[2] = { 9, 0 };            // Response3 (SOCKS5) expects 5
        EXPECT_EQ(-1, doc->ProcessProxyResponse3(buff, 2));
        delete doc;
    }
}

// Short packets synthesize the failure byte before dispatch.
TEST(ProxyR5, ShortPacketsSynthesizeFailure)
{
    NotifyToOutput toOutput;
    {   // Response1 count < 2: 0xFF = no acceptable methods
        CMUSHclientDoc* doc = make_doc();
        char buff[2] = { 5, 0 };
        EXPECT_EQ(-1, doc->ProcessProxyResponse1(buff, 1));
        EXPECT_TRUE(saw(doc, "no acceptable methods"));
        delete doc;
    }
    {   // Response3 SOCKS5 count < 2: general failure
        CMUSHclientDoc* doc = make_doc();
        doc->m_iSocksProcessing = eProxyServerSocks5;
        char buff[2] = { 5, 9 };
        EXPECT_EQ(-1, doc->ProcessProxyResponse3(buff, 1));
        EXPECT_TRUE(saw(doc, "general SOCKS server failure"));
        delete doc;
    }
}

// RFC 1929 length bytes cap the username/password at 255 characters.
TEST(ProxyR5, OverlongProxyCredentialsRefused)
{
    CMUSHclientDoc* doc = make_doc();
    doc->m_strProxyUserName = CString('u', 300);
    doc->m_strProxyPassword = "pw";
    char buff[2] = { 5, 2 };   // auth required → SendProxyUserNameAndPassword
    EXPECT_EQ(-1, doc->ProcessProxyResponse1(buff, 2));
    // the refusal disconnected: the transport was torn down before anything
    // could hit the wire
    EXPECT_EQ(nullptr, doc->m_pTransport);
    delete doc;
}

// The doc-side ITransportEvents bridge: the passive callbacks forward (or
// deliberately ignore) without a live connection.
TEST(TransportEventsR5, BridgeForwardsTlsAndIgnoresWritable)
{
    CMUSHclientDoc* doc = make_doc();
    delete doc->m_pTransport;         // replace the recording transport
    doc->m_pTransport = nullptr;
    doc->CreateTransport();           // real adapter + the events bridge
    ASSERT_NE(nullptr, doc->m_pTransportEvents);

    // CDocTransportEvents' only base is pal::ITransportEvents, so the
    // pointer values coincide (the class is complete only in doc_shared.cpp).
    pal::ITransportEvents* ev =
        reinterpret_cast<pal::ITransportEvents*>(doc->m_pTransportEvents);

    ev->onWritable();                                       // no-op by design
    doc->m_iConnectPhase = eConnectConnectedToMud;
    ev->onTlsEstablished("TLSv1.3", "TLS_AES_128_GCM_SHA256", nullptr);
    EXPECT_TRUE(saw(doc, "TLSv1.3"));
    ev->onTlsFailed(pal::TlsFailure::Handshake, "wrong version number");
    EXPECT_TRUE(saw(doc, "wrong version number"));

    doc->TeardownTransport();
    delete doc;
}

// ConnectionClosed: the button-bar update for view numbers 1-10, auto-log
// close, and the notify-on-disconnect Note/box arms.
TEST(ConnectionClosedR5, ViewButtonAutoLogAndNotifyArms)
{
    {   // view number in range + auto-log close + notify via Note
        BOOL saved = App.m_bNotifyOnDisconnect;
        App.m_bNotifyOnDisconnect = TRUE;
        NotifyToOutput toOutput;
        CMUSHclientDoc* doc = make_doc();
        doc->m_view_number = 1;

        char logpath[128];
        snprintf(logpath, sizeof logpath, "/tmp/mc_r5_autolog_%d.txt", (int)getpid());
        doc->m_logfile = fopen(logpath, "w");
        ASSERT_NE(nullptr, doc->m_logfile);
        doc->m_logfile_name = logpath;
        doc->m_strAutoLogFileName = logpath;   // marks the log auto-opened

        doc->m_bDisconnectOK = false;          // unexpected disconnect
        doc->ConnectionClosed(0);
        EXPECT_EQ(nullptr, doc->m_logfile);    // CloseLog ran
        EXPECT_TRUE(saw(doc, "server has closed the connection"));
        unlink(logpath);
        App.m_bNotifyOnDisconnect = saved;
        delete doc;
    }
    {   // notify via message box (not routed to output)
        BOOL saved = App.m_bNotifyOnDisconnect;
        App.m_bNotifyOnDisconnect = TRUE;
        NotifyToOutput toBox(true, /*toOutput=*/false);
        CMUSHclientDoc* doc = make_doc();
        doc->m_bDisconnectOK = false;
        doc->ConnectionClosed(0);
        EXPECT_FALSE(saw(doc, "server has closed the connection"));
        App.m_bNotifyOnDisconnect = saved;
        delete doc;
    }
}

// Host-name resolution failure with box-style notification.
TEST(ConnectionR5, ResolveFailureViaMessageBoxArm)
{
    NotifyToOutput toBox(true, /*toOutput=*/false);
    CMUSHclientDoc* doc = make_doc();
    doc->m_iConnectPhase = eConnectMudNameLookup;
    struct in_addr addr; addr.s_addr = 0;
    doc->LookupHostNameComplete(addr, eConnectMudNameLookup, false);
    EXPECT_FALSE(saw(doc, "Unable to resolve host name"));
    EXPECT_EQ((int)eConnectNotConnected, (int)doc->m_iConnectPhase);
    delete doc;
}

// ── round 5: SendMsg / DoSendMsg family ───────────────────────────────────

TEST(SendMsgR5, GuardsAndEchoFlags)
{
    CMUSHclientDoc* doc = make_doc();
    RecTransport& t = wire(doc);

    // OnPluginSent guard: nothing may be sent from inside the callback
    doc->m_bPluginProcessingSent = true;
    doc->SendMsg("blocked", false, false, false);
    doc->DoSendMsg("blocked", false, false);
    EXPECT_TRUE(t.sent.empty());
    doc->m_bPluginProcessingSent = false;

    // world-suppressed echo
    doc->m_bNoEcho = true;
    doc->SendMsg("quiet", true, false, false);
    EXPECT_NE(std::string::npos, t.sent.find("quiet"));
    EXPECT_FALSE(saw(doc, "quiet"));   // echo suppressed
    doc->m_bNoEcho = false;

    // German umlaut fixup arm
    doc->m_bTranslateGerman = true;
    doc->SendMsg("gruesse", false, false, false);
    doc->m_bTranslateGerman = false;

    // mapper accumulation arm (AddToMap is a headless no-op)
    doc->m_bMapping = true;
    doc->DoSendMsg("north", false, false);
    doc->m_bMapping = false;

    delete doc;
}

TEST(SendMsgR5, ImmediateFlagsWhenQueueBusy)
{
    CMUSHclientDoc* doc = make_doc();
    doc->m_iSpeedWalkDelay = 10;                       // queue machinery on
    doc->m_QueuedCommandsList.AddTail("nnorth");       // something pending
    // bQueueIt=false but the queue is non-empty → IMMEDIATE_WITH_ECHO ('I'),
    // lowercased to 'i' because bLogIt=false.
    doc->SendMsg("look", true, false, false);
    doc->m_QueuedCommandsList.AddTail("nnorth");
    // bEcho=false → IMMEDIATE_WITHOUT_ECHO ('W') → 'w'.
    doc->SendMsg("hide", false, false, false);

    // Each immediate command is queued as <flag-byte><command>; assert the
    // exact flag byte, not merely that a command was queued.  (Swapping the
    // WITH/WITHOUT_ECHO flag would flip 'i'→'w' on "look".)
    bool sawLook = false, sawHide = false;
    for (POSITION pos = doc->m_QueuedCommandsList.GetHeadPosition(); pos; ) {
        CString e = doc->m_QueuedCommandsList.GetNext(pos);
        if (e == "ilook") sawLook = true;   // IMMEDIATE_WITH_ECHO, no-log
        if (e == "whide") sawHide = true;   // IMMEDIATE_WITHOUT_ECHO, no-log
    }
    EXPECT_TRUE(sawLook);
    EXPECT_TRUE(sawHide);

    doc->m_iSpeedWalkDelay = 0;
    delete doc;
}

TEST(SendMsgR5, SpamPreventionInjectsMessage)
{
    CMUSHclientDoc* doc = make_doc();
    RecTransport& t = wire(doc);
    doc->m_bEnableSpamPrevention = true;
    doc->m_iSpamLineCount = 3;
    doc->m_strSpamMessage = "look";

    for (int i = 0; i < 5; ++i)
        doc->DoSendMsg("kill rat", false, false);

    // after the 4th identical command the spam breaker sent "look"
    EXPECT_NE(std::string::npos, t.sent.find("look"));
    delete doc;
}

// A command echoed onto a line that already has MUD text starts a new line
// (the NOTE_OR_COMMAND flag-change arm).
TEST(SendMsgR5, CommandAfterPartialMudLineStartsNewLine)
{
    CMUSHclientDoc* doc = make_doc();
    char prompt[] = "prompt> ";
    doc->OnBytesReceived(prompt, (int)sizeof prompt - 1);   // partial MUD line
    doc->DoSendMsg("north", true, false);                   // echoed command
    EXPECT_TRUE(saw(doc, "north"));
    delete doc;
}

// ── round 5: queued-command status display ────────────────────────────────

TEST(QueueR5, ShowQueuedCommandsFormatsRepeatsAndOverflow)
{
    // ShowQueuedCommands publishes the formatted queue via SetStatusMessageNow,
    // which forwards to g_wxhook_set_status (a no-op headless).  Capture it so
    // the formatting is actually observable.
    static std::string s_status;
    auto* saved = g_wxhook_set_status;
    g_wxhook_set_status = [](const char* s){ s_status = s ? s : ""; };

    CMUSHclientDoc* doc = make_doc();

    // empty entries skipped; repeats counted (2×); multi-char directions
    // bracketed.  Each entry is <flag-byte><command>; ShowQueuedCommands strips
    // the leading flag with Mid(1), so "n"→"" (skipped), "nnorth"→"north", etc.
    doc->m_QueuedCommandsList.AddTail("n");        // empty command → skipped
    doc->m_QueuedCommandsList.AddTail("nnorth");
    doc->m_QueuedCommandsList.AddTail("nnorth");   // repeated
    doc->m_QueuedCommandsList.AddTail("nne");
    s_status.clear();
    doc->ShowQueuedCommands();
    EXPECT_EQ(std::string("Queued: 2(north) (ne) "), s_status);

    // a long queue overflows MAX_SHOWN and ends with " ..."
    doc->m_QueuedCommandsList.RemoveAll();
    for (int i = 0; i < 60; ++i) {
        doc->m_QueuedCommandsList.AddTail("neast");
        doc->m_QueuedCommandsList.AddTail("nwest");
    }
    s_status.clear();
    doc->ShowQueuedCommands();
    EXPECT_EQ(0u, s_status.find("Queued: "));
    EXPECT_NE(std::string::npos, s_status.find("..."));

    doc->m_QueuedCommandsList.RemoveAll();
    delete doc;

    g_wxhook_set_status = saved;
}

// ── round 5: option-machinery guard arms (free helpers) ───────────────────

TEST(OptionHelpersR5, RangeAndSizeGuards)
{
    struct { int v4; } base = { 0 };
    static char nm[] = "opt";
    tConfigurationNumericOption table[2] = {};
    table[0].pName = nm;
    table[0].iOffset = 0;
    table[0].iLength = 4;
    table[0].iMinimum = 0;
    table[0].iMaximum = 100;

    bool changed = false;
    EXPECT_EQ((long)eUnknownOption,
              SetBaseOptionItem(-1, table, 1, (char*)&base, 1, changed));
    EXPECT_EQ((long)eUnknownOption,
              SetBaseOptionItem(5, table, 1, (char*)&base, 1, changed));
    EXPECT_EQ(-1.0, GetBaseOptionItem(-1, table, 1, (char*)&base));

    table[0].iLength = 3;   // no such member size
    EXPECT_EQ((long)eUnknownOption,
              SetBaseOptionItem(0, table, 1, (char*)&base, 1, changed));

    CString sbase;
    tConfigurationAlphaOption atable[1] = {};
    CString v("x");
    EXPECT_EQ((long)eUnknownOption,
              SetBaseAlphaOptionItem(-1, atable, 0, (char*)&sbase, v, changed));
    EXPECT_STREQ("", (LPCTSTR)GetBaseAlphaOptionItem(-1, atable, 0, (char*)&sbase));
}

// The alternative-inverse rendering keeps the missing ANSI combinations.
TEST(StyleRgbR5, AlternativeInverseRemainingArms)
{
    CMUSHclientDoc* doc = make_doc();
    doc->m_bAlternativeInverse = TRUE;
    doc->m_normalcolour[1] = RGB(100, 0, 0);
    doc->m_normalcolour[2] = RGB(0, 100, 0);
    doc->m_boldcolour[1]   = RGB(200, 0, 0);
    COLORREF fg = 0, bg = 0;

    // inverse, fg < 8, NOT hilited → normal palette for colour2
    CStyle invPlain = make_style(COLOUR_ANSI | INVERSE, 1, 2);
    doc->GetStyleRGB(&invPlain, fg, bg);
    EXPECT_EQ(RGB(0, 100, 0), fg);        // colour1 from background
    EXPECT_EQ(RGB(100, 0, 0), bg);        // colour2 from normal palette

    // not inverse, fg >= 8 → xterm palette
    CStyle x = make_style(COLOUR_ANSI, 20, 2);
    doc->GetStyleRGB(&x, fg, bg);
    EXPECT_EQ(RGB(0, 100, 0), bg);

    // not inverse, fg < 8, NOT hilited → normal palette both ways
    CStyle plain = make_style(COLOUR_ANSI, 1, 2);
    doc->GetStyleRGB(&plain, fg, bg);
    EXPECT_EQ(RGB(100, 0, 0), fg);
    EXPECT_EQ(RGB(0, 100, 0), bg);

    // inverse with bg >= 8 → xterm palette for colour1
    CStyle invx = make_style(COLOUR_ANSI | INVERSE, 1, 20);
    doc->GetStyleRGB(&invx, fg, bg);

    doc->m_bAlternativeInverse = FALSE;

    // classic mode: HILITE with fg >= 8 → xterm palette (not bold table)
    CStyle boldx = make_style(COLOUR_ANSI | HILITE, 20, 2);
    doc->GetStyleRGB(&boldx, fg, bg);
    delete doc;
}

// ── round 5: small doc_shared arms ────────────────────────────────────────

// Reversing a bracketed direction word that is not in the direction map.
TEST(SmallArmsR5, ReverseSpeedwalkUnknownWord)
{
    CMUSHclientDoc* doc = make_doc();
    CString rev = doc->DoReverseSpeedwalk("(fly)");
    delete doc;
}

// Input-log postamble with a '%' goes through FormatTime.
TEST(SmallArmsR5, LogPostambleFormatsTime)
{
    CMUSHclientDoc* doc = make_doc();
    char logpath[128];
    snprintf(logpath, sizeof logpath, "/tmp/mc_r5_log_%d.txt", (int)getpid());
    doc->m_logfile = fopen(logpath, "w");
    ASSERT_NE(nullptr, doc->m_logfile);
    doc->m_logfile_name = logpath;
    doc->m_log_input = true;
    doc->m_strLogLinePreambleInput  = "%H:";
    doc->m_strLogLinePostambleInput = " [%H]";
    doc->LogCommand("north");
    fclose(doc->m_logfile);
    doc->m_logfile = nullptr;
    unlink(logpath);
    delete doc;
}

// AddStyle's no-current-line guards return NULL instead of crashing.
TEST(SmallArmsR5, AddStyleWithoutALine)
{
    CMUSHclientDoc* doc = make_doc();
    CLine* savedLine = doc->m_pCurrentLine;
    doc->m_pCurrentLine = nullptr;
    EXPECT_EQ(nullptr, doc->AddStyle(0, 0, 0, 0, nullptr));
    EXPECT_EQ(nullptr, doc->AddStyle(0, 0, 0, 0, "", "", ""));
    doc->m_pCurrentLine = savedLine;
    delete doc;
}

// New-activity sound arm: first unseen line with a configured sound (the
// headless PlaySoundFile backend simply reports no audio).
TEST(SmallArmsR5, NewActivitySoundRequested)
{
    CMUSHclientDoc* doc = make_doc();
    doc->m_new_activity_sound = "beep.wav";
    doc->m_new_lines = 0;
    char line[] = "something happened\r\n";
    doc->OnBytesReceived(line, (int)sizeof line - 1);
    EXPECT_TRUE(doc->m_new_lines > 0);
    delete doc;
}

// ── unread lines / "is this world's window active?" ───────────────────────
//
// StartNewLine (doc_shared.cpp) counts a line as unread only while the world's
// window is NOT the active one.  Windows answers that with the two CView
// pointers its views maintain; the port never constructs those views, so the
// test was permanently "inactive" there: every line of every world — including
// the one being read — counted as unread, and nothing ever reset the counter.
// Both frontends now drive the same predicate (IsWorldViewActive), which the
// wx frame sets from its activate/deactivate handling.
TEST(UnreadLines, CounterFollowsWorldWindowActivation)
{
    CMUSHclientDoc* doc = make_doc();
    char line[] = "something happened\r\n";

    // A doc whose window has not been activated is a background world.
    EXPECT_FALSE(doc->IsWorldViewActive());
    doc->m_new_lines = 0;
    doc->OnBytesReceived(line, (int)sizeof line - 1);
    const long unreadWhileHidden = doc->m_new_lines;
    EXPECT_TRUE(unreadWhileHidden > 0);

    // Bringing the window to the front marks them read…
    doc->SetWorldViewActive(true);
    EXPECT_TRUE(doc->IsWorldViewActive());
    EXPECT_EQ(0L, doc->m_new_lines);

    // …and output arriving in the world you are looking at is never "unread".
    doc->OnBytesReceived(line, (int)sizeof line - 1);
    EXPECT_EQ(0L, doc->m_new_lines);

    // Switching away starts the count again.
    doc->SetWorldViewActive(false);
    EXPECT_FALSE(doc->IsWorldViewActive());
    doc->OnBytesReceived(line, (int)sizeof line - 1);
    EXPECT_TRUE(doc->m_new_lines > 0);

    delete doc;
}

// GetInfo(113) is "is this world's window the active one?" (the same predicate),
// and GetInfo(202) is the unread-line count.  113 used to answer "the world is
// connected" on the port — a different question, and one 106/107 already answer.
TEST(UnreadLines, GetInfoReportsActivationAndUnreadCount)
{
    CMUSHclientDoc* doc = make_doc();
    char line[] = "something happened\r\n";

    doc->m_new_lines = 0;
    EXPECT_FALSE(doc->GetInfo(113).boolVal != 0);   // connected, but not active
    doc->OnBytesReceived(line, (int)sizeof line - 1);
    EXPECT_EQ(doc->m_new_lines, doc->GetInfo(202).lVal);
    EXPECT_TRUE(doc->GetInfo(202).lVal > 0);

    doc->SetWorldViewActive(true);
    EXPECT_TRUE(doc->GetInfo(113).boolVal != 0);
    EXPECT_EQ(0L, doc->GetInfo(202).lVal);

    delete doc;
}

// ── ApplyTlsFallbackChoice — what a TLS failure prompt actually does ─────────
// The two recoveries are not interchangeable: a certificate that cannot be
// verified leaves the connection ENCRYPTED, and only a failed handshake is
// allowed to drop to plaintext.  Both frontends (mainfrm.cpp on Windows,
// platform/wx_mfc_impl.cpp here) used to carry their own hand-copied version
// of this with no test on either platform, and a mistake is invisible: no
// error, no output, the world just connects.

static void tls_doc(CMUSHclientDoc& doc)
{
    doc.m_bUseSSL              = 1;
    doc.m_bSSLAllowInvalidCert = false;
}

TEST(Connection, TlsCertFailureConsentStaysEncrypted)
{
    CMUSHclientDoc doc; tls_doc(doc);

    EXPECT_TRUE(doc.ApplyTlsFallbackChoice(/*certFailure*/true, /*agreed*/true));
    EXPECT_TRUE(doc.m_bSSLAllowInvalidCert);
    // THE assertion.  Consenting to an unverifiable certificate must never
    // turn encryption off -- that is a different question the user was not
    // asked, and the answer they gave is not consent to it.
    EXPECT_TRUE(doc.m_bUseSSL != 0);
}

TEST(Connection, TlsHandshakeFailureConsentGoesInTheClear)
{
    CMUSHclientDoc doc; tls_doc(doc);

    EXPECT_TRUE(doc.ApplyTlsFallbackChoice(/*certFailure*/false, /*agreed*/true));
    EXPECT_TRUE(doc.m_bUseSSL == 0);            // deliberately unencrypted
    // ...and it does NOT also whitelist bad certificates for this world: the
    // next connection is plaintext, so the flag would be a lie left behind for
    // whenever TLS is turned back on.
    EXPECT_FALSE(doc.m_bSSLAllowInvalidCert);
}

TEST(Connection, TlsPromptDeclinedChangesNothingEitherWay)
{
    // Declining must be inert -- no reconnect, and above all no downgrade.
    for (int certFailure = 0; certFailure <= 1; ++certFailure) {
        CMUSHclientDoc doc; tls_doc(doc);
        EXPECT_FALSE(doc.ApplyTlsFallbackChoice(certFailure != 0, /*agreed*/false));
        EXPECT_TRUE(doc.m_bUseSSL != 0);
        EXPECT_FALSE(doc.m_bSSLAllowInvalidCert);
    }
}

TEST(Connection, TlsChoiceOnlyEverLoosensTheArmItWasAskedAbout)
{
    // Cross-check the whole 2x2 in one place, so a future edit that makes both
    // arms do the same thing fails here even if the two tests above were
    // updated to match it.
    struct { bool certFailure, agreed, expectSSL, expectAllowInvalid; } kCases[] = {
        { true,  true,  true,  true  },   // unverified, still encrypted
        { true,  false, true,  false },   // declined
        { false, true,  false, false },   // plaintext fallback
        { false, false, true,  false },   // declined
    };
    for (const auto& c : kCases) {
        CMUSHclientDoc doc; tls_doc(doc);
        const bool reconnect = doc.ApplyTlsFallbackChoice(c.certFailure, c.agreed);
        EXPECT_EQ(c.agreed, reconnect);
        EXPECT_EQ(c.expectSSL, doc.m_bUseSSL != 0);
        EXPECT_EQ(c.expectAllowInvalid, (bool) doc.m_bSSLAllowInvalidCert);
    }
}

// ── the speed-walk / command queue drain ────────────────────────────────────
// The queue is how a speed-walk is paced: SendMsg stores each command as one
// flag byte plus the text, and a drain releases them a slot at a time.  Until
// now there were THREE copies of the drain -- CTimerWnd::OnTimer,
// CTimerWnd::ChangeTimerRate's flush arm, and the port's re-implementation
// inside the world frame's timer, which exists because there is no CTimerWnd
// here -- and none of them was reachable from any test.  They now share one
// body, and this is it under test.
//
// The flag byte (doc.h:317-325): letter = which of four kinds, CASE = log it.
//   E/e queue+echo   N/n queue+no-echo   I/i immediate+echo   W/w immediate+no-echo

TEST(QueueDrain, FlagByteDecodesAllEightCodes)
{
    struct { char c; bool echo, log, paced; } kCases[] = {
        { 'E', true,  true,  true  },   { 'e', true,  false, true  },
        { 'N', false, true,  true  },   { 'n', false, false, true  },
        { 'I', true,  true,  false },   { 'i', true,  false, false },
        { 'W', false, true,  false },   { 'w', false, false, false },
    };
    for (const auto& c : kCases) {
        const CMUSHclientDoc::QueuedFlags f =
            CMUSHclientDoc::DecodeQueuedCommandFlag(c.c);
        if (f.bEcho != c.echo || f.bLog != c.log || f.bPacedSlot != c.paced)
            printf("  '%c': echo %d log %d paced %d (want %d %d %d)\n",
                   c.c, (int)f.bEcho, (int)f.bLog, (int)f.bPacedSlot,
                   (int)c.echo, (int)c.log, (int)c.paced);
        EXPECT_EQ(c.echo,  f.bEcho);
        EXPECT_EQ(c.log,   f.bLog);
        EXPECT_EQ(c.paced, f.bPacedSlot);
    }
}

TEST(QueueDrain, LowerCaseActuallySuppressesLogging)
{
    // UPSTREAM-BUGS #12.  SendMsg lower-cases the flag byte to mean "do not
    // log this", and every drain recovered that as `c >= 'A'` -- which all
    // eight codes satisfy, 'e' being 101 and 'A' being 65.  So the flag was
    // read as "log it" no matter what, and the feature had never worked.
    EXPECT_TRUE(CMUSHclientDoc::DecodeQueuedCommandFlag('E').bLog);
    EXPECT_FALSE(CMUSHclientDoc::DecodeQueuedCommandFlag('e').bLog);
    // The four lower-case codes are exactly the four that must not log.
    for (char c : { 'e', 'n', 'i', 'w' })
        EXPECT_FALSE(CMUSHclientDoc::DecodeQueuedCommandFlag(c).bLog);
    for (char c : { 'E', 'N', 'I', 'W' })
        EXPECT_TRUE(CMUSHclientDoc::DecodeQueuedCommandFlag(c).bLog);
}

TEST(QueueDrain, EchoIsCaseInsensitive)
{
    // UPSTREAM-BUGS #13.  The tick drain compared with toupper and the
    // flush-everything drain compared case-sensitively, so the same command
    // echoed from one and not the other.  Case carries the LOG flag only.
    EXPECT_EQ(CMUSHclientDoc::DecodeQueuedCommandFlag('E').bEcho,
              CMUSHclientDoc::DecodeQueuedCommandFlag('e').bEcho);
    EXPECT_EQ(CMUSHclientDoc::DecodeQueuedCommandFlag('I').bEcho,
              CMUSHclientDoc::DecodeQueuedCommandFlag('i').bEcho);
    EXPECT_EQ(CMUSHclientDoc::DecodeQueuedCommandFlag('N').bEcho,
              CMUSHclientDoc::DecodeQueuedCommandFlag('n').bEcho);
}

TEST(QueueDrain, OneSlotStopsAfterThePacedCommand)
{
    CMUSHclientDoc* doc = make_doc();
    // Two immediates ride along with the first queued command; the second
    // queued command waits for the next slot.
    doc->m_QueuedCommandsList.AddTail("Ilook");     // immediate
    doc->m_QueuedCommandsList.AddTail("Enorth");    // paced -- ends the slot
    doc->m_QueuedCommandsList.AddTail("Eeast");     // next slot

    EXPECT_EQ(2, doc->DrainOneQueuedSlot());
    EXPECT_EQ(1, (int)doc->m_QueuedCommandsList.GetCount());
    EXPECT_TRUE(wire(doc).sent.find("look")  != std::string::npos);
    EXPECT_TRUE(wire(doc).sent.find("north") != std::string::npos);
    EXPECT_TRUE(wire(doc).sent.find("east")  == std::string::npos);

    EXPECT_EQ(1, doc->DrainOneQueuedSlot());
    EXPECT_TRUE(doc->m_QueuedCommandsList.IsEmpty());
    EXPECT_TRUE(wire(doc).sent.find("east")  != std::string::npos);
    delete doc;
}

TEST(QueueDrain, DrainAllEmptiesTheQueueRegardlessOfPacing)
{
    CMUSHclientDoc* doc = make_doc();
    doc->m_QueuedCommandsList.AddTail("Enorth");
    doc->m_QueuedCommandsList.AddTail("Eeast");
    doc->m_QueuedCommandsList.AddTail("nsouth");

    EXPECT_EQ(3, doc->DrainAllQueuedCommands());
    EXPECT_TRUE(doc->m_QueuedCommandsList.IsEmpty());
    const std::string& out = wire(doc).sent;
    EXPECT_TRUE(out.find("north") != std::string::npos);
    EXPECT_TRUE(out.find("east")  != std::string::npos);
    EXPECT_TRUE(out.find("south") != std::string::npos);
    delete doc;
}

TEST(QueueDrain, PacingReleasesOneSlotPerDelay)
{
    CMUSHclientDoc* doc = make_doc();
    doc->m_iSpeedWalkDelay = 250;                  // ms between commands
    for (const char* c : { "Ea", "Eb", "Ec" })
        doc->m_QueuedCommandsList.AddTail(c);

    int accum = 0;
    // Two ticks of 100 ms is not yet a slot.
    EXPECT_EQ(0, doc->DrainQueuedCommands(100, accum));
    EXPECT_EQ(0, doc->DrainQueuedCommands(100, accum));
    EXPECT_EQ(3, (int)doc->m_QueuedCommandsList.GetCount());
    // The third crosses 250 and releases exactly one.
    EXPECT_EQ(1, doc->DrainQueuedCommands(100, accum));
    EXPECT_EQ(2, (int)doc->m_QueuedCommandsList.GetCount());
    // The remainder carries over rather than being thrown away -- otherwise a
    // delay that is not a multiple of the tick drifts slower and slower.
    EXPECT_EQ(50, accum);
    delete doc;
}

TEST(QueueDrain, ALongTickReleasesEverySlotItCovers)
{
    CMUSHclientDoc* doc = make_doc();
    doc->m_iSpeedWalkDelay = 100;
    for (const char* c : { "Ea", "Eb", "Ec", "Ed" })
        doc->m_QueuedCommandsList.AddTail(c);

    // The app was busy: 350 ms of wall time arrives in one tick.  Three slots
    // are due, and they must all be released rather than one per call.
    int accum = 0;
    EXPECT_EQ(3, doc->DrainQueuedCommands(350, accum));
    EXPECT_EQ(1, (int)doc->m_QueuedCommandsList.GetCount());
    EXPECT_EQ(50, accum);
    delete doc;
}

TEST(QueueDrain, ZeroDelayFlushesImmediatelyAndBanksNoCredit)
{
    CMUSHclientDoc* doc = make_doc();
    doc->m_iSpeedWalkDelay = 0;                    // no pacing wanted
    doc->m_QueuedCommandsList.AddTail("Ea");
    doc->m_QueuedCommandsList.AddTail("Eb");

    int accum = 999;                               // stale credit
    EXPECT_EQ(2, doc->DrainQueuedCommands(100, accum));
    EXPECT_TRUE(doc->m_QueuedCommandsList.IsEmpty());
    EXPECT_EQ(0, accum);
    delete doc;
}

TEST(QueueDrain, CreditIsForgottenWhileTheQueueIsEmpty)
{
    // The leftover from one walk must not shorten the FIRST delay of the next.
    // Note what this needs to discriminate: the empty-queue branch does not
    // accumulate either, so simply idling and reading the accumulator proves
    // nothing -- it is zero either way.  The credit has to be banked first, the
    // queue drained empty, and only then a new command queued.
    CMUSHclientDoc* doc = make_doc();
    doc->m_iSpeedWalkDelay = 250;
    int accum = 0;

    doc->m_QueuedCommandsList.AddTail("Ea");
    doc->DrainQueuedCommands(100, accum);
    doc->DrainQueuedCommands(100, accum);
    EXPECT_EQ(1, doc->DrainQueuedCommands(100, accum));   // 300 >= 250: sent
    EXPECT_TRUE(doc->m_QueuedCommandsList.IsEmpty());
    EXPECT_EQ(50, accum);                                 // 50 left over

    doc->DrainQueuedCommands(100, accum);                 // idle: forget it
    EXPECT_EQ(0, accum);

    // A fresh command now waits a FULL delay.  Two 100 ms ticks is 200, which
    // is short of 250 -- but would be 250 exactly if the 50 had been kept.
    doc->m_QueuedCommandsList.AddTail("Eb");
    EXPECT_EQ(0, doc->DrainQueuedCommands(100, accum));
    EXPECT_EQ(0, doc->DrainQueuedCommands(100, accum));
    EXPECT_EQ(1, (int)doc->m_QueuedCommandsList.GetCount());
    EXPECT_TRUE(wire(doc).sent.find("b") == std::string::npos);
    delete doc;
}

TEST(QueueDrain, AnEmptyEntryIsSkippedRatherThanDecoded)
{
    // A zero-length entry has no flag byte; strCommand[0] on it is a read past
    // the end.  It should be dropped, not decoded as some arbitrary byte.
    CMUSHclientDoc* doc = make_doc();
    doc->m_QueuedCommandsList.AddTail("");
    doc->m_QueuedCommandsList.AddTail("Enorth");
    EXPECT_EQ(1, doc->DrainAllQueuedCommands());
    EXPECT_TRUE(doc->m_QueuedCommandsList.IsEmpty());
    EXPECT_TRUE(wire(doc).sent.find("north") != std::string::npos);
    delete doc;
}

MC_TEST_MAIN()
