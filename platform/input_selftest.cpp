// platform/input_selftest.cpp — in-app conformance test for the command-input
// bar (--input-selftest).
//
// The four behaviours it pins live in CWorldChildFrame and CMUSHclientApp, both
// of which are in mushgui_adapters and so unreachable from the 33 headless
// mushcore test executables.  The canonical decisions they call are covered
// headlessly (tests/test_commandbar.cpp); what can only be checked HERE is the
// wiring — that the real key handler routes Ctrl+Backspace to the canonical
// body, that the real arrow handler consults the caret gate, and that a
// cancelled spell check aborts a send.  All three were port bugs:
//
//   * Ctrl+Backspace was hardwired to delete-a-word, so Windows' default action
//     for that key (Recall Last Word) did not exist and the world option
//     "ctrl_backspace_deletes_last_word" had no reader at all;
//   * plain Up/Down browsed the history from ANY caret position, so pressing Up
//     to move between the lines of a Shift+Return composition destroyed it;
//   * CMUSHclientApp::SpellCheckString folded "user cancelled" into "here is
//     your text back unchanged", so spell-check-on-send could not abort a send
//     the way sendvw.cpp:344 does.
//
// Runs before the GUI boots (after the MFC→wx hooks are installed, which the
// world frame needs) and exits without opening any world.

#ifndef _WIN32

// Include order is critical (same rule as wx_world_child.cpp):
//   1. wx headers first (so wx's min/max, MessageBox etc. are not shadowed)
//   2. MUSHclient / MFC-compat headers after
#include <wx/wx.h>
#include <wx/mdi.h>

#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "platform/wx_world_child.h"
#include "platform/selftest_support.h"   // plugin-loading fixture
#include "platform/wx_mfc_hooks.h"    // g_wxhook_message_box
#include "platform/wx_osx_text.h"     // wxOSXSmartSubstitutionsDisabled
#include "pal/itransport.h"           // the recording transport below (F2/F9)

#include <cstdio>
#include <functional>

namespace {

int g_pass = 0, g_fail = 0;

void chk(bool ok, const char* what)
{
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    (ok ? g_pass : g_fail)++;
}

void chkStr(const wxString& got, const char* want, const char* what)
{
    const bool ok = (got == wxString::FromUTF8(want));
    printf("  [%s] %s%s", ok ? "PASS" : "FAIL", what, ok ? "\n" : "");
    if (!ok)
        printf("  (wanted \"%s\", got \"%s\")\n", want,
               (const char*) got.utf8_str());
    (ok ? g_pass : g_fail)++;
}

// Synthesise a key-down on the input bar, exactly as wx delivers one, so the
// frame's real wxEVT_KEY_DOWN binding (OnInputKeyDown) runs.
void sendKey(wxTextCtrl* inp, int code, bool ctrl = false, bool alt = false)
{
    wxKeyEvent e(wxEVT_KEY_DOWN);
    e.m_keyCode      = code;
    e.m_controlDown  = ctrl;
    e.m_altDown      = alt;
#if defined(__WXOSX__) || defined(__WXMAC__)
    // RawControlDown() — which every binding in OnInputKeyDown tests — reads
    // this member on macOS and m_controlDown everywhere else.
    e.m_rawControlDown = ctrl;
#endif
    e.SetEventObject(inp);
    inp->GetEventHandler()->ProcessEvent(e);
}

// A stand-in for the spellchecker.lua `spellcheck` global.  Returning nil is
// how it reports "the user pressed Cancel" (scripting/spellchecker.lua:254);
// returning a string is a completed check.
int g_spellCalls = 0;
int spell_cancel(lua_State* L)   { ++g_spellCalls; lua_pushnil(L);         return 1; }
int spell_correct(lua_State* L)  { ++g_spellCalls; lua_pushstring(L, "the"); return 1; }

void installSpellChecker(lua_CFunction fn)
{
    if (App.m_SpellChecker_Lua)
        lua_close(App.m_SpellChecker_Lua);
    App.m_SpellChecker_Lua = MakeLuaState();
    lua_pushcfunction(App.m_SpellChecker_Lua, fn);
    lua_setglobal(App.m_SpellChecker_Lua, SPELLCHECKFUNCTION);
    App.m_bSpellCheckOK = true;
    g_spellCalls = 0;
}

// Any ::UMessageBox this harness provokes would otherwise open a real modal
// alert and wait forever under ctest.  Swallow them (answering OK) and log
// them instead, so a check that goes wrong shows up as a FAIL rather than a
// test-suite hang.  The disconnected-world "you must connect first" box on the
// send path is the one that actually occurs.
int swallow_message_box(const char* text, const char* /*caption*/, unsigned)
{
    printf("  (suppressed message box: %s)\n", text ? text : "");
    return IDOK;
}

// A pal::ITransport that just discards what is sent -- the same recipe
// tests/test_connection.cpp's RecTransport and world_frame_selftest.cpp's
// RecordingTransport use, headless.  Only needed so DoSendMsg's own
// ConnectionOpen() gate does not swallow the F2/F9 checks below before they
// reach the echo/log logic under test.
struct RecordingTransport : pal::ITransport {
    void resolve (const char*, pal::ResolveToken) override {}
    void cancelResolve () override {}
    void connect (unsigned int, unsigned short, const pal::TransportOptions&) override {}
    void send (const char*, size_t) override {}
    void startTls (const pal::TlsParams&) override {}
    void close () override {}
    std::string lastErrorString () const override { return "rec"; }
};

} // namespace

// Let the frame's own 100 ms world timer run.  Used by the no-echo arm to
// prove the timer CALLS the poll, which driving the poll directly cannot.
// Pump the event loop until `done` is true, or give up after `limitMs`.
//
// NOT a fixed sleep.  It was one -- 350 ms for a 100 ms timer -- and that is
// fine on an idle machine and marginal when the extended selftests are running
// beside it: the run that caught this had ctest -j4 driving four GUI apps.  A
// deadline that waits for the CONDITION is both faster in the normal case and
// immune to load; the limit is only there so a genuine regression fails
// instead of hanging.
static bool pumpUntil(const std::function<bool()>& done, int limitMs = 3000)
{
    const wxLongLong until = wxGetLocalTimeMillis() + limitMs;
    while (wxGetLocalTimeMillis() < until) {
        if (done()) return true;
        wxYield();
        wxMilliSleep(5);
    }
    wxYield();
    return done();
}

