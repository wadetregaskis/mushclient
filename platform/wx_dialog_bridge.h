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
class wxBitmap;

// Load a bundled dialog-art image (Tip bulb, app icon, tray icons) by its Win32
// resource id.  Invalid wxBitmap if unknown/missing.  (Drives both the dialog
// image statics and the macOS Dock icon.)
wxBitmap LoadDialogArt(int resId);

// The process-wide registry of recovered dialog/menu templates (lazy-loaded).
RcResources& MfcDialogResources();

// Populate an arbitrary window (a wxDialog, or a wxTreebook page wxPanel)
// with the template's controls, absolutely positioned.  Sets the target's
// min-client-size to the template size.
void PopulateWindowFromTemplate(wxWindow* target, const RcDialog& t);

// Build a standalone wxDialog (controls absolutely positioned per the
// template) from a recovered template.  The caller owns the returned dialog.
wxDialog* BuildWxDialogFromTemplate(wxWindow* parent, const RcDialog& t);

#endif // !_WIN32
