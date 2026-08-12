// png_load_shared.cpp
//
// LoadPng / LoadPngMemory — decode a PNG (from a file, or from memory) into a
// Windows-shaped HBITMAP.  Declared in stdafx.h.  Compiled by every build.
//
// Moved verbatim out of Utilities.cpp, which is mushgui_adapters-only, so that
// the adopted miniwindow family links headless: WindowLoadImage and
// WindowLoadImageMemory are the only two callers that matter, and they are the
// image half of that family (docs/STUB-ADOPTION-2026-08.md §5 step 3).
//
// It reaches the GDI shim through exactly two entry points, ::CreateDIBSection
// and CBitmap-shaped handles, so what it does depends on which shim is linked:
// the wx build gets real pixels (platform/wx_gdi_compat.cpp), and the headless
// build's ::CreateDIBSection returns nullptr (platform/gdi_headless.cpp), which
// makes both functions report eUnableToLoadImage rather than crash.  That is the
// correct Stage-1 behaviour — the miniwindow exists and its geometry is real,
// only the pixels are absent — and it becomes real when the headless DIB store
// lands in Stage 2.
//
// The two static png_* callbacks and BytesPerLine come along because they are
// private to these two functions and nothing else in Utilities.cpp used them.

#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "scripting/errors.h"
#include "png/png.h"

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

static int BytesPerLine (int nWidth, int nBitsPerPixel)
  {
  return ( (nWidth * nBitsPerPixel + 31) & (~31) ) / 8;
  }

