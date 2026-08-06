// platform/wx_app.cpp
// wxWidgets application entry point for the mushgui target.
//
// This file ONLY includes wxWidgets headers and wx_mfc_hooks.h.
// All interaction with the MUSHclient source tree goes through the
// function pointer hooks set by wx_mfc_install_hooks() (implemented in
// wx_mfc_impl.cpp, which is the only file that includes both wx and
// MUSHclient headers).

#ifndef _WIN32

#include <wx/wx.h>
#include <wx/image.h>          // wxInitAllImageHandlers
#include <csignal>             // signal, SIGPIPE
#include <cstdlib>             // std::exit (selftest flag)
#include "platform/extended_selftest.h"   // the loop-running test mode
#include "platform/wx_mfc_hooks.h"

// Defined in wx_dialog_bridge.cpp: parse the bundled dialog resources once.
void LoadMfcDialogResources();

// Defined in wx_surface.cpp: the pal::ISurface conformance selftest
// (docs/PAL.md S2), run via the hidden --pal-surface-selftest flag.
int RunPalSurfaceSelfTest();

// Defined in wx_surface.cpp: install the wx ISurface adapter as the S3
// miniwindow-migration bridge (pal::g_surface + adopt/release).
void InstallWxSurfaceBridge();

// Defined in posix_transport.cpp (mushapp): the ITransport conformance
// harness, run via the hidden --pal-transport-selftest flag.
int RunPalTransportSelfTest();

// In-app conformance harness for the REAL world.Window* engine over the
// installed surface bridge (platform/miniwindow_selftest.cpp), run via the
// hidden --pal-miniwindow-selftest flag.
int RunPalMiniWindowSelfTest();

// In-app conformance harness for the wx bridge layers mushcore cannot link:
// window-geometry persistence, the dialog-template → wx control mapping and the
// world-creation defaults (platform/gui_bridge_selftest.cpp), run via the hidden
// --gui-bridge-selftest flag.
int RunGuiBridgeSelfTest();
// Defined in platform/wx_composite_selftest.cpp: the wx miniwindow compositor
// (blit placement + the post-paint m_rect) and its tooltip gate, run via the
// hidden --pal-composite-selftest flag.  Both bodies live in the executable
// (wx_world_child.cpp), so mushcore's headless tests cannot reach them.
int RunWxCompositeSelfTest();
// In-app conformance harness for the world window adapter — the wx
// pal::IWorldView (platform/world_frame_selftest.cpp), run via the hidden
// --world-frame-selftest flag.  It needs the real CWorldChildFrame, which the
// headless mushcore test executables cannot link.
int RunWorldFrameSelfTest();

// In-app conformance harness for the REAL menu bar (platform/menubar_selftest.cpp),
// run via the hidden --menubar-selftest flag.  The menu bar is built inside the
// MFC->wx bridge hook, so no headless executable can see it -- and its two
// failure modes (an accelerator wx cannot parse, two commands claiming the same
// key) are both completely silent.
int RunMenuBarSelfTest();

// In-app conformance harness for the five embedded scripting engines
// (platform/engines_selftest.cpp), run via the hidden --engines-selftest flag.
// They cannot be tested headlessly: each pulls in a whole interpreter, which the
// 31 mushcore test executables must not link.
int RunEmbeddedEnginesSelfTest(const char* only);

// In-app conformance harness for the notepad windows
// (platform/notepad_selftest.cpp), run via the hidden --notepad-selftest flag.
// CTextDocument and its wxFrame are GUI-only, so none of it links into the
// headless test executables.
int RunNotepadSelfTest();
// In-app conformance harness for the command-input bar
// (platform/input_selftest.cpp), run via the hidden --input-selftest flag.
// Unlike the harnesses above it needs the MFC→wx hooks installed (it builds a
// real CWorldChildFrame), so it is dispatched further down OnInit rather than
// in the early argv loop.
int RunInputSelfTest();
// In-app conformance harness for the real COutputPane — selection freeze,
// keep_pause_at_bottom, Clear Output, auto-copy, world.TextRectangle
// (platform/outputpane_selftest.cpp), run via --outputpane-selftest.  The pane
// is a wxScrolledWindow in mushgui_adapters, so the headless tests cannot see it.
int RunOutputPaneSelfTest();

#include "pal/surface_bridge.h"   // pal::g_surface — checked before installing
                                  // the bridge for the miniwindow selftest

