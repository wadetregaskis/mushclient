// wx_activity.h — the Activity window (Game ▸ Activity List) on the port.
//
// The canonical CActivityView is an MFC CListView hosted in an MDI doc/view
// (CMultiDocTemplate), a framework the port does not run — world windows are
// the wx CWorldChildFrame instead.  So, like the other views, the activity list
// is a wx window here: a standalone frame with a wxListCtrl summarising every
// open world (status / line counts / connection duration), refreshed on a timer.
#pragma once

// Create the activity window, or raise it if already open (singleton).
void ShowActivityWindow();

// Is it open?  CMainFrame::OnUpdateDisplayActivitylist (mainfrm.cpp:1164) ticks
// Game ▸ Activity List from `App.m_pActivityView != NULL`; the port has no
// CActivityView, so the menu bar's update-UI arm asks this instead.  The window
// is a singleton, and on Windows the tick is the only sign it is already open —
// choosing the item again merely re-activates it.
bool ActivityWindowIsOpen();
