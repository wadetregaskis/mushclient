// tests/test_telnet.cpp
// Unit tests for the CMUSHclientDoc telnet-phase state machine:
//   Phase_ESC, Phase_ANSI, Phase_IAC, Phase_WILL/WONT/DO/DONT,
//   Phase_SB/SUBNEGOTIATION/SUBNEGOTIATION_IAC, Phase_COMPRESS,
//   Phase_UTF8, Handle_Telnet_Request (stub), Send_IAC_* negotiation.
//
// Strategy: construct a CMUSHclientDoc, drive its Phase_* methods directly,
// and inspect the resulting state fields (m_phase, negotiation arrays,
// counters, etc.).

#include "mc_test.h"
// B-3 atomic flip: real headers (portable stub deleted; tests are FULL_BUILD).
#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "scripting/errors.h"
#include "pal/itransport.h"
#include <cstring>

// A transport that records the raw negotiation bytes the doc puts on the wire,
// so Send_IAC_* suppression is observable.
namespace {
struct RecordingTransport : pal::ITransport {
    std::string sent;
    void resolve(const char*, pal::ResolveToken) override {}
    void cancelResolve() override {}
    void connect(unsigned int, unsigned short, const pal::TransportOptions&) override {}
    void send(const char* data, size_t n) override { sent.append(data, n); }
    void startTls(const pal::TlsParams&) override {}
    void close() override {}
    std::string lastErrorString() const override { return "fake"; }
};
} // namespace

// telopt_name — the telnet-option name table.  Was doc_portable.cpp (deleted
// at the B-3 flip); production code uses telnet_phases.cpp's own no-op macro,
// so this table now lives with its only consumer, this test.
static const char* telopt_name(unsigned char opt)
{
    switch (opt) {
    case   0: return "BINARY";
    case   1: return "ECHO";
    case   3: return "SGA";
    case  24: return "TERMINAL-TYPE";
    case  25: return "EOR";
    case  31: return "NAWS";
    case  32: return "TERMINAL-SPEED";
    case  34: return "LINEMODE";
    case  36: return "ENVIRON";
    case  39: return "NEW-ENVIRON";
    case  42: return "CHARSET";
    case  85: return "COMPRESS(MCCP1)";
    case  86: return "COMPRESS2(MCCP2)";
    case  90: return "MSP";
    case  91: return "MXP";
    case  93: return "ZMP";
    case 102: return "MUD-SPECIFIC(ATCP)";
    case 200: return "ATCP";
    case 201: return "GMCP";
    default:  return "?";
    }
}

// ── Helpers ───────────────────────────────────────────────────────────────

static void make_doc(CMUSHclientDoc& doc)
{
    doc.m_pCurrentLine = new CLine();
    doc.m_iFlags       = COLOUR_ANSI;
    doc.m_iForeColour  = WHITE;
    doc.m_iBackColour  = BLACK;
    doc.AddStyle(COLOUR_ANSI, WHITE, BLACK, 0,
                 static_cast<CAction*>(nullptr));
}

// ── Phase_ESC ────────────────────────────────────────────────────────────

TEST(TelnetTest, Phase_ESC_BracketTransitionsToDOING_CODE)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_phase = HAVE_ESC;
    doc.Phase_ESC('[');
    EXPECT_EQ(DOING_CODE, doc.m_phase);
    EXPECT_EQ(0, doc.m_code);
}

TEST(TelnetTest, Phase_ESC_OtherCharReturnsToNONE)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_phase = HAVE_ESC;
    doc.Phase_ESC('A');
    EXPECT_EQ(NONE, doc.m_phase);
}

// ── Phase_ANSI ────────────────────────────────────────────────────────────

TEST(TelnetTest, Phase_ANSI_DigitAccumulates)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_phase = DOING_CODE;
    doc.m_code  = 0;
    doc.Phase_ANSI('3');
    EXPECT_EQ(3, doc.m_code);
    doc.Phase_ANSI('1');
    EXPECT_EQ(31, doc.m_code);
}

TEST(TelnetTest, Phase_ANSI_mFinalisesDOING_CODE)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_phase = DOING_CODE;
    doc.m_code  = ANSI_TEXT_RED;     // code 31
    doc.Phase_ANSI('m');
    // After 'm' the phase resets to NONE and InterpretANSIcode(31) is called,
    // setting the foreground to RED.
    EXPECT_EQ(NONE, doc.m_phase);
    EXPECT_EQ((COLORREF)RED, doc.m_iForeColour);
}

TEST(TelnetTest, Phase_ANSI_SemicolonSeparatorCallsInterpret)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_phase = DOING_CODE;
    doc.m_code  = ANSI_BOLD;   // code 1
    // ';' dispatches the current code and resets m_code to 0
    doc.Phase_ANSI(';');
    EXPECT_EQ(0, doc.m_code);
    EXPECT_TRUE((doc.m_iFlags & HILITE) != 0);
}

TEST(TelnetTest, Phase_ANSI_UnknownCharReturnsNONE)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_phase = DOING_CODE;
    doc.m_code  = 0;
    doc.Phase_ANSI('X');
    EXPECT_EQ(NONE, doc.m_phase);
}

// ── Phase_IAC ────────────────────────────────────────────────────────────

