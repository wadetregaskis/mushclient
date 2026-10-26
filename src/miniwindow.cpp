// MiniWindow.cpp : implementation of the CMiniWindow class
//
// See: http://www.gammon.com.au/mw

#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "mushview.h"
#include "scripting/errors.h"
#include "Color.h"
#include "mainfrm.h"
#include "blending.h"

#include "pal/surface_bridge.h"   // pal::ISurface migration bridge (docs/PAL.md S3)

#define PNG_NO_CONSOLE_IO
#include "png/png.h"

// constructor
CMiniWindow::CMiniWindow ()  :
          m_oldBitmap (NULL),
          m_Bitmap (NULL),
          m_iWidth (0), m_iHeight (0),
          m_iPosition (0), m_iFlags (0),
          m_iBackgroundColour (0), m_bShow (false),
          m_rect (0, 0, 0, 0),
          m_bTemporarilyHide (false),
          m_bLaidOut (false),
          m_last_mouseposition (0, 0),
          m_last_mouse_update (0),
          m_client_mouseposition (0, 0),
          m_FlagsOnMouseDown (0),
          m_ZOrder (0),
          m_bExecutingScript (false)
  {
  pdc = new CDC;
  pdc->CreateCompatibleDC(NULL);
  pdc->SetTextAlign (TA_LEFT | TA_TOP);
  m_tDateInstalled = CTime::GetCurrentTime();  // when miniwindow loaded
  }  // end CMiniWindow::CMiniWindow  (constructor)

CMiniWindow::~CMiniWindow ()  // destructor
  {

  // Release the adopted pal::ISurface before the pdc goes away.
  if (pal::g_releaseAdoptedSurface) pal::g_releaseAdoptedSurface (m_surfaceId);
  m_surfaceId = pal::SurfaceId::None;

  // get rid of old one if any
  if (m_Bitmap)
    {
    pdc->SelectObject(m_oldBitmap);    // swap old one back
    m_Bitmap->DeleteObject ();        // delete the one we made
    delete m_Bitmap;
    }


  // delete our fonts (the CFont is owned by the surface adapter now, S3b)
  if (pal::g_surface)
    for (FontMapIterator fit = m_Fonts.begin ();
         fit != m_Fonts.end ();
         fit++)
           pal::g_surface->destroyFont (fit->second);

  m_Fonts.clear ();

  // delete our images
  for (ImageMapIterator it = m_Images.begin ();
       it != m_Images.end ();
       it++)
         delete it->second;

  m_Images.clear ();

  // delete our hotspots
  for (HotspotMapIterator hit = m_Hotspots.begin ();
       hit != m_Hotspots.end ();
       hit++)
         delete hit->second;

  m_Hotspots.clear ();

  delete pdc;
  }   // end CMiniWindow::~CMiniWindow  (destructor)


// a negative or zero value for the RH side of a rectange is considered offset from the right edge
// eg. -1 is 1 pixel in from right, 0 is the RH edge.
long CMiniWindow::FixRight (const long Right)
  {

  if (Right <= 0)
    return m_iWidth + Right;

  return Right;
  }  // end CMiniWindow::FixRight

static int BytesPerLine (int nWidth, int nBitsPerPixel)
  {
  return ( (nWidth * nBitsPerPixel + 31) & (~31) ) / 8;
  }

// Decode a Win32 pen spec (colour + style bitmask + width) into a toolkit-neutral
// pal::PenSpec for the ISurface seam (docs/PAL.md S3c). Call ONLY after
// ValidatePenStyle has accepted PenStyle, so the STYLE/ENDCAP/JOIN fields are one
// of the known flags; the adapter re-encodes them (GdiSurface reproduces the old MakeAPen
// exactly, so Windows is unchanged).
static pal::PenSpec PenSpecFromWin32 (long PenColour, long PenStyle, long PenWidth)
  {
  pal::PenSpec p;
  p.colour = (pal::Colour) PenColour;
  p.width  = (int) PenWidth;
  switch (PenStyle & PS_STYLE_MASK)
    {
    case PS_NULL:        p.style = pal::PenStyle::Null;        break;
    case PS_INSIDEFRAME: p.style = pal::PenStyle::InsideFrame; break;
    case PS_DASH:        p.style = pal::PenStyle::Dash;        break;
    case PS_DOT:         p.style = pal::PenStyle::Dot;         break;
    case PS_DASHDOT:     p.style = pal::PenStyle::DashDot;     break;
    case PS_DASHDOTDOT:  p.style = pal::PenStyle::DashDotDot;  break;
    default:             p.style = pal::PenStyle::Solid;       break;  // PS_SOLID
    }
  switch (PenStyle & PS_ENDCAP_MASK)
    {
    case PS_ENDCAP_SQUARE: p.endcap = pal::PenEndcap::Square; break;
    case PS_ENDCAP_FLAT:   p.endcap = pal::PenEndcap::Flat;   break;
    default:               p.endcap = pal::PenEndcap::Round;  break;  // PS_ENDCAP_ROUND
    }
  switch (PenStyle & PS_JOIN_MASK)
    {
    case PS_JOIN_BEVEL: p.join = pal::PenJoin::Bevel; break;
    case PS_JOIN_MITER: p.join = pal::PenJoin::Miter; break;
    default:            p.join = pal::PenJoin::Round; break;  // PS_JOIN_ROUND
    }
  return p;
  } // end of PenSpecFromWin32

// a negative or zero value for the bottom of a rectange is considered offset from the bottom edge
long CMiniWindow::FixBottom (const long Bottom)
  {

  if (Bottom <= 0)
    return m_iHeight + Bottom;

  return Bottom;

  }  // end  CMiniWindow::FixBottom


/* positions:

  0 = stretch to output view size
  1 = stretch with aspect ratio

  2 = stretch to owner size
  3 = stretch with aspect ratio

  -- going clockwise here:

  -- top
  4 = top left
  5 = center left-right at top
  6 = top right

  -- rh side
  7 = on right, center top-bottom
  8 = on right, at bottom

  -- bottom
  9 = center left-right at bottom

  -- lh side
  10 = on left, at bottom
  11 = on left, center top-bottom

  -- middle
  12 = center all

  13 = tile


  */


// create (or re-create) a mini-window
void CMiniWindow::Create (long Left, long Top, long Width, long Height,
                          short Position, long Flags,
                          COLORREF BackgroundColour)
  {

  m_Location.x           = Left            ;
  m_Location.y           = Top             ;
  m_iWidth               = Width           ;
  m_iHeight              = Height          ;
  m_iPosition            = Position        ;
  m_iFlags               = Flags           ;
  m_iBackgroundColour    = BackgroundColour;

  // get rid of old one if any
  if (m_Bitmap)
    {
    pdc->SelectObject(m_oldBitmap);    // swap old one back
    m_Bitmap->DeleteObject ();
    delete m_Bitmap;
    }

  // get rid of old device context and make a new one
  delete pdc;
  pdc = new CDC;
  pdc->CreateCompatibleDC(NULL);
  pdc->SetTextAlign (TA_LEFT | TA_TOP);

  m_Bitmap = new CBitmap;

  //  CreateBitmap with zero-dimensions creates a monochrome bitmap, so force to be at least 1x1
#ifdef _WIN32
  m_Bitmap->CreateBitmap (MAX (m_iWidth, 1), MAX (m_iHeight, 1), 1, GetDeviceCaps((*pdc), BITSPIXEL), NULL);
#else
  // Same canvas, at the display's real resolution.  The miniwindow is still
  // m_iWidth x m_iHeight in the coordinates every plugin draws with -- only the
  // number of physical pixels behind them changes.  Previously this was a 1x
  // bitmap that the compositor stretched to its logical size, so on a Retina
  // panel every miniwindow was drawn at half resolution and blown up: blocky
  // maps, and text that read as soft and undersized next to the output pane.
  // On a 72 dpi display the scale is 1 and this is byte-for-byte the old path.
  m_Bitmap->CreateCanvasBitmap (MAX (m_iWidth, 1), MAX (m_iHeight, 1));
#endif
  m_oldBitmap = pdc->SelectObject (m_Bitmap);
  pdc->SetWindowOrg(0, 0);

  pdc->FillSolidRect (0, 0, m_iWidth, m_iHeight, m_iBackgroundColour);

  // Adopt the freshly-created pdc as a pal::ISurface for migrated ops.
  if (pal::g_releaseAdoptedSurface) pal::g_releaseAdoptedSurface (m_surfaceId);
  m_surfaceId = pal::g_adoptSurface
              ? pal::g_adoptSurface (pdc, m_iWidth, m_iHeight)
              : pal::SurfaceId::None;

  m_bShow = false;

  // a newly created window has no hotspots
  if ((Flags & MINIWINDOW_KEEP_HOTSPOTS) == 0)
    DeleteAllHotspots ();

  } // end of MiniWindow::Create


// set/clear the show flag so the window becomes visible
void  CMiniWindow::Show (bool bShow)
  {
  m_bShow = bShow;
  }    // end of CMiniWindow::Show

/*

  Actions:

  1 = FrameRect        ( 1 pixel )
  2 = FillRect
  3 = InvertRect
  4 = 3D Rect    (Colour1 is top and left edge colour, Colour2 is bottom and right edge colour)
  5 = DrawEdge   (draws a 3d-style edge with optional fill)

  Colour1 = style of edge:

  EDGE_RAISED:      // 5
  EDGE_ETCHED:      // 6
  EDGE_BUMP:        // 9
  EDGE_SUNKEN:      // 10

  Colour2 = where to draw it:

  BF_TOPLEFT      0x3
  BF_TOPRIGHT     0x6
  BF_BOTTOMLEFT   0x9
  BF_BOTTOMRIGHT  0xC
  BF_RECT         0xF

  BF_DIAGONAL     0x0010

  // For diagonal lines, the BF_RECT flags specify the end point of the
  // vector bounded by the rectangle parameter.

  BF_DIAGONAL_ENDTOPLEFT      0x13
  BF_DIAGONAL_ENDTOPRIGHT     0x16
  BF_DIAGONAL_ENDBOTTOMLEFT   0x19
  BF_DIAGONAL_ENDBOTTOMRIGHT  0x1C

  Additional Colour2 flags:

  BF_MIDDLE       0x0800   Fill in the middle
  BF_SOFT         0x1000   For softer buttons
  BF_ADJUST       0x2000   Calculate the space left over
  BF_FLAT         0x4000   For flat rather than 3D borders
  BF_MONO         0x8000   For monochrome borders


  6 = Flood Fill Border (fills to border specified by Colour1)

  7 = Flood Fill Surface (fills while on surface specified by Colour1)

*/

// various rectangle operations
long  CMiniWindow::RectOp (short Action, long Left, long Top, long Right, long Bottom, long Colour1, long Colour2)
  {
  // Draw through the pal::ISurface adapter adopted onto this window's pdc
  // (docs/PAL.md S3). FixRight/FixBottom (<=0 = offset-from-edge) stays engine-
  // side, above the seam. The adapter is the native drawing backend on every
  // platform (GDI on Windows, wx on the port), so this is behaviour-identical to
  // the former direct GDI calls — except floodFill, a no-op via wxDC::FloodFill
  // on wxOSX, which now works.
  pal::ISurface* srf = pal::g_surface;
  if (!srf) return eOK;                 // adapter is installed at app startup
  const pal::SurfaceId sid = m_surfaceId;
  const int L = (int) Left, T = (int) Top;
  const int R = (int) FixRight (Right), B = (int) FixBottom (Bottom);
  const pal::Rect   rc { L, T, R - L, B - T };
  const pal::Colour c1 = (pal::Colour) Colour1, c2 = (pal::Colour) Colour2;

  switch (Action)
    {
    case 1:   // frame: 1px border via 4 solid fills (MFC FrameRect)
      {
      const pal::BrushSpec b { pal::BrushStyle::Solid, c1 };
      srf->fillRect (sid, { rc.x, rc.y, rc.w, 1 }, b);
      srf->fillRect (sid, { rc.x, rc.y + rc.h - 1, rc.w, 1 }, b);
      srf->fillRect (sid, { rc.x, rc.y, 1, rc.h }, b);
      srf->fillRect (sid, { rc.x + rc.w - 1, rc.y, 1, rc.h }, b);
      return eOK;
      }
    case 2:   // fill
      srf->fillRect (sid, rc, { pal::BrushStyle::Solid, c1 });
      return eOK;
    case 3:   // invert
      srf->invertRect (sid, rc);
      return eOK;
    case 4:   // 3D rect: MFC Draw3dRect — top+left in c1, right+bottom in c2
      {
      const pal::BrushSpec tl  { pal::BrushStyle::Solid, c1 };
      const pal::BrushSpec brc { pal::BrushStyle::Solid, c2 };
      srf->fillRect (sid, { rc.x, rc.y, rc.w - 1, 1 }, tl);           // top
      srf->fillRect (sid, { rc.x, rc.y, 1, rc.h - 1 }, tl);           // left
      srf->fillRect (sid, { rc.x + rc.w - 1, rc.y, 1, rc.h }, brc);   // right
      srf->fillRect (sid, { rc.x, rc.y + rc.h - 1, rc.w, 1 }, brc);   // bottom
      return eOK;
      }
    case 5:   // DrawEdge
      switch (Colour1)   // nEdge — must be one of these 4 flags
        {
        case EDGE_RAISED: case EDGE_ETCHED: case EDGE_BUMP: case EDGE_SUNKEN: break;
        default: return eBadParameter;
        }
      if ((Colour2 & 0xFF) > 0x1F) return eBadParameter;
      srf->drawEdge (sid, rc, (int) Colour1, (int) Colour2);
      return eOK;
    case 6:   // flood fill to border: seed (L,T), fill c2, border c1
      srf->floodFill (sid, { L, T }, c2, c1, pal::FloodMode::Border);
      return eOK;
    case 7:   // flood fill on surface colour c1 with c2
      srf->floodFill (sid, { L, T }, c2, c1, pal::FloodMode::Surface);
      return eOK;
    default:
      return eUnknownOption;
    }
  } // end of CMiniWindow::RectOp

static long ValidatePenStyle (const long PenStyle, const long PenWidth)
  {

  switch (PenStyle & PS_STYLE_MASK)
    {
      // must be one of these flags
      case PS_SOLID:           // 0
      case PS_NULL:            // 5
      case PS_INSIDEFRAME:     // 6
        break;

      case PS_DASH:            // 1       /* -------  */
      case PS_DOT:             // 2       /* .......  */
      case PS_DASHDOT:         // 3       /* _._._._  */
      case PS_DASHDOTDOT:      // 4       /* _.._.._  */
         if (PenWidth > 1)
           return ePenStyleNotValid;
         break;

    default: return ePenStyleNotValid;
    }

  switch (PenStyle & PS_ENDCAP_MASK)
    {
      // must be one of these flags
      case PS_ENDCAP_ROUND:   // 0x000
      case PS_ENDCAP_SQUARE:  // 0x100
      case PS_ENDCAP_FLAT:    // 0x200
        break;

    default: return ePenStyleNotValid;
    }

  switch (PenStyle & PS_JOIN_MASK)
    {
      // must be one of these flags
      case PS_JOIN_ROUND:     // 0x0000
      case PS_JOIN_BEVEL:     // 0x1000
      case PS_JOIN_MITER:     // 0x2000
        break;

    default: return ePenStyleNotValid;
    }

  return eOK;
  }


long ValidateBrushStyle (const long BrushStyle,
                          const long PenColour,
                          const long BrushColour,
                          CBrush & br)
  {


  LOGBRUSH lb;
  lb.lbColor = PenColour;

  switch (BrushStyle)
    {
      // must be one of these flags
      case BS_SOLID:           // 0
         lb.lbStyle = BS_SOLID;
         lb.lbColor = BrushColour;
         break;

      case BS_NULL:            // 1
         lb.lbStyle = BS_NULL;
         break;

      // hatched styles:
      case 2:
         lb.lbStyle = BS_HATCHED;
         lb.lbHatch = HS_HORIZONTAL;
         break;

      case 3:
         lb.lbStyle = BS_HATCHED;
         lb.lbHatch = HS_VERTICAL;
         break;

      case 4:
         lb.lbStyle = BS_HATCHED;
         lb.lbHatch = HS_FDIAGONAL;
         break;

      case 5:
         lb.lbStyle = BS_HATCHED;
         lb.lbHatch = HS_BDIAGONAL;
         break;

      case 6:
         lb.lbStyle = BS_HATCHED;
         lb.lbHatch = HS_CROSS;
         break;

      case 7:
         lb.lbStyle = BS_HATCHED;
         lb.lbHatch = HS_DIAGCROSS;
         break;

         // each byte is a line, so 0xAA would be 10101010   (top line)
         //                         0x55 would be 01010101   (next line)
      case 8:       // fine hatch
        {
        CBitmap bitmap;
        WORD    wBits[] = { 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55 };

        bitmap.CreateBitmap (8, 8, 1, 1, wBits);
        br.CreatePatternBrush (&bitmap);
        return eOK;
        }

      case 9:      // medium hatch
        {
        CBitmap bitmap;
        WORD    wBits[] = { 0x33, 0x33, 0xCC, 0xCC, 0x33, 0x33, 0xCC, 0xCC, };

        bitmap.CreateBitmap (8, 8, 1, 1, wBits);
        br.CreatePatternBrush (&bitmap);
        return eOK;
        }

      case 10:       // coarse hatch
        {
        CBitmap bitmap;
        WORD    wBits[] = { 0x0F, 0x0F, 0x0F, 0x0F, 0xF0, 0xF0, 0xF0, 0xF0 };

        bitmap.CreateBitmap (8, 8, 1, 1, wBits);
        br.CreatePatternBrush (&bitmap);
        return eOK;
        }

      case 11:       // waves - horizontal
        {
        CBitmap bitmap;
        WORD    wBits[] = { 0xCC, 0x33, 0x00, 0x00, 0xCC, 0x33, 0x00, 0x00  };

        bitmap.CreateBitmap (8, 8, 1, 1, wBits);
        br.CreatePatternBrush (&bitmap);
        return eOK;
        }

      case 12:       // waves - vertical
        {
        CBitmap bitmap;
        WORD    wBits[] = { 0x11, 0x11, 0x22, 0x22, 0x11, 0x11, 0x22, 0x22  };

        bitmap.CreateBitmap (8, 8, 1, 1, wBits);
        br.CreatePatternBrush (&bitmap);
        return eOK;
        }

    default: return eBrushStyleNotValid;
    }

  br.CreateBrushIndirect (&lb);

  return eOK;
  }

