# MUSHclient Platform Abstraction Layer (PAL) — spec

**Status:** living document.  Three of the eight interfaces are LIVE on every
platform: `IWorldView` (typed-only, P2 complete; **unconditional since P4** —
SOutputLine promoted, so MSVC compiles the seam too, though no native Windows
adapter emits through it yet), `ISurface` (S1–S4 complete incl. the native GDI
adapter), and `ITransport` (T1–T5 complete — CWorldSocket is gone; Windows,
macOS and the headless engine all connect through the seam).  Every seam the
engine calls is now compiled on every platform.  Windows-side correctness of
the native adapters is by-reasoning until Windows CI exists.

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
2. **`ISurface`** — the 2-D raster surface behind miniwindows (~20 primitives:
   shapes/lines/gradient/flood/invert, text + measure + metrics, fonts, blit with
   colour-key, region pixel I/O). The output pane's *text* is **not** behind it
   (both views rasterize styled lines themselves; the engine ships them via
   `IWorldView`). Full evidence-grounded design: *ISurface — design* below.
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
- Image filters/transforms — core math over `ISurface` region pixel I/O (also
  makes them unit-testable).
- Printing (`genprint`) — add `IPrinter` when we reach it, not speculatively.
- The 5 scripting interpreters — portable libraries behind `IScriptEngine`, not a
  platform-GUI concern.

## ISurface — design

Derived from a full inventory of the three real clients: `miniwindow.cpp` (the
~30 `world.Window*` methods and their GDI + raw-DIB usage), `platform/
wx_gdi_compat.cpp` (the wx shim — ground truth for the GDI surface MUSHclient
*actually exercises*, and for what ships as a no-op with no visible loss), and
the two paint paths (`mushview.cpp` `OnDraw` / `platform/wx_outputpane.cpp` +
`wx_world_child.cpp` composite). Workload note: in the bundled mapper + Quow's
plugins, text draw/measure dominates by an order of magnitude, then
rect/line/gradient/circle/polygon; the blend/filter machinery is
correctness-critical but cold.

### Scope

An `ISurface` adapter owns **per-miniwindow offscreen raster surfaces** and
**fonts**. That is all.

- **Output-pane text is out.** The engine ships styled lines through
  `IWorldView::appendLine`; each view rasterizes them itself (the mac pane
  deliberately places glyphs on its own integer grid — metrics and layout must
  share one frontend-local source of truth).
- **Images are out** as seam objects. PNG/BMP decode is already portable
  (libpng, in-tree); a loaded image is an engine-owned pixel buffer (POD), a
  snapshot of another window is `readPixels`. Images reach the adapter only as
  the source argument of `blit`/`writePixels`. No `ImageId`.
- **Hotspots are out** (pure engine-side geometry — `PtInRect` over stored
  rects; never touches pixels).
- **Composition is a separate, tiny contract** (below), not `ISurface` ops.

### The contract

POD argument types (in `pal_types.h` as needed):

- `PixelBuffer {bytes, stride, Size, format}` — **exactly the Windows DIB
  contract**: format `BGR24` or `BGRA32`, rows 4-byte aligned, **bottom-up**.
  Deliberate: the ~1500 lines of existing blend/filter/merge loops
  (miniwindow.cpp 2072–3186, 3487–3577) index buffers assuming this layout and
  must run byte-identical on Windows at S5; a saner top-down/RGBA layout is a
  possible v2 *after* Windows CI can validate loop rewrites.
- `PenSpec {style, width, colour, endcap/join}` — solid/null/inside-frame any
  width; dash variants width ≤ 1.
- `BrushSpec {style 0–12, fg, bg, optional PixelBuffer pattern}` — the pattern
  tiles from the surface origin and may be **any size** (`WindowImageOp` builds
  pattern brushes from arbitrary loaded images); the 5 built-in 8×8 mono
  patterns and the 6 hatches may be expanded engine-side into pattern pixels,
  using BGRA32 with alpha 0 for transparent gaps (`BrushColour == -1`) — the
  one sanctioned use of alpha in an *input* buffer.
- `FontSpec {face, pixel height, weight, italic/underline/strikeout, charset
  hint, pitch/family hint}` — both hints pass through on Windows (GDI font
  mapping uses them); other adapters may ignore them.
- `FontMetrics` — the ~20 `TEXTMETRIC`-ish fields `WindowFontInfo` exposes,
  plus the **realized face name** (`GetTextFace` after platform substitution —
  `WindowFontInfo 21`). Fidelity is tiered: height, ascent, descent, internal/
  external leading and average/max char width MUST be real and consistent with
  rendering (plugins compute layout from them); the remaining fields are
  best-effort, documented per adapter.
- `Matrix2x3`.

