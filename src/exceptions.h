
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

/////////////////////////////////////////////////////////////////////////////
// CSystemException

void ThrowSystemException(DWORD dwError = 0);

class CSystemException : public CException
{
	// Construction
	DECLARE_DYNAMIC(CSystemException)
public:
	CSystemException(DWORD dwError);

	// Attributes
public:
	DWORD	m_dwError;

	// Implementation
public:
	virtual BOOL GetErrorMessage(LPTSTR lpszError, UINT nMaxError, PUINT pnHelpContext = NULL);
};


/////////////////////////////////////////////////////////////////////////////
// CErrorException

void ThrowErrorException(UINT nFormatID, ...);
void ThrowErrorException(LPCTSTR lpszFormat, ...) MC_PRINTF_FORMAT(1, 2);

class CErrorException : public CException
{
	// Construction
	DECLARE_DYNAMIC(CErrorException)
public:
	CErrorException(LPCTSTR lpszFormat, ...) MC_PRINTF_FORMAT(2, 3);
	CErrorException(UINT nFormatID, ...);

	// Attributes
public:
	CString	m_strError;

	// Implementation
public:
	virtual BOOL GetErrorMessage(LPTSTR lpszError, UINT nMaxError, PUINT pnHelpContext = NULL);
};


/////////////////////////////////////////////////////////////////////////////
// CHECKHRESULT_EXCEPTION

// Use this for calls which return the high-order bit set on an error (ie. on a COM
// call).

struct CHECKHRESULT_EXCEPTION
  {
  CHECKHRESULT_EXCEPTION (HRESULT hr = 0) { if (FAILED(hr)) ThrowSystemException (hr); }
  HRESULT operator = (HRESULT hr)         { if (FAILED(hr)) ThrowSystemException (hr); return hr; }
  };

/////////////////////////////////////////////////////////////////////////////
// CHECKOSSTATUS_EXCEPTION

// Use this for calls which return 0 if successfull, but on failure the error is
//  returned directly.

struct CHECKOSSTATUS_EXCEPTION
  {
  CHECKOSSTATUS_EXCEPTION (long i = 0) { if (i != 0) ThrowSystemException (i); }
  long operator = (long i)             { if (i != 0) ThrowSystemException (i); return i; }
  };

/////////////////////////////////////////////////////////////////////////////
// CHECKZERO_EXCEPTION

// Use this for calls which return 0 if successfull, but on failure the error is
//  returned in GetLastError ()

struct CHECKZERO_EXCEPTION
  {
  CHECKZERO_EXCEPTION (long i = 0) { if (i != 0) ThrowSystemException (); }
  long operator = (long i)         { if (i != 0) ThrowSystemException (); return i; }
  };

/////////////////////////////////////////////////////////////////////////////
// CHECKNONZERO_EXCEPTION

// Use this for calls which return non-zero if successfull, but on failure the error is
//  returned in GetLastError ()

struct CHECKNONZERO_EXCEPTION
  {
  CHECKNONZERO_EXCEPTION (long i = 1) { if (i == 0) ThrowSystemException (); }
  long operator = (long i)            { if (i == 0) ThrowSystemException (); return i; }
  };

/////////////////////////////////////////////////////////////////////////////
// CHECKNOTMINUSONE_EXCEPTION

// Use this for calls which return -1 on an error, but any other value if OK.

struct CHECKNOTMINUSONE_EXCEPTION
  {
  CHECKNOTMINUSONE_EXCEPTION (long i = 0) { if (i == -1) ThrowSystemException (); }
  long operator = (long i)                { if (i == -1) ThrowSystemException (); return i; }
  };

