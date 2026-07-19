// platform/mfc_exceptions.h
// Minimal MFC exception stubs for non-Windows platforms.
//
// The existing exceptions.h / exceptions.cpp use CException (MFC base),
// CString, HRESULT, etc.  We only need the bare minimum to compile.

#pragma once

#ifndef _WIN32

#include <stdexcept>
#include <string>
#include <cstring>   // strncpy/strerror (ReportError / CFileException messages)
#include <cstdio>    // snprintf (CFileException::GetErrorMessage)
#include <cerrno>    // errno constants (CFileException::ErrnoToException)
#include "windows_types.h"
#include "mfc_string.h"
#include "mfc_object.h"   // CRuntimeClass + SHIM_RUNTIME_CLASS_ROOT (RTTI)

// Defined in platform/windows_stubs.cpp (mushcore) / mushapp_stubs.cpp
// (mushapp): routes through the wx message-box hook in the GUI, stderr headless.
int TMessageBox(const char* sText, UINT nType);

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
    // MFC-faithful: display the exception's message (the virtual
    // GetErrorMessage — CFileException/CErrorException/CSystemException all
    // override it).  Canonical catch blocks do `e->ReportError()` expecting the
    // user to SEE the failure; the old always-silent stub swallowed it.
    virtual BOOL ReportError(UINT nType = MB_OK, UINT /*nMessageID*/ = 0) {
        char szError[512] = "";
        if (!GetErrorMessage(szError, sizeof szError) || szError[0] == '\0')
            strncpy(szError, "An unknown error has occurred.", sizeof szError - 1);
        return TMessageBox(szError, nType);
    }
    void Delete() { delete this; }
};

// ── CMemoryException ──────────────────────────────────────────────────────

class CMemoryException : public CException {};
// MFC throws a CMemoryException* (pointer); the canonical source catches it as
// `catch (CMemoryException* e)` / `catch (CException* e)`. Throwing std::bad_alloc
// by value would slip past every such handler and terminate the app (the same
// hazard already fixed for AfxThrowArchiveException — see windows_types.h).
inline void AfxThrowMemoryException() { throw new CMemoryException(); }

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

    // Map an errno value onto the MFC cause codes (MFC CFileException::
    // ErrnoToException equivalent, POSIX errnos).  Callers read m_cause —
    // e.g. doc.cpp OnGamePastefile tests against CFileException::endOfFile.
    static int ErrnoToException(int nErrno) {
        switch (nErrno) {
            case 0:       return none;
            case ENOENT:  return fileNotFound;
            case ENOTDIR: return badPath;
            case EPERM: case EACCES: case EROFS:
            case EISDIR: case ETXTBSY: return accessDenied;
            case EMFILE: case ENFILE:  return tooManyOpenFiles;
            case ENOSPC: case EDQUOT:  return diskFull;
            case EBADF: case EINVAL:   return invalidFile;
            default:      return generic;
        }
    }

    // MFC-style human-readable message so ReportError shows something useful.
    BOOL GetErrorMessage(LPTSTR lpszError, UINT nMaxError,
                         UINT* /*pnHelpContext*/ = nullptr) override {
        if (!lpszError || nMaxError == 0) return FALSE;
        const char* name = m_strFileName.IsEmpty () ? "an unnamed file"
                                                    : (LPCTSTR) m_strFileName;
        const char* what;
        switch (m_cause) {
            case fileNotFound:     what = "%s was not found";                  break;
            case badPath:          what = "%s contains an invalid path";       break;
            case tooManyOpenFiles: what = "%s could not be opened because there are too many open files"; break;
            case accessDenied:     what = "Access to %s was denied";           break;
            case invalidFile:      what = "An attempt was made to access %s in an invalid way"; break;
            case directoryFull:    what = "%s could not be created because the directory is full"; break;
            case badSeek:          what = "An attempt was made to set the file pointer of %s to an invalid position"; break;
            case hardIO:           what = "A hardware I/O error was reported while accessing %s"; break;
            case sharingViolation: what = "A sharing violation occurred while accessing %s"; break;
            case lockViolation:    what = "A locking violation occurred while accessing %s"; break;
            case diskFull:         what = "The disk is full while accessing %s"; break;
            case endOfFile:        what = "An attempt was made to access %s past its end"; break;
            default:               what = "An error occurred while accessing %s"; break;
        }
        snprintf(lpszError, nMaxError, what, name);
        // append ": <strerror>" when we have the OS error (POSIX errno)
        if (m_lOsError > 0) {
            size_t len = strlen(lpszError);
            if (len < nMaxError - 1)
                snprintf(lpszError + len, nMaxError - len, ": %s",
                         strerror((int) m_lOsError));
        }
        return TRUE;
    }

    static void ThrowOsError(LONG lOsError, LPCTSTR name = nullptr) {
        // POSIX: OS error numbers ARE errno values
        throw new CFileException(ErrnoToException((int) lOsError), lOsError, name);
    }
    static void ThrowErrno(int nErrno, LPCTSTR name = nullptr) {
        throw new CFileException(ErrnoToException(nErrno), nErrno, name);
    }
};

// MFC global thrower (heap CFileException*, caller does e->Delete()) — thrown
// by the shim CFile path-constructor on open failure, matching real MFC.
inline void AfxThrowFileException(int cause, LONG lOsError = -1,
                                  LPCTSTR lpszFileName = nullptr) {
    throw new CFileException(cause, lOsError, lpszFileName);
}

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
