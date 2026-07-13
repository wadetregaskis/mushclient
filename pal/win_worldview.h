// pal/win_worldview.h — the native Windows adapter for pal::IWorldView
// (design: docs/PAL.md; contract: pal/iworldview.h).
//
// One CWinWorldView per world document.  Relocation, not invention: every
// method body is the exact `#ifdef _WIN32` half of its engine call site —
// the CMUSHView/CSendView view loops, the Frame title/infobar operations,
// the child-frame window management — moved behind the seam so those sites
// can later become unconditional Emit*/Get* helper calls (the family-by-
// family flips, mirroring the ISurface S3 / ITransport T5 migrations).
//
// UNWIRED as landed: nothing sets CMUSHclientDoc::m_pWorldView on Windows
// yet, so no method here is reachable and Windows behaviour is unchanged.
// Wiring + per-family site flips follow once Windows testing can validate
// them.  The display family (appendLine/batch/repaint) flips LAST — Windows
// paints by pull (UpdateAllViews → CMUSHView::OnDraw), and those bodies are
// the faithful invalidation equivalents until that design lands.
//
// CANNOT be compiled or run in the macOS port environment: correctness is
// by-construction/reasoning (each body is the exact original call
// sequence), the pal/gdi_surface.cpp + pal/win_transport.cpp discipline.

#ifdef _WIN32
#pragma once

#include "pal/iworldview.h"

class CMUSHclientDoc;

namespace pal {

// Factory: the concrete type stays private to the adapter TU.  The caller
// owns the returned object (destroy it before the doc dies).
IWorldView* CreateWinWorldView(CMUSHclientDoc* pDoc);

} // namespace pal

#endif // _WIN32
