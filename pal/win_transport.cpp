// pal/win_transport.cpp — the native Windows pal::ITransport adapter (T5).
//
// Relocation, not invention: every mechanic below is the exact call sequence
// the shipping Windows client used, moved behind the seam —
//
//   socket + events   CWorldSocket (worldsock.cpp): an MFC CAsyncSocket
//                     created with FD_READ|FD_WRITE|FD_CONNECT|FD_CLOSE,
//                     WSAAsyncSelect message delivery (main thread, never
//                     re-entrant)
//   keepalives        CMUSHclientDoc::ConnectSocket's SIO_KEEPALIVE_VALS
//                     WSAIoctl ({on, 2 min idle, 6 s interval})
//   buffered send     CWorldSocket::OnSend — m_outstanding_data append/drain,
//                     including the TLS SSL_write WANT_* retry
//   TLS session       CMUSHclientDoc::StartSSLHandshake (Windows body: TLS1.2
//                     minimum, verify-peer with an always-continue callback,
//                     the CertOpenSystemStore("ROOT") certificate import,
//                     SNI + SSL_set1_host) + ContinueSSLHandshake stepping +
//                     worldsock.cpp OnReceive's SSL_pending drain
//   async DNS         CMUSHclientDoc::LookupHostName / HostNameResolved —
//                     WSAAsyncGetHostByName into a MAXGETHOSTSTRUCT buffer,
//                     completion parsed as ((LPIN_ADDR)hostent->h_addr);
//                     delivered here to an adapter-owned message-only window
//                     instead of the main frame
//
// Structure mirrors platform/posix_transport.cpp 1:1 (that adapter is the
// live-verified reference; reviewing this file is a diff exercise against
// it).  This file CANNOT be compiled here (no MSVC) — by-reasoning parity,
// the pal/gdi_surface.cpp discipline.  Engine policy (Notes, prompts, the
// connect-phase machine, SOCKS, MCCP) stays in doc_shared.cpp.

#ifdef _WIN32

#include "stdafx.h"          // MFC (CAsyncSocket) + winsock
#include "pal/win_transport.h"

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509.h>
#include <wincrypt.h>

#include <memory>
#include <string>

// From: mstcpip.h  (the same block doc.cpp carried — the header itself was
// not shipped with all SDKs, so MUSHclient defines the pieces it needs)
/* Argument structure for SIO_KEEPALIVE_VALS */
struct tcp_keepalive {
    ULONG onoff;
    ULONG keepalivetime;
    ULONG keepaliveinterval;
};
#ifndef IOC_VENDOR
#define IOC_VENDOR    0x18000000
#endif
#ifndef _WSAIOW
#define _WSAIOW(x,y)  (IOC_IN|(x)|(y))
#endif
#ifndef SIO_KEEPALIVE_VALS
#define SIO_KEEPALIVE_VALS  _WSAIOW(IOC_VENDOR,4)
#endif

namespace pal {

namespace {

// verify callback that always succeeds - we check the result after handshake
// (verbatim from doc.cpp: the handshake always completes, the REAL verdict is
// read post-handshake via SSL_get_verify_result so the engine can offer
// "connect anyway" instead of OpenSSL aborting mid-handshake)
int ssl_verify_callback(int /*preverify_ok*/, X509_STORE_CTX* /*ctx*/)
{
    return 1;
}

// Messages posted to the adapter's message-only window.
const UINT WM_WT_RESOLVED       = WM_APP + 401;  // WSAAsyncGetHostByName completion
const UINT WM_WT_RESOLVE_FAILED = WM_APP + 402;  // initiation failed (deferred)
const UINT WM_WT_CONNECTED      = WM_APP + 403;  // immediate connect (deferred)

const char* const kWndClass = "MUSHclientPalWinTransport";

} // namespace


// ── WinTransport ─────────────────────────────────────────────────────────────

class WinTransport : public ITransport {
public:
    explicit WinTransport(ITransportEvents& events)
        : m_events(events), m_alive(std::make_shared<bool>(true))
    {
        m_sock.m_owner = this;
        registerWindowClass();
        // Message-only window: receives the DNS completion and the deferred
        // immediate-connect notification.  Serviced by the app's ordinary
        // message loop — main thread, never re-entrant.
        m_hWnd = ::CreateWindowExA(0, kWndClass, "", 0, 0, 0, 0, 0,
                                   HWND_MESSAGE, NULL,
                                   AfxGetInstanceHandle(), NULL);
        if (m_hWnd)
            ::SetWindowLongPtrA(m_hWnd, GWLP_USERDATA, (LONG_PTR)this);
    }

