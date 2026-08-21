// tests/test_tabcomplete.cpp
// Characterization tests for CMUSHclientDoc::TabCompleteCommand — the canonical
// Tab-completion algorithm hoisted out of CSendView into
// doc_tabcomplete_shared.cpp so both frontends run one body.
//
// These pin the behaviour the wx port used to get wrong when it had its own
// from-scratch Tab handler: the defaults list is consulted (and beats the output
// buffer), the word under the cursor is what gets replaced (not the whole line),
// App.m_strWordDelimiters decides the word boundaries, and the three world
// options — m_bLowerCaseTabCompletion, m_bTabCompletionSpace,
// m_iTabCompletionLines — actually do something.

#include "mc_test.h"
#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"

#include <cstring>
#include <string>

#include "fake_worldview.h"

// ── helpers ─────────────────────────────────────────────────────────────────

// A world with the stock delimiter set and a view attached.
struct TabFixture
{
    CMUSHclientDoc doc;
    FakeWorldView  view;
    CString        savedDelims;
    // Restores the doc's own view, which on Windows it constructed and will
    // delete -- see ScopedWorldView in mc_test.h.  Declared after the two it
    // refers to so it is destroyed first.
    mc_test::ScopedWorldView<CMUSHclientDoc, FakeWorldView> attach;

    TabFixture() : attach(doc, &view)
    {
        savedDelims = App.m_strWordDelimiters;
        // The shipping default (globalregistryoptions.cpp "WordDelimiters").
        App.m_strWordDelimiters = ".,()[]\"'";

        doc.m_iConnectPhase = eConnectConnectedToMud;
        doc.m_iFlags        = COLOUR_ANSI;
        doc.m_iForeColour   = WHITE;
        doc.m_iBackColour   = BLACK;

        // Stock-ish option values; individual tests override.
        doc.m_bLowerCaseTabCompletion = false;
        doc.m_bTabCompletionSpace     = false;
        doc.m_iTabCompletionLines     = 200;
        doc.m_strTabCompletionDefaults = "";
    }

    ~TabFixture()
    {
        App.m_strWordDelimiters = savedDelims;
    }

    // Put `text` in the input bar with the caret at `caret` (defaults to end).
    void type(const char* text, int caret = -1)
    {
        view.cmd = text;
        int c = (caret < 0) ? (int)view.cmd.size() : caret;
        view.cmdSel.start = view.cmdSel.end = c;
    }

    // Push a line of MUD output into m_LineList, which is what the output-buffer
    // scan walks.
    void output(const char* text)
    {
        std::string s(text);
        s += "\r\n";
        doc.DisplayMsg(s.c_str(), (int)s.size(), 0);
        doc.UpdateAllViews(nullptr);
    }

    const std::string& command() const { return view.cmd; }
};

// ── the defaults list ───────────────────────────────────────────────────────

TEST(TabComplete, completes_from_the_defaults_list)
{
    TabFixture f;
    f.doc.m_strTabCompletionDefaults = "cackle giggle";
    f.type("cac");

    f.doc.TabCompleteCommand();

    EXPECT_EQ(std::string("cackle"), f.command());
}

TEST(TabComplete, defaults_list_beats_the_output_buffer)
{
    TabFixture f;
    f.doc.m_strTabCompletionDefaults = "cackle";
    f.output("you hear a cacophony of noise");
    f.type("cac");

    f.doc.TabCompleteCommand();

    // Windows searches the defaults list first and returns on the first hit,
    // never reaching the output buffer.
    EXPECT_EQ(std::string("cackle"), f.command());
}

// ── the output buffer ───────────────────────────────────────────────────────

TEST(TabComplete, completes_from_recent_output)
{
    TabFixture f;
    f.output("the Librarian shambles past");
    f.type("Librar");

    f.doc.TabCompleteCommand();

    EXPECT_EQ(std::string("Librarian"), f.command());
}

TEST(TabComplete, output_scan_stops_at_the_line_limit)
{
    TabFixture f;
    f.doc.m_iTabCompletionLines = 2;
    f.output("Ankh-Morpork is here");   // too far back to be scanned
    f.output("filler one");
    f.output("filler two");
    f.type("Ankh");

    f.doc.TabCompleteCommand();

    // Only the last 2 lines are searched, so nothing matches and the input is
    // left exactly as typed.
    EXPECT_EQ(std::string("Ankh"), f.command());
}

