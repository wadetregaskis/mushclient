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
  #include "MUSHclient.h"   // App / CColours (ColourToName's reverse lookup)
#else
  #include "platform/doc_stub.h"   // CString, std::string
#endif

#include <cctype>
#include <cstdio>
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

// ── Free-function twins single-sourced from doc_portable.cpp (Campaign B) ─────
// Hand-reimplemented in doc_portable.cpp (mushcore) as twins of the Windows
// originals in MUSHclient.cpp / Utilities.cpp / methods_utilities.cpp; verified
// byte-identical in behaviour (audit workflow), so single-sourced here.

// Generate256colours fills the xterm-256 palette.  The table is defined
// per-build (MUSHclient.cpp for Windows/mushapp, doc_portable.cpp for mushcore);
// this function only writes it.  Declared in MUSHclient.cpp / platform/doc_stub.h.
extern COLORREF xterm_256_colours[256];

void Generate256colours(void)
{
    const unsigned char values[6] = { 0, 95, 135, 175, 215, 255 };

    xterm_256_colours[ 0] = RGB(  0,   0,   0);
    xterm_256_colours[ 1] = RGB(128,   0,   0);
    xterm_256_colours[ 2] = RGB(  0, 128,   0);
    xterm_256_colours[ 3] = RGB(128, 128,   0);
    xterm_256_colours[ 4] = RGB(  0,   0, 128);
    xterm_256_colours[ 5] = RGB(128,   0, 128);
    xterm_256_colours[ 6] = RGB(  0, 128, 128);
    xterm_256_colours[ 7] = RGB(192, 192, 192);
    xterm_256_colours[ 8] = RGB(128, 128, 128);
    xterm_256_colours[ 9] = RGB(255,   0,   0);
    xterm_256_colours[10] = RGB(  0, 255,   0);
    xterm_256_colours[11] = RGB(255, 255,   0);
    xterm_256_colours[12] = RGB(  0,   0, 255);
    xterm_256_colours[13] = RGB(255,   0, 255);
    xterm_256_colours[14] = RGB(  0, 255, 255);
    xterm_256_colours[15] = RGB(255, 255, 255);

    for (int r = 0; r < 6; r++)
        for (int g = 0; g < 6; g++)
            for (int b = 0; b < 6; b++)
                xterm_256_colours[16 + r*36 + g*6 + b] =
                    RGB(values[r], values[g], values[b]);

    for (int grey = 0; grey < 24; grey++) {
        unsigned char v = (unsigned char)(8 + grey * 10);
        xterm_256_colours[232 + grey] = RGB(v, v, v);
    }
}

// convert an ANSI attribute number to its ESC sequence (e.g. 1 → "\033[1m").
// Declared in stdafx.h / platform/doc_stub.h.
CString AnsiCode(short iCode)
{
    char buf[16];
    snprintf(buf, sizeof buf, "\033[%dm", (int)iCode);
    return CString(buf);
}

// escape HTML special characters: < > & " → &lt; &gt; &amp; &quot;
// Declared in stdafx.h / platform/doc_stub.h.
CString FixHTMLString(CString strToFix)
{
    CString strOld = strToFix;
    CString strNew;
    int i;
    while ((i = strOld.FindOneOf("<>&\"")) != -1) {
        strNew += strOld.Left(i);
        switch (strOld[i]) {
        case '<':  strNew += "&lt;";   break;
        case '>':  strNew += "&gt;";   break;
        case '&':  strNew += "&amp;";  break;
        case '"':  strNew += "&quot;"; break;
        }
        strOld = strOld.Mid(i + 1);
    }
    strNew += strOld;
    return strNew;
}

