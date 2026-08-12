// platform/notepad_selftest.cpp — in-app conformance test for the notepad
// windows (--notepad-selftest).
//
// The notepad is GUI-only: CTextDocument lives in mushgui_adapters and its
// window is a real wxFrame, so none of it links into the 30 headless test
// executables.  This harness drives the REAL objects — the real
// hook_create_text_window, the real CWxNotepadFrame, the real
// CTextDocument::SaveModified, the real quit hook — inside the app binary, then
// exits before the GUI boots, exactly like the PAL/engine selftests.
//
// It covers three behaviours the port had wrong against the canonical
// Windows/MFC source:
//   1. the document's modified flag: set where CreateTextWindow (Utilities.cpp)
//      and CEditView's EN_CHANGE handler set it, and consulted by SaveModified
//      the way CDocument::SaveModified consults it;
//   2. the Convert menu's end-of-line transforms, which are written against a
//      CRLF CEdit buffer and must be given one (notepad_convert_shared.cpp);
//   3. quitting: MFC's SaveAllModified walks the text-document template too, so
//      the port's quit hook must as well.
//
// Every prompt is answered by a stub installed over g_wxhook_message_box, which
// is also what makes "did it ask?" observable — a passing run must never put a
// modal on screen.

#ifndef _WIN32

#include <wx/wx.h>
#include <wx/menu.h>

#include "../stdafx.h"
#include "../MUSHclient.h"
#include "../TextDocument.h"
#include "../doc.h"

#include "wx_notepad.h"
#include "wx_mfc_hooks.h"

#include <cstdio>
#include <string>

namespace {

int g_npPass = 0, g_npFail = 0;
void npCheck (bool ok, const char* what)
{
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    (ok ? g_npPass : g_npFail)++;
}

// ── Message-box stub ───────────────────────────────────────────────────────
// Counts prompts and answers them without a window.  UMessageBox routes here
// (platform/mushapp_stubs.cpp) whenever g_wxhook_message_box is set.
int  g_promptCount  = 0;
int  g_promptAnswer = IDNO;
CString g_lastPrompt;

int CountingMessageBox (const char* text, const char* /*caption*/, unsigned /*flags*/)
{
    g_promptCount++;
    g_lastPrompt = text ? text : "";
    return g_promptAnswer;
}

// ── Notepad construction ───────────────────────────────────────────────────

// Build a notepad through the REAL creation hook and return its document.
// Hidden immediately: the assertions never need it on screen, and a ctest run
// should not throw windows at the user.
CTextDocument* MakeNotepad (const char* title, const char* text)
{
    if (!g_wxhook_create_text_window)
        return nullptr;
    void* pv = g_wxhook_create_text_window(
        text, title,
        nullptr,          // no related world
        1,                // unique document number
        "", 0, 0, 0,      // font: defaults
        0, 0,             // colours: defaults
        "", "",           // no search string / recall preamble
        false, false, false, false, false,
        0,
        eNotepadNormal);
    if (CWxNotepadFrame* f = CWxNotepadFrame::Find(title))
        f->Hide();
    return static_cast<CTextDocument*>(pv);
}

// Fire a Convert-menu command on a notepad by its menu item's label text, so
// the assertion goes through the real event table / real handler rather than a
// private entry point.  Returns false if the item is not there.
bool FireConvert (CWxNotepadFrame* frame, const char* labelSubstring)
{
    wxMenuBar* mb = frame->GetMenuBar();
    if (!mb) return false;
    const int menuIdx = mb->FindMenu(wxT("Convert"));
    if (menuIdx == wxNOT_FOUND) return false;
    wxMenu* menu = mb->GetMenu(menuIdx);
    if (!menu) return false;
    for (size_t i = 0; i < menu->GetMenuItemCount(); ++i)
    {
        wxMenuItem* item = menu->FindItemByPosition(i);
        if (!item || item->IsSeparator()) continue;
        if (item->GetItemLabelText().Contains(wxString::FromUTF8(labelSubstring)))
        {
            wxCommandEvent evt(wxEVT_MENU, item->GetId());
            evt.SetEventObject(frame);
            frame->GetEventHandler()->ProcessEvent(evt);
            return true;
        }
    }
    return false;
}

} // namespace

