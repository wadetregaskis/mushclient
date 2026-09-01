// pal/chat_protocol.h — the MudMaster / zChat negotiation handshake, as data.
//
// A chat peer's very first packet decides three things that matter and one that
// matters a lot:
//
//   * which dialect we speak to it ("CHAT:" = MudMaster, "ZCHAT:" = zChat, and
//     zChat moves file transfers to 1024-byte blocks);
//   * what name it claims, which is what the user sees in the accept prompt and
//     what every plugin is handed as the peer's identity;
//   * where it says it can be called back (the last five characters of the
//     line after the name are its port, the rest its address);
//   * and whether the connection is allowed AT ALL — ON_PLUGIN_CHAT_ACCEPT can
//     veto it, and a chat connection that gets in has live snooping of the
//     user's session and can send them files.
//
// WHY IT WAS LIFTED
//
// All of it lived inside CChatSocket::OnReceive.  chatsock.cpp is in
// MUSHGUI_ADAPTERS_SOURCES, so no headless test links it, and
// platform/windows_stubs.cpp defines OnReceive/OnSend/OnClose/OnConnect as
// EMPTY bodies for the test build — a test that called OnReceive would be
// testing the stub, not the protocol (the "stubs defend wrong contracts"
// shape).  The in-app selftests cannot reach it either: they all exit from
// wxApp::OnInit, long before any socket delivers a packet.  So the prefix
// validation, the two string splits, the address/port arithmetic and the plugin
// veto had never executed under test on this platform at all.
//
// Header-only and free of wx/MFC types, so chatsock.cpp (which compiles on
// Windows too) and the headless tests share one body — the pal/image_placement.h
// treatment.
//
// NOT modelled here: everything that needs a socket.  Framing of the message
// stream after the handshake (the \xFF terminator and the fixed-length file
// blocks), the message dispatch, file transfer, and the listen socket's accept
// path all stay in chatsock.cpp / chatlistensock.cpp.  Two of those have since
// been driven for real: tests/test_chat_sockets.cpp runs the accept path and the
// handshake end to end over loopback, and the MudMaster terminator search now
// calls chatFindChar below, so it too agrees with MFC about an embedded NUL.
// The rest -- the file-block framing, the dispatch and file transfer -- is still
// untested, so do not read this file's existence as "the chat protocol is
// covered".

#pragma once

#include <cstdlib>
#include <cstring>
#include <string>

namespace pal {

// Mirrors the m_iChatConnectionType enum in chatsock.h.  Restated rather than
// included so this header stays MFC-free; the two are pinned equal by a
// static_assert in chatsock.cpp.
enum {
    kChatMudMaster = 0,
    kChatZMud      = 1,
};

// m_iFileBlockSize: the constructor's default, and the bump zChat gets.
enum {
    kChatFileBlockSize  = 500,
    kZChatFileBlockSize = 1024,
};

// MFC's CString::Find(char) is strchr over the buffer, so it stops at the first
// embedded NUL even though GetLength() counts past it.  The port's CString is
// std::string-backed and does NOT stop.  A chat peer sends raw bytes and can
// put a NUL anywhere it likes, so the two builds parsed such a packet
// differently; searching in MFC's terms here is what makes them agree.
// Returns -1 when ch is not reached, exactly like Find.
inline int chatFindChar(const char* p, int len, char ch)
{
    for (int i = 0; i < len; ++i) {
        if (p[i] == '\0') return -1;    // strchr gives up here
        if (p[i] == ch)   return i;
    }
    return -1;
}

// ── "YES:" — the server accepted the call we made ─────────────────────────

struct ChatAcceptance {
    bool        ok = false;         // false: not an acceptance, drop the session
    std::string remoteUserName;     // may contain NULs; the whole line if no \n
    int         consumed = 0;       // bytes of the packet the handshake used up
};

// buff/count are the raw bytes just read from the socket.
inline ChatAcceptance parseChatAcceptance(const char* buff, int count)
{
    ChatAcceptance a;
    // count < 5, not < 4: a bare "YES:" with no name at all is a rejection.
    if (buff == nullptr || count < 5 || memcmp(buff, "YES:", 4) != 0)
        return a;

    a.ok = true;
    const char* p = buff + 4;
    const int   n = count - 4;

    const int i = chatFindChar(p, n, '\n');
    if (i == -1) {
        // No terminator: the whole remainder is the name and nothing is left
        // over for the message stream.
        a.remoteUserName.assign(p, n);
        a.consumed = count;
    } else {
        a.remoteUserName.assign(p, i);
        a.consumed = 4 + i + 1;     // + the \n itself
    }
    return a;
}

// ── "CHAT:" / "ZCHAT:" — someone is calling us ────────────────────────────

struct ChatRequest {
    bool ok = false;                // false: not a chat negotiation, refuse it
    int  connectionType = kChatMudMaster;
    long fileBlockSize  = kChatFileBlockSize;
    std::string remoteUserName;     // truncated at the zChat tab if present

