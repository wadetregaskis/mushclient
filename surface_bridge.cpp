// surface_bridge.cpp — default (null) definitions of the pal::ISurface
// migration bridge globals (pal/surface_bridge.h).
//
// Compiled into mushapp on the port AND the MSVC project (MUSHclient.vcxproj).
// The frontend overwrites these pointers at startup: the wx adapter on the port
// (InstallWxSurfaceBridge, wx_surface.cpp), the GDI adapter on Windows
// (InstallGdiSurfaceBridge, gdi_surface.cpp). Toolkit-free — no stdafx.h.

#include "pal/surface_bridge.h"

namespace pal {

ISurface* g_surface = nullptr;
SurfaceId (*g_adoptSurface)(void*, int, int) = nullptr;
void      (*g_releaseAdoptedSurface)(SurfaceId) = nullptr;

} // namespace pal
