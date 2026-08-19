// tests/test_hyperlink.cpp
// Characterization tests for CMUSHclientDoc::HyperlinkClicked — the canonical
// send-link / prompt-link handling hoisted out of CMUSHView::OnLButtonDown into
// doc_hyperlink_shared.cpp so both frontends run one body.
//
// These pin what the wx port used to get wrong when it had its own link-click
// lambda: a send-link on a NOTE line is executed rather than sent, the
// !!pluginID:sub(arg) convention is decoded into a plugin call (this is how a
// plugin makes a link that calls back into itself — lua/mapper.lua relies on
// it), a prompt-link fills the command bar instead of sending, the world's
// "echo hyperlink in output window" option decides the echo, and
// "add hyperlink to command history" decides the history entry.

#include "mc_test.h"
#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "pal/itransport.h"
#include "fake_transport.h"

#include <string>

// ── helpers ─────────────────────────────────────────────────────────────────

// Enough of a transport that ConnectionOpen() is true and SendMsg records.
using LinkTransport = RecordingTransport;

struct LinkFixture
{
    CMUSHclientDoc doc;
    LinkTransport* wire = nullptr;

    LinkFixture()
    {
        doc.m_mush_name = "TestWorld";
        doc.m_server    = "mud.example.com";
        doc.m_port      = 4000;
        doc.m_wrap      = FALSE;
        doc.m_nWrapColumn = 500;
        doc.m_iConnectPhase = eConnectConnectedToMud;
        wire = new LinkTransport();
        doc.m_pTransport = wire;
        // Pin the two options the tests below flip, so the others are
        // deterministic whatever the shipped defaults are.
        doc.m_bEchoHyperlinkInOutputWindow  = FALSE;
        doc.m_bHyperlinkAddsToCommandHistory = FALSE;
        doc.Note("");     // retire the ctor-time 80-column line
    }

    ~LinkFixture()
    {
        delete doc.m_pTransport;
        doc.m_pTransport = nullptr;
    }

    // Pretend the world is closed, WITHOUT the "reconnect?" message box:
    // CheckConnected bails early on the 0.0.0.0 address.
    void disconnect()
    {
        doc.m_iConnectPhase = eConnectNotConnected;
        doc.m_server        = "0.0.0.0";
    }

    std::string text() const
    {
        std::string out;
        for (POSITION pos = doc.m_LineList.GetHeadPosition(); pos; ) {
            CLine* l = doc.m_LineList.GetNext(pos);
            out.append(l->text, l->len);
            out += '\n';
        }
        return out;
    }

    bool saw(const char* needle) const
    {
        return text().find(needle) != std::string::npos;
    }
};

// Add a plain-text alias (same shape as tests/test_aliases.cpp's helper).
static void add_alias(CMUSHclientDoc& doc, const char* match, const char* send)
{
    CAlias* a    = new CAlias();
    a->name      = CString(match);
    a->contents  = CString(send);
    a->bEnabled  = TRUE;
    a->bRegexp   = false;
    a->iSendTo   = eSendToWorld;
    a->iSequence = DEFAULT_ALIAS_SEQUENCE;
    a->strLabel  = CString(match);
    a->regexp    = regcomp(ConvertToRegularExpression(a->name, true, true), 0);
    doc.GetAliasMap().SetAt(a->strLabel, a);
    doc.GetAliasArray().Add(a);
    doc.GetAliasRevMap()[a] = std::string(match);
}

// A syntactically valid but not-installed plugin id: 24 hex digits.
static const char* kPluginID = "753ba7e011f3c8943a885f18";

// ── note lines execute, they do not send ────────────────────────────────────

// mushview.cpp: "action_send on a note line will be executed, not sent
// (for the world.Hyperlink function)".  "Executed" means it goes through the
// command evaluator — aliases, script sends, speedwalks — where an ordinary
// send-link is transmitted verbatim.  An alias makes the difference visible:
// the port ran EvaluateCommand for BOTH, so every link was alias-expanded.
TEST(Hyperlink, NoteLineLinkIsExecutedSoAliasesExpand)
{
    LinkFixture f;
    add_alias(f.doc, "look", "examine the room");
    EXPECT_EQ((int) eHyperlinkNothing,
              (int) f.doc.HyperlinkClicked("look", ACTION_SEND, true));
    EXPECT_EQ(std::string("examine the room\r\n"), f.wire->sent);
}

TEST(Hyperlink, OrdinaryLineLinkIsSentVerbatimWithoutAliasExpansion)
{
    LinkFixture f;
    add_alias(f.doc, "look", "examine the room");
    EXPECT_EQ((int) eHyperlinkNothing,
              (int) f.doc.HyperlinkClicked("look", ACTION_SEND, false));
    EXPECT_EQ(std::string("look\r\n"), f.wire->sent);
}

// ── the !!pluginID:sub(arg) plugin-call convention ──────────────────────────

// The decode existed only in mushview.cpp, i.e. only in a view the port never
// constructs, so on the port the whole ~60-character string was sent to the MUD.
// With no such plugin installed the canonical path reports it — which is proof
// the string was recognised as a plugin call rather than executed as a command.
TEST(Hyperlink, PluginCallSyntaxIsDecodedAndReportsAMissingPlugin)
{
    LinkFixture f;
    CString link = CString("!!") + kPluginID + ":mysub(1234)";
    EXPECT_EQ((int) eHyperlinkNothing,
              (int) f.doc.HyperlinkClicked(link, ACTION_SEND, true));
    EXPECT_TRUE(f.saw("is not installed"));
    EXPECT_TRUE(f.saw(kPluginID));            // named by id: the plugin is absent
    EXPECT_EQ(std::string(), f.wire->sent);   // and nothing was sent
}

