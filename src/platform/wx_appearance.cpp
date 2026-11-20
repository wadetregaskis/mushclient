// wx_appearance.cpp -- Global Preferences' "Appearance" on macOS and Linux.
//
// wxWidgets follows the system by itself: NSApplication's effective appearance on
// macOS (the bundle script does not set NSRequiresAquaSystemAppearance), the GTK
// theme on Linux.  All that is left to do is forcing light or dark when the user asks.
// wxApp::SetAppearance arrived in 3.3.0; with anything older the preference is inert
// and the client just follows the system.  (Windows: src/appearance_win.cpp.)

#include <wx/wx.h>
#include <wx/app.h>
#include <wx/settings.h>
#include <wx/version.h>

#include "appearance.h"

extern void (* g_pfnAppearanceApply) (int iPreference);
extern bool (* g_pfnAppearanceIsDark) ();

static void ApplyAppearance (int iPreference)
{
#if wxCHECK_VERSION(3, 3, 0)
    if (!wxTheApp)
        return;

    wxApp::Appearance appearance = wxApp::Appearance::System;
    if (iPreference == APPEARANCE_LIGHT)
        appearance = wxApp::Appearance::Light;
    else if (iPreference == APPEARANCE_DARK)
        appearance = wxApp::Appearance::Dark;

    // Can refuse (AppearanceResult::CannotChange) on a toolkit that needs it decided
    // before any window exists: that leaves the system's choice until the next start.
    wxTheApp->SetAppearance(appearance);
#else
    (void) iPreference;
#endif
}

static bool AppearanceIsDark ()
{
    return wxSystemSettings::GetAppearance().IsDark();
}

void WxAppearance_Install ()
{
    g_pfnAppearanceApply = ApplyAppearance;
    g_pfnAppearanceIsDark = AppearanceIsDark;
}
