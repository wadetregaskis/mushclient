// platform/app_config.h — one door to the app's persisted preferences.
//
// Everything the port remembers between runs — the recent-worlds list, main and
// per-world window geometry, toolbar placement, the find bar's last search —
// goes through wxConfig, and every site used to construct its own:
//
//     wxConfig cfg (wxT ("mushclient"));
//
// On macOS that resolves to ~/Library/Preferences/mushclient Preferences: the
// REAL file belonging to the user's installed copy of the app.  Fine in
// production and a hazard in a test, because a selftest that reaches far enough
// into the app lifecycle to touch preferences would silently rewrite the
// preferences of whatever MUSHclient the person is actually using.  Setting
// $HOME does not help — macOS resolves the preferences directory from the
// password database, not the environment (see the port-memory note on wxConfig
// living in the real home).
//
// So the construction is behind this factory instead, and a test can point it
// at a scratch file.  The shape deliberately mirrors what it replaced: it hands
// back a NEW config object per call, which the caller lets go out of scope, so
// reads still see the current file contents and writes still flush at the end
// of the scope.  A long-lived shared instance would have changed when data
// reaches disk, which is not a change worth making for this.

#pragma once

#ifndef _WIN32

#include <wx/config.h>
#include <wx/string.h>
#include <memory>

// A config object for the app's preferences.  Never null.  Let it go out of
// scope to flush, exactly as the local wxConfig it replaced.
std::unique_ptr<wxConfigBase> OpenAppConfig();

// Redirect OpenAppConfig at `path` instead of the user's real preferences.
// An empty path restores the real ones.  Intended for the extended selftests,
// which run far enough into the app to write preferences; see
// platform/wx_app.cpp.
void SetAppConfigPathForTesting(const wxString& path);

// Where OpenAppConfig is currently pointing: empty when it is the user's real
// preferences.  Exists so a test can ASSERT it has been redirected rather than
// assume it — the failure being silently corrupting someone's live settings,
// which no assertion downstream would notice.
wxString AppConfigPathForTesting();

#endif // !_WIN32
