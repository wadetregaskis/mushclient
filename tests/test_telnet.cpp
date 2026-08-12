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
#include <string>

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

// ── IAC GA / EOR prompt marking ──────────────────────────────────────────
//
// A GA or EOR means "go ahead, it is your turn": the text that has arrived
// since the last line break IS the server's prompt.  MarkPromptLine stamps
// that line with the PROMPT flag so "clear everything above the prompt" can
// find it later, and records its number in m_last_prompt_line.
//
// These drive the real display pipeline rather than poking m_pCurrentLine,
// because WHICH line gets stamped depends on whether the server terminated
// the prompt before sending the GA -- the two conventions in the wild.

// A doc set up to receive MUD output, as test_display's fixture does: no
// pre-made current line, so DisplayMsg builds and links the line list itself.
static void make_stream_doc(CMUSHclientDoc& doc)
{
    doc.m_iConnectPhase = eConnectConnectedToMud;
}

static void mud(CMUSHclientDoc& doc, const char* s)
{
    doc.DisplayMsg(s, (int)strlen(s), 0);
}

// Drive one IAC <cmd>, then feed on whatever character Phase_IAC substituted
// -- which is how the real caller behaves, and is what makes the
// convert-GA-to-newline arm actually terminate the line.
static void send_iac(CMUSHclientDoc& doc, unsigned char cmd)
{
    unsigned char c = cmd;
    doc.Phase_IAC(c);
    if (c) {
        char ch = (char) c;
        doc.DisplayMsg(&ch, 1, 0);
    }
}

// The common convention: the prompt is deliberately NOT terminated, so it is
// still the in-progress line when the GA arrives.
TEST(TelnetTest, IAC_GA_StampsTheUnterminatedPromptLine)
{
    CMUSHclientDoc doc; make_stream_doc(doc);
    doc.m_bConvertGAtoNewline = 0;
    mud(doc, "You are in a forest.\nHP:100 MP:50 > ");

    const long lines_before = doc.m_total_lines;
    send_iac(doc, GO_AHEAD);

    ASSERT_NE(nullptr, doc.m_pCurrentLine);
    EXPECT_TRUE(std::string(doc.m_pCurrentLine->text, doc.m_pCurrentLine->len)
                == "HP:100 MP:50 > ");
    EXPECT_TRUE((doc.m_pCurrentLine->flags & PROMPT) != 0);
    EXPECT_EQ(doc.m_pCurrentLine->m_nLineNumber, doc.m_last_prompt_line);

    // world.GetInfo(289) keeps its upstream meaning: the line number current
    // when the GA was seen.  Here that is the same line.
    EXPECT_EQ(lines_before, doc.m_last_line_with_IAC_GA);
    EXPECT_EQ(doc.m_last_prompt_line, doc.m_last_line_with_IAC_GA);
}

// The other convention: EOR as an end-of-record marker, sent AFTER the
// newline.  The prompt is already committed and the in-progress line is a
// fresh empty one, so the stamp has to go one line back -- and here
// m_last_prompt_line and m_last_line_with_IAC_GA genuinely differ.
TEST(TelnetTest, IAC_EOR_StampsTheLastCommittedLineWhenPromptWasTerminated)
{
    CMUSHclientDoc doc; make_stream_doc(doc);
    doc.m_bConvertGAtoNewline = 0;
    mud(doc, "You are in a forest.\nHP:100 MP:50 >\n");

    ASSERT_NE(nullptr, doc.m_pCurrentLine);
    ASSERT_EQ(0, doc.m_pCurrentLine->len);      // fresh empty line

    send_iac(doc, EOR);

    // The empty in-progress line is NOT the prompt...
    EXPECT_TRUE((doc.m_pCurrentLine->flags & PROMPT) == 0);
    // ...the committed line before it is.
    POSITION pos = doc.m_LineList.GetTailPosition();
    doc.m_LineList.GetPrev(pos);                // skip the in-progress line
    ASSERT_NE(nullptr, pos);
    CLine* prompt = doc.m_LineList.GetAt(pos);
    EXPECT_TRUE(std::string(prompt->text, prompt->len) == "HP:100 MP:50 >");
    EXPECT_TRUE((prompt->flags & PROMPT) != 0);
    EXPECT_EQ(prompt->m_nLineNumber, doc.m_last_prompt_line);

    // The upstream field still names the line current at GA time, which is the
    // empty one -- one further on.  That difference is the whole reason
    // m_last_prompt_line exists.
    EXPECT_EQ(doc.m_pCurrentLine->m_nLineNumber, doc.m_last_line_with_IAC_GA);
    EXPECT_LT(doc.m_last_prompt_line, doc.m_last_line_with_IAC_GA);
}

