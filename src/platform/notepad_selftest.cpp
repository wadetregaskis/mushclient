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
#include <wx/mdi.h>

#include "../stdafx.h"
#include "../MUSHclient.h"
#include "../TextDocument.h"
#include "../doc.h"

#include "wx_notepad.h"
#include "wx_mfc_hooks.h"
#include "wx_world_child.h"
#include "../pal/itransport.h"

#include <algorithm>   // std::sort / std::unique (the Convert id-distinctness check)
#include <cstdio>
#include <string>
#include <vector>

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

// Read a file's raw bytes, for the line-ending round-trip checks.  Deliberately
// NOT via NotepadReadFile: that is one of the functions under test here, and
// comparing its output with itself would make the assertion vacuous.
std::string SlurpFile (const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) return "<unreadable>";
    std::string out;
    char buf[4096];
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, f)) > 0)
        out.append(buf, got);
    fclose(f);
    return out;
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

// Find a menu item anywhere on the bar by its containing menu's label and a
// substring of the item's own label.  Returns nullptr if either lookup fails.
wxMenuItem* FindMenuItem (CWxNotepadFrame* frame, const char* menuLabel,
                          const char* labelSubstring)
{
    wxMenuBar* mb = frame->GetMenuBar();
    if (!mb) return nullptr;
    const int menuIdx = mb->FindMenu(wxString::FromUTF8(menuLabel));
    if (menuIdx == wxNOT_FOUND) return nullptr;
    wxMenu* menu = mb->GetMenu(menuIdx);
    if (!menu) return nullptr;
    for (size_t i = 0; i < menu->GetMenuItemCount(); ++i)
    {
        wxMenuItem* item = menu->FindItemByPosition(i);
        if (item && !item->IsSeparator() &&
            item->GetItemLabelText().Contains(wxString::FromUTF8(labelSubstring)))
            return item;
    }
    return nullptr;
}

// Ask the frame's real wxEVT_UPDATE_UI handler what it would do with a menu
// item, without an event loop or a menu ever actually opening (the same
// no-loop technique platform/menubar_selftest.cpp uses).
struct UiAnswer {
    bool claimed = false;
    bool enabled = true;
    std::string text;
};

UiAnswer ProbeUi (CWxNotepadFrame* frame, int id)
{
    wxUpdateUIEvent evt(id);
    evt.SetEventObject(frame);
    UiAnswer a;
    a.claimed = frame->GetEventHandler()->ProcessEvent(evt);
    a.enabled = evt.GetEnabled();
    a.text    = std::string(evt.GetText().utf8_str());
    return a;
}