// Build a toolkit-neutral pal::BrushSpec for the ISurface seam (docs/PAL.md S3c)
// from the Win32 brush params of WindowCircleOp / WindowPolygon. Returns eOK (and
// fills 'spec'), or eBrushStyleNotValid — the same validation the old
// ValidateBrushStyle did. The 5 built-in PATTERN styles (8-12) point
// spec.monoPattern at a static 8x8 mono table + carry fg/bg; the adapter builds
// the mono CreatePatternBrush and applies the colours with GDI mono-brush
// semantics, so Windows is byte-exact (the 0/1->text/bk mapping is GDI's, not ours).
static long BrushSpecFromWin32 (long BrushStyle, long PenColour, long BrushColour,
                                pal::BrushSpec & spec)
  {
  static const unsigned char kFine  [8] = { 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55 };
  static const unsigned char kMedium[8] = { 0x33, 0x33, 0xCC, 0xCC, 0x33, 0x33, 0xCC, 0xCC };
  static const unsigned char kCoarse[8] = { 0x0F, 0x0F, 0x0F, 0x0F, 0xF0, 0xF0, 0xF0, 0xF0 };
  static const unsigned char kWaveH [8] = { 0xCC, 0x33, 0x00, 0x00, 0xCC, 0x33, 0x00, 0x00 };
  static const unsigned char kWaveV [8] = { 0x11, 0x11, 0x22, 0x22, 0x11, 0x11, 0x22, 0x22 };
  static const unsigned char * const kPatterns [5] = { kFine, kMedium, kCoarse, kWaveH, kWaveV };

  switch (BrushStyle)
    {
    case BS_SOLID:                                       // 0
      spec.style = pal::BrushStyle::Solid;
      spec.fg    = (pal::Colour) BrushColour;
      break;
    case BS_NULL:                                        // 1
      spec.style = pal::BrushStyle::Null;
      break;
    case 2: case 3: case 4: case 5: case 6: case 7:      // GDI hatches HS_*
      spec.style = pal::BrushStyle::Hatch;
      spec.hatch = (pal::HatchKind) (BrushStyle - 2);
      spec.fg    = (pal::Colour) PenColour;              // hatch line colour (lb.lbColor)
      spec.bg    = (pal::Colour) BrushColour;            // gap colour (SetBkColor); -1 = transparent
      break;
    case 8: case 9: case 10: case 11: case 12:           // built-in mono patterns
      spec.style       = pal::BrushStyle::Pattern;
      spec.monoPattern = kPatterns [BrushStyle - 8];
      spec.fg          = (pal::Colour) BrushColour;      // SetTextColor (mono fg)
      spec.bg          = (pal::Colour) PenColour;        // SetBkColor  (mono bg)
      break;
    default:
      return eBrushStyleNotValid;
    }
  return eOK;
  } // end of BrushSpecFromWin32

// Read a region from a surface into a full-request-size buffer, exactly like
// the old "BitBlt into a zero-initialised DIBSection": a read past the source's
// edge leaves zero bytes there (alpha 0 = keep base, for the alpha ops) instead
// of shrinking the buffer the way adapters' readPixels clipping does.
static void ReadRegionZeroFill (pal::ISurface * srf, pal::SurfaceId id,
                                long x, long y, long w, long h,
                                pal::PixelFormat fmt, pal::PixelBuffer & out)
  {
  out.format = fmt;
  out.size   = { (int) w, (int) h };
  out.stride = pal::PixelBuffer::strideFor ((int) w, fmt);
  out.bytes.assign ((size_t) out.stride * h, 0);

  pal::PixelBuffer part;
  srf->readPixels (id, { (int) x, (int) y, (int) w, (int) h }, part, fmt);
  if (!part.valid ())
    return;

  // the adapter clipped to the source bounds; place the surviving pixels where
  // BitBlt would have put them (offset by whatever was clipped off left/top —
  // zero for the image ops, whose x/y are pre-clamped >= 0)
  long dx = x < 0 ? -x : 0,
       dy = y < 0 ? -y : 0;
  long copyW = part.size.w < w - dx ? part.size.w : w - dx;
  if (copyW <= 0)
    return;
  for (int row = 0; row < part.size.h && dy + row < h; row++)
    memcpy (out.row ((int) dy + row) + (size_t) dx * out.bytesPerPixel (),
            part.row (row),
            (size_t) copyW * part.bytesPerPixel ());
  } // end of ReadRegionZeroFill

/*

  Actions:

  1 = Ellipse
  2 = Rectangle (can have thicker pen style and be filled)
  3 = Round Rectangle
  4 = Chord (a closed figure bounded by the intersection of an ellipse and a line segment)
  5 = Pie (Draws a pie-shaped wedge by drawing an elliptical arc whose center and two endpoints are joined by lines)

  */

// various circle/ellipse/pie operations
long CMiniWindow::CircleOp (short Action,
                            long Left, long Top, long Right, long Bottom,
                            long PenColour, long PenStyle, long PenWidth,
                            long BrushColour, long BrushStyle,
                            long Extra1, long Extra2, long Extra3, long Extra4)
  {
  if (ValidatePenStyle (PenStyle, PenWidth))
    return ePenStyleNotValid;

  pal::BrushSpec brush;
  if (BrushSpecFromWin32 (BrushStyle, PenColour, BrushColour, brush))
    return eBrushStyleNotValid;

  // Map the action to a shape kind (unknown -> eUnknownOption, as before).
  pal::ShapeKind kind;
  switch (Action)
    {
    case 1:  kind = pal::ShapeKind::Ellipse;   break;
    case 2:  kind = pal::ShapeKind::Rect;       break;
    case 3:  kind = pal::ShapeKind::RoundRect;  break;   // Extra1/2 = corner ellipse
    case 4:  kind = pal::ShapeKind::Chord;       break;  // Extra1/2 start, Extra3/4 end
    case 5:  kind = pal::ShapeKind::Pie;         break;  // Extra1/2 start, Extra3/4 end
    default: return eUnknownOption;
    }

  // Draw through the seam (docs/PAL.md S3c). FixRight/FixBottom stays engine-side;
  // the a/b points carry the RoundRect corner / Chord+Pie endpoints, exactly as
  // the old CPoint(Extra1,Extra2)/CPoint(Extra3,Extra4).
  pal::ISurface* srf = pal::g_surface;
  if (!srf) return eOK;

  const long R = FixRight (Right), B = FixBottom (Bottom);
  srf->shape (m_surfaceId, kind,
              { (int) Left, (int) Top, (int) (R - Left), (int) (B - Top) },
              { (int) Extra1, (int) Extra2 },
              { (int) Extra3, (int) Extra4 },
              PenSpecFromWin32 (PenColour, PenStyle, PenWidth),
              brush);

  return eOK;

  } // end of CMiniWindow::CircleOp


// add a font to our map of fonts by name (eg. "inventory")
long CMiniWindow::Font (LPCTSTR FontId,        // eg. "inventory"
                        LPCTSTR FontName,      // eg. "Courier New"
                       double Size,
                       BOOL Bold, BOOL Italic, BOOL Underline, BOOL Strikeout,
                       short Charset, short PitchAndFamily)
  {

  FontMapIterator it = m_Fonts.find (FontId);

  if (it != m_Fonts.end ())
    {
    if (pal::g_surface)
      pal::g_surface->destroyFont (it->second);   // delete existing font
    m_Fonts.erase (it);
    }

  // no font name, and zero point size requested - delete old font
  if (FontName [0] == 0 && Size == 0.0)
    return eOK;

  pal::ISurface* srf = pal::g_surface;
  if (!srf)
    return eCannotAddFont;

  // Create the font through the surface seam (docs/PAL.md S3b). The point size
  // is converted to a positive pixel height via the adapter's dpi() — exactly
  // the old  -MulDiv (Size, GetDeviceCaps (LOGPIXELSY), 72)  (the adapter negates
  // it for the cell-height CreateFont convention). Default 10pt if Size == 0.
  //
  // This is the Windows formula, unmodified, on every platform.  A miniwindow is
  // a PIXEL canvas: the plugin gives WindowCreate a width and height in pixels
  // and positions every WindowText call in pixels, all of it sized against what
  // the font occupies on Windows -- where 96 logical dpi makes a 10-point font
  // MulDiv (10, 96, 72) = 13 of those pixels.
  //
  // An adapter whose dpi() is NOT 96 therefore has to correct for that, and the
  // correction belongs in the adapter, not here: pal::CpuRaster (dpi() 96, the
  // Windows-parity reference) needs none at all, while pal::WxSurface (the real
  // display PPI, 144 on a Retina Mac) needs one -- different arithmetic for the
  // same Windows result, so it is adapter-shaped by construction.  See
  // pal::WxSurface::createFont.
  //
  // Measured (D-21): a compensating factor applied HERE instead ran the wx path
  // 1.30-1.33x oversize and the CpuRaster parity reference 1.78x, which is what
  // made plugin strings clip the edge of their own miniwindow.
  pal::FontSpec spec;
  spec.face               = FontName;
  double dPoints = Size ? Size : 10.0;

  spec.pixelHeight        = MulDiv (dPoints, srf->dpi (), 72);
  spec.weight             = Bold ? FW_BOLD : FW_NORMAL;
  spec.italic             = Italic    != 0;
  spec.underline          = Underline != 0;
  spec.strikeout          = Strikeout != 0;
  spec.charsetHint        = Charset;
  spec.pitchAndFamilyHint = PitchAndFamily;

  pal::FontId id = srf->createFont (spec);
  if (id == pal::FontId::None)
    return eCannotAddFont;

  m_Fonts [FontId] = id;
  return eOK;

  }   // end of CMiniWindow::Font


static void SetUpVariantLong (VARIANT & tVariant, const long iContents)
  {
  VariantClear (&tVariant);
  tVariant.vt = VT_I4;
  tVariant.lVal = iContents;
  }   // end of SetUpVariantLong

static void SetUpVariantString (VARIANT & tVariant, const CString & strContents)
  {
  VariantClear (&tVariant);
  tVariant.vt = VT_BSTR;
  tVariant.bstrVal = strContents.AllocSysString ();
  }   // end of SetUpVariantString

static void SetUpVariantBool (VARIANT & tVariant, const BOOL iContents)
  {
  VariantClear (&tVariant);
  tVariant.vt = VT_BOOL;
  tVariant.boolVal = iContents;
  }   // end of SetUpVariantBool

static void SetUpVariantDate (VARIANT & tVariant, const COleDateTime iContents)
  {
  VariantClear (&tVariant);
  tVariant.vt = VT_DATE;
  tVariant.date = iContents;
  }   // end of SetUpVariantDate

// return info about the select font
void CMiniWindow::FontInfo (LPCTSTR FontId, long InfoType, VARIANT & vaResult)
  {
  FontMapIterator it = m_Fonts.find (FontId);

  if (it == m_Fonts.end ())
    return;   // no such font

  // One seam call replaces select-font + GetTextMetrics + GetTextFace + restore;
  // every InfoType maps to a pal::FontMetrics field (docs/PAL.md S3b).
  pal::FontMetrics m = pal::g_surface ? pal::g_surface->fontMetrics (it->second)
                                      : pal::FontMetrics ();

  switch (InfoType)
    {

    case  1:  SetUpVariantLong (vaResult, m.height);           break;
    case  2:  SetUpVariantLong (vaResult, m.ascent);           break;
    case  3:  SetUpVariantLong (vaResult, m.descent);          break;
    case  4:  SetUpVariantLong (vaResult, m.internalLeading);  break;
    case  5:  SetUpVariantLong (vaResult, m.externalLeading);  break;
    case  6:  SetUpVariantLong (vaResult, m.aveCharWidth);     break;
    case  7:  SetUpVariantLong (vaResult, m.maxCharWidth);     break;
    case  8:  SetUpVariantLong (vaResult, m.weight);           break;
    case  9:  SetUpVariantLong (vaResult, m.overhang);         break;
    case 10:  SetUpVariantLong (vaResult, m.digitizedAspectX); break;
    case 11:  SetUpVariantLong (vaResult, m.digitizedAspectY); break;
    case 12:  SetUpVariantLong (vaResult, m.firstChar);        break;
    case 13:  SetUpVariantLong (vaResult, m.lastChar);         break;
    case 14:  SetUpVariantLong (vaResult, m.defaultChar);      break;
    case 15:  SetUpVariantLong (vaResult, m.breakChar);        break;
    case 16:  SetUpVariantLong (vaResult, m.italic);           break;
    case 17:  SetUpVariantLong (vaResult, m.underlined);       break;
    case 18:  SetUpVariantLong (vaResult, m.struckOut);        break;
    case 19:  SetUpVariantLong (vaResult, m.pitchAndFamily);   break;
    case 20:  SetUpVariantLong (vaResult, m.charset);          break;
    case 21:  SetUpVariantString (vaResult, CString (m.face.c_str ())); break;

    default:
      vaResult.vt = VT_NULL;
      break;

    } // end of switch
  }  // end of CMiniWindow::FontInfo

// return list of fonts we installed
void CMiniWindow::FontList (VARIANT & vaResult)
  {
  COleSafeArray sa;   // for array list

  long iCount = 0;

  // put the arrays into the array
  if (!m_Fonts.empty ())    // cannot create empty dimension
    {
    sa.CreateOneDim (VT_VARIANT, m_Fonts.size ());

    for (FontMapIterator it = m_Fonts.begin ();
         it != m_Fonts.end ();
         it++)
           {
            // the array must be a bloody array of variants, or VBscript kicks up
            COleVariant v (it->first.c_str ());
            sa.PutElement (&iCount, &v);
            iCount++;
           }

    } // end of having at least one

  vaResult = sa.Detach ();


  } // end of  CMiniWindow::FontList



//helper function to calculate length of UTF8 string
static long CalculateUTF8length (LPCTSTR Text, size_t length)
  {

  int erroroffset;
  int iBad = _pcre_valid_utf ((unsigned char  *) Text, length, &erroroffset);
  if (iBad > 0)
    return -1;

    // string is OK, calculate its length

  int i = 0;   // length

  // this algorithm assumes the UTF-8 is OK, based on the earlier check

  for (const unsigned char *p = (const unsigned char *) Text ;
       length-- > 0;
       i++)
    {
    int ab;    // additional bytes
    int c = *p++;  // this byte

    if (c < 128)
      continue;     // zero additional bytes

    ab = _pcre_utf8_table4 [c & 0x3f];  /* Number of additional bytes */

    length -= ab;  // we know string is valid already, so just skip the additional bytes (ab)
    p += ab;

    }

  return i;

  }   // end of CalculateUTF8length

// output text, ordinary or UTF8 - returns length of text
long CMiniWindow::Text (LPCTSTR FontId,  // which previously-created font
                        LPCTSTR Text,    // what to say
                        long Left, long Top, long Right, long Bottom, // where to say it
                        long Colour,       // colour to show it in
                        BOOL Unicode)      // true if UTF8

  {
  FontMapIterator it = m_Fonts.find (FontId);

  if (it == m_Fonts.end ())
    return -2;

  size_t length = strlen (Text);

  // give up if no text
  if (length <= 0)
    return 0;

  // quick sanity check on our UTF8 stuff
  if (Unicode)
    {
    if (CalculateUTF8length (Text, length) < 0)
      return -3;    // ohno!
    }

  pal::ISurface* srf = pal::g_surface;
  if (!srf)
    return -2;

  // Draw through the surface seam (docs/PAL.md S3b). The adapter auto-detects
  // UTF-8 vs the font's legacy charset (the decided UTF-8 + charset-hint model),
  // so the per-call Unicode flag only gates the validity guard above; rendering
  // is byte-identical to the old ExtTextOut/ExtTextOutW + ETO_CLIPPED path. The
  // clip box is the engine's FixRight/FixBottom-resolved rectangle, unchanged.
  const long right  = FixRight (Right);
  const long bottom = FixBottom (Bottom);
  const pal::Rect clip { (int) Left, (int) Top, (int) (right - Left), (int) (bottom - Top) };

  int width = srf->drawText (m_surfaceId, it->second, Text, length,
                             { (int) Left, (int) Top }, (pal::Colour) Colour, &clip);

  return min ((long) width, right - Left);  // if clipped, length is width of rectangle
  } // end of CMiniWindow::Text


// measure text, ordinary or UTF8
long CMiniWindow::TextWidth (LPCTSTR FontId,  // which previously-created font
                             LPCTSTR Text,    // what to measure
                             BOOL Unicode)    // true if UTF8
  {

  FontMapIterator it = m_Fonts.find (FontId);

  if (it == m_Fonts.end ())
    return -2;

  size_t length = strlen (Text);

  // give up if no text
  if (length <= 0)
    return 0;

  // quick sanity check on our UTF8 stuff
  if (Unicode)
    {
    if (CalculateUTF8length (Text, length) < 0)
      return -3;    // ohno!
    }

  pal::ISurface* srf = pal::g_surface;
  if (!srf)
    return -2;

  // Measure through the seam (docs/PAL.md S3b) — the same font metrics drawText
  // uses, so WindowTextWidth and WindowText agree.
  return srf->measureText (it->second, Text, length).w;

  }  // end of  CMiniWindow::TextWidth


// draws a straight line
long CMiniWindow::Line (long x1, long y1, long x2, long y2,
                        long PenColour, long PenStyle, long PenWidth)
  {

  if (ValidatePenStyle (PenStyle, PenWidth))
    return ePenStyleNotValid;

  // Draw through the ISurface seam (docs/PAL.md S3c). Note: NO FixRight/FixBottom
  // on x2/y2 (as at version 4.38).
  pal::ISurface* srf = pal::g_surface;
  if (!srf) return eOK;

  srf->line (m_surfaceId, { (int) x1, (int) y1 }, { (int) x2, (int) y2 },
             PenSpecFromWin32 (PenColour, PenStyle, PenWidth));

  return eOK;

  }  // end of CMiniWindow::Line


