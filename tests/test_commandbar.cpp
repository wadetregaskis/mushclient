// tests/test_commandbar.cpp
// Characterization tests for the command-bar commands hoisted out of CSendView
// into doc_commandbar_shared.cpp so both frontends run one body:
//
//   RepeatLastWord        — Edit ▸ Recall Last Word / Ctrl+Backspace
//   GlobalChangeCommand   — Game ▸ Global Change, minus the dialog
//   CommandCaretAtEdge    — the Up/Down history gate
//   IsScriptPrefixCommand — "don't spell check script commands"
//
// Each of these pins behaviour the wx port used to get wrong, because the only
// reader lived in CSendView and the port never instantiates that view:
//
//   * Ctrl+Backspace was hardwired to delete-a-word, so Windows' DEFAULT action
//     for the key (recall the last word of the previous command) did not exist
//     and "ctrl_backspace_deletes_last_word" had no reader at all;
//   * Global Change ignored the input bar's selection and rewrote the whole
//     control, and never reported "no replacements made";
//   * plain Up/Down browsed history from any caret position, destroying a
//     multi-line composition;
//   * spell-check-on-send ran on script-prefix commands.

#include "mc_test.h"
#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"

#include <string>

#include "fake_worldview.h"

// ── helpers ─────────────────────────────────────────────────────────────────

struct BarFixture
{
    CMUSHclientDoc doc;
    FakeWorldView  view;
    // Restores the doc's own view, which on Windows it constructed and will
    // delete -- see ScopedWorldView in mc_test.h.  Declared after the two it
    // refers to so it is destroyed first.
    mc_test::ScopedWorldView<CMUSHclientDoc, FakeWorldView> attach;

    BarFixture() : attach(doc, &view) {}

    // Put `text` in the input bar with the caret at `caret` (defaults to end).
    void type(const char* text, int caret = -1)
    {
        view.cmd = text;
        int c = (caret < 0) ? (int)view.cmd.size() : caret;
        view.cmdSel.start = view.cmdSel.end = c;
    }
    // Put `text` in the bar with [from, to) selected.
    void select(const char* text, int from, int to)
    {
        view.cmd = text;
        view.cmdSel.start = from;
        view.cmdSel.end   = to;
    }

    const std::string& command() const { return view.cmd; }
    int caret() const { return view.cmdSel.start; }
};

// ── Recall Last Word (ctrl_backspace_deletes_last_word OFF — the default) ────

TEST(RecallLastWord, is_the_default_action_and_appends_the_last_word)
{
    // The scenario from the bug report: "get sword from bag" is the last thing
    // sent, the user has typed "put " and presses Ctrl+Backspace.  Windows
    // completes it to "put bag"; the port used to delete "put ".
    BarFixture f;
    f.doc.m_bCtrlBackspaceDeletesLastWord = false;   // doc_construct.cpp default
    f.view.history.push_back("get sword from bag");
    f.type("put ");

    f.doc.RepeatLastWord();

    EXPECT_EQ(std::string("put bag"), f.command());
}

TEST(RecallLastWord, a_single_word_command_recalls_the_whole_thing)
{
    BarFixture f;
    f.doc.m_bCtrlBackspaceDeletesLastWord = false;
    f.view.history.push_back("north");
    f.type("");

    f.doc.RepeatLastWord();

    EXPECT_EQ(std::string("north"), f.command());
}

TEST(RecallLastWord, trailing_spaces_on_the_recalled_line_are_trimmed_first)
{
    BarFixture f;
    f.doc.m_bCtrlBackspaceDeletesLastWord = false;
    f.view.history.push_back("kill rat   ");
    f.type("look ");

    f.doc.RepeatLastWord();

    EXPECT_EQ(std::string("look rat"), f.command());
}

TEST(RecallLastWord, uses_the_newest_history_entry)
{
    BarFixture f;
    f.doc.m_bCtrlBackspaceDeletesLastWord = false;
    f.view.history.push_back("get lamp");
    f.view.history.push_back("open door");
    f.type("close ");

    f.doc.RepeatLastWord();

    EXPECT_EQ(std::string("close door"), f.command());
}

