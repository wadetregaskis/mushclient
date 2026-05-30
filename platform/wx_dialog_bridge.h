// platform/wx_dialog_bridge.h
//
// Declarations shared between wx_dialog_bridge.cpp (wx-only: resource registry
// + wxDialog builder) and wx_mfc_impl.cpp (both-headers: the DoModal/EndDialog
// orchestration that drives the canonical CDialog).  wx-only — Windows uses the
// localisation DLL.

#pragma once

#ifndef _WIN32

#include "wx_dialog_resource.h"

class wxWindow;
class wxDialog;

// The process-wide registry of recovered dialog/menu templates (lazy-loaded).
RcResources& MfcDialogResources();

// Build a wxDialog (controls absolutely positioned per the template) from a
// recovered template.  The caller owns the returned dialog.
wxDialog* BuildWxDialogFromTemplate(wxWindow* parent, const RcDialog& t);

#endif // !_WIN32
