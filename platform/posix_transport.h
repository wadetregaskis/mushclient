// platform/posix_transport.h — the POSIX/mac adapter for pal::ITransport
// (design: docs/PAL.md "## ITransport — design"; contract: pal/itransport.h).
//
// One PosixTransport per world connection attempt. It owns the fd (via an
// internal CAsyncSocket ridden by the app's existing 20 ms Poll pump), the
// buffered outgoing queue (including the TLS WANT_* retry), the WHOLE TLS
// session (context, non-blocking handshake stepping, verification, encrypted
// read/write, the SSL_pending drain), and async DNS (a detached getaddrinfo
// thread whose completion is marshalled back to the main thread).
//
// Event-delivery notes beyond the base contract:
//  - PumpTransportCompletions() must be called periodically on the main
//    thread (alongside CAsyncSocket::Poll) — it delivers DNS completions.
//    It is deliberately toolkit-free so the in-app selftest can drive it.
//  - onConnect for an immediately-successful connect (loopback) is deferred
//    to the next Poll tick, never delivered from inside connect().  An
//    immediate LOCAL failure (no route etc.) IS reported synchronously from
//    connect(), matching the engine's old inline error handling.
//  - Any event handler may destroy the transport; delivery is gate-guarded.

#ifndef _WIN32
#pragma once

#include "pal/itransport.h"

namespace pal {

// Factory: the concrete type stays private to the adapter TU.
// The caller owns the returned object (delete it to tear down; deleting from
// inside one of its own event callbacks is safe).
ITransport* CreatePosixTransport(ITransportEvents& events);

// Deliver pending main-thread completions (async DNS results) for ALL live
// PosixTransport instances.  Call on the main thread, alongside
// CAsyncSocket::Poll().
void PumpTransportCompletions();

// Populate a client SSL_CTX's trust store.  Exposed (rather than kept inside
// startTls) so a test can assert it still finds anchors when OpenSSL's own
// compiled-in default path is absent — which is the situation on every Mac that
// does not have the BUILD machine's Homebrew openssl installed.  Returns the
// number of anchors loaded, so "we have a trust store" is checkable.
// ctx is a SSL_CTX*; typed as void* so this header does not drag in <openssl/ssl.h>.
int ConfigureTrustAnchors(void* ctx);

} // namespace pal

// Hidden conformance harness (--pal-transport-selftest): exercises the real
// adapter against in-process TCP and TLS servers.  Returns the failure count.
int RunPalTransportSelfTest();

#endif // !_WIN32
