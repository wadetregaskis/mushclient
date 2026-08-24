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
//
// ── Rearranging (2026-08-23) ──────────────────────────────────────────
// All three bars are created CBRS_GRIPPER and given EnableDocking
// (CBRS_ALIGN_ANY) on Windows (mainfrm.cpp:318-409), and the very first startup
// tip is about it — tips.txt:1, "You can drag the "game" toolbar alongside the
// "file" toolbar, to save room at the top of the screen".  So the bars here sit
// in a wxAuiManager-managed STRIP that occupies the top row of the world
// window's sizer: drag a bar by its gripper to reorder the rows, or to pack two
// or three of them onto one row.
//
// Deliberately NOT offered: docking a bar against the window's left, right or
// bottom edge, and floating it in a window of its own.  Those describe where
// the STRIP sits, not how the bars sit within it, and the strip is a single row
// of the frame's wxBoxSizer — the output pane, find bar, command bar, info bar
// and status bar are that sizer's other rows, and none of them is an AUI pane.
// Supporting them would mean handing the whole world window to wxAuiManager and
// re-expressing all five as panes, which is a different change from this one.
//
// Placement is app-wide, exactly as visibility already is: Windows has ONE set
// of bars, on its single always-present main frame, so a drag there moves "the"
// toolbars.  Here every world window owns a set, so a drag in one is pushed to
// all the others — the same rule wx_mfc_impl.cpp's View ▸ Toolbars handler
// follows for a visibility toggle.
//
// It is stored as a row plus an ORDER WITHIN THAT ROW, not as a wxAuiManager
// perspective string, because wxAuiManager rewrites a toolbar pane's dock_pos
// to a PIXEL offset while it lays the row out (measured: a bar dropped at
// position 1 came back reading 321).  A perspective saved from a 900-px window
// therefore carries numbers a 600-px window cannot reproduce, and two windows
// of different widths would push conflicting layouts at each other forever.  A
// row and an order are the same on any width.

#pragma once

#ifndef _WIN32

#include <wx/wx.h>
#include <wx/toolbar.h>
#include <memory>
#include <set>

class CMUSHclientDoc;
class wxAuiManager;

class CWorldToolBars
{
public:
    enum Which { eStandard = 0, eWorld = 1, eActivity = 2, eBarCount = 3 };

    // Where a bar sits in the strip: which row of it, and where along that row,
    // counted from 0 at the left.  That is the whole of the drag state — the
    // strip is one top dock, so there is no direction, size or float rectangle.
    struct Placement { int row = 0; int order = 0; };

    // Builds the bars inside a strip inserted at the top of `sizer`, restoring
    // the saved arrangement.  A bar whose art is missing is skipped: the others
    // still appear.
    CWorldToolBars (wxFrame* frame, wxBoxSizer* sizer);
    ~CWorldToolBars ();

    void SetBarVisible (Which which, bool show);
    bool IsBarVisible  (Which which) const;

    // View ▸ Reset Toolbar Locations.
    void ResetLocations ();

    // ── Drag state ──────────────────────────────────────────────────
    Placement GetPlacement (Which which) const;

    // Put the bars where this says — which is all a completed drag amounts to.
    // The GESTURE belongs to wxAuiManager and is not ours to test: there is no
    // way to inject the mouse events it reads.  Everything the gesture ends in
    // IS ours, and this is the seam the in-app selftest drives it through: the
    // relayout, the strip's new height, the save, and the push to every other
    // world window.  Taking the whole arrangement rather than one bar and a
    // destination is what lets it express a pure REORDER — three bars each
    // still alone on a row, in a different order — which is the case that has
    // to leave StripHeight alone.  Also how ResetLocations restores canonical.
    void SetArrangement (const Placement place[eBarCount]);

    // The height the strip actually HAS, in pixels — one row's worth per row.
    // This is what tells "two bars now share a row" apart from "the code did
    // nothing": a pure REORDER leaves it unchanged, a MERGE drops it.  It is
    // the real size rather than the wanted one deliberately, so that it also
    // covers the frame being told (the strip is a row of the frame's sizer, and
    // an arrangement the frame never re-laid out leaves a blank gap).
    int StripHeight () const;

    // Re-run the canonical ON_UPDATE_COMMAND_UI equivalents: enable/disable and
    // check state for every button, plus the activity bar's per-world image.
    // Cheap enough for the world timer; only touches wx state that changed.
    void UpdateState (CMUSHclientDoc* activeDoc);

    // Re-read App.m_iActivityButtonBarStyle and swap the activity strip.
    // Mirrors the LoadBitmap call GlobalPrefs.cpp makes when prefs are OK'd.
    void ReloadActivityStyle ();

private:
    wxToolBar* Build (Which which);
    // Read back what wxAuiManager currently has, canonicalised: rows compacted
    // from 0 and orders restarted within each row.  Hidden bars and bars that
    // failed to build keep the shared record's value rather than being renumbered
    // out of existence.
    void       ReadPlacements  (Placement out[eBarCount]) const;
    void       ApplyPlacements (const Placement place[eBarCount]);
    // Save + push to the other world windows, but only if something moved.
    void       PublishPlacements ();
    // The frame owns the strip's height (it is a sizer row), so re-lay the frame
    // out when the AUI layout changes what the strip needs.
    void       SyncStripHeight ();
    // Called after a drag: from wxAuiManager's own render event for immediacy,
    // and again from the 100 ms UpdateState tick so a missed render costs
    // latency rather than a wrong layout.
    void       PollLayout ();
    // Act on a click.  Takes the id rather than the button record because the
    // button table is private to the .cpp.
    void       DispatchId (int id);
    void       Forward (int cmdId);
    CMUSHclientDoc* ActiveDoc () const;

    wxFrame*    m_frame;
    CMUSHclientDoc* m_doc = nullptr;    // active world, refreshed by UpdateState
    wxBoxSizer* m_sizer;
    wxPanel*    m_host    = nullptr;    // the strip: one row of m_sizer
    wxAuiManager* m_mgr   = nullptr;    // manages m_host, not the frame
    wxToolBar*  m_bars    [eBarCount];
    bool        m_visible [eBarCount];

    int         m_stripHeight  = -1;    // last wanted height acted on; -1 = never
    bool        m_pollPending  = false; // a deferred PollLayout is already queued
    // Makes that deferred call a no-op if this object is gone before it runs.
    std::shared_ptr<bool> m_alive;

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