int RunNotepadSelfTest()
{
    printf("NOTEPAD-SELFTEST: driving the real notepad windows\n");

    // The app's InitInstance never runs here, so make the document template
    // CreateTextWindow needs.  Same class and id InitInstance uses.
    if (!App.m_pNormalDocTemplate)
        App.m_pNormalDocTemplate =
            new CMultiDocTemplate(IDR_NORMALTYPE, nullptr, nullptr, nullptr);

    g_wxhook_message_box = CountingMessageBox;

    // ── 1. Creation sets the modified flag exactly where Windows does ───────
    // Utilities.cpp CreateTextWindow: "nothing to put there" returns BEFORE
    // SetModifiedFlag(TRUE), so an empty notepad starts clean and one handed
    // content starts dirty.
    CTextDocument* withText = MakeNotepad("selftest_np_text", "hello world\n");
    npCheck(withText != nullptr, "CreateTextWindow returns a document");
    if (!withText)
    {
        printf("NOTEPAD-SELFTEST: cannot continue without a document\n");
        return 1;
    }
    npCheck(withText->IsModified(), "a notepad created WITH text starts modified");

    CTextDocument* empty = MakeNotepad("selftest_np_empty", "");
    npCheck(empty && !empty->IsModified(),
            "an EMPTY notepad starts unmodified");

    // ── 2. SaveModified consults the modified flag ──────────────────────────
    // CDocument::SaveModified (which the Windows arm falls through to) returns
    // TRUE immediately when the document is clean.  The port used to prompt
    // unconditionally, so closing a freshly-opened or freshly-saved notepad
    // always asked.
    g_promptCount = 0;
    g_promptAnswer = IDNO;
    npCheck(withText->SaveModified() == TRUE,
            "SaveModified on a modified notepad returns TRUE when told No");
    npCheck(g_promptCount == 1, "...and it DID ask (exactly once)");

    withText->SetModifiedFlag(FALSE);
    g_promptCount = 0;
    npCheck(withText->SaveModified() == TRUE,
            "SaveModified on an unmodified notepad returns TRUE");
    npCheck(g_promptCount == 0, "...without asking anything");

    // Cancel still cancels.
    withText->SetModifiedFlag(TRUE);
    g_promptCount = 0;
    g_promptAnswer = IDCANCEL;
    npCheck(withText->SaveModified() == FALSE,
            "SaveModified returns FALSE when the user cancels");
    npCheck(g_promptCount == 1, "...having asked once");
    g_promptAnswer = IDNO;

    // The per-notepad settings still short-circuit ahead of the flag, as they
    // do on Windows: a Recall window closes without asking even when dirty.
    withText->SetModifiedFlag(TRUE);
    withText->m_iNotepadType = eNotepadRecall;
    g_promptCount = 0;
    npCheck(withText->SaveModified() == TRUE && g_promptCount == 0,
            "a Recall notepad still closes without asking");
    withText->m_iNotepadType = eNotepadNormal;

    // ── 3. Edits mark the document modified ────────────────────────────────
    // CEditView reflects EN_CHANGE to SetModifiedFlag; the port binds
    // wxEVT_TEXT, which is what typing, paste, the Convert menu and
    // world.AppendToNotepad's WriteText all raise.  Without it the flag would
    // never become TRUE and check 2 above would silently discard real edits.
    withText->SetModifiedFlag(FALSE);
    npCheck(!withText->IsModified(), "flag cleared before the edit");
    if (withText->m_fn_replace_sel)
        withText->m_fn_replace_sel(std::string("appended by a script\n"));
    npCheck(withText->IsModified(),
            "text written into the notepad marks the document modified");

    // ── 3b. Legacy-charset bytes are WRITTEN, not dropped (2026-08-12) ─────
    // m_fn_replace_sel decoded with all-or-nothing wxString::FromUTF8, so
    // world.AppendToNotepad("caf\xE9") wrote NOTHING while returning true —
    // and world.ReplaceNotepad, which selects all first, replaced the whole
    // notepad with that nothing.  Now it decodes leniently, like every other
    // write path in this file.
    {
        CWxNotepadFrame* frame = CWxNotepadFrame::Find("selftest_np_text");
        npCheck(frame != nullptr, "the text notepad's frame is findable");
        if (frame && withText->m_fn_replace_sel && withText->m_fn_set_sel)
        {
            const size_t before = frame->GetText().size();
            withText->m_fn_replace_sel(std::string("caf\xE9 latte\n"));
            const std::string after = frame->GetText();
            npCheck(after.size() > before,
                    "ill-formed UTF-8 still appends (it used to write nothing)");
            npCheck(after.find("caf") != std::string::npos &&
                    after.find("latte") != std::string::npos,
                    "...with the ASCII part of the text intact");

            // The replace arm: select-all + write must REPLACE, not erase.
            withText->m_fn_set_sel(0, -1);
            withText->m_fn_replace_sel(std::string("nouveau caf\xE9\n"));
            const std::string replaced = frame->GetText();
            npCheck(!replaced.empty() &&
                    replaced.find("nouveau") != std::string::npos,
                    "ReplaceNotepad-shape write with legacy bytes replaces "
                    "(it used to erase the notepad)");
        }
    }

    // ── 4. The Convert menu's end-of-line commands ─────────────────────────
    // wxOSX rewrites every CR to LF on the way into the control, so the
    // canonical CR-keyed transforms have to be given a CRLF buffer.  Assert the
    // wx fact first — the whole boundary conversion rests on it.
    {
        CTextDocument* conv = MakeNotepad("selftest_np_convert", "a\r\nb\r\nc\n");
        CWxNotepadFrame* frame = CWxNotepadFrame::Find("selftest_np_convert");
        npCheck(conv != nullptr && frame != nullptr, "convert notepad created");
        if (frame)
        {
            npCheck(frame->GetText().find('\r') == std::string::npos,
                    "the wx control cannot hold a carriage return (CR became LF)");

            frame->AppendText("one\ntwo\nthree", true /*replace*/);
            npCheck(FireConvert(frame, "Remove end-of-lines"),
                    "Convert menu has 'Remove end-of-lines'");
            npCheck(frame->GetText() == "one two three",
                    "Remove end-of-lines joins the lines (it used to do nothing)");

            frame->AppendText("a\nb\nc", true /*replace*/);
            // ASCII-only fragment of "DOS -> Mac (CRLF -> CR)"; the label's
            // arrows are not representable in a narrow literal here.
            npCheck(FireConvert(frame, "Mac (CRLF"),
                    "Convert menu has the DOS to Mac conversion");
            npCheck(frame->GetText() == "a\nb\nc",
                    "DOS to Mac keeps the line structure (it used to delete every break)");
        }
    }

    // ── 5. Quitting asks about unsaved notepads ────────────────────────────
    // -[NSApp terminate:] skips MFC's document flow entirely, so the port's
    // quit hook stands in for CWinApp::SaveAllModified — which walks EVERY
    // registered doc template, the text one included.  It used to walk worlds
    // only, and returned early when there was no world template at all.
    npCheck(g_wxhook_query_quit_save != nullptr, "the quit hook is installed");
    if (g_wxhook_query_quit_save)
    {
        // Everything clean → quit proceeds, silently.
        POSITION pos = App.m_pNormalDocTemplate->GetFirstDocPosition();
        while (pos)
            if (CDocument* d = App.m_pNormalDocTemplate->GetNextDoc(pos))
                d->SetModifiedFlag(FALSE);
        g_promptCount = 0;
        g_lastPrompt = "";
        npCheck(g_wxhook_query_quit_save() == 1, "quit allowed with nothing modified");
        npCheck(g_promptCount == 0, "...and nothing was asked");

        // One dirty notepad → exactly one prompt, and No still quits.
        withText->SetModifiedFlag(TRUE);
        g_promptCount = 0;
        g_lastPrompt = "";   // so the name check below cannot pass on a stale value
        g_promptAnswer = IDNO;
        npCheck(g_wxhook_query_quit_save() == 1, "quit allowed after answering No");
        npCheck(g_promptCount == 1,
                "quitting ASKED about the unsaved notepad (it used to discard it)");
        npCheck(strstr((LPCTSTR) g_lastPrompt, "selftest_np_text") != nullptr,
                "...about that notepad by name");

        // Cancel at that prompt must cancel the quit.
        withText->SetModifiedFlag(TRUE);
        g_promptCount = 0;
        g_promptAnswer = IDCANCEL;
        npCheck(g_wxhook_query_quit_save() == 0, "Cancel at the prompt cancels the quit");
        npCheck(g_promptCount == 1, "...having asked once");
        withText->SetModifiedFlag(FALSE);
        g_promptAnswer = IDNO;
    }

    printf("NOTEPAD-SELFTEST: %d passed, %d failed\n", g_npPass, g_npFail);
    return g_npFail;
}

#endif // !_WIN32