    ~WinTransport() override
    {
        *m_alive = false;   // guard event-handler continuations (see handleReadable)
        cancelResolve();
        teardownTls();
        m_sock.Close();
        if (m_hWnd) {
            ::DestroyWindow(m_hWnd);   // no further messages can arrive
            m_hWnd = NULL;
        }
    }

    // ── DNS ────────────────────────────────────────────────────────────────
    // WSAAsyncGetHostByName — the exact LookupHostName mechanics (gethostbyname
    // resolution semantics, MAXGETHOSTSTRUCT buffer), delivered to this
    // adapter's window instead of the main frame.
    void resolve(const char* host, ResolveToken token) override
    {
        cancelResolve();
        m_resolveToken = token;
        m_lastResolveError = 0;
        if (!m_hWnd) {
            // out-of-resources only (window creation failed): without a
            // window neither the completion nor a deferred failure can be
            // delivered — report synchronously rather than losing it
            m_lastResolveError = ::WSAGetLastError();
            m_events.onResolved(token, 0, false);
            return;
        }
        m_hLookup = ::WSAAsyncGetHostByName(m_hWnd, WM_WT_RESOLVED,
                                            host ? host : "",
                                            m_hostBuf, sizeof m_hostBuf);
        if (!m_hLookup) {
            // could not even initiate — report failure, but deferred so the
            // engine never sees a re-entrant onResolved.  This is the path
            // upstream answered with its own message box; keeping the code
            // means the ordinary "Unable to resolve host name" line says
            // exactly as much as that box did.
            m_lastResolveError = ::WSAGetLastError();
            ::PostMessage(m_hWnd, WM_WT_RESOLVE_FAILED, 0, 0);
        }
    }

    void cancelResolve() override
    {
        if (m_hLookup) {
            ::WSACancelAsyncRequest(m_hLookup);   // == OnConnectionDisconnect's cancel
            m_hLookup = NULL;
        }
    }

    // ── connect ────────────────────────────────────────────────────────────
    void connect(unsigned int addrIPv4, unsigned short port,
                 const TransportOptions& options) override
    {
        SOCKADDR_IN sa;
        memset(&sa, 0, sizeof sa);
        sa.sin_family      = AF_INET;
        sa.sin_addr.s_addr = addrIPv4;
        sa.sin_port        = htons(port);

        // the exact CWorldSocket::Create call (ConnectSocket)
        if (!m_sock.Create(0, SOCK_STREAM,
                           FD_READ | FD_WRITE | FD_CONNECT | FD_CLOSE, NULL)) {
            m_events.onConnect(m_sock.GetLastError());   // immediate local failure
            return;
        }

        if (options.keepalives) {
            // see: http://www.gammon.com.au/forum/?id=9848  (ConnectSocket)
            DWORD dwBytesReturned;
            tcp_keepalive keepaliveData = {1, 60 * 1000 * 2, 6000};  // every 2 minutes
            WSAIoctl(m_sock.m_hSocket, SIO_KEEPALIVE_VALS, &keepaliveData,
                     sizeof(keepaliveData), NULL, 0, &dwBytesReturned,
                     NULL, NULL);
        }

        m_sock.AsyncSelect();

        if (m_sock.Connect((SOCKADDR*)&sa, sizeof sa)) {
            // Immediate success: defer delivery via the message window — the
            // engine never sees a re-entrant onConnect.  (The old engine
            // called OnConnect(0) inline from InitiateConnection; deferral is
            // the contract's non-re-entrancy rule, live-verified on POSIX.
            // Windows non-blocking connect never actually completes inline;
            // the no-window fallback matches the old inline call.)
            if (m_hWnd)
                ::PostMessage(m_hWnd, WM_WT_CONNECTED, 0, 0);
            else
                m_events.onConnect(0);
            return;
        }
        if (m_sock.GetLastError() == WSAEWOULDBLOCK)
            return;                    // in progress — FD_CONNECT delivers OnConnect
        m_events.onConnect(m_sock.GetLastError());   // immediate local failure
    }

    // ── send (append-and-it-drains) ────────────────────────────────────────
    void send(const char* data, size_t n) override
    {
        m_outgoing.append(data, n);
        // Mirror SendPacket's OnSend(0) kick "in case FD_WRITE message got
        // lost" — but never write application data mid-handshake (the old
        // code buffered through the handshake the same way).
        if (!m_tlsHandshaking)
            flushSend();
    }