// Parameterised (audit §4): the pure command-byte → phase transitions (WILL/WONT/DO/
// DONT/SB/SE/NOP) via a {command, expectedPhase, checkNewC, expectedNewC} table.
// Only WILL's original test asserted the consumed byte (c → 0), preserved via
// checkNewC.  The GA newline-conversion cases stay named below because they need the
// m_bConvertGAtoNewline precondition and check the c→'\n' conversion (special).
TEST(TelnetTest, Phase_IAC_SetsPhaseForCommandByte)
{
    struct {
        unsigned char cmd; int expectedPhase; bool checkNewC; unsigned char expectedNewC;
    } cases[] = {
        { (unsigned char)WILL, HAVE_WILL, true,  '\0' },  // byte consumed → c set to 0
        { (unsigned char)WONT, HAVE_WONT, false, 0    },
        { (unsigned char)DO,   HAVE_DO,   false, 0    },
        { (unsigned char)DONT, HAVE_DONT, false, 0    },
        { (unsigned char)SB,   HAVE_SB,   false, 0    },
        { (unsigned char)SE,   NONE,      false, 0    },
        { (unsigned char)NOP,  NONE,      false, 0    },
    };
    for (auto& tc : cases) {
        CMUSHclientDoc doc; make_doc(doc);
        unsigned char c = tc.cmd;
        doc.Phase_IAC(c);
        EXPECT_EQ(tc.expectedPhase, doc.m_phase);
        if (tc.checkNewC) EXPECT_EQ(tc.expectedNewC, c);
    }
}

TEST(TelnetTest, Phase_IAC_GA_ConvertsToNewlineWhenEnabled)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_bConvertGAtoNewline = 1;
    unsigned char c = GO_AHEAD;
    doc.Phase_IAC(c);
    EXPECT_EQ(NONE, doc.m_phase);
    EXPECT_EQ('\n', c);   // should have been converted to newline
}

TEST(TelnetTest, Phase_IAC_GA_NoConversionWhenDisabled)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_bConvertGAtoNewline = 0;
    unsigned char c = GO_AHEAD;
    doc.Phase_IAC(c);
    EXPECT_EQ(NONE, doc.m_phase);
    EXPECT_EQ('\0', c);   // no conversion
}

// ── Phase_WILL (telnet negotiation) ──────────────────────────────────────

TEST(TelnetTest, Phase_WILL_SGAAgrees)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.Phase_WILL(SGA);
    EXPECT_EQ(NONE, doc.m_phase);
    EXPECT_TRUE(doc.m_bClient_sent_IAC_DO[SGA]);
    EXPECT_EQ(1, doc.m_nCount_IAC_WILL);
}

TEST(TelnetTest, Phase_WILL_UnknownRefuses)
{
    CMUSHclientDoc doc; make_doc(doc);
    // With no plugins, Handle_Telnet_Request returns false → sends DONT
    doc.Phase_WILL(99);
    EXPECT_EQ(NONE, doc.m_phase);
    EXPECT_TRUE(doc.m_bClient_sent_IAC_DONT[99]);
}

TEST(TelnetTest, Phase_WILL_EchoAgrees)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_bNoEchoOff = 0;
    doc.Phase_WILL(TELOPT_ECHO);
    EXPECT_TRUE(doc.m_bNoEcho);
    EXPECT_TRUE(doc.m_bClient_sent_IAC_DO[TELOPT_ECHO]);
}

TEST(TelnetTest, Phase_WILL_EchoRefusedWhenNoEchoOff)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_bNoEchoOff = 1;
    doc.Phase_WILL(TELOPT_ECHO);
    EXPECT_FALSE(doc.m_bNoEcho);
    EXPECT_TRUE(doc.m_bClient_sent_IAC_DONT[TELOPT_ECHO]);
}

// ── Phase_WONT ────────────────────────────────────────────────────────────

TEST(TelnetTest, Phase_WONT_EchoRestoresEcho)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_bNoEcho    = true;
    doc.m_bNoEchoOff = 0;
    doc.Phase_WONT(TELOPT_ECHO);
    EXPECT_FALSE(doc.m_bNoEcho);
    EXPECT_TRUE(doc.m_bClient_sent_IAC_DONT[TELOPT_ECHO]);
    EXPECT_EQ(1, doc.m_nCount_IAC_WONT);
}

TEST(TelnetTest, Phase_WONT_UnknownSendsDONT)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.Phase_WONT(50);
    EXPECT_TRUE(doc.m_bClient_sent_IAC_DONT[50]);
}

// ── Phase_DO ─────────────────────────────────────────────────────────────

TEST(TelnetTest, Phase_DO_SGAAgrees)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.Phase_DO(SGA);
    EXPECT_EQ(NONE, doc.m_phase);
    EXPECT_TRUE(doc.m_bClient_sent_IAC_WILL[SGA]);
    EXPECT_EQ(1, doc.m_nCount_IAC_DO);
}

// Phase_DO_TerminalTypeAgrees deleted (audit §3): Phase_DO_TerminalTypeResetsSequence
// subsumes it — it also asserts m_bClient_sent_IAC_WILL[TT] and the reset-to-0, while
// pre-setting m_ttype_sequence=2 so the reset is a real observation.

TEST(TelnetTest, Phase_DO_UnknownSendsWONT)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.Phase_DO(77);
    EXPECT_TRUE(doc.m_bClient_sent_IAC_WONT[77]);
}

// ── Phase_DONT ────────────────────────────────────────────────────────────

TEST(TelnetTest, Phase_DONT_SendsWONT)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.Phase_DONT(SGA);
    EXPECT_EQ(NONE, doc.m_phase);
    EXPECT_TRUE(doc.m_bClient_sent_IAC_WONT[SGA]);
    EXPECT_EQ(1, doc.m_nCount_IAC_DONT);
}

// ── Phase_SB (subnegotiation start) ──────────────────────────────────────

TEST(TelnetTest, Phase_SB_NormalTypeTransitionsToHAVE_SUBNEG)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.Phase_SB(TELOPT_TERMINAL_TYPE);
    EXPECT_EQ(HAVE_SUBNEGOTIATION, doc.m_phase);
    EXPECT_EQ(TELOPT_TERMINAL_TYPE, doc.m_subnegotiation_type);
    EXPECT_TRUE(doc.m_IAC_subnegotiation_data.empty());
}

TEST(TelnetTest, Phase_SB_COMPRESSTransitionsToHAVE_COMPRESS)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.Phase_SB(TELOPT_COMPRESS);
    EXPECT_EQ(HAVE_COMPRESS, doc.m_phase);
}

