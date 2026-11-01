// dwrite_text.cpp : see dwrite_text.h

#include "stdafx.h"
#include "dwrite_text.h"

#ifdef _WIN32

#include <d2d1.h>
#include <dwrite.h>

namespace dwtext {

namespace {

template <typename T> void Release (T * & p)
  {
  if (p)
    {
    p->Release ();
    p = NULL;
    }
  }

typedef HRESULT (WINAPI * D2D1CreateFactoryFn) (D2D1_FACTORY_TYPE, REFIID, const D2D1_FACTORY_OPTIONS *, void **);
typedef HRESULT (WINAPI * DWriteCreateFactoryFn) (DWRITE_FACTORY_TYPE, REFIID, IUnknown **);

ID2D1Factory * pD2d = NULL;
IDWriteFactory * pWrite = NULL;
IDWriteGdiInterop * pInterop = NULL;
ID2D1DCRenderTarget * pTarget = NULL;
ID2D1SolidColorBrush * pBrush = NULL;
bool bTried = false;
bool bAvailable = false;

bool Load ()
  {
  if (bTried)
    return bAvailable;
  bTried = true;

  HMODULE hD2d = ::LoadLibraryA ("d2d1.dll");
  HMODULE hWrite = ::LoadLibraryA ("dwrite.dll");
  if (!hD2d || !hWrite)
    return false;

  D2D1CreateFactoryFn pD2dCreate = (D2D1CreateFactoryFn) ::GetProcAddress (hD2d, "D2D1CreateFactory");
  DWriteCreateFactoryFn pWriteCreate = (DWriteCreateFactoryFn) ::GetProcAddress (hWrite, "DWriteCreateFactory");
  if (!pD2dCreate || !pWriteCreate)
    return false;

  if (FAILED (pD2dCreate (D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof (ID2D1Factory), NULL, (void **) &pD2d)))
    return false;
  if (FAILED (pWriteCreate (DWRITE_FACTORY_TYPE_SHARED, __uuidof (IDWriteFactory), (IUnknown **) &pWrite)))
    return false;
  if (FAILED (pWrite->GetGdiInterop (&pInterop)))
    return false;

  // Pixels, whatever the display: the view works in real ones, as GDI does.
  D2D1_RENDER_TARGET_PROPERTIES props;
  ZeroMemory (&props, sizeof props);
  props.type = D2D1_RENDER_TARGET_TYPE_DEFAULT;
  props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
  props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
  props.dpiX = 96.0f;
  props.dpiY = 96.0f;
  props.usage = D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE;
  props.minLevel = D2D1_FEATURE_LEVEL_DEFAULT;
  if (FAILED (pD2d->CreateDCRenderTarget (&props, &pTarget)))
    return false;

  bAvailable = true;
  return true;
  }

// The text in the font that's in hdc, or NULL if DirectWrite can't use that font.
// Metrics of the font come back in iBaseline (pixels below the layout's top).
IDWriteTextLayout * MakeLayout (HDC hdc, const WCHAR * pText, int iLength, float & fBaseline, int & iGdiAscent,
                                int iScale, bool bAllowStandIn)
  {
  if (!Load () || iLength <= 0)
    return NULL;

  LOGFONTW lf;
  HGDIOBJ hFont = ::GetCurrentObject (hdc, OBJ_FONT);
  if (!hFont || !::GetObjectW (hFont, sizeof lf, &lf))
    return NULL;

  TEXTMETRICW tm;
  if (!::GetTextMetricsW (hdc, &tm))
    return NULL;
  const bool bStandIn = (tm.tmPitchAndFamily & (TMPF_VECTOR | TMPF_TRUETYPE)) == 0;
  if (bStandIn && !bAllowStandIn)
    return NULL;    // a bitmap font
  iGdiAscent = tm.tmAscent * iScale;
  if (bStandIn)
    wcscpy_s (lf.lfFaceName, L"Consolas");

  IDWriteFont * pFont = NULL;
  if (FAILED (pInterop->CreateFontFromLOGFONT (&lf, &pFont)))
    return NULL;

  IDWriteFontFamily * pFamily = NULL;
  IDWriteLocalizedStrings * pNames = NULL;
  WCHAR sName [LF_FACESIZE + 1] = L"";
  if (SUCCEEDED (pFont->GetFontFamily (&pFamily)) && SUCCEEDED (pFamily->GetFamilyNames (&pNames)))
    pNames->GetString (0, sName, LF_FACESIZE);

  const DWRITE_FONT_WEIGHT weight = pFont->GetWeight ();
  const DWRITE_FONT_STYLE style = pFont->GetStyle ();
  const DWRITE_FONT_STRETCH stretch = pFont->GetStretch ();

  Release (pNames);
  Release (pFamily);
  Release (pFont);

  if (!sName [0])
    return NULL;

  // what GDI's height comes to in an em
  const float fEm = (float) ((tm.tmHeight - tm.tmInternalLeading) * iScale);

  IDWriteTextFormat * pFormat = NULL;
  if (FAILED (pWrite->CreateTextFormat (sName, NULL, weight, style, stretch, fEm, L"", &pFormat)))
    return NULL;
  pFormat->SetWordWrapping (DWRITE_WORD_WRAPPING_NO_WRAP);

  IDWriteTextLayout * pLayout = NULL;
  const HRESULT hr = pWrite->CreateTextLayout (pText, iLength, pFormat, 100000.0f, 1000.0f, &pLayout);
  Release (pFormat);
  if (FAILED (hr))
    return NULL;

  DWRITE_TEXT_RANGE all = { 0, (UINT32) iLength };
  if (lf.lfUnderline)
    pLayout->SetUnderline (TRUE, all);
  if (lf.lfStrikeOut)
    pLayout->SetStrikethrough (TRUE, all);

  DWRITE_LINE_METRICS line;
  UINT32 iLines = 0;
  fBaseline = 0.0f;
  if (SUCCEEDED (pLayout->GetLineMetrics (&line, 1, &iLines)) && iLines > 0)
    fBaseline = line.baseline;

  return pLayout;
  }

}  // end of anonymous namespace

bool Measure (HDC hdc, const WCHAR * pText, int iLength, int & iWidth, int iScale, bool bAllowStandIn)
  {
  float fBaseline;
  int iAscent;
  IDWriteTextLayout * pLayout = MakeLayout (hdc, pText, iLength, fBaseline, iAscent, iScale, bAllowStandIn);
  if (!pLayout)
    return false;

  DWRITE_TEXT_METRICS metrics;
  const bool bOK = SUCCEEDED (pLayout->GetMetrics (&metrics));
  if (bOK)
    iWidth = (int) (metrics.widthIncludingTrailingWhitespace + 0.5f);
  Release (pLayout);
  return bOK;
  }

bool Draw (HDC hdc, const RECT & r, const WCHAR * pText, int iLength, COLORREF colour, int iScale, bool bAllowStandIn)
  {
  float fBaseline;
  int iAscent;
  IDWriteTextLayout * pLayout = MakeLayout (hdc, pText, iLength, fBaseline, iAscent, iScale, bAllowStandIn);
  if (!pLayout)
    return false;

  bool bOK = false;
  if (SUCCEEDED (pTarget->BindDC (hdc, &r)))
    {
    const D2D1_COLOR_F colourF = D2D1::ColorF (GetRValue (colour) / 255.0f, GetGValue (colour) / 255.0f, GetBValue (colour) / 255.0f);
    if (!pBrush)
      pTarget->CreateSolidColorBrush (colourF, &pBrush);
    if (pBrush)
      {
      pBrush->SetColor (colourF);

      pTarget->BeginDraw ();

      // the same baseline as GDI's text beside it
      D2D1_POINT_2F origin;
      origin.x = 0.0f;
      origin.y = (float) iAscent - fBaseline;

      // ENABLE_COLOR_FONT is 4 from Windows 8.1, and is ignored before that
      pTarget->DrawTextLayout (origin, pLayout, pBrush,
                               (D2D1_DRAW_TEXT_OPTIONS) (D2D1_DRAW_TEXT_OPTIONS_NONE | 4));

      HRESULT hr = pTarget->EndDraw ();
      if (hr == D2DERR_RECREATE_TARGET)
        {
        // start afresh next time
        Release (pBrush);
        Release (pTarget);
        bTried = false;
        bAvailable = false;
        Release (pInterop);
        Release (pWrite);
        Release (pD2d);
        }
      bOK = SUCCEEDED (hr);
      }
    }

  Release (pLayout);
  return bOK;
  }

}  // namespace dwtext

#endif  // _WIN32
