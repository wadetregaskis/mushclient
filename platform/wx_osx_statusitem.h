// wx_osx_statusitem.h — the macOS menu-bar (status-bar) item that stands in for
// MUSHclient's Windows system-tray icon.
//
// Global Preferences ▸ Tray/Taskbar ▸ "Icon Placement: System tray" (and
// "Both") asks for an icon outside the app's own windows.  On Windows that is
// Shell_NotifyIcon; the canonical CMainFrame::AddTrayIcon (mainfrm.cpp:1980)
// builds one and could never show anything here, because it needs ::LoadImage's
// RESOURCE form, which this port answers with nullptr (mfc_ui.h:4783) — so the
// setting round-tripped perfectly and did nothing.
//
// macOS has no system tray.  The closest thing, and the one users expect, is an
// NSStatusItem in the menu bar: a small icon at the right of the menu bar with
// a menu on it.  This header is the whole AppKit surface; everything that
// decides WHAT to show lives in the portable wx_statusitem.cpp beside it.
//
// The menu's CONTENT is pulled, not pushed: the item's NSMenu has a delegate,
// so every time the user opens it AppKit calls back through the five
// mc_statusitem_* entry points below and the menu is rebuilt from the live
// document list.  A pushed snapshot would go stale the moment a world opened,
// closed or received a line.
//
// No-op on every non-Apple platform.
#pragma once

class wxBitmap;

#ifndef _WIN32

// Create (or update) the menu-bar item.  `icon` is drawn at the menu bar's own
// height; an invalid bitmap falls back to a drawn placeholder so the item is
// never an invisible blank (see the .mm).  `tooltip` may be null.
// Returns true if the item exists afterwards.
bool wxOSXStatusItemShow(const wxBitmap& icon, const char* tooltip);

// Remove the menu-bar item.  Safe when there is none.
void wxOSXStatusItemHide();

// Bring MUSHclient to the front — the [NSApp activateIgnoringOtherApps:] that
// stands in for the SetForegroundWindow + SetFocus Windows does when the tray
// icon is clicked.  Lives here rather than in wx_osx_text.mm because the
// existing wxOSXActivateApp() takes an application NAME and walks
// -[NSWorkspace runningApplications]; activating ourselves needs neither.
void wxOSXActivateSelf();

// True while the menu-bar item exists.
bool wxOSXStatusItemVisible();

// ── Observability, for platform/statusitem_selftest.cpp ────────────────────
// A status item is not reachable through wx, and "the code did nothing" must
// FAIL the test rather than silently pass, so the harness needs to read back
// what AppKit actually holds — not what we believe we asked for.

// Pixel size of the image the item's button is currently showing.  0 when
// there is no item or no image; a blank invisible item therefore fails.
int wxOSXStatusItemImageWidth();
int wxOSXStatusItemImageHeight();

// Drive the NSMenu delegate exactly as opening the menu does, and return the
// number of NSMenuItems it built (-1 when there is no item).  This is the
// assertion that the pull path works end to end, delegate included.
int wxOSXStatusItemRebuildMenu();

// Read back one built NSMenuItem.  `title` receives at most `titleLen` bytes
// (always NUL-terminated when titleLen > 0).  `tag` receives the MUSHclient
// command id carried on the item, `enabled` 1/0, `separator` 1/0.  Any output
// pointer may be null.  False when there is no item at that index.
bool wxOSXStatusItemMenuItemAt(int index, char* title, int titleLen,
                               int* tag, int* enabled, int* separator);

// ── Called BY the .mm, implemented in wx_statusitem.cpp ────────────────────
// The menu model, pulled at open time.  Kept as plain C so the .mm needs none
// of the MUSHclient headers.
extern "C" {
int         mc_statusitem_menu_count(void);         // rebuilds the model, returns its size
const char* mc_statusitem_menu_label(int index);    // "" out of range
int         mc_statusitem_menu_command(int index);  // command id; 0 for a separator
int         mc_statusitem_menu_enabled(int index);  // 1/0
void        mc_statusitem_menu_invoke(int command); // activate the app + run the command
}

#endif // !_WIN32
