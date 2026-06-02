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
