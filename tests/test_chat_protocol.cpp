// tests/test_chat_protocol.cpp — the MudMaster / zChat handshake.
//
// pal/chat_protocol.h holds the decisions CChatSocket::OnReceive makes about a
// peer's very first packet, restated here so they can be driven without a
// socket.  The chat family adoption (docs/STUB-ADOPTION-2026-08.md §5 step 2)
// put chatsock.cpp itself in mushcore, so OnReceive is the real state machine
// in this build too -- pal/chat_protocol.h exists to isolate the pure
// wire-format decisions for direct unit testing, not because the socket code
// is otherwise unreachable.
//
// The last group is the one with teeth.  ON_PLUGIN_CHAT_ACCEPT is the only
// programmatic bar on an incoming chat connection, and a connection that gets
// past it can snoop the user's session and push files at them, so "a plugin
// said no" must never end in an accepted socket -- including when the world
// also wants to prompt, which is the path where a second answer exists.
//
// The fixtures below are the wire format this client itself emits (chatsock.cpp
// SendData: "CHAT:%s\n%s%-5u" and "ZCHAT:%s\t\n%s%05u"), so the two directions
// are pinned against each other rather than against a guess.

#include "mc_test.h"
#include "pal/chat_protocol.h"

// The zChat wire-stamp round trip (CChatSocket::GetStamp / MakeStamp /
// Process_Stamp) is not part of pal::chat_protocol.h -- it lives in the real
// chatsock.cpp, which the chat family adoption (docs/STUB-ADOPTION-2026-08.md
// §5 step 2) put in mushcore, so it is reachable here directly.
#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "chatsock.h"

#include <string>

using std::string;

// Feed a std::string so fixtures can carry embedded NULs and the length is
// never taken with strlen.
static pal::ChatRequest req(const string& s) {
    return pal::parseChatConnectionRequest(s.data(), (int) s.size());
}
static pal::ChatAcceptance acc(const string& s) {
    return pal::parseChatAcceptance(s.data(), (int) s.size());
}

// ── prefix validation: which dialect, and what it changes ──────────────────

TEST(ChatProtocol, ChatPrefixSelectsMudMaster) {
    const pal::ChatRequest r = req("CHAT:Fred\n192.168.1.5" "4050 ");
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(pal::kChatMudMaster, r.connectionType);
    // MudMaster keeps the constructor's block size; only zChat moves it.
    EXPECT_EQ((long) pal::kChatFileBlockSize, r.fileBlockSize);
    EXPECT_EQ(500L, r.fileBlockSize);
    EXPECT_EQ(string("Fred"), r.remoteUserName);
}

TEST(ChatProtocol, ZChatPrefixSelectsZMudAndBumpsTheFileBlockSize) {
    const pal::ChatRequest r = req("ZCHAT:Fred\t\n192.168.1.5" "04050");
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(pal::kChatZMud, r.connectionType);
    EXPECT_EQ(1024L, r.fileBlockSize);
    EXPECT_EQ(string("Fred"), r.remoteUserName);
}

TEST(ChatProtocol, TheTwoDialectsAreDistinguishable) {
    // Guards against a parse that answers "zChat" (or MudMaster) for both:
    // the block size then disagrees with the peer and every file transfer
    // desynchronises.
    EXPECT_NE(req("CHAT:Fred\nx4050 ").connectionType,
              req("ZCHAT:Fred\t\nx04050").connectionType);
    EXPECT_NE(req("CHAT:Fred\nx4050 ").fileBlockSize,
              req("ZCHAT:Fred\t\nx04050").fileBlockSize);
}

TEST(ChatProtocol, TruncatedOrWrongPrefixIsRefused) {
    EXPECT_FALSE(req("").ok);                 // empty payload
    EXPECT_FALSE(req("C").ok);
    EXPECT_FALSE(req("CHAT:").ok);            // prefix only
    EXPECT_FALSE(req("CHAT:x").ok);           // 6 bytes: one short of the minimum
    EXPECT_FALSE(req("ZCHAT:").ok);           // 6 bytes, so not even compared
    EXPECT_FALSE(req("chat:Fred\n").ok);      // lower case
    EXPECT_FALSE(req("CHATS:Fred\n").ok);     // right letters, wrong shape
    EXPECT_FALSE(req("ZCHATFred\n").ok);      // colon missing
    EXPECT_FALSE(req("YES:Fred\n").ok);       // the OTHER handshake
    EXPECT_FALSE(req("xCHAT:Fred\n").ok);     // leading junk
}

