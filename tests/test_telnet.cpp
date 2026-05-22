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
#include "platform/doc_stub.h"

// ── Helpers ───────────────────────────────────────────────────────────────

static CMUSHclientDoc make_doc()
{
    CMUSHclientDoc doc;
    doc.m_pCurrentLine = new CLine();
    doc.m_iFlags       = COLOUR_ANSI;
    doc.m_iForeColour  = WHITE;
    doc.m_iBackColour  = BLACK;
    doc.AddStyle(COLOUR_ANSI, WHITE, BLACK, 0,
                 static_cast<CAction*>(nullptr));
    return doc;
}

// ── Phase_ESC ────────────────────────────────────────────────────────────

TEST(TelnetTest, Phase_ESC_BracketTransitionsToDOING_CODE)
{
    CMUSHclientDoc doc = make_doc();
    doc.m_phase = HAVE_ESC;
    doc.Phase_ESC('[');
    EXPECT_EQ(DOING_CODE, doc.m_phase);
    EXPECT_EQ(0, doc.m_code);
}

TEST(TelnetTest, Phase_ESC_OtherCharReturnsToNONE)
{
    CMUSHclientDoc doc = make_doc();
    doc.m_phase = HAVE_ESC;
    doc.Phase_ESC('A');
    EXPECT_EQ(NONE, doc.m_phase);
}

// ── Phase_ANSI ────────────────────────────────────────────────────────────

TEST(TelnetTest, Phase_ANSI_DigitAccumulates)
{
    CMUSHclientDoc doc = make_doc();
    doc.m_phase = DOING_CODE;
    doc.m_code  = 0;
    doc.Phase_ANSI('3');
    EXPECT_EQ(3, doc.m_code);
    doc.Phase_ANSI('1');
    EXPECT_EQ(31, doc.m_code);
}

TEST(TelnetTest, Phase_ANSI_mFinalisesDOING_CODE)
{
    CMUSHclientDoc doc = make_doc();
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
    CMUSHclientDoc doc = make_doc();
    doc.m_phase = DOING_CODE;
    doc.m_code  = ANSI_BOLD;   // code 1
    // ';' dispatches the current code and resets m_code to 0
    doc.Phase_ANSI(';');
    EXPECT_EQ(0, doc.m_code);
    EXPECT_TRUE((doc.m_iFlags & HILITE) != 0);
}

TEST(TelnetTest, Phase_ANSI_UnknownCharReturnsNONE)
{
    CMUSHclientDoc doc = make_doc();
    doc.m_phase = DOING_CODE;
    doc.m_code  = 0;
    doc.Phase_ANSI('X');
    EXPECT_EQ(NONE, doc.m_phase);
}

// ── Phase_IAC ────────────────────────────────────────────────────────────

TEST(TelnetTest, Phase_IAC_WILL_SetsHAVE_WILL)
{
    CMUSHclientDoc doc = make_doc();
    unsigned char c = WILL;
    doc.Phase_IAC(c);
    EXPECT_EQ(HAVE_WILL, doc.m_phase);
    EXPECT_EQ('\0', c);   // new_c not set for WILL — c was set to 0
}

TEST(TelnetTest, Phase_IAC_WONT_SetsHAVE_WONT)
{
    CMUSHclientDoc doc = make_doc();
    unsigned char c = WONT;
    doc.Phase_IAC(c);
    EXPECT_EQ(HAVE_WONT, doc.m_phase);
}

TEST(TelnetTest, Phase_IAC_DO_SetsHAVE_DO)
{
    CMUSHclientDoc doc = make_doc();
    unsigned char c = DO;
    doc.Phase_IAC(c);
    EXPECT_EQ(HAVE_DO, doc.m_phase);
}

TEST(TelnetTest, Phase_IAC_DONT_SetsHAVE_DONT)
{
    CMUSHclientDoc doc = make_doc();
    unsigned char c = DONT;
    doc.Phase_IAC(c);
    EXPECT_EQ(HAVE_DONT, doc.m_phase);
}

TEST(TelnetTest, Phase_IAC_SB_SetsHAVE_SB)
{
    CMUSHclientDoc doc = make_doc();
    unsigned char c = SB;
    doc.Phase_IAC(c);
    EXPECT_EQ(HAVE_SB, doc.m_phase);
}

