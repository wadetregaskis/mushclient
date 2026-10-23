// platform/app_config.cpp — see the header.

#ifndef _WIN32

#include "platform/app_config.h"

#include <wx/fileconf.h>
#include <sys/stat.h>
#include <string>

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
#ifdef __WXGTK__
    // On Linux/Unix wxConfig("mushclient") defaults to ~/.mushclient, which
    // collides with the directory ~/.mushclient/ created for history files.
    // Store preferences in ~/.mushclient/preferences.cfg instead.
    const char* home = getenv("HOME");
    if (home && *home) {
        std::string dir = std::string(home) + "/.mushclient";
        ::mkdir(dir.c_str(), 0700);
        std::string path = dir + "/preferences.cfg";
        return std::unique_ptr<wxConfigBase>(
            new wxFileConfig(wxEmptyString, wxEmptyString,
                             wxString::FromUTF8(path.c_str()), wxEmptyString,
                             wxCONFIG_USE_LOCAL_FILE));
    }
#endif
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