TEST(TabComplete, no_match_leaves_the_command_untouched)
{
    TabFixture f;
    f.output("nothing relevant here");
    f.type("zzz");

    f.doc.TabCompleteCommand();

    EXPECT_EQ(std::string("zzz"), f.command());
}

TEST(TabComplete, does_not_complete_to_a_same_length_word)
{
    TabFixture f;
    // Same length as what gets typed, but a different case, so a botched
    // "len >= typed" guard would visibly rewrite the input.
    f.output("CACK");
    f.type("cack");

    f.doc.TabCompleteCommand();

    // "don't match on same length word" — the replacement must be strictly
    // longer than what was typed.
    EXPECT_EQ(std::string("cack"), f.command());
}

// ── the word under the cursor ───────────────────────────────────────────────

TEST(TabComplete, replaces_only_the_word_under_the_cursor)
{
    TabFixture f;
    f.doc.m_strTabCompletionDefaults = "Librarian";
    f.type("say hello to Librar");

    f.doc.TabCompleteCommand();

    // The port's old handler replaced the entire input line here.
    EXPECT_EQ(std::string("say hello to Librarian"), f.command());
}

TEST(TabComplete, word_boundary_honours_the_delimiter_set)
{
    TabFixture f;
    App.m_strWordDelimiters = ".,()[]\"'";
    f.doc.m_strTabCompletionDefaults = "Librarian";
    f.type("(Librar");

    f.doc.TabCompleteCommand();

    // '(' is a delimiter, so the word starts after it and the paren survives.
    EXPECT_EQ(std::string("(Librarian"), f.command());
}

TEST(TabComplete, does_nothing_when_text_is_selected)
{
    TabFixture f;
    f.doc.m_strTabCompletionDefaults = "cackle";
    f.view.cmd = "cac";
    f.view.cmdSel.start = 0;
    f.view.cmdSel.end   = 3;   // a real selection, not a caret

    f.doc.TabCompleteCommand();

    EXPECT_EQ(std::string("cac"), f.command());
}

TEST(TabComplete, does_nothing_on_an_empty_command)
{
    TabFixture f;
    f.doc.m_strTabCompletionDefaults = "cackle";
    f.type("");

    f.doc.TabCompleteCommand();

    EXPECT_EQ(std::string(""), f.command());
}

TEST(TabComplete, does_nothing_when_the_caret_follows_a_delimiter)
{
    TabFixture f;
    f.doc.m_strTabCompletionDefaults = "cackle";
    f.type("say ");   // caret sits right after a space

    f.doc.TabCompleteCommand();

    EXPECT_EQ(std::string("say "), f.command());
}

// ── the three world options ─────────────────────────────────────────────────

TEST(TabComplete, lower_case_option_downcases_the_replacement)
{
    TabFixture f;
    f.doc.m_bLowerCaseTabCompletion = true;
    f.output("the Librarian shambles past");
    f.type("Librar");

    f.doc.TabCompleteCommand();

    EXPECT_EQ(std::string("librarian"), f.command());
}

TEST(TabComplete, space_option_appends_a_trailing_space)
{
    TabFixture f;
    f.doc.m_bTabCompletionSpace = true;
    f.doc.m_strTabCompletionDefaults = "cackle";
    f.type("cac");

    f.doc.TabCompleteCommand();

    EXPECT_EQ(std::string("cackle "), f.command());
}

TEST(TabComplete, caret_inside_a_word_forces_the_trailing_space)
{
    TabFixture f;
    f.doc.m_bTabCompletionSpace = false;
    f.doc.m_strTabCompletionDefaults = "cackle";
    // "cacX" with the caret after "cac": the next character is not a space or a
    // delimiter, so the canonical code turns the space option on for this one
    // completion...
    f.type("cacX", 3);

    f.doc.TabCompleteCommand();

    EXPECT_EQ(std::string("cackle X"), f.command());
    // ...and restores it afterwards.
    EXPECT_FALSE(f.doc.m_bTabCompletionSpace);
}

TEST(TabComplete, space_option_is_restored_after_a_failed_completion)
{
    TabFixture f;
    f.doc.m_bTabCompletionSpace = true;
    f.type("zzzQ", 3);   // forces the override on, then finds no match

    f.doc.TabCompleteCommand();

    EXPECT_TRUE(f.doc.m_bTabCompletionSpace);
}

MC_TEST_MAIN()
