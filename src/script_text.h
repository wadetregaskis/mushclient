// script_text.h — world text on its way to a script, as a BSTR.
//
// A world's text is bytes, in whatever encoding that world uses.  World methods
// return strings in a BSTR, and on Windows MFC makes one by converting through
// the process's code page, which is lossless only when every byte is a valid
// character in it.  On a double-byte system, or with the process on UTF-8 (the
// 64-bit builds' manifest), bytes that are not valid there -- a Latin-1 world's
// "caf\xE9", a GBK plugin's literals -- came back to Lua or Python as U+FFFD.
//
// So in the 64-bit builds, whose scripts are all Lua or embedded engines, a BSTR
// from a world method is just a carrier: each byte as one Windows-1252 character
// (MC_BstrFromBytes), read back the same way.  It is never shown to anyone as
// text.  Win32 keeps real UTF-16 BSTRs, because its VBScript and JScript read them
// as text through ActiveScript, and it has no manifest change to be damaged by.
//
// Use these rather than CString::AllocSysString, CString (BSTR) and COleVariant in
// anything a script receives.

#pragma once

#if defined(_WIN32) && defined(MUSHCLIENT_EMBEDDED_ENGINES)

BSTR    ScriptBstr  (const CString & strBytes);    // free with SysFreeString
CString ScriptBytes (const BSTR b);                // null -> ""

class CScriptVariant : public COleVariant
  {
  public:
    CScriptVariant () {}
    CScriptVariant (LPCTSTR s)         { SetBytes (s); }
    CScriptVariant (const CString & s) { SetBytes (s); }
    template <class T> explicit CScriptVariant (T v) : COleVariant (v) {}

    CScriptVariant & operator= (LPCTSTR s)         { Clear (); SetBytes (s); return *this; }
    CScriptVariant & operator= (const CString & s) { Clear (); SetBytes (s); return *this; }
    template <class T> CScriptVariant & operator= (T v) { COleVariant::operator= (v); return *this; }

  private:
    void SetBytes (const CString & s) { vt = VT_BSTR; bstrVal = ScriptBstr (s); }
  };

#else

inline BSTR    ScriptBstr  (const CString & strBytes) { return strBytes.AllocSysString (); }
inline CString ScriptBytes (const BSTR b)             { return CString (b); }
typedef COleVariant CScriptVariant;

#endif
