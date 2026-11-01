// tests/win/test_hidpi_win.cpp -- miniwindows over real GDI at a high dpi: plugins draw in
// 96-dpi coordinates, and the real pixels are underneath.  Real CDCs and real bitmaps; the
// only thing simulated is which dpi the window is on (CMUSHclientDoc::m_iDpi), so the
// test doesn't care what the monitor it runs on is doing.

#include "mc_test.h"
#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "miniwindow.h"
#include "dpi.h"

#include <string>
#include <vector>

namespace {

// What is really in the window's bitmap, by physical pixel: 0x00RRGGBB.
struct Physical
  {
  int width = 0, height = 0;
  std::vector<unsigned int> pixels;
  explicit Physical (CMiniWindow * mw)
    {
    HDC hdc = mw->GetDC ()->GetSafeHdc ();
    HBITMAP hbm = (HBITMAP) ::GetCurrentObject (hdc, OBJ_BITMAP);
    BITMAP bm;
    ::GetObject (hbm, sizeof bm, &bm);
    width = bm.bmWidth;
    height = bm.bmHeight;
    pixels.resize (width * height);
    BITMAPINFO bmi;
    ZeroMemory (&bmi, sizeof bmi);
    bmi.bmiHeader.biSize = sizeof bmi.bmiHeader;
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    ::GetDIBits (hdc, hbm, 0, height, &pixels [0], &bmi, DIB_RGB_COLORS);
    for (unsigned int & p : pixels)
      p &= 0xFFFFFF;
    }
  unsigned int at (int x, int y) const { return pixels [y * width + x]; }
  };

const unsigned int kRedDib = 0xFF0000;    // as GetDIBits has it
const COLORREF kRed = RGB (255, 0, 0);
const COLORREF kBlack = RGB (0, 0, 0);

CMiniWindow * MakeWindow (CMUSHclientDoc & doc, const int iDpi, const char * name = "w")
  {
  doc.m_iDpi = iDpi;
  EXPECT_EQ(0, (int) doc.WindowCreate (name, 0, 0, 100, 50, 4, 0, kBlack));
  return doc.m_MiniWindows [name];
  }

}  // end of anonymous namespace

TEST(HiDpiTest, BackingIsRealPixelsAndTheCoordinatesAreNot) {
  CMUSHclientDoc doc;
  CMiniWindow * mw = MakeWindow (doc, 192);
  ASSERT_TRUE(mw != NULL);

  EXPECT_EQ(100, (int) mw->GetWidth ());     // what the plugin asked for
  EXPECT_EQ(50, (int) mw->GetHeight ());

  Physical p (mw);
  EXPECT_EQ(200, p.width);                   // what is behind it
  EXPECT_EQ(100, p.height);
}

TEST(HiDpiTest, AtNinetySixTheDcIsNotTouched) {
  CMUSHclientDoc doc;
  CMiniWindow * mw = MakeWindow (doc, 96);
  ASSERT_TRUE(mw != NULL);
  EXPECT_EQ(MM_TEXT, mw->GetDC ()->GetMapMode ());
  Physical p (mw);
  EXPECT_EQ(100, p.width);
  EXPECT_EQ(50, p.height);
}

TEST(HiDpiTest, ARectangleCoversTheRightPixelsAndExcludesItsEdges) {
  CMUSHclientDoc doc;
  CMiniWindow * mw = MakeWindow (doc, 192);
  ASSERT_TRUE(mw != NULL);
  doc.WindowRectOp ("w", 2, 10, 10, 20, 15, kRed, 0);     // fill

  Physical p (mw);
  EXPECT_EQ(kRedDib, p.at (20, 20));         // logical 10,10
  EXPECT_EQ(kRedDib, p.at (39, 29));         // the last pixel of logical 19,14
  EXPECT_EQ(0u, p.at (19, 20));              // just outside, left
  EXPECT_EQ(0u, p.at (40, 20));              // just outside, right
  EXPECT_EQ(0u, p.at (20, 19));              // above
  EXPECT_EQ(0u, p.at (20, 30));              // below
}

TEST(HiDpiTest, ALogicalPixelIsWholeOnEveryCommonScale) {
  for (int iDpi : { 120, 144, 168, 192, 288 }) {
    CMUSHclientDoc doc;
    CMiniWindow * mw = MakeWindow (doc, iDpi);
    ASSERT_TRUE(mw != NULL);

    EXPECT_EQ(0, (int) doc.WindowSetPixel ("w", 30, 20, kRed));
    EXPECT_EQ((long) kRed, doc.WindowGetPixel ("w", 30, 20));
    EXPECT_EQ(0L, doc.WindowGetPixel ("w", 31, 20));
    EXPECT_EQ(0L, doc.WindowGetPixel ("w", 29, 20));
    EXPECT_EQ(0L, doc.WindowGetPixel ("w", 30, 21));
    EXPECT_EQ(0L, doc.WindowGetPixel ("w", 30, 19));

    // and every real pixel of it was set
    Physical p (mw);
    const int x0 = ::MulDiv (30, iDpi, 96), x1 = ::MulDiv (31, iDpi, 96);
    const int y0 = ::MulDiv (20, iDpi, 96), y1 = ::MulDiv (21, iDpi, 96);
    int iSet = 0;
    for (int y = y0; y < y1; y++)
      for (int x = x0; x < x1; x++)
        if (p.at (x, y) == kRedDib)
          iSet++;
    EXPECT_EQ((x1 - x0) * (y1 - y0), iSet);
  }
}