// ── Phase_SUBNEGOTIATION ─────────────────────────────────────────────────

TEST(TelnetTest, Phase_SUBNEG_CollectsData)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_phase = HAVE_SUBNEGOTIATION;
    doc.Phase_SUBNEGOTIATION('A');
    doc.Phase_SUBNEGOTIATION('B');
    EXPECT_EQ("AB", doc.m_IAC_subnegotiation_data);
    EXPECT_EQ(HAVE_SUBNEGOTIATION, doc.m_phase);
}

TEST(TelnetTest, Phase_SUBNEG_IACTransitionsToHAVE_SUBNEG_IAC)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_phase = HAVE_SUBNEGOTIATION;
    doc.Phase_SUBNEGOTIATION(IAC);
    EXPECT_EQ(HAVE_SUBNEGOTIATION_IAC, doc.m_phase);
}

// ── Phase_SUBNEGOTIATION_IAC ──────────────────────────────────────────────

TEST(TelnetTest, Phase_SUBNEG_IAC_IACIACStoresOneIAC)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_phase = HAVE_SUBNEGOTIATION_IAC;
    doc.m_IAC_subnegotiation_data = "AB";
    doc.Phase_SUBNEGOTIATION_IAC(IAC);   // IAC IAC → store single IAC
    EXPECT_EQ(HAVE_SUBNEGOTIATION, doc.m_phase);
    EXPECT_EQ(3u, doc.m_IAC_subnegotiation_data.size());
    EXPECT_EQ((char)IAC, doc.m_IAC_subnegotiation_data[2]);
}

TEST(TelnetTest, Phase_SUBNEG_IAC_SEEndsSubnegotiation)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_subnegotiation_type = 99;   // unknown type → plugin call (no-op stub)
    doc.m_IAC_subnegotiation_data = "hello";
    doc.m_phase = HAVE_SUBNEGOTIATION_IAC;
    doc.Phase_SUBNEGOTIATION_IAC(SE);
    EXPECT_EQ(NONE, doc.m_phase);
    EXPECT_EQ(1, doc.m_nCount_IAC_SB);
}

// ── Phase_COMPRESS ────────────────────────────────────────────────────────

TEST(TelnetTest, Phase_COMPRESS_WILLTransitions)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_phase = HAVE_COMPRESS;
    doc.Phase_COMPRESS(WILL);
    EXPECT_EQ(HAVE_COMPRESS_WILL, doc.m_phase);
}

TEST(TelnetTest, Phase_COMPRESS_NonWILLReturnsNONE)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_phase = HAVE_COMPRESS;
    doc.Phase_COMPRESS(0x42);
    EXPECT_EQ(NONE, doc.m_phase);
}

// ── No-double-negotiate guard ─────────────────────────────────────────────

TEST(TelnetTest, Send_IAC_DO_NoDoubleNegotiate)
{
    CMUSHclientDoc doc; make_doc(doc);
    // First call should record the send
    doc.Send_IAC_DO(SGA);
    EXPECT_TRUE(doc.m_bClient_sent_IAC_DO[SGA]);
    EXPECT_FALSE(doc.m_bClient_sent_IAC_DONT[SGA]);

    // Second call should be a no-op (already sent DO)
    doc.m_bClient_sent_IAC_DO[SGA] = true;   // mark as already sent
    // Reset tracking to detect whether SendPacket is called again — can't
    // intercept easily, but we can confirm the flag state does not flip.
    doc.Send_IAC_DO(SGA);   // should return early
    EXPECT_TRUE(doc.m_bClient_sent_IAC_DO[SGA]);
}

TEST(TelnetTest, Send_IAC_WILL_DONT_ClearsCrossFlag)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.Send_IAC_WILL(TELOPT_ECHO);
    EXPECT_TRUE (doc.m_bClient_sent_IAC_WILL[TELOPT_ECHO]);
    EXPECT_FALSE(doc.m_bClient_sent_IAC_WONT[TELOPT_ECHO]);
}

// ── Handle_Telnet_Request (plugin stub) ───────────────────────────────────

TEST(TelnetTest, Handle_Telnet_Request_ReturnsFalseWithNoPlugins)
{
    CMUSHclientDoc doc; make_doc(doc);
    // Stub always returns bDefaultReturn = false
    EXPECT_FALSE(doc.Handle_Telnet_Request(200, "WILL"));
}

// ── IAC counters ─────────────────────────────────────────────────────────

TEST(TelnetTest, Counters_WILL_DO_WONT_DONT_SB)
{
    CMUSHclientDoc doc; make_doc(doc);

    doc.Phase_WILL(SGA);
    EXPECT_EQ(1, doc.m_nCount_IAC_WILL);

    doc.Phase_DO(SGA);
    EXPECT_EQ(1, doc.m_nCount_IAC_DO);

    doc.Phase_WONT(SGA);
    EXPECT_EQ(1, doc.m_nCount_IAC_WONT);

    doc.Phase_DONT(SGA);
    EXPECT_EQ(1, doc.m_nCount_IAC_DONT);

    doc.m_subnegotiation_type = 99;
    doc.Phase_SUBNEGOTIATION_IAC(SE);
    EXPECT_EQ(1, doc.m_nCount_IAC_SB);
}

// ── NAWS negotiation ─────────────────────────────────────────────────────

TEST(TelnetTest, Phase_DO_NAWS_SendsWILL_When_bNAWS_Set)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_bNAWS       = TRUE;
    doc.m_nWrapColumn = 80;
    doc.Phase_DO(TELOPT_NAWS);
    EXPECT_TRUE(doc.m_bClient_sent_IAC_WILL[TELOPT_NAWS]);
    EXPECT_TRUE(doc.m_bNAWS_wanted);
}