// draws an arc
long CMiniWindow::Arc (long Left, long Top, long Right, long Bottom,
                        long x1, long y1,
                        long x2, long y2,
                        long PenColour, long PenStyle, long PenWidth)
  {

  if (ValidatePenStyle (PenStyle, PenWidth))
    return ePenStyleNotValid;

  // Draw through the ISurface seam (docs/PAL.md S3c). FixRight/FixBottom (<=0 =
  // offset-from-edge) stays engine-side; it resolves Right/Bottom AND the arc
  // end-point x2/y2 (but NOT the start x1/y1), exactly as the old pdc->Arc.
  pal::ISurface* srf = pal::g_surface;
  if (!srf) return eOK;

  const long R = FixRight (Right), B = FixBottom (Bottom);
  srf->arc (m_surfaceId, { (int) Left, (int) Top, (int) (R - Left), (int) (B - Top) },
            { (int) x1, (int) y1 },
            { (int) FixRight (x2), (int) FixBottom (y2) },
            PenSpecFromWin32 (PenColour, PenStyle, PenWidth));

  return eOK;

  }  // end of CMiniWindow::Arc


// return info about the window
void CMiniWindow::Info (long InfoType, VARIANT & vaResult)
  {

  switch (InfoType)
    {
    case  1:  SetUpVariantLong    (vaResult, m_Location.x);        break; // left
    case  2:  SetUpVariantLong    (vaResult, m_Location.y);        break; // top
    case  3:  SetUpVariantLong    (vaResult, m_iWidth);            break; // width
    case  4:  SetUpVariantLong    (vaResult, m_iHeight);           break; // height
    case  5:  SetUpVariantBool    (vaResult, m_bShow);             break; // show flag
    case  6:  SetUpVariantBool    (vaResult, m_bTemporarilyHide);  break; // is it hidden right now?
    case  7:  SetUpVariantLong    (vaResult, m_iPosition);         break; // layout mode
    case  8:  SetUpVariantLong    (vaResult, m_iFlags);            break; // flags
    case  9:  SetUpVariantLong    (vaResult, m_iBackgroundColour); break; // background colour

    case 10:  SetUpVariantLong    (vaResult, m_rect.left);         break; // where it is right now
    case 11:  SetUpVariantLong    (vaResult, m_rect.top);          break; //       "
    case 12:  SetUpVariantLong    (vaResult, m_rect.right);        break; //       "
    case 13:  SetUpVariantLong    (vaResult, m_rect.bottom);       break; //       "

    case 14:  SetUpVariantLong    (vaResult, m_last_mouseposition.x);  break; //  last mouse x position
    case 15:  SetUpVariantLong    (vaResult, m_last_mouseposition.y);  break; //  last mouse y position

    case 16:  SetUpVariantLong    (vaResult, m_last_mouse_update);  break;    //  last position update count

    case 17:  SetUpVariantLong    (vaResult, m_client_mouseposition.x); break; //  last client mouse x position
    case 18:  SetUpVariantLong    (vaResult, m_client_mouseposition.y); break; //  last client mouse y position

    case 19:  SetUpVariantString  (vaResult, m_sMouseOverHotspot.c_str ()); break; // mouse-over hotspot
    case 20:  SetUpVariantString  (vaResult, m_sMouseDownHotspot.c_str ()); break; // mouse-down hotspot
    case 21:  SetUpVariantDate    (vaResult, COleDateTime (m_tDateInstalled.GetTime ()));  break;

    case 22:  SetUpVariantLong    (vaResult, m_ZOrder);  break; //  Z-Order
    case 23:  SetUpVariantString  (vaResult, m_sCreatingPlugin.c_str ()); break; // creating plugin ID

    default:
      vaResult.vt = VT_NULL;
      break;

    } // end of switch

  }  // end of CMiniWindow::Info


// loads an image file, ready for drawing into window
long CMiniWindow::LoadImage (LPCTSTR ImageId, LPCTSTR FileName)
  {
  ImageMapIterator it = m_Images.find (ImageId);

  if (it != m_Images.end ())
    {
    delete it->second;         // delete existing image
    m_Images.erase (it);
    }

  CString strFileName = FileName;

  strFileName.TrimLeft ();
  strFileName.TrimRight ();

  // no file name means get rid of image
  if (strFileName.IsEmpty ())
    return eOK;

  // have to be long enough to have x.bmp
  if (strFileName.GetLength () < 5)
    return eBadParameter;

  // handle PNG files separately
  if (strFileName.Right (4).CompareNoCase (".png") == 0)
    return LoadPngImage (ImageId, FileName);

  // must be .bmp or .png file
  if (strFileName.Right (4).CompareNoCase (".bmp") != 0)
    return eBadParameter;


  HBITMAP hBmp = (HBITMAP)::LoadImage(
                  NULL,
                  FileName,
                  IMAGE_BITMAP,
                  0,
                  0,
                  LR_LOADFROMFILE|LR_CREATEDIBSECTION
                  );

   if (hBmp)
     {
      CBitmap * pImage = new CBitmap;
      pImage->Attach (hBmp);
      m_MonoImages.erase (ImageId);   // stale CreateImage pattern, if any
      m_Images [ImageId] = pImage;
      return eOK;
     }  // end of having a bitmap loaded


   if (GetLastError () == 2)
     return eFileNotFound;

   return eUnableToLoadImage;


  } // end of CMiniWindow::LoadImage


// loads an image from memory, ready for drawing into window
long CMiniWindow::LoadImageMemory(LPCTSTR ImageId,
                                 unsigned char * Buffer,
                                 const size_t Length,
                                 const bool bAlpha)
  {
  ImageMapIterator it = m_Images.find (ImageId);

  if (it != m_Images.end ())
    {
    delete it->second;         // delete existing image
    m_Images.erase (it);
    }

  HBITMAP hbmp;

  long result =  LoadPngMemory (Buffer, Length, hbmp, bAlpha);

  if (result != eOK)
    return result;

  // make bitmap to add to images list
  CBitmap * pImage = new CBitmap;
  pImage->Attach (hbmp);
  m_MonoImages.erase (ImageId);   // stale CreateImage pattern, if any
  m_Images [ImageId] = pImage;

  return eOK;

  } // end of CMiniWindow::LoadImageMemory


static void user_error_fn(png_structp png_ptr,
        png_const_charp error_msg)
  {
//  AfxMessageBox (error_msg);
  }

static void user_warning_fn(png_structp png_ptr,
    png_const_charp warning_msg)
  {
//  AfxMessageBox (warning_msg);

  }


long CMiniWindow::LoadPngImage (LPCTSTR ImageId, LPCTSTR FileName)
  {

  HBITMAP hbmp;

  long result =  LoadPng (FileName, hbmp);

  if (result != eOK)
    return result;

  // make bitmap to add to images list
  CBitmap * pImage = new CBitmap;
  pImage->Attach (hbmp);
  m_MonoImages.erase (ImageId);   // stale CreateImage pattern, if any
  m_Images [ImageId] = pImage;

  return eOK;
  } // end of CMiniWindow::LoadPngImage



// saves an image file
long CMiniWindow::Write (LPCTSTR FileName)
  {

  CString strFileName = FileName;

  strFileName.TrimLeft ();
  strFileName.TrimRight ();

  if (strFileName.IsEmpty ())
    return eNoNameSpecified;

  // have to be long enough to have x.bmp
  if (strFileName.GetLength () < 5)
    return eBadParameter;

  // must be .bmp or .png file
  if (strFileName.Right (4).CompareNoCase (".bmp") != 0 &&
      strFileName.Right (4).CompareNoCase (".png") != 0)
    return eBadParameter;

  // get window data — readPixels emits the exact DIB bytes (BGR24, bottom-up,
  // padded rows) the old BitBlt-into-a-DIBSection copy produced, so the PNG
  // and BMP writers below consume the buffer unchanged
  pal::ISurface * srf = pal::g_surface;
  if (!srf)
    return eOK;

  BITMAPINFO bmi;
  ZeroMemory (&bmi, sizeof bmi);

  // sizeof the HEADER, not sizeof the whole BITMAPINFO (which is 4 bytes more
  // for its trailing bmiColors [1]).  This same bmi.bmiHeader is written out as
  // the .bmp file's info header below, so a biSize of 44 described a 40-byte
  // header and made the file unopenable by any strict reader (macOS ImageIO
  // refuses it outright).
  bmi.bmiHeader.biSize = sizeof bmi.bmiHeader;
  bmi.bmiHeader.biWidth =          m_iWidth;
  bmi.bmiHeader.biHeight =         m_iHeight;
  bmi.bmiHeader.biPlanes =         1;
  bmi.bmiHeader.biBitCount =       24;
  bmi.bmiHeader.biCompression =    BI_RGB;
  bmi.bmiHeader.biSizeImage =      m_iHeight * BytesPerLine (m_iWidth, 24);

  pal::PixelBuffer buf;
  srf->readPixels (m_surfaceId, { 0, 0, (int) m_iWidth, (int) m_iHeight },
                   buf, pal::PixelFormat::BGR24);
  if (!buf.valid () || buf.bytes.size () < (size_t) bmi.bmiHeader.biSizeImage)
    return eOK;   // cannot happen: a full-window read always succeeds

  unsigned char * pA = buf.bytes.data ();

  long iReturn = eOK;

    // must be .bmp or .png file
  if (strFileName.Right (4).CompareNoCase (".png") == 0)
    iReturn = WritePng (FileName, &bmi, pA);
  else
    {     // write BMP file

    // create requested file
    CFile file;
    if( !file.Open (FileName, CFile::modeWrite | CFile::modeCreate))
      return eCouldNotOpenFile;

    BITMAPFILEHEADER  hdr;
    ZeroMemory (&hdr, sizeof hdr);

    // Fill in the fields of the file header
    hdr.bfType    = ((WORD) ('M' << 8) | 'B');  // is always "BM"
    hdr.bfOffBits = sizeof hdr  + sizeof bmi.bmiHeader;
    hdr.bfSize    =  hdr.bfOffBits + bmi.bmiHeader.biSizeImage;

    try
      {
      // Write the file header
      file.Write( &hdr, sizeof hdr);

      // Write the bitmap info header
      file.Write( &bmi.bmiHeader, sizeof bmi.bmiHeader);

      // Write the bits
      file.Write( pA,  bmi.bmiHeader.biSizeImage);

      // Flush + close inside the try so a failed write is reported, not
      // swallowed: off Windows CFile is a buffered stdio FILE*, so a small BMP
      // that fits in one buffer would otherwise reach disk only in ~CFile's
      // unchecked fclose -- world.WindowWrite would return eOK for a 0-byte file
      // on a full disk.  (Windows' CFile is unbuffered; the error already
      // surfaced at the first Write, so this is a no-op there.)
      file.Close();
      }
    catch (CFileException * e)
      {
      e->Delete ();
      iReturn = eLogFileBadWrite;
      } // end of catching a file exception

    }   // end of writing BMP file

  return iReturn;

  } // end of CMiniWindow::Write

long CMiniWindow::WritePng (LPCTSTR FileName, const BITMAPINFO * bmi, unsigned char * pData)
  {
  // open file
#ifdef _WIN32
  FILE *fp = fopen(FileName, "wb");
#else
  FILE *fp = fopen(PortableFilePath(FileName).c_str(), "wb");   // plugins use '\' paths
#endif
  if (!fp)
      return (eCouldNotOpenFile);

  // create PNG structure
  png_structp png_ptr = png_create_write_struct
       (PNG_LIBPNG_VER_STRING, NULL,
        user_error_fn, user_warning_fn);

  if (!png_ptr)
    {
    fclose (fp);
    return eLogFileBadWrite;
    }

  // create info structure
  png_infop info_ptr = png_create_info_struct(png_ptr);
  if (!info_ptr)
    {
    png_destroy_write_struct(&png_ptr, (png_infopp)NULL);
    fclose (fp);
    return eLogFileBadWrite;
    }


#ifdef _MSC_VER
#pragma warning (push)
#pragma warning (disable : 4611)  // interaction between '_setjmp' and C++ object destruction is non-portable
#endif

  // if png fails it will longjmp back to here, so we destroy the structure,
  // close the file, and wrap up
  if (setjmp(png_jmpbuf(png_ptr)))
    {
    png_destroy_write_struct(&png_ptr, &info_ptr);
    fclose (fp);
    return eLogFileBadWrite;
    }

#ifdef _MSC_VER
#pragma warning (pop)
#endif

  // initialize IO
  png_init_io (png_ptr, fp);

  // tell PNG the file format
  png_set_IHDR  (png_ptr,
                 info_ptr,
                 bmi->bmiHeader.biWidth,
                 bmi->bmiHeader.biHeight,
                 8,        // bits per pixel
                 PNG_COLOR_TYPE_RGB,
                 PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);

  // set up pointer to each row of data
  vector<png_byte *> row_pointers;
  row_pointers.resize (bmi->bmiHeader.biHeight);

  long bpl = BytesPerLine (bmi->bmiHeader.biWidth, 24);

  long row;
  unsigned char * p = pData;

  // have to reverse row order
  png_uint_32 iHeight = png_get_image_height (png_ptr, info_ptr);
  for (row = 0; row < iHeight; row++, p += bpl)
     row_pointers [bmi->bmiHeader.biHeight - row - 1] = p;

  // tell png where our pixel data is
  png_set_rows (png_ptr, info_ptr, &row_pointers [0]);

  // write out the image data
  png_write_png (png_ptr, info_ptr, PNG_TRANSFORM_BGR, NULL);

  // wrap up
  png_write_end (png_ptr, info_ptr);

  // done with data
  png_destroy_write_struct (&png_ptr, &info_ptr);

  // done with file
  fclose (fp);

  return eOK;
  } // end of CMiniWindow::WritePng


/*

  for monitors with 256 colours or less, consider:

HDRAWDIB hdd = DrawDibOpen();
DrawDibDraw(hdd, hPaintDC,0,0,BITMAP_WIDTH,BITMAP_HEIGHT,&m_BitmapInfo,m_pBits,0,0,BITMAP_WIDTH,BITMAP_HEIGHT,DDF_HALFTONE);
DrawDibClose(hdd);

  */


// draw a previously-loaded image into the window
long CMiniWindow::DrawImage(LPCTSTR ImageId,
               long Left, long Top, long Right, long Bottom,
               short Mode,
               long SrcLeft, long SrcTop, long SrcRight, long SrcBottom)
  {

  ImageMapIterator it = m_Images.find (ImageId);

  if (it == m_Images.end ())
    return eImageNotInstalled;

  CBitmap * bitmap = it->second;
  BITMAP  bi;
  bitmap->GetBitmap (&bi);

  pal::ISurface* srf = pal::g_surface;
  if (!srf) return eOK;

  // Adopt the image bitmap as a TEMPORARY source surface for the seam blit
  // (docs/PAL.md S3d). The image stays engine-owned (m_Images); this just wraps
  // its CDC exactly as the old bmDC, so DrawImage needs no image-storage change.
  CDC bmDC;
  bmDC.CreateCompatibleDC (&(*pdc));
  CBitmap * pOldbmp = bmDC.SelectObject (bitmap);
  pal::SurfaceId srcSid = pal::g_adoptSurface
                        ? pal::g_adoptSurface (&bmDC, bi.bmWidth, bi.bmHeight)
                        : pal::SurfaceId::None;

  long iResult = eOK;

  // adjust so that -1 means 1 from right
  if (SrcRight <= 0)
    SrcRight = bi.bmWidth + SrcRight;

  if (SrcBottom <= 0)
    SrcBottom =  bi.bmHeight + SrcBottom;

  // calculate size of desired rectangle
  long iWidth = SrcRight - SrcLeft;
  long iHeight = SrcBottom - SrcTop;

  if (iWidth > 0 && iHeight > 0)   // sanity check
    switch (Mode)
      {
      case 1:   // straight copy (== BitBlt SRCCOPY)
        srf->blit (m_surfaceId, { (int) Left, (int) Top, (int) iWidth, (int) iHeight },
                   srcSid, { (int) SrcLeft, (int) SrcTop, (int) iWidth, (int) iHeight },
                   pal::BlitMode::Copy, 0, pal::BlitQuality::Fast, nullptr);
        break;

      case 2:   // stretch (== StretchBlt SRCCOPY, HALFTONE)
        srf->blit (m_surfaceId,
                   { (int) Left, (int) Top,
                     (int) (FixRight (Right) - Left), (int) (FixBottom (Bottom) - Top) },
                   srcSid, { (int) SrcLeft, (int) SrcTop,
                             (int) (SrcRight - SrcLeft), (int) (SrcBottom - SrcTop) },
                   pal::BlitMode::Copy, 0, pal::BlitQuality::Smooth, nullptr);
        break;

      case 3:   // transparency: colour-key = the image's top-left pixel (0,0)
        srf->blit (m_surfaceId, { (int) Left, (int) Top, (int) iWidth, (int) iHeight },
                   srcSid, { (int) SrcLeft, (int) SrcTop, (int) iWidth, (int) iHeight },
                   pal::BlitMode::TransparentKey, srf->getPixel (srcSid, { 0, 0 }),
                   pal::BlitQuality::Fast, nullptr);
        break;

      default:
        iResult = eBadParameter;
        break;
      } // end of switch

  if (pal::g_releaseAdoptedSurface)
    pal::g_releaseAdoptedSurface (srcSid);
  bmDC.SelectObject (pOldbmp);

  return iResult;
  }  // end of CMiniWindow::DrawImage


// return list of images
void CMiniWindow::ImageList(VARIANT & vaResult)
  {
  COleSafeArray sa;   // for array list

  long iCount = 0;

  // put the arrays into the array
  if (!m_Images.empty ())    // cannot create empty dimension
    {
    sa.CreateOneDim (VT_VARIANT, m_Images.size ());

    for (ImageMapIterator it = m_Images.begin ();
         it != m_Images.end ();
         it++)
           {
            // the array must be a bloody array of variants, or VBscript kicks up
            COleVariant v (it->first.c_str ());
            sa.PutElement (&iCount, &v);
            iCount++;
           }

    } // end of having at least one

  vaResult = sa.Detach ();

  }   // end of CMiniWindow::ImageList



