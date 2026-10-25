// platform/wingdi.h — stub for Windows <wingdi.h>
// All GDI types (LOGFONT, TEXTMETRIC, COLORREF, etc.) are provided by
// platform/mfc_ui.h, which is included via stdafx.h → platform/compat.h.
// This file exists only so that #include <wingdi.h> resolves on non-Windows.
#pragma once
#ifndef _WIN32
// Nothing needed here — mfc_ui.h covers all GDI types used by MUSHclient.
#endif
