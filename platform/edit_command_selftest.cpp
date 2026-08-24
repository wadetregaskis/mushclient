// platform/edit_command_selftest.cpp — Edit ▸ Cut / Paste / Undo must not
// recurse (--extended-selftest=edit-commands).
//
// WHAT BROKE
//
// These three menu items keep their STANDARD wx ids so that AppKit routes
// cut:/paste:/undo: down the responder chain to whatever text field is first
// responder — see the Edit menu in wx_mfc_impl.cpp, which explains why Copy and
// Select All deliberately do the opposite.  When one of them arrives as a wx
// command event instead, the MDI frame's own dispatcher acts on the focused
// field.
//
// It used to do that by re-dispatching a wxCommandEvent to that window.  A
// wxCommandEvent PROPAGATES: when nothing handles it, wxWindowBase::TryAfter
// walks it up the parent chain looking for a taker, which reaches the frame,
// whose dispatcher sent it straight back down to the same window.  For ever.
//
// "Nothing handles it" was not the unusual case, it was the ONLY case: no
// wxWidgets control binds wxID_CUT/wxID_PASTE/wxID_UNDO, precisely because on
// this platform they are AppKit's native actions.  So Edit ▸ Undo with no world
// open recursed until the stack died — SIGSEGV on the guard page at 511 frames.
// The fix acts on the focused wxTextCtrl directly, as the Copy and Select All
// handlers next to it already do.
//
// WHY THIS NEEDS THE LOOP-RUNNING MODE
//
// The arm branches on wxWindow::FindFocus(), and a loop-free harness cannot
// place focus at all: measured, FindFocus() stays null through the whole of
// --menubar-selftest even after Show() + SetFocus() on a real frame.  A
// loop-free version of this test therefore never reaches the branch, and every
// assertion in it passes on the broken code — which is exactly what the first
// draft did, until the assertion that focus had actually landed caught it.
// That assertion is kept below for the same reason.

#ifndef _WIN32

// ── wxWidgets (first — same include-order rule as wx_mfc_impl.cpp) ─────────
#include <wx/wx.h>
#include <wx/mdi.h>

// ── MFC-compat / canonical ────────────────────────────────────────────────
#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "childfrm.h"
#include "MUSHview.h"
#include "resource.h"
#include "platform/wx_mfc_hooks.h"
#include "platform/extended_selftest.h"
#ifdef __APPLE__
#include "platform/wx_osx_statusitem.h"   // wxOSXActivateSelf
#endif

#include <cstdio>
#include <string>

namespace {

int g_fail = 0;

bool chk(bool ok, const std::string& what)
{
    if (!ExtendedSelfTestCheck(ok, what.c_str())) { ++g_fail; return false; }
    return true;
}

// The focused field the arm is supposed to act on.  It overrides the three
// operations and deliberately does NOT chain to the base: the real Cut and
// Paste move the SYSTEM CLIPBOARD, which belongs to whoever is running the
// tests and is not this harness's to spend.  What is under test is that the
// frame's dispatcher calls the right method on the focused field — the port's
// half — not that wxTextCtrl can cut text, which is wxWidgets' half.
class ProbeText : public wxTextCtrl
{
public:
    ProbeText(wxWindow* parent)
        : wxTextCtrl(parent, wxID_ANY, wxT("abc")) {}

    int cuts = 0, pastes = 0, undos = 0;

    void Cut()   override { ++cuts;   }
    void Paste() override { ++pastes; }
    void Undo()  override { ++undos;  }
};

// How many times the frame's dispatcher has been offered the command under
// test.  A free function rather than a lambda so Unbind can match it again.
int g_seen = 0;
void CountEditCmd(wxCommandEvent& evt)
{
    ++g_seen;
    // Pass it on, so the real dispatcher downstream of us still runs — but only
    // for the first few passes.  If the recursion ever comes back, consuming
    // the event here turns it into a [FAIL] line instead of a stack overflow
    // that takes the harness down with it and reports nothing at all.
    if (g_seen < 4)
        evt.Skip();
}

// Focus a window and wait for it: focus arrives through the window server, so
// it is not there the instant SetFocus returns.  Bounded — a wait that never
// ends is the watchdog's problem, not a test result.
bool FocusOn(wxWindow* w)
{
    w->SetFocus();
    for (int i = 0; i < 50 && wxWindow::FindFocus() != w; ++i) {
        wxYield();                 // legal here: the extended mode HAS a loop
        wxMilliSleep(20);
    }
    return wxWindow::FindFocus() == w;
}

// What actually has focus, for the message when it is not what was asked for.
std::string FocusName()
{
    wxWindow* const f = wxWindow::FindFocus();
    return f ? std::string(wxString(f->GetClassInfo()->GetClassName()).utf8_str())
             : std::string("nothing");
}

const struct { int id; const char* name; int ProbeText::* counter; } kCmds[] = {
    { wxID_UNDO,  "Undo",  &ProbeText::undos  },
    { wxID_CUT,   "Cut",   &ProbeText::cuts   },
    { wxID_PASTE, "Paste", &ProbeText::pastes },
};

} // namespace

// ══════════════════════════════════════════════════════════════════════════

