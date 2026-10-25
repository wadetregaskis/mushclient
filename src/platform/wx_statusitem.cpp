// wx_statusitem.cpp — the portable half of the macOS menu-bar item.
//
// Global Preferences ▸ Tray/Taskbar ▸ "Icon Placement" offers Task bar / System
// tray / Both.  On this port the pref round-tripped through the dialog, the
// registry table and CMainFrame::AddTrayIcon and produced nothing at all: that
// function bails on `if (m_niData.hIcon == NULL) return;` (mainfrm.cpp:2000),
// and the icon can only ever be NULL here because ::LoadImage's RESOURCE form
// answers nullptr on the port (mfc_ui.h:4783).  Both of its callers ran; both
// achieved nothing.
//
// ── What the menu contains, and why it is ONE menu ────────────────────────
//
// Windows hangs two different menus off the tray icon, split by mouse button
// (CMainFrame::WindowProc, mainfrm.cpp:2085):
//
//   left click / double click  →  restore + SetForegroundWindow + SetFocus
//   right click                →  the same, then LeftTrayClick()
//
// LeftTrayClick (mainfrm.cpp:2125 — the name is a misnomer; only the RIGHT
// button reaches it) builds the list of open worlds, each labelled
// "name (new-lines)" and carrying the id TRAY_FIRST_MENU + view_number - 1,
// which CMainFrame::OnTrayMenu turns back into a world to activate.  That is
// the menu the Windows tray icon actually shows.
//
// RightTrayClick (mainfrm.cpp:2182) builds IDR_RIGHT_TRAY_MENU — About / Exit.
// TWO THINGS ABOUT IT ARE NOT WHAT THEY LOOK LIKE, and both were checked at
// HEAD before this was written:
//
//   * it has NO CALLER.  `grep -rn RightTrayClick` finds the definition, the
//     declaration in mainfrm.h and the port's stub declaration — and nothing
//     else, on any platform.  IDR_RIGHT_TRAY_MENU is therefore never shown on
//     Windows either.
//   * the "graft the main menu in" line is commented out (mainfrm.cpp:2218),
//     so even if it were called it would not carry the main menus.  The
//     mainmenu.LoadMenu(IDR_MUSHCLTYPE) above it is left dangling.
//
// A macOS status item has one click gesture and one menu, so the two are
// merged: the worlds (what the Windows icon shows) on top, a separator, then
// IDR_RIGHT_TRAY_MENU's own items, read from the recovered resource so the
// labels stay in step with it.  Every entry is one MUSHclient already defines
// for this icon; nothing new is invented.
//
// Not carried over: RightTrayClick's trailing "(enabled)" / "(disabled)" grey
// label.  Its comment says it exists because that menu could be raised while a
// modal dialog was up and MFC would not grey the items — a Windows crash
// avoidance, not a feature, and a permanently grey "(enabled)" row in a macOS
// menu bar is noise.
//
// ── Clicking the item ─────────────────────────────────────────────────────
//
// Choosing any entry activates the application first — the SetForegroundWindow
// + SetFocus that Windows does before it shows this menu.  Doing it at
// selection rather than at menu-open is deliberate: on macOS, raising every
// MUSHclient window over whatever the user is looking at merely because they
// opened a menu-bar menu would be startling, whereas each of these entries
// wants the app in front anyway (a world entry ends in ActivateClientWindow).
// So the pair of Windows behaviours — "activate the app" and "show the menu" —
// are both present, in the order macOS expects.

#ifndef _WIN32

#include <wx/wx.h>
#include <wx/image.h>
#include <wx/log.h>

#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "mainfrm.h"           // CMainFrame::OnTrayMenu, TRAY_FIRST_MENU/_COUNT
#include "resource.h"          // IDR_RIGHT_TRAY_MENU

#include "platform/wx_statusitem.h"
#include "platform/wx_osx_statusitem.h"
#include "platform/wx_dialog_bridge.h"      // LoadDialogArt, MfcDialogResources
#include "platform/wx_dialog_resource.h"    // RcMenu / RcMenuItem

#include <map>
#include <string>
#include <vector>

