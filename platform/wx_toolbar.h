// wx_toolbar.h — the three canonical MFC toolbars, rebuilt over wxToolBar.
//
// Windows builds Standard / World / Activity as dockable CToolBars owned by
// CMainFrame (mainfrm.cpp OnCreate), with layouts from IDR_MAINFRAME,
// IDR_GAME_TOOLBAR and IDR_ACTIVITY_TOOLBAR in MUSHclient.rc and art from
// res/toolbar1.bmp, res/game_too.bmp and the six activity strips.
//
// On wxOSX the wxMDIParentFrame is hidden — the world child frame IS the window
// the user sees — so a toolbar on the parent would never appear.  Each world
// window therefore carries its own set, exactly as it already carries its own
// status bar (see apply_status_bar_visibility in wx_mfc_impl.cpp).

#pragma once

#ifndef _WIN32

#include <wx/wx.h>
#include <wx/toolbar.h>
#include <set>

class CMUSHclientDoc;

class CWorldToolBars
{
public:
    enum Which { eStandard = 0, eWorld = 1, eActivity = 2, eBarCount = 3 };

    // Builds the bars as children of `frame` and inserts them at the top of
    // `sizer`, in canonical order.  A bar whose art is missing is skipped: the
    // others still appear.
    CWorldToolBars (wxFrame* frame, wxBoxSizer* sizer);

    void SetBarVisible (Which which, bool show);
    bool IsBarVisible  (Which which) const;

    // View ▸ Reset Toolbar Locations.  There is no drag-docking here (the bars
    // are sizer rows, not floating frames), so "reset" means "show all three".
    void ResetLocations ();

    // Re-run the canonical ON_UPDATE_COMMAND_UI equivalents: enable/disable and
    // check state for every button, plus the activity bar's per-world image.
    // Cheap enough for the world timer; only touches wx state that changed.
    void UpdateState (CMUSHclientDoc* activeDoc);

    // Re-read App.m_iActivityButtonBarStyle and swap the activity strip.
    // Mirrors the LoadBitmap call GlobalPrefs.cpp makes when prefs are OK'd.
    void ReloadActivityStyle ();

private:
    wxToolBar* Build (Which which);
    // Act on a click.  Takes the id rather than the button record because the
    // button table is private to the .cpp.
    void       DispatchId (int id);
    void       Forward (int cmdId);
    CMUSHclientDoc* ActiveDoc () const;

    wxFrame*    m_frame;
    CMUSHclientDoc* m_doc = nullptr;    // active world, refreshed by UpdateState
    wxBoxSizer* m_sizer;
    wxToolBar*  m_bars    [eBarCount];
    bool        m_visible [eBarCount];

    // Last image index pushed to each activity button, so UpdateState only
    // rebuilds a bitmap when the world's state actually changed (the canonical
    // OnUpdateBtnWorlds does the same `if (iNewImage != iImage)` test).
    int         m_activityImage [10];

    // Command ids owned by these bars.  The forwarding handler consults this so
    // it never intercepts an unrelated command routed through this frame.
    std::set<int> m_ourIds;

    // True while a click is being handed to the parent frame's command handler;
    // see the guard in the forwarding lambda for why this is not optional.
    bool m_forwarding = false;
};

#endif  // !_WIN32
