// pal/surface_bridge.h — the S3-migration bridge between CMiniWindow (engine)
// and a platform pal::ISurface adapter.
//
// During the ISurface migration (docs/PAL.md S3) CMiniWindow still owns its
// offscreen CDC/bitmap; a migrated op family draws through pal::ISurface into
// that SAME context by "adopting" it. The adapter and the adopt entry point
// live in the frontend (mac: wx_surface.cpp); this header stays toolkit-free
// (native context passed as void*) so the engine names only PAL types.
//
// The frontend installs these at startup: the wx adapter on the port
// (InstallWxSurfaceBridge), the GDI adapter on Windows (InstallGdiSurfaceBridge).
// Every platform installs one, so CMiniWindow can call the PAL unconditionally.
// This header is toolkit-free (native context passed as void*), so it compiles
// in every build — the CMake port AND the MSVC project.

#pragma once

#include "pal/isurface.h"

namespace pal {

// The platform surface adapter (installed at startup on every platform).
extern ISurface* g_surface;

// Wrap an existing native drawing context (a CDC*, as void*) as a surface the
// adapter draws into without owning it; returns SurfaceId::None if unavailable.
extern SurfaceId (*g_adoptSurface)(void* nativeCtx, int w, int h);
extern void      (*g_releaseAdoptedSurface)(SurfaceId);

// WindowText / WindowTextWidth with Unicode=false: the bytes are in the font's
// charset, whatever they look like.  The seam carries no such flag (the decided
// UTF-8 + charset-hint model), and the GDI adapter otherwise PROBES for UTF-8 --
// which a short GBK or Big5 string often passes (bytes C3 A8 are a hanzi there
// and e-grave in UTF-8), so it drew the wrong characters where MUSHclient 5
// drew the right ones.  So the engine says so, for the length of one call, and
// the GDI adapter honours it; the others decode by the charset hint anyway.
extern bool g_legacyText;
struct LegacyTextScope
  {
  explicit LegacyTextScope (bool legacy) : m_old (g_legacyText) { g_legacyText = legacy; }
  ~LegacyTextScope () { g_legacyText = m_old; }
  bool m_old;
  };

} // namespace pal
