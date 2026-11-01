// appearance.cpp : the arithmetic behind light and dark chrome (see appearance.h).
// No windows here, on any platform, so that the tests can run it anywhere.

#include "stdafx.h"
#include "appearance.h"

#include <algorithm>
#include <vector>

bool Appearance_ResolveDark (int iPreference, bool bSystemDark, bool bDarkSupported)
  {
  if (!bDarkSupported)
    return false;

  switch (iPreference)
    {
    case APPEARANCE_LIGHT:  return false;
    case APPEARANCE_DARK:   return true;
    default:                return bSystemDark;   // includes a bad value in the database
    }
  }

bool Appearance_WindowsBuildSupportsDark (unsigned long iBuild)
  {
  return iBuild >= 17763;
  }

int Appearance_ImmersiveDarkAttribute (unsigned long iBuild)
  {
  return iBuild >= 18985 ? 20 : 19;
  }

unsigned long Appearance_ToolbarPixelForDark (unsigned long iColour)
  {
  const int r = iColour & 0xFF;
  const int g = (iColour >> 8) & 0xFF;
  const int b = (iColour >> 16) & 0xFF;

  const int iMax = (std::max) (r, (std::max) (g, b));
  const int iMin = (std::min) (r, (std::min) (g, b));

  int nr, ng, nb;

  if (iMax - iMin < 24)
    {
    // A grey: outlines and shadows were drawn dark to show up on a light face.
    // Turn it over.  Black becomes nearly white, and white goes to a soft light
    // grey, so that a white page with a black outline stays a page, not a blob.
    const int iGrey = (r + g + b) / 3;
    const int iNew = iGrey >= 192 ? 200 + (iGrey - 192) * 32 / 63    // 192..255 -> 200..232
                                  : 232 - iGrey * 100 / 192;         // 0..191   -> 232..133
    nr = ng = nb = iNew;
    }
  else if (iMax < 170 || (r * 30 + g * 59 + b * 11) / 100 < 75)
    {
    // A dark colour (navy, maroon, dark green, or pure blue) that would vanish.  Lift it, keeping
    // the hue: scale so the brightest channel reaches 220, and raise the floor.
    nr = 60 + r * 160 / iMax;
    ng = 60 + g * 160 / iMax;
    nb = 60 + b * 160 / iMax;
    }
  else
    {
    nr = r;
    ng = g;
    nb = b;
    }

  return (unsigned long) (nr | (ng << 8) | (nb << 16));
  }

unsigned int Appearance_DisabledPixel (unsigned int iPixel, bool bDark)
  {
  const unsigned int a = iPixel >> 24;
  const unsigned int r = (iPixel >> 16) & 0xFF;
  const unsigned int g = (iPixel >> 8) & 0xFF;
  const unsigned int b = iPixel & 0xFF;

  // grey it, then fade it towards the face behind it
  const unsigned int iGrey = (r * 30 + g * 59 + b * 11) / 100;
  const unsigned int iNewAlpha = a * (bDark ? 45 : 40) / 100;

  return (iNewAlpha << 24) | (iGrey << 16) | (iGrey << 8) | iGrey;
  }

// Area-averaging reduction in straight alpha: the colour of each destination pixel
// is weighted by the coverage AND the alpha of what lands in it, or else the
// transparent background's colour would bleed into the icon's edges.
static void BoxDown (const unsigned int * pSrc, int iSrcWidth, int iSrcHeight,
                     unsigned int * pDst, int iDstWidth, int iDstHeight)
  {
  for (int y = 0; y < iDstHeight; y++)
    {
    const double y0 = (double) y * iSrcHeight / iDstHeight;
    const double y1 = (double) (y + 1) * iSrcHeight / iDstHeight;

    for (int x = 0; x < iDstWidth; x++)
      {
      const double x0 = (double) x * iSrcWidth / iDstWidth;
      const double x1 = (double) (x + 1) * iSrcWidth / iDstWidth;

      double dA = 0, dR = 0, dG = 0, dB = 0, dArea = 0;

      for (int sy = (int) y0; sy < iSrcHeight && sy < y1; sy++)
        {
        const double dH = (std::min) ((double) sy + 1, y1) - (std::max) ((double) sy, y0);

        for (int sx = (int) x0; sx < iSrcWidth && sx < x1; sx++)
          {
          const double dW = (std::min) ((double) sx + 1, x1) - (std::max) ((double) sx, x0);
          const double dCover = dW * dH;
          const unsigned int p = pSrc [sy * iSrcWidth + sx];
          const double a = (p >> 24) * dCover;
          dArea += dCover;
          dA += a;
          dR += ((p >> 16) & 0xFF) * a;
          dG += ((p >> 8) & 0xFF) * a;
          dB += (p & 0xFF) * a;
          }
        }

      unsigned int iOut = 0;
      if (dA > 0)
        {
        const unsigned int a = (unsigned int) (dA / dArea + 0.5);
        const unsigned int r = (unsigned int) (dR / dA + 0.5);
        const unsigned int g = (unsigned int) (dG / dA + 0.5);
        const unsigned int b = (unsigned int) (dB / dA + 0.5);
        iOut = (a << 24) | (r << 16) | (g << 8) | b;
        }
      pDst [y * iDstWidth + x] = iOut;
      }
    }
  }

void Appearance_ScaleImage (const unsigned int * pSrc, int iSrcWidth, int iSrcHeight,
                            unsigned int * pDst, int iDstWidth, int iDstHeight)
  {
  if (iSrcWidth <= 0 || iSrcHeight <= 0 || iDstWidth <= 0 || iDstHeight <= 0)
    return;

  // the smallest whole multiple at least as large as the target, on each axis
  const int iFactorX = (iDstWidth + iSrcWidth - 1) / iSrcWidth;
  const int iFactorY = (iDstHeight + iSrcHeight - 1) / iSrcHeight;

  const int iBigWidth = iSrcWidth * iFactorX;
  const int iBigHeight = iSrcHeight * iFactorY;

  std::vector<unsigned int> big (iBigWidth * iBigHeight);
  for (int y = 0; y < iBigHeight; y++)
    for (int x = 0; x < iBigWidth; x++)
      big [y * iBigWidth + x] = pSrc [(y / iFactorY) * iSrcWidth + (x / iFactorX)];

  if (iBigWidth == iDstWidth && iBigHeight == iDstHeight)
    std::copy (big.begin (), big.end (), pDst);
  else
    BoxDown (&big [0], iBigWidth, iBigHeight, pDst, iDstWidth, iDstHeight);
  }

// ---------------------------------------------------------------------------
// macOS and Linux: wxWidgets does the work.  platform/wx_appearance.cpp sets the hook.

#ifndef _WIN32

void (* g_pfnAppearanceApply) (int iPreference) = nullptr;
bool (* g_pfnAppearanceIsDark) () = nullptr;

static int iAppearancePreference = APPEARANCE_SYSTEM;

void Appearance_Init (int iPreference)
  {
  iAppearancePreference = iPreference;
  if (g_pfnAppearanceApply)
    g_pfnAppearanceApply (iPreference);
  }

void Appearance_SetPreference (int iPreference)
  {
  Appearance_Init (iPreference);
  }

bool Appearance_IsDark ()
  {
  return g_pfnAppearanceIsDark && g_pfnAppearanceIsDark ();
  }

#endif  // !_WIN32