TEST(ChatProtocol, SevenBytesIsTheShortestRequest) {
    // The bound is load-bearing in both directions: below it the six-byte
    // "ZCHAT:" compare would read past the packet.
    EXPECT_TRUE(req("CHAT:ab").ok);           // exactly 7
    EXPECT_FALSE(req("CHAT:a").ok);           // exactly 6
    EXPECT_TRUE(req("ZCHAT:a").ok);           // exactly 7
    EXPECT_EQ(pal::kChatZMud, req("ZCHAT:a").connectionType);
    EXPECT_EQ(string("a"), req("ZCHAT:a").remoteUserName);
}

TEST(ChatProtocol, ARefusedRequestCarriesNoName) {
    // A caller that ignores !ok must not find a usable name waiting for it.
    const pal::ChatRequest r = req("HELLO:Fred\n1.2.3.4" "4050 ");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.remoteUserName.empty());
    EXPECT_FALSE(r.haveAllegedAddress);
}

// ── the name split ─────────────────────────────────────────────────────────

TEST(ChatProtocol, NameStopsAtTheNewline) {
    EXPECT_EQ(string("Fred"), req("CHAT:Fred\n1.2.3.4" "4050 ").remoteUserName);
}

TEST(ChatProtocol, NameStopsAtTheTab) {
    // zChat writes name-then-tab; the tab split is unconditional upstream, so
    // a MudMaster peer that sends one is cut too.
    EXPECT_EQ(string("Fred"), req("ZCHAT:Fred\tsecret\n1.2.3.4" "04050").remoteUserName);
    EXPECT_EQ(string("Fred"), req("CHAT:Fred\tsecret\n1.2.3.4" "4050 ").remoteUserName);
    // A tab BEFORE any name leaves it empty rather than swallowing the line.
    EXPECT_EQ(string(""), req("ZCHAT:\tFred\n1.2.3.4" "04050").remoteUserName);
}

TEST(ChatProtocol, NoNewlineMakesTheWholeRemainderTheName) {
    const pal::ChatRequest r = req("CHAT:Fredward");
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(string("Fredward"), r.remoteUserName);
    // ...and nothing is left over to be an address.
    EXPECT_FALSE(r.haveAllegedAddress);
}

TEST(ChatProtocol, AnEmptyNameIsAllowedAndDoesNotEatTheAddress) {
    const pal::ChatRequest r = req("CHAT:\n192.168.1.5" "4050 ");
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(string(""), r.remoteUserName);
    ASSERT_TRUE(r.haveAllegedAddress);
    EXPECT_EQ(string("192.168.1.5"), r.allegedAddress);
}

// ── the alleged address / port: the last five characters ───────────────────

TEST(ChatProtocol, MudMasterAddressAndPortRoundTripOurOwnFormat) {
    // "CHAT:%s\n%s%-5u" -- port left-justified in five columns.
    const pal::ChatRequest r = req("CHAT:Fred\n192.168.1.5" "4050 ");
    ASSERT_TRUE(r.haveAllegedAddress);
    EXPECT_EQ(string("192.168.1.5"), r.allegedAddress);
    EXPECT_EQ((short) 4050, r.allegedPort);
}

TEST(ChatProtocol, ZChatAddressAndPortRoundTripOurOwnFormat) {
    // "ZCHAT:%s\t\n%s%05u" -- port zero-padded to five columns.
    const pal::ChatRequest r = req("ZCHAT:Fred\t\n192.168.1.5" "04050");
    ASSERT_TRUE(r.haveAllegedAddress);
    EXPECT_EQ(string("192.168.1.5"), r.allegedAddress);
    EXPECT_EQ((short) 4050, r.allegedPort);
}

TEST(ChatProtocol, ZChatSecurityBlockAfterTheSecondNewlineIsDiscarded) {
    const pal::ChatRequest r = req("ZCHAT:Fred\t\n192.168.1.5" "04050"
                                   "\nPGPKEYGOESHERE\nmore");
    ASSERT_TRUE(r.haveAllegedAddress);
    EXPECT_EQ(string("192.168.1.5"), r.allegedAddress);
    EXPECT_EQ((short) 4050, r.allegedPort);
}

TEST(ChatProtocol, SixCharactersIsTheShortestAddressLine) {
    // Strictly greater than five: five would be all port and no address, and
    // upstream keeps the socket's "<Unknown>" default instead.
    const pal::ChatRequest six = req("CHAT:Fred\nX" "04050");
    ASSERT_TRUE(six.haveAllegedAddress);
    EXPECT_EQ(string("X"), six.allegedAddress);
    EXPECT_EQ((short) 4050, six.allegedPort);

    const pal::ChatRequest five = req("CHAT:Fred\n" "04050");
    EXPECT_TRUE(five.ok);
    EXPECT_FALSE(five.haveAllegedAddress);
}

