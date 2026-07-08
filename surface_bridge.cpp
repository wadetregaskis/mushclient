// surface_bridge.cpp — default (null) definitions of the pal::ISurface
// migration bridge globals (pal/surface_bridge.h).
//
// Compiled into mushapp on the port; the wx frontend overwrites these pointers
// at startup (pal::InstallWxSurfaceBridge, wx_surface.cpp). Left null in
// mushcore/tests and any adapter-less build, so CMiniWindow uses its pdc path.

#ifndef _WIN32

#include "pal/surface_bridge.h"

namespace pal {

ISurface* g_surface = nullptr;
SurfaceId (*g_adoptSurface)(void*, int, int) = nullptr;
void      (*g_releaseAdoptedSurface)(SurfaceId) = nullptr;

} // namespace pal

#endif // !_WIN32
