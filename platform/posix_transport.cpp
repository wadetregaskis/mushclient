// platform/posix_transport.cpp — the POSIX/mac pal::ITransport adapter.
//
// Relocation, not invention: the fd/pump mechanics come from
// platform/posix_socket.h (the adapter rides the SAME 20 ms Poll pump via an
// internal CAsyncSocket), the buffered-send semantics from
// CWorldSocket::OnSend (worldsock.cpp), the TLS session from
// CMUSHclientDoc::StartSSLHandshake/ContinueSSLHandshake (doc.cpp) minus the
// doc-side policy (Notes/prompts stay in the engine — the adapter only
// REPORTS via onTlsEstablished/onTlsFailed), and async DNS from the
// getaddrinfo thread CWorldChildFrame used to install as m_fn_lookup_host —
// here marshalled through a toolkit-free completion mailbox drained by
// PumpTransportCompletions() so the selftest can drive it without a wx loop.
//
// See docs/PAL.md "## ITransport — design" and pal/itransport.h for the
// contract (main-thread, non-re-entrant delivery; handlers may destroy the
// transport; no dialogs inside events).

#ifndef _WIN32

#include "platform/posix_socket.h"
#include "platform/posix_transport.h"

#ifdef OPENSSL_FOUND
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#endif

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace pal {

class PosixTransport;   // defined below; the mailbox stores pointers to it

// ── main-thread completion mailbox (async DNS results) ──────────────────────
// The resolver thread pushes here; PumpTransportCompletions() drains on the
// main thread.  Each entry carries a liveness gate so a completion for a
// destroyed (or cancelled) transport is dropped, never delivered.

namespace {

struct Gate {
    std::atomic<bool> alive { true };
    std::atomic<unsigned> resolveGen { 0 };
};

struct ResolveCompletion {
    std::shared_ptr<Gate> gate;
    // Dereferenced ONLY on the main thread and ONLY while gate->alive — the
    // transport is destroyed on the main thread too (its dtor clears alive
    // first), so a true gate means the pointer is valid.
    PosixTransport* transport = nullptr;
    unsigned     gen = 0;
    ResolveToken token = 0;
    unsigned int addr = 0;     // network byte order
    bool         ok = false;
    // getaddrinfo's return code, carried across the thread boundary rather
    // than written to the transport from the resolver thread — the member it
    // ends up in is read on the main thread by lastResolveError(), and the
    // gate only makes the POINTER safe there, not a concurrent write.
    int          gaiError = 0;
};

// Deliberately IMMORTAL (leaked once, never destroyed).  The resolver thread is
// detached and nothing waits for it, so a lookup still in flight at quit --
// getaddrinfo can block for tens of seconds on bad DNS or a dropped VPN -- would
// otherwise wake after static destruction, lock a destroyed mutex and push_back
// into a freed vector.  The per-transport Gate protects the transport, not these
// two objects.  An object that is never destroyed cannot be used after it is.
struct Mailbox {
    std::mutex lock;
    std::vector<ResolveCompletion> items;
};
Mailbox& MB()
{
    static Mailbox* m = new Mailbox;   // never deleted, on purpose
    return *m;
}

} // namespace


// ── PosixTransport ───────────────────────────────────────────────────────────

class PosixTransport : public ITransport {
public:
    explicit PosixTransport(ITransportEvents& events)
        : m_events(events), m_gate(std::make_shared<Gate>())
    {
        m_sock.m_owner = this;
    }

    ~PosixTransport() override
    {
        m_gate->alive = false;   // drop in-flight DNS + guard event loops
#ifdef OPENSSL_FOUND
        teardownTls();
#endif
        m_sock.Close();
    }

    // ── DNS ────────────────────────────────────────────────────────────────
    void resolve(const char* host, ResolveToken token) override
    {
        const unsigned gen = ++m_gate->resolveGen;   // invalidates any prior lookup
        std::shared_ptr<Gate> gate = m_gate;
        PosixTransport* self = this;
        std::string hostCopy(host ? host : "");
        std::thread([gate, self, gen, token, hostCopy]() {
            struct addrinfo hints {};
            struct addrinfo* res = nullptr;
            hints.ai_family   = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            const int gai = ::getaddrinfo(hostCopy.c_str(), nullptr, &hints, &res);
            bool ok = (gai == 0 && res != nullptr);
            unsigned int addr = 0;
            if (ok) {
                addr = reinterpret_cast<struct sockaddr_in*>(res->ai_addr)
                           ->sin_addr.s_addr;
                ::freeaddrinfo(res);
            }
            else if (res != nullptr)
                ::freeaddrinfo(res);   // gai == 0 but empty list: still ours to free
            std::lock_guard<std::mutex> lk(MB().lock);
            MB().items.push_back({ gate, self, gen, token, addr, ok, gai });
        }).detach();
    }

    void cancelResolve() override { ++m_gate->resolveGen; }

    // ── connect ────────────────────────────────────────────────────────────
    void connect(unsigned int addrIPv4, unsigned short port,
                 const TransportOptions& options) override
    {
        SOCKADDR_IN sa {};
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = addrIPv4;
        sa.sin_port = htons(port);

        if (!m_sock.Create(0, SOCK_STREAM,
                           FD_READ | FD_WRITE | FD_CONNECT | FD_CLOSE, nullptr)) {
            m_events.onConnect(errno ? errno : -1);   // immediate local failure
            return;
        }

        if (options.keepalives) {
            // The shipping keepalive policy (ConnectSocket's POSIX branch):
            // idle 120 s, probe interval 6 s, 6 probes.
            int on = 1;
            ::setsockopt(m_sock.m_hSocket, SOL_SOCKET, SO_KEEPALIVE,
                         &on, sizeof on);
#ifdef TCP_KEEPALIVE      // macOS spelling of the idle time
            int idle = 120;
            ::setsockopt(m_sock.m_hSocket, IPPROTO_TCP, TCP_KEEPALIVE,
                         &idle, sizeof idle);
#endif
#ifdef TCP_KEEPIDLE       // Linux spelling
            int idleL = 120;
            ::setsockopt(m_sock.m_hSocket, IPPROTO_TCP, TCP_KEEPIDLE,
                         &idleL, sizeof idleL);
#endif
#ifdef TCP_KEEPINTVL
            int intvl = 6;
            ::setsockopt(m_sock.m_hSocket, IPPROTO_TCP, TCP_KEEPINTVL,
                         &intvl, sizeof intvl);
#endif
#ifdef TCP_KEEPCNT
            int cnt = 6;
            ::setsockopt(m_sock.m_hSocket, IPPROTO_TCP, TCP_KEEPCNT,
                         &cnt, sizeof cnt);
#endif
        }

        m_sock.AsyncSelect();

        if (m_sock.Connect(reinterpret_cast<SOCKADDR*>(&sa), sizeof sa)) {
            // Immediate success (loopback): defer delivery to the next Poll
            // tick — the socket is writable, so OnSend fires promptly and the
            // engine never sees a re-entrant onConnect.
            m_sock.m_awaitingConnect = true;
            return;
        }
        if (CAsyncSocket::GetLastError() == WSAEWOULDBLOCK)
            return;                       // in progress — Poll delivers OnConnect
        m_events.onConnect(errno ? errno : -1);   // immediate local failure
    }

    // ── send (append-and-it-drains) ────────────────────────────────────────
    void send(const char* data, size_t n) override
    {
        m_outgoing.append(data, n);
        // Mirror SendPacket's OnSend(0) kick "in case FD_WRITE got lost" —
        // but never write application data mid-handshake (the old code
        // buffered through the handshake the same way).
        if (!m_tlsHandshaking)
            flushSend();
    }

    // ── TLS ────────────────────────────────────────────────────────────────
    void startTls(const TlsParams& params) override
    {
#ifdef OPENSSL_FOUND
        m_tlsParams = params;

        m_ctx = SSL_CTX_new(TLS_client_method());
        if (!m_ctx) {
            m_events.onTlsFailed(TlsFailure::Handshake,
                                 "unable to create SSL context");
            return;
        }
        SSL_CTX_set_min_proto_version(m_ctx, TLS1_2_VERSION);
        // Verify-peer with an always-continue callback: the handshake always
        // completes; the REAL verdict is read post-handshake via
        // SSL_get_verify_result (the shipping scheme — it lets the engine
        // offer "connect anyway" instead of OpenSSL aborting mid-handshake).
        SSL_CTX_set_verify(m_ctx, SSL_VERIFY_PEER,
                           [](int, X509_STORE_CTX*) -> int { return 1; });
        ConfigureTrustAnchors(m_ctx);

        m_ssl = SSL_new(m_ctx);
        if (!m_ssl) {
            teardownTls();
            m_events.onTlsFailed(TlsFailure::Handshake,
                                 "unable to create SSL object");
            return;
        }
        SSL_set_fd(m_ssl, m_sock.m_hSocket);
        if (!m_tlsParams.serverName.empty()) {
            SSL_set_tlsext_host_name(m_ssl, m_tlsParams.serverName.c_str());
            // CN/SAN pinning — without it any valid chain for ANY host passes.
            SSL_set1_host(m_ssl, m_tlsParams.serverName.c_str());
        }
        SSL_set_connect_state(m_ssl);
        m_tlsHandshaking = true;
        stepTls();
#else
        (void)params;
        m_events.onTlsFailed(TlsFailure::Handshake, "built without OpenSSL");
#endif
    }

    // ── teardown ───────────────────────────────────────────────────────────
    void close() override
    {
#ifdef OPENSSL_FOUND
        teardownTls();
#endif
        // ShutDownSocket semantics: stop events, shut down both directions,
        // close.  No onClosed for a locally requested close.  Guarded as the
        // original ShutDownSocket was (and real MFC's AsyncSelect ASSERTs on
        // an invalid handle — keeps the Windows twin a 1:1 diff).
        if (m_sock.m_hSocket != INVALID_SOCKET) {
            m_sock.AsyncSelect(0);
            m_sock.ShutDown(CAsyncSocket::both);
            m_sock.Close();
        }
        m_outgoing.clear();
    }

    std::string lastErrorString() const override
    {
        return std::string(strerror(errno));
    }

    // getaddrinfo's code from the last failed resolve, with its OWN renderer.
    // EAI_* is not errno: GetSocketError's non-Windows arm is strerror, which
    // turns EAI_NONAME (8) into "Exec format error" — see pal/itransport.h.
    int lastResolveError() const override { return m_lastResolveError; }

    std::string lastResolveErrorString() const override
    {
        if (m_lastResolveError == 0)
            return std::string();
        const char* s = ::gai_strerror(m_lastResolveError);
        return s ? std::string(s) : std::string();
    }

private:
    // ── the pump-facing socket ─────────────────────────────────────────────
    // Rides the app's existing CAsyncSocket::Poll pump; forwards events into
    // the owning transport.
    struct PumpSocket : CAsyncSocket {
        PosixTransport* m_owner = nullptr;
        bool m_awaitingConnect = false;   // immediate-connect: deliver on next writable

        void OnConnect(int nErrorCode) override
        {
            // A connect that never completed must not go on to report a CLOSE.
            // On a refused connect select() marks the fd BOTH writable and
            // readable; getsockopt(SO_ERROR) consumes the error, so the peek in
            // the read arm that follows sees EOF and fires OnClose -- and the
            // doc then ran the whole disconnect sequence (disconnect script,
            // ON_PLUGIN_DISCONNECT, the session-statistics Notes, auto-log
            // close) for a session that never existed.  Winsock posts no
            // FD_CLOSE for a connect that never succeeded; closing here gives
            // the pump nothing left to report, and its post-callback
            // INVALID_SOCKET guard skips the rest of this pass.
            if (nErrorCode) Close();
            if (m_owner) m_owner->handleConnect(nErrorCode);
        }
        void OnSend(int) override
        {
            if (!m_owner) return;
            if (m_awaitingConnect) {
                m_awaitingConnect = false;
                m_owner->handleConnect(0);
                return;
            }
            m_owner->handleWritable();
        }
        void OnReceive(int) override
        {
            if (m_owner) m_owner->handleReadable();
        }
        void OnClose(int nErrorCode) override
        {
            if (m_owner) m_owner->handleClosed(nErrorCode);
        }
    };

    // ── event plumbing (each may destroy `this` — gate-guard after) ───────
    void handleConnect(int err) { m_events.onConnect(err); }

    void handleWritable()
    {
#ifdef OPENSSL_FOUND
        if (m_tlsHandshaking) { stepTls(); return; }
#else
        if (m_tlsHandshaking) return;
#endif
        if (m_outgoing.empty()) return;
        std::shared_ptr<Gate> gate = m_gate;
        flushSend();
        if (!gate->alive) return;
        if (m_outgoing.empty())
            m_events.onWritable();
    }

    void handleReadable()
    {
#ifdef OPENSSL_FOUND
        if (m_tlsHandshaking) { stepTls(); return; }
#else
        if (m_tlsHandshaking) return;
#endif
        char buff[9000];
        int count;
        std::shared_ptr<Gate> gate = m_gate;

#ifdef OPENSSL_FOUND
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
            if (!gate->alive) return;
            // Drain buffered decrypted data: no further read event will fire
            // for bytes OpenSSL already holds (the worldsock SSL_pending loop).
            while (gate->alive && m_tlsUp && m_ssl && SSL_pending(m_ssl) > 0) {
                count = SSL_read(m_ssl, buff, (int)sizeof buff - 1);
                if (count <= 0) break;
                m_events.onBytes(buff, (size_t)count);
            }
            return;
        }
#endif
        count = m_sock.Receive(buff, (int)sizeof buff - 1);
        if (count == SOCKET_ERROR) {
            m_events.onClosed(CAsyncSocket::GetLastError());
            return;
        }
        if (count <= 0) return;   // EOF arrives via OnClose (Poll's MSG_PEEK)
        m_events.onBytes(buff, (size_t)count);
    }

    void handleClosed(int err)
    {
#ifdef OPENSSL_FOUND
        teardownTls();
#endif
        m_events.onClosed(err);
    }

    // ── buffered send (CWorldSocket::OnSend semantics) ─────────────────────
    void flushSend()
    {
        if (m_outgoing.empty()) return;
#ifdef OPENSSL_FOUND
        if (m_tlsUp && m_ssl) {
            int count = SSL_write(m_ssl, m_outgoing.data(),
                                  (int)m_outgoing.length());
            if (count > 0) {
                m_outgoing.erase(0, (size_t)count);
            } else {
                int ssl_err = SSL_get_error(m_ssl, count);
                if (ssl_err != SSL_ERROR_WANT_WRITE &&
                    ssl_err != SSL_ERROR_WANT_READ) {
                    // Hard TLS write error: shut the socket down; the pump's
                    // EOF detection delivers onClosed (the worldsock shape).
                    m_sock.AsyncSelect(FD_READ | FD_CLOSE);
                    m_sock.ShutDown(CAsyncSocket::both);
                    m_outgoing.clear();
                }
            }
            return;
        }
#endif
        int count = m_sock.Send(m_outgoing.data(), (int)m_outgoing.length());
        if (count > 0) {
            m_outgoing.erase(0, (size_t)count);
        } else if (count == SOCKET_ERROR &&
                   CAsyncSocket::GetLastError() != WSAEWOULDBLOCK) {
            m_sock.AsyncSelect(FD_READ | FD_CLOSE);
            m_sock.ShutDown(CAsyncSocket::both);
            m_outgoing.clear();
        }
    }

#ifdef OPENSSL_FOUND
    // ── TLS session (ContinueSSLHandshake minus doc policy) ────────────────
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
            std::shared_ptr<Gate> gate = m_gate;
            m_events.onTlsEstablished(version ? version : "",
                                      cipher ? cipher : "",
                                      verifyFailure.c_str());
            if (!gate->alive) return;
            flushSend();   // release anything buffered during the handshake
            return;
        }

        int ssl_err = SSL_get_error(m_ssl, ret);
        if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE)
            return;   // stepped again from the next readable/writable event

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
            if (m_tlsUp) SSL_shutdown(m_ssl);
            SSL_free(m_ssl);
            m_ssl = nullptr;
        }
        if (m_ctx) {
            SSL_CTX_free(m_ctx);
            m_ctx = nullptr;
        }
        m_tlsUp = false;
        m_tlsHandshaking = false;
    }
