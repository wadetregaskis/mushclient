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
#include "platform/wx_mfc_hooks.h"    // g_wxhook_message_box

#include <cstdio>

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

} // namespace

// Let the frame's own 100 ms world timer run.  Used by the no-echo arm to
// prove the timer CALLS the poll, which driving the poll directly cannot.
static void pumpWorldTicks(wxWindow* w, int ms)
{
    (void) w;
    const wxLongLong until = wxGetLocalTimeMillis() + ms;
    while (wxGetLocalTimeMillis() < until) {
        wxYield();
        wxMilliSleep(10);
    }
    wxYield();
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
    App.m_pWorldDocTemplate->AddDocument(doc);
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
        pumpWorldTicks(child, 350);        // the timer runs every 100 ms
        chk(secret->IsShown() && !normal->IsShown(),
            "the world TIMER swaps the bar, not just a direct poll");

        doc->m_bNoEcho = false;
        pumpWorldTicks(child, 350);
        chk(normal->IsShown() && !secret->IsShown(),
            "...and swaps it back when the prompt is over");

        // Leave the frame in the state the rest of the app expects.
        doc->m_bNoEcho = false;
        child->PollNoEcho();
        normal->Clear();
    }

    printf("INPUT-SELFTEST: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

#endif // !_WIN32