// ── The icon ──────────────────────────────────────────────────────────────
// The same choice CMainFrame::AddTrayIcon makes, over the same art the Dock
// icon already uses (platform/dialog_art, bundled into Contents/Resources):
// index 10 with a filename means the user's own file, 1..9 are the shipped
// novelty icons IDI_ICON1..9, and anything else is the default IDR_MUSHCLTYPE.
//
// Deliberately NOT blocked on new artwork.  macOS status items conventionally
// use a monochrome template image, and none of MUSHclient's icons is one — but
// -setTemplate: renders by alpha alone, which would flatten a document icon to
// a featureless blob and, worse, throw away the distinction the "Tray Icon"
// preference exists to express.  Colour status items are ordinary on macOS, so
// the shipped artwork is used as-is at menu-bar height.  Only the last-resort
// fallback (drawn in the .mm when even IDR_MAINFRAME is missing) is a template
// image, because it IS monochrome by construction.
static wxBitmap TrayIconBitmap()
{
    const int sel = (int) App.m_iTrayIcon;
    wxBitmap bmp;

    if (sel == 10 && !App.m_strTrayIconFileName.IsEmpty())
    {
        wxImage im;
        wxLogNull noLog;   // a bad path must degrade, not pop an error dialog
        if (im.LoadFile(wxString::FromUTF8((const char*) App.m_strTrayIconFileName)))
            bmp = wxBitmap(im);
    }
    else if (sel >= 1 && sel <= 9)
        bmp = LoadDialogArt(287 + sel);        // IDI_ICON1 (288) .. IDI_ICON9 (296)

    if (!bmp.IsOk())
        bmp = LoadDialogArt(129);              // IDR_MUSHCLTYPE — Windows' tray default
    if (!bmp.IsOk())
        bmp = LoadDialogArt(128);              // IDR_MAINFRAME — the app icon
    // Still possibly invalid (no bundled art at all): the .mm then draws its own
    // placeholder, so a missing file can never leave an invisible blank item.
    return bmp;
}

// ── The menu model ────────────────────────────────────────────────────────

std::vector<TrayMenuEntry> BuildTrayMenu()
{
    std::vector<TrayMenuEntry> out;

    // ── worlds: CMainFrame::LeftTrayClick, verbatim ───────────────────────
    // The canonical std::multimap<string,int>: sorted by the displayed name,
    // and the VALUE is view_number - 1, not the position.  The
    // two differ whenever alphabetical order differs from open order, and
    // OnTrayMenu resolves by view number, so carrying the position would
    // activate the wrong world.
    std::multimap<std::string, int> worlds;
    if (App.m_pWorldDocTemplate)          // null before InitInstance has run
    {
        POSITION pos = App.m_pWorldDocTemplate->GetFirstDocPosition();
        while (pos != NULL)
        {
            CMUSHclientDoc* pDoc =
                (CMUSHclientDoc*) App.m_pWorldDocTemplate->GetNextDoc(pos);
            if (!pDoc)
                continue;

            CString strName = pDoc->m_mush_name;
            if (pDoc->m_new_lines)
                // (int), which the canonical mainfrm.cpp:2165 does not need:
                // m_new_lines is a long, 32-bit on Win32 but 64-bit here, and
                // "%i" reads four bytes.  Same text, no LP64 varargs mismatch.
                strName += CFormat(" (%i)", (int) pDoc->m_new_lines);

            if ((pDoc->m_view_number - 1) < TRAY_MENU_COUNT)
                worlds.insert(std::make_pair(std::string((const char*) strName),
                                             pDoc->m_view_number - 1));
        }
    }

    if (worlds.empty())
    {
        TrayMenuEntry e;
        e.label   = "(no worlds open)";
        e.command = TRAY_FIRST_MENU;   // the id Windows gives this placeholder
        e.enabled = false;
        out.push_back(e);
    }
    else
        for (std::multimap<std::string, int>::const_iterator i = worlds.begin();
             i != worlds.end(); ++i)
        {
            TrayMenuEntry e;
            e.label   = i->first;
            e.command = TRAY_FIRST_MENU + i->second;
            e.enabled = true;
            out.push_back(e);
        }

    // ── separator ─────────────────────────────────────────────────────────
    {
        TrayMenuEntry e;
        e.command = 0;               // 0 IS the separator marker
        e.enabled = false;
        out.push_back(e);
    }

    // ── IDR_RIGHT_TRAY_MENU, from the recovered resource ──────────────────
    // Read rather than hard-coded so the labels track the .rc.  The template is
    // one POPUP wrapping the real items; its own trailing separator is dropped
    // (a menu must not end in one).
    if (const RcMenu* m = MfcDialogResources().FindMenu(IDR_RIGHT_TRAY_MENU))
    {
        const std::vector<RcMenuItem>* items = &m->items;
        if (items->size() == 1 && (*items)[0].isPopup)
            items = &(*items)[0].children;

        for (size_t i = 0; i < items->size(); ++i)
        {
            const RcMenuItem& it = (*items)[i];
            if (it.isSeparator || it.isPopup || it.id == 0)
                continue;
            TrayMenuEntry e;
            e.label   = it.text;
            e.command = it.id;
            e.enabled = true;
            out.push_back(e);
        }
    }

    return out;
}

