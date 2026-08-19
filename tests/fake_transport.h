// tests/fake_transport.h — the recording pal::ITransport test double.
// Extracted (batch F31, cluster C35) from the nine near-identical copies that
// had accreted across test_display.cpp, test_aliases.cpp, test_hyperlink.cpp,
// test_lua_scripting.cpp, test_mxp.cpp (x2), test_telnet.cpp (x2),
// test_plugins.cpp and test_pal_transport.cpp — the precedent is
// tests/fake_worldview.h, which did the same thing for pal::IWorldView.
//
// Most consuming files only ever wanted "a transport that records what the
// doc sends and otherwise does nothing" — RecordingTransport is exactly that.
// The two outliers that track connect/resolve/TLS detail in more depth
// (test_connection.cpp's RecTransport, formerly test_pal_transport.cpp's
// FakeTransport) either derive from this or are now a plain alias of it —
// see each file for which.

#pragma once

#include "pal/itransport.h"
#include <string>

struct RecordingTransport : pal::ITransport {
    std::string sent;
    bool closed = false;
    bool tlsStarted = false;
    pal::ResolveToken lastToken = -1;

    // When true, send() drops bytes instead of recording them — the
    // NullTransport shape some callers want (a live transport that goes
    // nowhere, e.g. to prove a plugin veto stops the doc before anything
    // reaches the wire, without caring what would have been sent).
    bool dropSends = false;

    explicit RecordingTransport(bool dropSends_ = false) : dropSends(dropSends_) {}

    void resolve(const char*, pal::ResolveToken token) override { lastToken = token; }
    void cancelResolve() override { lastToken = -1; }
    void connect(unsigned int, unsigned short, const pal::TransportOptions&) override {}
    void send(const char* data, size_t n) override {
        if (!dropSends) sent.append(data, n);
    }
    void startTls(const pal::TlsParams&) override { tlsStarted = true; }
    void close() override { closed = true; }
    std::string lastErrorString() const override { return "fake"; }
};