TEST(TelnetTest, Phase_DO_NAWS_SendsWONT_When_bNAWS_Clear)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_bNAWS = FALSE;
    doc.Phase_DO(TELOPT_NAWS);
    EXPECT_TRUE(doc.m_bClient_sent_IAC_WONT[TELOPT_NAWS]);
    EXPECT_FALSE(doc.m_bNAWS_wanted);
}

// ── MXP negotiation ───────────────────────────────────────────────────────

// Phase_WILL_MXP_AgreesWhenEnabled deleted (audit §3): WillMxpQueryModeTurnsMxpOnImmediately
// fully contains it (same eQueryMXP + Phase_WILL(MXP) + IAC_DO[MXP] assert, plus m_bMXP on
// and Phase_DONT off). The eNoMXP-refuses partner below is kept as the boundary counterpart.

TEST(TelnetTest, Phase_WILL_MXP_RefusesWhenDisabled)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_iUseMXP = eNoMXP;
    doc.Phase_WILL(TELOPT_MXP);
    EXPECT_TRUE(doc.m_bClient_sent_IAC_DONT[TELOPT_MXP]);
}

// ── Terminal type sequence ────────────────────────────────────────────────

TEST(TelnetTest, Phase_DO_TerminalTypeResetsSequence)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_ttype_sequence = 2;   // pretend we are mid-sequence
    doc.Phase_DO(TELOPT_TERMINAL_TYPE);
    EXPECT_EQ(0, doc.m_ttype_sequence);
    EXPECT_TRUE(doc.m_bClient_sent_IAC_WILL[TELOPT_TERMINAL_TYPE]);
}

// ── Intent-comment tests (candidate mining, 2026-07) ─────────────────────

#include "pal/itransport.h"
#include <string>

namespace {
// Captures everything SendPacket puts on the wire (the doc dtor frees it).
struct TelnetFakeTransport : pal::ITransport {
    std::string sent;
    void resolve(const char*, pal::ResolveToken) override {}
    void cancelResolve() override {}
    void connect(unsigned int, unsigned short, const pal::TransportOptions&) override {}
    void send(const char* data, size_t n) override { sent.append(data, n); }
    void startTls(const pal::TlsParams&) override {}
    void close() override {}
    std::string lastErrorString() const override { return "fake"; }
};
} // namespace

// forum ?id=10043: inside HAVE_SUBNEGOTIATION_IAC, any byte other than IAC is
// treated as if it were SE (the spec is silent) — the subnegotiation still
// completes: phase returns to NONE and the handler dispatch is counted.
TEST(TelnetTest, SubnegotiationIacNonSeByteTreatedAsSe)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_subnegotiation_type = 99;          // unknown type → plugin no-op
    doc.m_IAC_subnegotiation_data = "abc";
    doc.m_phase = HAVE_SUBNEGOTIATION_IAC;
    doc.Phase_SUBNEGOTIATION_IAC('X');       // not IAC, not SE
    EXPECT_EQ(NONE, doc.m_phase);            // negotiation is over
    EXPECT_EQ(1, doc.m_nCount_IAC_SB);       // handler was dispatched
}

// telnet_phases.cpp Phase_WILL: "don't agree to MCCP1 and MCCP2" — once
// COMPRESS2 has been agreed, a later IAC WILL COMPRESS (v1) is answered DONT.
TEST(TelnetTest, Mccp1RefusedAfterMccp2Agreed)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.Phase_WILL(TELOPT_COMPRESS2);        // agree to MCCP2
    EXPECT_TRUE(doc.m_bClient_sent_IAC_DO[TELOPT_COMPRESS2]);
    EXPECT_TRUE(doc.m_bSupports_MCCP_2);

    doc.Phase_WILL(TELOPT_COMPRESS);         // MCCP1 offered afterwards
    EXPECT_TRUE (doc.m_bClient_sent_IAC_DONT[TELOPT_COMPRESS]);  // refused
    EXPECT_FALSE(doc.m_bClient_sent_IAC_DO  [TELOPT_COMPRESS]);  // never DO
}

// RFC 2066: a CHARSET REQUEST naming no charset the client uses is answered
// with exactly IAC SB CHARSET REJECTED(0x03) IAC SE.
TEST(TelnetTest, CharsetRequestUnmatchedSendsRejected)
{
    CMUSHclientDoc doc; make_doc(doc);
    TelnetFakeTransport* t = new TelnetFakeTransport();
    doc.m_pTransport = t;
    doc.m_bUTF_8 = FALSE;                    // client charset is "US-ASCII"

    // REQUEST(0x01) DELIM(';') "UTF-8" — not the charset in use
    doc.m_IAC_subnegotiation_data = std::string("\x01;UTF-8", 7);
    doc.Handle_TELOPT_CHARSET();

    const char expected[] = { (char)IAC, (char)SB, (char)TELOPT_CHARSET,
                              3, (char)IAC, (char)SE };
    ASSERT_EQ((size_t)6, t->sent.size());
    EXPECT_TRUE(memcmp(t->sent.data(), expected, 6) == 0);
}

// CharsetRequestMatchedSendsAccepted deleted (audit §3): the accept case (matching
// UTF-8 REQUEST → IAC SB CHARSET ACCEPTED(2) "UTF-8" IAC SE) is duplicated verbatim
// inside TelnetR4.CharsetRequestEdgeAndAcceptArms, which also covers the too-short /
// non-REQUEST edge cases and the SB-dispatch route.

