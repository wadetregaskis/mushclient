// pal/surface_bridge.h — the S3-migration bridge between CMiniWindow (engine)
// and a platform pal::ISurface adapter.
//
// During the ISurface migration (docs/PAL.md S3) CMiniWindow still owns its
// offscreen CDC/bitmap; a migrated op family draws through pal::ISurface into
// that SAME context by "adopting" it. The adapter and the adopt entry point
// live in the frontend (mac: wx_surface.cpp); this header stays toolkit-free
// (native context passed as void*) so the engine names only PAL types.
//
// These globals are null unless a frontend installs them, so the un-migrated
// pdc path runs whenever no adapter is present (Windows until the S5 GDI
// adapter, and mushcore/tests). The Windows build never sees this header —
// every use in miniwindow.cpp is #ifndef _WIN32.

#pragma once
#ifndef _WIN32

#include "pal/isurface.h"

namespace pal {

// The platform surface adapter, or null.
extern ISurface* g_surface;

// Wrap an existing native drawing context (a CDC*, as void*) as a surface the
// adapter draws into without owning it; returns SurfaceId::None if unavailable.
extern SurfaceId (*g_adoptSurface)(void* nativeCtx, int w, int h);
extern void      (*g_releaseAdoptedSurface)(SurfaceId);

} // namespace pal

#endif // !_WIN32