long LoadPng (LPCTSTR FileName, HBITMAP & hbmp)  // load a PNG file into a bitmap
  {
  // open file
#ifdef _WIN32
  FILE *fp = fopen(FileName, "rb");
#else
  // Plugins (e.g. Quow's Cow Bar map tiles) reference images by Windows-style
  // backslash paths; POSIX needs '/'.
  FILE *fp = fopen(PortableFilePath(FileName).c_str(), "rb");
#endif
  if (!fp)
      return (eFileNotFound);

unsigned char header [8];

  // read signature
  if (fread(header, 1, sizeof header, fp) != sizeof header)
    {
    fclose (fp);
    return (eUnableToLoadImage);    // file too small
    }

  // check signature
  if (png_sig_cmp(header, 0, sizeof header))
    {
    fclose (fp);
    return (eUnableToLoadImage);   // bad signature
    }

  // create PNG structure
  png_structp png_ptr = png_create_read_struct
       (PNG_LIBPNG_VER_STRING, NULL,
        user_error_fn, user_warning_fn);

  if (!png_ptr)
    {
    fclose (fp);
    return eUnableToLoadImage;
    }

  // create info structure
  png_infop info_ptr = png_create_info_struct(png_ptr);
  if (!info_ptr)
    {
    png_destroy_read_struct(&png_ptr,
       (png_infopp)NULL, (png_infopp)NULL);
    fclose (fp);
    return eUnableToLoadImage;
    }

  // create end info structure
  png_infop end_info = png_create_info_struct(png_ptr);
  if (!end_info)
    {
    png_destroy_read_struct(&png_ptr, &info_ptr,
      (png_infopp)NULL);
    fclose (fp);
    return eUnableToLoadImage;
    }

#ifdef _MSC_VER
#pragma warning (push)
#pragma warning (disable : 4611)  // interaction between '_setjmp' and C++ object destruction is non-portable
#endif

  // if png fails it will longjmp back to here, so we destroy the structure,
  // close the file, and wrap up
  if (setjmp(png_jmpbuf(png_ptr)))
    {
    png_destroy_read_struct(&png_ptr, &info_ptr,
       &end_info);
    fclose (fp);
    return eUnableToLoadImage;
    }

#ifdef _MSC_VER
#pragma warning (pop)
#endif

  // initialize IO
  png_init_io (png_ptr, fp);

  // tell it we have read the sig
  png_set_sig_bytes(png_ptr, sizeof header);

  int png_transforms = PNG_TRANSFORM_STRIP_16 |    // Strip 16-bit samples to 8 bits
                       PNG_TRANSFORM_PACKING |     // Expand 1, 2 and 4-bit samples to bytes
                       PNG_TRANSFORM_EXPAND  |     // expand out to 8 bits if less
                       PNG_TRANSFORM_BGR;          // Flip RGB to BGR, RGBA to BGRA

  // read the file
  png_read_png(png_ptr, info_ptr, png_transforms, NULL);

  // note: png_read_end not needed with png_read_png (done already)

  // get the row pointers
  png_bytep * row_pointers = png_get_rows(png_ptr, info_ptr);

  // make a bitmap
  CDC B_DC;
  B_DC.CreateCompatibleDC (NULL);
  CBitmap B_bmp;

  BITMAPINFO bmiB;
  ZeroMemory (&bmiB, sizeof bmiB);

  int bit_per_pixel = 24;

  if (png_get_color_type (png_ptr, info_ptr) & PNG_COLOR_MASK_ALPHA)
     bit_per_pixel = 32;

  long bpl = BytesPerLine (png_get_image_width (png_ptr, info_ptr), bit_per_pixel);

  bmiB.bmiHeader.biSize = sizeof bmiB;
  bmiB.bmiHeader.biWidth =          png_get_image_width (png_ptr, info_ptr);
  bmiB.bmiHeader.biHeight =         png_get_image_height (png_ptr, info_ptr);
  bmiB.bmiHeader.biPlanes =         1;
  bmiB.bmiHeader.biBitCount =       bit_per_pixel;
  bmiB.bmiHeader.biCompression =    BI_RGB;
  bmiB.bmiHeader.biSizeImage =      png_get_image_height (png_ptr, info_ptr) * bpl;

  unsigned char * pB = NULL;

  hbmp = CreateDIBSection(NULL, &bmiB, DIB_RGB_COLORS, (void**) &pB, NULL, 0);

  if (!hbmp)
    {
    // Free what the success path frees -- this early return used to leak the
    // open FILE* and all three libpng structs on every failed load.
    png_destroy_read_struct(&png_ptr, &info_ptr, &end_info);
    fclose(fp);
    return eUnableToLoadImage;
    }

  long row;
  unsigned char * p = pB;

  // have to reverse row order

  // Copy only what libpng ALLOCATED, not the whole DIB stride.  bpl is padded
  // to a 4-byte boundary; png_read_png allocates each row at exactly
  // png_get_rowbytes(), so for any 24bpp image whose width is not a multiple
  // of 4 this memcpy used to read 1-3 bytes past the end of every row (proved
  // under AddressSanitizer: a 7-wide image reads 24 from a 21-byte
  // allocation).  The DIB comes back zeroed, so the pad bytes stay 0 instead
  // of picking up heap contents — and since nothing ever reads them as pixels,
  // not one visible pixel changes on either platform.
  const size_t rowBytes = (size_t) png_get_rowbytes (png_ptr, info_ptr);
  const size_t copyBytes = rowBytes < (size_t) bpl ? rowBytes : (size_t) bpl;

  png_uint_32 iHeight = png_get_image_height (png_ptr, info_ptr);
  for (row = 0; row < iHeight; row++, p += bpl)
     memcpy (p, row_pointers [iHeight - row - 1], copyBytes);

  // done with data
  png_destroy_read_struct(&png_ptr, &info_ptr, &end_info);

  // done with file
  fclose(fp);

  return eOK;
  }     // end of  LoadPng

typedef struct
  {
  unsigned char * BufferPos;
  size_t LengthRemaining;
  } tPngBufferInfo;

void ReadDataFromInputStream(png_structp png_ptr, png_bytep outBytes,
                      png_size_t byteCountToRead)
{
   if(png_get_io_ptr (png_ptr) == NULL)
     {
     png_error(png_ptr, "No io_ptr structure");
     return;   // oops
     }

   // get our structure with buffer and length left in it
   tPngBufferInfo * PngBufferInfo = (tPngBufferInfo*)png_get_io_ptr (png_ptr);

   if (byteCountToRead > PngBufferInfo->LengthRemaining)
     {
     png_error(png_ptr, "Insufficient data in buffer");
     return;
     }

   // copy wanted bytes
   memcpy (outBytes, PngBufferInfo->BufferPos, byteCountToRead);
   PngBufferInfo->BufferPos += byteCountToRead;
   PngBufferInfo->LengthRemaining -= byteCountToRead;

}  // end of ReadDataFromInputStream