// return info about the selected image
void CMiniWindow::ImageInfo (LPCTSTR ImageId, long InfoType, VARIANT & vaResult)
  {
  ImageMapIterator it = m_Images.find (ImageId);

  if (it == m_Images.end ())
    return;   // no such Image

  CBitmap * bitmap = it->second;

  BITMAP  bi;
  bitmap->GetBitmap(&bi);

  switch (InfoType)
    {

    case  1:  SetUpVariantLong (vaResult, bi.bmType);      break;
    case  2:  SetUpVariantLong (vaResult, bi.bmWidth);     break;
    case  3:  SetUpVariantLong (vaResult, bi.bmHeight);    break;
    case  4:  SetUpVariantLong (vaResult, bi.bmWidthBytes);break;
    case  5:  SetUpVariantLong (vaResult, bi.bmPlanes);    break;
    case  6:  SetUpVariantLong (vaResult, bi.bmBitsPixel); break;

     default:
      vaResult.vt = VT_NULL;
      break;

    } // end of switch

  }  // end of CMiniWindow::ImageInfo


// draw bezier curves
long CMiniWindow::Bezier(LPCTSTR Points, long PenColour, long PenStyle, long PenWidth)
  {

  if (ValidatePenStyle (PenStyle, PenWidth))
    return ePenStyleNotValid;

  vector<string> v;

  StringToVector (Points, v, ",");

  int iCount = v.size ();

  // must have at least a start point (2 items), plus one extra (3 points)
  if (iCount < 8)
    return eInvalidNumberOfPoints;

  // and has to be 2 more than 6 items  (1 more than 3n points)
  if ((iCount % 6) != 2)
    return eInvalidNumberOfPoints;

  iCount = iCount / 2;  // number of points

  vector<pal::Point> points (iCount);

  int iCurrent = 0;

  for (vector<string>::const_iterator i = v.begin (); i != v.end (); i++)
    {
    if (!IsStringNumber (*i, true))
      return eInvalidPoint;
    points [iCurrent].x = (int) atol (i->c_str ());
    i++;  // we know this is safe becaue of earlier check (we must have pairs of numbers)
    if (!IsStringNumber (*i, true))
      return eInvalidPoint;
    points [iCurrent].y = (int) atol (i->c_str ());
    iCurrent++;  // onto next point
    }

  // Draw through the ISurface seam (docs/PAL.md S3c).
  pal::ISurface* srf = pal::g_surface;
  if (!srf) return eOK;

  srf->bezier (m_surfaceId, points.data (), (size_t) iCount,
               PenSpecFromWin32 (PenColour, PenStyle, PenWidth));

  return eOK;

  }   // end of CMiniWindow::Bezier


// draw a polygon (straight lines)
long CMiniWindow::Polygon(LPCTSTR Points,
                         long PenColour, short PenStyle, long PenWidth,
                         long BrushColour, long BrushStyle,
                         BOOL Close,
                         BOOL Winding)
  {

  if (ValidatePenStyle (PenStyle, PenWidth))
    return ePenStyleNotValid;

  pal::BrushSpec brush;
  if (BrushSpecFromWin32 (BrushStyle, PenColour, BrushColour, brush))
    return eBrushStyleNotValid;

  vector<string> v;

  StringToVector (Points, v, ",");

  int iCount = v.size ();

  // must have at least a start point (2 items), plus one extra (1 point)
  if (iCount < 4)
    return eInvalidNumberOfPoints;

  // it has to be divisible by 2
  if ((iCount % 2) != 0)
    return eInvalidNumberOfPoints;

  iCount = iCount / 2;  // number of points

  vector<pal::Point> points (iCount);

  int iCurrent = 0;

  for (vector<string>::const_iterator i = v.begin (); i != v.end (); i++)
    {
    if (!IsStringNumber (*i, true))
      return eInvalidPoint;
    points [iCurrent].x = (int) atol (i->c_str ());
    i++;  // we know this is safe becaue of earlier check (we must have pairs of numbers)
    if (!IsStringNumber (*i, true))
      return eInvalidPoint;
    points [iCurrent].y = (int) atol (i->c_str ());
    iCurrent++;  // onto next point
    }

  // Draw through the seam (docs/PAL.md S3c). Close = filled polygon (pen + brush,
  // the Winding/Alternate fill mode); open = polyline (pen only).
  pal::ISurface* srf = pal::g_surface;
  if (!srf) return eOK;

  const pal::PenSpec pen = PenSpecFromWin32 (PenColour, PenStyle, PenWidth);
  if (Close)
    srf->polygon (m_surfaceId, points.data (), (size_t) iCount, pen, brush,
                  Winding ? pal::PolyFill::Winding : pal::PolyFill::Alternate);
  else
    srf->polyline (m_surfaceId, points.data (), (size_t) iCount, pen);

  return eOK;

  }   // end of CMiniWindow::Polygon

// reposition window
long CMiniWindow::Position(long Left, long Top,
                           short Position,
                           long Flags)
  {
  m_Location.x           = Left     ;
  m_Location.y           = Top      ;
  m_iPosition            = Position ;
  m_iFlags               = Flags    ;

  return eOK;

  } // end of  CMiniWindow::Position


/*

  Cursor values:

  0:  arrow
  1:  hand
  2:  I-beam
  3:  + symbol
  4:  wait (hour-glass)
  5:  up arrow
  6:  arrow nw-se
  7:  arrow ne-sw
  8:  arrow e-w
  9:  arrow n-s
 10:  arrow - all ways
 11:  (X) no, no, I won't do that, but ...
 12:  help  (? symbol)


  Flags:

  0x01 : report all mouseovers, not just when first entering the hotspot

  */

// add a hotspot for handling mouse-over, mouse up/down events
long CMiniWindow::AddHotspot(CMUSHclientDoc * pDoc,
                             LPCTSTR HotspotId,
                             string sPluginID,
                             long Left, long Top, long Right, long Bottom,
                             LPCTSTR MouseOver,
                             LPCTSTR CancelMouseOver,
                             LPCTSTR MouseDown,
                             LPCTSTR CancelMouseDown,
                             LPCTSTR MouseUp,
                             LPCTSTR TooltipText,
                             long Cursor,
                             long Flags)
  {

  if (strlen (MouseOver) > 0 && CheckLabel (MouseOver, true))
    return eInvalidObjectLabel;
  if (strlen (CancelMouseOver) > 0 && CheckLabel (CancelMouseOver, true))
    return eInvalidObjectLabel;
  if (strlen (MouseDown) > 0 && CheckLabel (MouseDown, true))
    return eInvalidObjectLabel;
  if (strlen (CancelMouseDown) > 0 && CheckLabel (CancelMouseDown, true))
    return eInvalidObjectLabel;
  if (strlen (MouseUp) > 0 && CheckLabel (MouseUp, true))
    return eInvalidObjectLabel;

  // can't switch plugins here :)
  if (!m_sCallbackPlugin.empty () && m_sCallbackPlugin != sPluginID)
    return eHotspotPluginChanged;

  m_sCallbackPlugin = sPluginID;

  HotspotMapIterator it = m_Hotspots.find (HotspotId);

  if (it != m_Hotspots.end ())
    {
    delete it->second;         // delete existing hotspot
    m_Hotspots.erase (it);
    if (m_sMouseOverHotspot == HotspotId)
      m_sMouseOverHotspot.erase ();

    if (m_sMouseDownHotspot == HotspotId)
      m_sMouseDownHotspot.erase ();

    }

  CHotspot * pHotspot = new CHotspot;

  pHotspot->m_rect              = CRect (Left, Top, FixRight (Right), FixBottom (Bottom));
  pHotspot->m_sMouseOver        = MouseOver;
  pHotspot->m_sCancelMouseOver  = CancelMouseOver;
  pHotspot->m_sMouseDown        = MouseDown;
  pHotspot->m_sCancelMouseDown  = CancelMouseDown;
  pHotspot->m_sMouseUp          = MouseUp;
  pHotspot->m_sTooltipText      = TooltipText;
  pHotspot->m_Cursor            = Cursor;
  pHotspot->m_Flags             = Flags;

  // if not in a plugin, look in main world for hotspot callbacks, and remember the dispatch ID
  if (sPluginID.empty ())
    {
    CString strErrorMessage;

    pHotspot->m_dispid_MouseOver        = pDoc->GetProcedureDispid (MouseOver, "mouse over", "", strErrorMessage);
    pHotspot->m_dispid_CancelMouseOver  = pDoc->GetProcedureDispid (CancelMouseOver, "cancel mouse over", "", strErrorMessage);
    pHotspot->m_dispid_MouseDown        = pDoc->GetProcedureDispid (MouseDown, "mouse down", "", strErrorMessage);
    pHotspot->m_dispid_CancelMouseDown  = pDoc->GetProcedureDispid (CancelMouseDown, "cancel mouse down", "", strErrorMessage);
    pHotspot->m_dispid_MouseUp          = pDoc->GetProcedureDispid (MouseUp, "mouse up", "", strErrorMessage);
    }

  m_Hotspots [HotspotId] = pHotspot;

  return eOK;
  }    // end of CMiniWindow::AddHotspot

// remove a previously-installed hotspot
long CMiniWindow::DeleteHotspot(LPCTSTR HotspotId)
  {

  HotspotMapIterator it = m_Hotspots.find (HotspotId);

  if (it == m_Hotspots.end ())
    return eHotspotNotInstalled;   // no such hotspot

  delete it->second;

  m_Hotspots.erase (it);

  if (m_sMouseOverHotspot == HotspotId)
    m_sMouseOverHotspot.erase ();

  if (m_sMouseDownHotspot == HotspotId)
    m_sMouseDownHotspot.erase ();

  if (m_Hotspots.empty ())
    m_sCallbackPlugin.erase ();

  return eOK;
  }    // end of CMiniWindow::DeleteHotspot

// return list of all hotspots in this miniwindow
void CMiniWindow::HotspotList(VARIANT & vaResult)
  {
  COleSafeArray sa;   // for array list

  long iCount = 0;

  // put the arrays into the array
  if (!m_Hotspots.empty ())    // cannot create empty dimension
    {
    sa.CreateOneDim (VT_VARIANT, m_Hotspots.size ());

    for (HotspotMapIterator it = m_Hotspots.begin ();
         it != m_Hotspots.end ();
         it++)
           {
            // the array must be a bloody array of variants, or VBscript kicks up
            COleVariant v (it->first.c_str ());
            sa.PutElement (&iCount, &v);
            iCount++;
           }

    } // end of having at least one

  vaResult = sa.Detach ();

  }    // end of CMiniWindow::HotspotList

// delete all hotspots
long CMiniWindow::DeleteAllHotspots()
  {
  // delete our hotspots
  for (HotspotMapIterator hit = m_Hotspots.begin ();
       hit != m_Hotspots.end ();
       hit++)
         delete hit->second;

  m_Hotspots.clear ();
  m_sMouseOverHotspot.erase ();
  m_sMouseDownHotspot.erase ();
  m_sCallbackPlugin.erase ();
  return eOK;
  }    // end of CMiniWindow::DeleteAllHotspots

// get information about a hotspot
void CMiniWindow::HotspotInfo(LPCTSTR HotspotId, long InfoType, VARIANT & vaResult)
  {

  HotspotMapIterator it = m_Hotspots.find (HotspotId);

  if (it == m_Hotspots.end ())
    return;   // no such hotspot

  CHotspot * pHotspot = it->second;

  switch (InfoType)
    {

    case  1:  SetUpVariantLong   (vaResult, pHotspot->m_rect.left);                 break; // left
    case  2:  SetUpVariantLong   (vaResult, pHotspot->m_rect.top);                  break; // top
    case  3:  SetUpVariantLong   (vaResult, pHotspot->m_rect.right);                break; // right
    case  4:  SetUpVariantLong   (vaResult, pHotspot->m_rect.bottom);               break; // bottom
    case  5:  SetUpVariantString (vaResult, pHotspot->m_sMouseOver.c_str ());       break;
    case  6:  SetUpVariantString (vaResult, pHotspot->m_sCancelMouseOver.c_str ()); break;
    case  7:  SetUpVariantString (vaResult, pHotspot->m_sMouseDown.c_str ());       break;
    case  8:  SetUpVariantString (vaResult, pHotspot->m_sCancelMouseDown.c_str ()); break;
    case  9:  SetUpVariantString (vaResult, pHotspot->m_sMouseUp.c_str ());         break;
    case 10:  SetUpVariantString (vaResult, pHotspot->m_sTooltipText.c_str ());     break;
    case 11:  SetUpVariantLong   (vaResult, pHotspot->m_Cursor);                    break; // cursor code
    case 12:  SetUpVariantLong   (vaResult, pHotspot->m_Flags);                     break; // flags
    case 13:  SetUpVariantString (vaResult, pHotspot->m_sMoveCallback.c_str ());    break; // drag-and-drop move callback
    case 14:  SetUpVariantString (vaResult, pHotspot->m_sReleaseCallback.c_str ()); break; // drag-and-drop release callback
    case 15:  SetUpVariantLong   (vaResult, pHotspot->m_DragFlags);                 break; // drag-and-drop flags

    default:
      vaResult.vt = VT_NULL;
      break;

    } // end of switch

  }    // end of CMiniWindow::HotspotInfo


long CMiniWindow::ImageOp(short Action,
                          long Left, long Top, long Right, long Bottom,
                          long PenColour, long PenStyle, long PenWidth,
                          long BrushColour, LPCTSTR ImageId,
                          long EllipseWidth, long EllipseHeight)
  {

  ImageMapIterator it = m_Images.find (ImageId);

  if (it == m_Images.end ())
    return eImageNotInstalled;

  CBitmap * bitmap = it->second;

  if (ValidatePenStyle (PenStyle, PenWidth))
    return ePenStyleNotValid;

  pal::ISurface * srf = pal::g_surface;
  if (!srf)
    return eOK;

  pal::BrushSpec brush;
  brush.style = pal::BrushStyle::Pattern;

  pal::PixelBuffer pat;
  MonoImageMapIterator mit = m_MonoImages.find (ImageId);
  if (mit != m_MonoImages.end ())
    {
    // a CreateImage 8x8 mono bitmap: the original CreatePatternBrush kept it
    // MONO, so GDI coloured it — "1-bits will come out in pen colour, and
    // 0-bits will come out in background colour" via SetTextColor(BrushColour)
    // + SetBkColor(PenColour). Same carrier as the 5 built-in mono patterns,
    // so each adapter applies its own text/bk mapping.
    brush.monoPattern = mit->second.data ();
    brush.fg          = (pal::Colour) BrushColour;   // SetTextColor
    brush.bg          = (pal::Colour) PenColour;     // SetBkColor
    }
  else
    {
    // a colour image: GDI tiles the bitmap as-is (text/bk colours are ignored
    // for colour pattern brushes) — read it out and carry the pixels
    BITMAP bi;
    bitmap->GetBitmap (&bi);
    CDC bmDC;
    bmDC.CreateCompatibleDC (&(*pdc));
    CBitmap * pOldbmp = bmDC.SelectObject (bitmap);
    pal::SurfaceId sid = pal::g_adoptSurface
                       ? pal::g_adoptSurface (&bmDC, bi.bmWidth, bi.bmHeight)
                       : pal::SurfaceId::None;
    srf->readPixels (sid, { 0, 0, (int) bi.bmWidth, (int) bi.bmHeight },
                     pat, pal::PixelFormat::BGR24);
    if (pal::g_releaseAdoptedSurface)
      pal::g_releaseAdoptedSurface (sid);
    bmDC.SelectObject (pOldbmp);
    if (!pat.valid ())
      return eOK;
    brush.pattern = &pat;
    }

  long iResult = eUnknownOption;

  pal::ShapeKind kind;

  switch (Action)
    {
    case 1:  kind = pal::ShapeKind::Ellipse;    iResult = eOK; break;
    case 2:  kind = pal::ShapeKind::Rect;       iResult = eOK; break;
    case 3:  kind = pal::ShapeKind::RoundRect;  iResult = eOK; break;
    default: return iResult;
    } // end of switch

  const long R = FixRight (Right), B = FixBottom (Bottom);
  srf->shape (m_surfaceId, kind,
              { (int) Left, (int) Top, (int) (R - Left), (int) (B - Top) },
              { (int) EllipseWidth, (int) EllipseHeight },   // RoundRect corner
              { 0, 0 },
              PenSpecFromWin32 (PenColour, PenStyle, PenWidth),
              brush);

  return iResult;


  }   // end of  CMiniWindow::ImageOp

long CMiniWindow::CreateImage(LPCTSTR ImageId, long Row1, long Row2, long Row3, long Row4, long Row5, long Row6, long Row7, long Row8)
  {

  ImageMapIterator it = m_Images.find (ImageId);

  if (it != m_Images.end ())
    {
    delete it->second;         // delete existing image
    m_Images.erase (it);
    }

  CBitmap * pImage = new CBitmap;

  // bottom row comes first in a bitmap
  WORD    wBits[8] = { (WORD) Row8, (WORD) Row7, (WORD) Row6, (WORD) Row5,
                       (WORD) Row4, (WORD) Row3, (WORD) Row2, (WORD) Row1 };

  pImage->CreateBitmap (8, 8, 1, 1, wBits);

  m_Images [ImageId] = pImage;

  // remember the pattern so ImageOp can rebuild a REAL mono brush portably
  // (top scanline first, matching the wBits order CreateBitmap consumes)
  const unsigned char rows [8] = { (unsigned char) Row8, (unsigned char) Row7,
                                   (unsigned char) Row6, (unsigned char) Row5,
                                   (unsigned char) Row4, (unsigned char) Row3,
                                   (unsigned char) Row2, (unsigned char) Row1 };
  m_MonoImages [ImageId] = vector<unsigned char> (rows, rows + 8);

  return eOK;

  }  // end of CMiniWindow::CreateImage

// see: http://www.nathanm.com/photoshop-blending-math/
// and: http://www.pegtop.net/delphi/articles/blendmodes/

#define Blend_It(Op) \
   do \
      if (Opacity < 1.0) \
        for (i = 0; i < count; i++) \
          pB [i]     = Blend_Opacity (pA [i], pB [i], Op, Opacity);\
      else\
        for (i = 0; i < count; i ++)\
          pB [i]      = Op (pA [i], pB [i]);\
   while (false)

