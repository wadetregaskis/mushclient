//  utilities_shared.cpp
//
//  Pure-logic utility functions single-sourced across every build — the Windows
//  client (via MUSHclient.vcxproj), the macOS/Linux product (mushapp) and the
//  headless test engine (mushcore) all compile THIS one definition, so the
//  copies can no longer drift.  Precedent: ansi.cpp, doc_shared.cpp,
//  speedwalk.cpp.
//
//  myAtoF, encodeBase64 and EditDistance were verified byte-for-byte
//  behaviourally identical between the old Windows Utilities.cpp copies and the
//  portable utilities_portable.cpp copies before being merged (cosmetic
//  macro/typedef/min-helper differences only) — Windows behaviour unchanged.
//
//  IsStringNumber, getBase64Value, decodeBase64 and SeeIfBase64 had DRIFTED
//  between the two builds; each was reconciled to a single canonical behaviour
//  before being merged here (see the per-function notes below): IsStringNumber
//  now honours bSigned (the old Windows copy ignored it); the base64 decoders
//  keep Windows' throw-on-invalid contract (the port had dropped it) plus a
//  fix for a 1-byte over-read on empty input.
//
//  Only ShutDownSocket stays out (in utilities_portable.cpp) — it drives a
//  CAsyncSocket and is genuinely platform-specific.

#if defined(_WIN32) || defined(MUSHCLIENT_FULL_BUILD)
  #include "stdafx.h"
#else
  #include "platform/doc_stub.h"   // CString, std::string
#endif

#include <cctype>
#include <cstring>
#include <string>
#include <vector>

using std::string;
using std::vector;

// ── IsStringNumber ──────────────────────────────────────────────────────────
// True if `s` is a run of decimal digits.  A leading '+'/'-' is accepted ONLY
// when bSigned is true (bSigned defaults to false — see the stdafx.h prototype).
// Declared in stdafx.h / platform/doc_stub.h.
//
// This honours bSigned; the pre-2026-07 Windows copy ignored the argument and
// always stripped a leading sign — a latent defect reconciled here (the port
// already behaved correctly).  All callers that must accept negatives pass
// bSigned=true (array_less, miniwindow point-parsing); the only bSigned=false
// caller (regexp wildcard-name test) is unaffected for realistic input.
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

// ── myAtoF ──────────────────────────────────────────────────────────────────
// Locale-independent string→double conversion (sqlite3AtoF-derived); also
// underpins SQLite's text-to-float path.  Returns the number of characters
// consumed.  Declared in stdafx.h / platform/doc_stub.h.
int myAtoF(const char* z, double* pResult)
{
    typedef long double LONGDOUBLE_TYPE;
    typedef unsigned char u8;

    int sign = 1;
    const char* zBegin = z;
    LONGDOUBLE_TYPE v1 = 0.0;
    int nSignificant = 0;

    while (isspace((u8)*z)) z++;
    if (*z == '-')      { sign = -1; z++; }
    else if (*z == '+') { z++; }
    while (z[0] == '0') { z++; }

    while (isdigit((u8)*z)) {
        v1 = v1 * 10.0 + (*z - '0');
        z++;
        nSignificant++;
    }
    if (*z == '.') {
        LONGDOUBLE_TYPE divisor = 1.0;
        z++;
        if (nSignificant == 0) {
            while (z[0] == '0') { divisor *= 10.0; z++; }
        }
        while (isdigit((u8)*z)) {
            if (nSignificant < 18) {
                v1 = v1 * 10.0 + (*z - '0');
                divisor *= 10.0;
                nSignificant++;
            }
            z++;
        }
        v1 /= divisor;
    }
    if (*z == 'e' || *z == 'E') {
        int esign = 1, eval = 0;
        LONGDOUBLE_TYPE scale = 1.0;
        z++;
        if (*z == '-')      { esign = -1; z++; }
        else if (*z == '+') { z++; }
        while (isdigit((u8)*z)) { eval = eval * 10 + *z - '0'; z++; }
        while (eval >= 64) { scale *= 1.0e+64; eval -= 64; }
        while (eval >= 16) { scale *= 1.0e+16; eval -= 16; }
        while (eval >= 4)  { scale *= 1.0e+4;  eval -= 4;  }
        while (eval >= 1)  { scale *= 1.0e+1;  eval -= 1;  }
        if (esign < 0) v1 /= scale; else v1 *= scale;
    }
    *pResult = (double)(sign < 0 ? -v1 : v1);
    return (int)(z - zBegin);
}