// escape HTML special characters for multi-line element content: < > & tab →
// &lt; &gt; &amp; &#9; (newlines and quotes preserved).  Body verbatim from the
// Windows Utilities.cpp (relocated here when xml/xml_save_world.cpp was adopted
// into mushcore).  Declared in stdafx.h / platform/doc_stub.h.
CString FixHTMLMultilineString (const CString strToFix)
  {
  CString strOldString = strToFix;
  CString strNewString;

  int i;

  while ((i = strOldString.FindOneOf ("<>&\t")) != -1)
    {
    strNewString += strOldString.Left (i);

    switch (strOldString [i])
      {
      case '<': strNewString += "&lt;"; break;
      case '>': strNewString += "&gt;"; break;
      case '&': strNewString += "&amp;"; break;
      case '\t': strNewString += "&#9;"; break;
      }

    strOldString = strOldString.Mid (i + 1);
    }

  strNewString += strOldString;


  return strNewString;

  } // end of FixHTMLMultilineString

// wildcard → regular expression.  Behaviourally identical to the Windows
// Utilities.cpp form; the only difference is the control-char (<0x20) hex escape
// uses snprintf("%02x") instead of the Windows-only _itoa (same two-digit
// lowercase output).  Declared in stdafx.h / platform/doc_stub.h.
CString ConvertToRegularExpression(const CString& strMatchString,
                                   bool bWholeLine,
                                   bool bMakeAsterisksWildcards)
{
    CString strRegexp;
    int iSize = 0;

    // Count how many extra bytes the output needs vs. the input.
    for (const char* p = strMatchString; *p; p++)
    {
        unsigned char c = (unsigned char)*p;
        if (c < ' ')
            iSize += 3;          // \xhh
        else if (isalnum(c) || *p == ' ' || c >= 0x80)
            /* no change */;
        else if (*p == '*')
            iSize += 4;          // '*' → "(.*?)" (5 chars vs 1 → +4)
        else
            iSize++;             // escape prefix '\'
    }

    char* pOut = strRegexp.GetBuffer(strMatchString.GetLength() +
                                     iSize +
                                     2 +    // ^ and $
                                     10);   // null + padding

    if (bWholeLine)
        *pOut++ = '^';

    for (const char* p = strMatchString; *p; p++)
    {
        unsigned char c = (unsigned char)*p;
        if (*p == '\n')
            { *pOut++ = '\\'; *pOut++ = 'n'; }
        else if (c < ' ')
        {
            char buf[8];
            snprintf(buf, sizeof(buf), "%02x", (unsigned)c);
            *pOut++ = '\\'; *pOut++ = 'x';
            *pOut++ = buf[0]; *pOut++ = buf[1];
        }
        else if (isalnum(c) || *p == ' ' || c >= 0x80)
            *pOut++ = *p;
        else if (*p == '*' && bMakeAsterisksWildcards)
            { *pOut++ = '('; *pOut++ = '.'; *pOut++ = '*'; *pOut++ = '?'; *pOut++ = ')'; }
        else
            { *pOut++ = '\\'; *pOut++ = *p; }
    }

    if (bWholeLine)
        *pOut++ = '$';

    *pOut++ = '\0';
    strRegexp.ReleaseBuffer(-1);
    return strRegexp;
}

// ── Reconcile-first twins: adopted the Windows body (the port had drifted) ────
// Each differed between the port and Windows; single-sourced by taking the
// Windows body verbatim, so Windows is byte-for-byte unchanged and the port
// gains parity (drift-audit workflow).  (AdjustColour is deferred — its CColor /
// ADJUST_COLOUR_* dependencies aren't cleanly reachable from a standalone TU.)

// MXP-action lookup hash (SHS/SHA over action+hint+variable).  The value is an
// ephemeral in-memory bucket key, never persisted; the port had drifted to an
// FNV-1a hash.  Declared in stdafx.h / platform/doc_stub.h.
unsigned long MakeActionHash(const CString& strAction,
                             const CString& strHint,
                             const CString& strVariable)
{
    SHS_INFO shsInfo;
    CString strHash = strAction;
    strHash += strHint;
    strHash += strVariable;

    shsInit(&shsInfo);
    shsUpdate(&shsInfo, (unsigned char*)(const char*)strHash, strHash.GetLength());
    shsFinal(&shsInfo);
    return shsInfo.digest[0];
}