// we have to do this a row at a time, because there might be a filler at the end of each row
// and thus, a discontinuity which throws out the r/g/b sequence
#define Colour_Op(fR,fG,fB) \
   do \
     {  \
     for (row = 0; row < iHeight; row++)\
      {\
      long base = row * perline;\
      unsigned char rA, gA, bA, rB, gB, bB; \
      /* each mode reads only some channels */ \
      (void) rA; (void) gA; (void) bA; (void) rB; (void) gB; (void) bB; \
      if (Opacity < 1.0) \
        for (i = 0; i < perline - 2; ) \
          {\
          bA = pA [base + i];     \
          gA = pA [base + i + 1]; \
          rA = pA [base + i + 2]; \
          bB = pB [base + i];     \
          gB = pB [base + i + 1]; \
          rB = pB [base + i + 2]; \
          pB [base + i] = Simple_Opacity (bB, fB, Opacity); \
          i++;\
          pB [base + i] = Simple_Opacity (gB, fG, Opacity); \
          i++;\
          pB [base + i] = Simple_Opacity (rB, fR, Opacity); \
          i++;\
          }\
      else\
        for (i = 0; i < perline - 2;)\
          {\
          bA = pA [base + i];     \
          gA = pA [base + i + 1]; \
          rA = pA [base + i + 2]; \
          bB = pB [base + i];     \
          gB = pB [base + i + 1]; \
          rB = pB [base + i + 2]; \
          pB [base + i] = fB;  \
          i++;\
          pB [base + i] = fG;  \
          i++;\
          pB [base + i] = fR;  \
          i++;\
          }\
        }\
       }\
   while (false)



// see also: CMUSHclientDoc::BlendPixel

long CMiniWindow::BlendImage(LPCTSTR ImageId,
                  long Left, long Top, long Right, long Bottom,
                  short Mode, double Opacity,
                  long SrcLeft, long SrcTop, long SrcRight, long SrcBottom)
  {

  // constrain to what we actually have
  if (Left < 0)
    Left = 0;
  if (Top < 0)
    Top = 0;
  if (Right > m_iWidth)
    Right = m_iWidth;
  if (Bottom > m_iHeight)
    Bottom = m_iHeight;

  ImageMapIterator it = m_Images.find (ImageId);

  if (it == m_Images.end ())
    return eImageNotInstalled;

  if (Opacity < 0.0 || Opacity > 1.0)
    return eBadParameter;

  CBitmap * bitmap = it->second;

  BITMAP  bi;
  bitmap->GetBitmap(&bi);

  // calculate size of desired rectangle
  long iWidth = FixRight (Right) - Left;
  long iHeight = FixBottom (Bottom) - Top;

  // constrain to what we actually have
  if (SrcLeft < 0)
    SrcLeft = 0;
  if (SrcTop < 0)
    SrcTop = 0;
  if (SrcRight > bi.bmWidth )
    SrcRight = bi.bmWidth ;
  if (SrcBottom > bi.bmHeight)
    SrcBottom = bi.bmHeight;

  // adjust so that -1 means 1 from right
  if (SrcRight <= 0)
    SrcRight = bi.bmWidth + SrcRight;

  if (SrcBottom <= 0)
    SrcBottom =  bi.bmHeight + SrcBottom;

  // width and height are the smaller of the two rectangles
  iWidth = min (iWidth, SrcRight - SrcLeft);
  iHeight = min (iHeight, SrcBottom - SrcTop);

  if (iWidth <= 0 || iHeight <= 0)   // sanity check
    return eOK;

  pal::ISurface * srf = pal::g_surface;
  if (!srf)
    return eOK;

  // both layers are BGR24 region reads; the rectangles are pre-clamped in
  // bounds above, exactly as MergeImageAlpha

  // upper layer (from image id) — via a temporary adopt, as DrawImage
  pal::PixelBuffer bufA;
  {
  CDC bmDC;
  bmDC.CreateCompatibleDC(&(*pdc));
  CBitmap *pOldbmp = bmDC.SelectObject(bitmap);
  pal::SurfaceId sid = pal::g_adoptSurface
                     ? pal::g_adoptSurface (&bmDC, bi.bmWidth, bi.bmHeight)
                     : pal::SurfaceId::None;
  srf->readPixels (sid, { (int) SrcLeft, (int) SrcTop, (int) iWidth, (int) iHeight },
                   bufA, pal::PixelFormat::BGR24);
  if (pal::g_releaseAdoptedSurface)
    pal::g_releaseAdoptedSurface (sid);
  bmDC.SelectObject(pOldbmp);
  }

  // base image (from miniwindow)
  pal::PixelBuffer bufB;
  srf->readPixels (m_surfaceId,
                   { (int) Left, (int) Top, (int) iWidth, (int) iHeight },
                   bufB, pal::PixelFormat::BGR24);

  if (!bufA.valid () || !bufB.valid ()
      || bufA.bytes.size () != bufB.bytes.size ())
    return eOK;

  unsigned char * pA = bufA.bytes.data ();
  unsigned char * pB = bufB.bytes.data ();


  // manipulate image here   A = blend, B = base


  long count = (long) bufB.bytes.size ();
  long perline = bufB.stride;

  long i, row;

  // precompute cos table
  static unsigned char cos_table [256];
  static bool bTableComputed = false;

  if (!bTableComputed)
    {
    double pi_div255 = 3.1415926535898 / 255.0;
    for (i = 0; i < 256; i++)
      {
      double a = 64.0 - cos ((double) i * pi_div255) * 64.0;
      cos_table [i] = (uint8) (a + 0.5);  // round
      }
    bTableComputed = true;
    } // table needs computing

  switch (Mode)
    {

    // normal modes
    case  1: Blend_It (Blend_Normal);       break;
    case  2: Blend_It (Blend_Average);      break;
    case  3: Blend_It (Blend_Interpolate);  break;

     case  4:  // dissolve - randomly choose pixels based on opacity
       {

       for (row = 0; row < iHeight; row++)
        {
        long base = row * perline;
        unsigned char rA, gA, bA, rB, gB, bB;
        for (i = 0; i < perline - 2; )
          {
          double rnd = genrand ();
          bA = pA [base + i];
          gA = pA [base + i + 1];
          rA = pA [base + i + 2];

          bB = pB [base + i];
          gB = pB [base + i + 1];
          rB = pB [base + i + 2];

          pB [base + i] = (rnd < Opacity) ? bA : bB;;
          i++;
          pB [base + i] = (rnd < Opacity) ? gA : gB;
          i++;
          pB [base + i] = (rnd < Opacity) ? rA : rB;
          i++;

          }
         }  // end for each row

       }
      break;

    // darkening modes
    case  5: Blend_It (Blend_Darken);     break;
    case  6: Blend_It (Blend_Multiply);   break;
    case  7: Blend_It (Blend_ColorBurn);  break;
    case  8: Blend_It (Blend_LinearBurn); break;
    case  9: Blend_It (Blend_InverseColorBurn);  break;
    case 10: Blend_It (Blend_Subtract);   break;

    // lightening modes
    case 11: Blend_It (Blend_Lighten);    break;
    case 12: Blend_It (Blend_Screen);     break;
    case 13: Blend_It (Blend_ColorDodge); break;
    case 14: Blend_It (Blend_LinearDodge);break;
    case 15: Blend_It (Blend_InverseColorDodge);  break;
    case 16: Blend_It (Blend_Add);        break;

    // soft/hard light etc.
    case 17: Blend_It (Blend_Overlay);    break;
    case 18: Blend_It (Blend_SoftLight);  break;
    case 19: Blend_It (Blend_HardLight);  break;
    case 20: Blend_It (Blend_VividLight); break;
    case 21: Blend_It (Blend_LinearLight);break;
    case 22: Blend_It (Blend_PinLight);   break;
    case 23: Blend_It (Blend_HardMix);    break;

    // difference modes
    case 24: Blend_It (Blend_Difference); break;
    case 25: Blend_It (Blend_Exclusion);  break;

    // glow modes
    case 26: Blend_It (Blend_Reflect);    break;
    case 27: Blend_It (Blend_Glow);       break;
    case 28: Blend_It (Blend_Freeze);     break;
    case 29: Blend_It (Blend_Heat);       break;

    case 30: Blend_It (Blend_Negation);   break;
    case 31: Blend_It (Blend_Phoenix);    break;
    case 32: Blend_It (Blend_Stamp);      break;

    // logical modes
    case 33: Blend_It (Blend_Xor);      break;
    case 34: Blend_It (Blend_And);      break;
    case 35: Blend_It (Blend_Or);       break;

    // the follow modes take one colour from the blend and retain 2 from the base
    case 36: Colour_Op (rA, gB, bB); break;  // red
    case 37: Colour_Op (rB, gA, bB); break;  // green
    case 38: Colour_Op (rB, gB, bA); break;  // blue

    // the follow modes take two colours from the blend and retain 1 from the base
    case 39: Colour_Op (rA, gA, bB); break;  // yellow
    case 40: Colour_Op (rB, gA, bA); break;  // cyan
    case 41: Colour_Op (rA, gB, bA); break;  // magenta

    // limit green
    case 42: Colour_Op (rA, (gA > rA) ? rA : gA, bA); break;  // green limited by red
    case 43: Colour_Op (rA, (gA > bA) ? bA : gA, bA); break;  // green limited by blue
    case 44: Colour_Op (rA, (gA > ((rA + bA) / 2)) ? ((rA + bA) / 2) : gA, bA); break;  // green limited by average of red and blue

    // limit blue
    case 45: Colour_Op (rA, gA, (bA > rA) ? rA : bA); break;  // blue limited by red
    case 46: Colour_Op (rA, gA, (bA > gA) ? gA : bA); break;  // blue limited by green
    case 47: Colour_Op (rA, gA, (bA > ((rA + gA) / 2)) ? ((rA + gA) / 2) : bA); break; // blue limited by average of red and green

    // limit red
    case 48: Colour_Op ((rA > gA) ? gA : rA, gA, bA); break;  // red limited by green
    case 49: Colour_Op ((rA > bA) ? bA : rA, gA, bA); break;  // red limited by blue
    case 50: Colour_Op ((rA > ((gA + bA) / 2)) ? ((gA + bA) / 2) : rA, gA, bA); break;  // red limited by average of green and blue

    // select single colour
    case 51: Colour_Op (rA, 0, 0); break;  // red only       (looks red)
    case 52: Colour_Op (0, gA, 0); break;  // green only     (looks green)
    case 53: Colour_Op (0, 0, bA); break;  // blue only      (looks blue)

    // discard single colour
    case 54: Colour_Op (0, gA, bA); break;  // discard red    (looks cyan)
    case 55: Colour_Op (rA, 0, bA); break;  // discard green  (looks magenta)
    case 56: Colour_Op (rA, gA, 0); break;  // discard blue   (looks yellow)

    // one colour to all channels  (ie. it looks grey)
    case 57: Colour_Op (rA, rA, rA); break;  // all red
    case 58: Colour_Op (gA, gA, gA); break;  // all green
    case 59: Colour_Op (bA, bA, bA); break;  // all blue

    case 60:           // Hue mode
      {
       for (row = 0; row < iHeight; row++)
        {
        long base = row * perline;
        unsigned char rA, gA, bA, rB, gB, bB;
        for (i = 0; i < perline - 2; )
          {
          CColor cA, cB;    // A = blend, B = base
          bA = pA [base + i];
          gA = pA [base + i + 1];
          rA = pA [base + i + 2];
          bB = pB [base + i];
          gB = pB [base + i + 1];
          rB = pB [base + i + 2];

          cA.SetRGB (rA, gA, bA);
          cB.SetRGB (rB, gB, bB);

          cB.SetHue (cA.GetHue ());  // hue of blend image, others from base image
          pB [base + i] = Simple_Opacity (bB, cB.GetBlue (), Opacity);
          i++;
          pB [base + i] = Simple_Opacity (gB, cB.GetGreen (), Opacity);
          i++;
          pB [base + i] = Simple_Opacity (rB, cB.GetRed (), Opacity);
          i++;
          }
         }  // end for each row
       }
      break;

    case 61:           // Saturation mode
      {
       for (row = 0; row < iHeight; row++)
        {
        long base = row * perline;
        unsigned char rA, gA, bA, rB, gB, bB;
        for (i = 0; i < perline - 2; )
          {
          CColor cA, cB;    // A = blend, B = base
          bA = pA [base + i];
          gA = pA [base + i + 1];
          rA = pA [base + i + 2];
          bB = pB [base + i];
          gB = pB [base + i + 1];
          rB = pB [base + i + 2];

          cA.SetRGB (rA, gA, bA);
          cB.SetRGB (rB, gB, bB);

          cB.SetSaturation (cA.GetSaturation ());  // saturation of blend image, others from base image
          pB [base + i] = Simple_Opacity (bB, cB.GetBlue (), Opacity);
          i++;
          pB [base + i] = Simple_Opacity (gB, cB.GetGreen (), Opacity);
          i++;
          pB [base + i] = Simple_Opacity (rB, cB.GetRed (), Opacity);
          i++;
          }
         }  // end for each row
       }
      break;

    case 62:           // Colour mode
      {
       for (row = 0; row < iHeight; row++)
        {
        long base = row * perline;
        unsigned char rA, gA, bA, rB, gB, bB;
        for (i = 0; i < perline - 2; )
          {
          CColor cA, cB;    // A = blend, B = base
          bA = pA [base + i];
          gA = pA [base + i + 1];
          rA = pA [base + i + 2];
          bB = pB [base + i];
          gB = pB [base + i + 1];
          rB = pB [base + i + 2];

          cA.SetRGB (rA, gA, bA);
          cB.SetRGB (rB, gB, bB);

          cB.SetHue (cA.GetHue ());  // hue of blend image,
          cB.SetSaturation (cA.GetSaturation ());  // saturation of blend image, luminance from base image
          pB [base + i] = Simple_Opacity (bB, cB.GetBlue (), Opacity);
          i++;
          pB [base + i] = Simple_Opacity (gB, cB.GetGreen (), Opacity);
          i++;
          pB [base + i] = Simple_Opacity (rB, cB.GetRed (), Opacity);
          i++;
          }
         }  // end for each row
       }
      break;

      /*

    Note: I don't get the same results here as using Photoshop on the same images. Not quite sure why,
    some test code below seems to indicate that for all possible values of RGB, they map to HSL and back
    again without error.

    In the absence of any obvious bug, I am assuming that by plugging the Luminance from one colour into the
    L channel of another, I am creating an out-of-gamut colour, which then cannot be represented properly.

    I can't find, so far, any code that would limit the value to be in-gamut, so for now this Luminance (and
    Colour, Saturation and Hue too, probably) should be regarded as experimental.  - NJG - 2/Aug/2008.

    */

    case 63:           // Luminance mode
      {
       for (row = 0; row < iHeight; row++)
        {
        long base = row * perline;
        unsigned char rA, gA, bA, rB, gB, bB;
        for (i = 0; i < perline - 2; )
          {
          CColor cA, cB;    // A = blend, B = base
          bA = pA [base + i];
          gA = pA [base + i + 1];
          rA = pA [base + i + 2];
          bB = pB [base + i];
          gB = pB [base + i + 1];
          rB = pB [base + i + 2];

          // bit of a fudge here, but what can you do?

          // this is going to brighten the really dark pixels, however at least then they
          // should retain their base colour, rather than be promoted up to something garish

//          if (rB < 12) rB = 12;
//          if (gB < 12) gB = 12;
//          if (bB < 12) bB = 12;

          cA.SetRGB (rA, gA, bA);
          cB.SetRGB (rB, gB, bB);

          cB.SetLuminance (cA.GetLuminance ());  // luminance from blend image, others from base image

          pB [base + i] = Simple_Opacity (bB, cB.GetBlue (), Opacity);
          i++;
          pB [base + i] = Simple_Opacity (gB, cB.GetGreen (), Opacity);
          i++;
          pB [base + i] = Simple_Opacity (rB, cB.GetRed (), Opacity);
          i++;

          }
         }  // end for each row
       }
      break;

    case 64:           // HSL (hue to red, saturation to green, luminance to blue)
      {
       for (row = 0; row < iHeight; row++)
        {
        long base = row * perline;
        unsigned char rA, gA, bA, rB, gB, bB;
        for (i = 0; i < perline - 2; )
          {
          CColor cA, cB;    // A = blend, B = base
          bA = pA [base + i];
          gA = pA [base + i + 1];
          rA = pA [base + i + 2];
          bB = pB [base + i];
          gB = pB [base + i + 1];
          rB = pB [base + i + 2];

          cA.SetRGB (rA, gA, bA);

          pB [base + i] = Simple_Opacity (bB, cA.GetLuminance () * 255, Opacity);
          i++;
          pB [base + i] = Simple_Opacity (gB, cA.GetSaturation () * 255, Opacity);
          i++;
          pB [base + i] = Simple_Opacity (rB, (cA.GetHue () / 360.0) * 255, Opacity);
          i++;

          }
         }  // end for each row
       }
      break;

      /*
    case 65: // testing  conversion to/from RGB to HSL and back again is consistent.
      {
      int r, g, b;
      CColor cA;

      for (r = 0; r < 256; r ++)
        for (g = 0; g < 256; g ++)
          for (b = 0; b < 256; b ++)
            {
            cA.SetRGB (r, g, b);
            cA.GetLuminance ();  // force change to HSL
            ASSERT (r == cA.GetRed ());
            ASSERT (g == cA.GetGreen ());
            ASSERT (b == cA.GetBlue ());

            }


      }
    break;

      */

    default: return eUnknownOption;
    }

  // copy result back

  srf->writePixels (m_surfaceId,
                    { (int) Left, (int) Top, (int) iWidth, (int) iHeight }, bufB);

  return eOK;



  }   // end of CMiniWindow::BlendImage