    // False leaves the socket's constructor defaults ("<Unknown>" /
    // DEFAULT_CHAT_PORT) in place — the caller must not overwrite them.
    bool  haveAllegedAddress = false;
    std::string allegedAddress;
    // short, and the store below casts to short as well.  Either alone would
    // truncate; both are kept so that widening one does not silently start
    // handing the socket a port its own `short` member cannot hold.
    short allegedPort = 0;
};

inline ChatRequest parseChatConnectionRequest(const char* buff, int count)
{
    ChatRequest r;
    // 7 is the shortest thing that can be a request: "CHAT:" plus two bytes.
    // It also makes the 6-byte "ZCHAT:" compare safe.
    if (buff == nullptr || count < 7)
        return r;

    int off;
    if (memcmp(buff, "CHAT:", 5) == 0) {
        off = 5;
        r.connectionType = kChatMudMaster;
    } else if (memcmp(buff, "ZCHAT:", 6) == 0) {
        off = 6;
        r.connectionType = kChatZMud;
        r.fileBlockSize  = kZChatFileBlockSize;   // larger size for zChat
    } else {
        return r;
    }
    r.ok = true;

    const char* p = buff + off;
    const int   n = count - off;

    const int i = chatFindChar(p, n, '\n');
    if (i == -1)
        r.remoteUserName.assign(p, n);
    else
        r.remoteUserName.assign(p, i);

    // zChat puts a tab after the name (MudMaster does not, but the split is
    // unconditional upstream and a MudMaster name has no tab in it anyway).
    const int iTab = chatFindChar(r.remoteUserName.data(),
                                  (int) r.remoteUserName.size(), '\t');
    if (iTab != -1)
        r.remoteUserName.resize(iTab);

    // What is left after the name line.  With no \n there is nothing left: the
    // name swallowed the packet.
    std::string rest;
    if (i != -1)
        rest.assign(p + i + 1, n - i - 1);

    // zChat's security block follows another newline; throw it away.
    const int j = chatFindChar(rest.data(), (int) rest.size(), '\n');
    if (j != -1)
        rest.resize(j);

    // Strictly greater than 5: the last five characters are the port, so a
    // five-character line would leave an empty address, and upstream would
    // rather keep the defaults than record that.
    if ((int) rest.size() > 5) {
        r.haveAllegedAddress = true;
        r.allegedPort    = (short) atoi(rest.substr(rest.size() - 5).c_str());
        r.allegedAddress = rest.substr(0, rest.size() - 5);
    }
    return r;
}

// ── accepting the call ────────────────────────────────────────────────────

// What ON_PLUGIN_CHAT_ACCEPT is handed: the peer's REAL address (from the
// socket, not from its claim) and the name it claims, comma-separated.  A
// plugin that splits on the comma and finds more than two fields is looking at
// a name containing one — upstream does not quote it, and neither do we.
inline std::string chatAcceptCallbackArgument(const char* realAddress,
                                              const std::string& remoteUserName)
{
    std::string s(realAddress ? realAddress : "");
    s += ',';
    s += remoteUserName;
    return s;
}

enum ChatAcceptDecision {
    eChatAcceptAsk,             // no verdict yet — put the prompt up
    eChatAcceptAllow,
    eChatAcceptRejectPlugin,
    eChatAcceptRejectUser,
};

// Stage one.  A plugin veto is FINAL and short-circuits: the user is never
// asked, so there is no dialog through which a rejected peer can be let in.
inline ChatAcceptDecision chatAcceptAfterPlugins(bool pluginsAccepted,
                                                 bool validateIncomingCalls)
{
    if (!pluginsAccepted)
        return eChatAcceptRejectPlugin;
    return validateIncomingCalls ? eChatAcceptAsk : eChatAcceptAllow;
}

// Stage two, reached only from eChatAcceptAsk.
inline ChatAcceptDecision chatAcceptAfterUser(bool userAccepted)
{
    return userAccepted ? eChatAcceptAllow : eChatAcceptRejectUser;
}

// Anything that is not an outright Allow closes the socket.  Written as its own
// predicate so a decision that never got past eChatAcceptAsk (a prompt the
// caller forgot to run) fails closed rather than open.
inline bool chatConnectionAllowed(ChatAcceptDecision d)
{
    return d == eChatAcceptAllow;
}

} // namespace pal