TEST(ChatProtocol, AddressLineShorterThanFiveIsIgnored) {
    for (const char* tail : { "", "1", "12", "123", "1234" }) {
        const pal::ChatRequest r = req(string("CHAT:Fred\n") + tail);
        EXPECT_TRUE(r.ok);
        EXPECT_FALSE(r.haveAllegedAddress);
        EXPECT_TRUE(r.allegedAddress.empty());
        EXPECT_EQ((short) 0, r.allegedPort);
    }
}

TEST(ChatProtocol, ANonNumericPortReadsAsZero) {
    // atoi, not a validator: garbage in the port columns is port 0, and the
    // address keeps everything before them.
    const pal::ChatRequest r = req("CHAT:Fred\n1.2.3.4" "abcde");
    ASSERT_TRUE(r.haveAllegedAddress);
    EXPECT_EQ(string("1.2.3.4"), r.allegedAddress);
    EXPECT_EQ((short) 0, r.allegedPort);
}

TEST(ChatProtocol, APortAbove32767WrapsBecauseTheFieldIsShort) {
    // m_iAllegedPort is a short, so this is what the socket has always stored;
    // pinned here so the extraction cannot quietly widen it.
    const pal::ChatRequest r = req("CHAT:Fred\n1.2.3.4" "65535");
    ASSERT_TRUE(r.haveAllegedAddress);
    EXPECT_EQ(string("1.2.3.4"), r.allegedAddress);
    EXPECT_EQ((short) -1, r.allegedPort);
}

// ── embedded NULs: MFC's CString::Find gives up at one ─────────────────────

TEST(ChatProtocol, ANulBeforeTheNewlineHidesIt) {
    // MFC's Find is strchr, so the newline after a NUL is invisible: the whole
    // remainder becomes the name and there is no address line left.  The port's
    // std::string-backed CString did NOT stop there, which is exactly the
    // divergence this shared parse removes.
    const string packet = string("CHAT:Fr") + '\0' + string("ed\n1.2.3.4" "4050 ");
    const pal::ChatRequest r = req(packet);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(packet.size() - 5, r.remoteUserName.size());
    EXPECT_FALSE(r.haveAllegedAddress);
}

TEST(ChatProtocol, ANulBeforeTheTabHidesItToo) {
    const string packet = string("ZCHAT:Fr") + '\0' + string("ed\tsecret\n1.2.3.4" "04050");
    const pal::ChatRequest r = req(packet);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(packet.size() - 6, r.remoteUserName.size());
}

TEST(ChatProtocol, FindCharStopsAtNulAndReportsMisses) {
    EXPECT_EQ(3, pal::chatFindChar("abc\nd", 5, '\n'));
    EXPECT_EQ(-1, pal::chatFindChar("abcd", 4, '\n'));
    EXPECT_EQ(-1, pal::chatFindChar("ab\0\n", 4, '\n'));   // NUL first
    EXPECT_EQ(0, pal::chatFindChar("\nab", 3, '\n'));
    EXPECT_EQ(-1, pal::chatFindChar("", 0, '\n'));
    // The length bounds the search even when the buffer continues.
    EXPECT_EQ(-1, pal::chatFindChar("ab\n", 2, '\n'));
}

// ── "YES:" -- the answer to a call we made ─────────────────────────────────

TEST(ChatProtocol, YesPrefixAcceptsAndNamesTheServer) {
    const pal::ChatAcceptance a = acc("YES:TheServer\n");
    ASSERT_TRUE(a.ok);
    EXPECT_EQ(string("TheServer"), a.remoteUserName);
    EXPECT_EQ(14, a.consumed);          // the whole packet, newline included
}

TEST(ChatProtocol, TextAfterTheAcceptanceIsLeftForTheMessageStream) {
    const string packet = "YES:TheServer\nHELLO";
    const pal::ChatAcceptance a = acc(packet);
    ASSERT_TRUE(a.ok);
    EXPECT_EQ(string("TheServer"), a.remoteUserName);
    EXPECT_EQ(14, a.consumed);
    EXPECT_EQ(string("HELLO"), packet.substr(a.consumed));
}

TEST(ChatProtocol, AnAcceptanceWithNoNewlineConsumesEverything) {
    const pal::ChatAcceptance a = acc("YES:TheServer");
    ASSERT_TRUE(a.ok);
    EXPECT_EQ(string("TheServer"), a.remoteUserName);
    EXPECT_EQ(13, a.consumed);          // nothing left over
}