// ── Pull interface for the NSMenu delegate ────────────────────────────────
// The model is rebuilt by mc_statusitem_menu_count and then read row by row, so
// the labels must outlive that call — hence the file-static cache rather than a
// value returned across the C boundary.
namespace {
std::vector<TrayMenuEntry> g_menu;
}

extern "C" int mc_statusitem_menu_count(void)
{
    g_menu = BuildTrayMenu();
    return (int) g_menu.size();
}

extern "C" const char* mc_statusitem_menu_label(int index)
{
    if (index < 0 || index >= (int) g_menu.size()) return "";
    return g_menu[index].label.c_str();
}

extern "C" int mc_statusitem_menu_command(int index)
{
    if (index < 0 || index >= (int) g_menu.size()) return 0;
    return g_menu[index].command;
}

extern "C" int mc_statusitem_menu_enabled(int index)
{
    if (index < 0 || index >= (int) g_menu.size()) return 0;
    return g_menu[index].enabled ? 1 : 0;
}

extern "C" void mc_statusitem_menu_invoke(int command)
{
    if (command == 0)
        return;

    // Windows' SetForegroundWindow + SetFocus, done first — see the header
    // comment for why this is at selection time rather than at menu-open.
    wxOSXActivateSelf();

    // A world: hand it straight to the canonical handler, which already has a
    // non-Windows arm (mainfrm.cpp:2245 — pDoc->ActivateClientWindow()).
    if (command >= TRAY_FIRST_MENU && command < TRAY_FIRST_MENU + TRAY_MENU_COUNT)
    {
        Frame.OnTrayMenu((UINT) command);
        return;
    }

    // Anything else is a canonical command id out of IDR_RIGHT_TRAY_MENU.
    // Route it through the main frame exactly as a menu-bar click does, so it
    // gets the whole dispatcher (the canonical→wx id translation, the
    // table-driven handlers, the port-local arms) rather than a second copy.
    if (App.m_pMainWnd && App.m_pMainWnd->m_hWnd)
    {
        wxWindow* frame = static_cast<wxWindow*>(App.m_pMainWnd->m_hWnd);
        wxCommandEvent evt(wxEVT_MENU, command);
        evt.SetEventObject(frame);
        frame->GetEventHandler()->ProcessEvent(evt);
    }
}

// ── The preference ────────────────────────────────────────────────────────

void ApplyTrayItemFromPrefs()
{
    const bool wanted = App.m_iIconPlacement == ICON_PLACEMENT_TRAY ||
                        App.m_iIconPlacement == ICON_PLACEMENT_BOTH;

    // Note the "else": AddTrayIcon has no canonical counterpart that REMOVES
    // the icon, so turning the setting off in the preferences dialog would
    // otherwise leave the item in the menu bar until the next launch.  Doing
    // the hide here (rather than adding a RemoveTrayIcon) keeps both callers of
    // AddTrayIcon honest with one function.
    if (!wanted)
    {
        wxOSXStatusItemHide();
        return;
    }

    wxOSXStatusItemShow(TrayIconBitmap(), "MUSHclient");
}

// ── ICON_PLACEMENT_TRAY on macOS ──────────────────────────────────────────
// Windows' "System tray" (as opposed to "Both") ALSO hides the app from the
// task bar, via the hidden owner window MUSHclient.cpp:630-650 creates.  There
// is no equivalent here and none is invented: an app can be hidden from the
// Dock only by LSUIElement in Info.plist, which is fixed at build time, not a
// runtime switch — so it could not follow a preference anyway, and doing it
// would also remove the app menu and the ⌘Tab entry.  "System tray" and "Both"
// therefore differ on Windows and behave alike here: the menu-bar item appears,
// the Dock icon stays.


#ifndef __APPLE__
#include <wx/taskbar.h>
#include <wx/dcmemory.h>
#include <memory>

namespace {

static wxBitmap MakePlaceholderBitmap(int side)
{
    wxBitmap bmp(side, side, 32);
    wxMemoryDC dc(bmp);
    dc.SetBackground(*wxWHITE_BRUSH);
    dc.Clear();
    dc.SetPen(wxPen(*wxBLACK, 1));
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    dc.DrawRoundedRectangle(1, 1, side - 2, side - 2, 3.0);
    dc.SetBrush(*wxBLACK_BRUSH);
    dc.DrawCircle(side / 2, side / 2, 2);
    dc.SelectObject(wxNullBitmap);
    return bmp;
}

class CMcTaskBarIcon : public wxTaskBarIcon {
public:
    wxBitmap m_bmp;
    std::unique_ptr<wxMenu> m_builtMenu;