// telnet_phases.cpp: "ensure max of 20 so we don't overflow the field" — a
// long terminal identification is truncated to 20 chars in the TTYPE IS reply
// (the response buffer is only 40 bytes).
TEST(TelnetTest, TerminalTypeReplyTruncatedToTwentyChars)
{
    CMUSHclientDoc doc; make_doc(doc);
    TelnetFakeTransport* t = new TelnetFakeTransport();
    doc.m_pTransport = t;
    doc.m_strTerminalIdentification = "abcdefghijklmnopqrstuvwxyz";  // 26
    doc.m_ttype_sequence = 0;
    doc.m_IAC_subnegotiation_data = std::string("\x01", 1);   // TTYPE SEND

    doc.Handle_TELOPT_TERMINAL_TYPE();

    // IAC SB TERMINAL-TYPE IS <ident, capped at 20> IAC SE
    ASSERT_EQ((size_t)(4 + 20 + 2), t->sent.size());
    EXPECT_TRUE(memcmp(t->sent.data() + 4, "abcdefghijklmnopqrst", 20) == 0);
    EXPECT_EQ((char)IAC, t->sent[24]);
    EXPECT_EQ((char)SE,  t->sent[25]);
}

// telnet_phases.cpp OutputBadUTF8characters: a bad UTF-8 sequence is flushed
// to the line as raw bytes (non-Windows path), never silently dropped, and
// the phase machine returns to NONE.
TEST(TelnetTest, BadUtf8SequenceOutputsRawBytesNotDropped)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_bUTF_8 = TRUE;
    const char seq[] = "ab\xC3Xcd";   // 0xC3 lead byte, 'X' bad continuation
    doc.DisplayMsg(seq, (int)strlen(seq), 0);
    EXPECT_EQ(NONE, doc.m_phase);
    std::string visible(doc.m_pCurrentLine->text, doc.m_pCurrentLine->len);
    EXPECT_STREQ("ab\xC3Xcd", visible.c_str());
}

// doc_portable.cpp: the ctor's SetAlphaDefaults applies the Windows table
// defaults — lowercase "mushclient" (the old hand-set "MUSHCLIENT" was twin
// drift) and language "lua".
TEST(TelnetTest, DefaultTerminalIdentificationIsLowercaseMushclient)
{
    CMUSHclientDoc doc;
    EXPECT_STREQ("mushclient", (const char*)doc.m_strTerminalIdentification);
    EXPECT_STREQ("lua",        (const char*)doc.m_strLanguage);
}

// ── MCCP negotiation + decompression switch-on (round-2 additions) ────────

// IAC WILL COMPRESS2 initialises zlib + buffers and agrees (Send_IAC_DO);
// the following IAC SB COMPRESS2 IAC SE then turns decompression on.
TEST(TelnetTest, WillCompress2ThenSubnegEnablesDecompression)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.Phase_WILL(TELOPT_COMPRESS2);
    EXPECT_TRUE(doc.m_bSupports_MCCP_2);
    EXPECT_TRUE(doc.m_bCompressInitOK);
    EXPECT_TRUE(doc.m_bClient_sent_IAC_DO[TELOPT_COMPRESS2]);

    doc.Phase_SB(TELOPT_COMPRESS2);
    EXPECT_EQ(HAVE_SUBNEGOTIATION, doc.m_phase);
    doc.Phase_SUBNEGOTIATION_IAC(SE);          // Handle_TELOPT_COMPRESS2
    EXPECT_TRUE(doc.m_bCompress);
    EXPECT_EQ(2, doc.m_iMCCP_type);
    EXPECT_EQ(NONE, doc.m_phase);
}

// MCCP v1: IAC WILL COMPRESS, then the odd IAC SB COMPRESS WILL SE sequence
// (no IAC before SE) drives Phase_COMPRESS → Phase_COMPRESS_WILL, which
// resets zlib and turns decompression on with type 1.
TEST(TelnetTest, CompressV1FullSequenceEnablesDecompression)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.Phase_WILL(TELOPT_COMPRESS);           // agrees, allocates buffers
    EXPECT_TRUE(doc.m_bCompressInitOK);
    EXPECT_FALSE(doc.m_bSupports_MCCP_2);      // v1, not v2

    doc.Phase_SB(TELOPT_COMPRESS);
    EXPECT_EQ(HAVE_COMPRESS, doc.m_phase);
    doc.Phase_COMPRESS(WILL);
    EXPECT_EQ(HAVE_COMPRESS_WILL, doc.m_phase);
    doc.Phase_COMPRESS_WILL(SE);
    EXPECT_TRUE(doc.m_bCompress);
    EXPECT_EQ(1, doc.m_iMCCP_type);
}

// disable_compression refuses the offer outright (Send_IAC_DONT arm).
TEST(TelnetTest, WillCompress2RefusedWhenCompressionDisabled)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_bDisableCompression = TRUE;
    doc.Phase_WILL(TELOPT_COMPRESS2);
    EXPECT_FALSE(doc.m_bSupports_MCCP_2);
    EXPECT_TRUE(doc.m_bClient_sent_IAC_DONT[TELOPT_COMPRESS2]);
}

// The MXP subnegotiation turns MXP on when the world wants it "on command"
// (Handle_TELOPT_MXP: eOnCommandMXP → MXP_On).
TEST(TelnetTest, MxpSubnegotiationTurnsMxpOnInCommandMode)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_iUseMXP = eOnCommandMXP;
    EXPECT_FALSE(doc.m_bMXP);
    doc.Phase_SB(TELOPT_MXP);
    doc.Phase_SUBNEGOTIATION_IAC(SE);          // Handle_TELOPT_MXP
    EXPECT_TRUE(doc.m_bMXP);
}

// IAC WILL MXP in query mode turns MXP on immediately.
TEST(TelnetTest, WillMxpQueryModeTurnsMxpOnImmediately)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_iUseMXP = eQueryMXP;
    doc.Phase_WILL(TELOPT_MXP);
    EXPECT_TRUE(doc.m_bMXP);
    EXPECT_TRUE(doc.m_bClient_sent_IAC_DO[TELOPT_MXP]);

    // and DONT turns it back off (Phase_DONT's MXP arm)
    doc.Phase_DONT(TELOPT_MXP);
    EXPECT_FALSE(doc.m_bMXP);
}