int RunInputSelfTest()
{
    printf("INPUT-SELFTEST: driving the real command-bar key + send handlers\n");

    g_wxhook_message_box = swallow_message_box;

    wxMDIParentFrame* parent =
        new wxMDIParentFrame(NULL, wxID_ANY, wxT("input selftest"),
                             wxDefaultPosition, wxSize(900, 700));

    // The doc-list walks that sending a command performs (status line,
    // activity refresh, "is this world still connected") all go through
    // App.m_pWorldDocTemplate, which InitInstance normally creates — and this
    // harness deliberately runs BEFORE InitInstance so no world is opened.
    // Without these, a command that actually reaches the send path dereferences
    // a null template and the process dies with SIGSEGV instead of reporting a
    // failed check.
    if (!App.m_pWorldDocTemplate)
        App.m_pWorldDocTemplate    = new CMultiDocTemplate(0, NULL, NULL, NULL);
    if (!App.m_pNormalDocTemplate)
        App.m_pNormalDocTemplate   = new CMultiDocTemplate(0, NULL, NULL, NULL);
    if (!App.m_pActivityDocTemplate)
        App.m_pActivityDocTemplate = new CMultiDocTemplate(0, NULL, NULL, NULL);

    CMUSHclientDoc* doc = new CMUSHclientDoc();
    // (doc ctor already ran AddDocument -- doc_construct.cpp:521;
    //  a second add left a DANGLING duplicate entry after close)
    // A name no real world uses, so the per-world history file this frame keys
    // off cannot collide with (or be read from) the user's own.  Nothing is
    // written: the file is only saved when the frame is CLOSED, and this
    // selftest exits instead.
    doc->m_mush_name = "zz_mushclient_input_selftest";

    CWorldChildFrame* child =
        new CWorldChildFrame(parent, doc, wxT("input selftest"));

    wxTextCtrl* inp = child->ActiveInputPublic();
    if (!inp)
    {
        printf("INPUT-SELFTEST: no input bar - cannot run\n");
        return 1;
    }

    // ── macOS text substitutions are OFF on the bar the user types into ────
    // Found by using the app against a live MUD (2026-08-10): `say "hi" -- ok`
    // reached the command bar as `say "hi" — ok` — curly quotes and an em dash
    // — and that is what would have been SENT.  MUD grammars are ASCII, so a
    // smart quote is not a cosmetic difference, it is a command that does not
    // parse.
    //
    // The helper was never broken; it simply was not called for this control,
    // while the notepad and multiline dialog fields did get it.  So the thing
    // worth asserting is the WIRING, and it has to be asserted on the REAL
    // frame's control — which is exactly what this selftest already builds.
    // A test of wxOSXDisableSmartSubstitutions itself would have stayed green
    // throughout the entire period the bug existed.
    {
        wxTextCtrl* normalBar = child->NormalInputPublic();
        chk(normalBar != NULL, "substitutions: the world window has a command bar");
        chk(normalBar && wxOSXSmartSubstitutionsDisabled(normalBar),
            "macOS smart quote/dash/replacement are disabled on the command bar");
    }

    // ── Ctrl+Backspace: the world option chooses, and OFF is the default ────
    // (the history starts empty: nothing has ever written a history file for
    //  this world name, and nothing here writes one — deleteCommandHistory,
    //  which would, is deliberately not called.)
    child->addToCommandHistory("get sword from bag");

    doc->m_bCtrlBackspaceDeletesLastWord = false;   // shipped default
    inp->SetValue(wxT("put "));
    inp->SetInsertionPointEnd();
    sendKey(inp, WXK_BACK, /*ctrl*/ true);
    chkStr(inp->GetValue(), "put bag",
           "Ctrl+Backspace recalls the last word by default");

    doc->m_bCtrlBackspaceDeletesLastWord = true;
    inp->SetValue(wxT("get sword from"));
    inp->SetInsertionPointEnd();
    sendKey(inp, WXK_BACK, /*ctrl*/ true);
    chkStr(inp->GetValue(), "get sword",
           "ctrl_backspace_deletes_last_word ON deletes the word instead");
    chk(inp->GetInsertionPoint() == 9,
        "...and leaves the caret where the preceding word ended");

    // The Edit-menu entry point reaches the same body.
    doc->m_bCtrlBackspaceDeletesLastWord = false;
    inp->SetValue(wxT("wield "));
    inp->SetInsertionPointEnd();
    child->RecallLastWord();
    chkStr(inp->GetValue(), "wield bag",
           "Edit > Recall Last Word runs the same command");

    // Ctrl+W keeps the Unix delete-word behaviour (no Windows binding to break):
    // unlike Recall Last Word's delete arm it leaves the preceding space.
    inp->SetValue(wxT("get sword from"));
    inp->SetInsertionPointEnd();
    sendKey(inp, 'W', /*ctrl*/ true);
    chkStr(inp->GetValue(), "get sword ",
           "Ctrl+W still deletes a word (and keeps the space)");

    // ── the Up/Down caret gate ─────────────────────────────────────────────
    child->addToCommandHistory("older command");
    child->addToCommandHistory("newest command");
    doc->m_bArrowsChangeHistory   = true;
    doc->m_bArrowRecallsPartial   = false;
    doc->m_bAltArrowRecallsPartial= false;

    // Caret in the middle of a two-line composition: the arrow must reach the
    // text control, NOT the history.
    inp->SetValue(wxT("look\nnorth"));
    inp->SetInsertionPoint(7);
    sendKey(inp, WXK_UP);
    chkStr(inp->GetValue(), "look\nnorth",
           "Up mid-text leaves a multi-line composition alone");

    // ...but at the very end (and at the very start) it still browses, so the
    // gate cannot be satisfied by simply never browsing.
    inp->SetValue(wxT("look\nnorth"));
    inp->SetInsertionPointEnd();
    sendKey(inp, WXK_UP);
    chkStr(inp->GetValue(), "newest command",
           "Up from the end of the text still browses the history");

    // Down mid-text.  Deliberately AFTER the Up above, which left the history
    // cursor mid-walk: an ungated Down would step forward from there and
    // overwrite the bar.  From a fresh cursor Down is a no-op whatever the gate
    // says, so testing it first would prove nothing.
    inp->SetValue(wxT("look\nnorth"));
    inp->SetInsertionPoint(2);
    sendKey(inp, WXK_DOWN);
    chkStr(inp->GetValue(), "look\nnorth",
           "Down mid-text leaves a multi-line composition alone");

    // A new entry resets the browse cursor (the press above left it parked
    // mid-walk) as well as giving this check its own expected value.
    child->addToCommandHistory("newer still");

    inp->SetValue(wxT("look\nnorth"));
    inp->SetInsertionPoint(0);
    sendKey(inp, WXK_UP);
    chkStr(inp->GetValue(), "newer still",
           "Up from the start of the text still browses the history");

    // ── arrows_change_history OFF must not swallow Ctrl+P too ──────────────
    // The option is meant to free plain Up/Down for caret movement; it has no
    // business also disabling Ctrl+P (remapped to WXK_UP just above, but with
    // RawControlDown() still set on the event) -- Windows routes Ctrl+P through
    // OnFileCtrlP, entirely outside the option.
    {
        const unsigned savedArrows = doc->m_bArrowsChangeHistory;
        const BOOL     savedCtrlP  = doc->m_bCtrlPGoesToPreviousCommand;

        child->addToCommandHistory("f3_older");
        child->addToCommandHistory("f3_newest");

        doc->m_bArrowsChangeHistory        = 0;
        doc->m_bCtrlPGoesToPreviousCommand = TRUE;

        sendKey(inp, 'X'); inp->Clear();   // restart the browse cursor
        sendKey(inp, WXK_UP);
        chkStr(inp->GetValue(), "",
               "arrows_change_history OFF: a plain Up leaves the bar alone");

        sendKey(inp, 'P', /*ctrl*/ true);
        chkStr(inp->GetValue(), "f3_newest",
               "...but Ctrl+P still reaches the history");

        doc->m_bArrowsChangeHistory        = savedArrows;
        doc->m_bCtrlPGoesToPreviousCommand = savedCtrlP;
        sendKey(inp, 'X'); inp->Clear();   // restart the browse cursor for what follows
    }

    // ── caret movement does not cancel a history browse ────────────────────
    // Every key that was not Up/Down or a bare modifier reset m_histIdx, so
    // pressing Left to fix one character of a recalled command -- or Home/End
    // to reach an end of it -- threw the browse away and sent the next Up back
    // to the newest entry.  Windows keeps its history position until the
    // command is sent.  Text-CHANGING keys still reset, deliberately: there is
    // no CheckTyping / confirm-before-replacing-typing equivalent here, so that
    // reset is what stops an edit being silently discarded -- which is why the
    // last check below is a control and not an oversight.
    child->addToCommandHistory("hist_a");
    child->addToCommandHistory("hist_b");
    child->addToCommandHistory("hist_c");

    // Each arm starts from the bottom of the history.  inp->Clear() alone will
    // NOT do that -- it is a programmatic change, so no key event reaches the
    // frame and the browse cursor stays exactly where the previous arm left it
    // -- so send a text-changing key, which is the very reset under test.
    auto restartBrowse = [&]() { sendKey(inp, 'X'); inp->Clear(); };

    restartBrowse();
    sendKey(inp, WXK_UP);
    sendKey(inp, WXK_UP);
    chkStr(inp->GetValue(), "hist_b", "two Ups reach the second-newest entry");
    sendKey(inp, WXK_LEFT);
    sendKey(inp, WXK_UP);
    chkStr(inp->GetValue(), "hist_a",
           "...and a caret move between them does not restart the browse");

    // Home and End are the same kind of key.
    restartBrowse();
    sendKey(inp, WXK_UP);
    sendKey(inp, WXK_UP);
    sendKey(inp, WXK_HOME);
    sendKey(inp, WXK_END);
    sendKey(inp, WXK_UP);
    chkStr(inp->GetValue(), "hist_a", "...nor do Home and End");

    // The control: a key that CHANGES the text still cancels, so the two checks
    // above cannot be satisfied by a handler that simply never resets.
    restartBrowse();
    sendKey(inp, WXK_UP);
    sendKey(inp, WXK_UP);
    sendKey(inp, 'X');
    sendKey(inp, WXK_UP);
    chkStr(inp->GetValue(), "hist_c",
           "...but typing a character does cancel it, back to the newest");

    // ── Ctrl+Up / Ctrl+Down nudge the OUTPUT, and leave the history alone ──
    // Windows' Display ▸ Line Up / Line Down (ID_TEST_LINEUP / ID_TEST_LINEDOWN
    // -> CSendView -> CMUSHView::OnTestLineup / OnTestLinedown) scroll the
    // output by exactly one line without the caret leaving the command bar.
    // The port had no arm for them, and the fall-through was NOT inert: neither
    // the arrows_change_history skip nor the caret-at-edge skip excludes
    // RawControlDown, so control reached the ordinary history walk and Ctrl+Up
    // silently did what plain Up does.  The second assertion in each pair is
    // what would have caught that -- a scroll test alone would not.
    {
        COutputPane* pane = child->OutputPane();
        chk(pane != NULL, "line scroll: the world window has an output pane");

        // DisplayMsg (and so Note) is a no-op until m_LinePositions is
        // allocated, which normally happens at world load; this doc never went
        // through that, so without this the pane stays empty and every
        // assertion below passes vacuously.
        doc->SetUpOutputWindow();
        for (int i = 0; i < 300; ++i)
            doc->Note(("line scroll fixture " + std::to_string(i + 1)).c_str());
        doc->UpdateAllViews(NULL);

        const int lh = pane ? pane->LineHeight() : 0;
        chk(pane && pane->LineCount() > 100 && lh > 0,
            "line scroll (fixture): the pane really holds a scrollable buffer");

        if (pane && lh > 0) {
            // Somewhere in the middle, so a nudge in either direction has room.
            pane->ScrollToLine(150);
            const long start = pane->GetScrollPositionY();

            restartBrowse();                      // browse cursor at the newest
            inp->SetValue(wxT("half-typed"));

            sendKey(inp, WXK_UP, /*ctrl*/ true);
            const long afterUp = pane->GetScrollPositionY();
            chk(afterUp == start - lh,
                "line scroll: Ctrl+Up moves the view up by exactly one line");
            chkStr(inp->GetValue(), "half-typed",
                   "line scroll: ...and does NOT replace what is typed (was: "
                   "it walked the command history like a plain Up)");

            sendKey(inp, WXK_DOWN, /*ctrl*/ true);
            chk(pane->GetScrollPositionY() == start,
                "line scroll: Ctrl+Down brings it back down one line");
            chkStr(inp->GetValue(), "half-typed",
                   "line scroll: ...and leaves the typing alone too");

            // The browse cursor must be untouched as well as the text: a
            // handler that restored the text but had already moved the index
            // would send the next plain Up to the wrong entry.
            inp->Clear();
            sendKey(inp, WXK_UP);
            chkStr(inp->GetValue(), "hist_c",
                   "line scroll: the history browse is still at the newest "
                   "entry, so Ctrl+Up never moved it");

            // The control: a PLAIN Up in the same state does walk the history
            // and does not scroll, so the assertions above discriminate on the
            // modifier rather than on the arrow keys having gone inert.
            const long before = pane->GetScrollPositionY();
            sendKey(inp, WXK_UP);
            chkStr(inp->GetValue(), "hist_b",
                   "line scroll (control): a plain Up still walks the history");
            chk(pane->GetScrollPositionY() == before,
                "line scroll (control): ...and does not scroll the output");

            pane->ScrollToBottom();
        }
        restartBrowse();
    }

    // ── Escape's history push must not leave the browse cursor off by one ──
    // With escape_deletes_input and save_deleted_command both on, Escape pushes
    // the discarded text into the history; AT THE LIMIT that costs an
    // erase(begin()), which shifts every index down by one.  Keeping the browse
    // position across Escape is right -- Windows' OnProcessEscape does the same
    // -- but it holds a POSITION, which survives RemoveHead, while this holds an
    // INDEX, so the cursor was left naming the wrong entry and the next Up
    // re-showed the line already on screen.
    {
        const LONG     savedLines = doc->m_nHistoryLines;
        const unsigned savedEsc   = doc->m_bEscapeDeletesInput;
        const LONG     savedSave  = doc->m_bSaveDeletedCommand;
        doc->m_nHistoryLines       = 20;
        doc->m_bEscapeDeletesInput = 1;
        doc->m_bSaveDeletedCommand = 1;

        // Exactly 20 entries: addToCommandHistory trims to the limit after each
        // add, so whatever the checks above left behind is gone by the last one.
        for (int i = 0; i < 20; ++i) {
            char entry[16];
            snprintf(entry, sizeof(entry), "esc_%02d", i);
            child->addToCommandHistory(entry);
        }

        inp->Clear();
        sendKey(inp, WXK_UP);
        sendKey(inp, WXK_UP);
        chkStr(inp->GetValue(), "esc_18", "escape: two Ups reach esc_18");

        // Pushes "esc_18" and evicts "esc_00", so every index moves.
        sendKey(inp, WXK_ESCAPE);
        chkStr(inp->GetValue(), "", "...and Escape clears the bar");

        sendKey(inp, WXK_UP);
        chkStr(inp->GetValue(), "esc_17",
               "...leaving the browse cursor on the same entry, so Up steps on");

        doc->m_nHistoryLines       = savedLines;
        doc->m_bEscapeDeletesInput = savedEsc;
        doc->m_bSaveDeletedCommand = savedSave;
    }

    // ── the spell-check cancel channel ─────────────────────────────────────
    {
        installSpellChecker(spell_cancel);
        bool bCancelled = false;
        CString out = App.SpellCheckString("teh", true, &bCancelled);
        chk(bCancelled, "SpellCheckString reports a cancelled dialog");
        chk(out == "teh", "...and hands back the original text");

        installSpellChecker(spell_correct);
        bCancelled = true;
        out = App.SpellCheckString("teh", true, &bCancelled);
        chk(!bCancelled, "a completed check is not reported as cancelled");
        chk(out == "the", "...and hands back the corrected text");
    }

    // ── spell check on send ────────────────────────────────────────────────
    // Cancelling must abort the send and leave the text in the bar
    // (sendvw.cpp:344 `if (!bOK) return;`).
    doc->m_bSpellCheckOnSend = true;
    doc->m_bEnableScripts    = true;
    doc->m_strScriptPrefix   = "/";

    installSpellChecker(spell_cancel);
    inp->SetValue(wxT("teh quick brown fox"));
    inp->SetInsertionPointEnd();
    {
        wxCommandEvent enter(wxEVT_TEXT_ENTER, wxID_HIGHEST + 200 /*ID_INPUT*/);
        enter.SetEventObject(inp);
        child->GetEventHandler()->ProcessEvent(enter);
    }
    chk(g_spellCalls == 1, "spell check on send runs for an ordinary command");
    chkStr(inp->GetValue(), "teh quick brown fox",
           "...and cancelling it aborts the send, text intact");

    // A script-prefix command is never checked at all (sendvw.cpp:331-335), so
    // the cancelling checker cannot fire and the line is sent (bar cleared).
    installSpellChecker(spell_cancel);
    inp->SetValue(wxT("/print (1)"));
    inp->SetInsertionPointEnd();
    {
        wxCommandEvent enter(wxEVT_TEXT_ENTER, wxID_HIGHEST + 200 /*ID_INPUT*/);
        enter.SetEventObject(inp);
        child->GetEventHandler()->ProcessEvent(enter);
    }
    chk(g_spellCalls == 0, "a script-prefix command is not spell checked");
    chkStr(inp->GetValue(), "",
           "...and is sent normally (the bar is cleared)");

    // ── what OnPluginCommandEntered's two rejections do to the bar ─────────
    // A plugin answers the callback with the (possibly rewritten) command, or
    // with "\t" to DISCARD it or "\r" to REJECT it.  The difference between
    // those two is entirely about the typed text: "\r" leaves it in the bar so
    // the user can fix it and press Enter again ("are you sure you want to kill
    // the dragon?"), and "\t" clears it -- unless auto-repeat is on, which
    // keeps it so Enter repeats.  The port cleared the bar BEFORE running the
    // callback, which collapsed all three answers onto "your typing is gone".
    //
    // Only the "\t" arm is reachable from here, and only because that arm's
    // behaviour does not depend on what the bar holds.  The "\r" arm cannot be
    // simulated this way at all: the sentinel is what the PLUGIN returns while
    // the bar still holds the user's own text, so putting "\r" in the bar tests
    // nothing (and a lone CR does not survive SetValue in a multiline control
    // anyway).  That arm, and the fix's other half -- that the callback fires at
    // all for a BARE Enter, which it did not -- both need a real plugin with a
    // script engine, which cannot be loaded this early.  NOT covered here.
    doc->m_bSpellCheckOnSend = false;
    {
        const bool savedRepeat = doc->m_bAutoRepeat != 0;

        auto pressEnter = [&]() {
            wxCommandEvent enter(wxEVT_TEXT_ENTER, wxID_HIGHEST + 200 /*ID_INPUT*/);
            enter.SetEventObject(inp);
            child->GetEventHandler()->ProcessEvent(enter);
        };

        // "\t" — discarded.  Cleared, EXCEPT with auto-repeat on.
        doc->m_bAutoRepeat = 0;
        inp->SetValue(wxT("\t"));
        pressEnter();
        chkStr(inp->GetValue(), "", "a discarded command (\"\\t\") clears the bar");

        doc->m_bAutoRepeat = 1;
        inp->SetValue(wxT("\t"));
        pressEnter();
        chkStr(inp->GetValue(), "\t",
               "...but not when auto-repeat is on, which keeps it to repeat");

        // The control: an ordinary command must still clear the bar, or the
        // three checks above would pass on a handler that never clears at all.
        doc->m_bAutoRepeat = 0;
        inp->SetValue(wxT("look"));
        pressEnter();
        chkStr(inp->GetValue(), "", "an accepted command still clears the bar");

        doc->m_bAutoRepeat = savedRepeat ? 1 : 0;
    }

    // ── OnPluginCommandEntered, through a REAL plugin ──────────────────────
    // The two arms the "\t" block above cannot reach, because the sentinel is
    // what the PLUGIN returns while the bar still holds the user's own text:
    //   "\r" rejects the command and LEAVES the typing in the bar to be fixed;
    //   and the callback fires even for a BARE Enter, which the port used to
    //   skip with `if (!cmd.IsEmpty())`.
    // Both were uncovered until a selftest could load a plugin at all.
    {
        SelfTestEnablePluginLoading();

        char seenPath[512];
        snprintf(seenPath, sizeof(seenPath), "/tmp/mc_selftest_entered_%d.log",
                 (int) getpid());
        remove(seenPath);

        char body[1024];
        snprintf(body, sizeof(body),
                 "function OnPluginCommandEntered(s)\n"
                 "  local h = io.open(\"%s\", \"a\")\n"
                 "  if h then h:write(\"[\" .. s .. \"]\\n\") h:close() end\n"
                 "  if s == \"reject me\" then return \"\\r\" end\n"
                 "  return s\n"
                 "end", seenPath);

        const std::string pluginPath = SelfTestWritePlugin(
            "entered", "abcdefabcdefabcdef445566", "EnteredProbe", body);
        chk(!pluginPath.empty(), "OnPluginCommandEntered: wrote a plugin");

        const long rc = doc->LoadPlugin(CString(pluginPath.c_str()));
        chk(rc == 0, "OnPluginCommandEntered: the plugin loaded");

        auto pressEnter2 = [&]() {
            wxCommandEvent enter(wxEVT_TEXT_ENTER, wxID_HIGHEST + 200 /*ID_INPUT*/);
            enter.SetEventObject(inp);
            child->GetEventHandler()->ProcessEvent(enter);
        };
        auto seen = [&]() {
            std::string s;
            if (FILE* fp = fopen(seenPath, "rb")) {
                int ch; while ((ch = fgetc(fp)) != EOF) s += (char) ch;
                fclose(fp);
            }
            return s;
        };

        // A rejected command stays in the bar to be edited.
        inp->SetValue(wxT("reject me"));
        pressEnter2();
        chkStr(inp->GetValue(), "reject me",
               "a plugin rejecting a command leaves the typing in the bar");

        // ...and an accepted one is still sent and cleared.
        inp->SetValue(wxT("keep me"));
        pressEnter2();
        chkStr(inp->GetValue(), "",
               "...while an accepted one is sent and the bar cleared");

        // A BARE Enter reaches the callback too.
        remove(seenPath);
        inp->SetValue(wxEmptyString);
        pressEnter2();
        chk(seen() == "[]\n",
            "the callback fires for a bare Enter, with the empty command");

        // ── auto repeat re-enters the SENT command, selected ──────────────
        // Windows puts strOriginalCommand back in the bar and selects it all
        // (sendvw.cpp:741-748), so Enter repeats it and typing anything at all
        // replaces it.  The port cleared the bar and re-derived the repeat from
        // m_history.back() on the next blank Enter -- a DIFFERENT command as
        // soon as the one just sent was kept out of the history (an alias with
        // "omit from command history", or a password).  The auto-repeat-OFF
        // check above -- "an accepted command still clears the bar" -- is the
        // control: a handler that always clears fails here, and one that never
        // clears fails there.
        {
            const bool savedRepeat2 = doc->m_bAutoRepeat != 0;
            doc->m_bAutoRepeat = 1;

            remove(seenPath);
            inp->SetValue(wxT("north"));
            pressEnter2();
            chkStr(inp->GetValue(), "north",
                   "auto repeat leaves the sent command in the bar");
            long selFrom = -1, selTo = -1;
            inp->GetSelection(&selFrom, &selTo);
            chk(selFrom == 0 && selTo == 5,
                "...with the whole string selected, so typing replaces it");

            // ...and a second Enter really does send it again -- read off the
            // plugin's own log, not off the bar, so this is about the SEND and
            // not a restatement of the check above.
            pressEnter2();
            chk(seen() == "[north]\n[north]\n",
                "...so pressing Enter again repeats the command");

            doc->m_bAutoRepeat = savedRepeat2 ? 1 : 0;
        }

        remove(seenPath);
        remove(pluginPath.c_str());
    }

    // ── auto-repeat: the first Up must not re-show what's already on screen ─
    // reenterForAutoRepeat puts the just-sent command back in the bar AFTER
    // addToCommandHistory has already made it the newest entry, so an ordinary
    // walk starting at "the newest entry" just re-displays the text already
    // there.  Windows skips one further entry in exactly this state
    // (sendvw.cpp:533-538).  Two DISTINCT commands, or Up would trivially find
    // "the same" entry it started on and prove nothing.
    {
        const bool           savedRepeat     = doc->m_bAutoRepeat != 0;
        const unsigned short savedArrows     = doc->m_bArrowsChangeHistory;
        const BOOL           savedPartial    = doc->m_bArrowRecallsPartial;
        const unsigned short savedAltPartial = doc->m_bAltArrowRecallsPartial;
        doc->m_bArrowsChangeHistory    = 1;
        doc->m_bArrowRecallsPartial    = 0;
        doc->m_bAltArrowRecallsPartial = 0;
        doc->m_bAutoRepeat             = 1;

        auto pressEnter3 = [&]() {
            wxCommandEvent enter(wxEVT_TEXT_ENTER, wxID_HIGHEST + 200 /*ID_INPUT*/);
            enter.SetEventObject(inp);
            child->GetEventHandler()->ProcessEvent(enter);
        };

        sendKey(inp, 'X'); inp->Clear();   // restart the browse cursor

        inp->SetValue(wxT("f4_older"));
        pressEnter3();
        inp->SetValue(wxT("f4_newer"));
        pressEnter3();
        // The bar now holds "f4_newer", SELECTED -- auto-repeat's re-entry
        // (reenterForAutoRepeat: SetSelection(-1,-1), "select all").  Collapse
        // it first, as any caret-moving key (End, Right...) naturally would:
        // the SEPARATE caret-edge gate (CommandCaretAtEdge(), added for a
        // different bug) does not treat a full-string SELECTION as "at the
        // edge", so an uncollapsed selection would block the Up before it
        // ever reached the history-walk code this check is actually about.
        inp->SetInsertionPointEnd();

        sendKey(inp, WXK_UP);
        chkStr(inp->GetValue(), "f4_older",
               "auto repeat: the first Up reaches the OLDER command, not the "
               "one already on screen");

        doc->m_bAutoRepeat             = savedRepeat ? 1 : 0;
        doc->m_bArrowsChangeHistory    = savedArrows;
        doc->m_bArrowRecallsPartial    = savedPartial;
        doc->m_bAltArrowRecallsPartial = savedAltPartial;
    }

    // ── the password-prompt bar swap ───────────────────────────────────────
    // The MUD asks for a password with IAC WILL ECHO; the world timer polls
    // m_bNoEcho and swaps the visible bar for a masked one (and back).  Get it
    // wrong and the password is typed into an echoing control, or survives in a
    // control after the prompt has gone.  Driven through PollNoEcho rather than
    // ApplyNoEcho so the LATCH is under test too: ApplyNoEcho steals focus and
    // clears the masked bar, and only m_lastNoEcho stops the 100 ms tick doing
    // that over and over for as long as echo stays off.
    // Last in this file on purpose: it leaves m_input hidden for a while, and
    // every check above drives that control directly.
    {
        wxTextCtrl* normal = child->NormalInputPublic();
        wxTextCtrl* secret = child->PasswordInputPublic();
        chk(normal != NULL && secret != NULL,
            "the world window has both a normal and a masked command bar");
        chk(secret && (secret->GetWindowStyle() & wxTE_PASSWORD) != 0,
            "...and the second one really masks what is typed");
        chk(normal && secret && normal->IsShown() && !secret->IsShown(),
            "with echo on, the normal bar is the visible one");

        // Half-typed command in the bar when the prompt arrives.
        normal->SetValue(wxT("who is on"));

        doc->m_bNoEcho = true;
        chk(child->PollNoEcho(), "the poll notices the MUD suppressing echo");
        chk(!normal->IsShown() && secret->IsShown(),
            "...and swaps the masked bar in for the normal one");
        chk(child->ActiveInputPublic() == secret,
            "...so the masked bar is what the send path now reads");
        chkStr(secret->GetValue(), "",
               "...and it starts empty, whatever was in it before");
        chkStr(normal->GetValue(), "who is on",
               "...while the half-typed command waits, hidden, as on Windows");

        // The latch.  A second poll with the state unchanged must do NOTHING —
        // asserted on the password text, not just the return value, because a
        // re-apply would silently wipe what the user was typing.
        secret->SetValue(wxT("hunter2"));
        chk(!child->PollNoEcho(),
            "a second poll with echo still off does not re-apply");
        chkStr(secret->GetValue(), "hunter2",
               "...so the half-typed password is left alone");
        chk(secret->IsShown() && !normal->IsShown(),
            "...and the bars stay swapped");

        doc->m_bNoEcho = false;
        chk(child->PollNoEcho(), "the poll notices echo being restored");
        chk(normal->IsShown() && !secret->IsShown(),
            "...and swaps the normal bar back");
        chk(child->ActiveInputPublic() == normal,
            "...so the send path reads the normal bar again");
        chkStr(secret->GetValue(), "",
               "...and the password does NOT survive the swap");
        chkStr(normal->GetValue(), "who is on",
               "...while the command typed before the prompt comes back");
        chk(!child->PollNoEcho(),
            "a second poll with echo back on does not re-apply either");

        // Entering the masked bar clears it too, so nothing a script or an
        // earlier prompt left in it can be submitted as the next answer.
        secret->SetValue(wxT("left over"));
        doc->m_bNoEcho = true;
        chk(child->PollNoEcho(), "echo goes off again");
        chkStr(secret->GetValue(), "",
               "...and the masked bar is wiped on the way IN as well");

        // ── ...and that the TIMER actually calls it ────────────────────────
        // Everything above drives PollNoEcho directly, which tests the
        // function and NOT the wiring: deleting the PollNoEcho() line from
        // OnWorldTimer leaves every assertion above green while a real MUD
        // password prompt would be typed into a visible, echoing control.
        // So this arm touches nothing but m_bNoEcho and lets the world timer
        // find it -- the only assertion here that fails if the call site goes.
        doc->m_bNoEcho = false;
        child->PollNoEcho();               // settle, then stop calling it
        normal->Clear();

        doc->m_bNoEcho = true;             // as IAC WILL ECHO would leave it
        pumpUntil([&]{ return secret->IsShown() && !normal->IsShown(); });
        chk(secret->IsShown() && !normal->IsShown(),
            "the world TIMER swaps the bar, not just a direct poll");

        doc->m_bNoEcho = false;
        pumpUntil([&]{ return normal->IsShown() && !secret->IsShown(); });
        chk(normal->IsShown() && !secret->IsShown(),
            "...and swaps it back when the prompt is over");

        // Leave the frame in the state the rest of the app expects.
        doc->m_bNoEcho = false;
        child->PollNoEcho();
        normal->Clear();
    }

    // ── the password bar-swap RACE: spell check, echo, log and history ─────
    // m_bNoEcho is set the instant the telnet parser sees IAC WILL/WONT ECHO,
    // but the visible swap to and from the masked bar happens in OnWorldTimer,
    // up to 100 ms later.  In the window after the server restores echo, the
    // flag is already false while the masked control is STILL on screen and
    // still focused -- so any gate that tests the flag alone, rather than
    // which CONTROL the text came from, misclassifies a password typed in
    // that window as an ordinary command.  The four checks below drive that
    // race directly: show the masked bar via PollNoEcho, then flip m_bNoEcho
    // back to false WITHOUT polling again, exactly as a server that restores
    // echo just before the port's next 100 ms tick would leave things.
    {
        wxTextCtrl* normal = child->NormalInputPublic();
        wxTextCtrl* secret = child->PasswordInputPublic();

        // ── F1: the spell-check gate ────────────────────────────────────────
        {
            const bool savedSpell = doc->m_bSpellCheckOnSend != 0;
            doc->m_bSpellCheckOnSend = true;

            doc->m_bNoEcho = true;
            child->PollNoEcho();      // show the masked bar
            doc->m_bNoEcho = false;   // ...then the race

            installSpellChecker(spell_correct);
            secret->SetValue(wxT("hunter2"));
            {
                wxCommandEvent enter(wxEVT_TEXT_ENTER);
                enter.SetEventObject(secret);
                secret->GetEventHandler()->ProcessEvent(enter);
            }
            chk(g_spellCalls == 0,
                "F1: a password still in the masked bar is not spell-checked, "
                "even with m_bNoEcho already false");

            // Control: the identical press on the ORDINARY bar (not in the
            // race) is still checked, so a handler that skips spell-check for
            // everyone would pass the assertion above for the wrong reason.
            doc->m_bNoEcho = false;
            child->PollNoEcho();      // swap the ordinary bar back in
            installSpellChecker(spell_correct);
            normal->SetValue(wxT("teh"));
            {
                wxCommandEvent enter(wxEVT_TEXT_ENTER, wxID_HIGHEST + 200 /*ID_INPUT*/);
                enter.SetEventObject(normal);
                child->GetEventHandler()->ProcessEvent(enter);
            }
            chk(g_spellCalls == 1,
                "...while the same press on the ordinary bar is");

            doc->m_bSpellCheckOnSend = savedSpell;
            normal->Clear();
        }

        // ── F2 / F9: echo/log suppression on the send path ──────────────────
        // A "fake connection" (RecordingTransport) is needed here: DoSendMsg
        // and Execute's empty-line arm both bail out via ConnectionOpen() /
        // CheckConnected() before reaching the echo logic otherwise.
        {
            // DisplayMsg (which USER_INPUT echo goes through) is a no-op until
            // m_LinePositions is allocated -- normally done at world load. This
            // doc never went through that, so without this the LineList counts
            // below never move either way and both assertions below would pass
            // vacuously.
            doc->SetUpOutputWindow();

            RecordingTransport wire;
            doc->m_pTransport       = &wire;
            doc->m_iConnectPhase    = eConnectConnectedToMud;
            doc->m_display_my_input = TRUE;

            // F2: a NON-empty password, in the race (m_bNoEcho already false).
            doc->m_bNoEcho = true;
            child->PollNoEcho();      // show the masked bar
            doc->m_bNoEcho = false;   // ...then the race

            const long before = doc->m_LineList.GetCount();
            secret->SetValue(wxT("hunter2"));
            {
                wxCommandEvent enter(wxEVT_TEXT_ENTER);
                enter.SetEventObject(secret);
                secret->GetEventHandler()->ProcessEvent(enter);
            }
            // Drawing and logging are gated by two DIFFERENT flags, so this
            // assertion proves only the drawing half: m_bNoEcho suppresses the
            // echo, while staying out of the log is bOmitFromLog's job (see the
            // isPassword block in wx_world_child.cpp and CSuppressInputLogging
            // in evaluate.cpp).  The log half is covered headlessly by
            // EvaluateArms.OmitFromLogKeepsPasswordOutOfTheSessionLog.
            chk(doc->m_LineList.GetCount() == before,
                "F2: a password typed in the race is not drawn to the output "
                "pane");

            // Control: the identical text typed in the ordinary bar DOES draw
            // a line, so a handler that suppresses ALL echo would pass the
            // assertion above for the wrong reason.
            doc->m_bNoEcho = false;
            child->PollNoEcho();      // swap the ordinary bar back in
            normal->SetValue(wxT("hunter2"));
            {
                wxCommandEvent enter(wxEVT_TEXT_ENTER, wxID_HIGHEST + 200 /*ID_INPUT*/);
                enter.SetEventObject(normal);
                child->GetEventHandler()->ProcessEvent(enter);
            }
            chk(doc->m_LineList.GetCount() > before,
                "...while the same text typed in the ordinary bar is");

            // F9: a BLANK Enter at a masked prompt.  No race needed here --
            // m_bNoEcho stays TRUE throughout, which is exactly what exposed
            // the bug: DoSendMsg (which the old code called directly) never
            // tested m_bNoEcho at all, only SendMsg does.
            doc->m_bNoEcho = true;
            child->PollNoEcho();      // show the masked bar again

            const long beforeBlank = doc->m_LineList.GetCount();
            secret->Clear();
            {
                wxCommandEvent enter(wxEVT_TEXT_ENTER);
                enter.SetEventObject(secret);
                secret->GetEventHandler()->ProcessEvent(enter);
            }
            chk(doc->m_LineList.GetCount() == beforeBlank,
                "F9: a blank Enter at a masked prompt draws no output line");

            // Control: a blank Enter on the ordinary bar (connected, echo on)
            // DOES draw a line (SendMsg's empty-list arm sends one blank
            // line), so a handler that swallows every blank Enter would pass
            // the assertion above for the wrong reason.
            doc->m_bNoEcho = false;
            child->PollNoEcho();      // swap the ordinary bar back in
            normal->Clear();
            {
                wxCommandEvent enter(wxEVT_TEXT_ENTER, wxID_HIGHEST + 200 /*ID_INPUT*/);
                enter.SetEventObject(normal);
                child->GetEventHandler()->ProcessEvent(enter);
            }
            chk(doc->m_LineList.GetCount() > beforeBlank,
                "...while the same blank Enter on the ordinary bar does");

            doc->m_pTransport    = nullptr;
            doc->m_iConnectPhase = eConnectNotConnected;
        }

        // ── F6: world.PushCommand() must not persist a password to disk ─────
        // The same rule OnInputEnter's isPassword gate uses (m_history is
        // persisted to a PLAINTEXT file at close, see :792-798), applied to
        // PushCommand's own write into it.
        {
            // A password string not typed anywhere else in this file -- F2's
            // own checks above push "hunter2" through the ORDINARY bar, and
            // commandList(0) would still find that leftover entry and pass
            // this check for the wrong reason.
            doc->m_bNoEcho = true;
            child->PollNoEcho();      // show the masked bar
            secret->SetValue(wxT("f6_hunter3"));

            std::string ret = child->pushCommand();
            chk(ret == "f6_hunter3",
                "F6: PushCommand still RETURNS the password text");

            bool found = false;
            for (const std::string& c : child->commandList(0))
                if (c == "f6_hunter3") found = true;
            chk(!found, "...but does not record it into the persisted history");

            // Control: the identical text pushed from the ORDINARY bar IS
            // recorded, so an implementation that records nothing at all
            // would pass the assertion above for the wrong reason.
            doc->m_bNoEcho = false;
            child->PollNoEcho();      // swap the ordinary bar back in
            normal->SetValue(wxT("look_f6"));

            std::string ret2 = child->pushCommand();
            chk(ret2 == "look_f6",
                "...and the normal-bar push still returns its text");

            bool found2 = false;
            for (const std::string& c : child->commandList(0))
                if (c == "look_f6") found2 = true;
            chk(found2, "...and this one DOES get recorded");
        }

        // Leave the frame in the state the rest of the app expects.
        doc->m_bNoEcho = false;
        child->PollNoEcho();
        normal->Clear();
    }

    // ── F7: SEND_NOW macros must (conditionally) enter the command history ──
    // do_not_add_macros_to_command_history had no reader at all before this;
    // an unfixed FireMacro leaves BOTH checks below finding nothing, so the
    // second assertion needs the first one to have proven the wiring exists.
    {
        const CString        savedMac   = doc->m_macros[MAC_F7];
        const unsigned short savedType  = doc->m_macro_type[MAC_F7];
        const unsigned short savedDoNot = doc->m_bDoNotAddMacrosToCommandHistory;
        doc->m_bOmitFromCommandHistory  = false;
        doc->m_bNoEcho                  = false;
        doc->m_macro_type[MAC_F7]       = SEND_NOW;

        doc->m_bDoNotAddMacrosToCommandHistory = 0;
        doc->m_macros[MAC_F7] = "f7_macro_on";
        child->FireMacro(MAC_F7);
        auto list1 = child->commandList(0);
        chk(!list1.empty() && list1.front() == "f7_macro_on",
            "F7: a SEND_NOW macro enters the command history by default");

        doc->m_bDoNotAddMacrosToCommandHistory = 1;
        doc->m_macros[MAC_F7] = "f7_macro_off";
        child->FireMacro(MAC_F7);
        bool foundOff = false;
        for (const std::string& c : child->commandList(0))
            if (c == "f7_macro_off") foundOff = true;
        chk(!foundOff,
            "...but not when do_not_add_macros_to_command_history is set");

        doc->m_macros[MAC_F7]                  = savedMac;
        doc->m_macro_type[MAC_F7]              = savedType;
        doc->m_bDoNotAddMacrosToCommandHistory = savedDoNot;
    }

    // ── F8: Display > Clear Command History (ID_DISPLAY_CLEAR_COMMAND_HISTORY,
    // 32343) -- the menu item itself is pinned in --menubar-selftest; this is
    // the confirm-then-wipe handler, driven directly through the confirmation
    // seam (g_wxhook_message_box) both ways, so a handler that wipes
    // regardless of the answer fails the No arm rather than passing by luck.
    {
        int (*savedMB)(const char*, const char*, unsigned) = g_wxhook_message_box;
        static int s_clearAnswer  = IDNO;
        static int s_clearPrompts = 0;
        g_wxhook_message_box = [](const char*, const char*, unsigned) -> int {
            ++s_clearPrompts;
            return s_clearAnswer;
        };

        child->deleteCommandHistory();   // start from a known-empty state
        child->addToCommandHistory("f8_one");
        child->addToCommandHistory("f8_two");
        chk(!child->commandList(0).empty(),
            "F8: the history has entries to clear before the test starts");

        s_clearAnswer  = IDNO;
        s_clearPrompts = 0;
        child->ClearCommandHistory();
        chk(s_clearPrompts == 1, "...ClearCommandHistory asks for confirmation");
        chk(!child->commandList(0).empty(),
            "...and answering No leaves the history alone");

        s_clearAnswer = IDYES;
        child->ClearCommandHistory();
        chk(s_clearPrompts == 2, "...asks again");
        chk(child->commandList(0).empty(),
            "...and answering Yes empties the in-memory history");

        const std::string& histPath = child->HistoryFilePath();
        if (!histPath.empty()) {
            if (FILE* f = fopen(histPath.c_str(), "rb")) {
                fseek(f, 0, SEEK_END);
                const long sz = ftell(f);
                fclose(f);
                chk(sz == 0, "...and the persisted history file is emptied too");
            }
        }

        g_wxhook_message_box = savedMB;
    }

    // ── BUG-2 (GUI drive run 2026-08-12/13): the command-bar Ctrl+click ────
    // alias menu. On macOS a Control+click on a native NSTextField/NSTextView
    // is intercepted by AppKit before it ever becomes a wxEVT_LEFT_DOWN --
    // routed instead to menuForEvent:, which wx surfaces as
    // wxEVT_CONTEXT_MENU -- so the port's original Ctrl+click handling (bound
    // only to LEFT_DOWN, in BuildInputBar) silently never ran on this
    // control; the native Look Up/Cut/Copy/Paste menu won every time.
    //
    // wxContextMenuEvent carries no modifier flags of its own, so the real
    // fix has to poll wxGetKeyState(WXK_RAW_CONTROL) live -- something a
    // headless/in-app selftest cannot fake without synthesising real OS
    // keyboard input (out of reach here; see the port's own notes on
    // Escape-dependent paths being unreachable by automation). What CAN be
    // pinned without touching live OS state, and without risking a REAL
    // native or custom popup menu blocking this selftest forever (there is
    // no running event loop yet to dismiss one), is the decision rule the
    // fix's context-menu handler consults -- so it is split out.
    // "Did nothing" fails both checks below: an unfixed/reverted rule would
    // return the SAME answer regardless of rawControlDown.
    chk(child->WantsInputBarAliasMenu(true) == true,
        "command bar: Ctrl+ (context-menu) click wants the alias menu");
    chk(child->WantsInputBarAliasMenu(false) == false,
        "...and a plain right-click still defers to the native edit menu");

    printf("INPUT-SELFTEST: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

#endif // !_WIN32
