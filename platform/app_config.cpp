// platform/app_config.cpp — see the header.

#ifndef _WIN32

#include "platform/app_config.h"

#include <wx/fileconf.h>

namespace {
// Empty means "the user's real preferences".  A process-wide single value
// rather than anything thread-aware: it is set once, before the app opens
// anything, and never changes afterwards.
wxString g_testConfigPath;
} // namespace

std::unique_ptr<wxConfigBase> OpenAppConfig()
{
    if (!g_testConfigPath.empty()) {
        // wxCONFIG_USE_LOCAL_FILE with an explicit local path and an empty
        // global one: everything lands in the file we named and nothing falls
        // back to a system-wide location.
        return std::unique_ptr<wxConfigBase>(
            new wxFileConfig(wxEmptyString, wxEmptyString,
                             g_testConfigPath, wxEmptyString,
                             wxCONFIG_USE_LOCAL_FILE));
    }
    return std::unique_ptr<wxConfigBase>(new wxConfig(wxT("mushclient")));
}

void SetAppConfigPathForTesting(const wxString& path)
{
    g_testConfigPath = path;
}

wxString AppConfigPathForTesting()
{
    return g_testConfigPath;
}

#endif // !_WIN32
