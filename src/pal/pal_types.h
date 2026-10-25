// pal/pal_types.h — plain value types that cross the Platform Abstraction Layer.
//
// The PAL is MUSHclient's boundary between the platform-agnostic core and the
// per-platform frontends (native Win32/MFC, wx, headless test fakes, future
// SwiftUI/TUI). NOTHING here may name a toolkit type — only std / POD — so the
// same signatures compile against every frontend. See docs/PAL.md.

#pragma once

#include <cstdint>

namespace pal {

struct Point { int x = 0, y = 0; };
struct Size  { int w = 0, h = 0; };
struct Rect  { int x = 0, y = 0, w = 0, h = 0; };
struct Range { int start = 0, end = 0; };   // half-open [start,end); e.g. a selection

// An output-pane text selection as line/column endpoints (1-based; 0 = none),
// matching MUSHclient's GetSelectionStart/EndLine/Column model.
struct TextRange { int startLine = 0, startCol = 0, endLine = 0, endCol = 0; };

// A colour in the same packing MUSHclient uses internally (0x00BBGGRR, i.e.
// COLORREF). Kept as a plain integer so no GDI/wx colour type crosses the seam.
using Colour = std::uint32_t;

// Opaque handles minted and owned by the frontend; the core only stores and
// passes them back. Strongly typed so they can't be confused with each other.
// (No ImageId: images are engine-owned pixel buffers, not seam objects — see
// the ISurface design in docs/PAL.md.)
enum class SurfaceId : std::uint32_t { None = 0 };
enum class FontId    : std::uint32_t { None = 0 };
enum class TimerId   : std::uint64_t { None = 0 };

} // namespace pal