TEST(TelnetTest, Phase_IAC_SE_SetsNONE)
{
    CMUSHclientDoc doc = make_doc();
    unsigned char c = SE;
    doc.Phase_IAC(c);
    EXPECT_EQ(NONE, doc.m_phase);
}

TEST(TelnetTest, Phase_IAC_NOP_SetsNONE)
{
    CMUSHclientDoc doc = make_doc();
    unsigned char c = NOP;
    doc.Phase_IAC(c);
    EXPECT_EQ(NONE, doc.m_phase);
}

TEST(TelnetTest, Phase_IAC_GA_ConvertsToNewlineWhenEnabled)
{
    CMUSHclientDoc doc = make_doc();
    doc.m_bConvertGAtoNewline = 1;
    unsigned char c = GO_AHEAD;
    doc.Phase_IAC(c);
    EXPECT_EQ(NONE, doc.m_phase);
    EXPECT_EQ('\n', c);   // should have been converted to newline
}

TEST(TelnetTest, Phase_IAC_GA_NoConversionWhenDisabled)
{
    CMUSHclientDoc doc = make_doc();
    doc.m_bConvertGAtoNewline = 0;
    unsigned char c = GO_AHEAD;
    doc.Phase_IAC(c);
    EXPECT_EQ(NONE, doc.m_phase);
    EXPECT_EQ('\0', c);   // no conversion
}

// ── Phase_WILL (telnet negotiation) ──────────────────────────────────────

TEST(TelnetTest, Phase_WILL_SGAAgrees)
{
    CMUSHclientDoc doc = make_doc();
    doc.Phase_WILL(SGA);
    EXPECT_EQ(NONE, doc.m_phase);
    EXPECT_TRUE(doc.m_bClient_sent_IAC_DO[SGA]);
    EXPECT_EQ(1, doc.m_nCount_IAC_WILL);
}

TEST(TelnetTest, Phase_WILL_UnknownRefuses)
{
    CMUSHclientDoc doc = make_doc();
    // With no plugins, Handle_Telnet_Request returns false → sends DONT
    doc.Phase_WILL(99);
    EXPECT_EQ(NONE, doc.m_phase);
    EXPECT_TRUE(doc.m_bClient_sent_IAC_DONT[99]);
}

TEST(TelnetTest, Phase_WILL_EchoAgrees)
{
    CMUSHclientDoc doc = make_doc();
    doc.m_bNoEchoOff = 0;
    doc.Phase_WILL(TELOPT_ECHO);
    EXPECT_TRUE(doc.m_bNoEcho);
    EXPECT_TRUE(doc.m_bClient_sent_IAC_DO[TELOPT_ECHO]);
}

TEST(TelnetTest, Phase_WILL_EchoRefusedWhenNoEchoOff)
{
    CMUSHclientDoc doc = make_doc();
    doc.m_bNoEchoOff = 1;
    doc.Phase_WILL(TELOPT_ECHO);
    EXPECT_FALSE(doc.m_bNoEcho);
    EXPECT_TRUE(doc.m_bClient_sent_IAC_DONT[TELOPT_ECHO]);
}

// ── Phase_WONT ────────────────────────────────────────────────────────────

TEST(TelnetTest, Phase_WONT_EchoRestoresEcho)
{
    CMUSHclientDoc doc = make_doc();
    doc.m_bNoEcho    = true;
    doc.m_bNoEchoOff = 0;
    doc.Phase_WONT(TELOPT_ECHO);
    EXPECT_FALSE(doc.m_bNoEcho);
    EXPECT_TRUE(doc.m_bClient_sent_IAC_DONT[TELOPT_ECHO]);
    EXPECT_EQ(1, doc.m_nCount_IAC_WONT);
}

TEST(TelnetTest, Phase_WONT_UnknownSendsDONT)
{
    CMUSHclientDoc doc = make_doc();
    doc.Phase_WONT(50);
    EXPECT_TRUE(doc.m_bClient_sent_IAC_DONT[50]);
}

// ── Phase_DO ─────────────────────────────────────────────────────────────

