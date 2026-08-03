
// ── printf-style argument checking ────────────────────────────────────────
// Real on GCC/clang, silently nothing on MSVC (which has no equivalent
// attribute), so the Windows build is unaffected.  Argument numbers are 1-based
// and COUNT the implicit `this` for non-static member functions and
// constructors — so a member taking (fmt, ...) is (2, 3), a free function (1, 2).
#ifndef MC_PRINTF_FORMAT
#  if defined(__GNUC__) || defined(__clang__)
#    define MC_PRINTF_FORMAT(fmtArg, firstVararg) \
            __attribute__((format(printf, fmtArg, firstVararg)))
#  else
#    define MC_PRINTF_FORMAT(fmtArg, firstVararg)
#  endif
#endif
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
  CFormat (LPCTSTR lpszFormat, ...) MC_PRINTF_FORMAT(2, 3);
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

