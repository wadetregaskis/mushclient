// bmp_load_shared.cpp
//
// LoadBmp — decode a Windows .bmp file into a Windows-shaped HBITMAP.
// Declared in stdafx.h.  Compiled by every build.
//
// On Windows this work is done by ::LoadImage(IMAGE_BITMAP, LR_LOADFROMFILE),
// a Win32 API that does not exist off it.  The port's shim for that returned
// nullptr, so every .bmp silently failed in all THREE of its callers --
// world.WindowLoadImage (miniwindow.cpp:1126), world.SetBackgroundImage and
// world.SetForegroundImage (methods_output.cpp:410, :482) -- and the caller
// then reported eFileNotFound or eUnableToLoadImage for a file that was there
// and perfectly readable.  .png worked, because that has a decoder of its own
// (png_load_shared.cpp); .bmp had none.
//
// Deliberately narrow.  BI_RGB only, at 24 or 32 bits per pixel, or 8-bit
// palettised -- which is every .bmp any image editor writes by default and
// every one MUSHclient's own documentation describes.  Run-length encoded
// (BI_RLE8/BI_RLE4), bitfield, 16-bit, JPEG- and PNG-in-BMP variants are
// REFUSED with eUnableToLoadImage rather than half-decoded: a wrong picture is
// worse than a reported failure, and the caller already has an error path.
//
// Output matches what LoadPng produces so the two are interchangeable to
// CBitmap::Attach: a bottom-up DIB section (positive biHeight), BI_RGB, 24bpp
// unless the source carried an alpha channel, in which case 32.
//
// Every read from the file buffer is bounds-checked against the actual byte
// count, not against the header's own claims -- a .bmp is an untrusted file and
// its header can say anything.

#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "scripting/errors.h"

#include <vector>

// Bytes per DIB row, padded to a 4-byte boundary.  Same rule as the file's own
// stride and as miniwindow.cpp's BytesPerLine.
static long BmpBytesPerLine (int nWidth, int nBitsPerPixel)
{
    return (((long) nWidth * nBitsPerPixel + 31) / 32) * 4;
}

// Little-endian scalar reads.  The on-disk headers are packed and unaligned, so
// they are read field by field rather than by casting a struct over the bytes.
static unsigned short BmpU16 (const unsigned char* p)
{
    return (unsigned short) (p[0] | (p[1] << 8));
}

static unsigned int BmpU32 (const unsigned char* p)
{
    return (unsigned int) (p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned int) p[3] << 24));
}

static int BmpS32 (const unsigned char* p)
{
    return (int) BmpU32 (p);
}