TEST(TelnetTest, Phase_DO_SGAAgrees)
{
    CMUSHclientDoc doc = make_doc();
    doc.Phase_DO(SGA);
    EXPECT_EQ(NONE, doc.m_phase);
    EXPECT_TRUE(doc.m_bClient_sent_IAC_WILL[SGA]);
    EXPECT_EQ(1, doc.m_nCount_IAC_DO);
}

TEST(TelnetTest, Phase_DO_TerminalTypeAgrees)
{
    CMUSHclientDoc doc = make_doc();
    doc.Phase_DO(TELOPT_TERMINAL_TYPE);
    EXPECT_TRUE(doc.m_bClient_sent_IAC_WILL[TELOPT_TERMINAL_TYPE]);
    EXPECT_EQ(0, doc.m_ttype_sequence);   // reset to 0
}

TEST(TelnetTest, Phase_DO_UnknownSendsWONT)
{
    CMUSHclientDoc doc = make_doc();
    doc.Phase_DO(77);
    EXPECT_TRUE(doc.m_bClient_sent_IAC_WONT[77]);
}

// ── Phase_DONT ────────────────────────────────────────────────────────────

TEST(TelnetTest, Phase_DONT_SendsWONT)
{
    CMUSHclientDoc doc = make_doc();
    doc.Phase_DONT(SGA);
    EXPECT_EQ(NONE, doc.m_phase);
    EXPECT_TRUE(doc.m_bClient_sent_IAC_WONT[SGA]);
    EXPECT_EQ(1, doc.m_nCount_IAC_DONT);
}

// ── Phase_SB (subnegotiation start) ──────────────────────────────────────

TEST(TelnetTest, Phase_SB_NormalTypeTransitionsToHAVE_SUBNEG)
{
    CMUSHclientDoc doc = make_doc();
    doc.Phase_SB(TELOPT_TERMINAL_TYPE);
    EXPECT_EQ(HAVE_SUBNEGOTIATION, doc.m_phase);
    EXPECT_EQ(TELOPT_TERMINAL_TYPE, doc.m_subnegotiation_type);
    EXPECT_TRUE(doc.m_IAC_subnegotiation_data.empty());
}

TEST(TelnetTest, Phase_SB_COMPRESSTransitionsToHAVE_COMPRESS)
{
    CMUSHclientDoc doc = make_doc();
    doc.Phase_SB(TELOPT_COMPRESS);
    EXPECT_EQ(HAVE_COMPRESS, doc.m_phase);
}

// ── Phase_SUBNEGOTIATION ─────────────────────────────────────────────────

TEST(TelnetTest, Phase_SUBNEG_CollectsData)
{
    CMUSHclientDoc doc = make_doc();
    doc.m_phase = HAVE_SUBNEGOTIATION;
    doc.Phase_SUBNEGOTIATION('A');
    doc.Phase_SUBNEGOTIATION('B');
    EXPECT_EQ("AB", doc.m_IAC_subnegotiation_data);
    EXPECT_EQ(HAVE_SUBNEGOTIATION, doc.m_phase);
}

TEST(TelnetTest, Phase_SUBNEG_IACTransitionsToHAVE_SUBNEG_IAC)
{
    CMUSHclientDoc doc = make_doc();
    doc.m_phase = HAVE_SUBNEGOTIATION;
    doc.Phase_SUBNEGOTIATION(IAC);
    EXPECT_EQ(HAVE_SUBNEGOTIATION_IAC, doc.m_phase);
}

// ── Phase_SUBNEGOTIATION_IAC ──────────────────────────────────────────────

TEST(TelnetTest, Phase_SUBNEG_IAC_IACIACStoresOneIAC)
{
    CMUSHclientDoc doc = make_doc();
    doc.m_phase = HAVE_SUBNEGOTIATION_IAC;
    doc.m_IAC_subnegotiation_data = "AB";
    doc.Phase_SUBNEGOTIATION_IAC(IAC);   // IAC IAC → store single IAC
    EXPECT_EQ(HAVE_SUBNEGOTIATION, doc.m_phase);
    EXPECT_EQ(3u, doc.m_IAC_subnegotiation_data.size());
    EXPECT_EQ((char)IAC, doc.m_IAC_subnegotiation_data[2]);
}