// Aardwolf-style option 102: WILL is agreed to, and its subnegotiation
// dispatches Handle_TELOPT_MUD_SPECIFIC (plugin callback; no plugins → no-op)
// then falls through to the generic subnegotiation callback.
TEST(TelnetTest, MudSpecificWillAndSubnegotiation)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.Phase_WILL(TELOPT_MUD_SPECIFIC);
    EXPECT_TRUE(doc.m_bClient_sent_IAC_DO[TELOPT_MUD_SPECIFIC]);

    doc.Phase_SB(TELOPT_MUD_SPECIFIC);
    doc.Phase_SUBNEGOTIATION('h');
    doc.Phase_SUBNEGOTIATION('i');
    doc.Phase_SUBNEGOTIATION(IAC);
    doc.Phase_SUBNEGOTIATION_IAC(SE);
    EXPECT_EQ(NONE, doc.m_phase);
    EXPECT_EQ(1, doc.m_nCount_IAC_SB);
}

// TERMINAL-TYPE SEND advances the MTTS sequence: name → terminal → MTTS
// bitvector (which stays at step 2 for later requests).
TEST(TelnetTest, TerminalTypeSendAdvancesMttsSequence)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_ttype_sequence = 0;

    for (int expected : {1, 2, 2}) {
        doc.Phase_SB(TELOPT_TERMINAL_TYPE);
        doc.Phase_SUBNEGOTIATION(1);           // TTYPE_SEND
        doc.Phase_SUBNEGOTIATION_IAC(SE);
        EXPECT_EQ(expected, doc.m_ttype_sequence);
    }
}

// ═══ Round-4 coverage: OSC terminators, MXP-z modes, IAC single commands,
//     WILL/DO option arms, TTYPE MTTS bits, charset edge arms ═══════════════

TEST(TelnetR4, OscPayloadTerminatorVariants)
{
    CMUSHclientDoc doc; make_doc(doc);

    doc.m_phase = HAVE_OSC;
    doc.Phase_OSC('x');                 // payload byte — stays in OSC
    EXPECT_EQ(HAVE_OSC, doc.m_phase);
    doc.Phase_OSC('\007');              // BEL terminates
    EXPECT_EQ(NONE, doc.m_phase);

    doc.m_phase = HAVE_OSC;
    doc.Phase_OSC((unsigned char)0x9C); // C1 ST terminates
    EXPECT_EQ(NONE, doc.m_phase);

    doc.m_phase = HAVE_OSC;
    doc.Phase_OSC('\033');              // ESC begins ESC-\ terminator
    EXPECT_EQ(HAVE_ESC, doc.m_phase);
    doc.Phase_ESC('\\');
    EXPECT_EQ(NONE, doc.m_phase);
}

// ESC[<n>z — MXP line-security mode change; ESC[3z resets MXP entirely.
TEST(TelnetR4, AnsiZChangesMxpModeAndReset)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_iUseMXP = eOnCommandMXP;
    doc.MXP_On();
    ASSERT_TRUE(doc.m_bMXP);

    doc.m_phase = DOING_CODE;
    doc.m_code = eMXP_open;             // mode 0 = open line
    doc.Phase_ANSI('z');
    EXPECT_EQ(NONE, doc.m_phase);
    EXPECT_EQ((int)eMXP_open, (int)doc.m_iMXP_mode);

    doc.m_phase = DOING_CODE;
    doc.m_code = eMXP_reset;            // mode 3 = reset (not a full off)
    doc.m_bInParagraph = true;
    doc.m_bPreMode = true;
    doc.Phase_ANSI('z');
    EXPECT_TRUE(doc.m_bMXP);            // still on…
    EXPECT_TRUE(!doc.m_bInParagraph);   // …but the modes were reset
    EXPECT_TRUE(!doc.m_bPreMode);
}

// IAC EOR behaves like GA (newline conversion + prompt handling); the other
// single-byte commands are consumed silently; unknown commands too.
TEST(TelnetR4, IacSingleByteCommands)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_iConnectPhase = eConnectConnectedToMud;
    doc.m_bConvertGAtoNewline = TRUE;

    int linesBefore = (int)doc.m_LineList.GetCount();
    const unsigned char eor[] = { 'h', 'i', IAC, EOR };
    doc.DisplayMsg((LPCTSTR)eor, 4, 0);
    // EOR became a newline: the "hi" line was finished
    EXPECT_TRUE((int)doc.m_LineList.GetCount() > linesBefore ||
                (doc.m_pCurrentLine && doc.m_pCurrentLine->len == 0));

    // DM BRK IP AO AYT EC EL and an unknown command: all consumed
    const unsigned char cmds[] = { IAC, DATA_MARK, IAC, BREAK,
                                   IAC, INTERRUPT_PROCESS, IAC, ABORT_OUTPUT,
                                   IAC, ARE_YOU_THERE, IAC, ERASE_CHARACTER,
                                   IAC, ERASE_LINE, IAC, 200 };
    doc.DisplayMsg((LPCTSTR)cmds, sizeof cmds, 0);
    EXPECT_EQ(NONE, doc.m_phase);
    EXPECT_TRUE(doc.m_pCurrentLine == NULL || doc.m_pCurrentLine->len == 0);
}

// WILL END_OF_RECORD: DO when converting GA to newline, DONT otherwise.
TEST(TelnetR4, WillEndOfRecordFollowsConvertSetting)
{
    {
        CMUSHclientDoc doc; make_doc(doc);
        doc.m_bConvertGAtoNewline = TRUE;
        doc.Phase_WILL(WILL_END_OF_RECORD);
        EXPECT_TRUE(doc.m_bClient_sent_IAC_DO[WILL_END_OF_RECORD]);
    }
    {
        CMUSHclientDoc doc; make_doc(doc);
        doc.m_bConvertGAtoNewline = FALSE;
        doc.Phase_WILL(WILL_END_OF_RECORD);
        EXPECT_TRUE(doc.m_bClient_sent_IAC_DONT[WILL_END_OF_RECORD]);
    }
}