    wxMenu* BuildMenu()
    {
        wxMenu* menu = new wxMenu();
        const int n = mc_statusitem_menu_count();
        for (int i = 0; i < n; ++i) {
            const int command = mc_statusitem_menu_command(i);
            if (command == 0) {
                menu->AppendSeparator();
                continue;
            }
            const char* label = mc_statusitem_menu_label(i);
            wxMenuItem* item = menu->Append(command, wxString::FromUTF8(label ? label : ""));
            item->Enable(mc_statusitem_menu_enabled(i) != 0);
        }
        menu->Bind(wxEVT_MENU, [](wxCommandEvent& evt) {
            mc_statusitem_menu_invoke(evt.GetId());
        });
        return menu;
    }

    wxMenu* CreatePopupMenu() override
    {
        return BuildMenu();
    }
};

static CMcTaskBarIcon* g_taskBarIcon = nullptr;

} // namespace

#include <wx/evtloop.h>

bool wxOSXStatusItemShow(const wxBitmap& icon, const char* tooltip)
{
    if (!g_taskBarIcon)
        g_taskBarIcon = new CMcTaskBarIcon();
    wxBitmap bmp = icon.IsOk() ? icon : MakePlaceholderBitmap(18);
    g_taskBarIcon->m_bmp = bmp;
    if (wxEventLoopBase::GetActive())
        g_taskBarIcon->SetIcon(wxBitmapBundle::FromBitmap(bmp),
                               wxString::FromUTF8(tooltip ? tooltip : ""));
    return true;
}

void wxOSXStatusItemHide()
{
    if (!g_taskBarIcon)
        return;
    g_taskBarIcon->RemoveIcon();
    g_taskBarIcon->Destroy();
    g_taskBarIcon = nullptr;
}

void wxOSXActivateSelf()
{
    if (App.m_pMainWnd && App.m_pMainWnd->m_hWnd) {
        if (auto* tlw = dynamic_cast<wxTopLevelWindow*>(
                static_cast<wxWindow*>(App.m_pMainWnd->m_hWnd)))
            tlw->Raise();
    }
}

bool wxOSXStatusItemVisible()
{
    return g_taskBarIcon != nullptr && g_taskBarIcon->m_bmp.IsOk();
}

int wxOSXStatusItemImageWidth()
{
    return (g_taskBarIcon && g_taskBarIcon->m_bmp.IsOk())
               ? g_taskBarIcon->m_bmp.GetWidth() : 0;
}

int wxOSXStatusItemImageHeight()
{
    return (g_taskBarIcon && g_taskBarIcon->m_bmp.IsOk())
               ? g_taskBarIcon->m_bmp.GetHeight() : 0;
}

int wxOSXStatusItemRebuildMenu()
{
    if (!g_taskBarIcon) return -1;
    g_taskBarIcon->m_builtMenu.reset(g_taskBarIcon->CreatePopupMenu());
    if (!g_taskBarIcon->m_builtMenu) return -1;
    return (int)g_taskBarIcon->m_builtMenu->GetMenuItemCount();
}

bool wxOSXStatusItemMenuItemAt(int index, char* title, int titleLen,
                               int* tag, int* enabled, int* separator)
{
    if (titleLen > 0 && title)
        title[0] = '\0';
    if (!g_taskBarIcon || !g_taskBarIcon->m_builtMenu) return false;
    wxMenu* menu = g_taskBarIcon->m_builtMenu.get();
    if (index < 0 || (size_t)index >= menu->GetMenuItemCount()) return false;
    wxMenuItem* item = menu->FindItemByPosition((size_t)index);
    if (!item) return false;
    if (title && titleLen > 0) {
        const wxString label = item->GetItemLabelText();
        const wxScopedCharBuffer utf8 = label.utf8_str();
        const char* s = utf8.data() ? utf8.data() : "";
        std::strncpy(title, s, (size_t)titleLen - 1);
        title[titleLen - 1] = '\0';
    }
    if (tag)       *tag       = item->GetId();
    if (enabled)   *enabled   = item->IsEnabled() ? 1 : 0;
    if (separator) *separator = item->IsSeparator() ? 1 : 0;
    return true;
}
#endif // !__APPLE__

#endif // !_WIN32