// convert C-style escape sequences (\n \t \xNN ...).  Builds into a CString
// buffer, and ReleaseBuffer(-1) re-measures with strlen, so an embedded NUL
// (from \x00, or a bare \x) TRUNCATES the string — matching Windows.  The port
// had preserved the embedded NUL.  Declared in stdafx.h / platform/doc_stub.h.
CString FixupEscapeSequences(const CString& strSource)
{
    CString strDest = strSource;
    char* p = strDest.GetBuffer(strSource.GetLength() + 1);
    char* pNew = p;
    char c;
    int i;

    for (; *p; p++)
    {
        c = *p;
        if (c == '\\')
        {
            c = *(++p);
            switch (c)
            {
            case 'a':  c = '\a'; break;
            case 'b':  c = '\b'; break;
            case 'f':  c = '\f'; break;
            case 'n':  c = '\n'; break;
            case 'r':  c = '\r'; break;
            case 't':  c = '\t'; break;
            case 'v':  c = '\v'; break;
            case '\'': c = '\''; break;
            case '\"': c = '\"'; break;
            case '\\': c = '\\'; break;
            case '?':  c = '\?'; break;
            case 'x':
                c = 0;
                p++;
                for (i = 0; *p && isxdigit(*p) && i < 2; p++, i++)
                {
                    int iNewDigit = toupper(*p);
                    if (iNewDigit >= 'A')
                        iNewDigit -= 7;
                    c = (c << 4) + iNewDigit - '0';
                }
                p--;  // backtrack over trailing non-hex character
                break;
            }
        }
        *pNew++ = c;
    }

    *pNew++ = 0;
    strDest.ReleaseBuffer(-1);
    return strDest;
}

// TRUE if `strLabel` is NOT a valid label: first char must be alpha; the rest
// alpha/digit/underscore, plus '.' when bScript is true.  The port had dropped
// the bScript parameter (and the '.' rule).  Declared (with the default arg) in
// stdafx.h, which the mushcore build also sees via doc_stub.h.
BOOL CheckLabel(const CString& strLabel, const bool bScript)
{
    if (strLabel.IsEmpty())
        return TRUE;

    // first character must be a letter
    if (!isalpha(strLabel[0]))
        return TRUE;

    for (int i = 1; i < strLabel.GetLength(); i++)
        if (!isalpha(strLabel[i]) &&
            !isdigit(strLabel[i]) &&
            strLabel[i] != '_' &&
            (strLabel[i] != '.' || !bScript))
            return TRUE;

    return FALSE;   // ok
}

// ── strnrchr ─────────────────────────────────────────────────────────────────
// Find the last occurrence of c in the string s, maximum length count.
// (Relocated verbatim from Utilities.cpp — needed by the display family in
// every build.)  Declared in stdafx.h / platform/doc_stub.h.
const char *strnrchr(const char *s, const char c, size_t count)
{
  const char * ret = NULL;
  size_t n = 0;
  while (*s && n < count++)
    {
    if( *s == c )
        ret = s;  // remember where character is, keep searching
    s++;
    }
  return ret;
} // end of strnrchr

// ── FixUpGerman ──────────────────────────────────────────────────────────────
// Replace German umlaut sequences if required — see bug request 240.
// (Relocated verbatim from Utilities.cpp — needed by DoSendMsg in every build.)
// Declared in stdafx.h / platform/doc_stub.h.
CString FixUpGerman (const CString strMessage)
  {
  CString strResult = strMessage;

  strResult.Replace ("\xFC", "ue");    // u with 2 dots
  strResult.Replace ("\xDC", "Ue");    // U with 2 dots
  strResult.Replace ("\xE4", "ae");    // a with 2 dots
  strResult.Replace ("\xC4", "Ae");    // A with 2 dots
  strResult.Replace ("\xF6", "oe");    // o with 2 dots
  strResult.Replace ("\xD6", "Oe");    // O with 2 dots
  strResult.Replace ("\xDF", "ss");    // B

  return strResult;

  } // end of FixUpGerman