long LoadPngMemory (unsigned char * Buffer, const size_t Length, HBITMAP & hbmp, const bool bAlpha)  // load a PNG file into a bitmap
  {

  if (Length < 8)
    return (eUnableToLoadImage);    // too small for signature

  // check signature
  if (png_sig_cmp (Buffer, 0, 8))
    return (eUnableToLoadImage);   // bad signature

  // create PNG structure
  png_structp png_ptr = png_create_read_struct
       (PNG_LIBPNG_VER_STRING, NULL,
        user_error_fn, user_warning_fn);

  if (!png_ptr)
    return eUnableToLoadImage;

  // create info structure
  png_infop info_ptr = png_create_info_struct(png_ptr);
  if (!info_ptr)
    {
    png_destroy_read_struct(&png_ptr,
       (png_infopp)NULL, (png_infopp)NULL);
    return eUnableToLoadImage;
    }

  // create end info structure
  png_infop end_info = png_create_info_struct(png_ptr);
  if (!end_info)
    {
    png_destroy_read_struct(&png_ptr, &info_ptr,
      (png_infopp)NULL);
    return eUnableToLoadImage;
    }

#ifdef _MSC_VER
#pragma warning (push)
#pragma warning (disable : 4611)  // interaction between '_setjmp' and C++ object destruction is non-portable
#endif

  // if png fails it will longjmp back to here, so we destroy the structure,
  // and wrap up
  if (setjmp(png_jmpbuf(png_ptr)))
    {
    png_destroy_read_struct(&png_ptr, &info_ptr,
       &end_info);
    return eUnableToLoadImage;
    }

#ifdef _MSC_VER
#pragma warning (pop)
#endif

  tPngBufferInfo PngBufferInfo;
  PngBufferInfo.BufferPos = Buffer;
  PngBufferInfo.LengthRemaining = Length;

  // initialize IO
  png_set_read_fn(png_ptr, &PngBufferInfo, ReadDataFromInputStream);


  int png_transforms = PNG_TRANSFORM_STRIP_16 |    // Strip 16-bit samples to 8 bits
                       PNG_TRANSFORM_PACKING |     // Expand 1, 2 and 4-bit samples to bytes
                       PNG_TRANSFORM_BGR;          // Flip RGB to BGR, RGBA to BGRA


  if (bAlpha)
    png_transforms |= PNG_TRANSFORM_SWAP_ALPHA;

  // read the file
  png_read_png(png_ptr, info_ptr, png_transforms, NULL);

  // note: png_read_end not needed with png_read_png (done already)

  // get the row pointers
  png_bytep * row_pointers = png_get_rows(png_ptr, info_ptr);

  // make a bitmap
  CDC B_DC;
  B_DC.CreateCompatibleDC (NULL);
  CBitmap B_bmp;

  BITMAPINFO bmiB;
  ZeroMemory (&bmiB, sizeof bmiB);

  int bit_per_pixel = 24;

  if (png_get_color_type (png_ptr, info_ptr) & PNG_COLOR_MASK_ALPHA)
     bit_per_pixel = 32;

  long bpl = BytesPerLine (png_get_image_width (png_ptr, info_ptr), bit_per_pixel);

  bmiB.bmiHeader.biSize = sizeof bmiB;
  bmiB.bmiHeader.biWidth =          png_get_image_width (png_ptr, info_ptr);
  bmiB.bmiHeader.biHeight =         png_get_image_height (png_ptr, info_ptr);
  bmiB.bmiHeader.biPlanes =         1;
  bmiB.bmiHeader.biBitCount =       bit_per_pixel;
  bmiB.bmiHeader.biCompression =    BI_RGB;
  bmiB.bmiHeader.biSizeImage =      png_get_image_height (png_ptr, info_ptr) * bpl;

  unsigned char * pB = NULL;

  hbmp = CreateDIBSection(NULL, &bmiB, DIB_RGB_COLORS, (void**) &pB, NULL, 0);

  if (!hbmp)
    {
    // Same leak as LoadPng's arm above, minus the FILE* (memory source).
    png_destroy_read_struct(&png_ptr, &info_ptr, &end_info);
    return eUnableToLoadImage;
    }

  long row;
  unsigned char * p = pB;

  // Copy only what libpng allocated — see the identical fix in LoadPng above.
  const size_t rowBytes = (size_t) png_get_rowbytes (png_ptr, info_ptr);
  const size_t copyBytes = rowBytes < (size_t) bpl ? rowBytes : (size_t) bpl;

  // have to reverse row order
  png_uint_32 iHeight = png_get_image_height (png_ptr, info_ptr);
  for (row = 0; row < iHeight; row++, p += bpl)
     memcpy (p, row_pointers [iHeight - row - 1], copyBytes);

  // done with data
  png_destroy_read_struct(&png_ptr, &info_ptr, &end_info);

  return eOK;
  }     // end of  LoadPngMemory