```cpp
// lifecycle
SurfaceId createSurface(Size, Colour background);      // filled, TA_LEFT|TA_TOP baked in
void      destroySurface(SurfaceId);
// region pixel I/O — the direct-access path (see rationale)
void      readPixels (SurfaceId, Rect, PixelBuffer& out);
void      writePixels(SurfaceId, Rect, const PixelBuffer&);
Colour    getPixel   (SurfaceId, Point);                // -1 out of bounds (CLR_INVALID)
// drawing — stateless; stroke AND fill in one call where GDI does
void fillRect   (SurfaceId, Rect, BrushSpec);           // + FillSolidRect
void invertRect (SurfaceId, Rect);                      // dest-NOT; the only ROP kept
void drawEdge   (SurfaceId, Rect, edge, flags);         // EDGE_*/BF_* system bevels only
                 // (Draw3dRect takes two arbitrary colours — decomposed core-side)
void shape      (SurfaceId, kind, Rect, Point a, Point b, PenSpec, BrushSpec);
                 // kind: rect | roundRect(a = ellipse w/h) | ellipse | chord | pie.
                 // chord/pie: a and b define rays from the ellipse centre; the arc
                 // runs counter-clockwise from the a-ray to the b-ray (GDI default);
                 // degenerate rays follow GDI behaviour.
void line       (SurfaceId, Point, Point, PenSpec);     // GDI last-pixel rule noted below
void polyline   (SurfaceId, span<Point>, PenSpec);
void polygon    (SurfaceId, span<Point>, PenSpec, BrushSpec, fillMode /* alternate */);
void bezier     (SurfaceId, span<Point>, PenSpec);
void arc        (SurfaceId, Rect, Point, Point, PenSpec); // radial convention as chord/pie
void floodFill  (SurfaceId, Point seed, Colour fill, Colour ref, mode);
                 // border:  fill until hitting ref-coloured boundary (GDI FloodFill)
                 // surface: fill while on ref colour (ExtFloodFill FLOODFILLSURFACE)
                 // fill = the brush colour GDI takes from the selected brush (Colour2)
void gradient   (SurfaceId, Rect, Colour from, Colour to, bool horizontal);
// blit — source is another surface or an engine-owned buffer
void blit(SurfaceId dst, Rect dstRect, SurfaceOrPixels src, Rect srcRect,
          BlitMode mode /* copy | transparentKey(Colour) — EXACT match */,
          Quality /* fast | smooth(HALFTONE) */, const Matrix2x3* = nullptr);
          // matrix non-null ⇒ dstRect is ignored: device coords come entirely from
          // M·src (GDI GM_ADVANCED convention — WindowTransformImage passes its
          // placement inside the matrix); mode/quality still apply.
// text — UTF-8 in; draw and measure MUST use identical metrics.
// Background is always transparent (WindowText hard-sets TRANSPARENT,
// miniwindow.cpp:850) — no BkMode at the seam.
FontId      createFont(FontSpec);   void destroyFont(FontId);
int         drawText   (SurfaceId, FontId, string_view utf8, Point,
                        Colour, const Rect* clip);       // → width drawn
Size        measureText(FontId, string_view utf8);      // hot (mapper calls per label)
FontMetrics fontMetrics(FontId);
int         dpi();                                       // for points→pixels (see decisions)
```

~22 ops. Everything else the `world.Window*` API does stays in the core:
`FixRight`/`FixBottom` (≤0 = offset-from-edge) translation, font/image
name-caches, tiling (a loop of blits), resize (create + blit + destroy),
`Draw3dRect` (two arbitrary-colour bevel edges → four 1-px `fillRect`s), the
per-pixel blend/filter/merge/gradient-texture math (now over
`readPixels`/`writePixels`, hence unit-testable), pattern definitions
(mono/hatch → pattern pixels), PNG/BMP codecs and save/write paths (whole-
surface `readPixels` as BGR24 bottom-up is exactly what `WindowWrite`/
`WritePng` need).

### Rationale for the load-bearing choices

- **Region pixel I/O, not `lockPixels`.** ~12 `CreateDIBSection` sites feed
  already-portable per-pixel loops. On Windows those loops *already* round-trip
  `BitBlt(surface → temp DIB) → mutate → BitBlt back`; because `PixelBuffer`
  preserves the DIB layout **exactly** (BGR24/BGRA32, bottom-up, padded rows),
  `readPixels`/`writePixels` *is* that round-trip and the loop bodies move over
  **unmodified** — no re-indexing, so S5 runs byte-identical math on Windows.
  Implementable by all three adapters (GDI DIB blit / wx row-copy / headless
  memcpy), and strictly better than the wx shim's current per-pixel `wxImage`
  sync loops. Zero-copy `lockPixels` remains a compatible v2 optimization.
  **Never** a per-pixel virtual call: the seam is region-granular.
- **Stateless ops.** The GDI code uses select-use-restore everywhere and has
  documented state leaks (`FillSolidRect` clobbers the DC bk colour;
  `SetTextColor`/`SetBkColor` triple-duty for hatch gaps and mask blits). Pens/
  brushes/fonts as per-call arguments eliminate that class of bug at the seam.