// ── Make_Absolute_Path ───────────────────────────────────────────────────────
// Relocated verbatim from Utilities.cpp (needed by FormatTime in every build).
// working_dir is defined per-build: MUSHclient.cpp (Windows/mushapp) and
// doc_portable.cpp (mushcore, empty → relative paths stay relative headless).
extern char working_dir [_MAX_PATH];

const char * Make_Absolute_Path (CString strFileName)
  {

// if the path does *not* start with one of:
//    x:  (ie. a drive);
//    /something (ie. a top level directory) ; or
//    \something (ie. a top level directory)
//  then we prepend the MUSHclient working directory

  if (!(
      isalpha ((unsigned char) strFileName [0]) &&
      strFileName [1] == ':'               // eg. c: blah
      ) &&
      strFileName [0] != '\\' &&           // eg. \mydir\blah or \\server\blah
      strFileName [0] != '/'            // eg. /mydir/blah or //server/blah
     )
    {
    // if the filename starts with ./ or .\ then drop it, otherwise
    //   we get junk like:  c:\mushclient\.\logs\filename

    if (strFileName.Left (2) == "./" ||
        strFileName.Left (2) == ".\\")
      strFileName = strFileName.Mid (2);

    strFileName = working_dir +  strFileName;
    }

// this is declared static for a very good reason - some of the places this is used
// the *pointer* is copied into a structure (for CFileDialog) and thus it must persist
// long enough for the dialog to run.
static char sPathName [_MAX_PATH];

  // ensure not too long
  strFileName = strFileName.Left (sizeof (sPathName) - 1);

  // copy to retain
  strcpy (sPathName, (const char *) strFileName);

  return sPathName;

  } // end of Make_Absolute_Path

// ── RemoveFinalSlash ─────────────────────────────────────────────────────────
// Relocated verbatim from Utilities.cpp (the real loader xml/xml_load_world.cpp
// uses it in $PLUGINSDEFAULTDIR/$PROGRAMDIR/$WORLDDIR substitution, and
// mushcore compiles that loader now).

// if string ends in a path separator, get rid of it
CString RemoveFinalSlash (CString str)
  {

  if (!str.IsEmpty ())
    {
    char last = str [str.GetLength () - 1];
    if (last == '\\' || last == '/')
      str = str.Left (str.GetLength () - 1);
    }

  return str;

  } // end of RemoveFinalSlash


// ── tolower(string) / GetWord / StringToVector ─────────────────────────────
// Relocated verbatim from the Windows Utilities.cpp.  The old portable twin
// of StringToVector had DRIFTED: its 4th argument meant include_delim (append
// the delimiter to each token) instead of Windows' trim_spaces, and it
// returned [""] instead of [] for empty input.  Every shared call site
// (telnet_phases.cpp, scripting.cpp, methods_arrays.cpp) was written against
// the Windows semantics, so the port now matches them.

// returns a lower case version of the string
string tolower (const string & s)
  {
string d (s);

  transform (d.begin (), d.end (), d.begin (), (int(*)(int)) tolower);
  return d;
  }  // end of tolower

// split a line into the first word, and rest-of-the-line
string GetWord (string & s,
                const string delim,
                const bool trim_spaces)
  {

  // find delimiter
  string::size_type i (s.find (delim));

  // split into before and after delimiter
  string w (s.substr (0, i));

  // if no delimiter, remainder is empty
  if (i == string::npos)
    s.erase ();
  else
    // erase up to the delimiter
    s.erase (0, i + delim.size ());

  // trim spaces if required
  if (trim_spaces)
    {
    w = trim (w);
    s = trim (s);
    }

  // return first word in line
  return w;

  } // end of GetWord