// convert_ga_to_newline turns the GA into a line break.  The stamp must land
// on the prompt itself, before that break commits it -- not on the empty line
// the break creates.
TEST(TelnetTest, IAC_GA_ConvertedToNewlineStillStampsThePromptLine)
{
    CMUSHclientDoc doc; make_stream_doc(doc);
    doc.m_bConvertGAtoNewline = 1;
    mud(doc, "HP:100 MP:50 > ");

    send_iac(doc, GO_AHEAD);       // becomes '\n', committing the prompt

    POSITION pos = doc.m_LineList.GetTailPosition();
    doc.m_LineList.GetPrev(pos);   // the empty line the newline started
    ASSERT_NE(nullptr, pos);
    CLine* prompt = doc.m_LineList.GetAt(pos);
    EXPECT_TRUE(std::string(prompt->text, prompt->len) == "HP:100 MP:50 > ");
    EXPECT_TRUE((prompt->flags & PROMPT) != 0);
    EXPECT_EQ(prompt->m_nLineNumber, doc.m_last_prompt_line);
}

// Nothing has arrived at all: there is no prompt to remember, and in
// particular MarkPromptLine must not walk off an empty line list.
TEST(TelnetTest, IAC_GA_OnAnEmptyBufferMarksNothing)
{
    CMUSHclientDoc doc; make_stream_doc(doc);
    doc.m_bConvertGAtoNewline = 0;

    send_iac(doc, GO_AHEAD);

    EXPECT_EQ(0L, doc.m_last_prompt_line);
}

// A GA arriving where the buffer holds nothing but the empty in-progress line
// must not stamp that line -- an empty line is never a prompt.
TEST(TelnetTest, IAC_GA_DoesNotStampAnEmptyInProgressLine)
{
    CMUSHclientDoc doc; make_stream_doc(doc);
    doc.m_bConvertGAtoNewline = 0;
    mud(doc, "\n");                 // one empty committed line + the pending one

    send_iac(doc, GO_AHEAD);

    EXPECT_EQ(0L, doc.m_last_prompt_line);
    for (POSITION pos = doc.m_LineList.GetHeadPosition(); pos; )
        EXPECT_TRUE((doc.m_LineList.GetNext(pos)->flags & PROMPT) == 0);
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

// telnet_phases.cpp OutputBadUTF8characters: in a UTF-8 world a byte that is not
// valid UTF-8 is assumed to be ANSI and re-encoded AS UTF-8, so what lands in the
// line buffer is still well-formed.  Windows does this with
// MultiByteToWideChar(CP_THREAD_ACP) + WideCharToMultiByte(CP_UTF8); the port
// reproduces CP1252, that codepage on the Western Windows this was written for.
//
// The port used to pass the raw byte through, which left INVALID UTF-8 in the
// buffer for every downstream consumer to trip over -- the wx output pane
// decodes a coloured run all-or-nothing, so one stray byte blanked the entire
// run it landed in.
static std::string visible_line(CMUSHclientDoc& doc)
{
    return std::string(doc.m_pCurrentLine->text, doc.m_pCurrentLine->len);
}

// Well-formedness is the property that matters, so check it directly rather than
// only comparing bytes: a future change to the mapping still has to keep this.
static bool is_valid_utf8(const std::string& s)
{
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = (unsigned char) s[i];
        int extra;
        if      (c < 0x80)                  extra = 0;
        else if (c >= 0xC2 && c <= 0xDF)    extra = 1;
        else if (c >= 0xE0 && c <= 0xEF)    extra = 2;
        else if (c >= 0xF0 && c <= 0xF4)    extra = 3;
        else return false;                       // stray continuation / overlong
        if (i + extra >= s.size() && extra > 0)
            return false;                        // truncated
        for (int k = 1; k <= extra; k++)
            if (((unsigned char) s[i + k] & 0xC0) != 0x80) return false;
        i += extra + 1;
    }
    return true;
}