- **Combined stroke+fill** with the full pen/brush zoo: pen styles solid/null/
  inside-frame any width, dash variants width ≤ 1, endcap/join variants; brush
  styles 0–12 (solid, null, 6 hatches, 5 built-in 8×8 mono patterns,
  pattern-from-image). Fill-only or stroke-only APIs break
  `Rectangle`/`Ellipse`/`RoundRect`/`Chord`/`Pie`/`Polygon` parity.
- **Exact colour-key blit as a primitive** replaces the
  `SRCINVERT`/`SRCAND`/`SRCINVERT` mono-mask ROP dance (used by image draw,
  transform-image, and the per-paint transparent-window composite). Exact match
  only — no anti-aliased or alpha-blended edges — and composable with the 2×3
  affine (`WindowTransformImage` wraps the whole keyed sequence in
  `SetWorldTransform`). `invertRect` is the only other ROP anything needs.
- **Alpha is pinned OFF at the surface.** The backing store is opaque-24bpp
  *semantics* regardless of depth; per-pixel alpha exists only in loaded 32bpp
  images and is applied by engine CPU math. Adapters must not let backend alpha
  leak into readback.
- **Text round-trips.** `drawText` returns min(measured, clip) width as
  `WindowText` does; `measureText` must agree with `drawText` and be cheap;
  `fontMetrics` must be real (plugins compute layout from it). Input is
  validated UTF-8 (+ charset hint for legacy non-Unicode text). Per-glyph dx
  arrays and rotated text are provably unused — excluded.
- **Headless-implementable.** Toolkit-free CPU rasterization of this contract
  is straightforward (and is the test adapter). That keeps the wx adapter
  honest, unlocks golden-image tests of the `world.Window*` bodies, and leaves
  Phase-4 miniwindow parity for `mushcore` open rather than foreclosed.
- **Proven-dead surface excluded** (the shim ships these as no-ops with no
  visible loss): regions, palettes, map modes/viewport origins, world transforms
  beyond the blit matrix, `MaskBlt`, `ScrollDC`, `PatBlt` beyond invert,
  dx-array/tabbed/gray text, `SetTextCharacterExtra`, rotated fonts.

### The composition contract (engine ↔ view, not ISurface)

Per paint, both frontends iterate the doc's z-ordered miniwindow list twice
(underneath → text → on-top). The engine owns placement *spec* (position mode
0–13, location, flags incl. draw-underneath / ignore-mouse / transparent +
key colour, z-order — a sorted engine-side vector); the **view** computes the
landed rect from its own geometry and **writes back** `m_rect` +
`m_bTemporarilyHide`, which engine APIs (`WindowInfo`, hotspot hit-testing at
reverse z-order) then read. Formalised as:
`compositionList() → span<SurfacePlacement>` (pull, per paint) where
`SurfacePlacement {SurfaceId, name, Size, mode, Point location, flags, Colour
key}` in z order, + `placementResult(name, Rect, bool hidden)` (push back).
**Pixels for the composite:** the compositing view and the `ISurface` adapter
are the *same* quarantined frontend layer, so the view resolves each
`SurfaceId` to its own backing store zero-copy (today's `WxDCFromCDC`
equivalent) — no PAL op needed, and the hot path stays copy-free; a split
design (e.g. the headless adapter under test) may fall back to `readPixels`.
Mouse routing goes engine-side over that data: view sends mouse events with
coordinates (capturing from button-down to button-up, as `SetCapture` does
today so drags keep receiving moves outside the window); engine hit-tests
(reverse z, click-through flags) and returns `{consumed, cursorId, tooltip}` —
which also fixes a live parity gap: the mac view ignores `CHotspot::m_Cursor`
today (Windows maps −1..12 to system cursors). Same contract carries the doc
background/foreground images (currently Windows-only paint — a known gap)
if/when the mac view adopts them.

### Decisions taken + open decisions