// copies one miniwindow image as a stand-alone image
long CMiniWindow::ImageFromWindow(LPCTSTR ImageId, CMiniWindow * pSrcWindow)
  {


  ImageMapIterator it = m_Images.find (ImageId);

  if (it != m_Images.end ())
    {
    delete it->second;         // delete existing image
    m_Images.erase (it);
    }

  pal::ISurface * srf = pal::g_surface;
  if (!srf)
    return eOK;

  // read the source window's pixels — BGRA32 with alpha pinned 0, exactly the
  // bytes the old screen-compatible-bitmap BitBlt copy carried (GDI-drawn
  // surfaces leave 0 in the high byte)
  pal::PixelBuffer buf;
  srf->readPixels (pSrcWindow->m_surfaceId,
                   { 0, 0, (int) pSrcWindow->m_iWidth, (int) pSrcWindow->m_iHeight },
                   buf, pal::PixelFormat::BGRA32);
  if (!buf.valid ())
    return eOK;

  // materialize as a stand-alone 32bpp bitmap the way the image loaders do
  // (CreateDIBSection + Attach). On Windows this stores a 32bpp DIBSection
  // where a screen-depth DDB was stored before — identical bytes, identical
  // behaviour in every image op on a 32-bit display.
  BITMAPINFO bmi;
  ZeroMemory (&bmi, sizeof bmi);
  bmi.bmiHeader.biSize = sizeof bmi;
  bmi.bmiHeader.biWidth =          buf.size.w;
  bmi.bmiHeader.biHeight =         buf.size.h;
  bmi.bmiHeader.biPlanes =         1;
  bmi.bmiHeader.biBitCount =       32;
  bmi.bmiHeader.biCompression =    BI_RGB;
  bmi.bmiHeader.biSizeImage =      buf.size.h * BytesPerLine (buf.size.w, 32);

  if ((size_t) bmi.bmiHeader.biSizeImage > buf.bytes.size ())
    return eOK;   // cannot happen: BGRA32 stride is exactly BytesPerLine(w, 32)

  unsigned char * pBits = NULL;
  HBITMAP hbm = CreateDIBSection (NULL, &bmi, DIB_RGB_COLORS, (void**) &pBits, NULL, 0);
  if (!hbm || !pBits)
    return eOK;

  memcpy (pBits, buf.bytes.data (), bmi.bmiHeader.biSizeImage);

  CBitmap * pImage = new CBitmap;
  pImage->Attach ((HGDIOBJ) hbm);

  // save in map
  m_MonoImages.erase (ImageId);   // stale CreateImage pattern, if any
  m_Images [ImageId] = pImage;
  return eOK;

  } // end of CMiniWindow::ImageFromWindow


static void MakeTexture (const COLORREF Multiplier,
                         unsigned char * pBuffer,
                         const long iWidth,
                         const long iHeight)
  {

  long   rval = GetRValue (Multiplier),
         gval = GetGValue (Multiplier),
         bval = GetBValue (Multiplier);

  long row, col;
  long c;

  int increment =  BytesPerLine (iWidth, 24);

   // main loop is columns
   for (col = 0; col < iWidth; col++)
     {
     unsigned char * p = pBuffer + col * 3;
     for (row = 0; row < iHeight; row++)
       {
       c = col ^ row;
       p [0] = c * bval;
       p [1] = c * gval;
       p [2] = c * rval;
       p += increment;
       }  // end of each row
     }  // end of each column


  }  // end of MakeTexture


long CMiniWindow::Gradient(long Left, long Top, long Right, long Bottom,
                      long StartColour, long EndColour,
                      short Mode)
  {
  // calculate size of desired rectangle
  long iWidth = FixRight (Right) - Left;
  long iHeight = FixBottom (Bottom) - Top;

  if (iWidth <= 0 || iHeight <= 0)   // sanity check
    return eOK;

  pal::ISurface* srf = pal::g_surface;
  if (!srf) return eOK;

  // Mode 3 = texture fill (docs/PAL.md S3d): MakeTexture (portable noise) into a
  // BGR24 buffer, then writePixels — exactly the old CreateDIBSection + MakeTexture
  // + BitBlt (SRCCOPY). The buffer is bottom-up like the old positive-height DIB,
  // so the texture orientation is unchanged.
  if (Mode == 3)
    {
    pal::PixelBuffer tex;
    tex.format = pal::PixelFormat::BGR24;
    tex.size   = { (int) iWidth, (int) iHeight };
    tex.stride = pal::PixelBuffer::strideFor ((int) iWidth, pal::PixelFormat::BGR24);
    tex.bytes.resize ((size_t) tex.stride * iHeight);
    MakeTexture (StartColour, tex.bytes.data (), iWidth, iHeight);
    srf->writePixels (m_surfaceId, { (int) Left, (int) Top, (int) iWidth, (int) iHeight }, tex);
    return eOK;
    }

  // Modes 1 (horizontal) / 2 (vertical): a linear two-colour gradient; anything
  // else is eUnknownOption, as before. The GDI adapter fills via GradientFill (the
  // old Windows-2000+ path, byte-exact); the wx adapter uses its exact-endpoint strips.
  if (Mode != 1 && Mode != 2)
    return eUnknownOption;

  srf->gradient (m_surfaceId, { (int) Left, (int) Top, (int) iWidth, (int) iHeight },
                 (pal::Colour) StartColour, (pal::Colour) EndColour, Mode == 1);
  return eOK;

  } // end of CMiniWindow::Gradient

static void GeneralFilter (unsigned char * inbuf, long iWidth, long iHeight,
                            const long iPerLine,
                            const double Options,
                            const double * iMatrix,
                            const double iDivisor)
  {

  unsigned char * pi;
  long total;

  long row, col, rgb;
  int i;
  int iLastByte = iWidth * 3;
  long window [5];

  if (Options != 2)    // 2 = vertical only
    {
    // horizontal
    // main loop is rows
    for (row = 0; row < iHeight; row++)
     {

     pi = inbuf + iPerLine * row;


     for (rgb = 0; rgb < 3; rgb++)
       {

       // preload 4 slots in window for edge case  (-2 to +1)
       for (col = 0; col < 4; col++)
          window [col + 1] = pi [min (max (rgb + (col * 3) - 6, 0), iLastByte - 1)];

       // Every pixel column, including the last.  The bound used to be
       // `iLastByte - 4`, which stops at the largest multiple of 3 strictly
       // below 3*iWidth - 4 -- that is pixel iWidth - 2, so the RIGHTMOST pixel
       // column was read (as a neighbour) but never written, and stayed
       // unfiltered.  Writing it is in range (col + rgb <= iLastByte - 1) and
       // the look-ahead read is already clamped to iLastByte - 1 below, which is
       // the same edge clamp the last two columns have always used.
       for (col = 0; col < iLastByte; col += 3)
         {
         // we add 3 to get to the next pixel in that colour (R, G or B)

         // insert right-most byte into window

         window [0] = window [1];
         window [1] = window [2];
         window [2] = window [3];
         window [3] = window [4];
         window [4] = pi [min (max (col + rgb + 6, 0), iLastByte - 1)];

         total = 0;

         for (i = 0; i < 5; i++)
            total +=  window [i] * iMatrix [i];

         pi [col + rgb] = (uint8) (min (max (total / iDivisor, 0), 255));
         }   // end of each column
       }     // end of each colour

     }  // end of each row
    }   // horizontal wanted

  if (Options != 1)    // 1 = horizontal only
  {

  // vertical
    // main loop is columns
    for (col = 0; col < iLastByte; col++)
      {

       // preload 4 slots in window for edge case  (-2 to +1)
       for (row = 0; row < 4; row++)
         {
         long from = min (max (row - 2, 0), iHeight - 1);
         pi = inbuf + col + (from * iPerLine);
         window [row + 1] = *pi;
         }

      for (row = 0; row < iHeight; row++)
       {

       // Look TWO rows ahead, matching the horizontal pass (which reads
       // col + rgb + 6, i.e. +2 pixels) and the +1 the preload above leaves in
       // window [4].  This used to read row + 3: after the shift the window then
       // held rows -1..+3 rather than -2..+2, so the kernel was centred one row
       // off the row it writes, and row 2 of the region was never sampled at all
       // (the preload covers rows -2..+1 -> 0,0,0,1 and the loop then started at
       // 3).  The clamp keeps the last two rows reading the bottom row, as before.
       long from = min (max (row + 2, 0), iHeight - 1);
       pi = inbuf + col + (from * iPerLine);

       // insert right-most byte into window

       window [0] = window [1];
       window [1] = window [2];
       window [2] = window [3];
       window [3] = window [4];
       window [4] = *pi;

       total = 0;

       for (i = 0; i < 5; i++)
          total +=  window [i] * iMatrix [i];

       pi = inbuf + col + (row * iPerLine);
       pi [0] = (uint8) (min (max (total / iDivisor, 0), 255));

       }  // end of each row
      }  // end of each column
    }  // vertical wanted

  } // end of Blur


static void Noise (unsigned char * inbuf, long iWidth, long iHeight, long iPerLine, double Options)
  {

  unsigned char * pi = inbuf;
  long count = iPerLine * iHeight;
  long i, c;
  double threshold = Options / 100.0;
  for (i = 0; i < count; i++)
    {
    c = *pi;
    c += (128 - genrand () * 256) * threshold;
    *pi++  = CLAMP (c);
    }

  } // end of Noise

static void MonoNoise (unsigned char * inbuf, long iWidth, long iHeight, long iPerLine, double Options)
  {

  long i, j, c, row;
  double threshold = Options / 100.0;


  for (row = 0; row < iHeight; row++)
    {
    unsigned char * pi = inbuf + (row * iPerLine);
    for (i = 0; i < iWidth; i ++)
      {
      j = (128 - genrand () * 256) * threshold;

      c = *pi + j;
      *pi++  = CLAMP (c);
      c = *pi + j;
      *pi++  = CLAMP (c);
      c = *pi + j;
      *pi++  = CLAMP (c);
      }
     }  // end for each row


  } // end of MonoNoise


static void Brightness (unsigned char * inbuf, long iWidth, long iHeight, long iPerLine, double Options)
  {

  unsigned char * pi = inbuf;
  long count = iPerLine * iHeight;
  long i, c;
  for (i = 0; i < count; i++)
    {
    c = *pi;
    c += Options;
    *pi++  = CLAMP (c);
    }

  } // end of Brightness


static void BrightnessMultiply (unsigned char * inbuf, long iWidth, long iHeight, long iPerLine, double Options)
  {

  unsigned char * pi = inbuf;
  long count = iPerLine * iHeight;
  long i, c;
  for (i = 0; i < count; i++)
    {
    c = *pi;
    c *= Options;
    *pi++  = CLAMP (c);
    }

  } // end of BrightnessMultiply

static void Contrast (unsigned char * inbuf, long iWidth, long iHeight, long iPerLine, double Options)
  {

  unsigned char lookup [256];  // lookup table for speed

  unsigned char * pi = inbuf;
  long count = iPerLine * iHeight;
  long i;
  double c;

  for (i = 0; i < 256; i++)
    {
    c = i - 128;    // center on zero
    c *= Options;  // multiply by contrast
    c += 128;  // put back
    lookup [i] = CLAMP (c);
    }  // end of for loop

  // now convert image using lookup table
  for (i = 0; i < count; i++, pi++)
    *pi = lookup [*pi];

  } // end of Contrast

static void Gamma (unsigned char * inbuf, long iWidth, long iHeight, long iPerLine, double Options)
  {

  unsigned char lookup [256];  // lookup table for speed

  unsigned char * pi = inbuf;
  long count = iPerLine * iHeight;
  long i;
  double c;
  if (Options < 0.0)
    Options = 0.0;

  for (i = 0; i < 256; i++)
    {
    c = ( (double) i) / 255.0;  // normalize it
    c = pow (c, Options);
    c *= 255;
    lookup [i] = CLAMP (c);
    }  // end of for loop

  // now convert image using lookup table
  for (i = 0; i < count; i++, pi++)
    *pi = lookup [*pi];

  } // end of Gamma


static void ColourBrightness (unsigned char * inbuf, long iWidth, long iHeight,
                              long iPerLine, double Options, long iColour)
  {

  long i, row, c;

  for (row = 0; row < iHeight; row++)
    {
    unsigned char * pi = inbuf + (row * iPerLine) + iColour;
    for (i = 0; i < iWidth; i ++)
      {
      c = *pi;
      c += Options;
      *pi  = CLAMP (c);
      pi += 3;
      }
     }  // end for each row

  } // end of ColourBrightness


static void ColourBrightnessMultiply (unsigned char * inbuf, long iWidth, long iHeight,
                              long iPerLine, double Options, long iColour)
  {

  long i, row, c;

  for (row = 0; row < iHeight; row++)
    {
    unsigned char * pi = inbuf + (row * iPerLine) + iColour;
    for (i = 0; i < iWidth; i ++)
      {
      c = *pi;
      c *= Options;
      *pi  = CLAMP (c);
      pi += 3;
      }
     }  // end for each row

  } // end of ColourBrightnessMultiply


static void ColourContrast (unsigned char * inbuf, long iWidth, long iHeight,
                            long iPerLine, double Options, long iColour)
  {

  unsigned char lookup [256];  // lookup table for speed

  long i, row;
  double c;

  for (i = 0; i < 256; i++)
    {
    c = i - 128;    // center on zero
    c *= Options;  // multiply by contrast
    c += 128;  // put back
    lookup [i] = CLAMP (c);
    }  // end of for loop

  // now convert image using lookup table
  for (row = 0; row < iHeight; row++)
    {
    unsigned char * pi = inbuf + (row * iPerLine) + iColour;
    for (i = 0; i < iWidth; i ++)
      {
      *pi = lookup [*pi];
      pi += 3;
      }
     }  // end for each row

    } // end of ColourContrast

static void ColourGamma (unsigned char * inbuf, long iWidth, long iHeight,
                         long iPerLine, double Options, long iColour)
  {

  unsigned char lookup [256];  // lookup table for speed

  long i, row;
  double c;

  if (Options < 0.0)
    Options = 0.0;

  for (i = 0; i < 256; i++)
    {
    c = ( (double) i) / 255.0;  // normalize it
    c = pow (c, Options);
    c *= 255;
    lookup [i] = CLAMP (c);
    }  // end of for loop

  // now convert image using lookup table
  for (row = 0; row < iHeight; row++)
    {
    unsigned char * pi = inbuf + (row * iPerLine) + iColour;
    for (i = 0; i < iWidth; i ++)
      {
      *pi = lookup [*pi];
      pi += 3;
      }
     }  // end for each row

  } // end of ColourGamma


// see: http://en.wikipedia.org/wiki/Grayscale

static void MakeGreyscale (unsigned char * inbuf, long iWidth, long iHeight, long iPerLine, double Options, const bool bLinear)
  {

  long i, row;
  double c;


  for (row = 0; row < iHeight; row++)
    {
    unsigned char * pi = inbuf + (row * iPerLine);
    for (i = 0; i < iWidth; i ++)
      {

      if (bLinear)
        {
        c =  pi [0] + pi [1] + pi [2];
        c /= 3;
        }
      else
        c = pi [0] * 0.11 + // blue   (perceptual)
            pi [1] * 0.59 + // green
            pi [2] * 0.30;  // red

      *pi++  = CLAMP (c);
      *pi++  = CLAMP (c);
      *pi++  = CLAMP (c);
      }
     }  // end for each row


  } // end of MakeGreyscale


static void Average (unsigned char * pBuffer,
                      const long iWidth,
                      const long iHeight)
  {

  long row, col;
  __int64 r = 0, g = 0, b = 0, count = 0;

  int increment =  BytesPerLine (iWidth, 24);

  // find average

  // main loop is columns
  for (col = 0; col < iWidth; col++)
   {
   unsigned char * p = pBuffer + col * 3;
   for (row = 0; row < iHeight; row++)
     {
     b += p [0];
     g += p [1];
     r += p [2];
     count++;
     p += increment;
     }  // end of each row
   }  // end of each column

  r /= count;
  g /= count;
  b /= count;

  // apply average

  // main loop is columns
  for (col = 0; col < iWidth; col++)
   {
   unsigned char * p = pBuffer + col * 3;
   for (row = 0; row < iHeight; row++)
     {
     p [0] = CLAMP (b);
     p [1] = CLAMP (g);
     p [2] = CLAMP (r);
     p += increment;
     }  // end of each row
   }  // end of each column

  }  // end of Average


