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
#include "platform/wx_mfc_hooks.h"

// Defined in wx_dialog_bridge.cpp: parse the bundled dialog resources once.
void LoadMfcDialogResources();

class CMUSHWxApp : public wxApp {
public:
    bool OnInit() override
    {
        // Name the app "MUSHclient" (the canonical Windows name) so the macOS
        // application menu's Hide/Quit items and the app menu title read
        // "MUSHclient" rather than the executable basename.
        SetAppName(wxT("MUSHclient"));
        SetAppDisplayName(wxT("MUSHclient"));

        // Register every wxImage handler (BMP/PNG/ICO/…) up front, before any
        // MUSHclient code can load an image — the Tip-of-the-Day bulb, dialog art,
        // the Dock icon, plugin PNGs.  Registering lazily on first load is fragile:
        // whichever load happens first can run before wx is fully ready, and if it
        // doesn't register the handlers, later loads fail with "Unknown image data
        // format" (the litebulb.bmp error).  OnInit is the canonical place for this.
        wxInitAllImageHandlers();

        // Quit the app when the last top-level window is destroyed (the default,
        // made explicit): together with the Quit handler closing every top-level
        // window, this guarantees Cmd-Q actually terminates rather than leaving a
        // windowless process behind.
        SetExitOnFrameDelete(true);

        // Install all MFC→wx bridge hooks before any MUSHclient code runs.
        wx_mfc_install_hooks();

        // Pre-load the recovered dialog/menu resources (logs the parse result).
        LoadMfcDialogResources();

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

        return true;
    }

    int OnExit() override
    {
        // ExitInstance hook could be added later if cleanup is needed.
        return wxApp::OnExit();
    }
};

wxIMPLEMENT_APP(CMUSHWxApp);

#endif // !_WIN32