class CMUSHWxApp : public wxApp {
    bool m_extendedSelfTest = false;
public:
    bool OnInit() override
    {
        // Hidden conformance harness: exercise the wx ISurface adapter against
        // the contract goldens inside a real (headless-enough) wx app, print
        // the results, and exit without booting the GUI.
        for (int i = 1; i < argc; ++i) {
            if (wxString(argv[i]) == wxT("--pal-surface-selftest")) {
                const int failures = RunPalSurfaceSelfTest();
                std::exit(failures == 0 ? 0 : 1);
            }
            if (wxString(argv[i]) == wxT("--pal-transport-selftest")) {
                const int failures = RunPalTransportSelfTest();
                std::exit(failures == 0 ? 0 : 1);
            }
            // --engines-selftest runs every language; --engines-selftest=tcl
            // runs one, so ctest can register a separate entry per language and
            // report SKIPPED (exit 77) for interpreters this build lacks.
            {
                const wxString a(argv[i]);
                if (a == wxT("--engines-selftest") || a.StartsWith(wxT("--engines-selftest="))) {
                    const wxString which = a.AfterFirst(wxT('='));
                    const wxScopedCharBuffer utf8 = which.utf8_str();
                    std::exit(RunEmbeddedEnginesSelfTest(
                        which.IsEmpty() ? nullptr : utf8.data()));
                }
            }
            if (wxString(argv[i]) == wxT("--notepad-selftest")) {
                // Needs the MFC->wx hooks: the harness drives the real notepad
                // creation hook, the real message-box seam and the real quit
                // hook.  Installing them is pointer assignment only, and the
                // normal install point below is never reached here.
                wx_mfc_install_hooks();
                const int failures = RunNotepadSelfTest();
                std::exit(failures == 0 ? 0 : 1);
            }
            if (wxString(argv[i]) == wxT("--gui-bridge-selftest")) {
                // Needs the MFC hooks installed (the dialog bridge builds
                // controls through them) but NOT the app booted: no world is
                // opened, no window is shown.
                wx_mfc_install_hooks();
                LoadMfcDialogResources();
                std::exit(RunGuiBridgeSelfTest());
            }
            if (wxString(argv[i]) == wxT("--pal-composite-selftest")) {
                // Same bridge requirement as the miniwindow selftest below:
                // the fixtures it composites are real CMiniWindow canvases.
                if (!pal::g_surface)
                    InstallWxSurfaceBridge();
                const int failures = RunWxCompositeSelfTest();
                std::exit(failures == 0 ? 0 : 1);
            }
            if (wxString(argv[i]) == wxT("--outputpane-selftest")) {
                const int failures = RunOutputPaneSelfTest();
                std::exit(failures == 0 ? 0 : 1);
            }
            if (wxString(argv[i]) == wxT("--pal-miniwindow-selftest")) {
                // CMiniWindow draws through the bridge unconditionally; the
                // normal install point is below (after this loop), so install
                // it here first.
                if (!pal::g_surface)
                    InstallWxSurfaceBridge();
                const int failures = RunPalMiniWindowSelfTest();
                std::exit(failures == 0 ? 0 : 1);
            }
        }

        // Don't let a write to a peer-closed MUD socket raise SIGPIPE and kill the
        // app; send() then returns EPIPE, which the world socket treats as a
        // disconnect.  (Per-socket SO_NOSIGPIPE also covers macOS; this is the
        // belt-and-suspenders net that also covers Linux send().)
        signal(SIGPIPE, SIG_IGN);

        // Name the app "MUSHclient" (the canonical Windows name) so the macOS
        // application menu's Hide/Quit items and the app menu title read
        // "MUSHclient" rather than the executable basename.
        SetAppName(wxT("MUSHclient"));
        SetAppDisplayName(wxT("MUSHclient"));

        // Install the pal::ISurface adapter as the miniwindow-drawing bridge
        // (docs/PAL.md S3) before any world/plugin can create a miniwindow.
        InstallWxSurfaceBridge();

        // Register every wxImage handler (BMP/PNG/ICO/…) up front, before any
        // MUSHclient code can load an image — the Tip-of-the-Day bulb, dialog art,
        // the Dock icon, plugin PNGs.  Registering lazily on first load is fragile:
        // whichever load happens first can run before wx is fully ready, and if it
        // doesn't register the handlers, later loads fail with "Unknown image data
        // format" (the litebulb.bmp error).  OnInit is the canonical place for this.
        wxInitAllImageHandlers();

        // Exit when the last top-level window is destroyed (the wx default, made
        // explicit) so closing the final window also ends the app.  The app-menu
        // / Cmd-Q Quit does NOT rely on this on macOS — it terminates via
        // -[NSApplication terminate:] (see wx_quit_application), because trusting
        // ExitOnFrameDelete to end the loop after closing the windows proved
        // unreliable there (the process lingered windowless).
        SetExitOnFrameDelete(true);

        // Install all MFC→wx bridge hooks before any MUSHclient code runs.
        wx_mfc_install_hooks();

        // Pre-load the recovered dialog/menu resources (logs the parse result).
        LoadMfcDialogResources();

        // Command-input conformance harness.  It builds a real world frame, so
        // it runs here — after the bridge hooks and dialog resources are in
        // place — but still before InitInstance, so no world is opened and the
        // user's prefs/startup list are never touched.
        for (int i = 1; i < argc; ++i)
            if (wxString(argv[i]) == wxT("--input-selftest"))
                std::exit(RunInputSelfTest());
        // The world-window conformance harness runs LATER than the other
        // selftests: it builds a real CWorldChildFrame, which reaches into the
        // MFC→wx bridge, so the hooks above must already be installed.  It
        // still exits before InitInstance, so no prefs, worlds or main window
        // are touched.
        for (int i = 1; i < argc; ++i) {
            if (wxString(argv[i]) == wxT("--world-frame-selftest")) {
                const int failures = RunWorldFrameSelfTest();
                std::exit(failures == 0 ? 0 : 1);
            }
        }
        // The menu-bar harness runs the bridge's own main-frame hook, so it
        // needs the hooks installed too -- and, like the others, exits before
        // InitInstance, so no world is opened and the geometry SAVE (which is
        // on close) never runs.
        for (int i = 1; i < argc; ++i) {
            if (wxString(argv[i]) == wxT("--menubar-selftest")) {
                const int failures = RunMenuBarSelfTest();
                std::exit(failures == 0 ? 0 : 1);
            }
        }

        // ── the extended (loop-running) selftests ──────────────────────────
        // Unlike every flag above, these do NOT exit here: they need the event
        // loop that OnRun starts, because that is the only way a paint is
        // delivered or a modal completes.  Arming installs isolated
        // preferences FIRST -- this mode runs far enough into the app to write
        // them, and on macOS the real ones belong to the user's installed copy
        // -- then schedules the harness and starts a watchdog.
        {
            wxString which;
            if (WantsExtendedSelfTest(argc, argv, which)) {
                if (!ArmExtendedSelfTest(which))
                    std::exit(2);            // unknown name: a usage error
                m_extendedSelfTest = true;
                return true;      // straight to OnRun; the harness ends the loop
            }
        }

        // Boot the real MUSHclient application.
        // The hook calls App.InitInstance() which creates the main MDI frame.
        if (!g_wxhook_init_instance || !g_wxhook_init_instance())
            return false;

        // Open any world files passed on the command line (matches the Windows
        // `mushclient world.mcl` behaviour).  Uses the same hook as File > Open
        // World.  InitInstance itself ignores argv on this port (the bootstrap
        // hook hard-codes an empty command line), so handle it here.
        if (g_wxhook_open_world_file)
            for (int i = 1; i < argc; ++i) {
                wxString path = argv[i];
                if (!path.empty())
                    g_wxhook_open_world_file(path.utf8_str());
            }

        // Discworld edition: open the bundled default world if nothing else
        // opened (no saved startup world, none on the command line).  No-op for
        // a normal build (no default-world marker in Contents/Resources).
        if (g_wxhook_open_default_world)
            g_wxhook_open_default_world();

        return true;
    }