#endif

public:
    // Called only by PumpTransportCompletions (main thread, gate-checked).
    void deliverResolve(ResolveToken token, unsigned int addr, bool ok,
                        int gaiError)
    {
        m_lastResolveError = ok ? 0 : gaiError;
        m_events.onResolved(token, addr, ok);
    }

private:
    ITransportEvents&     m_events;
    std::shared_ptr<Gate> m_gate;
    PumpSocket            m_sock;
    std::string           m_outgoing;
    // Written only in deliverResolve, i.e. on the main thread from the pump —
    // never by the resolver thread, which reports its code through the mailbox.
    int                   m_lastResolveError = 0;

    bool m_tlsHandshaking = false;
    bool m_tlsUp = false;
#ifdef OPENSSL_FOUND
    SSL_CTX*  m_ctx = nullptr;
    SSL*      m_ssl = nullptr;
    TlsParams m_tlsParams;
#endif
};

// ── factory + completion pump ────────────────────────────────────────────────

// ── ConfigureTrustAnchors ─────────────────────────────────────────────────
// Fill a client SSL_CTX's trust store, and say how many anchors ended up in it.
//
// SSL_CTX_set_default_verify_paths() alone is NOT enough on macOS.  It looks in
// the OPENSSLDIR compiled into whichever libcrypto we linked, which for a
// Homebrew build is /opt/homebrew/etc/openssl@3 — a path that exists on the
// BUILD machine and on essentially no user's machine.  Measured with the
// linked library: with that directory present the store gets 184 anchors; with
// it absent it gets ZERO.  Every TLS world would then fail verification and
// raise the "connect anyway?" prompt on every single connection, which is worse
// than useless — it teaches the user to click through certificate warnings.
//
// macOS ships Apple's maintained root bundle at /etc/ssl/cert.pem (128 roots).
// Load it too.  The call is additive and harmless where the defaults already
// worked, and a no-op if the file is missing, so this stays correct on Linux,
// where set_default_verify_paths finds the distribution's store by itself.
int ConfigureTrustAnchors(void* vctx)
{
#ifdef OPENSSL_FOUND
    SSL_CTX* ctx = static_cast<SSL_CTX*>(vctx);
    if (!ctx) return 0;

    SSL_CTX_set_default_verify_paths(ctx);

#ifdef __APPLE__
    // Ignore the result: a failure here just means the file was not there, and
    // the defaults above may already have supplied a usable store.
    SSL_CTX_load_verify_locations(ctx, "/etc/ssl/cert.pem", nullptr);
#endif

    X509_STORE* store = SSL_CTX_get_cert_store(ctx);
    if (!store) return 0;
    STACK_OF(X509_OBJECT)* objs = X509_STORE_get0_objects(store);
    return objs ? sk_X509_OBJECT_num(objs) : 0;
#else
    (void) vctx;
    return 0;
#endif
}