long LoadBmp (LPCTSTR FileName, HBITMAP & hbmp)
{
    hbmp = NULL;

    FILE* fp = fopen (FileName, "rb");
    if (fp == NULL)
        return eFileNotFound;

    std::vector<unsigned char> file;
    {
        unsigned char buf[65536];
        size_t got;
        while ((got = fread (buf, 1, sizeof buf, fp)) > 0)
        {
            // A .bmp large enough to overflow the size arithmetic below is not
            // something to try to decode.
            if (file.size () > (size_t) 512 * 1024 * 1024)
            {
                fclose (fp);
                return eUnableToLoadImage;
            }
            file.insert (file.end (), buf, buf + got);
        }
    }
    fclose (fp);

    // BITMAPFILEHEADER (14) + BITMAPINFOHEADER (40)
    if (file.size () < 54)
        return eUnableToLoadImage;
    if (file[0] != 'B' || file[1] != 'M')
        return eUnableToLoadImage;

    const unsigned int offBits = BmpU32 (&file[10]);
    const unsigned int hdrSize = BmpU32 (&file[14]);
    if (hdrSize < 40)
        return eUnableToLoadImage;      // BITMAPCOREHEADER and friends: refused

    const int          width       = BmpS32 (&file[18]);
    const int          rawHeight   = BmpS32 (&file[22]);
    const unsigned short bitCount  = BmpU16 (&file[28]);
    const unsigned int compression = BmpU32 (&file[30]);
    unsigned int       clrUsed     = (file.size () >= 54) ? BmpU32 (&file[46]) : 0;

    if (compression != 0 /*BI_RGB*/)
        return eUnableToLoadImage;
    if (bitCount != 8 && bitCount != 24 && bitCount != 32)
        return eUnableToLoadImage;
    if (width <= 0 || rawHeight == 0)
        return eUnableToLoadImage;

    // A negative height means the rows are stored TOP-DOWN.  The DIB we build is
    // always bottom-up, so this decides whether the copy reverses.
    const bool topDown = (rawHeight < 0);
    const long height  = topDown ? -(long) rawHeight : (long) rawHeight;

    // Guard the multiply before it is used as a size.
    if (height > 65535 || width > 65535)
        return eUnableToLoadImage;

    const long srcStride = BmpBytesPerLine (width, bitCount);

    // The palette sits between the header and the pixels.
    const unsigned char* palette = NULL;
    unsigned int paletteEntries = 0;
    if (bitCount == 8)
    {
        paletteEntries = clrUsed ? clrUsed : 256;
        if (paletteEntries > 256)
            return eUnableToLoadImage;
        const size_t palOffset = 14 + hdrSize;
        if (palOffset + (size_t) paletteEntries * 4 > file.size ())
            return eUnableToLoadImage;
        palette = &file[palOffset];
    }

    // Every pixel byte the file claims to hold must actually be there.
    if ((size_t) offBits >= file.size ())
        return eUnableToLoadImage;
    if ((size_t) srcStride * (size_t) height > file.size () - (size_t) offBits)
        return eUnableToLoadImage;

    const int dstBits = (bitCount == 32) ? 32 : 24;
    const long dstStride = BmpBytesPerLine (width, dstBits);

    BITMAPINFO bmiB;
    ZeroMemory (&bmiB, sizeof bmiB);
    bmiB.bmiHeader.biSize        = sizeof bmiB;
    bmiB.bmiHeader.biWidth       = width;
    bmiB.bmiHeader.biHeight      = height;      // positive: bottom-up, as LoadPng
    bmiB.bmiHeader.biPlanes      = 1;
    bmiB.bmiHeader.biBitCount    = dstBits;
    bmiB.bmiHeader.biCompression = BI_RGB;
    bmiB.bmiHeader.biSizeImage   = height * dstStride;

    unsigned char* pB = NULL;
    hbmp = CreateDIBSection (NULL, &bmiB, DIB_RGB_COLORS, (void**) &pB, NULL, 0);
    if (!hbmp || !pB)
    {
        hbmp = NULL;
        return eUnableToLoadImage;   // headless before the DIB store, as LoadPng
    }

    const unsigned char* src = &file[offBits];

    for (long row = 0; row < height; row++)
    {
        // The DIB is bottom-up.  A bottom-up source copies straight across; a
        // top-down one is read from the far end.
        const unsigned char* s = src + (size_t) srcStride *
                                 (topDown ? (size_t) (height - 1 - row) : (size_t) row);
        unsigned char* d = pB + (size_t) dstStride * (size_t) row;

        if (bitCount == 8)
        {
            for (long x = 0; x < width; x++)
            {
                const unsigned char idx = s[x];
                if (idx >= paletteEntries)
                {
                    // Out-of-range index: leave the pixel at the zeroed value
                    // rather than reading past the palette.
                    d += 3;
                    continue;
                }
                const unsigned char* e = palette + (size_t) idx * 4;
                *d++ = e[0];   // blue
                *d++ = e[1];   // green
                *d++ = e[2];   // red
            }
        }
        else
        {
            // 24 and 32 bpp are already in the DIB's own BGR(A) order.
            memcpy (d, s, (size_t) width * (bitCount / 8));
        }
    }

    return eOK;
}   // end of LoadBmp

// ── ::LoadImage ────────────────────────────────────────────────────────────
// Windows has the real one: winuser.h #defines LoadImage to LoadImageA and
// declares it __stdcall dllimport, so a definition of it here is a redefinition
// that does not compile.  Only the port needs the stand-in below.
#ifndef _WIN32

// Declared in platform/mfc_ui.h.  Only the shape MUSHclient actually uses is
// served: a .bmp FILE loaded into a DIB section, which is what
// world.WindowLoadImage, world.SetBackgroundImage and world.SetForegroundImage
// ask for.  The RESOURCE form (Splash.cpp, mainfrm.cpp's tray icon) still
// answers null -- those want a bitmap out of the Windows resource fork, which
// this port does not have, and both callers already degrade on null.
HANDLE LoadImage (HINSTANCE hInst, LPCTSTR name, UINT type, int, int, UINT flags)
{
    if (hInst != NULL || type != IMAGE_BITMAP || !(flags & LR_LOADFROMFILE) ||
        name == NULL || (uintptr_t) name <= 0xFFFF)
        return nullptr;

    HBITMAP hbmp = NULL;
    const long rc = LoadBmp (name, hbmp);
    if (rc == eOK)
        return (HANDLE) hbmp;

    // Set the last error the way Win32's ::LoadImage does, because all three
    // callers re-derive the REASON from it rather than from a return code:
    // `if (GetLastError () == 2) return eFileNotFound; return e...`.  Leaving
    // errno alone made that read whatever the last unrelated syscall had left,
    // so an unreadable-but-present .bmp could be reported as "file not found"
    // purely because something earlier had failed to open something else.
    //   2 == ENOENT here and == ERROR_FILE_NOT_FOUND on Windows, which is the
    // coincidence the shared code already leans on.
    SetLastError ((rc == eFileNotFound) ? 2 : 0);
    return nullptr;
}

#endif // _WIN32