    // The EXTENDED selftests are the only mode that reaches OnRun: everything
    // else exits from OnInit.  OnRun's return value is the process exit code,
    // so it is what carries their verdict out of the event loop.
    int OnRun() override
    {
        const int rc = wxApp::OnRun();
        return m_extendedSelfTest ? ExtendedSelfTestExitCode() : rc;
    }

    int OnExit() override
    {
        // ExitInstance hook could be added later if cleanup is needed.
        return wxApp::OnExit();
    }

    // MFC-parity top-level exception net.  On Windows, a CException* escaping a
    // command handler (e.g. ThrowErrorException from a bad regexp typed into the
    // Find/Recall dialogs) is caught by CWinApp::ProcessWndProcException, shown
    // in a message box, and the app keeps running.  Route the same cases through
    // the bridge hook (which rethrows and catches MFC exceptions); any other
    // exception type rethrows into wx's default termination path, unchanged.
    bool OnExceptionInMainLoop() override
    {
        if (g_wxhook_report_mfc_exception && g_wxhook_report_mfc_exception())
            return true;   // reported — keep the main loop running
        throw;             // not an MFC exception — default wx behaviour
    }

#ifdef __APPLE__
    // Every quit on macOS — Cmd-Q, the app menu, and the Dock's Quit — funnels
    // through Cocoa's applicationShouldTerminate:, which wx forwards here.  Run
    // the world save/close flow first: prompt to save modified worlds and persist
    // plugin layout (Quow's miniwindow positions).  Returning false (the user
    // cancelled a save prompt) cancels the quit; -[NSApp terminate:] otherwise
    // bypasses the MFC document save path entirely.
    bool OSXOnShouldTerminate() override
    {
        if (g_wxhook_query_quit_save && !g_wxhook_query_quit_save())
            return false;
        return wxApp::OSXOnShouldTerminate();
    }
#endif
};

wxIMPLEMENT_APP(CMUSHWxApp);

#endif // !_WIN32