ITransport* CreatePosixTransport(ITransportEvents& events)
{
    return new PosixTransport(events);
}

void PumpTransportCompletions()
{
    std::vector<ResolveCompletion> pending;
    {
        std::lock_guard<std::mutex> lk(MB().lock);
        pending.swap(MB().items);
    }
    for (const ResolveCompletion& rc : pending) {
        // Deliver only if the transport is alive AND this is still the
        // current lookup (cancelResolve / a newer resolve bumps the gen).
        // The dtor clears alive on the main thread before the object dies,
        // so a true gate here means rc.transport is valid.
        if (rc.gate->alive && rc.gate->resolveGen == rc.gen)
            rc.transport->deliverResolve(rc.token, rc.addr, rc.ok, rc.gaiError);
    }
}

} // namespace pal

// ═════════════════════════════════════════════════════════════════════════════
// Conformance selftest (hidden --pal-transport-selftest flag)
//
// Exercises the REAL adapter against in-process servers:
//   TCP — resolve, connect (immediate/loopback), send, onBytes, peer close.
//   TLS — a self-signed in-process TLS server: allowInvalidCert=true must
//         establish (the "connect anyway" path) and deliver decrypted bytes;
//         allowInvalidCert=false must refuse with TlsFailure::CertVerify.
//   Safety — destroying the transport from inside its own onBytes handler.
//
// The pump is driven directly (CAsyncSocket::Poll + PumpTransportCompletions)
// — no event loop required, so this runs before the GUI boots and exits.
// ═════════════════════════════════════════════════════════════════════════════

