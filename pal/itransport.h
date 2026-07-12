// pal/itransport.h — the async byte-transport seam of the Platform Abstraction
// Layer (design: docs/PAL.md "## ITransport — design").
//
// One ITransport per world connection attempt. The adapter owns: the fd, the
// event source (select pump on POSIX, WSAAsyncSelect messages on Windows),
// DNS resolution, and the WHOLE TLS session (context, handshake stepping,
// verification, encrypted read/write, the decrypted-bytes drain). The engine
// keeps: the connect-phase machine, SOCKS proxy negotiation (cleartext bytes
// through send()/onBytes(), below TLS), MCCP, telnet, reconnect policy, and
// all disconnect UX.
//
// Contract rules (from the 2026-07 survey of the shipping stack):
//  - Events are delivered on the main thread and never re-entrantly.
//  - An event handler may destroy the transport mid-event; adapters must
//    tolerate that (defer destruction or generation-guard, as the POSIX
//    Poll() pump already does).
//  - NO dialogs inside events. The TLS cert-decision prompt stays an
//    engine-side deferred main-loop action that mutates policy
//    (allow-unverified / plaintext fallback) and reconnects from scratch;
//    hosts with no UI fail closed.
//
// Adapters: POSIX (relocated posix_socket.h pump + getaddrinfo thread +
// OpenSSL), Windows native (T5, by-construction parity), and a scripted fake
// for tests. Implementation lands with migration step T2; this header (T1)
// fixes the contract so the fake + characterization tests can pin the phase
// machine first.

#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace pal {

// Options applied at connect time. Keepalive timing is fixed engine policy
// (120 s idle, 6 s interval, 6 probes — the shipping values); the seam only
// carries on/off.
struct TransportOptions {
    bool keepalives = false;
};

// Parameters for starting TLS over the established (possibly proxy-tunneled)
// stream. serverName drives both SNI and hostname pinning (SSL_set1_host);
// allowInvalidCert is the session-only user consent flag.
struct TlsParams {
    std::string serverName;
    bool        allowInvalidCert = false;
};

enum class TlsFailure {
    CertVerify,   // handshake completed but the chain/hostname failed
    Handshake,    // the handshake itself failed
};

// A caller-chosen token echoed back with the resolve result so stale
// completions can be dropped (the engine passes its connect phase — the
// shipping staleness guard in LookupHostNameComplete).
using ResolveToken = int;

// Implemented by the engine (the doc). All callbacks arrive on the main
// thread, never re-entrantly.
class ITransportEvents {
public:
    virtual ~ITransportEvents() = default;

    // resolve() finished. ok=false → resolution failed. addrIPv4 is the
    // address in network byte order (the engine stores it into its
    // SOCKADDR_IN and may chain a proxy resolve before connecting).
    virtual void onResolved(ResolveToken token, unsigned int addrIPv4,
                            bool ok) = 0;

    // connect() finished: err == 0 → connected (including immediate
    // success), else a normalized error code.
    virtual void onConnect(int err) = 0;

    // Bytes arrived. When TLS is active these are DECRYPTED bytes and the
    // adapter drains its own buffered plaintext (the SSL_pending loop) —
    // the engine never re-reads.
    virtual void onBytes(const char* data, size_t n) = 0;

    // The buffered send queue drained enough to accept more data.
    virtual void onWritable() = 0;

    // Peer closed or a transport error ended the session. The engine runs
    // its disconnect bookkeeping (scripts, plugin callbacks, stats).
    virtual void onClosed(int err) = 0;

    // startTls() outcome. On failure the adapter has already torn the TLS
    // session down; the engine decides what to do (prompt via its own
    // deferred main-loop path, or fail closed). On success, verifyFailure is
    // empty when the chain + hostname verified clean; when the session was
    // allowed to proceed under TlsParams::allowInvalidCert despite a failed
    // verification it carries the reason (the engine warns the user — the
    // shipping "proceeding with UNVERIFIED certificate" Note).
    virtual void onTlsEstablished(std::string_view version,
                                  std::string_view cipher,
                                  std::string_view verifyFailure) = 0;
    virtual void onTlsFailed(TlsFailure kind, std::string_view reason) = 0;
};

class ITransport {
public:
    virtual ~ITransport() = default;

    // Async DNS. At most one resolve in flight; cancelResolve() (or
    // destruction) drops it. The token is echoed in onResolved.
    virtual void resolve(std::string_view host, ResolveToken token) = 0;
    virtual void cancelResolve() = 0;

    // Async connect to an IPv4 address (network byte order) + port.
    // Reports via onConnect.
    virtual void connect(unsigned int addrIPv4, unsigned short port,
                         const TransportOptions& options) = 0;

    // Buffered write: append and it drains on writability (absorbs the
    // shipping m_outstanding_data semantics, including TLS WANT_* retry).
    virtual void send(const char* data, size_t n) = 0;

    // Begin TLS over the established stream. The adapter steps the
    // handshake itself on read/write readiness; the engine never sees
    // WANT_READ/WANT_WRITE. Reports via onTlsEstablished/onTlsFailed.
    virtual void startTls(const TlsParams& params) = 0;

    // Graceful teardown (shutdown both directions, then close). onClosed
    // is NOT fired for a locally requested close.
    virtual void close() = 0;

    // Human-readable form of the last error, for the engine's Notes.
    virtual std::string lastErrorString() const = 0;
};

} // namespace pal