TEST(RecallLastWord, replaces_the_selection_like_ReplaceSel)
{
    BarFixture f;
    f.doc.m_bCtrlBackspaceDeletesLastWord = false;
    f.view.history.push_back("wield sword");
    f.select("put xx", 4, 6);

    f.doc.RepeatLastWord();

    EXPECT_EQ(std::string("put sword"), f.command());
}

TEST(RecallLastWord, does_nothing_with_no_history)
{
    BarFixture f;
    f.doc.m_bCtrlBackspaceDeletesLastWord = false;
    f.type("put ");

    f.doc.RepeatLastWord();

    EXPECT_EQ(std::string("put "), f.command());
}

// ── the delete arm (ctrl_backspace_deletes_last_word ON) ────────────────────

TEST(RecallLastWord, option_on_deletes_the_word_before_the_caret)
{
    BarFixture f;
    f.doc.m_bCtrlBackspaceDeletesLastWord = true;
    f.view.history.push_back("get sword from bag");   // must be ignored
    f.type("get sword from");

    f.doc.RepeatLastWord();

    // The preceding SPACE goes too (strFirst.Left (iPos) stops short of it),
    // and the caret lands where the earlier word ended.
    EXPECT_EQ(std::string("get sword"), f.command());
    EXPECT_EQ(9, f.caret());
}

TEST(RecallLastWord, option_on_skips_trailing_spaces_before_deleting)
{
    BarFixture f;
    f.doc.m_bCtrlBackspaceDeletesLastWord = true;
    f.type("get sword   ");

    f.doc.RepeatLastWord();

    EXPECT_EQ(std::string("get"), f.command());
    EXPECT_EQ(3, f.caret());
}

TEST(RecallLastWord, option_on_with_no_preceding_space_clears_to_the_start)
{
    BarFixture f;
    f.doc.m_bCtrlBackspaceDeletesLastWord = true;
    f.type("get");

    f.doc.RepeatLastWord();

    EXPECT_EQ(std::string(""), f.command());
    EXPECT_EQ(0, f.caret());
}

TEST(RecallLastWord, option_on_keeps_the_text_after_the_caret)
{
    BarFixture f;
    f.doc.m_bCtrlBackspaceDeletesLastWord = true;
    f.type("get sword bag", 9);   // caret right after "sword"

    f.doc.RepeatLastWord();

    EXPECT_EQ(std::string("get bag"), f.command());
    EXPECT_EQ(3, f.caret());
}

TEST(RecallLastWord, option_on_with_a_selection_just_deletes_the_selection)
{
    BarFixture f;
    f.doc.m_bCtrlBackspaceDeletesLastWord = true;
    f.select("hello world", 0, 6);

    f.doc.RepeatLastWord();

    EXPECT_EQ(std::string("world"), f.command());
}

TEST(RecallLastWord, option_on_does_nothing_with_an_empty_bar)
{
    BarFixture f;
    f.doc.m_bCtrlBackspaceDeletesLastWord = true;
    f.type("");

    f.doc.RepeatLastWord();

    EXPECT_EQ(std::string(""), f.command());
}

// ── Global Change ───────────────────────────────────────────────────────────

TEST(GlobalChange, is_scoped_to_the_selection)
{
    // sendvw.cpp:2077.  "say hello rat; kill rat" with only the trailing
    // "kill rat" selected must leave the first "rat" alone.
    BarFixture f;
    f.select("say hello rat; kill rat", 15, 23);

    EXPECT_TRUE(f.doc.GlobalChangeCommand("rat", "bat", 15, 23));

    EXPECT_EQ(std::string("say hello rat; kill bat"), f.command());
}

TEST(GlobalChange, with_nothing_selected_changes_the_whole_line)
{
    BarFixture f;
    f.type("say hello rat; kill rat", 0);   // caret at 0, no selection

    EXPECT_TRUE(f.doc.GlobalChangeCommand("rat", "bat", 0, 0));

    EXPECT_EQ(std::string("say hello bat; kill bat"), f.command());
}

TEST(GlobalChange, reports_failure_and_leaves_the_bar_alone_when_nothing_matches)
{
    BarFixture f;
    f.type("say hello rat", 0);

    EXPECT_FALSE(f.doc.GlobalChangeCommand("zzz", "bat", 0, 0));

    EXPECT_EQ(std::string("say hello rat"), f.command());
}