#include <cstdio>

namespace {

int g_ttPass = 0, g_ttFail = 0;
void ttCheck(bool ok, const char* what)
{
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    (ok ? g_ttPass : g_ttFail)++;
}

// Recorder for the events side of the contract.
struct RecordingEvents : pal::ITransportEvents {
    int  connects = 0, closes = 0, writables = 0;
    int  lastConnectErr = -999, lastCloseErr = -999;
    bool resolved = false, resolveOk = false;
    pal::ResolveToken resolveToken = -1;
    unsigned int resolveAddr = 0;
    std::string bytes;
    bool tlsEstablished = false, tlsFailed = false;
    pal::TlsFailure tlsFailKind = pal::TlsFailure::Handshake;
    std::string tlsVersion, tlsFailReason, tlsVerifyFailure;
    pal::ITransport* deleteOnBytes = nullptr;   // destroy-mid-event check

    void onResolved(pal::ResolveToken token, unsigned int addr, bool ok) override
    {
        resolved = true; resolveOk = ok; resolveToken = token; resolveAddr = addr;
    }
    void onConnect(int err) override { ++connects; lastConnectErr = err; }
    void onBytes(const char* data, size_t n) override
    {
        bytes.append(data, n);
        if (deleteOnBytes) {
            pal::ITransport* t = deleteOnBytes;
            deleteOnBytes = nullptr;
            delete t;                         // the contract allows this
        }
    }
    void onWritable() override { ++writables; }
    void onClosed(int err) override { ++closes; lastCloseErr = err; }
    void onTlsEstablished(const char* version, const char*,
                          const char* verifyFailure) override
    {
        tlsEstablished = true; tlsVersion.assign(version);
        tlsVerifyFailure.assign(verifyFailure);
    }
    void onTlsFailed(pal::TlsFailure kind, const char* reason) override
    {
        tlsFailed = true; tlsFailKind = kind; tlsFailReason.assign(reason);
    }
};

// Drive the pump until pred() or ~timeout. Returns pred()'s final value.
template <typename Pred>
bool pumpUntil(Pred pred, int maxTicks = 500)
{
    for (int i = 0; i < maxTicks; ++i) {
        if (pred()) return true;
        CAsyncSocket::Poll(10);
        pal::PumpTransportCompletions();
        // Poll returns immediately when no sockets are registered (the DNS
        // case) — sleep so the loop represents real elapsed time.
        ::usleep(2000);
    }
    return pred();
}

// A plain in-process TCP server: accept one client, send greeting, echo the
// first packet back prefixed, then close.  Returns the bound port.
unsigned short StartTcpServer(std::thread& out)
{
    int srv = ::socket(AF_INET, SOCK_STREAM, 0);
    SOCKADDR_IN sa {};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = 0;
    ::bind(srv, reinterpret_cast<SOCKADDR*>(&sa), sizeof sa);
    socklen_t len = sizeof sa;
    ::getsockname(srv, reinterpret_cast<SOCKADDR*>(&sa), &len);
    ::listen(srv, 1);
    out = std::thread([srv]() {
        int c = ::accept(srv, nullptr, nullptr);
        if (c >= 0) {
            ::send(c, "HELLO\r\n", 7, 0);
            char buf[64];
            int n = (int)::recv(c, buf, sizeof buf, 0);
            if (n > 0) {
                std::string reply = "ECHO:" + std::string(buf, (size_t)n);
                ::send(c, reply.data(), reply.size(), 0);
            }
            ::shutdown(c, 2);
            ::close(c);
        }
        ::close(srv);
    });
    return ntohs(sa.sin_port);
}

#ifdef OPENSSL_FOUND
// A self-signed in-process TLS server: accept one client, complete the
// handshake, send a greeting, then close.  Returns the bound port.
unsigned short StartTlsServer(std::thread& out)
{
    int srv = ::socket(AF_INET, SOCK_STREAM, 0);
    SOCKADDR_IN sa {};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = 0;
    ::bind(srv, reinterpret_cast<SOCKADDR*>(&sa), sizeof sa);
    socklen_t len = sizeof sa;
    ::getsockname(srv, reinterpret_cast<SOCKADDR*>(&sa), &len);
    ::listen(srv, 1);
    out = std::thread([srv]() {
        // Fresh throwaway identity per server.
        EVP_PKEY* pkey = EVP_RSA_gen(2048);
        X509* x = X509_new();
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

        int c = ::accept(srv, nullptr, nullptr);
        if (c >= 0) {
            SSL* ssl = SSL_new(ctx);
            SSL_set_fd(ssl, c);
            if (SSL_accept(ssl) == 1) {
                SSL_write(ssl, "TLSHELLO", 8);
                // linger briefly so the client can read before teardown
                char buf[16];
                SSL_read(ssl, buf, sizeof buf);   // returns on client close
                SSL_shutdown(ssl);
            }
            SSL_free(ssl);
            ::close(c);
        }
        SSL_CTX_free(ctx);
        X509_free(x);
        EVP_PKEY_free(pkey);
        ::close(srv);
    });
    return ntohs(sa.sin_port);
}
#endif // OPENSSL_FOUND

} // namespace

