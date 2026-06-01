// platform/mfc_exceptions.h
// Minimal MFC exception stubs for non-Windows platforms.
//
// The existing exceptions.h / exceptions.cpp use CException (MFC base),
// CString, HRESULT, etc.  We only need the bare minimum to compile.

#pragma once

#ifndef _WIN32

#include <stdexcept>
#include <string>
#include "windows_types.h"
#include "mfc_string.h"
#include "mfc_object.h"   // CRuntimeClass + SHIM_RUNTIME_CLASS_ROOT (RTTI)

// ── CException base ───────────────────────────────────────────────────────
// MFC CException is the root of all MFC exceptions.

class CException : public std::exception
{
    SHIM_RUNTIME_CLASS_ROOT(CException)
public:
    virtual ~CException() {}
    // MFC signature: writes error message into buffer, returns TRUE on success.
    virtual BOOL GetErrorMessage(LPTSTR lpszError, UINT nMaxError,
                                 UINT* /*pnHelpContext*/ = nullptr) {
        if (lpszError && nMaxError > 0) lpszError[0] = '\0';
        return FALSE;
    }
    // Display error message — stub does nothing on non-Windows.
    virtual BOOL ReportError(UINT = 0xFFFF, UINT = 0) { return FALSE; }
    void Delete() { delete this; }
};

// ── CMemoryException ──────────────────────────────────────────────────────

class CMemoryException : public CException {};
inline void AfxThrowMemoryException() { throw std::bad_alloc(); }

// ── CUserException ────────────────────────────────────────────────────────

class CUserException : public CException {};

// ── CFileException ────────────────────────────────────────────────────────

class CFileException : public CException {
public:
    int   m_cause    = 0;
    LONG  m_lOsError = 0;
    CString m_strFileName;
    enum {
        none = 0, generic, fileNotFound, badPath, tooManyOpenFiles,
        accessDenied, invalidFile, removeCurrentDir, directoryFull,
        badSeek, hardIO, sharingViolation, lockViolation, diskFull, endOfFile
    };
    explicit CFileException(int cause = none, LONG osErr = -1, LPCTSTR name = nullptr)
        : m_cause(cause), m_lOsError(osErr) { if (name) m_strFileName = name; }
    static void ThrowOsError(LONG lOsError, LPCTSTR name = nullptr) {
        throw new CFileException(generic, lOsError, name);
    }
    static void ThrowErrno(int nErrno, LPCTSTR name = nullptr) {
        throw new CFileException(generic, nErrno, name);
    }
};

// ── COleException ─────────────────────────────────────────────────────────

class COleException : public CException {
public:
    HRESULT m_sc = E_FAIL;
    void Delete() { delete this; }
};

// ── CNotSupportedException ────────────────────────────────────────────────

class CNotSupportedException : public CException {};
inline void AfxThrowNotSupportedException() { throw new CNotSupportedException(); }

// ── CInvalidArgException ──────────────────────────────────────────────────

class CInvalidArgException : public CException {};
inline void AfxThrowInvalidArgException() { throw new CInvalidArgException(); }

// ── Helper: CATCH / AND_CATCH / END_CATCH macros ─────────────────────────
// MFC uses these; we map them to try/catch syntax so the callers compile.
// (The source uses the C++ try/catch directly in most places.)

#define CATCH(class, e)        catch (class* e)
#define AND_CATCH(class, e)    catch (class* e)
#define END_CATCH

#endif // !_WIN32