- **Taken:** exclusive right/bottom rects at the seam; `line`'s **normative**
  behaviour is GDI's last-pixel-exclusive rule (the Windows adapter conforms by
  construction; the wx adapter's inclusive rendering is a *documented, accepted
  deviation* that already ships — not conformance, and fixable independently);
  `shape` with a null pen draws the figure one pixel smaller on right/bottom
  (GDI's documented null-pen shrink — normative, pinned by the golden tests);
  a partially-out-of-bounds blit source draws the in-bounds intersection at
  the correspondingly shifted destination (BitBlt clipping); adapters MUST
  reject a `PixelBuffer` whose declared geometry is not backed by its bytes
  (`PixelBuffer::valid()`);
  surface has no alpha; images are engine-owned buffers; region I/O over lock;
  `FontId`/`SurfaceId` opaque, adapter-owned.
- **Decided — DPI: Mac-native typographic sizing (`dpi()` = the shim's
  `GetDeviceCaps(LOGPIXELSY)` = `wxGetDisplayPPI`; the status quo).** On a 1x
  display that is ~72; on a 2x Retina Mac it is 144 (a point stays a point — the
  extra pixels are the backing-store resolution). Either way the adapter reads
  the *same* value the product already uses, so text size is unchanged from what
  ships. Owner rationale: a user's 12 pt is 12 pt — text in MUSHclient should
  render the same size as 12 pt in any other Mac app.
  Cross-platform ramification, accepted: pixel-layout plugins authored on
  Windows show text ~25 % smaller relative to their fixed-pixel boxes
  (metric-driven plugins self-correct at any DPI). The safety asymmetry
  supports this: smaller-than-authored text always *fits* (cosmetic), while
  pin-96 would grow all plugin text ~⅓ and can *clip/overflow* pixel-tuned
  layouts (functional). If under-sizing ever genuinely bites, the escape hatch
  is an opt-in "Windows plugin font parity" preference (per-world, additive),
  not a default flip. Windows keeps reporting real GDI DPI (96), unchanged.
- **Decided — legacy charsets: the seam stays UTF-8 + charset hint; the wx
  adapter decodes.** When non-Unicode `WindowText` bytes are not valid UTF-8,
  the wx adapter converts them using the font's charset hint via the *existing*
  machinery already proven in the output pane (`WinCharsetToEncoding` +
  `wxCSConv`, `wx_outputpane.cpp:43-90`) — mirroring GDI's "font charset
  interprets the bytes" rule. Fix-forward (today: mojibake); the CPU adapter
  ignores the hint; engine-side decode remains an option if a toolkit-free
  path ever needs it. Windows native path untouched.
- **Decided — `ON_PLUGIN_DRAW_OUTPUT_WINDOW`: fire at the same point as
  Windows** (between the underneath-miniwindow composite and the text pass;
  args: 1-based first visible line, pixel scroll offset). No design reason
  against it; lands with S4. Two engineering notes: (1) argument fidelity —
  Windows passes *doc* line indices, and the mac pane keeps its own projected
  line list, so the pane→doc index mapping must be pinned (trim drift);
  (2) re-entrancy — the callback runs script mid-paint, so line-appends /
  repaints triggered inside it must be deferred until the paint completes
  (Windows relies on the documented draw-only discipline; we should guard).
  No bundled or Quow plugin calls it today, so priority is parity, not urgency.

### ISurface migration (S-steps, each green + independently reviewable)

- **S1** `pal/isurface.h` + PODs; headless CPU-raster adapter (the test/reference
  implementation); golden raster tests (draw → `readPixels` → compare).
- **S2** wx adapter implementing `ISurface` over the existing shim machinery
  (wrap, don't rewrite) — the canonical pixel buffer kills the DIB↔bitmap sync tax.
- **S3** reroute `CMiniWindow` op-family by op-family (rects → text → shapes →
  blits → pixel ops) onto `ISurface`, live-verifying Quow's minimap + the mapper
  after each family; the temp-DIB round-trips collapse onto `readPixels`/
  `writePixels`. **The seam call is UNCONDITIONAL** — no `#ifdef`, no runtime
  `if(surface)` fork (owner decision: a runtime check is no better than an
  `#ifdef`). `miniwindow.cpp` deletes the old `pdc` path as each family moves, so
  it keeps exactly one drawing path on every platform from S3 onward.
- **S5 is folded into S3, not deferred.** Because the seam call is unconditional,
  Windows draws through `ISurface` too — so each S3 family lands its **native GDI
  adapter method** (`pal/gdi_surface.{h,cpp}`, in `MUSHclient.vcxproj`) in the
  same step. Each op emits the *exact* MFC/GDI call the original `CMiniWindow`
  code used, so Windows is behaviour-identical **by construction** — the only
  safety net for the MSVC build, which can't be compiled/tested in the dev
  environment. The mac/Linux port (same unconditional source via the wx adapter)
  is the live proxy: build + `--pal-surface-selftest` + `ctest` gate each commit.
  `InstallGdiSurfaceBridge()` runs at `InitInstance` (Windows) mirroring
  `InstallWxSurfaceBridge()` (port), so `pal::g_surface` is always installed.
- **S4** formalise the composition contract (placement list, rect write-back,
  engine-side hit-testing with cursor ids — fixes the cursor parity gap).
  **S4a landed:** placement is engine-owned — `CalculateMiniWindowRectangles`
  (miniwindow_compose.cpp; the old `CMUSHView::Calculate_MiniWindow_Rectangles`
  moved verbatim) is called by both views with their own geometry; the wx
  view also applies Blit_Bitmap's post-paint `m_rect` mutation so
  `WindowInfo 12/13` return the Windows convention ({left, top, width,
  height}), and its hotspot hit-test reads left/top + size (robust to the
  convention; a documented port-only improvement over Windows' PtInRect,
  which cannot match a window positioned beyond its own size).
  **S4b1 landed:** the hotspot hit-test is engine-owned too
  (`MouseOverMiniWindow`, same TU; the Windows member forwards; the wx
  frame calls it) — with one flagged, deliberate strict extension: the
  window bounds test reads left/top + size, so hotspots also work for
  windows positioned beyond their own size (upstream's PtInRect over the
  mutated post-paint rect could never match there). And the **cursor parity
  gap is fixed**: the wx pane maps `CHotspot::m_Cursor` (−1..12) to wx
  stock cursors exactly as `CMUSHView::OnSetCursor` maps to Win32 ones.
  **S4b2 landed:** the whole stateful mouse family
  (`Send_Mouse_Event_To_Plugin`, `Mouse_Move/Down/Up/Wheel_MiniWindow`)
  moved verbatim to the same TU; view-specific touches (tooltips,
  mouse-leave arming, capture, keyboard modifiers) go through a small
  `MiniWindowMouseHooks` interface each frontend implements;
  `m_sPreviousMiniWindow` is doc-homed (one view per doc — identical
  behaviour). The wx frame's ~230-line mouse mirror is deleted; the port
  gained real drag capture, Windows' exact wheel flag packing, and
  wheel-consumption over any miniwindow.
  **S4c landed:** the wx pane fires `ON_PLUGIN_DRAW_OUTPUT_WINDOW` between
  the underneath composite and the text pass with Windows' exact arguments
  (1-based first visible doc line + pixel scroll offset; pane→doc line
  mapping via the tail-aligned counts), guarded against mid-paint script
  re-entrancy (deferred appends). **S4 is complete.**

Status: **S1** (CPU reference), **S2** (wx adapter), and **S3a–S3c** are landed on
all platforms — the whole **rect + text/font + shape/pen** drawing surface now
routes through `ISurface`: `WindowRectOp`; `WindowText`/`WindowFont`/`WindowTextWidth`/
`WindowFontInfo` (fonts owned by the adapter, keyed by `FontId`); `WindowLine`/
`WindowArc`/`WindowBezier`/`WindowCircleOp`/`WindowPolygon` with the pen model (the
engine decodes the Win32 pen/brush params into neutral `PenSpec`/`BrushSpec`; each
adapter re-encodes, the GDI adapter reproducing `MakeAPen`/`CreatePatternBrush`
exactly), including the six hatches and MUSHclient's five built-in mono pattern
brushes (carried as `BrushSpec.monoPattern` so each adapter builds a native mono
brush and GDI applies its own text/bk mapping — byte-exact without guessing).
`WindowGradient` is also landed (all three modes: linear H/V via `gradient()`,
and the texture fill via `MakeTexture` → `writePixels()`). **S3d completed the
rest — every CMiniWindow drawing/image op now routes through the seam**:
`WindowGetPixel`/`WindowSetPixel`; `WindowDrawImage` (the temp-adopt trick:
the engine-owned image `CBitmap` is wrapped as a source surface at draw time —
no storage refactor); `WindowFilter` (`readPixels` → the 27 unchanged filter
loops → `writePixels`); `WindowDrawImageAlpha`/`WindowGetImageAlpha` via
`PixelFormat::BGRA32_Alpha` — an alpha-preserving read reproducing Windows
BitBlt-into-32bpp-DIB semantics (the wx adapter reads the selected bitmap's
`wxImage` directly since `wxDC::Blit` composites on wxOSX; reads past the
image edge zero-fill engine-side exactly like BitBlt); `MergeImageAlpha` and
`BlendImage` (BGR24 reads + the untouched merge/blend-mode loops);
`ImageFromWindow` (`readPixels` BGRA32 → a 32bpp DIBSection via the loaders'
CreateDIBSection+Attach pattern); `WindowImageOp` (image-pattern brushes:
colour images carried as `BrushSpec.pattern` pixels, CreateImage mono bitmaps
kept REAL mono via a `MonoImageMap` stash so GDI's text/bk colouring applies);
`Write` (save-window read); and `TransformImage` (the `Matrix2x3` blit — GDI
runs the original `SetWorldTransform` sequence verbatim, wx/CPU inverse-map).
`pdc` survives only as the adopted backing store (create/resize/destroy).
GDI-adapter stubs left: `createSurface` and the engine-unused buffer-source
`blit` overload only.

## ITransport — design

Derived from a full survey of the real connection stack (2026-07-12): the shared
`CWorldSocket` (worldsock.cpp — compiled into BOTH builds), the doc-level
lifecycle in doc.cpp (ConnectSocket/InitiateConnection/OnConnect/
ConnectionEstablished/ReceiveMsg/SendPacket/StartSSLHandshake/
ContinueSSLHandshake/LookupHostName/OnConnectionDisconnect), their hand-written
doc_portable.cpp twins, the POSIX `CAsyncSocket` shim (platform/posix_socket.h —
a select(2) pump driven by a 20 ms wx timer; generation-tagged registry against
callback self-deletion), and the OpenSSL usage.

### What the survey established

- **TLS is split-brain today.** The handshake and `SSL_read` are DOC-level in
  BOTH builds (doc.cpp:6758/6838/1019; doc_portable.cpp:1484/1532/1198); only
  `SSL_write` differs — socket-level with WANT_* retry in the full build
  (worldsock.cpp:105-119) vs inline in the portable `SendPacket` with the
  return value ignored and **no retry** (doc_portable.cpp:560-563 — a real
  latent bug the seam fixes by construction). `SSL*`/`SSL_CTX*` are doc
  members; a fresh per-connection CTX is built each handshake; the fd is
  reached through the shim (`SSL_set_fd(m_pSSL, m_pSocket->m_hSocket)`).
- **The proxy (SOCKS4/5) sits above the byte transport and below TLS**: all
  proxy bytes ride the normal buffered `SendPacket` path in cleartext, parsed
  by the phase machine in doc_shared.cpp; TLS starts only after the tunnel is
  up (doc_shared.cpp:414-417). Proxy logic is engine state, NOT a transport
  concern — the transport is proxy-unaware except for which address it dials.
- **The engine requires exactly these operations** of the socket layer:
  create+options (keepalives, Nagle-off), async resolve with cancellation and
  a staleness guard, async connect, buffered append-and-it-drains write, read
  on readiness, close notification (peer and locally synthesized), graceful
  shutdown, TLS session (start/step/verify/read/write/pending-drain/clean-
  shutdown/metadata), normalized error codes + strings.
- **Interaction points back into the engine/GUI**: connect complete/failed,
  bytes ready (with the SSL_pending re-drain), writable, closed (the
  disconnect-UX body in CWorldSocket::OnClose is really ENGINE logic — scripts,
  plugin callbacks, stats Notes), TLS outcome, DNS completion, and the
  cert-decision prompt — which is DEFERRED to the main loop by design
  (`WM_USER_SSL_FALLBACK_PROMPT`); the GUI prompts (allow-unverified /
  fall-back-to-plaintext → mutate policy flags → full reconnect), the headless
  build **fails closed with no prompt**.
- **Callbacks may destroy the socket/session mid-call** (worldsock.cpp:41-48;
  posix_socket.h generation guards). Any seam must keep that survivable.
- **The portable connect/SSL/proxy twins are compiled but unexercised** — no
  test drives a connection (tests hand-set `m_iConnectPhase`); mushcore has no
  pump at all. A fake ITransport makes the whole connect state machine
  testable for the first time.
- Chat (stream + listen/accept) and UDP (datagram listen) are separate
  `CAsyncSocket` consumers, mushapp-only, un-QA'd on the port; hostname chat
  calls are already broken off-Windows (the `WSAAsyncGetHostByName` shim
  returns failure).

### Scope (v1)

One `ITransport` instance per world connection attempt, owning: the fd, the
event source (pump/messages), DNS resolution, and the WHOLE TLS session. The
engine keeps: the connect-phase machine, proxy negotiation, MCCP, telnet,
reconnect policy (`m_bDisconnectOK` + the timer/countdown), auto-login, and all
disconnect UX (scripts/plugins/stats — today buried in CWorldSocket::OnClose,
to be repatriated into the engine).

Out of scope v1 (stay on the CAsyncSocket shim): the chat subsystem's
listen/accept, UDP datagrams, and `UdpSend`'s synchronous raw socket. Add a
listener/datagram surface only if chat is ever brought to parity ("don't
gold-plate").

### The contract (sketch — authoritative form lands in pal/itransport.h)

```cpp
struct TransportOptions { bool keepalives; /* timing fixed: 120s/6s/6 probes */ };
struct TlsParams  { std::string serverName;   // SNI + hostname pinning
                    bool allowInvalidCert; }; // session-only user consent
enum class TlsFailure { CertVerify, Handshake };

class ITransportEvents {           // implemented by the engine (the doc)
  onResolved(token, addr, ok);     // stale tokens dropped by the caller
  onConnect(int err);              // 0 = connected (or immediate success)
  onBytes(const byte* p, size_t n);// DECRYPTED bytes; absorbs the SSL_pending drain
  onWritable();                    // buffered queue drained enough to continue
  onClosed(int err);               // peer close or error; engine runs disconnect UX
  onTlsEstablished(version, cipher);
  onTlsFailed(TlsFailure, reason); // engine decides: GUI defers a prompt to the
};                                 // main loop; headless fails closed

class ITransport {
  resolve(host, token);  cancelResolve();
  connect(addr, port, TransportOptions);
  send(bytes);                     // append-and-it-drains (absorbs m_outstanding_data)
  startTls(TlsParams);             // steps itself on readiness; engine no longer
  close();                         //   sees WANT_READ/WANT_WRITE at all
  lastErrorString();
};
```

Contract rules: events are delivered on the main thread, never re-entrantly;
an event handler may destroy the transport (adapter defers destruction or
generation-guards, as the POSIX pump already does); **no dialogs inside
events** — the cert-decision prompt remains an engine-side deferred main-loop
action that mutates policy and reconnects from scratch (exact current
behaviour, GUI and headless alike).

### Adapters

- **POSIX** (mac/Linux): relocation, not rewrite — posix_socket.h's pump +
  registry, the getaddrinfo thread from wx_world_child.cpp (kills
  `m_fn_lookup_host`, the last loose doc callback), and the OpenSSL calls
  gathered from doc.cpp/worldsock.cpp/doc_portable.cpp into one place
  (per-connection CTX, TLS1.2+, SNI, `SSL_set1_host`, default verify paths).
- **Windows**: the same class shape over `CAsyncSocket`/WSAAsyncSelect +
  `WSAAsyncGetHostByName` + the identical OpenSSL calls (they are already
  platform-neutral apart from the ROOT-store import, which stays in the
  Windows adapter) — behaviour-identical by construction, like GdiSurface.
- **Fake** (tests): scripted byte streams + scripted TLS/connect outcomes →
  the connect/proxy/SSL phase machine becomes unit-testable.

### Migration (T1–T5, each step green)

- **T1 — DONE** (`08ed225`). `pal/itransport.h` + the fake adapter +
  characterization tests of the phase machine (SOCKS4/5 sequences incl. auth,
  TLS success/cert-fail/handshake-fail, clean close, mid-callback destruction).
- **T2 — DONE.** The POSIX adapter and its wiring, in four steps:
  - **T2a** (`fe7aa56`) the disconnect-UX body moved from
    `CWorldSocket::OnClose` into `CMUSHclientDoc::ConnectionClosed`.
  - **T2b** (`366c7c6`/`05fc74b`) `ReceiveMsg` split at the seam →
    `OnBytesReceived` (both builds), the adapter's `onBytes` landing point.
  - **T2c** (`a6f6870`) `platform/posix_transport.{h,cpp}` — the adapter,
    verified UNWIRED by the hidden `--pal-transport-selftest` (13 checks
    incl. real TLS both ways + destroy-inside-onBytes).
  - **T2d** (`5c7bb0e`) the engine WIRED onto it: `CDocTransportEvents`
    bridge, doc gains `m_pTransport`; ConnectSocket / InitiateConnection /
    SendPacket / LookupHostName / StartSSLHandshake / OnConnectionDisconnect
    switch to it on the port; `CWorldSocket` is now Windows-only. DNS (old
    T4) folded in here — `m_fn_lookup_host` deleted, the wx poll timer calls
    `pal::PumpTransportCompletions()`. Live-verified through the real GUI
    (connect / ANSI receive / keyboard send / server-close disconnect).
- **T3 — DONE** (decision: mushcore adopts the REAL POSIX transport).  In
  five steps, each green:
  - **T3a** (`fbea7a2`) substrate: posix_transport.cpp joins mushcore; the
    events bridge + TLS policy + `CreateTransport()` (the single
    adapter-choice point — T5 hooks Windows in here) + the DNS pair move to
    doc_shared.cpp; stub Frame gains no-op PostMessage (headless TLS
    failures keep failing closed — the prompt posts to nobody).
  - **T3b** (`864b4e3`) SendPacket/OnConnect/GetSocketError/
    GetConnectionStatus single-sourced; the portable SSL_write-no-retry bug
    dies; the SOCKS characterization tests capture bytes via a FakeTransport
    on the seam itself.
  - **T3c** (`6c19b8b`) ConnectSocket/OnBytesReceived/OnConnectionDisconnect
    single-sourced; StartSSLHandshake splits (port body shared, native body
    doc.cpp `#ifdef _WIN32` until T5); the remaining portable twins die;
    mushcore is socket-free (worldsock.cpp + m_pSocket removed from it).
  - **T3d** (`abbc8b9`) ConnectionEstablished + OpenLog single-sourced; the
    modal password prompt is full-build-only (headless behaves like a
    cancelled prompt).
  - **T3e** an end-to-end headless test drives the REAL lifecycle over a
    real loopback socket (resolve thread → connect → established → bytes →
    display → send → server close → bookkeeping + teardown) — coverage the
    twins never had.

  NOT single-sourced, deliberately: CheckConnected (3-line portable twin;
  prompt-UX policy, not socket drift), SendMsg/DoSendMsg + the display
  pipeline (the P2/P4 output territory), ReceiveMsg/ContinueSSLHandshake
  (Windows-only mechanics — they die at T5 with CWorldSocket).
- **T4 — DONE** as part of T2d (DNS folded in; `m_fn_lookup_host` deleted).
- **T5 — DONE** (adapter `f26b54c`, flip below).  pal/win_transport.{h,cpp}
  mirrors the live-verified POSIX adapter section for section (review = diff);
  every mechanic is the exact shipping call sequence relocated (CAsyncSocket
  events, SIO_KEEPALIVE_VALS, OnSend buffered send, the CertOpenSystemStore
  TLS body, WSAAsyncGetHostByName into an adapter-owned message-only window).
  The flip then made the transport THE connection path on every platform:
  CreateTransport() forks per platform; the #ifdef _WIN32 socket branches in
  the shared bodies are gone; doc.cpp's ReceiveMsg / ContinueSSLHandshake /
  StartSSLHandshake(Windows) / HostNameResolved are deleted; worldsock.{h,cpp}
  is removed from the repo; m_pSocket / m_hNameLookup / m_pGetHostStruct left
  the doc (the mainfrm WM_USER_HOST_NAME_RESOLVED dispatcher now serves ONLY
  chat sockets, which keep their own async DNS).  Windows correctness is
  by-reasoning until Windows CI exists — the one deliberate behaviour deltas
  there: bytes-out counted once (the upstream double-count died with
  CWorldSocket::OnSend), and a failed lookup *initiation* now reports through
  the ordinary "Unable to resolve host name" path.

Known quirks: `m_nBytesOut` was double-counted on the old socket path
(SendPacket AND the send path both add it); **the port drops the double-count
at T2d** — bytes are counted once at `SendPacket`. Windows keeps the upstream
double-count until T5 unifies it. The engine synthesizing `OnClose(0)` during
teardown is now an explicit `ConnectionClosed` call.

## Migration (incremental, always-green; Windows never perturbed)

- **P0** guardrails: coverage + golden characterization tests capturing the
  *shipping* output/behaviour before any move.
- **P1** kill the `*_portable` shadow: single-source the ~30 dual doc methods and
  the second Lua API so the core has one body per method (the tests then guard the
  shipped path).
- **P2** formalise the seam: promote the `m_fn_*` bundle into `IWorldView` (and the
  rest of the PAL); every frontend implements every method (no silent nulls).
  *COMPLETE (2026-07-12): IWorldView is typed-only end to end on the port.*
  All families are promoted — display/state (lines, note, paused, title, status,
  clear, background colour, max-lines, repaint, batching), the pulls
  (currentCommand/scrollPos/outputSelection/outputAreaChars/outputRect/mainRect/
  infobarSize), the command-input family, the output-pane family, the window
  family, and the infobar family — and every `m_fn_*` fallback is deleted:
  `m_pWorldView` is the only channel (no view → pushes inert, pulls empty/zero;
  headless stdout output still flows via the doc_portable DisplayMsg/Note
  fallbacks, which key off the view pointer). Main-frame ops ride on IWorldView
  deliberately: every engine call site is doc-scoped; re-partition onto
  `IAppShell` later if it gains real non-doc-scoped needs. Clipboard: the
  headless stubs return empty/OK; the real path is the `GetClipboardContents`/
  `putontoclipboard` globals (`IHostUi`, P5).

  *Windows adapter + flips (2026-07, WV-1…WV-5; Windows-validated 2026-07-15):*
  after P4b unguarded the seam, a **native Windows adapter**
  (`pal/win_worldview.cpp`, doc-owned, `CWinWorldView`) landed, then the engine
  sites had their `#ifdef _WIN32` forks deleted **family by family** so the
  seam call is the only path on every platform: title, info bar, command-input,
  window, and output families are all unconditional now (the deleted Windows
  halves live verbatim in the adapter).  Each flip is behaviour-identical by
  construction (helper body == adapter body == the original code); a Windows
  VM instance built + validated the result against MUSHclient 5.06 and its
  fixes are merged.  **The display family (`appendLine`/`note`/batch/repaint)
  is deliberately NOT flipped and stays platform-split**: Windows renders by
  *pulling* finished lines from the doc's `m_LineList` in `CMUSHView::OnDraw`,
  while the port *pushes* completed `SOutputLine`s to its own pane buffer via
  `EmitOutputLine` (in port-only TUs).  That is an inherent pull-vs-push data-
  model difference, not drift — unifying it means a shared output-pane
  rendering path, which is P5 rendering work.  A handful of other sites stay
  guarded as genuine platform-capability differences (multi-view `Which>1`,
  the `SetScroll` scrollbar-visibility flag, `Activate`'s child-frame vs
  main-frame semantics) — documented per-site in the WV commit messages.
- **P3** value types out of the shim into L0.
- **P4** demote `CMUSHclientDoc` → `WorldEngine` + `WorldModel` behind the PAL.
- **P5** unify the build into one core lib + per-platform adapters; delete the stubs.
- **P6** backfill tests to ~100%; coverage ratchet in CI.

## Open questions

- `editSettings(model)`: Windows runs the real property sheet; mac/wx renders from
  the model. (Leaning: yes.)
- `ISurface`: see *Decisions taken + open decisions* in the ISurface design above
  (DPI parity, legacy charsets, `ON_PLUGIN_DRAW_OUTPUT_WINDOW`). Handles question
  resolved: opaque adapter-owned ids.
- `SOutputLine` promoted to a cross-platform canonical type at P4b (2026-07-12);
  `platform/output_types.h` is now unconditional. The remaining P5 rendering
  question is whether Windows' output pane switches from pulling `m_LineList`
  to consuming pushed `SOutputLine`s (which would let the display family flip
  onto the seam like the other five).
