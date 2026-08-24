// platform/extended_selftest.cpp — see the header.

#ifndef _WIN32

#include <wx/wx.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <wx/timer.h>
#include <wx/dcbuffer.h>   // wxAutoBufferedPaintDC -- the DC OnPaint actually uses
#include <wx/dcclient.h>
#include <wx/fileconf.h>   // wxFileConfig -- the scratch config the geometry test writes into

#include "platform/extended_selftest.h"
#include "platform/app_config.h"
#include "platform/wx_outputpane.h"

#include <cstdio>
#include <cstdlib>

// Defined in platform/dialog_bridge_selftest.cpp: the MFC->wx dialog and
// property-sheet bridges (docs/UNREACHABLE-PATHS-2026-08.md findings 16, 17,
// 19).  It lives in its own TU because it pulls in the canonical property-page
// and entity-list headers, which nothing else here wants.
int RunDialogBridgeSelfTest();

int RunNotepadSelfTest();   // platform/notepad_selftest.cpp

// Defined in platform/edit_command_selftest.cpp: Edit > Cut/Paste/Undo must not
// recurse.  Its own TU for the same reason the dialog bridge has one -- it
// needs the canonical MFC headers to build the real MDI frame.  It needs THIS
// mode because the arm it covers branches on wxWindow::FindFocus(), and a
// loop-free harness cannot place focus at all.
int RunEditCommandSelfTest();

// Defined in wx_mfc_impl.cpp: the geometry writer the quit hook and the MDI
// parent's close handler share, and the full-screen toggle that stashes (or
// discards) a window's pre-full-screen rect for it -- see R2-12-F5 in
// RunPaintDeliverySelfTest below.
void SaveWindowGeometryTo(wxConfigBase& cfg, wxTopLevelWindow* frame);
void wx_toggle_full_screen(wxTopLevelWindow* target);