    // ── TLS ────────────────────────────────────────────────────────────────
    void startTls(const TlsParams& params) override
    {
        m_tlsParams = params;

        m_ctx = SSL_CTX_new(TLS_client_method());
        if (!m_ctx) {
            m_events.onTlsFailed(TlsFailure::Handshake,
                                 "unable to create SSL context");
            return;
        }
        // set minimum TLS version to 1.2  (StartSSLHandshake)
        SSL_CTX_set_min_proto_version(m_ctx, TLS1_2_VERSION);
        SSL_CTX_set_verify(m_ctx, SSL_VERIFY_PEER,
                           (SSL_verify_cb)ssl_verify_callback);

        // load Windows root certificate store into OpenSSL  (StartSSLHandshake)
        HCERTSTORE hStore = CertOpenSystemStore(0, "ROOT");
        if (hStore) {
            X509_STORE* store = SSL_CTX_get_cert_store(m_ctx);
            PCCERT_CONTEXT pCertCtx = NULL;
            while ((pCertCtx = CertEnumCertificatesInStore(hStore, pCertCtx)) != NULL) {
                const unsigned char* pDer = pCertCtx->pbCertEncoded;
                X509* x509 = d2i_X509(NULL, &pDer, pCertCtx->cbCertEncoded);
                if (x509) {
                    X509_STORE_add_cert(store, x509);
                    X509_free(x509);
                }
            }
            CertCloseStore(hStore, 0);
        }

        m_ssl = SSL_new(m_ctx);
        if (!m_ssl) {
            teardownTls();
            m_events.onTlsFailed(TlsFailure::Handshake,
                                 "unable to create SSL object");
            return;
        }
        SSL_set_fd(m_ssl, m_sock.m_hSocket);
        if (!m_tlsParams.serverName.empty()) {
            // SNI + CN/SAN pinning — without SSL_set1_host any valid chain
            // for ANY host passes SSL_get_verify_result (StartSSLHandshake)
            SSL_set_tlsext_host_name(m_ssl, m_tlsParams.serverName.c_str());
            SSL_set1_host(m_ssl, m_tlsParams.serverName.c_str());
        }
        SSL_set_connect_state(m_ssl);
        m_tlsHandshaking = true;
        stepTls();
    }

    // ── teardown ───────────────────────────────────────────────────────────
    void close() override
    {
        teardownTls();
        // ShutDownSocket semantics: stop events, shut down both directions,
        // close.  No onClosed for a locally requested close.  The whole
        // sequence is guarded — real MFC's AsyncSelect ASSERTs on an invalid
        // handle, and close() on a never-connected transport is routine
        // (Disconnect during a DNS lookup; teardown after a failed lookup).
        if (m_sock.m_hSocket != INVALID_SOCKET) {
            m_sock.AsyncSelect(0);
            m_sock.ShutDown(CAsyncSocket::both);
            m_sock.Close();
        }
        m_outgoing.clear();
    }

    std::string lastErrorString() const override
    {
        char buf[64];
        sprintf(buf, "winsock error %d", ::WSAGetLastError());
        return std::string(buf);
    }

    // WSAGETASYNCERROR from the completion, or WSAGetLastError from a failed
    // initiation — whichever ended the last resolve.  The engine appends it to
    // "Unable to resolve host name", which is what upstream printed.
    int lastResolveError() const override { return m_lastResolveError; }

private:
    // ── the event-facing socket ────────────────────────────────────────────
    // An MFC CAsyncSocket: WSAAsyncSelect posts to MFC's socket window, which
    // dispatches these on the main thread — the identical delivery CWorldSocket
    // had.  Each override forwards and touches NOTHING afterward: the handler
    // may destroy the transport (and with it this socket) mid-call — the same
    // discipline CWorldSocket::OnReceive documented ("save m_pDoc locally —
    // 'this' gets deleted inside ReceiveMsg").
    struct EventSocket : CAsyncSocket {
        WinTransport* m_owner = NULL;

        virtual void OnConnect(int nErrorCode)
        {
            if (m_owner) m_owner->handleConnect(nErrorCode);
        }
        virtual void OnSend(int nErrorCode)
        {
            if (nErrorCode)    // had an error, give up!
                return;
            if (m_owner) m_owner->handleWritable();
        }
        virtual void OnReceive(int /*nErrorCode*/)
        {
            if (m_owner) m_owner->handleReadable();
        }
        virtual void OnClose(int nErrorCode)
        {
            if (m_owner) m_owner->handleClosed(nErrorCode);
        }
    };

    // ── message-only window (DNS completion + deferred connect) ───────────
    static void registerWindowClass()
    {
        static bool s_registered = false;
        if (s_registered)
            return;
        WNDCLASSA wc;
        memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc   = WndProc;
        wc.hInstance     = AfxGetInstanceHandle();
        wc.lpszClassName = kWndClass;
        ::RegisterClassA(&wc);
        s_registered = true;
    }