TEST(ChatProtocol, FiveBytesIsTheShortestAcceptance) {
    const pal::ChatAcceptance a = acc("YES:x");
    ASSERT_TRUE(a.ok);
    EXPECT_EQ(string("x"), a.remoteUserName);
    EXPECT_EQ(5, a.consumed);
    // A bare "YES:" with no name is NOT an acceptance.
    EXPECT_FALSE(acc("YES:").ok);
}

TEST(ChatProtocol, AnythingElseIsARejectedCall) {
    EXPECT_FALSE(acc("").ok);
    EXPECT_FALSE(acc("N").ok);
    EXPECT_FALSE(acc("NO").ok);
    EXPECT_FALSE(acc("yes:x").ok);
    EXPECT_FALSE(acc("YE:xxx").ok);
    EXPECT_FALSE(acc("YESSIR").ok);          // the colon is part of the prefix
    EXPECT_FALSE(acc("CHAT:Fred\n").ok);     // the OTHER handshake
    EXPECT_FALSE(acc(" YES:x").ok);          // leading space
}

TEST(ChatProtocol, ARejectedAcceptanceCarriesNoNameAndConsumesNothing) {
    const pal::ChatAcceptance a = acc("NO:go away");
    EXPECT_FALSE(a.ok);
    EXPECT_TRUE(a.remoteUserName.empty());
    EXPECT_EQ(0, a.consumed);
}

TEST(ChatProtocol, ConsumedNeverRunsPastThePacket) {
    // The caller memmoves by `consumed`, so an over-count would read off the
    // end of a 1000-byte stack buffer.
    const char* packets[] = { "YES:x", "YES:x\n", "YES:name\ntail", "YES:name",
                              "YES:\n", "YES:\n\n" };
    for (const char* p : packets) {
        const string s(p);
        const pal::ChatAcceptance a = acc(s);
        ASSERT_TRUE(a.ok);
        EXPECT_GE(a.consumed, 0);
        EXPECT_LE(a.consumed, (int) s.size());
    }
}

// ── ON_PLUGIN_CHAT_ACCEPT: the veto ────────────────────────────────────────

TEST(ChatProtocol, APluginVetoIsFinal) {
    // Both worlds' settings, one answer.  The second case is the dangerous one:
    // if a veto produced eChatAcceptAsk, the accept prompt would run and a
    // "Yes" would let in a peer a plugin refused.
    EXPECT_EQ(pal::eChatAcceptRejectPlugin,
              pal::chatAcceptAfterPlugins(false, false));
    EXPECT_EQ(pal::eChatAcceptRejectPlugin,
              pal::chatAcceptAfterPlugins(false, true));
    EXPECT_FALSE(pal::chatConnectionAllowed(pal::chatAcceptAfterPlugins(false, false)));
    EXPECT_FALSE(pal::chatConnectionAllowed(pal::chatAcceptAfterPlugins(false, true)));
}

TEST(ChatProtocol, WithNoVetoTheWorldSettingDecidesWhetherToPrompt) {
    EXPECT_EQ(pal::eChatAcceptAllow, pal::chatAcceptAfterPlugins(true, false));
    EXPECT_EQ(pal::eChatAcceptAsk,   pal::chatAcceptAfterPlugins(true, true));
}

TEST(ChatProtocol, TheUserAnswerDecidesOnlyAfterAPrompt) {
    EXPECT_EQ(pal::eChatAcceptAllow,      pal::chatAcceptAfterUser(true));
    EXPECT_EQ(pal::eChatAcceptRejectUser, pal::chatAcceptAfterUser(false));
}

TEST(ChatProtocol, OnlyAnOutrightAllowOpensTheConnection) {
    EXPECT_TRUE(pal::chatConnectionAllowed(pal::eChatAcceptAllow));
    EXPECT_FALSE(pal::chatConnectionAllowed(pal::eChatAcceptRejectPlugin));
    EXPECT_FALSE(pal::chatConnectionAllowed(pal::eChatAcceptRejectUser));
    // An un-answered prompt fails CLOSED.
    EXPECT_FALSE(pal::chatConnectionAllowed(pal::eChatAcceptAsk));
}