// A pal::ITransport that just records what was sent, so a notepad's Send To
// World can be proven to actually reach the wire -- the same recipe
// tests/test_connection.cpp's RecTransport uses, headless.
struct RecordingTransport : pal::ITransport {
    std::string sent;
    void resolve (const char*, pal::ResolveToken) override {}
    void cancelResolve () override {}
    void connect (unsigned int, unsigned short, const pal::TransportOptions&) override {}
    void send (const char* data, size_t n) override { sent.append(data, n); }
    void startTls (const pal::TlsParams&) override {}
    void close () override {}
    std::string lastErrorString () const override { return "rec"; }
};

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

    // ── 3b2. GetNotepadLength counts UTF-8 BYTES, not wx characters (X04-F5) ─
    // world.GetNotepadText returns UTF-8 bytes (methods_notepad.cpp), so
    // world.GetNotepadLength must agree with it in bytes, not the wx text
    // control's own character count -- otherwise a well-formed multi-byte
    // notepad reports a length that does not match strlen() of its own text.
    // This drives the real product seam (platform/wx_notepad.cpp) directly,
    // headless coverage cannot reach it (tests/notepad_fixture.h's fixture is
    // already byte-based and never touches this file).  Reuses the "empty"
    // notepad from check 1 above (otherwise idle from here on) rather than
    // opening a new window -- this harness runs entirely inside wxApp::OnInit,
    // with no event loop pumping between window operations, and each extra
    // top-level frame measurably adds to that fragility.
    {
        CWxNotepadFrame* utf8Frame = CWxNotepadFrame::Find("selftest_np_empty");
        npCheck(empty != nullptr && utf8Frame != nullptr,
                "the empty notepad is reusable for the byte-length check");
        if (empty && empty->m_fn_get_text_length && empty->m_fn_set_sel &&
            empty->m_fn_replace_sel)
        {
            empty->m_fn_set_sel(0, -1);
            empty->m_fn_replace_sel(std::string("caf\xC3\xA9"));   // "café"

            const int len = empty->m_fn_get_text_length();
            npCheck(len == 5,
                    "the length callback reports UTF-8 BYTES, not wx "
                    "characters (\"caf\\xC3\\xA9\" is 4 characters, 5 bytes; "
                    "it used to report 4)");

            // AppendToTheNotepad's own append arm (methods_notepad.cpp:117-121):
            // get the length, select (length, length), write.  Must still land
            // at the true end now that the length is in bytes.
            empty->m_fn_set_sel(len, len);
            empty->m_fn_replace_sel(std::string(" latte"));
            npCheck(utf8Frame && utf8Frame->GetText() == "caf\xC3\xA9 latte",
                    "...and an append using that length still lands at the "
                    "true end of the text");
        }
    }

    // ── 3c. Search / navigation commands (2026-08-13) ──────────────────────
    // The canonical CTextView commands the bar lacked entirely — the whole
    // Search popup among them (the bite: Plugins ▸ Edit opens plugin .lua
    // here, unsearchable).  Menu presence asserted so "menu absent" FAILS;
    // the core ops driven directly (their dialogs are modal/modeless UI).
    {
        CWxNotepadFrame* frame = CWxNotepadFrame::Find("selftest_np_text");
        npCheck(frame != nullptr, "search: the fixture frame is findable");
        if (frame)
        {
            wxMenuBar* mb = frame->GetMenuBar();
            npCheck(mb && mb->FindMenu(wxT("Search")) != wxNOT_FOUND,
                    "the notepad has a Search menu (it had none)");

            frame->AppendText("alpha beta gamma beta\n", true /*replace*/);
            npCheck(frame->FindNextInText(wxT("beta"), false),
                    "Find finds a term");
            npCheck(frame->FindNextInText(wxT("beta"), false),
                    "Find Next advances to the second hit");
            npCheck(!frame->FindNextInText(wxT("zzz"), false),
                    "a missing term reports not-found");
            npCheck(frame->FindNextInText(wxT("BETA"), false),
                    "...case-insensitively by default");
            npCheck(!frame->FindNextInText(wxT("BETA"), true),
                    "...but not when match-case is on (control)");

            npCheck(frame->ReplaceAllInText(wxT("beta"), wxT("BETA"), true) == 2,
                    "Replace All counts its replacements");
            npCheck(frame->GetText().find("BETA gamma BETA")
                        != std::string::npos,
                    "...and really replaced both");

            // Go To Line + Insert Date/Time in one observable: the stamp
            // lands at the START of line 2, between aaa and bbb.
            frame->AppendText("aaa\nbbb\nccc\n", true /*replace*/);
            frame->GoToLine(2);
            frame->InsertDateTime();
            const std::string txt = frame->GetText();
            // The stamp is the only ':' in the fixture (the time HH:MM).
            const size_t iA = txt.find("aaa"), iY = txt.find(":"),
                         iB = txt.find("bbb");
            npCheck(iA != std::string::npos && iY != std::string::npos &&
                        iB != std::string::npos && iA < iY && iY < iB,
                    "Go To Line 2 + Insert Date/Time lands between the lines");

            frame->AppendText("one two\nthree\n", true /*replace*/);
            npCheck(frame->WordCountSummary().Contains(wxT("3 words")),
                    "Word Count counts words");
            npCheck(frame->WordCountSummary().Contains(wxT("3 lines")),
                    "...and lines, the canonical way");
        }
    }

    // ── 3d. Send To World (2026-08-15, BUG-5) ───────────────────────────────
    // CTextView::OnEditSendtoworld only ever fires for a notepad with a
    // related world (CTextDocument::FindWorld), and CTextView::OnUpdateEdit-
    // Sendtoworld disables + relabels the item when there is none.  The port
    // left the notepad's own Send To World menu item unconditionally enabled
    // with no update-UI handler at all, so a standalone notepad's copy looked
    // exactly as live as a world-tied one and silently did nothing when
    // clicked: verified live against a real connected world by selecting text
    // and invoking Send To World, twice (menu click and Shift+Cmd+S), with
    // server.log unchanged and no error or dialog either time.
    {
        // InitInstance never ran, so give the app the world doc template
        // FindWorld() walks — same recipe this file already uses for the
        // (separate) notepad/text template above.
        if (!App.m_pWorldDocTemplate)
            App.m_pWorldDocTemplate =
                new CMultiDocTemplate(IDR_MUSHCLTYPE, nullptr, nullptr, nullptr);

        RecordingTransport* wire = new RecordingTransport();
        CMUSHclientDoc* world = new CMUSHclientDoc();
        world->m_mush_name       = "zz_selftest_sendtoworld";
        world->m_iConnectPhase   = eConnectConnectedToMud;
        world->m_pTransport      = wire;
        world->m_bConfirmOnPaste = FALSE;   // must not raise a modal here
        world->SetUpOutputWindow();

        // CWorldChildFrame's constructor is what registers the doc with
        // App.m_pWorldDocTemplate (mirrors what hook_new_world does) --
        // without it FindWorld() would never find `world` at all.
        wxMDIParentFrame* parent = new wxMDIParentFrame(
            nullptr, wxID_ANY, wxT("selftest_stw_parent"),
            wxPoint(60, 60), wxSize(400, 300));
        new CWorldChildFrame(parent, world);

        // A notepad with NO related world -- exactly what Edit > New Notepad
        // makes.  The control case.
        CTextDocument* stray = MakeNotepad("selftest_np_stray", "unsent text");
        CWxNotepadFrame* strayFrame = CWxNotepadFrame::Find("selftest_np_stray");
        npCheck(stray != nullptr && strayFrame != nullptr,
                "stray (world-less) notepad created");
        if (strayFrame)
        {
            wxMenuItem* item = FindMenuItem(strayFrame, "Edit", "Send To");
            npCheck(item != nullptr, "the notepad has a Send To World item");
            if (item)
            {
                UiAnswer ui = ProbeUi(strayFrame, item->GetId());
                npCheck(ui.claimed, "...and its update-UI is answered");
                npCheck(!ui.enabled,
                        "...disabled with no related world (it used to stay enabled)");
                npCheck(ui.text.find("Send To") != std::string::npos &&
                        ui.text.find("zz_selftest") == std::string::npos,
                        "...with the generic caption (not a world's name)");

                // Clicking it anyway (a stale menu, direct dispatch) must
                // still be a harmless no-op, not a send.
                wxCommandEvent cmd(wxEVT_MENU, item->GetId());
                cmd.SetEventObject(strayFrame);
                strayFrame->GetEventHandler()->ProcessEvent(cmd);
                npCheck(wire->sent.empty(),
                        "...and invoking it anyway sends nothing");
            }
        }

        // A notepad WITH a related world -- what Flip To Notepad already
        // makes correctly, and the exact BUG-5 repro shape once New Notepad
        // is opened against a connected world.
        void* pv = g_wxhook_create_text_window(
            "hello from the notepad", "selftest_np_worldbound",
            world, world->m_iUniqueDocumentNumber,
            "", 0, 0, 0, 0, 0, "", "",
            false, false, false, false, false, 0, eNotepadNormal);
        CWxNotepadFrame* boundFrame = CWxNotepadFrame::Find("selftest_np_worldbound");
        npCheck(pv != nullptr && boundFrame != nullptr,
                "world-bound notepad created");
        if (boundFrame)
        {
            wxMenuItem* item = FindMenuItem(boundFrame, "Edit", "Send To");
            npCheck(item != nullptr, "...has a Send To World item too");
            if (item)
            {
                UiAnswer ui = ProbeUi(boundFrame, item->GetId());
                npCheck(ui.enabled,
                        "enabled once the notepad has a related, connected world");
                npCheck(ui.text.find("zz_selftest_sendtoworld") != std::string::npos,
                        "...captioned with the world's own name");

                wxCommandEvent cmd(wxEVT_MENU, item->GetId());
                cmd.SetEventObject(boundFrame);
                boundFrame->GetEventHandler()->ProcessEvent(cmd);
                npCheck(wire->sent.find("hello from the notepad") != std::string::npos,
                        "clicking it sends the notepad's text to the world "
                        "(BUG-5's repro: it used to reach nowhere)");
            }
        }

        // GetNotepadWindowPosition reports the EXACT width/height
        // MoveNotepadWindow set (E04-F2).  m_fn_get_rect used to emit
        // wxRect's INCLUSIVE right/bottom, but both consumers of the string
        // (this BSTR-formatted method and Lua's own
        // L_GetNotepadWindowPosition) subtract left/top from right/bottom as
        // if this were a Win32 RECT, whose right/bottom are EXCLUSIVE -- so
        // the reported width/height came back one pixel short.
        if (boundFrame)
        {
            npCheck(world->MoveNotepadWindow("selftest_np_worldbound",
                                             60, 80, 300, 200) != FALSE,
                    "MoveNotepadWindow moves the world-bound notepad");
            // NO wxYield() here, and none anywhere else in this harness.
            // The loop-free entry point (--notepad-selftest) exits from
            // wxApp::OnInit, so no event loop is active; wxYield() then
            // builds a TEMPORARY one (wxAppConsoleBase::Yield) and pumps
            // AppKit events into a half-booted app, which corrupts the
            // malloc tiny-zone free list.  That was this harness's
            // intermittent SIGSEGV/SIGABRT: ~2 runs in 12 normally, and
            // 6 of 6 under Guard Malloc, which selftest_notepad_loopfree
            // now pins.  Nothing needs the yield: m_fn_move_window is
            // wxWindow::SetSize and m_fn_get_rect is wxWindow::GetRect,
            // both applied synchronously on this side of the window
            // server, so the read-back below is already exact.

            BSTR bstrPos = world->GetNotepadWindowPosition("selftest_np_worldbound");
            CString pos = bstrPos;
            SysFreeString(bstrPos);
            npCheck(pos == "60,80,300,200",
                    "GetNotepadWindowPosition reports the EXACT width/height "
                    "MoveNotepadWindow set (it used to read one pixel short)");
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

    // ── 4a2. The four line-ending conversions actually convert (D-10) ──────
    // They were provable no-ops: the control cannot hold a CR, so the
    // canonical CR-keyed transforms had nothing to rewrite, and the only
    // assertion on them ("the text came back unchanged") was equally true of a
    // body that did nothing.  Now they change the DOCUMENT's line-ending tag,
    // and the discriminator is what lands on disk.
    //
    // Driven through the real menu items, so the event table, the id mapping
    // and OnConvert's new arm are all in the path.
    {
        // ASCII-only fragments of the menu labels, which carry "→" arrows.
        // Each is unique across the four: "(LF" only appears in Unix→DOS,
        // "Unix (CRLF" only in DOS→Unix, "(CR " only in Mac→DOS (DOS→Mac's
        // "(CRLF" has an L where that space is), "Mac (CRLF" only in DOS→Mac.
        struct ConvCase { const char* leaf; const char* label; const char* expected; };
        const ConvCase kCases[] = {
            { "u2d", "(LF",        "l1\r\nl2\r\n" },   // Unix → DOS
            { "d2u", "Unix (CRLF", "l1\nl2\n"     },   // DOS  → Unix
            { "m2d", "(CR ",       "l1\r\nl2\r\n" },   // Mac  → DOS
            { "d2m", "Mac (CRLF",  "l1\rl2\r"     },   // DOS  → Mac
        };
        std::vector<int> firedIds;

        for (const ConvCase& c : kCases)
        {
            // A distinct file per case: same starting state every time, and no
            // frame has to be closed and reopened under a title the registry
            // would still be holding.
            char path[128], leafname[64];
            snprintf(path, sizeof path,
                     "/tmp/mushclient_np_selftest_conv_%s.txt", c.leaf);
            snprintf(leafname, sizeof leafname,
                     "mushclient_np_selftest_conv_%s.txt", c.leaf);

            FILE* fp = fopen(path, "wb");
            if (fp) { fputs("l1\nl2\n", fp); fclose(fp); }

            CTextDocument* td = g_wxhook_open_text_file
                ? static_cast<CTextDocument*>(g_wxhook_open_text_file(path))
                : nullptr;
            CWxNotepadFrame* cf = CWxNotepadFrame::Find(leafname);
            if (cf) cf->Hide();
            npCheck(td != nullptr && cf != nullptr,
                    "the convert fixture opens into a notepad");
            if (!td || !cf)
            {
                remove(path);
                continue;
            }

            npCheck(td->m_iLineEndings == eLineEndingsLF,
                    "...starting as a Unix file");
            td->SetModifiedFlag(FALSE);

            // Which item the label fragment actually resolves to.  Two of the
            // four cases expect the SAME bytes (both DOS targets), so without
            // this a fragment that matched the wrong item would still pass —
            // the ids are collected and checked for distinctness below.
            wxMenuItem* item = FindMenuItem(cf, "Convert", c.label);
            npCheck(item != nullptr, "the Convert item is on the menu");
            if (item)
                firedIds.push_back(item->GetId());

            const std::string before = cf->GetText();
            npCheck(FireConvert(cf, c.label), "...and fires");

            // A tag-only change raises no wxEVT_TEXT, so nothing sets the
            // modified flag for it -- and SaveModified would then throw the
            // user's conversion away on close without even asking.
            npCheck(td->IsModified(),
                    "...and it marks the document modified (no wxEVT_TEXT "
                    "fires for a tag-only change)");
            npCheck(cf->GetText() == before,
                    "...without touching the text");

            npCheck(td->DoSave((LPCTSTR) td->GetPathName()) != FALSE,
                    "...the converted notepad saves");
            npCheck(SlurpFile(path) == c.expected,
                    "...and the SAVED BYTES carry the new line endings "
                    "(all four used to be no-ops)");

            td->SetModifiedFlag(FALSE);   // nothing to prompt about at quit
            remove(path);
        }

        // Four labels, four DIFFERENT menu items.  Without this, "(LF" and
        // "(CR " could both be resolving to Unix→DOS and the two DOS cases
        // would still agree with their (identical) expected bytes.
        std::vector<int> sorted = firedIds;
        std::sort(sorted.begin(), sorted.end());
        npCheck(sorted.size() == 4 &&
                std::unique(sorted.begin(), sorted.end()) == sorted.end(),
                "the four conversions are four distinct menu commands");
    }

    // ── 4b. File > New resets a Recall/search-results title (E19-F2) ───────
    // CTextDocument::OnFileNew's guard (TextDocument.cpp:460): don't make
    // another Recall window -- a fresh notepad opened from within a Recall/
    // search-results one must reset the title to the generic form, not
    // inherit the Recall notepad's own title.  The port's OnMenuNew used to
    // reproduce OnFileNew's body without this guard.
    {
        void* pv = g_wxhook_create_text_window(
            "recalled text", "selftest_np_recall",
            nullptr, 2,                // no related world, distinct doc number
            "", 0, 0, 0, 0, 0,
            "xyz", "",                 // non-empty search string -> Recall shape
            false, false, false, false, false, 0, eNotepadRecall);
        CWxNotepadFrame* recallFrame = CWxNotepadFrame::Find("selftest_np_recall");
        npCheck(pv != nullptr && recallFrame != nullptr,
                "Recall-shaped notepad created");
        if (recallFrame)
        {
            recallFrame->Hide();
            wxMenuItem* item = FindMenuItem(recallFrame, "File", "New notepad");
            npCheck(item != nullptr, "the notepad has a File > New notepad item");
            if (item)
            {
                wxCommandEvent cmd(wxEVT_MENU, item->GetId());
                cmd.SetEventObject(recallFrame);
                recallFrame->GetEventHandler()->ProcessEvent(cmd);

                CWxNotepadFrame* freshFrame = CWxNotepadFrame::Find("Untitled");
                npCheck(freshFrame != nullptr,
                        "File > New from a Recall notepad resets the title to "
                        "\"Untitled\" (it used to inherit the Recall title)");
                if (freshFrame) freshFrame->Hide();
            }
        }
    }

    // ── 4c. File > Open loads an existing file into a notepad (E19-F3) ─────
    // CTextDocument::OnFileOpen's canonical command; the port had no menu
    // item for it at all, so it was permanently unreachable from the UI.
    // Drives g_wxhook_open_text_file directly -- the SAME non-modal call
    // CWxNotepadFrame::OnMenuOpen makes once its wxFileDialog returns OK --
    // so the file dialog itself needn't run under ctest.
    {
        const char* path = "/tmp/mushclient_notepad_selftest_open.txt";
        const char* contents = "opened from disk\n";
        FILE* fp = fopen(path, "w");
        npCheck(fp != nullptr, "the fixture file could be created");
        if (fp)
        {
            fputs(contents, fp);
            fclose(fp);
        }

        npCheck(g_wxhook_open_text_file != nullptr,
                "the file-open hook is installed");
        if (g_wxhook_open_text_file && fp)
        {
            void* pv = g_wxhook_open_text_file(path);
            npCheck(pv != nullptr, "opening the file returns a document");

            CWxNotepadFrame* openedFrame =
                CWxNotepadFrame::Find("mushclient_notepad_selftest_open.txt");
            npCheck(openedFrame != nullptr,
                    "File > Open created a notepad titled after the file "
                    "(the menu item did not exist before)");
            if (openedFrame)
            {
                openedFrame->Hide();
                npCheck(openedFrame->GetText() == contents,
                        "...with the file's own contents");
                // The menu item itself, not just the hook it drives -- the
                // hook (g_wxhook_open_text_file) already had other callers
                // (world_debug.cpp, the Plugins dialog's Edit button), so
                // exercising it alone would not fail if only the NEW menu
                // item / OnMenuOpen wiring were missing.
                npCheck(FindMenuItem(openedFrame, "File", "Open") != nullptr,
                        "...and the File menu has the Open item that drives "
                        "this (the notepad had no File > Open at all)");
            }
        }

        remove(path);
    }

    // ── 4d. Line endings survive a load/save round trip (D-10) ─────────────
    //
    // The corruption this section exists for: the wx control does not COLLAPSE
    // a CRLF to LF, it EXPANDS it to two line breaks, and SaveFile wrote that
    // expansion back.  Opening any CRLF file (every bundled plugin is one)
    // through Plugins ▸ Edit, Game ▸ Edit Script File or File ▸ Open therefore
    // double-spaced it on screen, and one Ctrl+S rewrote the user's file that
    // way.  Two clicks, no warning.
    //
    // The pure functions are pinned headlessly (tests/test_notepad_convert.cpp,
    // NotepadLineEndings.*).  What can only be checked HERE is that the real
    // wxTextCtrl still behaves the way the fix assumes, and that the real frame
    // routes its loads, its saves and its script writes through the
    // normalisation.
    {
        // (i) The wx fact the whole design rests on.  A raw control, so this
        //     fails the day wxOSX starts collapsing CRLF — at which point the
        //     normalisation stops being load-bearing and should be revisited
        //     rather than silently kept.
        wxTextCtrl* raw = new wxTextCtrl(wxTheApp->GetTopWindow(), wxID_ANY,
                                         wxEmptyString, wxDefaultPosition,
                                         wxDefaultSize, wxTE_MULTILINE);
        raw->SetValue(wxT("a\r\nb"));
#ifdef __WXOSX__
        npCheck(raw->GetValue() == wxT("a\n\nb"),
                "the wx control EXPANDS a CRLF into two line breaks "
                "(the whole reason this section exists)");
        npCheck(raw->GetNumberOfLines() == 3,
                "...three lines where the file had two");
#else
        npCheck(raw->GetValue() == wxT("a\r\nb"),
                "the raw wxGTK control retains CR in GetValue() without normalisation");
        npCheck(raw->GetNumberOfLines() == 2,
                "...across two lines");
#endif
        raw->Destroy();

        // (ii) The frame must not let that happen.  Before the fix this text
        //      arrived verbatim and GetText() came back "a\n\nb\n\nc\n".
        CTextDocument* eol = MakeNotepad("selftest_np_eol", "a\r\nb\r\nc\r\n");
        CWxNotepadFrame* eolFrame = CWxNotepadFrame::Find("selftest_np_eol");
        npCheck(eol != nullptr && eolFrame != nullptr, "eol notepad created");
        if (eolFrame)
            npCheck(eolFrame->GetText() == "a\nb\nc\n",
                    "a notepad created from CRLF text is NOT double-spaced");

        // (iii) world.AppendToNotepad with embedded CRLF — the script-facing
        //       path, which reaches the control through m_fn_replace_sel.
        if (eolFrame && eol)
        {
            CMUSHclientDoc* npWorld = new CMUSHclientDoc();
            npWorld->m_mush_name = "zz_selftest_notepad_eol";
            eol->m_pRelatedWorld = npWorld;
            eol->m_iUniqueDocumentNumber = npWorld->m_iUniqueDocumentNumber;

            npCheck(npWorld->ReplaceNotepad("selftest_np_eol",
                                            "x\r\ny\r\nz") != FALSE,
                    "world.ReplaceNotepad reaches the notepad");
            npCheck(eolFrame->GetText() == "x\ny\nz",
                    "...and CRLF in the replaced text does not double-space it");

            npCheck(npWorld->AppendToNotepad("selftest_np_eol",
                                             "\r\np\r\nq") != FALSE,
                    "world.AppendToNotepad reaches the notepad");
            npCheck(eolFrame->GetText() == "x\ny\nz\np\nq",
                    "...and CRLF in the appended text does not double-space it");

            eol->m_pRelatedWorld = nullptr;
            delete npWorld;
        }

        // (iv) The acceptance criterion, end to end through the real objects:
        //      open a CRLF file, save it untouched, and the bytes must be
        //      identical.  Then edit one line and save again — every OTHER
        //      byte must still be identical, and the file must still be CRLF.
        const char* path = "/tmp/mushclient_notepad_selftest_crlf.xml";
        const std::string original =
            "<?xml version=\"1.0\"?>\r\n<muclient>\r\n<plugin name=\"x\"/>\r\n</muclient>\r\n";
        {
            FILE* fp = fopen(path, "wb");
            npCheck(fp != nullptr, "the CRLF fixture file could be created");
            if (fp)
            {
                fwrite(original.data(), 1, original.size(), fp);
                fclose(fp);
            }
        }

        if (g_wxhook_open_text_file)
        {
            CTextDocument* td =
                static_cast<CTextDocument*>(g_wxhook_open_text_file(path));
            npCheck(td != nullptr, "the CRLF file opens into a notepad");
            CWxNotepadFrame* crlfFrame =
                CWxNotepadFrame::Find("mushclient_notepad_selftest_crlf.xml");
            if (crlfFrame) crlfFrame->Hide();

            if (td && crlfFrame)
            {
                npCheck(td->m_iLineEndings == eLineEndingsCRLF,
                        "...tagged with the CRLF style it arrived in");
                npCheck(crlfFrame->GetText().find('\r') == std::string::npos,
                        "...with LF-only text in the control");
                npCheck(crlfFrame->GetText() ==
                            "<?xml version=\"1.0\"?>\n<muclient>\n"
                            "<plugin name=\"x\"/>\n</muclient>\n",
                        "...and NOT double-spaced (4 lines, not 8)");

                // Save with no edit at all.
                npCheck(td->DoSave((LPCTSTR) td->GetPathName()) != FALSE,
                        "the unedited notepad saves");
                npCheck(SlurpFile(path) == original,
                        "an unedited CRLF file saves back BYTE-IDENTICAL "
                        "(it used to come back double-spaced)");

                // Now change exactly one line, the way the user would.
                const int n = crlfFrame->ReplaceAllInText(
                    wxT("<plugin name=\"x\"/>"), wxT("<plugin name=\"yy\"/>"),
                    true /*matchCase*/);
                npCheck(n == 1, "exactly one line was edited");
                npCheck(td->DoSave((LPCTSTR) td->GetPathName()) != FALSE,
                        "the edited notepad saves");

                std::string expected = original;
                const size_t at = expected.find("<plugin name=\"x\"/>");
                if (at != std::string::npos)
                    expected.replace(at, strlen("<plugin name=\"x\"/>"),
                                     "<plugin name=\"yy\"/>");
                npCheck(SlurpFile(path) == expected,
                        "...and the file comes back byte-identical APART FROM "
                        "the edited line, still CRLF");
            }
        }
        remove(path);

        // (v) The control cannot represent CR, but a CR-only file must still
        //     be written back as a CR-only file.
        const char* crPath = "/tmp/mushclient_notepad_selftest_cr.txt";
        const std::string crOriginal = "one\rtwo\rthree\r";
        {
            FILE* fp = fopen(crPath, "wb");
            if (fp) { fwrite(crOriginal.data(), 1, crOriginal.size(), fp); fclose(fp); }
        }
        if (g_wxhook_open_text_file)
        {
            CTextDocument* td =
                static_cast<CTextDocument*>(g_wxhook_open_text_file(crPath));
            CWxNotepadFrame* crFrame =
                CWxNotepadFrame::Find("mushclient_notepad_selftest_cr.txt");
            if (crFrame) crFrame->Hide();
            npCheck(td != nullptr && td->m_iLineEndings == eLineEndingsCR,
                    "a CR-only file is tagged as CR");
            if (td)
            {
                npCheck(td->DoSave((LPCTSTR) td->GetPathName()) != FALSE,
                        "the CR-only notepad saves");
                npCheck(SlurpFile(crPath) == crOriginal,
                        "...back as a CR-only file, byte-identical");
            }
        }
        remove(crPath);

        // (vi) DoSave's bReplace flag is "Save Copy As": MFC's
        //      CDocument::DoSave writes the file on both arms but only calls
        //      SetPathName when bReplace is TRUE.  world.SaveNotepad's
        //      ReplaceExisting -- whose Lua default is FALSE -- passes
        //      straight through, so the port's unconditional re-target made
        //      every two-argument SaveNotepad silently redirect the user's
        //      next Cmd-S (and the close-time save) at the copy.
        const char* srcPath  = "/tmp/mushclient_notepad_selftest_copyas_src.txt";
        const char* copyPath = "/tmp/mushclient_notepad_selftest_copyas_copy.txt";
        const char* newPath  = "/tmp/mushclient_notepad_selftest_copyas_new.txt";
        const std::string srcText = "alpha\nbeta\n";
        {
            FILE* fp = fopen(srcPath, "wb");
            if (fp) { fwrite(srcText.data(), 1, srcText.size(), fp); fclose(fp); }
        }
        remove(copyPath);
        remove(newPath);
        if (g_wxhook_open_text_file)
        {
            CTextDocument* td =
                static_cast<CTextDocument*>(g_wxhook_open_text_file(srcPath));
            CWxNotepadFrame* frame =
                CWxNotepadFrame::Find("mushclient_notepad_selftest_copyas_src.txt");
            if (frame) frame->Hide();
            npCheck(td != nullptr && frame != nullptr,
                    "the save-a-copy fixture opens into a notepad");
            if (td && frame)
            {
                const CString before = td->GetPathName();
                const wxString beforeTitle = frame->GetTitle();

                // Dirty the document first, so "the flag clears on both arms"
                // below discriminates rather than restating the initial state.
                td->SetModifiedFlag(TRUE);

                npCheck(td->DoSave(copyPath, FALSE) != FALSE,
                        "DoSave(path, FALSE) writes the copy");
                npCheck(SlurpFile(copyPath) == srcText,
                        "...with the notepad's bytes");
                npCheck(td->GetPathName() == before,
                        "...but the document still points at the ORIGINAL file "
                        "(was: silently re-targeted at the copy)");
                npCheck(frame->GetTitle() == beforeTitle,
                        "...and the frame keeps its caption");
                npCheck(!td->IsModified(),
                        "...and the modified flag clears on both arms");

                // The control: a TRUE save must still re-target and rename --
                // a lambda that stopped saving, or stopped re-pointing
                // entirely, would pass everything above.
                npCheck(td->DoSave(newPath, TRUE) != FALSE,
                        "DoSave(path, TRUE) still saves");
                npCheck(td->GetPathName() == CString(newPath),
                        "...and DOES re-point the document (Save As semantics)");
                npCheck(frame->GetTitle() ==
                            wxT("mushclient_notepad_selftest_copyas_new.txt"),
                        "...and renames the frame to the new basename");
            }
        }
        remove(srcPath);
        remove(copyPath);
        remove(newPath);
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