TEST(TelnetTest, BadUtf8ByteIsReEncodedAsValidUtf8)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_bUTF_8 = TRUE;
    const char seq[] = "ab\xC3Xcd";   // 0xC3 lead byte, 'X' is a bad continuation
    doc.DisplayMsg(seq, (int)strlen(seq), 0);
    EXPECT_EQ(NONE, doc.m_phase);

    const std::string visible = visible_line(doc);
    // 0xC3 -> U+00C3 (LATIN CAPITAL A WITH TILDE) -> C3 83; 'X' passes through.
    EXPECT_STREQ("ab\xC3\x83Xcd", visible.c_str());
    EXPECT_TRUE(is_valid_utf8(visible));
}

// A byte in 0x80-0x9F is CP1252's own assignment, NOT Latin-1: 0x80 is the euro
// sign, three bytes in UTF-8.  Getting this wrong is invisible in a byte-count
// test, so assert the actual code point.
TEST(TelnetTest, BadUtf8ByteInCp1252RangeUsesTheCp1252Assignment)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_bUTF_8 = TRUE;
    const char seq[] = "a\x80z";
    doc.DisplayMsg(seq, (int)strlen(seq), 0);

    const std::string visible = visible_line(doc);
    EXPECT_STREQ("a\xE2\x82\xACz", visible.c_str());   // U+20AC EURO SIGN
    EXPECT_TRUE(is_valid_utf8(visible));
}

// 0xA0-0xFF are Latin-1, i.e. the code point equals the byte.  This is the
// common case: a server that says UTF-8 and emits a stray Latin-1 accent.
TEST(TelnetTest, BadUtf8ByteAboveCp1252SpecialsIsLatin1)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_bUTF_8 = TRUE;
    const char seq[] = "caf\xE9!";
    doc.DisplayMsg(seq, (int)strlen(seq), 0);

    const std::string visible = visible_line(doc);
    EXPECT_STREQ("caf\xC3\xA9!", visible.c_str());     // U+00E9 e-acute
    EXPECT_TRUE(is_valid_utf8(visible));
}

// The control: a NON-UTF-8 world must not be touched.  Its bytes are decoded by
// the font charset at draw time, so re-encoding them here would double-encode.
TEST(TelnetTest, BadByteIsLeftAloneInANonUtf8World)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_bUTF_8 = FALSE;
    const char seq[] = "caf\xE9!";
    doc.DisplayMsg(seq, (int)strlen(seq), 0);

    EXPECT_STREQ("caf\xE9!", visible_line(doc).c_str());
}