    static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam,
                                    LPARAM lParam)
    {
        WinTransport* self =
            (WinTransport*)::GetWindowLongPtrA(hWnd, GWLP_USERDATA);
        if (self) {
            switch (msg) {
                case WM_WT_RESOLVED:
                    // stale/cancelled completions carry a different handle
                    if ((HANDLE)wParam == self->m_hLookup) {
                        self->m_hLookup = NULL;
                        if (WSAGETASYNCERROR(lParam)) {
                            // keep the code: the engine appends it to
                            // "Unable to resolve host name", as upstream did
                            self->m_lastResolveError = WSAGETASYNCERROR(lParam);
                            self->m_events.onResolved(self->m_resolveToken,
                                                      0, false);
                        } else {
                            self->m_lastResolveError = 0;
                            // the exact HostNameResolved parse
                            struct hostent* pHostent =
                                (struct hostent*)self->m_hostBuf;
                            unsigned int addr =
                                ((LPIN_ADDR)pHostent->h_addr)->s_addr;
                            self->m_events.onResolved(self->m_resolveToken,
                                                      addr, true);
                        }
                    }
                    return 0;
                case WM_WT_RESOLVE_FAILED:
                    self->m_events.onResolved(self->m_resolveToken, 0, false);
                    return 0;
                case WM_WT_CONNECTED:
                    self->m_events.onConnect(0);
                    return 0;
            }
        }
        return ::DefWindowProcA(hWnd, msg, wParam, lParam);
    }

    // ── event plumbing ─────────────────────────────────────────────────────
    void handleConnect(int err) { m_events.onConnect(err); }

    void handleWritable()
    {
        if (m_tlsHandshaking) {
            stepTls();
            return;
        }
        if (m_outgoing.empty())
            return;
        std::shared_ptr<bool> alive = m_alive;
        flushSend();
        if (!*alive)
            return;
        if (m_outgoing.empty())
            m_events.onWritable();
    }

    void handleReadable()
    {
        if (m_tlsHandshaking) {
            stepTls();
            return;
        }
        // Sized at COMPRESS_BUFFER_LENGTH (doc.h == 10000): OnBytesReceived's
        // MCCP Z_STREAM_END path copies the post-stream residue (up to
        // avail_in, capped there at COMPRESS_BUFFER_LENGTH) back into this
        // buffer, so a smaller one overflows the stack on a crafted stream.
        // The literal, not the macro: a transport adapter does not include
        // doc.h, which is why platform/posix_transport.cpp spells it this way
        // too.
        char buff[10000];
        static_assert (sizeof buff >= 10000,
                       "receive buffer must hold the MCCP Z_STREAM_END residue");
        int count;
        std::shared_ptr<bool> alive = m_alive;

        if (m_tlsUp && m_ssl) {
            count = SSL_read(m_ssl, buff, (int)sizeof buff - 1);
            if (count <= 0) {
                int ssl_err = SSL_get_error(m_ssl, count);
                if (ssl_err == SSL_ERROR_WANT_READ ||
                    ssl_err == SSL_ERROR_WANT_WRITE)
                    return;
                // Clean TLS shutdown and hard errors both end the session
                // (matching ReceiveMsg's ZERO_RETURN / error paths).
                teardownTls();
                m_events.onClosed(0);
                return;
            }
            m_events.onBytes(buff, (size_t)count);
            if (!*alive)
                return;   // the handler destroyed the transport (contract)
            // Drain buffered decrypted data: no further FD_READ will fire for
            // bytes OpenSSL already holds (the worldsock OnReceive
            // SSL_pending loop, re-checking liveness every iteration).
            while (*alive && m_tlsUp && m_ssl && SSL_pending(m_ssl) > 0) {
                count = SSL_read(m_ssl, buff, (int)sizeof buff - 1);
                if (count <= 0)
                    break;
                m_events.onBytes(buff, (size_t)count);
            }
            return;
        }

        count = m_sock.Receive(buff, (int)sizeof buff - 1);
        if (count == SOCKET_ERROR) {
            m_events.onClosed(m_sock.GetLastError());
            return;
        }
        if (count <= 0)
            return;   // EOF arrives via FD_CLOSE → OnClose
        m_events.onBytes(buff, (size_t)count);
    }

    void handleClosed(int err)
    {
        teardownTls();
        m_events.onClosed(err);
    }