// Every one of the six syntax tests has to fail closed: a near-miss is executed
// as an ordinary command, not treated as a plugin call.
TEST(Hyperlink, NearMissPluginSyntaxIsExecutedInstead)
{
    struct { const char* what; CString link; } cases[] = {
        { "no !! prefix",     CString("xx") + kPluginID + ":mysub(1)" },
        { "no trailing )",    CString("!!") + kPluginID + ":mysub(1"  },
        { "no ( at all",      CString("!!") + kPluginID + ":mysub"    },
        { "id not hex",       CString("!!") + "zzzba7e011f3c8943a885f18" + ":mysub(1)" },
        { "id too short",     CString("!!") + "753ba7e011f3c894" + ":mysub(1)" },
        { "no colon",         CString("!!") + kPluginID + ".mysub(1)" },
        { "bad sub name",     CString("!!") + kPluginID + ":my sub(1)" },
    };

    for (const auto& c : cases) {
        LinkFixture f;
        EXPECT_EQ((int) eHyperlinkNothing,
                  (int) f.doc.HyperlinkClicked(c.link, ACTION_SEND, true));
        // Executed as a command, so no plugin diagnostics anywhere.  Print the
        // case name on failure — otherwise seven identical lines say nothing.
        if (f.saw("is not installed"))
            fprintf(stderr, "  near-miss treated as a plugin call: %s\n", c.what);
        EXPECT_FALSE(f.saw("is not installed"));
    }
}

// ── the not-connected guard ─────────────────────────────────────────────────

// mushview.cpp returns from OnLButtonDown without even moving the focus when
// CheckConnected() says no — hence a distinct outcome rather than "nothing".
TEST(Hyperlink, ClosedWorldAbortsWithoutSending)
{
    LinkFixture f;
    f.disconnect();
    EXPECT_EQ((int) eHyperlinkAbort,
              (int) f.doc.HyperlinkClicked("look", ACTION_SEND, false));
    EXPECT_EQ(std::string(), f.wire->sent);
}

// A note line executes BEFORE the connected check — that is the whole point of
// world.Hyperlink working in an offline world.
TEST(Hyperlink, NoteLineLinkStillWorksWhenNotConnected)
{
    LinkFixture f;
    f.disconnect();
    CString link = CString("!!") + kPluginID + ":mysub(1)";
    EXPECT_EQ((int) eHyperlinkNothing,
              (int) f.doc.HyperlinkClicked(link, ACTION_SEND, true));
    EXPECT_TRUE(f.saw("is not installed"));
}

// ── prompt links fill the command bar ───────────────────────────────────────

// ACTION_PROMPT (<send href="..." prompt>) must NOT send: Windows puts the text
// in the command bar.  The port sent it, so a prompt-link fired immediately.
TEST(Hyperlink, PromptLinkAsksTheCallerToFillTheCommandBar)
{
    LinkFixture f;
    EXPECT_EQ((int) eHyperlinkFillCommand,
              (int) f.doc.HyperlinkClicked("say hello", ACTION_PROMPT, false));
    EXPECT_EQ(std::string(), f.wire->sent);
}

// ── the two world options that were dead on the port ────────────────────────

// World Properties > MXP/Pueblo > "Echo hyperlink in output window"
// (m_bEchoHyperlinkInOutputWindow).  The port hardcoded m_display_my_input
// instead, so this checkbox did nothing (backlog item 41).
TEST(Hyperlink, EchoOptionDecidesWhetherTheCommandAppearsInTheOutput)
{
    {
        LinkFixture on;
        on.doc.m_bEchoHyperlinkInOutputWindow = TRUE;
        on.doc.m_display_my_input = FALSE;     // the flag the port used to read
        on.doc.HyperlinkClicked("kick the door", ACTION_SEND, false);
        EXPECT_TRUE(on.saw("kick the door"));
    }
    {
        LinkFixture off;
        off.doc.m_bEchoHyperlinkInOutputWindow = FALSE;
        off.doc.m_display_my_input = TRUE;     // ... and must not be consulted
        off.doc.HyperlinkClicked("kick the door", ACTION_SEND, false);
        EXPECT_FALSE(off.saw("kick the door"));
    }
}

// World Properties > MXP/Pueblo > "Save hyperlink mouse clicks in history"
// (m_bHyperlinkAddsToCommandHistory).  The port never touched the history at
// all (backlog item 40).  The entry itself is the frontend's own storage, so
// the shared body reports the decision rather than making it.
TEST(Hyperlink, HistoryOptionDecidesWhetherTheCallerRecordsTheClick)
{
    {
        LinkFixture on;
        on.doc.m_bHyperlinkAddsToCommandHistory = TRUE;
        EXPECT_EQ((int) eHyperlinkAddHistory,
                  (int) on.doc.HyperlinkClicked("north", ACTION_SEND, false));
    }
    {
        LinkFixture off;
        off.doc.m_bHyperlinkAddsToCommandHistory = FALSE;
        EXPECT_EQ((int) eHyperlinkNothing,
                  (int) off.doc.HyperlinkClicked("north", ACTION_SEND, false));
    }
}

// The history option applies to sends only — a prompt-link never gets recorded,
// and neither does a note-line link.
TEST(Hyperlink, HistoryOptionDoesNotApplyToPromptOrNoteLinks)
{
    LinkFixture f;
    f.doc.m_bHyperlinkAddsToCommandHistory = TRUE;
    EXPECT_EQ((int) eHyperlinkFillCommand,
              (int) f.doc.HyperlinkClicked("north", ACTION_PROMPT, false));
    EXPECT_EQ((int) eHyperlinkNothing,
              (int) f.doc.HyperlinkClicked("north", ACTION_SEND, true));
}

MC_TEST_MAIN()
