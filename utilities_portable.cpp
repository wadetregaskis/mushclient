// utilities_portable.cpp
//
// Portable subset of Utilities.cpp — functions with no Windows/MFC dependencies.
// On Windows the full Utilities.cpp is compiled instead (via the .vcxproj).
// This file is compiled only on non-Windows via CMakeLists.txt.

#ifndef _WIN32

#include "platform/posix_socket.h"
#include <string>
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

// ── myAtoF ────────────────────────────────────────────────────────────────────
// Locale-independent string→double conversion.  On Windows this lives in
// Utilities.cpp (where it also underpins SQLite's text-to-float path).
// Declared in stdafx.h: int myAtoF(const char *z, double *pResult);

#include <cctype>

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

// ── Base-64 helpers ───────────────────────────────────────────────────────────
// decodeBase64 / getBase64Value / SeeIfBase64 live in Utilities.cpp and
// xml/xml_serialize.cpp on Windows.  Both are needed by xmlparse.cpp on every
// platform, so we provide them here.
// Declared in stdafx.h: CString decodeBase64(const char *coded);
//                        void SeeIfBase64(CString &strText);

#include "platform/doc_stub.h"   // CString

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

// ── encodeBase64 ──────────────────────────────────────────────────────────────
// Mirrors Utilities.cpp encodeBase64; bMultiLine wraps at column 76.

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

// ── EditDistance ──────────────────────────────────────────────────────────────
// Levenshtein/Damerau edit distance between two strings.
// Declared in stdafx.h: int EditDistance(const string source, const string target);
// On Windows this lives in Utilities.cpp; here on non-Windows.

#include <vector>
using std::vector;

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

#endif // !_WIN32