TEST(GlobalChange, translates_backslash_t_to_a_tab)
{
    BarFixture f;
    f.type("a\tb", 0);

    EXPECT_TRUE(f.doc.GlobalChangeCommand("\\t", "-", 0, 0));

    EXPECT_EQ(std::string("a-b"), f.command());
}

TEST(GlobalChange, a_doubled_backslash_means_one_literal_backslash)
{
    BarFixture f;
    f.type("a\\tb", 0);      // a, backslash, t, b

    // "\\\\" is the two-character sequence \\ , which means one real backslash
    // — so this must NOT be read as an escape introducing \t.
    EXPECT_TRUE(f.doc.GlobalChangeCommand("\\\\", "/", 0, 0));

    EXPECT_EQ(std::string("a/tb"), f.command());
}

TEST(GlobalChange, carriage_returns_are_dropped)
{
    BarFixture f;
    f.type("a\r\nb", 0);

    EXPECT_TRUE(f.doc.GlobalChangeCommand("b", "c", 0, 0));

    EXPECT_EQ(std::string("a\nc"), f.command());
}

// ── the Up/Down history gate ────────────────────────────────────────────────

TEST(CommandCaretAtEdge, true_at_the_very_start)
{
    BarFixture f;
    f.type("look\nnorth", 0);
    EXPECT_TRUE(f.doc.CommandCaretAtEdge());
}

TEST(CommandCaretAtEdge, true_at_the_very_end)
{
    BarFixture f;
    f.type("look\nnorth");     // caret defaults to the end
    EXPECT_TRUE(f.doc.CommandCaretAtEdge());
}

TEST(CommandCaretAtEdge, false_in_the_middle)
{
    // The bug: a two-line composition ("look", Shift+Return, "north") with the
    // caret on the second line.  Windows moves the caret up a line; the port
    // threw the whole composition away and pasted a history entry over it.
    BarFixture f;
    f.type("look\nnorth", 7);
    EXPECT_FALSE(f.doc.CommandCaretAtEdge());
}

TEST(CommandCaretAtEdge, false_with_a_selection_spanning_the_whole_text)
{
    BarFixture f;
    f.select("look\nnorth", 0, 10);
    EXPECT_FALSE(f.doc.CommandCaretAtEdge());
}

TEST(CommandCaretAtEdge, true_for_an_empty_bar)
{
    BarFixture f;
    f.type("");
    EXPECT_TRUE(f.doc.CommandCaretAtEdge());
}

// ── "don't spell check script commands" ─────────────────────────────────────

TEST(ScriptPrefixCommand, matches_a_line_starting_with_the_prefix)
{
    BarFixture f;
    f.doc.m_bEnableScripts   = true;
    f.doc.m_strScriptPrefix  = "/";
    EXPECT_TRUE(f.doc.IsScriptPrefixCommand("/tell_all(\"hi\")"));
}

TEST(ScriptPrefixCommand, a_multi_character_prefix_works)
{
    BarFixture f;
    f.doc.m_bEnableScripts   = true;
    f.doc.m_strScriptPrefix  = "lua:";
    EXPECT_TRUE(f.doc.IsScriptPrefixCommand("lua:print (1)"));
    EXPECT_FALSE(f.doc.IsScriptPrefixCommand("lua print (1)"));
}

TEST(ScriptPrefixCommand, an_ordinary_command_does_not_match)
{
    BarFixture f;
    f.doc.m_bEnableScripts   = true;
    f.doc.m_strScriptPrefix  = "/";
    EXPECT_FALSE(f.doc.IsScriptPrefixCommand("teh quick brown fox"));
}

TEST(ScriptPrefixCommand, needs_scripting_enabled)
{
    BarFixture f;
    f.doc.m_bEnableScripts   = false;
    f.doc.m_strScriptPrefix  = "/";
    EXPECT_FALSE(f.doc.IsScriptPrefixCommand("/tell_all(\"hi\")"));
}

TEST(ScriptPrefixCommand, needs_a_prefix_to_be_configured)
{
    BarFixture f;
    f.doc.m_bEnableScripts   = true;
    f.doc.m_strScriptPrefix  = "";
    EXPECT_FALSE(f.doc.IsScriptPrefixCommand("anything at all"));
}

MC_TEST_MAIN()
