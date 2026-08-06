// platform/extended_selftest.cpp — see the header.

#ifndef _WIN32

#include <wx/wx.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <wx/timer.h>
#include <wx/dcbuffer.h>   // wxAutoBufferedPaintDC -- the DC OnPaint actually uses
#include <wx/dcclient.h>

#include "platform/extended_selftest.h"
#include "platform/app_config.h"

#include <cstdio>
#include <cstdlib>

// Defined in platform/dialog_bridge_selftest.cpp: the MFC->wx dialog and
// property-sheet bridges (docs/UNREACHABLE-PATHS-2026-08.md findings 16, 17,
// 19).  It lives in its own TU because it pulls in the canonical property-page
// and entity-list headers, which nothing else here wants.
int RunDialogBridgeSelfTest();

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

struct Harness { const wxChar* name; int (*run)(); };
const Harness kHarnesses[] = {
    { wxT("paint"),         &RunPaintDeliverySelfTest },
    { wxT("dialog-bridge"), &RunDialogBridgeSelfTest },
    { wxT("clear-semantics"), &RunClearSemanticsSelfTest },
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