// see also:  CMUSHclientDoc::FilterPixel
long CMiniWindow::Filter(long Left, long Top, long Right, long Bottom,
                  short Operation, double Options)
  {

  // constrain to what we actually have
  if (Left < 0)
    Left = 0;
  if (Top < 0)
    Top = 0;
  if (Right > m_iWidth)
    Right = m_iWidth;
  if (Bottom > m_iHeight)
    Bottom = m_iHeight;

  // calculate size of desired rectangle
  long iWidth = FixRight (Right) - Left;
  long iHeight = FixBottom (Bottom) - Top;

  if (iWidth < 1 || iHeight < 1)   // sanity check
    return eOK;

  // Read the window region, filter it (the exact pixel-math functions, unchanged),
  // and write it back through the seam (docs/PAL.md S3d) -- == the old
  // CreateDIBSection + BitBlt-in + filter + BitBlt-out. BGR24, bottom-up: the
  // PixelBuffer has the same layout + stride (== BytesPerLine(iWidth,24)) as the
  // old DIB, so the filters run byte-for-byte.
  pal::ISurface* srf = pal::g_surface;
  if (!srf) return eOK;

  pal::PixelBuffer buf;
  srf->readPixels (m_surfaceId, { (int) Left, (int) Top, (int) iWidth, (int) iHeight },
                   buf, pal::PixelFormat::BGR24);
  if (!buf.valid ())
    return eOK;

  unsigned char * pA = buf.bytes.data ();

  // manipulate image here

   switch (Operation)
     {
     case 1: Noise    (pA, iWidth, iHeight, BytesPerLine (iWidth, 24), Options); break;
     case 2: MonoNoise(pA, iWidth, iHeight, BytesPerLine (iWidth, 24), Options); break;
     case 3: // blur
       {
       double iMatrix [5] = { 1, 1, 1, 1, 1 };
       GeneralFilter     (pA, iWidth, iHeight, BytesPerLine (iWidth, 24), Options, iMatrix, 5);
       }
       break;
     case 4:  // sharpen
       {
       double iMatrix [5] = { -1, -1, 7, -1, -1 };
       GeneralFilter     (pA, iWidth, iHeight, BytesPerLine (iWidth, 24), Options, iMatrix, 3);
       }
       break;
     case 5:  // edge detect
       {
       double iMatrix [5] = { 0, 2.5, -6, 2.5, 0 };
       GeneralFilter     (pA, iWidth, iHeight, BytesPerLine (iWidth, 24), Options, iMatrix, 1);
       }
       break;
     case 6:  // emboss
       {
       double iMatrix [5] = { 1, 2, 1, -1, -2 };
       GeneralFilter     (pA, iWidth, iHeight, BytesPerLine (iWidth, 24), Options, iMatrix, 1);
       }
       break;
     case  7: Brightness        (pA, iWidth, iHeight, BytesPerLine (iWidth, 24), Options); break;      // additive brightness
     case  8: Contrast          (pA, iWidth, iHeight, BytesPerLine (iWidth, 24), Options); break;
     case  9: Gamma             (pA, iWidth, iHeight, BytesPerLine (iWidth, 24), Options); break;
     case 10: ColourBrightness  (pA, iWidth, iHeight, BytesPerLine (iWidth, 24), Options, 2); break;    // red
     case 11: ColourContrast    (pA, iWidth, iHeight, BytesPerLine (iWidth, 24), Options, 2); break;    // red
     case 12: ColourGamma       (pA, iWidth, iHeight, BytesPerLine (iWidth, 24), Options, 2); break;    // red
     case 13: ColourBrightness  (pA, iWidth, iHeight, BytesPerLine (iWidth, 24), Options, 1); break;    // green
     case 14: ColourContrast    (pA, iWidth, iHeight, BytesPerLine (iWidth, 24), Options, 1); break;    // green
     case 15: ColourGamma       (pA, iWidth, iHeight, BytesPerLine (iWidth, 24), Options, 1); break;    // green
     case 16: ColourBrightness  (pA, iWidth, iHeight, BytesPerLine (iWidth, 24), Options, 0); break;    // blue
     case 17: ColourContrast    (pA, iWidth, iHeight, BytesPerLine (iWidth, 24), Options, 0); break;    // blue
     case 18: ColourGamma       (pA, iWidth, iHeight, BytesPerLine (iWidth, 24), Options, 0); break;    // blue
     case 19: MakeGreyscale     (pA, iWidth, iHeight, BytesPerLine (iWidth, 24), Options, true); break;    // linear
     case 20: MakeGreyscale     (pA, iWidth, iHeight, BytesPerLine (iWidth, 24), Options, false); break;    // perceptual
     case 21: BrightnessMultiply(pA, iWidth, iHeight, BytesPerLine (iWidth, 24), Options); break;          // multiplicative brightness
     case 22: ColourBrightnessMultiply(pA, iWidth, iHeight, BytesPerLine (iWidth, 24), Options, 2); break;   // red
     case 23: ColourBrightnessMultiply(pA, iWidth, iHeight, BytesPerLine (iWidth, 24), Options, 1); break;   // green
     case 24: ColourBrightnessMultiply(pA, iWidth, iHeight, BytesPerLine (iWidth, 24), Options, 0); break;   // blue
     case 25: // lesser blur
       {
       double iMatrix [5] = { 0, 1, 1, 1, 0 };
       GeneralFilter     (pA, iWidth, iHeight, BytesPerLine (iWidth, 24), Options, iMatrix, 3);
       }
       break;
     case 26: // minor blur
       {
       double iMatrix [5] = { 0, 0.5, 1, 0.5, 0 };
       GeneralFilter     (pA, iWidth, iHeight, BytesPerLine (iWidth, 24), Options, iMatrix, 2);
       }
       break;
     case 27: Average             (pA, iWidth, iHeight); break;



     default: return eUnknownOption;

     }

  // copy result back
  srf->writePixels (m_surfaceId, { (int) Left, (int) Top, (int) iWidth, (int) iHeight }, buf);

  return eOK;

  } // end of CMiniWindow::Filter



long CMiniWindow::SetPixel(long x, long y, long Colour)
  {
  if (pal::g_surface)
    pal::g_surface->setPixel (m_surfaceId, { (int) x, (int) y }, (pal::Colour) Colour);
  return eOK;
  } // end of CMiniWindow::SetPixel


long CMiniWindow::SetZOrder(long Order)
  {
  m_ZOrder = Order;
  return eOK;
  }

long CMiniWindow::GetPixel(long x, long y)
  {
  // getPixel returns 0xFFFFFFFF (CLR_INVALID) out of bounds.  Windows returns
  // that through a 32-bit long, so a script sees -1 -- which is what
  // WindowGetPixel is documented to give for a pixel outside the window.  A
  // straight widening cast here yields 4294967295 instead, because long is
  // 64-bit on LP64, so `if WindowGetPixel(...) == -1` silently never matched.
  // Fold it at the doc boundary; the surface layer keeps returning 0xFFFFFFFF.
  if (!pal::g_surface)
    return -1;
  const pal::Colour c = pal::g_surface->getPixel (m_surfaceId, { (int) x, (int) y });
  return (c == 0xFFFFFFFFu) ? -1L : (long) c;
  } // end of CMiniWindow::GetPixel


long CMiniWindow::MergeImageAlpha(LPCTSTR ImageId, LPCTSTR MaskId,
                                  long Left, long Top, long Right, long Bottom,
                                  short Mode, double Opacity,
                                  long SrcLeft, long SrcTop, long SrcRight, long SrcBottom)
  {

  // constrain to what we actually have
  if (Left < 0)
    Left = 0;
  if (Top < 0)
    Top = 0;
  if (Right > m_iWidth)
    Right = m_iWidth;
  if (Bottom > m_iHeight)
    Bottom = m_iHeight;

  if (Opacity < 0.0 || Opacity > 1.0)
    return eBadParameter;

  // image to be merged
  ImageMapIterator it = m_Images.find (ImageId);

  if (it == m_Images.end ())
    return eImageNotInstalled;

  CBitmap * bitmap = it->second;

  // mask image
  it = m_Images.find (MaskId);

  if (it == m_Images.end ())
    return eImageNotInstalled;

  CBitmap * mask_bitmap = it->second;

  BITMAP  bi;
  bitmap->GetBitmap(&bi);

  // calculate size of desired rectangle
  long iWidth = FixRight (Right) - Left;
  long iHeight = FixBottom (Bottom) - Top;

  // constrain to what we actually have
  if (SrcLeft < 0)
    SrcLeft = 0;
  if (SrcTop < 0)
    SrcTop = 0;
  if (SrcRight > bi.bmWidth )
    SrcRight = bi.bmWidth ;
  if (SrcBottom > bi.bmHeight)
    SrcBottom = bi.bmHeight;

  // adjust so that -1 means 1 from right
  if (SrcRight <= 0)
    SrcRight = bi.bmWidth + SrcRight;

  if (SrcBottom <= 0)
    SrcBottom =  bi.bmHeight + SrcBottom;

  // width and height are the smaller of the two rectangles
  iWidth = min (iWidth, SrcRight - SrcLeft);
  iHeight = min (iHeight, SrcBottom - SrcTop);

  if (iWidth <= 0 || iHeight <= 0)   // sanity check
    return eOK;

  mask_bitmap->GetBitmap(&bi);

  // adjust down in case we don't use whole image
  SrcRight = SrcLeft + iWidth;
  SrcBottom = SrcTop + iHeight;

  // mask must at least be as big as the image we are merging
  if (bi.bmWidth < SrcRight)
    return eBadParameter;
  if (bi.bmHeight < SrcBottom)
    return eBadParameter;


  pal::ISurface * srf = pal::g_surface;
  if (!srf)
    return eOK;

  // all three layers are BGR24 region reads; every rectangle is pre-clamped
  // in bounds above, so no zero-fill is needed (unlike DrawImageAlpha)
  CDC bmDC;   // for loading bitmaps into
  bmDC.CreateCompatibleDC(&(*pdc));

  // merge layer (from image id) — via a temporary adopt, as DrawImage
  pal::PixelBuffer bufA;
  {
  BITMAP biImg;
  bitmap->GetBitmap (&biImg);
  CBitmap * pOldbmp = bmDC.SelectObject (bitmap);
  pal::SurfaceId sid = pal::g_adoptSurface
                     ? pal::g_adoptSurface (&bmDC, biImg.bmWidth, biImg.bmHeight)
                     : pal::SurfaceId::None;
  srf->readPixels (sid, { (int) SrcLeft, (int) SrcTop, (int) iWidth, (int) iHeight },
                   bufA, pal::PixelFormat::BGR24);
  if (pal::g_releaseAdoptedSurface)
    pal::g_releaseAdoptedSurface (sid);
  bmDC.SelectObject (pOldbmp);
  }

  // mask image — the same source rectangle (bi still holds the mask's dims)
  pal::PixelBuffer bufM;
  {
  CBitmap * pOldbmp = bmDC.SelectObject (mask_bitmap);
  pal::SurfaceId sid = pal::g_adoptSurface
                     ? pal::g_adoptSurface (&bmDC, bi.bmWidth, bi.bmHeight)
                     : pal::SurfaceId::None;
  srf->readPixels (sid, { (int) SrcLeft, (int) SrcTop, (int) iWidth, (int) iHeight },
                   bufM, pal::PixelFormat::BGR24);
  if (pal::g_releaseAdoptedSurface)
    pal::g_releaseAdoptedSurface (sid);
  bmDC.SelectObject (pOldbmp);
  }

  // base image (from miniwindow)
  pal::PixelBuffer bufB;
  srf->readPixels (m_surfaceId,
                   { (int) Left, (int) Top, (int) iWidth, (int) iHeight },
                   bufB, pal::PixelFormat::BGR24);

  if (!bufA.valid () || !bufB.valid () || !bufM.valid ()
      || bufA.bytes.size () != bufB.bytes.size ()
      || bufM.bytes.size () != bufB.bytes.size ())
    return eOK;

  unsigned char * pA = bufA.bytes.data ();
  unsigned char * pB = bufB.bytes.data ();
  unsigned char * pM = bufM.bytes.data ();

  long count = (long) bufB.bytes.size ();
  long perline = bufB.stride;

  long i;

  // do the merge

#define Blend_Mask(A,B,M) ((uint8) ((A * M + B * (255 - M)) / 255) )

  switch (Mode)
    {
    case 0:     // normal
      if (Opacity < 1.0)
        {
        for (i = 0; i < count; i++)
          {
          pB [i] = Simple_Opacity (pB [i], Blend_Mask (pA [i], pB [i], pM [i]), Opacity);
          }
        }
      else
        {
        for (i = 0; i < count; i ++)
          pB [i]  = Blend_Mask (pA [i], pB [i], pM [i]);
        }
      break;  // end of mode 0

    case 1:  // transparent on pixel at 0,0
      {
      // the old ::GetPixel (A_DC, 0, 0) — the A layer's top-left pixel, i.e.
      // image pixel (SrcLeft, SrcTop), straight from the buffer's bytes
      const unsigned char * pA00 = bufA.row (0);
      COLORREF opaque = RGB (pA00 [2], pA00 [1], pA00 [0]);

      long row;
     for (row = 0; row < iHeight; row++)
      {
      long base = row * perline;
      unsigned char rA, gA, bA, rB, gB, bB;
      if (Opacity < 1.0)
        for (i = 0; i < perline - 2; )
          {
          bA = pA [base + i];
          gA = pA [base + i + 1];
          rA = pA [base + i + 2];
          bB = pB [base + i];
          gB = pB [base + i + 1];
          rB = pB [base + i + 2];

          // if this pixel is the opaque one, take the base pixel instead
          if (RGB (rA, gA, bA) == opaque)
            {
            rA = rB;
            gA = gB;
            bA = bB;
            }

          pB [base + i] = Simple_Opacity (bB, Blend_Mask (bA, bB, pM [base + i]), Opacity);
          i++;

          pB [base + i] = Simple_Opacity (gB, Blend_Mask (gA, gB, pM [base + i]), Opacity);
          i++;

          pB [base + i] = Simple_Opacity (rB, Blend_Mask (rA, rB, pM [base + i]), Opacity);
          i++;
          }
      else
        for (i = 0; i < perline - 2;)
          {
          bA = pA [base + i];
          gA = pA [base + i + 1];
          rA = pA [base + i + 2];
          bB = pB [base + i];
          gB = pB [base + i + 1];
          rB = pB [base + i + 2];

          // if this pixel is the opaque one, take the base pixel instead
          if (RGB (rA, gA, bA) == opaque)
            {
            rA = rB;
            gA = gB;
            bA = bB;
            }

          pB [base + i] = Blend_Mask (bA, bB, pM [base + i]);
          i++;

          pB [base + i] = Blend_Mask (gA, gB, pM [base + i]);
          i++;

          pB [base + i] = Blend_Mask (rA, rB, pM [base + i]);
          i++;
          }   // end Opacity == 1
        }     // end for loop


      }
      break;    // end of mode 1

    default: return eUnknownOption;

    } // end of switch on Mode


  // copy result back

  srf->writePixels (m_surfaceId,
                    { (int) Left, (int) Top, (int) iWidth, (int) iHeight }, bufB);

  return eOK;

  } // end of CMiniWindow::MergeImageAlpha

static CString strMXP_menu_item [MXP_MENU_COUNT];

CMenu* GrabAMenu (void)
  {
#ifdef _WIN32
  CMenu menu;
  VERIFY(menu.LoadMenu(IDR_MXP_MENU));

  CMenu* pPopup = menu.GetSubMenu(0);
  if (pPopup == NULL)
    return NULL;
  menu.Detach ();    // CMenu is temporary on stack
  pPopup->DeleteMenu (0, MF_BYPOSITION);  // get rid of dummy item

  return pPopup;
#else
  // No menu resources off-Windows; hand out a fresh popup from the CMenu arena
  // (freed wholesale by CMiniWindow::Menu's CMenu::ResetArena() on next use).
  return CMenu::NewPopup ();
#endif
  }   // end of GrabAMenu

CString CMiniWindow::Menu(long Left, long Top, LPCTSTR Items, CMUSHView* pView,
                          CMUSHclientDoc* pDoc)
  {
CString strResult;
int align = TPM_LEFTALIGN | TPM_TOPALIGN;  // these are actually both zero

  // can't menu if not visible
  if (!m_bShow || m_bTemporarilyHide)
    return strResult;

  // can't if outside window
  if (Left < 0 || Left > m_iWidth ||
      Top < 0 || Top > m_iHeight)
      return strResult;

  // make relative to miniwindow
  Left += m_rect.left;
  Top  += m_rect.top;

  // can't have empty string
  if (Items [0] == 0)
    return strResult;

  LPCTSTR p = Items;
  bool bReturnNumber = false;

  if (*p == '!')
    {
    bReturnNumber = true;
    p++;
    }

  // alignment

  if (*p == '~' && strlen (p) > 3)
    {
    p++;  // skip the '~'
    // horizontal alignment
    switch (*p++)
      {
      case 'c':
      case 'C':
         align |= TPM_CENTERALIGN;
         break;
      case 'l':
      case 'L':
         align |= TPM_LEFTALIGN;
         break;
      case 'r':
      case 'R':
         align |= TPM_RIGHTALIGN;
         break;
      default:
         // ignore
         break;
      } // end of switch

    // vertical alignment
    switch (*p++)
      {
      case 'c':
      case 'C':
         align |= TPM_VCENTERALIGN;
         break;
      case 'b':
      case 'B':
         align |= TPM_BOTTOMALIGN;
         break;
      case 't':
      case 'T':
         align |= TPM_TOPALIGN;
         break;
      default:
         // ignore
         break;
      } // end of switch

    } // end of alignment code

  vector<string> v;

  StringToVector (p, v, "|");

  int iCount = v.size ();

  // must have at least one item
  if (iCount < 1)
    return strResult;

CPoint menupoint (Left, Top);


  list<CMenu*> vPopup;    // nested menus

#ifndef _WIN32
  // Free the menus built for the previous (now-closed, modal) popup before we
  // allocate this one from the arena.
  CMenu::ResetArena ();
#endif

  CMenu* pPopup = GrabAMenu ();   // top level
  if (pPopup == NULL)
    return strResult;     // doesn't exist, strange
  vPopup.push_back (pPopup);

  CWnd* pWndPopupOwner = (CWnd *) pView;
#ifdef _WIN32
  while (pWndPopupOwner->GetStyle() & WS_CHILD)
    pWndPopupOwner = pWndPopupOwner->GetParent();

  ClientToScreen(pView->m_hWnd, &menupoint);
#else
  // The wx port has no CMUSHView (pView is null), so the owner-window walk has
  // nothing to walk.  The client→screen conversion still has to happen — the
  // popup hook takes screen coordinates, like Win32 TrackPopupMenu — so ask the
  // frontend for the output pane that stands in for the view.  Without this the
  // hook had no usable anchor and fell back to the mouse cursor, which put the
  // menu somewhere else entirely whenever WindowMenu was called from an alias,
  // a timer or a trigger rather than from a click.
  if (pDoc && g_wxhook_output_client_to_screen)
    {
    int x = menupoint.x, y = menupoint.y;
    g_wxhook_output_client_to_screen (pDoc, &x, &y);
    menupoint.x = x;
    menupoint.y = y;
    }
#endif

  int j = 0;

  for (vector<string>::const_iterator i = v.begin (); i != v.end (); i++)
    {
    CString strItem = i->c_str ();

    if (*i == "-" || *i == "")
      vPopup.back ()->AppendMenu (MF_SEPARATOR, 0, "");
    else if (strItem.Left (1) == ">")    // nested menu
      {
      CMenu* pNestedMenu = GrabAMenu ();   // nested menu
      if (pNestedMenu)
        {
        vPopup.back ()->AppendMenu (MF_POPUP | MF_ENABLED, (UINT ) pNestedMenu->m_hMenu, strItem.Mid (1));
        vPopup.push_back (pNestedMenu);  // add to menu stack
        }
      }
    else if (strItem.Left (1) == "<")    // un-nest menu
      {
      if (vPopup.size () > 1)
        vPopup.pop_back ();  // drop last item
      }
    else
      {
      // flags - will always be a string
      UINT iFlags = MF_STRING;

      while (strItem.Left (1) == "+" ||
             strItem.Left (1) == "^")
        {
        // leading + means tick it
        if (strItem.Left (1) == "+")
          iFlags |= MF_CHECKED;
        // and a carat means disabled
        else if (strItem.Left (1) == "^")
          iFlags |= MF_GRAYED;

        strItem = strItem.Mid (1);  // get rid of first character
        } // end of while

      if (iFlags & MF_GRAYED)
        vPopup.back ()->AppendMenu (iFlags, 0, strItem);
      else
        {
        strMXP_menu_item [j] = strItem;
        vPopup.back ()->AppendMenu (iFlags, MXP_FIRST_MENU + j, strItem);
        j++;
        if (j >= MXP_MENU_COUNT)
          break;
        }
      }

    }

  // without this line the auto-enable always set "no items" to active
  Frame.m_bAutoMenuEnable  = FALSE;

  int iResult = vPopup.front ()->TrackPopupMenu(align | TPM_RIGHTBUTTON | TPM_NONOTIFY | TPM_RETURNCMD,
                          menupoint.x,
                          menupoint.y,
                          pWndPopupOwner);

  // put things back how they were
  Frame.m_bAutoMenuEnable  = TRUE;

  if (iResult > 0)
    {
    if (bReturnNumber)
      strResult = CFormat ("%i", iResult - MXP_FIRST_MENU + 1);
    else
      strResult = strMXP_menu_item [iResult - MXP_FIRST_MENU];
    }

  vPopup.front ()->DestroyMenu ();   // clean up

  return  strResult;  // will be empty for errors or cancelled


  }  // end of CMiniWindow::Menu

