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
- **Decided — DPI: Mac-native typographic sizing (`dpi()` ≈ 72 on the mac
  adapter; the status quo).** Owner rationale: a user's 12 pt is 12 pt — text
  in MUSHclient should render the same size as 12 pt in any other Mac app.
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
- **S3** reroute `CMiniWindow` op-family by op-family (shapes → text → blits →
  pixel ops) onto `ISurface`, live-verifying Quow's minimap + the mapper after
  each family; the temp-DIB round-trips collapse onto `readPixels`/`writePixels`.
- **S4** formalise the composition contract (placement list, rect write-back,
  engine-side hit-testing with cursor ids — fixes the cursor parity gap).
- **S5** Windows native adapter: `ISurface` implemented *by* GDI (DIB-backed,
  behaviour-identical by construction; validated on the owner's Windows CI);
  `miniwindow.cpp` keeps exactly one drawing path.

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
- `ISurface`: see *Decisions taken + open decisions* in the ISurface design above
  (DPI parity, legacy charsets, `ON_PLUGIN_DRAW_OUTPUT_WINDOW`). Handles question
  resolved: opaque adapter-owned ids.
- `SOutputLine` currently `#ifndef _WIN32`; promote to the canonical cross-platform
  line type as part of P4.