// Valid UTF-8 must reach the buffer untouched -- the repair path must not fire.
TEST(TelnetTest, ValidUtf8PassesThroughUnchanged)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_bUTF_8 = TRUE;
    const char seq[] = "caf\xC3\xA9!";
    doc.DisplayMsg(seq, (int)strlen(seq), 0);

    EXPECT_STREQ("caf\xC3\xA9!", visible_line(doc).c_str());
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
    // "ANSI", not "XTERM-256COLOR": the latter also asserts mouse tracking and
    // the OSC colour palette, which this client does not have.  Same answer on
    // both platforms -- the port used to differ here.
    EXPECT_TRUE(t->sent.find("ANSI") != std::string::npos);
    EXPECT_TRUE(t->sent.find("XTERM-256COLOR") == std::string::npos);

    doc.Handle_TELOPT_TERMINAL_TYPE();
    // 265 (ANSI + 256 colours + TRUECOLOR) | 4 (UTF-8) | 2048 (SSL) = 2317.
    // The TRUECOLOR bit is what MUDs gate 24-bit output on, and this client
    // really does render it (tests/test_ansi.cpp drives ESC[38;2;r;g;b through
    // to COLOUR_RGB); the port used to drop the bit and get banded output.
    EXPECT_TRUE(t->sent.find("MTTS 2317") != std::string::npos);
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


// Phase_UTF8 pcre validity gate (telnet_phases.cpp ~1046-1048): a COMPLETE
// multibyte sequence whose continuation bytes ALL carry the 10xxxxxx high bits
// -- so the earlier (c & 0xC0) != 0x80 check at ~1030 never fires -- but which
// is SEMANTICALLY invalid is caught only by _pcre_valid_utf.  Here ED A0 80
// encodes the UTF-16 surrogate U+D800, which _pcre_valid_utf rejects
// (PCRE_UTF8_ERR14 > 0) -> OutputBadUTF8characters.  Distinguishing observable:
// OutputBadUTF8characters walks the collected bytes setting m_cLastChar to each
// (ending on the last byte, 0x80); the valid-sequence arm (plain AddToLine)
// never touches m_cLastChar.  A pre-set sentinel proves which arm ran, so a
// mutation of the pcre gate flips the assertion (and NOT the ~1030 path, whose
// bad-continuation tests all trip earlier).
TEST(TelnetR5, Utf8SurrogateRejectedByPcreValidityGate)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_bUTF_8    = TRUE;
    doc.m_cLastChar = 0x7E;   // '~' sentinel -- survives ONLY the valid arm

    // ED A0 80 = U+D800: 0xA0 and 0x80 are both 10xxxxxx (they pass the ~1030
    // high-bits check); the sequence completes, then the pcre gate rejects it.
    const unsigned char seq[] = { 0xED, 0xA0, 0x80 };
    doc.DisplayMsg((LPCTSTR)seq, sizeof seq, 0);

    EXPECT_EQ(NONE, doc.m_phase);
    // reject arm ran: OutputBadUTF8characters advanced m_cLastChar to the final
    // raw byte (0x80); the valid arm would have left the 0x7E sentinel intact.
    EXPECT_EQ((char)0x80, doc.m_cLastChar);
}

// Same pcre gate, exercised via the 4-byte collection path (m_iUTF8BytesLeft=3)
// and a different _pcre_valid_utf arm: F4 90 80 80 encodes U+110000 (> the
// U+10FFFF maximum).  Its three continuation bytes are all 10xxxxxx (the ~1030
// check passes); the completed sequence is rejected by the pcre gate
// (PCRE_UTF8_ERR13 > 0) -> OutputBadUTF8characters, which sets m_cLastChar to
// the last raw byte (0x80).  The plain-AddToLine valid arm would leave the
// sentinel intact, so this flips on the same gate mutation.
TEST(TelnetR5, Utf8AboveMaxCodepointRejectedByPcreValidityGate)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_bUTF_8    = TRUE;
    doc.m_cLastChar = 0x7E;   // sentinel -- survives ONLY the valid arm

    const unsigned char seq[] = { 0xF4, 0x90, 0x80, 0x80 };  // U+110000 (> U+10FFFF)
    doc.DisplayMsg((LPCTSTR)seq, sizeof seq, 0);

    EXPECT_EQ(NONE, doc.m_phase);
    EXPECT_EQ((char)0x80, doc.m_cLastChar);
}

MC_TEST_MAIN()
