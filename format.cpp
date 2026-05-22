// Format.cpp - format CStrings on-the-fly
//

#include "stdafx.h"

// On non-MSVC platforms, format.h already provides inline implementations
// of the CFormat constructors.  Compiling the bodies below would cause
// redefinition errors, so we skip the rest of this file.
#ifndef _MSC_VER
// (nothing to define — inline versions in format.h are sufficient)
#else

#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char BASED_CODE THIS_FILE[] = __FILE__;
#endif


/////////////////////////////////////////////////////////////////////////////
// CFormat

CFormat::CFormat(LPCTSTR lpszFormat, ...)
{
	ASSERT(AfxIsValidString(lpszFormat, FALSE));

	va_list argList;
	va_start(argList, lpszFormat);
	FormatV(lpszFormat, argList);
	va_end(argList);
}

CFormat::CFormat(UINT nFormatID, ...)
{
	CString strFormat;
	VERIFY(strFormat.LoadString(nFormatID) != 0);

	va_list argList;
	va_start(argList, nFormatID);
	FormatV(strFormat, argList);
	va_end(argList);
}

CFormat::CFormat (LPCTSTR lpszFormat, va_list argList)
  {
	ASSERT(AfxIsValidString(lpszFormat, FALSE));
 
	FormatV(lpszFormat, argList);
  }

CFormat::CFormat (UINT nFormatID, va_list argList)
  {
	CString strFormat;
	VERIFY(strFormat.LoadString(nFormatID) != 0);

	FormatV(strFormat, argList);
  }


#endif // _MSC_VER