// To be symmetric, we assume an empty string (after trimming spaces)
// will give an empty vector.
// However, a non-empty string (with no delimiter) will give one item
// After that, you get an item per delimiter, plus 1.
// eg.  ""      => empty
//      "a"     => 1 item
//      "a,b"   => 2 items
//      "a,b,"  => 3 items (last one empty)

void StringToVector (const string s,
                     vector<string> & v,
                     const string delim,
                     const bool trim_spaces)
  {

  // start with initial string, trimmed of leading/trailing spaces if required
  string s1 (trim_spaces ? trim (s) : s);

  v.clear (); // ensure vector empty

  // no string? no elements
  if (s1.empty ())
    return;

  // add to vector while we have a delimiter
  while (!s1.empty () && s1.find (delim) != string::npos)
    v.push_back (GetWord (s1, delim, trim_spaces));

  // add final element
  v.push_back (s1);
  } // end of StringToVector

// ── Command-ID name mapping ──────────────────────────────────────
// Relocated VERBATIM from Utilities.cpp (2026-07, methods_commands adoption):
// mushcore compiles the real methods_commands.cpp (DoCommand /
// GetInternalCommandsList) and the real methods_sounds.cpp (relative sound
// paths), which need these pure-logic lookups; Utilities.cpp is
// mushapp/Windows-only.  The CommandIDs table itself is accelerators.cpp
// (also compiled into mushcore now).

extern tCommandIDMapping CommandIDs [];

CString CommandIDToString (const int ID)
  {
CString strFunction;

  // scan functions in the hope we find it :)

  if (ID)
    for (int i = 0; CommandIDs [i].iCommandID; i++)
      {
      if (ID == CommandIDs [i].iCommandID)
        {
        strFunction = CommandIDs [i].sCommandName;
        break;
        }
      } // end of finding which one

  return strFunction;
  }  // end of CommandIDToString


int StringToCommandID (const CString str)
  {
int ID = 0;

  // scan functions in the hope we find it :)

  for (int i = 0; CommandIDs [i].iCommandID; i++)
    {
    if (str.CompareNoCase (CommandIDs [i].sCommandName) == 0)
      {
      ID = CommandIDs [i].iCommandID;
      break;
      }
    } // end of finding which one

  return ID;

  } // end of StringToCommandID

// get a pathname and extract the directory from it
CString ExtractDirectory (CString str)
  {
#ifdef _WIN32
  int iSlash = str.ReverseFind ('\\');
  if (iSlash == -1)
     str = ".\\";
  else
    str = str.Left (iSlash + 1);
#else
  // On POSIX paths use '/' but also accept '\' in case of cross-platform world files.
  int iSlashF = str.ReverseFind ('/');
  int iSlashB = str.ReverseFind ('\\');
  int iSlash  = (iSlashF > iSlashB) ? iSlashF : iSlashB;
  if (iSlash == -1)
     str = "./";
  else
    str = str.Left (iSlash + 1);
#endif
  return str;
  } // end of ExtractDirectory


// ── ColourToName ────────────────────────────────────────────────────────────
// Relocated verbatim from Utilities.cpp.  The old doc_portable.cpp twin only
// knew 16 colour names, so headless world saves wrote #RRGGBB where Windows
// writes names like "dodgerblue"; App.m_ColoursMap is populated headless
// (stub App ctor runs MXP_LoadColours), so the real reverse lookup works
// everywhere.

CString ColourToName (const COLORREF colour)
  {
int iRed    = GetRValue (colour);
int iGreen  = GetGValue (colour);
int iBlue   = GetBValue (colour);

CString strName;

// see if we can find colour name in list
  for (POSITION pos = App.m_ColoursMap.GetStartPosition(); pos; )
    {

    CColours * pColour;
    CString strColourName;

    App.m_ColoursMap.GetNextAssoc (pos, strColourName, pColour);

    // note - colour might match more than one name
    if (pColour->iColour == colour)
      return strColourName;
    } // end of loop

 return CFormat ("#%02X%02X%02X",
                           iRed,
                           iGreen,
                           iBlue);

  } // end of ColourToName
