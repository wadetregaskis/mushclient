// wx_statusitem.h — the port's menu-bar ("system tray") item: what to show,
// and when.
//
// The AppKit half is platform/wx_osx_statusitem.{h,mm}; this half is portable
// C++ and decides the content.  Split that way for the same reason
// wx_osx_text.mm is split from its callers: AppKit types must not leak into
// TUs that also include the MFC/MUSHclient headers.
#pragma once

#ifndef _WIN32

#include <string>
#include <vector>

// One row of the menu-bar item's menu.  `command` is a MUSHclient command id:
// TRAY_FIRST_MENU + (world view number - 1) for a world (CMainFrame::OnTrayMenu
// consumes exactly that), or a canonical id such as ID_APP_ABOUT.  Zero means
// "separator", which is why no real command id may be zero (none is).
struct TrayMenuEntry
{
    std::string label;
    int         command = 0;
    bool        enabled = true;
};

// Build the menu as it should look RIGHT NOW.  Called on every menu open, so
// the world list is never stale.  See the .cpp for why the two Windows tray
// menus are merged into this one.
std::vector<TrayMenuEntry> BuildTrayMenu();

// Show or hide the menu-bar item according to App.m_iIconPlacement, with the
// icon App.m_iTrayIcon / App.m_strTrayIconFileName select.  Idempotent: safe to
// call at startup, whenever the preferences dialog closes, and in a test loop.
void ApplyTrayItemFromPrefs();

#endif // !_WIN32