// ── encodeBase64 ────────────────────────────────────────────────────────────
// RFC 2045 base64 encoder.  bMultiLine wraps at column 76 (a CRLF after every
// 57 input bytes / 76 output chars).  Declared in stdafx.h / platform/doc_stub.h.
static const char kBase64Code[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

#define BASE64_WRAP_POINT 57  // 76 output chars = 57 input bytes

CString encodeBase64(const char* plaintext, const bool bMultiLine)
{
    const unsigned char* pt = reinterpret_cast<const unsigned char*>(plaintext);
    int len = (int)strlen(plaintext);
    int i = 0, j = 0;

    CString strResult;
    // 4/3 expansion + padding + optional CRLFs
    char* result = strResult.GetBuffer(
        ((len / 3) * 4) + 4 +
        (bMultiLine ? (((len / BASE64_WRAP_POINT) + 1) * 2) : 0));

    for (i = 0; i < len - 2; i += 3) {
        int bytes = (pt[i] << 16) | (pt[i+1] << 8) | pt[i+2];
        result[j++] = kBase64Code[(bytes >> 18) & 0x3F];
        result[j++] = kBase64Code[(bytes >> 12) & 0x3F];
        result[j++] = kBase64Code[(bytes >>  6) & 0x3F];
        result[j++] = kBase64Code[(bytes      ) & 0x3F];
        if (bMultiLine && ((i % BASE64_WRAP_POINT) == (BASE64_WRAP_POINT - 3))) {
            result[j++] = '\r';
            result[j++] = '\n';
        }
    }

    switch (len - i) {
    case 1: {
        int bytes = pt[i] << 16;
        result[j++] = kBase64Code[(bytes >> 18) & 0x3F];
        result[j++] = kBase64Code[(bytes >> 12) & 0x3F];
        result[j++] = '='; result[j++] = '=';
        break;
    }
    case 2: {
        int bytes = (pt[i] << 16) | (pt[i+1] << 8);
        result[j++] = kBase64Code[(bytes >> 18) & 0x3F];
        result[j++] = kBase64Code[(bytes >> 12) & 0x3F];
        result[j++] = kBase64Code[(bytes >>  6) & 0x3F];
        result[j++] = '=';
        break;
    }
    default: break;
    }
    result[j] = '\0';
    strResult.ReleaseBuffer(-1);
    return strResult;
}

#undef BASE64_WRAP_POINT

// ── Base64 decode (getBase64Value + decodeBase64) ────────────────────────────
// Throws ThrowErrorException on invalid input — the intended contract: the
// world.Base64Decode script API wraps decode in try/catch and returns nil on the
// throw (and the notepad path try/catches it too).  The port had silently
// dropped the throw and returned garbage; restored here so the mac product and
// Windows agree.  getBase64Value stays file-static (used only by decodeBase64).
// Declared in stdafx.h / platform/doc_stub.h.

// takes a base64 character, returns its value (RFC 2045 Table 1)
static int getBase64Value(char code)
{
    int val = (int)code;
    int result = 0;

    if      (val >= 'A' && val <= 'Z') result = val - 65;
    else if (val >= 'a' && val <= 'z') result = val - 71;
    else if (val >= '0' && val <= '9') result = val + 4;
    else if (val == '+')               result = 62;
    else if (val == '/')               result = 63;
    else if (val == '=')               result = 0;
    else
        ThrowErrorException("Invalid non base64 character %c (%d)", code, code);
    return result;
}

CString decodeBase64(const char* coded)
{
    int j = 0;

    CString strResult;
    CString strEncoded = coded;

    // remove whitespace
    strEncoded.Remove(' ');
    strEncoded.Remove('\t');
    strEncoded.Remove('\n');
    strEncoded.Remove('\r');
    strEncoded.Remove('\f');
    strEncoded.Remove('\v');

    const char* p = static_cast<const char*>(strEncoded);
    int i = (int)strlen(p);

    // result will be 3/4 the size of the original
    char* d = strResult.GetBuffer(((i / 4) * 3) + 1);

    if (i % 4 != 0)
        ThrowErrorException("Not a valid base64 string - incorrect length (not mod 4).");

    // convert each 4 base64 bytes into 3 8-bit bytes
    for (; *p; p += 4)
    {
        d[j++] = (char)(getBase64Value(p[0]) << 2 | getBase64Value(p[1]) >> 4);
        d[j++] = (char)(getBase64Value(p[1]) << 4 | getBase64Value(p[2]) >> 2);
        d[j++] = (char)(getBase64Value(p[2]) << 6 | getBase64Value(p[3]));
    }

    d[j] = 0;

    // Trim the '=' padding of the final group.  Guard on i > 0: on empty (or
    // all-whitespace) input the loop never advanced p, so the original
    // unconditional *(--p) read one byte BEFORE the buffer.
    if (i > 0 && *(--p) == '=')
    {
        if (j > 0)
        {
            if (*(--p) == '=') d[j - 2] = '\0';
            else               d[j - 1] = '\0';
        }
    }

    strResult.ReleaseBuffer(-1);
    return strResult;
}

// ── SeeIfBase64 ──────────────────────────────────────────────────────────────
// If every non-whitespace byte is base64 and the count is a multiple of 4,
// decode in place (used when reading clipboard/XML that may be base64).  It only
// calls decodeBase64 on already-validated input, so the throw path above is
// unreachable from here.  On Windows this lived in xml/xml_serialize.cpp.
// Declared in stdafx.h.
void SeeIfBase64(CString& strText)
{
    const char* p = static_cast<const char*>(strText);
    char c;
    bool bBase64 = true;
    int iCount = 0;

    // locale-independent tests (see decodeBase64)
    for (iCount = 0; (c = *p) != 0; p++)
    {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v')
            continue;
        else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                 (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=')
            iCount++;
        else
        {
            bBase64 = false;
            break;
        }
    }

    // might be base64 if only base64 characters and mod-4 non-space length
    // (if base64 it can't be plain XML, because plain XML would contain '<')
    if (bBase64 && (iCount % 4 == 0))
        strText = decodeBase64(strText);
}

// ── EditDistance ────────────────────────────────────────────────────────────
// Levenshtein/Damerau edit distance between two strings (each capped at 20
// characters so a pathological word can't blow up the O(mn) matrix).
// Declared in stdafx.h / platform/doc_stub.h.
int EditDistance(const string source, const string target)
{
    const int MAX_LENGTH = 20;
    const int n = (int)source.length() < MAX_LENGTH ? (int)source.length() : MAX_LENGTH;
    const int m = (int)target.length() < MAX_LENGTH ? (int)target.length() : MAX_LENGTH;

    if (n == 0) return m;
    if (m == 0) return n;

    typedef vector< vector<int> > Tmatrix;
    Tmatrix matrix(n + 1);
    for (int i = 0; i <= n; i++) matrix[i].resize(m + 1);

    for (int i = 0; i <= n; i++) matrix[i][0] = i;
    for (int j = 0; j <= m; j++) matrix[0][j] = j;

    for (int i = 1; i <= n; i++) {
        const char s_i = source[i - 1];
        for (int j = 1; j <= m; j++) {
            const char t_j = target[j - 1];
            int cost = (s_i == t_j) ? 0 : 1;
            int above = matrix[i-1][j];
            int left  = matrix[i][j-1];
            int diag  = matrix[i-1][j-1];
            int cell  = above + 1 < left + 1 ? above + 1 : left + 1;
            if (diag + cost < cell) cell = diag + cost;
            if (i > 2 && j > 2) {
                int trans = matrix[i-2][j-2] + 1;
                if (source[i-2] != t_j) trans++;
                if (s_i != target[j-2]) trans++;
                if (cell > trans) cell = trans;
            }
            matrix[i][j] = cell;
        }
    }
    return matrix[n][m];
}
