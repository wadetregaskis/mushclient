// pal/win_transport.h — the native Windows adapter for pal::ITransport
// (design: docs/PAL.md "## ITransport — design"; contract: pal/itransport.h).
//
// One WinTransport per world connection attempt.  Relocation, not invention:
// it owns the socket (an internal MFC CAsyncSocket — the same WSAAsyncSelect
// message-driven delivery CWorldSocket used, so events arrive on the main
// thread and never re-entrantly), the buffered outgoing queue including the
// TLS WANT_* retry (CWorldSocket::OnSend), the WHOLE TLS session — context,
// Windows ROOT-store import, non-blocking handshake stepping, verification,
// encrypted read/write and the SSL_pending drain (the doc.cpp Windows
// StartSSLHandshake/ContinueSSLHandshake bodies + worldsock.cpp OnReceive) —
// and async DNS via WSAAsyncGetHostByName delivered to an adapter-owned
// message-only window (the doc.cpp LookupHostName/HostNameResolved
// mechanics, minus the engine policy).
//
// Event-delivery notes beyond the base contract:
//  - No pump call is needed: WSAAsyncSelect / WSAAsyncGetHostByName post to
//    windows serviced by the app's ordinary message loop.
//  - onConnect for an immediately-successful connect is deferred via a
//    self-posted message, never delivered from inside connect().  An
//    immediate LOCAL failure IS reported synchronously from connect(),
//    matching the engine's old inline error handling.
//  - Any event handler may destroy the transport; the internal socket's
//    forwarding callbacks touch nothing after the forward (the CWorldSocket
//    discipline), and destruction cancels the DNS lookup and destroys the
//    message window before anything else.
//
// CANNOT be compiled or run in the macOS port environment: correctness is
// by-construction/reasoning (each mechanic is the exact original call
// sequence), the same discipline as pal/gdi_surface.cpp.  Windows CI is the
// eventual empirical arbiter.

#ifdef _WIN32
#pragma once

#include "pal/itransport.h"

namespace pal {

// Factory: the concrete type stays private to the adapter TU.
// The caller owns the returned object (delete it to tear down; deleting from
// inside one of its own event callbacks is safe).
ITransport* CreateWinTransport(ITransportEvents& events);

} // namespace pal

#endif // _WIN32
