// platform/wx_dialog_bridge.cpp
//
// The MFC-dialog -> wxWidgets bridge.  Like wx_mfc_impl.cpp this file includes
// BOTH wx and MUSHclient headers: it owns the parsed dialog-resource registry
// and (subsequently) builds a wxDialog from a template, runs the canonical
// CDialog's OnInitDialog/DoDataExchange/handlers, and returns the modal result —
// so dialogs/**/*.cpp render natively with no per-dialog wx code.
//
// Step 9 (this commit): load + parse the recovered resources once, on demand.

#include <wx/wxprec.h>
#ifndef WX_PRECOMP
#  include <wx/wx.h>
#endif
#include <wx/stdpaths.h>
#include <wx/filename.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "wx_dialog_resource.h"

namespace {

std::string SlurpFile(const wxString& path)
{
    std::ifstream f(path.fn_str(), std::ios::binary);
    if (!f) return std::string();
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Locate a bundled resource file.  In a .app this is Contents/Resources/;
// for a plain (non-bundle) binary, fall back to the executable's directory.
wxString FindResource(const wxString& name)
{
    wxString candidates[] = {
        wxStandardPaths::Get().GetResourcesDir() + wxFILE_SEP_PATH + name,
        wxFileName(wxStandardPaths::Get().GetExecutablePath()).GetPath()
            + wxFILE_SEP_PATH + name,
    };
    for (const wxString& c : candidates)
        if (wxFileName::FileExists(c))
            return c;
    return candidates[0];   // best guess; caller logs the failure
}

} // namespace

// Process-wide registry of the recovered dialog/menu templates, loaded once.
RcResources& MfcDialogResources()
{
    static RcResources res;
    static bool loaded = false;
    if (!loaded)
    {
        loaded = true;
        wxString reshdr = FindResource(wxT("resource.h"));
        wxString rcfile = FindResource(wxT("mushclient_dialogs.rc"));
        std::string h  = SlurpFile(reshdr);
        std::string rc = SlurpFile(rcfile);
        if (!h.empty())  res.LoadSymbols(h);
        if (!rc.empty()) res.ParseRc(rc);
        std::fprintf(stderr,
            "[dialog-bridge] resources: %zu dialogs, %zu menus, %zu controls "
            "(%d unresolved control ids) from %s\n",
            res.DialogCount(), res.MenuCount(), res.ControlCount(),
            res.UnresolvedSymbols(),
            rc.empty() ? "<not found>" : (const char*)rcfile.utf8_str());
    }
    return res;
}

// Pre-load the resources at startup (wx-only callers use this void trampoline;
// it avoids first-dialog latency and surfaces any bundling problem immediately).
void LoadMfcDialogResources()
{
    MfcDialogResources();
}
