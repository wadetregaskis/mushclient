// utilities_portable.cpp
//
// Portable subset of Utilities.cpp — the non-Windows copies of functions that
// still differ from their Windows originals (behavioural drift) or that touch
// platform state, so they cannot yet be single-sourced.  On Windows the full
// Utilities.cpp is compiled instead (via the .vcxproj).  This file is compiled
// only on non-Windows via CMakeLists.txt.
//
// The genuinely platform-free, behaviourally-identical helpers (myAtoF,
// encodeBase64, EditDistance) have moved to utilities_shared.cpp, which every
// build compiles.  What remains here:
//   • IsStringNumber   — portable honours bSigned; Windows ignores it (drift)
//   • ShutDownSocket    — drives CAsyncSocket (not platform-free)
//   • getBase64Value / decodeBase64 — portable dropped Windows' throw + fixed an
//                         over-read on malformed input (drift)
//   • SeeIfBase64       — clean, but calls the drifted decodeBase64 and its
//                         Windows home is xml/xml_serialize.cpp, not Utilities.cpp

#ifndef _WIN32

#include "platform/posix_socket.h"
#include "platform/doc_stub.h"   // CString
#include <string>
#include <cstring>
using std::string;

// Declared in stdafx.h for all platforms:
//   bool IsStringNumber(const string & s, const bool bSigned = false);

bool IsStringNumber(const string & s, const bool bSigned)
{
    string::size_type pos = 0;

    if (s.empty())
        return false;  // empty? can't be a number

    if (bSigned && (s[0] == '+' || s[0] == '-'))
    {
        pos++;
        if (s.size() == 1)
            return false;  // can't just have the sign
    }

    return s.find_first_not_of("0123456789", pos) == string::npos;
}

// ── ShutDownSocket ────────────────────────────────────────────────────────────
// Gracefully shuts down and closes a socket.  On Windows this lives in
// Utilities.cpp; on macOS we provide the equivalent here.
void ShutDownSocket(CAsyncSocket& s)
{
    if (s.m_hSocket != INVALID_SOCKET) {
        s.AsyncSelect(0);           // stop interest in events
        s.ShutDown(CAsyncSocket::both);
        s.Close();
    }
}

// ── Base-64 helpers ───────────────────────────────────────────────────────────
// On Windows/mushapp decodeBase64 + getBase64Value live in Utilities.cpp and
// SeeIfBase64 lives in xml/xml_serialize.cpp; xmlparse.cpp needs them on every
// platform, so mushcore provides all three here.  (These stay portable-only —
// not in the shared TU — because decodeBase64/getBase64Value have drifted from
// their Windows originals: the port dropped Windows' ThrowErrorException and
// fixed an over-read on malformed input.)
// Declared in stdafx.h: CString decodeBase64(const char *coded);
//                        void SeeIfBase64(CString &strText);

static int getBase64Value(char code)
{
    int v = (int)(unsigned char)code;
    if (v >= 'A' && v <= 'Z') return v - 65;
    if (v >= 'a' && v <= 'z') return v - 71;
    if (v >= '0' && v <= '9') return v + 4;
    if (v == '+') return 62;
    if (v == '/') return 63;
    if (v == '=') return 0;
    // Invalid character — skip gracefully
    return 0;
}

CString decodeBase64(const char* coded)
{
    CString strEncoded(coded);
    strEncoded.Remove(' ');
    strEncoded.Remove('\t');
    strEncoded.Remove('\n');
    strEncoded.Remove('\r');
    strEncoded.Remove('\f');
    strEncoded.Remove('\v');

    const char* p = static_cast<const char*>(strEncoded);
    int i = (int)strlen(p);

    CString strResult;
    char* d = strResult.GetBuffer(((i / 4) * 3) + 1);
    int j = 0;

    // Only decode complete 4-character groups. A non-mod-4 length is a truncated
    // / invalid string; ignore the trailing partial group rather than reading
    // past the NUL terminator — the original `for (; *p; p += 4)` over-read and
    // over-wrote the (undersized) buffer on such input (e.g. "abc").
    int full = (i / 4) * 4;
    for (int k = 0; k < full; k += 4) {
        d[j++] = (char)(getBase64Value(p[k])     << 2 | getBase64Value(p[k + 1]) >> 4);
        d[j++] = (char)(getBase64Value(p[k + 1]) << 4 | getBase64Value(p[k + 2]) >> 2);
        d[j++] = (char)(getBase64Value(p[k + 2]) << 6 | getBase64Value(p[k + 3]));
    }
    d[j] = 0;

    // Trim padding bytes (the '=' chars at the end of the last full group).
    if (full > 0 && p[full - 1] == '=') {
        if (full > 1 && p[full - 2] == '=') d[j - 2] = '\0';
        else                                 d[j - 1] = '\0';
    }

    strResult.ReleaseBuffer(-1);
    return strResult;
}

void SeeIfBase64(CString& strText)
{
    const char* p = static_cast<const char*>(strText);
    char c;
    bool bBase64 = true;
    int iCount = 0;

    while ((c = *p++) != '\0') {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v')
            continue;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=') {
            iCount++;
        } else {
            bBase64 = false;
            break;
        }
    }

    if (bBase64 && (iCount % 4 == 0))
        strText = decodeBase64(strText);
}

#endif // !_WIN32