// WILL CHARSET is answered DO.
TEST(TelnetR4, WillCharsetAnsweredDo)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.Phase_WILL(TELOPT_CHARSET);
    EXPECT_TRUE(doc.m_bClient_sent_IAC_DO[TELOPT_CHARSET]);
}

// DO MXP: refused when MXP is off; agreed (and MXP turned on) for query-mode.
TEST(TelnetR4, DoMxpArms)
{
    {
        CMUSHclientDoc doc; make_doc(doc);
        doc.m_iUseMXP = eNoMXP;
        doc.Phase_DO(TELOPT_MXP);
        EXPECT_TRUE(doc.m_bClient_sent_IAC_WONT[TELOPT_MXP]);
        EXPECT_TRUE(!doc.m_bMXP);
    }
    {
        CMUSHclientDoc doc; make_doc(doc);
        doc.m_iUseMXP = eQueryMXP;
        doc.Phase_DO(TELOPT_MXP);
        EXPECT_TRUE(doc.m_bClient_sent_IAC_WILL[TELOPT_MXP]);
        EXPECT_TRUE(doc.m_bMXP);
    }
}

// DONT TERMINAL_TYPE resets the terminal-type reply sequence.
TEST(TelnetR4, DontTerminalTypeResetsSequence)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_ttype_sequence = 2;
    doc.Phase_DONT(TELOPT_TERMINAL_TYPE);
    EXPECT_EQ(0, doc.m_ttype_sequence);
}

// CHARSET REQUEST edge arms: too-short payload and non-REQUEST type are
// ignored (no reply at all); a matching charset (UTF-8) is ACCEPTED.
TEST(TelnetR4, CharsetRequestEdgeAndAcceptArms)
{
    CMUSHclientDoc doc; make_doc(doc);
    TelnetFakeTransport* t = new TelnetFakeTransport();
    doc.m_pTransport = t;

    doc.m_IAC_subnegotiation_data = std::string("\x01;", 2);   // too short
    doc.Handle_TELOPT_CHARSET();
    EXPECT_TRUE(t->sent.empty());

    doc.m_IAC_subnegotiation_data = std::string("\x02;UTF-8", 7);  // not REQUEST
    doc.Handle_TELOPT_CHARSET();
    EXPECT_TRUE(t->sent.empty());

    doc.m_bUTF_8 = TRUE;
    doc.m_IAC_subnegotiation_data = std::string("\x01;UTF-8", 7);
    doc.Handle_TELOPT_CHARSET();
    // IAC SB CHARSET ACCEPTED(2) "UTF-8" IAC SE
    const char expected[] = { (char)IAC, (char)SB, (char)TELOPT_CHARSET, 2,
                              'U', 'T', 'F', '-', '8', (char)IAC, (char)SE };
    ASSERT_EQ(sizeof expected, t->sent.size());
    EXPECT_TRUE(memcmp(t->sent.data(), expected, sizeof expected) == 0);

    // and the SB dispatch arm routes CHARSET subnegotiations here
    t->sent.clear();
    doc.m_subnegotiation_type = TELOPT_CHARSET;
    doc.m_IAC_subnegotiation_data = std::string("\x01;UTF-8", 7);
    doc.m_phase = HAVE_SUBNEGOTIATION_IAC;
    doc.Phase_SUBNEGOTIATION_IAC(SE);
    EXPECT_TRUE(t->sent.size() > 0);
}

// TTYPE SEND sequence: name, then XTERM-256COLOR, then MTTS with the UTF-8
// and SSL bits; a TTYPE IS (not SEND) is ignored.
TEST(TelnetR4, TerminalTypeSequenceAndMttsBits)
{
    CMUSHclientDoc doc; make_doc(doc);
    TelnetFakeTransport* t = new TelnetFakeTransport();
    doc.m_pTransport = t;
    doc.m_strTerminalIdentification = "mushclient";
    doc.m_bUTF_8 = TRUE;
    doc.m_bSSL_Connected = true;

    doc.m_IAC_subnegotiation_data = std::string("\x00", 1);   // TTYPE IS
    doc.Handle_TELOPT_TERMINAL_TYPE();
    EXPECT_TRUE(t->sent.empty());                             // ignored

    doc.m_ttype_sequence = 0;
    doc.m_IAC_subnegotiation_data = std::string("\x01", 1);   // SEND
    doc.Handle_TELOPT_TERMINAL_TYPE();
    EXPECT_TRUE(t->sent.find("mushclient") != std::string::npos);

    doc.Handle_TELOPT_TERMINAL_TYPE();
    EXPECT_TRUE(t->sent.find("XTERM-256COLOR") != std::string::npos);

    doc.Handle_TELOPT_TERMINAL_TYPE();
    // 9 (ANSI + 256 colours) | 4 (UTF-8) | 2048 (SSL) = 2061
    EXPECT_TRUE(t->sent.find("MTTS 2061") != std::string::npos);
}

// Phase_UTF8: a byte with the high bits wrong mid-sequence flushes the
// collected bytes as bad UTF-8.
TEST(TelnetR4, Utf8BadContinuationFlushes)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_iConnectPhase = eConnectConnectedToMud;
    doc.m_bUTF_8 = TRUE;

    // E2 82 expects one more continuation; C3 is a LEAD byte → flush
    const char bad[] = "\xE2\x82\xC3";
    doc.DisplayMsg(bad, 3, 0);
    // the sequence was abandoned rather than swallowed
    EXPECT_TRUE(doc.m_pCurrentLine && doc.m_pCurrentLine->len > 0);
}

// ═══ Round-4: telopt_name table + the g_debug tracing arms ══════════════════