long CMiniWindow::DragHandler(CMUSHclientDoc * pDoc, LPCTSTR HotspotId, string sPluginID, LPCTSTR MoveCallback, LPCTSTR ReleaseCallback, long Flags)
  {

  if (strlen (MoveCallback) > 0 && CheckLabel (MoveCallback, true))
    return eInvalidObjectLabel;
  if (strlen (ReleaseCallback) > 0 && CheckLabel (ReleaseCallback, true))
    return eInvalidObjectLabel;

  // can't switch plugins here :)
  if (!m_sCallbackPlugin.empty () && m_sCallbackPlugin != sPluginID)
    return eHotspotPluginChanged;

  HotspotMapIterator it = m_Hotspots.find (HotspotId);

  if (it == m_Hotspots.end ())
    return eHotspotNotInstalled;   // no such hotspot

  CHotspot * pHotspot = it->second;

  pHotspot->m_sMoveCallback = MoveCallback;
  pHotspot->m_sReleaseCallback = ReleaseCallback;
  pHotspot->m_DragFlags = Flags;


  // if not in a plugin, look in main world for hotspot callbacks, and remember the dispatch ID
  if (sPluginID.empty ())
    {

    CString strErrorMessage;

    pHotspot->m_dispid_MoveCallback     = pDoc->GetProcedureDispid (MoveCallback, "mouse move", "", strErrorMessage);
    pHotspot->m_dispid_ReleaseCallback  = pDoc->GetProcedureDispid (ReleaseCallback, "mouse release", "", strErrorMessage);
    }

  return eOK;


  } // end of CMiniWindow::DragHandler


// changes hotspot tooltip text
long CMiniWindow::HotspotTooltip(LPCTSTR HotspotId,
                             LPCTSTR TooltipText)
  {

  HotspotMapIterator it = m_Hotspots.find (HotspotId);

  if (it == m_Hotspots.end ())
    return eHotspotNotInstalled;

  CHotspot * pHotspot = it->second;

  pHotspot->m_sTooltipText      = TooltipText;

  return eOK;
  }    // end of CMiniWindow::HotspotTooltip

long CMiniWindow::DrawImageAlpha(LPCTSTR ImageId,
                    long Left, long Top, long Right, long Bottom,
                    double Opacity,
                    long SrcLeft, long SrcTop)
  {

  // constrain to what we actually have
  if (Left < 0)
    Left = 0;
  if (Top < 0)
    Top = 0;
  if (Right > m_iWidth)
    Right = m_iWidth;
  if (Bottom > m_iHeight)
    Bottom = m_iHeight;

  if (Opacity < 0.0 || Opacity > 1.0)
    return eBadParameter;

  // image to be merged
  ImageMapIterator it = m_Images.find (ImageId);

  if (it == m_Images.end ())
    return eImageNotInstalled;

  CBitmap * bitmap = it->second;


  BITMAP  bi;
  bitmap->GetBitmap(&bi);

  // can't do it unless we have alpha channel
  if (bi.bmBitsPixel != 32)
    return eImageNotInstalled;

  // calculate size of desired rectangle
  long iWidth = FixRight (Right) - Left;
  long iHeight = FixBottom (Bottom) - Top;

  // constrain to what we actually have
  if (SrcLeft < 0)
    SrcLeft = 0;
  if (SrcTop < 0)
    SrcTop = 0;

  if (iWidth <= 0 || iHeight <= 0)   // sanity check
    return eOK;

  pal::ISurface * srf = pal::g_surface;
  if (!srf)
    return eOK;

  // merge layer (from image id): the image's raw BGRA bytes with REAL alpha —
  // the old code BitBlt'ed the image into a zero-initialised 32bpp DIBSection,
  // which copies a 32bpp source's alpha verbatim and leaves zeros (alpha 0 =
  // keep base) wherever the read runs past the image edge. The temporary adopt
  // wraps the engine-owned CBitmap exactly as DrawImage does.
  CDC bmDC;   // for loading bitmaps into
  bmDC.CreateCompatibleDC(&(*pdc));
  CBitmap *pOldbmp = bmDC.SelectObject(bitmap);
  pal::SurfaceId imgSid = pal::g_adoptSurface
                        ? pal::g_adoptSurface (&bmDC, bi.bmWidth, bi.bmHeight)
                        : pal::SurfaceId::None;

  pal::PixelBuffer bufA;
  ReadRegionZeroFill (srf, imgSid, SrcLeft, SrcTop, iWidth, iHeight,
                      pal::PixelFormat::BGRA32_Alpha, bufA);

  if (pal::g_releaseAdoptedSurface)
    pal::g_releaseAdoptedSurface (imgSid);
  bmDC.SelectObject(pOldbmp);

  // base image (from miniwindow) — always in bounds (rect pre-clamped above);
  // its alpha bytes are pinned but the blend never reads them
  pal::PixelBuffer bufB;
  srf->readPixels (m_surfaceId,
                   { (int) Left, (int) Top, (int) iWidth, (int) iHeight },
                   bufB, pal::PixelFormat::BGRA32);

  if (!bufB.valid () || bufB.bytes.size () != bufA.bytes.size ())
    return eOK;

  unsigned char * pA = bufA.bytes.data ();
  unsigned char * pB = bufB.bytes.data ();

  long count = (long) bufB.bytes.size ();

  long i;

  // do the merge

#define Blend_Mask(A,B,M) ((uint8) ((A * M + B * (255 - M)) / 255) )

  if (Opacity < 1.0)
    {
    for (i = 0; i < count; i += 4)
      {
      pB [i]     = Simple_Opacity (pB [i], Blend_Mask (pA [i], pB [i], pA [i + 3]), Opacity);
      pB [i + 1] = Simple_Opacity (pB [i + 1], Blend_Mask (pA [i + 1], pB [i + 1], pA [i + 3]), Opacity);
      pB [i + 2] = Simple_Opacity (pB [i + 2], Blend_Mask (pA [i + 2], pB [i + 2], pA [i + 3]), Opacity);
      }
    }
  else
    {
    for (i = 0; i < count; i += 4)
      {
      pB [i]      = Blend_Mask (pA [i], pB [i], pA [i + 3]);
      pB [i + 1]  = Blend_Mask (pA [i + 1], pB [i + 1], pA [i + 3]);
      pB [i + 2]  = Blend_Mask (pA [i + 2], pB [i + 2], pA [i + 3]);
      }
    }


  // copy result back

  srf->writePixels (m_surfaceId,
                    { (int) Left, (int) Top, (int) iWidth, (int) iHeight }, bufB);

  return eOK;


  } // end of  CMiniWindow::DrawImageAlpha

long CMiniWindow::GetImageAlpha(LPCTSTR ImageId,
                    long Left, long Top, long Right, long Bottom,
                    long SrcLeft, long SrcTop)
  {

  // constrain to what we actually have
  if (Left < 0)
    Left = 0;
  if (Top < 0)
    Top = 0;
  if (Right > m_iWidth)
    Right = m_iWidth;
  if (Bottom > m_iHeight)
    Bottom = m_iHeight;

  // image to be merged
  ImageMapIterator it = m_Images.find (ImageId);

  if (it == m_Images.end ())
    return eImageNotInstalled;

  CBitmap * bitmap = it->second;


  BITMAP  bi;
  bitmap->GetBitmap(&bi);

  // can't do it unless we have alpha channel
  if (bi.bmBitsPixel != 32)
    return eImageNotInstalled;

  // calculate size of desired rectangle
  long iWidth = FixRight (Right) - Left;
  long iHeight = FixBottom (Bottom) - Top;

  // constrain to what we actually have
  if (SrcLeft < 0)
    SrcLeft = 0;
  if (SrcTop < 0)
    SrcTop = 0;

  if (iWidth <= 0 || iHeight <= 0)   // sanity check
    return eOK;

  pal::ISurface * srf = pal::g_surface;
  if (!srf)
    return eOK;

  // merge layer (from image id) — raw BGRA with real alpha, zero-filled past
  // the image edge, exactly the old BitBlt into a zero-initialised DIBSection
  CDC bmDC;   // for loading bitmaps into
  bmDC.CreateCompatibleDC(&(*pdc));
  CBitmap *pOldbmp = bmDC.SelectObject(bitmap);
  pal::SurfaceId imgSid = pal::g_adoptSurface
                        ? pal::g_adoptSurface (&bmDC, bi.bmWidth, bi.bmHeight)
                        : pal::SurfaceId::None;

  pal::PixelBuffer bufA;
  ReadRegionZeroFill (srf, imgSid, SrcLeft, SrcTop, iWidth, iHeight,
                      pal::PixelFormat::BGRA32_Alpha, bufA);

  if (pal::g_releaseAdoptedSurface)
    pal::g_releaseAdoptedSurface (imgSid);
  bmDC.SelectObject(pOldbmp);

  // base layer starts as zeros — the old B DIBSection was zero-initialised and
  // never blitted into
  pal::PixelBuffer bufB;
  bufB.format = pal::PixelFormat::BGRA32;
  bufB.size   = { (int) iWidth, (int) iHeight };
  bufB.stride = pal::PixelBuffer::strideFor ((int) iWidth, pal::PixelFormat::BGRA32);
  bufB.bytes.assign ((size_t) bufB.stride * iHeight, 0);

  unsigned char * pA = bufA.bytes.data ();
  unsigned char * pB = bufB.bytes.data ();

  long count = (long) bufB.bytes.size ();

  long i;

  // copy alpha channel into window

  for (i = 0; i < count; i += 4)
    {
    pB [i]      = pA [i + 3];
    pB [i + 1]  = pA [i + 3];
    pB [i + 2]  = pA [i + 3];
    }


  // copy result back

  srf->writePixels (m_surfaceId,
                    { (int) Left, (int) Top, (int) iWidth, (int) iHeight }, bufB);

  return eOK;


  } // end of  CMiniWindow::GetImageAlpha


long CMiniWindow::ScrollwheelHandler(CMUSHclientDoc * pDoc, LPCTSTR HotspotId, string sPluginID, LPCTSTR MoveCallback)
  {

  if (strlen (MoveCallback) > 0 && CheckLabel (MoveCallback, true))
    return eInvalidObjectLabel;

  // can't switch plugins here :)
  if (!m_sCallbackPlugin.empty () && m_sCallbackPlugin != sPluginID)
    return eHotspotPluginChanged;

  HotspotMapIterator it = m_Hotspots.find (HotspotId);

  if (it == m_Hotspots.end ())
    return eHotspotNotInstalled;   // no such hotspot

  CHotspot * pHotspot = it->second;

  pHotspot->m_sScrollwheelCallback = MoveCallback;


  // if not in a plugin, look in main world for hotspot callbacks, and remember the dispatch ID
  if (sPluginID.empty ())
    {

    CString strErrorMessage;

    pHotspot->m_dispid_ScrollwheelCallback     = pDoc->GetProcedureDispid (MoveCallback, "scroll wheel", "", strErrorMessage);
    }

  return eOK;


  } // end of CMiniWindow::ScrollwheelHandler


// resize a window

long CMiniWindow::Resize(long Width, long Height, long BackgroundColour)
  {

  // no change to size? wow, that was easy ...
  if (Width == m_iWidth && Height == m_iHeight)
    return eOK;


  CDC bmDC;   // for loading bitmaps into
  bmDC.CreateCompatibleDC(&(*pdc));

  // temporary bitmap
  CBitmap tempBitmap;
  tempBitmap.CreateCompatibleBitmap (&(*pdc), 1, 1);

  // select temporary bitmap into device context
  pdc->SelectObject(tempBitmap);

  // save old bitmap for copying from
  CBitmap * previousWindowBitmap = m_Bitmap;

  // select into new device context
  CBitmap * pOldbmp = bmDC.SelectObject (previousWindowBitmap);

  // make new bitmap for different size
  m_Bitmap = new CBitmap;

  //  CreateBitmap with zero-dimensions creates a monochrome bitmap, so force to be at least 1x1
#ifdef _WIN32
  m_Bitmap->CreateBitmap (MAX (Width, 1), MAX (Height, 1), 1, GetDeviceCaps((*pdc), BITSPIXEL), NULL);
#else
  // The RESIZE path needs the same HiDPI canvas as Create does, or a plugin
  // that resizes a miniwindow silently drops it back to 1x — which is what
  // happened to Quow's status panels, since they are created small and resized
  // to fit their content.  Everything drawn after that was half-resolution and
  // stretched, while windows that were never resized stayed sharp.
  m_Bitmap->CreateCanvasBitmap (MAX (Width, 1), MAX (Height, 1));
#endif
  pdc->SelectObject(m_Bitmap);
  pdc->SetWindowOrg(0, 0);

  // fill with requested fill colour
  pdc->FillSolidRect (0, 0, Width, Height, BackgroundColour);

  // copy old contents back
  pdc->BitBlt (0, 0, m_iWidth, m_iHeight, &bmDC, 0, 0, SRCCOPY);
  bmDC.SelectObject(pOldbmp);

  // done with previous bitmap from this miniwindow
  previousWindowBitmap->DeleteObject ();
  delete previousWindowBitmap;

  // remember new width and height

  m_iWidth  = Width ;
  m_iHeight = Height;

  // Re-adopt the (same) pdc at the new size for migrated ops.
  if (pal::g_releaseAdoptedSurface) pal::g_releaseAdoptedSurface (m_surfaceId);
  m_surfaceId = pal::g_adoptSurface
              ? pal::g_adoptSurface (pdc, m_iWidth, m_iHeight)
              : pal::SurfaceId::None;

  return eOK;

  } // end of CMiniWindow::Resize



// move a hotspot (maybe the window was resized)

long CMiniWindow::MoveHotspot(LPCTSTR HotspotId,
                             long Left, long Top, long Right, long Bottom)
  {

  HotspotMapIterator it = m_Hotspots.find (HotspotId);

  if (it == m_Hotspots.end ())
    return eHotspotNotInstalled;

  it->second->m_rect = CRect (Left, Top, FixRight (Right), FixBottom (Bottom));

  return eOK;
  }    // end of CMiniWindow::MoveHotspot



long CMiniWindow::TransformImage(LPCTSTR ImageId, float Left, float Top, short Mode, float Mxx, float Mxy, float Myx, float Myy)
  {

  ImageMapIterator it = m_Images.find (ImageId);

  if (it == m_Images.end ())
    return eImageNotInstalled;

  CBitmap * bitmap = it->second;

  BITMAP  bi;
  bitmap->GetBitmap(&bi);

  long iWidth  = bi.bmWidth;
  long iHeight = bi.bmHeight;

  if (iWidth <= 0 || iHeight <= 0)   // Sanity Claus
    return eOK;

  if (Mode != 1 && Mode != 3)
    return eBadParameter;

  // a non-invertible matrix made the old SetWorldTransform fail
  if ((double) Mxx * Myy - (double) Mxy * Myx == 0.0)
    return eBadParameter;

  pal::ISurface * srf = pal::g_surface;
  if (!srf)
    return eOK;

  // x' = x * Mxx + y * Mxy + Left
  // y' = x * Myx + y * Myy + Top
  pal::Matrix2x3 xform;   // == the old XFORM assignments, field for field
  xform.m11 = Mxx;        // cosine
  xform.m21 = Mxy;        // sine
  xform.m12 = Myx;        // - sine
  xform.m22 = Myy;        // cosine
  xform.dx  = Left;
  xform.dy  = Top;

  CDC bmDC;
  bmDC.CreateCompatibleDC(&(*pdc));
  CBitmap *pOldbmp = bmDC.SelectObject(bitmap);
  pal::SurfaceId imgSid = pal::g_adoptSurface
                        ? pal::g_adoptSurface (&bmDC, bi.bmWidth, bi.bmHeight)
                        : pal::SurfaceId::None;

  // Mode 1 = straight copy; Mode 3 = transparent on the image's (0,0) pixel
  // ("transparency, nom nom nom!"). Smooth = the old HALFTONE StretchBltMode.
  srf->blit (m_surfaceId, { 0, 0, 0, 0 },   // dstRect ignored with a matrix
             imgSid, { 0, 0, (int) iWidth, (int) iHeight },
             Mode == 1 ? pal::BlitMode::Copy : pal::BlitMode::TransparentKey,
             Mode == 3 ? srf->getPixel (imgSid, { 0, 0 }) : 0,
             pal::BlitQuality::Smooth, &xform);

  if (pal::g_releaseAdoptedSurface)
    pal::g_releaseAdoptedSurface (imgSid);
  bmDC.SelectObject(pOldbmp);

  return eOK;
  }   // end of CMiniWindow::TransformImage

