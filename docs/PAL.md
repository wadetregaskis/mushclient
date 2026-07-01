# MUSHclient Platform Abstraction Layer (PAL) — spec

**Status:** draft / living document. Design agreed; implementation just beginning
(`IWorldView` first). Nothing here is frozen.

## Goal

Restructure MUSHclient into a **platform-agnostic core** + **thin, swappable
platform adapters**, so that:

- The core (engine, protocol, data model, world/scripting API) has **no** OS or
  GUI dependency and is unit-testable to ~100% without a window server.
- Each platform is *one adapter* behind a small, stable interface set (this PAL).
  Today: native Win32/MFC on Windows, wxWidgets on macOS/Linux. Tomorrow, without
  touching the core: SwiftUI/AppKit, Qt, or a TUI.
- **Windows behaviour is preserved exactly.** Windows is not special-cased; it is
  simply another adapter. The core is as ignorant of Win32 as it is of wx.

The one non-negotiable invariant that makes this real:

> **Quarantine.** Nothing above an adapter may name a toolkit type. No
> `#include <wx/…>`, no `HWND`/`CWnd`/`MFC`, no `NSView` — anywhere but that
> platform's adapter TUs. Enforced structurally: the **core library target does
> not get wx / Win32 / AppKit on its include path**, so a leak fails to compile.

We preserve *behaviour*, not byte-for-byte Windows source. Genuine Windows bugs
may be fixed; correct behaviour must be reproduced exactly.

## Layering (dependencies point inward)

```
frontends:   native Win32/MFC  |  wx (mac/Linux)  |  headless test fakes  |  (future: SwiftUI, TUI)
             ────────────────────────── implement ──────────────────────────
PAL (L2):    IWorldView · ISurface · ITransport · IClock/ITimers · ISound
             · IHostUi · IAppShell · IFileSystem                (POD/std types only)
             ──────────────────────────── used by ────────────────────────────
engine (L1): WorldEngine + WorldModel — protocol parse, triggers/aliases/timers,
             the ~400-method world API + VARIANT dispatch, settings + validation,
             plugin lifecycle, the world.Window* bodies, styled-line layout
foundation (L0): string · geometry · collections · IO/serialize · regexp · hashing · datetime
```

Only frontends know a toolkit. Everything below the PAL is pure and testable.

## Types that cross the seam

POD / std only. No toolkit types. Scripting stays on the existing `VARIANT`
boundary; the styled line is `SOutputLine`/`SOutputRun` (`platform/output_types.h`
— to be promoted to a cross-platform canonical type).

```cpp
namespace pal {
  struct Point { int x, y; };
  struct Size  { int w, h; };
  struct Rect  { int x, y, w, h; };
  struct Range { int start, end; };          // e.g. a text selection
  using  Colour = uint32_t;                  // 0x00BBGGRR (matches COLORREF)
  using  Bytes  = /* span<const byte> */;
  enum class SurfaceId : uint32_t {};
  enum class ImageId   : uint32_t {};
  enum class FontId    : uint32_t {};
  enum class TimerId   : uint64_t {};
}
```

## The interfaces (8)

Signatures below are indicative; the authoritative form lives in `pal/*.h` as
each is implemented.

1. **`IWorldView`** — the per-world window (output + input + status). The cleaned
   form of the ~39 `m_fn_*` callbacks already in `doc_stub.h` (a de-facto,
   untyped version of this interface is *already injected* by the headless
   build). Core→frontend pushes + a few pulls (`currentCommand`, `outputSelection`,
   `scrollPos`). See `pal/iworldview.h`.
2. **`ISurface`** — the 2-D raster surface shared by miniwindows *and* the output
   pane. ~20 genuine raster primitives (fill/stroke/line/ellipse/arc/polygon/
   gradient, text + `measureText`, fonts, image load/draw/snapshot, `lockPixels`).
   The `world.Window*` *bodies* (hotspots, z-order, filters via `lockPixels`) stay
   in the core.
3. **`ITransport`** — async byte IO; the adapter owns sockets, TLS and DNS so the
   core sees only plaintext bytes + events.
4. **`IClock` + `ITimers`** — monotonic + wall-clock time; schedule/cancel.
   Fakeable virtual time → deterministic timer tests.
5. **`ISound`** — load/play/stop/status, volume/pan.
6. **`IHostUi`** — native modal UI + clipboard: messageBox, file/folder/font/
   colour/input pickers, popup menu, `editSettings(model)`. The core owns the
   settings *model* + validation; the frontend renders the dialog (on Windows,
   the real MFC property sheet — unchanged).
7. **`IAppShell`** — window/notepad lifecycle, main frame, flash/quit,
   openExternal, and small system queries (`sysColour`, `systemMetric`, `keyState`).
8. **`IFileSystem`** — `settingsDir()`, `dataDir()`, plus the few real file reads
   not already covered by portable `CFile`.

## Explicitly out of the PAL

- The ~80 widget-level shim hooks (`combo_*`, `listbox_*`, `listctrl_*`,
  `slider_*`, `spin_*`, `edit_*`, `button_*`, `get_dlg_item`, …) — these exist only
  to drive faked MFC controls in today's dialog bridge and become frontend-internal.
- Hotspots — core-side metadata + generic mouse events, not a PAL op.
- Image filters/transforms — core math over `ISurface::lockPixels` (also makes
  them unit-testable).
- Printing (`genprint`) — add `IPrinter` when we reach it, not speculatively.
- The 5 scripting interpreters — portable libraries behind `IScriptEngine`, not a
  platform-GUI concern.

## Migration (incremental, always-green; Windows never perturbed)

- **P0** guardrails: coverage + golden characterization tests capturing the
  *shipping* output/behaviour before any move.
- **P1** kill the `*_portable` shadow: single-source the ~30 dual doc methods and
  the second Lua API so the core has one body per method (the tests then guard the
  shipped path).
- **P2** formalise the seam: promote the `m_fn_*` bundle into `IWorldView` (and the
  rest of the PAL); every frontend implements every method (no silent nulls).
- **P3** value types out of the shim into L0.
- **P4** demote `CMUSHclientDoc` → `WorldEngine` + `WorldModel` behind the PAL.
- **P5** unify the build into one core lib + per-platform adapters; delete the stubs.
- **P6** backfill tests to ~100%; coverage ratchet in CI.

## Open questions

- `editSettings(model)`: Windows runs the real property sheet; mac/wx renders from
  the model. (Leaning: yes.)
- `ISurface` handles: opaque ints owned by the frontend. (Leaning: yes.)
- `SOutputLine` currently `#ifndef _WIN32`; promote to the canonical cross-platform
  line type as part of P4.