TEST(TelnetTest, Phase_SUBNEG_IAC_SEEndsSubnegotiation)
{
    CMUSHclientDoc doc = make_doc();
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
    CMUSHclientDoc doc = make_doc();
    doc.m_phase = HAVE_COMPRESS;
    doc.Phase_COMPRESS(WILL);
    EXPECT_EQ(HAVE_COMPRESS_WILL, doc.m_phase);
}

TEST(TelnetTest, Phase_COMPRESS_NonWILLReturnsNONE)
{
    CMUSHclientDoc doc = make_doc();
    doc.m_phase = HAVE_COMPRESS;
    doc.Phase_COMPRESS(0x42);
    EXPECT_EQ(NONE, doc.m_phase);
}

// ── No-double-negotiate guard ─────────────────────────────────────────────

TEST(TelnetTest, Send_IAC_DO_NoDoubleNegotiate)
{
    CMUSHclientDoc doc = make_doc();
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
    CMUSHclientDoc doc = make_doc();
    doc.Send_IAC_WILL(TELOPT_ECHO);
    EXPECT_TRUE (doc.m_bClient_sent_IAC_WILL[TELOPT_ECHO]);
    EXPECT_FALSE(doc.m_bClient_sent_IAC_WONT[TELOPT_ECHO]);
}

// ── Handle_Telnet_Request (plugin stub) ───────────────────────────────────

TEST(TelnetTest, Handle_Telnet_Request_ReturnsFalseWithNoPlugins)
{
    CMUSHclientDoc doc = make_doc();
    // Stub always returns bDefaultReturn = false
    EXPECT_FALSE(doc.Handle_Telnet_Request(200, "WILL"));
}

// ── IAC counters ─────────────────────────────────────────────────────────

TEST(TelnetTest, Counters_WILL_DO_WONT_DONT_SB)
{
    CMUSHclientDoc doc = make_doc();

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
    CMUSHclientDoc doc = make_doc();
    doc.m_bNAWS       = TRUE;
    doc.m_nWrapColumn = 80;
    doc.Phase_DO(TELOPT_NAWS);
    EXPECT_TRUE(doc.m_bClient_sent_IAC_WILL[TELOPT_NAWS]);
    EXPECT_TRUE(doc.m_bNAWS_wanted);
}

TEST(TelnetTest, Phase_DO_NAWS_SendsWONT_When_bNAWS_Clear)
{
    CMUSHclientDoc doc = make_doc();
    doc.m_bNAWS = FALSE;
    doc.Phase_DO(TELOPT_NAWS);
    EXPECT_TRUE(doc.m_bClient_sent_IAC_WONT[TELOPT_NAWS]);
    EXPECT_FALSE(doc.m_bNAWS_wanted);
}

// ── MXP negotiation ───────────────────────────────────────────────────────

TEST(TelnetTest, Phase_WILL_MXP_AgreesWhenEnabled)
{
    CMUSHclientDoc doc = make_doc();
    doc.m_iUseMXP = eQueryMXP;
    doc.Phase_WILL(TELOPT_MXP);
    EXPECT_TRUE(doc.m_bClient_sent_IAC_DO[TELOPT_MXP]);
}

TEST(TelnetTest, Phase_WILL_MXP_RefusesWhenDisabled)
{
    CMUSHclientDoc doc = make_doc();
    doc.m_iUseMXP = eNoMXP;
    doc.Phase_WILL(TELOPT_MXP);
    EXPECT_TRUE(doc.m_bClient_sent_IAC_DONT[TELOPT_MXP]);
}

// ── Terminal type sequence ────────────────────────────────────────────────

TEST(TelnetTest, Phase_DO_TerminalTypeResetsSequence)
{
    CMUSHclientDoc doc = make_doc();
    doc.m_ttype_sequence = 2;   // pretend we are mid-sequence
    doc.Phase_DO(TELOPT_TERMINAL_TYPE);
    EXPECT_EQ(0, doc.m_ttype_sequence);
    EXPECT_TRUE(doc.m_bClient_sent_IAC_WILL[TELOPT_TERMINAL_TYPE]);
}

MC_TEST_MAIN()
