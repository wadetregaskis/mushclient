// platform/menu_ui_rules.h — main-menu enable/check rules for the wx frame.
//
// On Windows the menu bar is greyed for free: MFC's m_bAutoMenuEnable walks the
// command-routing chain (active view → its document → CMainFrame → the app) and
// disables any item nothing in the chain handles.  With no world open, every
// command whose only ON_COMMAND lives on CMUSHView / CSendView / CMUSHclientDoc
// is therefore greyed out — that greying is how the user is told a world must
// be open.
//
// The port has no such chain: wxWidgets leaves a menu item enabled unless an
// update-UI handler says otherwise, so those same commands stayed live and
// silently did nothing when chosen (the frame's dispatcher `dynamic_cast`s to
// the active CWorldChildFrame and simply returns when there is none).
//
// These rules restate MFC's answer as data, and live in a shared TU rather than
// inside the wx frame's update-UI lambda so they can be tested headlessly
// (tests/test_menu_rules.cpp) — the lambda itself needs a live wxFrame.

#pragma once

class CMUSHclientDoc;

// The enable / check state of one main-menu command.
struct MenuCmdUiState {
    bool handled = false;   // false → not one of ours; the caller decides
    bool enabled = true;
    int  checked = -1;      // -1 = not a check item; 0 = unticked, 1 = ticked
};

// Enable/check for menu command `id` against the active world document (null
// when no world window is active).  Returns handled == false for ids these
// rules do not own, leaving them to the caller's own arms.
MenuCmdUiState MenuCommandUiState(int id, CMUSHclientDoc* doc);

// ── Canonical command ids the port's own menu bar spells differently ───────
// world.DoCommand(name) looks `name` up in accelerators.cpp's CommandIDs[] and
// posts the CANONICAL MFC id (ID_EDIT_CUT = 0xE123, ID_FILE_CLOSE = 0xE102, …).
// The port's menu bar builds those same commands on wx's own standard ids
// (wxID_CUT, wxID_CLOSE, …) because AppKit needs them there, so the frame's
// dispatcher matched the wx id only and every one of these DoCommand names was
// a silent no-op that still returned eOK.
//
// The mapping is expressed as a port-neutral action rather than a wx id so the
// table can be tested without linking wxWidgets; the frame turns the action
// into its own id.
enum PortMenuAction {
    kPortMenuNone = 0,
    kPortMenuCloseWorld,
    kPortMenuCut,
    kPortMenuPaste,
    kPortMenuSelectAll,
    kPortMenuUndo,
    kPortMenuAbout,
    kPortMenuCascade,
    kPortMenuTileHorz,
    kPortMenuTileVert,
    kPortMenuArrangeIcons,
};

// kPortMenuNone for every id the frame already dispatches under its canonical id.
PortMenuAction PortMenuActionForCommandId(int id);
