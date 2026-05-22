#pragma once

#ifndef _WIN32
#  include "platform/mfc_string.h"
#  include "platform/windows_types.h"
#  include <cstdarg>
#endif

/////////////////////////////////////////////////////////////////////////////
// CFormat

struct CFormat : CString
  {
  CFormat (LPCTSTR lpszFormat, ...);
  CFormat (UINT nFormatID, ...);
  CFormat (LPCTSTR lpszFormat, va_list argList);
  CFormat (UINT nFormatID, va_list argList);
  };

#ifndef _WIN32
// Inline implementations for non-Windows builds (format.cpp is Windows-only).
inline CFormat::CFormat(LPCTSTR lpszFormat, ...) {
    va_list args; va_start(args, lpszFormat);
    FormatV(lpszFormat, args);
    va_end(args);
}
inline CFormat::CFormat(UINT /*nFormatID*/, ...) {
    // Resource strings unavailable on non-Windows; leave empty.
}
inline CFormat::CFormat(LPCTSTR lpszFormat, va_list argList) {
    FormatV(lpszFormat, argList);
}
inline CFormat::CFormat(UINT /*nFormatID*/, va_list /*argList*/) {
    // Resource strings unavailable on non-Windows; leave empty.
}
#endif