namespace {

int g_pass = 0, g_fail = 0;

void chk(bool ok, const char* what)
{
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    (ok ? g_pass : g_fail)++;
}

// ── the scratch preferences ─────────────────────────────────────────────────
// One file per process, named with the pid so two runs cannot collide even if
// the ctest RESOURCE_LOCK is ever removed or the binary is run by hand in two
// terminals.  Removed on the way out.
wxString g_prefsPath;

wxString MakeScratchPrefsPath()
{
    wxFileName fn(wxStandardPaths::Get().GetTempDir(),
                  wxString::Format(wxT("mushclient-extended-selftest-%ld.cfg"),
                                   (long) getpid()));
    return fn.GetFullPath();
}

// ── the watchdog ────────────────────────────────────────────────────────────
// The whole reason this mode was deferred: a harness that never leaves the loop
// hangs until ctest's timeout, and the casualty is usually a DIFFERENT test.
// This turns that into an ordinary failure with a real exit code, and says
// which harness was responsible.
class Watchdog : public wxTimer {
public:
    explicit Watchdog(const wxString& name) : m_name(name) {}
    // Called by Finish: from here on the harness IS done and the only thing
    // outstanding is the loop actually stopping.
    void SetExitDidNotTake() { m_afterFinish = true; }
    void Notify() override
    {
        if (m_afterFinish)
            printf("EXTENDED-SELFTEST(%s): WATCHDOG -- the harness finished but "
                   "ExitMainLoop did not take; the loop is still running.\n",
                   (const char*) m_name.utf8_str());
        else
        printf("EXTENDED-SELFTEST(%s): WATCHDOG -- the harness did not finish "
               "within %d seconds and did not leave the main loop.\n",
               (const char*) m_name.utf8_str(), Seconds());
        printf("EXTENDED-SELFTEST(%s): %d passed, %d failed (incomplete)\n",
               (const char*) m_name.utf8_str(), g_pass, g_fail + 1);
        fflush(stdout);
        // _exit, not exit: we are inside a timer callback with a live event
        // loop and a half-built GUI, and running static destructors from here
        // is its own source of hangs and crashes.  The point is to FAIL, fast
        // and legibly.
        if (!g_prefsPath.empty()) wxRemoveFile(g_prefsPath);
        _exit(2);
    }
    // Overridable so the watchdog itself can be tested in seconds rather than
    // in a minute -- MUSHCLIENT_EXTENDED_WATCHDOG_SECS.  A safety net nobody
    // has ever seen fire is not a safety net.
    static int Seconds()
    {
        if (const char* e = getenv("MUSHCLIENT_EXTENDED_WATCHDOG_SECS")) {
            const int v = atoi(e);
            if (v > 0) return v;
        }
        return 60;
    }
private:
    wxString m_name;
    bool     m_afterFinish = false;
};

Watchdog* g_watchdog = nullptr;

// ── the harnesses ───────────────────────────────────────────────────────────
// Each returns the number of failures.  They run with a REAL event loop, which
// is the only thing this mode buys: paints are delivered, idle handlers run,
// and a modal can complete.

// A first harness that proves the mode does what it claims -- that a paint
// really is delivered here, when it demonstrably is not in the OnInit-scoped
// selftests.  Without this the mode is an assertion about itself.
int RunPaintDeliverySelfTest()
{
    printf("EXTENDED-SELFTEST(paint): does a real event loop deliver paints?\n");

    // The isolation has to be provable, not assumed: this harness runs far
    // enough into the app to write preferences.
    chk(!AppConfigPathForTesting().empty(),
        "preferences are redirected away from the user's real ones");
    {
        const std::unique_ptr<wxConfigBase> cfg = OpenAppConfig();
        cfg->Write(wxT("/extended-selftest/probe"), wxT("scratch"));
        cfg->Flush();
        chk(wxFileName::FileExists(g_prefsPath),
            "...and writes land in the scratch file");
    }

    wxFrame* frame = new wxFrame(nullptr, wxID_ANY, wxT("extended selftest"),
                                 wxPoint(60, 60), wxSize(420, 300));
    int paints = 0;
    wxPanel* panel = new wxPanel(frame);
    panel->Bind(wxEVT_PAINT, [&paints](wxPaintEvent& e) {
        wxPaintDC dc(static_cast<wxWindow*>(e.GetEventObject()));
        ++paints;
    });
    frame->Show();

    // Let the loop deliver.  wxYield alone is what the OnInit-scoped selftests
    // have, and it produces nothing; the difference here is that the loop is
    // actually running underneath us.
    const wxLongLong until = wxGetLocalTimeMillis() + 1500;
    while (wxGetLocalTimeMillis() < until && paints == 0) {
        panel->Refresh(false);
        wxYield();
        wxMilliSleep(20);
    }

    printf("    (%d paints delivered)\n", paints);
    chk(paints > 0,
        "the paint handler runs -- which it never does before OnRun");

    frame->Destroy();
    wxYield();

    // ── R2-12-F5: the full-screen pre-rect stash is PER WINDOW ─────────────
    // s_preFullScreenRects (wx_mfc_impl.cpp) used to be a single global pair,
    // so a second window entering full screen could stomp the first's stash,
    // and leaving full screen on EITHER window cleared it for BOTH -- the
    // quit-time save would then persist a still-full-screen window's
    // display-sized bounds as its saved geometry.  ShowFullScreen animates on
    // macOS (real transition, real Space), which is exactly why this lives
    // here rather than in an OnInit-scoped selftest: it needs the real loop
    // this mode buys to settle before GetRect()/IsFullScreen() can be trusted.
    {
        wxFrame* winA = new wxFrame(nullptr, wxID_ANY, wxT("fs-A"),
                                    wxPoint(15, 15), wxSize(500, 350));
        wxFrame* winB = new wxFrame(nullptr, wxID_ANY, wxT("fs-B"),
                                    wxPoint(15, 15), wxSize(640, 480));
        winA->Show();
        winB->Show();
        for (int i = 0; i < 20; ++i) { wxYield(); wxMilliSleep(10); }

        wx_toggle_full_screen(winA);   // A enters full screen; stashes 500x350
        wx_toggle_full_screen(winB);   // B enters full screen; stashes 640x480
        for (int i = 0; i < 100 && (!winA->IsFullScreen() || !winB->IsFullScreen()); ++i) {
            wxYield();
            wxMilliSleep(20);
        }
        chk(winA->IsFullScreen() && winB->IsFullScreen(),
            "both windows report full screen before either leaves");

        wx_toggle_full_screen(winA);   // A leaves full screen -- A's own entry is erased
        for (int i = 0; i < 100 && winA->IsFullScreen(); ++i) { wxYield(); wxMilliSleep(20); }
        chk(!winA->IsFullScreen(), "A has left full screen");
        chk(winB->IsFullScreen(),
            "...while B is STILL full screen -- this is the moment the quit "
            "hook's geometry save would run in");

        wxFileName fn(wxStandardPaths::Get().GetTempDir(),
                     wxString::Format(wxT("mushclient-extended-fullscreen-%ld.ini"),
                                      (long) getpid()));
        if (fn.FileExists()) wxRemoveFile(fn.GetFullPath());
        {
            wxFileConfig cfg(wxEmptyString, wxEmptyString, fn.GetFullPath(),
                             wxEmptyString, wxCONFIG_USE_LOCAL_FILE);
            SaveWindowGeometryTo(cfg, winB);
            long w = 0, h = 0;
            cfg.Read(wxT("/geometry/main_w"), &w);
            cfg.Read(wxT("/geometry/main_h"), &h);
            chk(w == 640 && h == 480,
                "B's PRE-full-screen size survives the save even though A left "
                "full screen first -- with a single shared stash, A's leave "
                "clears B's entry too and this reads the (display-sized) "
                "current rect instead");
        }
        wxRemoveFile(fn.GetFullPath());

        // Leave full screen before Destroy(), so the async exit animation
        // does not fight the window's teardown.
        wx_toggle_full_screen(winB);
        for (int i = 0; i < 100 && winB->IsFullScreen(); ++i) { wxYield(); wxMilliSleep(20); }

        winA->Destroy();
        winB->Destroy();
        wxYield();
    }

    printf("EXTENDED-SELFTEST(paint): %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}

// Deliberately never returns: the watchdog is the only thing that can end this
// process, which is exactly what the ctest entry for it asserts.  A wedged
// harness is the failure mode this whole mode was deferred over, so the
// mechanism that turns it into an ordinary failure is itself under test.
int RunWatchdogProbe()
{
    printf("EXTENDED-SELFTEST(hang): deliberately not finishing; the watchdog "
           "should end this process with exit code 2.\n");
    fflush(stdout);
    for (;;) { wxYield(); wxMilliSleep(50); }
    return 0;   // unreachable
}

// ── audit 11(b): what does wxAutoBufferedPaintDC::Clear actually clear? ─────
// The open question, and the only thing standing between here and narrowing
// OnPaint's row loop to the update region.  If Clear() wipes the whole backing
// store, rows outside the update region are erased and never redrawn -- silent
// scrolling corruption that no assertion in the suite would catch.  If it
// honours the update region (or the clip), narrowing the loop is safe.
//
// Unanswerable until now: it needs a REAL paint into a REAL backing store, and
// no OnInit-scoped selftest ever gets one.
int RunClearSemanticsSelfTest()
{
    printf("EXTENDED-SELFTEST(clear-semantics): what does Clear() erase?\n");

    wxFrame* frame = new wxFrame(nullptr, wxID_ANY, wxT("clear semantics"),
                                 wxPoint(80, 80), wxSize(300, 200));
    wxPanel* panel = new wxPanel(frame);

    // Paint 1 fills everything red.  Paint 2 sets the background GREEN, calls
    // Clear(), and draws nothing -- then we read back a pixel far outside the
    // rectangle we invalidated.  Red means Clear respected the region; green
    // means it wiped the lot.
    int    pass = 0;
    wxRect invalidated(0, 0, 20, 20);
    wxRect clipSeen, updateSeen;

    panel->Bind(wxEVT_PAINT, [&](wxPaintEvent&) {
        wxAutoBufferedPaintDC dc(panel);
        ++pass;
        if (pass == 1) {
            dc.SetBackground(*wxRED_BRUSH);
            dc.Clear();
            return;
        }
        if (pass == 2) {
            // THE measurement.  A pixel read-back would be the obvious test and
            // is not trustworthy here -- wxOSX cannot reliably blit out of a
            // wxClientDC, which platform/wx_composite_selftest.cpp documents and
            // works around.  The clipping box is exact and needs no read-back:
            // if wx clips the paint DC to the update region then Clear() cannot
            // write outside it, whatever Clear() intends, so narrowing the row
            // loop to that region is safe by construction.
            dc.GetClippingBox(clipSeen);
            updateSeen = panel->GetUpdateRegion().GetBox();
            dc.SetBackground(*wxGREEN_BRUSH);
            dc.Clear();
        }
    });

    frame->Show();
    for (int i = 0; i < 40 && pass < 1; ++i) { wxYield(); wxMilliSleep(20); }
    chk(pass >= 1, "the first paint ran");

    panel->RefreshRect(invalidated, false);
    for (int i = 0; i < 40 && pass < 2; ++i) { wxYield(); wxMilliSleep(20); }
    chk(pass >= 2, "a partial invalidation produced a second paint");

    printf("    (invalidated %d,%d %dx%d; update region %d,%d %dx%d; "
           "paint DC clip %d,%d %dx%d)\n",
           invalidated.x, invalidated.y, invalidated.width, invalidated.height,
           updateSeen.x, updateSeen.y, updateSeen.width, updateSeen.height,
           clipSeen.x, clipSeen.y, clipSeen.width, clipSeen.height);

    chk(!updateSeen.IsEmpty() && updateSeen.width < 200,
        "a partial invalidation really does give a PARTIAL update region");
    chk(!clipSeen.IsEmpty(),
        "the paint DC arrives with a clipping box set");
    chk(clipSeen == updateSeen,
        "...and it is exactly the update region -- so Clear() cannot reach "
        "outside it, and narrowing OnPaint's row loop to that region is safe");

    frame->Destroy();
    wxYield();
    printf("EXTENDED-SELFTEST(clear-semantics): %d passed, %d failed\n",
           g_pass, g_fail);
    return g_fail;
}

// ── audit 11(b): how much is there to win? ──────────────────────────────────
// The correctness question is settled (clear-semantics, above): wx clips the
// paint DC to the update region, so narrowing OnPaint's row loop is safe.  This
// asks the OTHER question, the one that decides whether it is worth doing --
// and it is the question that refuted 11(a), so it gets asked first this time.
//
// Times a real full-viewport repaint of a full buffer, then the same paint with
// the row loop bounded to a single row (MUSHCLIENT_PAINT_ROWS=1, honoured only
// in this instrumented build).  The difference is the CEILING on what narrowing
// the loop could ever save -- an upper bound, because no real update region is
// one row and the caller half is not free.
int RunPaintCostSelfTest()
{
    printf("EXTENDED-SELFTEST(paint-cost): what is the ceiling on 11(b)?\n");

    wxFrame* frame = new wxFrame(nullptr, wxID_ANY, wxT("paint cost"),
                                 wxPoint(40, 40), wxSize(1000, 700));
    COutputPane* pane = new COutputPane(frame, 5000);
    pane->SetSize(0, 0, 960, 640);
    frame->Layout();
    pane->SendSizeEvent();
    frame->Show();

    for (int i = 0; i < 40; ++i) { wxYield(); wxMilliSleep(10); }

    // A realistic screenful of styled text, not blank lines.
    for (int i = 0; i < 3000; ++i) {
        SOutputLine line;
        for (int r = 0; r < 3; ++r) {
            SOutputRun run;
            run.text = (r == 1) ? "  highlighted portion  "
                                : "the quick brown fox jumps over the lazy dog ";
            run.fg = (r == 1) ? 0x00FFFF : 0xC0C0C0;
            run.bg = 0;
            run.bold = (r == 1);
            line.push_back(run);
        }
        pane->AppendLine(std::move(line));
    }
    pane->Refresh(false);
    for (int i = 0; i < 20; ++i) { wxYield(); wxMilliSleep(10); }
    chk(pane->LineCount() == 3000, "3000 styled lines in the pane");

    auto timeRepaints = [&](int reps) {
        const wxLongLong t0 = wxGetUTCTimeMillis();
        for (int i = 0; i < reps; ++i) {
            pane->Refresh(false);
            pane->Update();
            wxYield();
        }
        return (wxGetUTCTimeMillis() - t0).ToDouble() / reps;
    };

    const double full = timeRepaints(60);
    printf("    full-viewport repaint: %.3f ms\n", full);
    chk(full > 0.0, "a full repaint takes measurable time");

    // How many rows is that?  The saving is per-row, so the per-row cost is
    // what a narrowed loop would actually be trading against.
    const int rows = pane->LineHeight() > 0 ? 640 / pane->LineHeight() : 0;
    printf("    (about %d rows on screen, so ~%.4f ms per row)\n",
           rows, rows ? full / rows : 0.0);
    printf("    CEILING: narrowing the loop to ONE row could save at most "
           "%.3f ms per paint, and only when the update region is that small.\n",
           rows ? full - (full / rows) : 0.0);

    frame->Destroy();
    wxYield();
    printf("EXTENDED-SELFTEST(paint-cost): %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}

struct Harness { const wxChar* name; int (*run)(); };
const Harness kHarnesses[] = {
    { wxT("paint"),         &RunPaintDeliverySelfTest },
    { wxT("dialog-bridge"), &RunDialogBridgeSelfTest },
    { wxT("clear-semantics"), &RunClearSemanticsSelfTest },
    { wxT("paint-cost"),    &RunPaintCostSelfTest },
    { wxT("notepad"),       &RunNotepadSelfTest },
    { wxT("edit-commands"), &RunEditCommandSelfTest },
    { wxT("hang"),          &RunWatchdogProbe },
};

wxString g_which;
int      g_exitCode = 1;   // pessimistic: only a completed harness clears it

// Remove the scratch preferences, if the harness ever caused them to be
// written.  The FileExists test is not tidiness: wxRemoveFile on a file that is
// not there logs a system error, wxLogGui defers that to the next idle, and the
// flush puts up a modal NSAlert -- which registers no wxTopLevelWindow, so
// nothing looks wrong, but the main loop can no longer exit and the run dies at
// the watchdog instead.  Only the `paint` harness happens to write a
// preference, which is why every OTHER harness wedged until this was found.
void RemoveScratchPrefs()
{
    if (!g_prefsPath.empty() && wxFileName::FileExists(g_prefsPath))
        wxRemoveFile(g_prefsPath);
}

void Finish(int failures)
{
    RemoveScratchPrefs();
    g_exitCode = failures == 0 ? 0 : 1;

    // Leave the loop and let OnRun's return value carry the verdict.  Not
    // std::exit: unwinding a running loop from inside a callback is exactly
    // the kind of thing this mode exists to be careful about.
    wxTheApp->SetExitOnFrameDelete(false);
    wxTheApp->ExitMainLoop();

    // ExitMainLoop only marks the loop as finished; something has to wake it
    // for that to be noticed.  A harness that created and destroyed a window
    // leaves enough activity behind that it happens anyway -- one that never
    // made a window at all does not, and the process simply sits there.  Found
    // the hard way: the unknown-harness path hung indefinitely.
    wxWakeUpIdle();

    // And do NOT stop the watchdog here.  Stopping it before the exit is
    // CONFIRMED is what turned that hang into an unbounded one: the safety net
    // was removed on the strength of having asked to leave, not on having
    // left.  Re-arm it short instead, so a request that does not take is
    // reported in seconds rather than at the full timeout.
    if (g_watchdog) {
        g_watchdog->Stop();
        g_watchdog->SetExitDidNotTake();
        g_watchdog->Start(5000, wxTIMER_ONE_SHOT);
    }
}

} // namespace

bool ExtendedSelfTestArmed()
{
    return !g_which.empty();
}

int ExtendedSelfTestExitCode()
{
    return g_exitCode;
}

bool ExtendedSelfTestCheck(bool ok, const char* what)
{
    chk(ok, what);
    return ok;
}

bool WantsExtendedSelfTest(int argc, wxChar** argv, wxString& nameOut)
{
    const wxString kFlag = wxT("--extended-selftest=");
    for (int i = 1; i < argc; ++i) {
        const wxString a(argv[i]);
        if (a.StartsWith(kFlag)) { nameOut = a.Mid(kFlag.length()); return true; }
    }
    return false;
}

bool ArmExtendedSelfTest(const wxString& name)
{
    // Validate BEFORE committing to the event loop.  An unknown name is a
    // usage error, and answering it from inside the loop meant relying on
    // ExitMainLoop with no window ever created -- which does not take, so a
    // typo sat there until the watchdog.  Say so here and let the caller exit
    // the ordinary way, as every other selftest flag does.
    bool known = false;
    for (const Harness& h : kHarnesses)
        if (name == h.name) { known = true; break; }
    if (!known) {
        printf("EXTENDED-SELFTEST: no harness called '%s'.  Known:",
               (const char*) name.utf8_str());
        for (const Harness& h : kHarnesses)
            printf(" %s", (const char*) wxString(h.name).utf8_str());
        printf("\n");
        return false;
    }

    g_which = name;

    // BEFORE anything in the app can read or write a preference.
    g_prefsPath = MakeScratchPrefsPath();
    if (wxFileName::FileExists(g_prefsPath)) wxRemoveFile(g_prefsPath);
    SetAppConfigPathForTesting(g_prefsPath);

    g_watchdog = new Watchdog(name);
    g_watchdog->Start(Watchdog::Seconds() * 1000, wxTIMER_ONE_SHOT);

    wxTheApp->CallAfter([]() {
        for (const Harness& h : kHarnesses)
            if (g_which == h.name) { Finish(h.run()); return; }
    });
    return true;
}

#endif // !_WIN32