TEST(ChatProtocol, TheWholeAcceptTruthTable) {
    // Drives the two stages exactly as CChatSocket::OnReceive does, so the
    // short-circuit is exercised rather than assumed: `asked` records whether
    // the modal would have been put up at all.
    struct Row { bool plugins, validate, user, expectAllowed, expectAsked; };
    static const Row rows[] = {
        { false, false, false, false, false },
        { false, false, true,  false, false },   // "Yes" cannot rescue a veto
        { false, true,  false, false, false },
        { false, true,  true,  false, false },   // ...not even with a prompt
        { true,  false, false, true,  false },   // no prompt wanted: straight in
        { true,  false, true,  true,  false },
        { true,  true,  false, false, true  },
        { true,  true,  true,  true,  true  },
    };
    for (const Row& r : rows) {
        pal::ChatAcceptDecision d = pal::chatAcceptAfterPlugins(r.plugins, r.validate);
        const bool asked = (d == pal::eChatAcceptAsk);
        if (asked)
            d = pal::chatAcceptAfterUser(r.user);
        EXPECT_EQ(r.expectAsked, asked);
        EXPECT_EQ(r.expectAllowed, pal::chatConnectionAllowed(d));
        // Whatever happened, the decision is resolved: nothing reaches the
        // socket still saying "ask me".
        EXPECT_NE(pal::eChatAcceptAsk, d);
    }
}

// ── what the plugin is shown ───────────────────────────────────────────────

TEST(ChatProtocol, TheVetoCallbackGetsTheRealAddressThenTheClaimedName) {
    // Order matters: a plugin's allow-list keys off the address, which is the
    // half the peer cannot forge (it comes from the accepted socket).
    EXPECT_EQ(string("10.0.0.1,Fred"),
              pal::chatAcceptCallbackArgument("10.0.0.1", "Fred"));
    EXPECT_EQ(string("10.0.0.1,"),
              pal::chatAcceptCallbackArgument("10.0.0.1", ""));
    EXPECT_EQ(string(",Fred"), pal::chatAcceptCallbackArgument("", "Fred"));
    EXPECT_EQ(string(",Fred"), pal::chatAcceptCallbackArgument(nullptr, "Fred"));
}

TEST(ChatProtocol, ACommaInTheNameIsNotEscaped) {
    // Recorded, not endorsed: upstream does not quote the fields, so a peer
    // calling itself "a,b" hands the plugin three of them.  A plugin that
    // splits on every comma must use the FIRST field as the address.
    const string s = pal::chatAcceptCallbackArgument("10.0.0.1", "a,b");
    EXPECT_EQ(string("10.0.0.1,a,b"), s);
    EXPECT_EQ(8u, s.find(','));
}

// ── the zChat stamp: LP64 sign-extension ────────────────────────────────────

TEST(ChatProtocol, ProcessStampRecognisesASelfCollisionWithTheTopBitSet) {
    // Process_Stamp's rule is "if the incoming stamp equals ours, we have a
    // collision -- mint a new one" (chatsock.cpp:1680).  GetStamp rebuilds the
    // 4-byte wire stamp through `int` arithmetic; if the top byte has bit 7
    // set, an `int` accumulator sign-extends the result to a huge negative
    // `long` on LP64, while m_zChatStamp (assigned straight from an
    // `unsigned int` SHS digest word) zero-extends to a small positive one.
    // The two then never compare equal for such a stamp, so a genuine
    // self-collision went unrecognised and the colliding stamp was kept
    // forever instead of being regenerated.
    CMUSHclientDoc doc;
    CChatSocket sock(&doc);
    sock.m_iChatConnectionType = eChatZMud;

    const long kOriginalStamp = 0x80000001L;
    sock.m_zChatStamp = kOriginalStamp;

    // The peer echoes back exactly our own stamp.
    CString wire = sock.MakeStamp (kOriginalStamp);
    ASSERT_EQ(4, wire.GetLength ());

    sock.Process_Stamp (wire);

    // A recognised collision regenerates m_zChatStamp via MakeRandomNumber;
    // the buggy comparison missed the match, so the stamp never changed.
    EXPECT_NE(kOriginalStamp, sock.m_zChatStamp);
}

TEST(ChatProtocol, GetStampZeroExtendsLikeTheDigestAssignmentDoes) {
    // Same defect, isolated to the extraction itself: bytes decoding to
    // 0x80000001 must come back as that same positive value cast to `long`,
    // not sign-extended to 0xFFFFFFFF80000001.
    CMUSHclientDoc doc;
    CChatSocket sock(&doc);
    sock.m_iChatConnectionType = eChatZMud;

    CString wire = sock.MakeStamp (0x80000001L);
    ASSERT_EQ(4, wire.GetLength ());

    const long iStamp = sock.GetStamp (wire);
    EXPECT_EQ(0x80000001L, iStamp);
    EXPECT_TRUE(wire.IsEmpty ());   // the 4 stamp bytes were consumed
}

MC_TEST_MAIN()
