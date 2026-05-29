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
#include "platform/wx_mfc_hooks.h"

class CMUSHWxApp : public wxApp {
public:
    bool OnInit() override
    {
        // Install all MFC→wx bridge hooks before any MUSHclient code runs.
        wx_mfc_install_hooks();

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