int RunPalTransportSelfTest()
{
    printf("PAL ITransport conformance selftest (POSIX adapter)\n");

    const unsigned int kLoopback = htonl(INADDR_LOOPBACK);

    // ── 1. async DNS ────────────────────────────────────────────────────────
    {
        RecordingEvents ev;
        pal::ITransport* t = pal::CreatePosixTransport(ev);
        t->resolve("localhost", 42);
        pumpUntil([&] { return ev.resolved; });
        ttCheck(ev.resolved && ev.resolveOk && ev.resolveToken == 42 &&
                ev.resolveAddr == kLoopback,
                "resolve(localhost) completes with 127.0.0.1 + token echo");

        // A successful resolve leaves NO error behind, so the message keeps its
        // short form after a good lookup.
        ttCheck(t->lastResolveError() == 0 &&
                t->lastResolveErrorString().empty(),
                "a successful resolve reports no error code");

        // A cancelled lookup must never deliver.
        RecordingEvents ev2;
        pal::ITransport* t2 = pal::CreatePosixTransport(ev2);
        t2->resolve("localhost", 7);
        t2->cancelResolve();
        pumpUntil([&] { return false; }, 30);   // give the thread time anyway
        ttCheck(!ev2.resolved, "cancelResolve drops the completion");
        delete t2;
        delete t;

        // ── a FAILED resolve reports getaddrinfo's code, rendered by
        // getaddrinfo's own strings.  `.invalid` is reserved by RFC 2606
        // precisely so it can never resolve, so this needs no network.
        //
        // The second assertion is the one that matters: EAI_* overlaps errno
        // numerically, so a code alone proves nothing about which table was
        // consulted.  strerror(EAI_NONAME==8) is "Exec format error" on macOS —
        // if that (or any other strerror text) came back, the message would be
        // confidently wrong rather than merely terse, which is the whole reason
        // lastResolveErrorString exists.
        RecordingEvents ev3;
        pal::ITransport* t3 = pal::CreatePosixTransport(ev3);
        t3->resolve("no-such-host.invalid", 99);
        pumpUntil([&] { return ev3.resolved; }, 600);
        ttCheck(ev3.resolved && !ev3.resolveOk,
                "resolve of a reserved .invalid name fails");
        ttCheck(t3->lastResolveError() != 0,
                "...and leaves a non-zero getaddrinfo code");
        {
            const std::string why = t3->lastResolveErrorString();
            const char* gai = ::gai_strerror(t3->lastResolveError());
            ttCheck(!why.empty() && gai && why == gai,
                    "...described by gai_strerror, not strerror");
            const char* wrong = strerror(t3->lastResolveError());
            ttCheck(!wrong || why != wrong,
                    "...and demonstrably NOT the errno text for the same number");
        }
        delete t3;
    }

    // ── 2. TCP round trip ───────────────────────────────────────────────────
    {
        std::thread server;
        unsigned short port = StartTcpServer(server);

        RecordingEvents ev;
        pal::ITransport* t = pal::CreatePosixTransport(ev);
        t->connect(kLoopback, port, pal::TransportOptions{ true });
        pumpUntil([&] { return ev.connects > 0; });
        ttCheck(ev.connects == 1 && ev.lastConnectErr == 0,
                "connect(loopback) delivers onConnect(0) exactly once");

        t->send("PING", 4);
        pumpUntil([&] { return ev.bytes.find("ECHO:PING") != std::string::npos; });
        ttCheck(ev.bytes.find("HELLO\r\n") != std::string::npos,
                "greeting bytes delivered via onBytes");
        ttCheck(ev.bytes.find("ECHO:PING") != std::string::npos,
                "sent bytes reach the server (echo returned)");

        pumpUntil([&] { return ev.closes > 0; });
        ttCheck(ev.closes == 1, "peer close delivers onClosed exactly once");

        delete t;
        server.join();
    }

    // ── 3. destroy-the-transport-inside-onBytes safety ──────────────────────
    {
        std::thread server;
        unsigned short port = StartTcpServer(server);

        RecordingEvents ev;
        pal::ITransport* t = pal::CreatePosixTransport(ev);
        ev.deleteOnBytes = t;                  // first onBytes deletes it
        t->connect(kLoopback, port, pal::TransportOptions{ false });
        pumpUntil([&] { return !ev.bytes.empty(); });
        // a few extra ticks: any use-after-free would crash here
        pumpUntil([&] { return false; }, 20);
        ttCheck(!ev.bytes.empty() && ev.closes == 0,
                "deleting the transport inside onBytes is safe (no crash, no stray events)");
        server.join();
    }

#ifdef OPENSSL_FOUND
    // ── 4. TLS: self-signed accepted when the user opted in ────────────────
    {
        std::thread server;
        unsigned short port = StartTlsServer(server);

        RecordingEvents ev;
        pal::ITransport* t = pal::CreatePosixTransport(ev);
        t->connect(kLoopback, port, pal::TransportOptions{ false });
        pumpUntil([&] { return ev.connects > 0; });

        pal::TlsParams params;
        params.serverName = "localhost";
        params.allowInvalidCert = true;        // the "connect anyway" consent
        t->startTls(params);
        pumpUntil([&] { return ev.tlsEstablished || ev.tlsFailed; });
        ttCheck(ev.tlsEstablished && !ev.tlsFailed,
                "startTls establishes with allowInvalidCert (self-signed accepted)");
        ttCheck(ev.tlsVersion.find("TLS") != std::string::npos,
                "onTlsEstablished reports a TLS version");
        ttCheck(!ev.tlsVerifyFailure.empty(),
                "consented self-signed session reports its verify failure");

        pumpUntil([&] { return ev.bytes.find("TLSHELLO") != std::string::npos; });
        ttCheck(ev.bytes.find("TLSHELLO") != std::string::npos,
                "decrypted bytes delivered via onBytes after TLS");

        delete t;                              // client close unblocks the server
        server.join();
    }

    // ── 5. TLS: self-signed REFUSED by default (fails closed) ──────────────
    {
        std::thread server;
        unsigned short port = StartTlsServer(server);

        RecordingEvents ev;
        pal::ITransport* t = pal::CreatePosixTransport(ev);
        t->connect(kLoopback, port, pal::TransportOptions{ false });
        pumpUntil([&] { return ev.connects > 0; });

        pal::TlsParams params;
        params.serverName = "localhost";
        params.allowInvalidCert = false;       // default policy
        t->startTls(params);
        pumpUntil([&] { return ev.tlsEstablished || ev.tlsFailed; });
        ttCheck(ev.tlsFailed && ev.tlsFailKind == pal::TlsFailure::CertVerify,
                "startTls refuses an unverifiable certificate (CertVerify)");
        ttCheck(!ev.tlsFailReason.empty(), "refusal carries a human-readable reason");

        delete t;
        server.join();
    }
#else
    printf("  [SKIP] TLS cases (built without OpenSSL)\n");
#endif

    printf("PAL-TRANSPORT-SELFTEST: %d passed, %d failed\n", g_ttPass, g_ttFail);
    return g_ttFail;
}

#endif // !_WIN32 (whole file)