// docs/FONT-PARITY-2026-08.md section 1: the em stays MulDiv (pt, 96, 72).  A plugin must
// measure exactly what it would at 96 dpi, or its layouts would all change.
TEST(HiDpiTest, FontsMeasureTheSameAtEveryScale) {
  const char * sample = "The quick brown fox";
  const char * const faces [] = { "Arial", "Courier New", "FixedSys", "Tahoma" };

  for (const char * face : faces) {
    long iWidth96 = 0, iHeight96 = 0;
    for (int iDpi : { 96, 144, 192 }) {
      CMUSHclientDoc doc;
      MakeWindow (doc, iDpi);
      EXPECT_EQ(0, (int) doc.WindowFont ("w", "f", face, 9, FALSE, FALSE, FALSE, FALSE, 0, 0));
      const long iWidth = doc.WindowTextWidth ("w", "f", sample, FALSE);
      VARIANT v = doc.WindowFontInfo ("w", "f", 1);
      const long iHeight = v.lVal;
      VariantClear (&v);
      if (iDpi == 96) {
        iWidth96 = iWidth;
        iHeight96 = iHeight;
        EXPECT_GT(iWidth96, 0L);
      } else {
        EXPECT_EQ(iWidth96, iWidth);
        EXPECT_EQ(iHeight96, iHeight);
      }
    }
  }
}

TEST(HiDpiTest, TextIsActuallyDrawnLargerOnALargerCanvas) {
  // The sum of what's lit, as a stand-in for "the glyphs were rendered at the real size".
  long iLit [2] = { 0, 0 };
  int iRow = 0;
  for (int iDpi : { 96, 192 }) {
    CMUSHclientDoc doc;
    CMiniWindow * mw = MakeWindow (doc, iDpi);
    doc.WindowFont ("w", "f", "Arial", 10, FALSE, FALSE, FALSE, FALSE, 0, 0);
    doc.WindowText ("w", "f", "Mmmm", 2, 2, 0, 0, RGB (255, 255, 255), FALSE);
    Physical p (mw);
    for (unsigned int px : p.pixels)
      if (px != 0)
        iLit [iRow]++;
    iRow++;
  }
  // about four times as many pixels at twice the size; certainly more than double
  EXPECT_GT(iLit [1], iLit [0] * 2);
}

TEST(HiDpiTest, BitmapFontsAreBlownUpInsteadOfVanishing) {
  long iLit [2] = { 0, 0 };
  int iRow = 0;
  for (int iDpi : { 96, 192 }) {
    CMUSHclientDoc doc;
    CMiniWindow * mw = MakeWindow (doc, iDpi);
    doc.WindowFont ("w", "f", "FixedSys", 9, FALSE, FALSE, FALSE, FALSE, 0, 0);
    doc.WindowText ("w", "f", "Mmmm", 2, 2, 0, 0, RGB (255, 255, 255), FALSE);
    Physical p (mw);
    for (unsigned int px : p.pixels)
      if (px != 0)
        iLit [iRow]++;
    iRow++;
  }
  EXPECT_GT(iLit [0], 0L);
  EXPECT_EQ(iLit [0] * 4, iLit [1]);     // every pixel of it became exactly four
}

TEST(HiDpiTest, ResizeKeepsContentsAndScale) {
  CMUSHclientDoc doc;
  CMiniWindow * mw = MakeWindow (doc, 192);
  doc.WindowRectOp ("w", 2, 0, 0, 10, 10, kRed, 0);
  EXPECT_EQ(0, (int) doc.WindowResize ("w", 150, 80, RGB (0, 0, 255)));

  Physical p (mw);
  EXPECT_EQ(300, p.width);
  EXPECT_EQ(160, p.height);
  EXPECT_EQ(kRedDib, p.at (19, 19));
  EXPECT_EQ(0xFFu, p.at (299, 159));       // the new area, in the new colour
  EXPECT_EQ((long) kRed, doc.WindowGetPixel ("w", 9, 9));
}

TEST(HiDpiTest, MovingToAnotherMonitorKeepsThePicture) {
  CMUSHclientDoc doc;
  CMiniWindow * mw = MakeWindow (doc, 192);
  doc.WindowRectOp ("w", 2, 20, 10, 40, 30, kRed, 0);

  mw->Rescale (96);

  Physical p (mw);
  EXPECT_EQ(100, p.width);
  EXPECT_EQ(50, p.height);
  EXPECT_EQ(kRedDib, p.at (30, 20));
  EXPECT_EQ(0u, p.at (5, 5));
  // and it goes on working in the same coordinates
  EXPECT_EQ((long) kRed, doc.WindowGetPixel ("w", 30, 20));
  doc.WindowRectOp ("w", 2, 0, 0, 5, 5, kRed, 0);
  EXPECT_EQ((long) kRed, doc.WindowGetPixel ("w", 4, 4));
}

TEST(HiDpiTest, ConversionsAreTheIdentityAtNinetySix) {
  CMUSHclientDoc doc;
  doc.m_iDpi = 96;
  for (int v : { -9, 0, 1, 7, 15, 12345 }) {
    EXPECT_EQ(v, doc.ToPhysical (v));
    EXPECT_EQ(v, doc.ToLogical (v));
  }
  doc.m_iDpi = 144;
  EXPECT_EQ(30, doc.ToPhysical (20));
  EXPECT_EQ(20, doc.ToLogical (30));
  EXPECT_EQ(-15, doc.ToPhysical (-10));
}

TEST(HiDpiTest, RasterFactorIsWhole) {
  EXPECT_EQ(1, Dpi_RasterFactor (96));
  EXPECT_EQ(1, Dpi_RasterFactor (120));    // 125%: its own size
  EXPECT_EQ(2, Dpi_RasterFactor (144));    // 150%
  EXPECT_EQ(2, Dpi_RasterFactor (192));
  EXPECT_EQ(3, Dpi_RasterFactor (288));
}