    // ── buffered send (CWorldSocket::OnSend semantics, verbatim) ──────────
    void flushSend()
    {
        if (m_outgoing.empty())
            return;
        if (m_tlsUp && m_ssl) {
            int count = SSL_write(m_ssl, m_outgoing.data(),
                                  (int)m_outgoing.length());
            if (count > 0) {
                m_outgoing.erase(0, (size_t)count);
            } else {
                int ssl_err = SSL_get_error(m_ssl, count);
                if (ssl_err != SSL_ERROR_WANT_WRITE &&
                    ssl_err != SSL_ERROR_WANT_READ) {
                    // Hard TLS write error: shut the socket down; FD_CLOSE
                    // delivers onClosed (the worldsock shape).
                    m_sock.AsyncSelect(FD_READ | FD_CLOSE);
                    m_sock.ShutDown(CAsyncSocket::both);
                    m_outgoing.clear();
                }
            }
            return;
        }
        int count = m_sock.Send(m_outgoing.data(), (int)m_outgoing.length());
        if (count > 0) {
            m_outgoing.erase(0, (size_t)count);
        } else if (count == SOCKET_ERROR &&
                   m_sock.GetLastError() != WSAEWOULDBLOCK) {
            m_sock.AsyncSelect(FD_READ | FD_CLOSE);
            m_sock.ShutDown(CAsyncSocket::both);
            m_outgoing.clear();
        }
    }

    // ── TLS session (ContinueSSLHandshake minus doc policy; == posix) ─────
    void stepTls()
    {
        int ret = SSL_do_handshake(m_ssl);
        if (ret == 1) {
            m_tlsHandshaking = false;

            long verify_result = SSL_get_verify_result(m_ssl);
            std::string verifyFailure;
            if (verify_result != X509_V_OK) {
                const char* reason =
                    X509_verify_cert_error_string(verify_result);
                std::string r = reason ? reason : "certificate not verified";
                if (!m_tlsParams.allowInvalidCert) {
                    teardownTls();
                    m_events.onTlsFailed(TlsFailure::CertVerify, r.c_str());
                    return;
                }
                verifyFailure = r;   // proceeding under user consent — report it
            }

            m_tlsUp = true;
            const char* version = SSL_get_version(m_ssl);
            const char* cipher  = SSL_get_cipher_name(m_ssl);
            std::shared_ptr<bool> alive = m_alive;
            m_events.onTlsEstablished(version ? version : "",
                                      cipher ? cipher : "",
                                      verifyFailure.c_str());
            if (!*alive)
                return;   // the handler destroyed the transport (contract)
            flushSend();   // release anything buffered during the handshake
            return;
        }

        int ssl_err = SSL_get_error(m_ssl, ret);
        if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE)
            return;   // stepped again from the next FD_READ / FD_WRITE

        unsigned long err_code = ERR_get_error();
        char err_buf[256];
        ERR_error_string_n(err_code, err_buf, sizeof err_buf);
        m_tlsHandshaking = false;
        teardownTls();
        m_events.onTlsFailed(TlsFailure::Handshake, err_buf);
    }

    void teardownTls()
    {
        // SSLCleanup semantics: orderly shutdown only for an established
        // session, then free both objects.
        if (m_ssl) {
            if (m_tlsUp)
                SSL_shutdown(m_ssl);
            SSL_free(m_ssl);
            m_ssl = NULL;
        }
        if (m_ctx) {
            SSL_CTX_free(m_ctx);
            m_ctx = NULL;
        }
        m_tlsUp = false;
        m_tlsHandshaking = false;
    }

    ITransportEvents& m_events;
    EventSocket       m_sock;
    std::string       m_outgoing;

    // Liveness sentinel: any event handler may destroy the transport
    // mid-call (the contract); continuations after an m_events call re-check
    // through a stack-held copy.  The dtor flips it.  (The POSIX adapter's
    // Gate, minus the resolve generation — DNS here is window-message based.)
    std::shared_ptr<bool> m_alive;

    HWND         m_hWnd         = NULL;
    HANDLE       m_hLookup      = NULL;   // WSAAsyncGetHostByName task handle
    ResolveToken m_resolveToken = 0;
    int          m_lastResolveError = 0;  // WSA code from the last failed resolve
    alignas(hostent) char m_hostBuf[MAXGETHOSTSTRUCT];   // hostent + data

    bool      m_tlsHandshaking = false;
    bool      m_tlsUp          = false;
    SSL_CTX*  m_ctx            = NULL;
    SSL*      m_ssl            = NULL;
    TlsParams m_tlsParams;
};


// ── factory ──────────────────────────────────────────────────────────────────

ITransport* CreateWinTransport(ITransportEvents& events)
{
    return new WinTransport(events);
}

} // namespace pal

#endif // _WIN32