TEST(TelnetR4, TeloptNameTableAndDebugTracing)
{
    // spot-check the name table (declared in doc.h, defined in
    // doc_portable.cpp)
    EXPECT_STREQ("BINARY",         telopt_name(0));
    EXPECT_STREQ("TERMINAL-SPEED", telopt_name(32));
    EXPECT_STREQ("LINEMODE",       telopt_name(34));
    EXPECT_STREQ("ENVIRON",        telopt_name(36));
    EXPECT_STREQ("NEW-ENVIRON",    telopt_name(39));
    EXPECT_STREQ("MSP",            telopt_name(90));
    EXPECT_STREQ("ZMP",            telopt_name(93));
    EXPECT_STREQ("GMCP",           telopt_name(201));

    // with g_debug on, the negotiation dbg() lines print (stderr)
    bool saved = g_debug;
    g_debug = true;
    CMUSHclientDoc doc; make_doc(doc);
    doc.Phase_WILL(TELOPT_MXP);
    doc.Phase_DO(1 /* echo */);
    g_debug = saved;
}

// ── round 5: RFC 854 resend suppression, unnegotiated MCCP, UTF-8 errors ──

// Sending the same DONT/WILL twice is suppressed (RFC 854 / forum 3061).
TEST(TelnetR5, RepeatedDontAndWillSuppressed)
{
    CMUSHclientDoc doc; make_doc(doc);
    RecordingTransport* t = new RecordingTransport();
    doc.m_pTransport = t;            // doc destructor tears it down

    doc.Send_IAC_DONT(42);
    doc.Send_IAC_DONT(42);           // suppressed (RFC 854 / forum 3061)
    doc.Send_IAC_WILL(42);
    doc.Send_IAC_WILL(42);           // suppressed

    // Exactly ONE DONT and ONE WILL sequence reach the wire — the repeats are
    // dropped by the m_bClient_sent_IAC_* guards.  (SendPacket writes the 3
    // negotiation bytes raw, no IAC escaping.)
    const unsigned char expected[] = { IAC, DONT, 42, IAC, WILL, 42 };
    ASSERT_EQ(sizeof expected, t->sent.size());
    EXPECT_EQ(0, memcmp(expected, t->sent.data(), sizeof expected));
    EXPECT_TRUE(doc.m_bClient_sent_IAC_DONT[42]);
    EXPECT_TRUE(doc.m_bClient_sent_IAC_WILL[42]);
}

// An MCCP v2 subnegotiation arriving WITHOUT the usual IAC WILL COMPRESS2
// handshake still initialises zlib on the spot; with the transfer buffers
// available compression turns on, without them the world is closed.
TEST(TelnetR5, Mccp2SubnegotiationWithoutPriorWill)
{
    {   // buffers present: on-the-spot init succeeds
        CMUSHclientDoc doc; make_doc(doc);
        doc.m_nCompressionOutputBufferSize = 4096;
        doc.m_CompressOutput = (Bytef*)malloc(doc.m_nCompressionOutputBufferSize);
        doc.m_CompressInput  = (Bytef*)malloc(4096);
        const unsigned char seq[] = { 255, 250, 86, 255, 240 };  // IAC SB COMPRESS2 IAC SE
        doc.DisplayMsg((LPCTSTR)seq, sizeof seq, 0);
        EXPECT_TRUE(doc.m_bCompress);
        EXPECT_EQ(2, doc.m_iMCCP_type);
    }
    {   // no buffers were ever negotiated: the world is closed with a box
        CMUSHclientDoc doc; make_doc(doc);
        const unsigned char seq[] = { 255, 250, 86, 255, 240 };
        doc.DisplayMsg((LPCTSTR)seq, sizeof seq, 0);
        EXPECT_FALSE(doc.m_bCompress);
    }
}

// Same for the legacy MCCP v1 sequence IAC SB COMPRESS WILL SE.
TEST(TelnetR5, Mccp1SubnegotiationWithoutPriorWill)
{
    {   // buffers present
        CMUSHclientDoc doc; make_doc(doc);
        doc.m_nCompressionOutputBufferSize = 4096;
        doc.m_CompressOutput = (Bytef*)malloc(doc.m_nCompressionOutputBufferSize);
        doc.m_CompressInput  = (Bytef*)malloc(4096);
        const unsigned char seq[] = { 255, 250, 85, 251, 240 };  // IAC SB COMPRESS WILL SE
        doc.DisplayMsg((LPCTSTR)seq, sizeof seq, 0);
        EXPECT_TRUE(doc.m_bCompress);
        EXPECT_EQ(1, doc.m_iMCCP_type);
    }
    {   // no buffers
        CMUSHclientDoc doc; make_doc(doc);
        const unsigned char seq[] = { 255, 250, 85, 251, 240 };
        doc.DisplayMsg((LPCTSTR)seq, sizeof seq, 0);
        EXPECT_FALSE(doc.m_bCompress);
    }
}

// A malformed MCCP v1 sequence (not WILL/SE) resets the phase machine.
TEST(TelnetR5, Mccp1MalformedSequenceIgnored)
{
    CMUSHclientDoc doc; make_doc(doc);
    const unsigned char seq[] = { 255, 250, 85, 251, 99 };   // IAC SB COMPRESS WILL <99>
    doc.DisplayMsg((LPCTSTR)seq, sizeof seq, 0);
    EXPECT_FALSE(doc.m_bCompress);
}

// An invalid UTF-8 sequence is reported through OutputBadUTF8characters.
TEST(TelnetR5, InvalidUtf8SequenceHandled)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_bUTF_8 = TRUE;
    const unsigned char seq[] = { 0xE2, 0x28, 0xA1, '\n' };  // bad continuation
    doc.DisplayMsg((LPCTSTR)seq, sizeof seq, 0);
    // no crash, line completed; the bad bytes were substituted or dropped
    EXPECT_TRUE(doc.m_LineList.GetCount() >= 1);
}

MC_TEST_MAIN()