int RunEditCommandSelfTest()
{
    printf("EXTENDED-SELFTEST(edit-commands): does the Cut/Paste/Undo arm "
           "recurse?\n");

    // The dispatcher under test is installed by hook_load_mdi_frame, which
    // InitInstance would normally call — and InitInstance deliberately does not
    // run under an extended selftest (it would open the user's real preferences
    // database), so call the hook directly, exactly as --menubar-selftest does.
    if (App.m_pWorldDocTemplate == NULL)
        App.m_pWorldDocTemplate = new CMultiDocTemplate(
            IDR_MUSHCLTYPE,
            RUNTIME_CLASS(CMUSHclientDoc),
            RUNTIME_CLASS(CChildFrame),
            RUNTIME_CLASS(CMUSHView));

    if (!chk(g_wxhook_load_mdi_frame != NULL, "the MDI-frame bridge hook is installed"))
        return g_fail;

    CWnd host;
    const int built = g_wxhook_load_mdi_frame(&host, IDR_MAINFRAME, 0, NULL);
    wxMDIParentFrame* frame = static_cast<wxMDIParentFrame*>(host.m_hWnd);
    if (!chk(built != 0 && frame != NULL, "the main frame was created"))
        return g_fail;

    wxFrame* editHost = new wxFrame(frame, wxID_ANY, wxT("edit-commands"),
                                    wxPoint(60, 60), wxSize(260, 110));

    // Two victims, because the two halves of this need opposite things.
    //   `inert`  — a plain window, which does NOT handle these commands.  That
    //              is the crash condition, and the only one: the old code's
    //              forwarded event found no taker and propagated back up.
    //   `victim` — a real text field, which does.  Needed for the other half,
    //              since the arm has to keep DOING something.
    wxWindow*  inert  = new wxWindow(editHost, wxID_ANY,
                                     wxPoint(4, 40), wxSize(80, 20));
    ProbeText* victim = new ProbeText(editHost);

    editHost->Show();
    editHost->Raise();
#ifdef __APPLE__
    // Bring the app to the FRONT, and this is load-bearing rather than
    // cosmetic: wxWindow::FindFocus() on wxOSX reports the first responder of
    // the KEY window, and a process launched from a shell has no key window at
    // all until it is activated.  Without this the harness reported
    // "FindFocus() = nothing" and had nothing to test — in the loop-free mode
    // AND this one, which is what ruled the event loop out as the cause.
    wxOSXActivateSelf();
#endif

    // ── half one: the recursion ─────────────────────────────────────────────
    if (!chk(FocusOn(inert),
             std::string("focus lands on a window that does not handle these "
                         "commands (FindFocus() = ") + FocusName() + ")")) {
        // Without focus the arm is unreachable and everything below it would
        // pass on the broken code too.  Stop rather than report a green vacuum.
        editHost->Destroy();
        frame->Destroy();
        wxYield();
        printf("EXTENDED-SELFTEST(edit-commands): %d failed\n", g_fail);
        return g_fail;
    }

    for (const auto& c : kCmds) {
        g_seen = 0;
        // Bound on the FRAME, so it counts both the command sent below and any
        // that arrives back up the parent chain — which is what the loop was
        // made of.  CountEditCmd stops passing it on after a few rounds, so a
        // regression is a [FAIL] line rather than a stack overflow that takes
        // the harness down and reports nothing.
        frame->Bind(wxEVT_MENU, &CountEditCmd, c.id);
        wxCommandEvent cmd(wxEVT_MENU, c.id);
        cmd.SetEventObject(frame);
        frame->GetEventHandler()->ProcessEvent(cmd);
        frame->Unbind(wxEVT_MENU, &CountEditCmd, c.id);

        chk(g_seen == 1,
            std::string("Edit > ") + c.name +
            " reaches the frame's dispatcher exactly once (was " +
            std::to_string(g_seen) + ")");
    }

    // ── half two: it must still DO something ────────────────────────────────
    // Not optional: "exactly once" is equally true of an arm deleted outright,
    // so without this the test could not tell a fix from a gutting.  This half
    // does NOT discriminate the original bug — with a real text field focused
    // the old code's forwarded event was consumed by wxTextCtrl's own handler
    // and never came back — and it is not meant to.  Half one is that test.
    if (chk(FocusOn(victim),
            std::string("focus lands on a text field (FindFocus() = ")
            + FocusName() + ")")) {
        for (const auto& c : kCmds) {
            victim->cuts = victim->pastes = victim->undos = 0;
            wxCommandEvent cmd(wxEVT_MENU, c.id);
            cmd.SetEventObject(frame);
            frame->GetEventHandler()->ProcessEvent(cmd);

            chk(victim->*(c.counter) == 1 &&
                    victim->cuts + victim->pastes + victim->undos == 1,
                std::string("...and Edit > ") + c.name + " calls " + c.name +
                "() on it, once, and nothing else (cut/paste/undo = " +
                std::to_string(victim->cuts) + "/" +
                std::to_string(victim->pastes) + "/" +
                std::to_string(victim->undos) + ")");
        }
    }

    editHost->Destroy();
    frame->Destroy();
    wxYield();

    printf("EXTENDED-SELFTEST(edit-commands): %d failed\n", g_fail);
    return g_fail;
}

#endif // !_WIN32
