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

  #include "stdafx.h"
  #include "MUSHclient.h"   // App / CColours (ColourToName's reverse lookup)
  #include "doc.h"          // StringToList (StringToKeyCode's tokenizer)

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifndef _WIN32
  #include <sys/utsname.h>   // OSVersionString asks the kernel, not os_version
#endif

using std::string;
using std::vector;

// ── IsStringNumber ──────────────────────────────────────────────────────────
// True if `s` is a run of decimal digits.  A leading '+'/'-' is accepted ONLY
// when bSigned is true (bSigned defaults to false — see the stdafx.h prototype).
// Declared in stdafx.h.
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
// consumed.  Declared in stdafx.h.
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
// 57 input bytes / 76 output chars).  Declared in stdafx.h.
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
// Declared in stdafx.h.

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
// Declared in stdafx.h.
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
// this function only writes it.  Declared in MUSHclient.cpp.
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
// Declared in stdafx.h.
CString AnsiCode(short iCode)
{
    char buf[16];
    snprintf(buf, sizeof buf, "\033[%dm", (int)iCode);
    return CString(buf);
}

// escape HTML special characters: < > & " → &lt; &gt; &amp; &quot;
// Declared in stdafx.h.
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
// into mushcore).  Declared in stdafx.h.
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
// lowercase output).  Declared in stdafx.h.
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
// FNV-1a hash.  Declared in stdafx.h.
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
// had preserved the embedded NUL.  Declared in stdafx.h.
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
// stdafx.h (every build includes it).
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
// every build.)  Declared in stdafx.h.
const char *strnrchr(const char *s, const char c, size_t count)
{
  const char * ret = NULL;
  size_t n = 0;
  // n++ < count, NOT n < count++: the latter increments the BOUND every
  // iteration (n stays 0), so 0 < count+iteration is always true and the
  // scan runs to the first NUL, ignoring `count` entirely.  Both callers
  // pass a CLine::text that is exactly `len` bytes with no NUL terminator
  // (Line.cpp sizes the buffer to the content), so a hostile long line with
  // no space read past the allocation.  Honour the length bound.
  while (n++ < count && *s)
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
// Declared in stdafx.h.
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


// ── DefaultWorldFileName ─────────────────────────────────────────────────────
// The filename to offer for a world that has never been saved: the WORLD NAME,
// scrubbed of the characters Windows forbids in a filename (the same set and
// the same strip loop as doc.cpp:2187), with ".mcl" ensured.
//
// Relocated here from the wx save hooks (platform/wx_mfc_impl.cpp) purely so a
// test can see it: it is a pure function of one string, but it used to sit next
// to a wxFileDialog::ShowModal, and no in-app selftest can complete a modal.
//
// The '/' in the scrub set is the one that matters.  It is a path separator,
// not just an illegal character, so an unscrubbed world name containing one
// would send the Save-As dialog to a directory the user never chose.
//
// (Windows itself reaches its copy of this via CDocument::OnSaveDocument, a
// path the port never takes — both save hooks compute the name themselves.
// Without this they offered the literal "world.mcl" for every world, so a user
// saving three worlds got three identical suggestions.)
// Declared in stdafx.h.
CString DefaultWorldFileName (const CString & strWorldName)
  {
  CString strClean = strWorldName;

  int i;
  while ((i = strClean.FindOneOf ("<>\"|?:#%;/\\")) != -1)
    strClean = strClean.Left (i) + strClean.Mid (i + 1);

  // A name that scrubs down to nothing but spaces is no more usable than an
  // empty one, so fold both onto the fallback.
  strClean.TrimLeft ();
  strClean.TrimRight ();

  if (strClean.IsEmpty ())
    return "world.mcl";          // unnamed world

  CString strLower = strClean;
  strLower.MakeLower ();
  if (strLower.Right (4) != ".mcl")
    strClean += ".mcl";

  return strClean;

  } // end of DefaultWorldFileName


// ── TipsFileName ─────────────────────────────────────────────────────────────
// Where CTipDlg looks for the Tip of the Day text, given the executable's own
// pathname: the directory the executable is in, plus the platform's suffix.
//
// Hoisted out of dialogs/TipDlg.cpp, which built it by strcpy-ing the module
// name into a `char [MAX_PATH]` and then strcat-ing the suffix on with no
// bound - so an install path whose directory part reached 238 characters (249
// on the Mac, where the suffix is longest) smashed the stack the moment
// Help > Tip of the Day, or a startup showing the tip, ran.  GetModuleFileName
// itself hands back up to 259 characters, so the input really can get there.
//
// A module name with no separator in it keeps the old behaviour: the suffix is
// appended to the whole thing.
// Declared in stdafx.h.
CString TipsFileName (const CString & strModulePath)
  {
  CString strDirectory = strModulePath;

// remove last part of file name to get working directory

#ifdef _WIN32
  int iSlash = strDirectory.ReverseFind ('\\');
#else
  int iSlash = strDirectory.ReverseFind ('/');
#endif

  if (iSlash != -1)
    strDirectory = strDirectory.Left (iSlash);

#ifdef _WIN32
  return strDirectory + "\\tips.txt";
#elif defined (__APPLE__)
  // In a .app bundle read-only resources live in Contents/Resources/, not
  // alongside the executable in Contents/MacOS/ (code signing requires that
  // only the executable sit there).
  return strDirectory + "/../Resources/tips.txt";
#else
  return strDirectory + "/tips.txt";
#endif

  } // end of TipsFileName


// ── tolower(string) / GetWord / StringToVector ─────────────────────────────
// Relocated verbatim from the Windows Utilities.cpp.  The old portable twin
// of StringToVector had DRIFTED: its 4th argument meant include_delim (append
// the delimiter to each token) instead of Windows' trim_spaces, and it
// returned [""] instead of [] for empty input.  Every shared call site
// (telnet_phases.cpp, scripting.cpp, methods_arrays.cpp) was written against
// the Windows semantics, so the port now matches them.

// Walk one UTF-8 character -- see the comment on the declaration in stdafx.h.
// The caller supplies n = bytes remaining, so a truncated sequence at the end of
// a buffer is reported rather than read past.
size_t UTF8NextChar (const unsigned char * p, size_t n, unsigned int & cp)
  {
  if (n == 0)          // defensive: callers loop while i < size
    {
    cp = 0xFFFD;
    return 1;
    }

  const unsigned char c = p [0];

  int extra;
  unsigned int v;

  if (c < 0x80)                       // ASCII, the overwhelming majority
    {
    cp = c;
    return 1;
    }
  else if (c >= 0xC2 && c <= 0xDF)  { extra = 1; v = c & 0x1Fu; }
  else if (c >= 0xE0 && c <= 0xEF)  { extra = 2; v = c & 0x0Fu; }
  else if (c >= 0xF0 && c <= 0xF4)  { extra = 3; v = c & 0x07u; }
  else
    {
    // A stray continuation byte (0x80-0xBF), an always-overlong lead (0xC0 /
    // 0xC1), or a lead beyond the Unicode range (0xF5-0xFF).
    cp = 0xFFFD;
    return 1;
    }

  for (int k = 1; k <= extra; k++)
    {
    if ((size_t) k >= n || (p [k] & 0xC0u) != 0x80u)
      {
      cp = 0xFFFD;      // truncated: consume only what actually belongs to it,
      return (size_t) k;  // so the byte that ended it is judged on its own
      }
    v = (v << 6) | (p [k] & 0x3Fu);
    }

  // Right shape, wrong value: an overlong encoding, a surrogate half, or beyond
  // U+10FFFF.  MultiByteToWideChar rejects all three as well.
  if ((extra == 2 && v < 0x800) || (extra == 3 && v < 0x10000) ||
      (v >= 0xD800 && v <= 0xDFFF) || v > 0x10FFFF)
    cp = 0xFFFD;
  else
    cp = v;

  return (size_t) extra + 1;
  } // end of UTF8NextChar

// Byte offset -> character offset.  See the declarations in stdafx.h.
size_t UTF8ByteToCharOffset (const char * p, size_t n, size_t byteOffset)
  {
  if (byteOffset > n)
    byteOffset = n;

  const unsigned char * u = (const unsigned char *) p;
  size_t i = 0, chars = 0;

  while (i < byteOffset)
    {
    unsigned int cp;
    i += UTF8NextChar (u + i, n - i, cp);
    chars++;
    }

  return chars;
  } // end of UTF8ByteToCharOffset

// Character offset -> byte offset.
size_t UTF8CharToByteOffset (const char * p, size_t n, size_t charOffset)
  {
  const unsigned char * u = (const unsigned char *) p;
  size_t i = 0, chars = 0;

  while (i < n && chars < charOffset)
    {
    unsigned int cp;
    i += UTF8NextChar (u + i, n - i, cp);
    chars++;
    }

  return i;
  } // end of UTF8CharToByteOffset

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
  // Accept BOTH separators, on both platforms, and let the later one win.
  //
  // The POSIX arm always did, "in case of cross-platform world files".  The
  // Windows arm looked only for a backslash, and that is an upstream defect
  // rather than a platform difference -- its own caller proves it.
  // xml_load_world.cpp decides whether a plugin filename is relative by asking
  // whether it contains a backslash OR a forward slash, so a plugin path with
  // forward slashes is a shape the loader expressly expects; six lines later it
  // hands that same string to this function.  With a backslash-only search the
  // answer was ".\", so the plugin's m_strDirectory -- and with it every
  // $PLUGINDIR the plugin uses -- resolved to the current directory instead of
  // the plugin's own.  Win32 has taken '/' as a path separator since NT, so
  // widening the search cannot mis-split a path a backslash-only search got
  // right: every such path has no '/' in it at all.  docs/UPSTREAM-BUGS.md 89.
  int iSlashF = str.ReverseFind ('/');
  int iSlashB = str.ReverseFind ('\\');
  int iSlash  = (iSlashF > iSlashB) ? iSlashF : iSlashB;
  if (iSlash == -1)
#ifdef _WIN32
     str = ".\\";    // the no-separator fallback still names the platform's own
#else
     str = "./";
#endif
  else
    str = str.Left (iSlash + 1);
  return str;
  } // end of ExtractDirectory


// ── ColourToName ────────────────────────────────────────────────────────────
// Relocated verbatim from Utilities.cpp.  The old doc_portable.cpp twin only
// knew 16 colour names, so headless world saves wrote #RRGGBB where Windows
// writes names like "dodgerblue"; App.m_ColoursMap is populated headless
// (stub App ctor runs MXP_LoadColours), so the real reverse lookup works
// everywhere.
//
// WHICH name, when a colour has several.  Nine RGB values in the HTML table
// (mxp/mxpinit.cpp) carry two names apiece: aqua/cyan, fuchsia/magenta, and
// seven -gray/-grey pairs.  The original returned the first match found while
// walking App.m_ColoursMap -- an MFC CMapStringToPtr, so the answer was
// whichever the hash buckets happened to yield first, with the comment "note -
// colour might match more than one name" acknowledging the ambiguity but not
// resolving it.
//
// That is not an internal detail: ColourToName's output is the TEXT WRITTEN
// INTO EVERY SAVED WORLD FILE.  Measured 2026-08-09, a stock 5.06 and this
// tree disagreed on four of the nine, so re-saving an untouched world rewrote
// `magenta` as `fuchsia` and `cyan` as `aqua`; and this tree was not even
// self-consistent, answering `dimgrey` but `darkgray`.  The order depends on
// the compiler's hash, the bucket count and the table's contents, so it can
// change again for no reason anyone would connect to it.
//
// So the choice is pinned.  kCanonicalColourNames is the name every SHIPPED
// MUSHclient emitted: for the three pairs that both had (aqua/cyan,
// fuchsia/magenta, lightgray/lightgrey) it is what 5.06 wrote, and for the six
// -grey spellings added after 5.06 shipped it is the -gray spelling, which is
// the only one that existed when those colours were last written to a file.
// Anything not listed falls back to the alphabetically first match, so the
// result is deterministic even for a colour that gains an alias tomorrow.

static const char * const kCanonicalColourNames [] =
  {
  // the two 5.06 wrote that a hash-order change had flipped
  "cyan", "magenta",
  // 5.06's answer for the one -gray/-grey pair it already had
  "lightgrey",
  // the six pairs whose -grey spelling arrived after 5.06: -gray is the
  // spelling any existing world file will be carrying
  "gray", "darkgray", "dimgray", "slategray", "darkslategray", "lightslategray",
  };

static bool IsCanonicalColourName (const CString & strName)
  {
  for (int i = 0; i < sizeof kCanonicalColourNames / sizeof kCanonicalColourNames [0]; i++)
    if (strName.CompareNoCase (kCanonicalColourNames [i]) == 0)
      return true;
  return false;
  }   // end of IsCanonicalColourName

CString ColourToName (const COLORREF colour)
  {
int iRed    = GetRValue (colour);
int iGreen  = GetGValue (colour);
int iBlue   = GetBValue (colour);

CString strBest;              // empty until we match something
bool    bBestIsCanonical = false;

// see if we can find colour name in list
  for (POSITION pos = App.m_ColoursMap.GetStartPosition(); pos; )
    {

    CColours * pColour;
    CString strColourName;

    App.m_ColoursMap.GetNextAssoc (pos, strColourName, pColour);

    // note - colour might match more than one name, so keep looking and take
    // the canonical one, or failing that the alphabetically first
    if (pColour->iColour != colour)
      continue;

    const bool bCanonical = IsCanonicalColourName (strColourName);

    if (strBest.IsEmpty () ||                                  // first match
        (bCanonical && !bBestIsCanonical) ||                   // canonical wins
        (bCanonical == bBestIsCanonical &&                     // else alphabetical
         strColourName.CompareNoCase (strBest) < 0))
      {
      strBest = strColourName;
      bBestIsCanonical = bCanonical;
      }
    } // end of loop

  if (!strBest.IsEmpty ())
    return strBest;

 return CFormat ("#%02X%02X%02X",
                           iRed,
                           iGreen,
                           iBlue);

  } // end of ColourToName

#ifndef _WIN32
// ── WidenUTF16LE (port-only) ────────────────────────────────────────────────
// Widen a raw UTF-16LE byte stream (BOM already stripped) into wchar_t code
// points, combining surrogate pairs.  Off-Windows wchar_t is 4 bytes, so the
// Windows idiom of casting the byte stream to LPCWSTR mis-reads it (two UTF-16
// units per wchar_t).  The UTF-16 file readers — CXMLparser::BuildStructure
// (xml/xmlparse.cpp) and IsArchiveXML (xml/xml_serialize.cpp) — widen through
// here first, then share the normal WideCharToMultiByte UTF-8 encoding.
// An odd trailing byte is ignored; an unpaired surrogate passes through as its
// own code point (matching the permissive Windows conversion).
void WidenUTF16LE (const unsigned char * pBytes, int nBytes,
                   std::vector<wchar_t> & vWide)
  {
  int nUnits = nBytes / 2;
  vWide.clear ();
  vWide.reserve (nUnits);
  for (int iUnit = 0; iUnit < nUnits; iUnit++)
    {
    unsigned long cp = pBytes [iUnit * 2] | (pBytes [iUnit * 2 + 1] << 8);
    if (cp >= 0xD800 && cp <= 0xDBFF && iUnit + 1 < nUnits)
      {
      unsigned long low = pBytes [(iUnit + 1) * 2] | (pBytes [(iUnit + 1) * 2 + 1] << 8);
      if (low >= 0xDC00 && low <= 0xDFFF)
        {
        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
        iUnit++;
        }
      }
    vWide.push_back ((wchar_t) cp);
    }
  } // end of WidenUTF16LE
#endif // !_WIN32

// - IsPluginID / IsSubName ---------------------------------------------------
// Moved here verbatim from Utilities.cpp (GUI-only): the shared hyperlink
// handler validates the !!pluginID:sub(arg) plugin-call form with them, so the
// headless engine needs them too.  Declared in stdafx.h.
// quick check to see if a string is a plugin ID
bool IsPluginID (const char * sID)
  {
  // return if id wrong length
  if (strlen (sID) != PLUGIN_UNIQUE_ID_LENGTH)
    return false;

  for (const char * p = sID; *p; p++)
    if (!isxdigit (*p))
      return false;

  return true;
  }  // end of IsPluginID

// quick check to see if a string is a subroutine name
bool IsSubName (const char * sName)
  {
const char * p = sName;

  int i;
// check for A-Z, 0-9 or underscore
  for (i = 0; *p && *p != '('; p++, i++)
    if (!isalnum (*p) && *p != '_' && *p != '.')
      return false;

// can't be zero length
  if (i == 0)
    return false;

  return true;
  }  // end of IsSubName

// - ScanMatchingBrace --------------------------------------------------------
// The Edit > Go To / Select To Matching Brace scan, moved verbatim out of
// Utilities.cpp::FindMatchingBrace (which is GUI-only) so every frontend runs
// one algorithm.  It was the only reader of App.m_nParenMatchFlags, and it sat
// behind CSendView/CTextView -- views the wx port never constructs -- so the
// port had its own scan that honoured none of the seven checkboxes on the
// global Notepad preferences page.
//
// Pure: takes the buffer and the caret (nEndChar in, both out), returns false
// where the original beeped.  The caller keeps the control-specific parts --
// reading the caret, applying the selection, and the beep.

#define DELIMS "{[()]}"
#define START_DELIMS "{[("
#define END_DELIMS "}])"
#define MAX_NEST 1000

static char GetMatchingDelimiter (const char cDelim)
  {
  switch (cDelim)
    {
    case '[' : return ']';
    case '(' : return ')';
    case '{' : return '}';
    case ']' : return '[';
    case ')' : return '(';
    case '}' : return '{';
    }   // end of switch
  return 0;   // shouldn't get here :)
  } // end of GetMatchingDelimiter

bool ScanMatchingBrace (const char * pBuf,
                        const int nLength,
                        int & nStartChar,
                        int & nEndChar,
                        const bool bSelect,
                        const unsigned int iFlags)
{
char cStartDelim;    // start delimiter, eg. [
char c;
const char * p;
const char * pStart;
const char * pStartBuf;
int iLevel = -1;
bool bBackwards = false;
char cNestArray [MAX_NEST];   // so we know when nested group ends

  pStartBuf = pBuf;   // to stop us going too far backwards

  // return if out of buffer
  if (nLength == 0 || nEndChar < 0 || nEndChar > nLength)
    {
    return false;
    }

  // if at end of buffer, go back one character
  if (nEndChar == nLength)
    nEndChar--;
  else
    // if no delimiter on right, try going back one
    if (strchr (DELIMS, pBuf [nEndChar]) == NULL &&
        nEndChar > 0)
      nEndChar--;

  // character on right of cursor
  cStartDelim = pBuf [nEndChar];

   int iEscapeCount = 0;
   p = pBuf;
   p += nEndChar;    // p points to start of search


   // if an odd number of escape characters, cannot use it
   while ((iFlags & PAREN_BACKSLASH_ESCAPES) &&
       p > pStartBuf &&
       *(p - 1) == '\\')
     {
     p--;
     iEscapeCount++;
     }

   if (iEscapeCount == 0)
     while ((iFlags & PAREN_PERCENT_ESCAPES) &&
         p > pStartBuf &&
         *(p - 1) == '%')
       {
       p--;
       iEscapeCount++;
       }

  // not on a delimiter, or delimiter preceded by an odd number of escapes? just exit
  if ((iEscapeCount & 1) || strchr (DELIMS, cStartDelim) == NULL)
    {
    return false;
    }

  nStartChar = nEndChar;    // selection will start here
  p = pBuf;
  p += nEndChar;    // p points to start of search
  pStart = p;       // where we started

  if (strchr (START_DELIMS, cStartDelim))
    {   // search forwards - work out end delimiter
     cNestArray [++iLevel] = GetMatchingDelimiter (cStartDelim);
     for (p ++ ; (c = *p) != 0; p++)
       {
       if (c == cNestArray [iLevel])    // reached end delimiter?
         {
         if (iLevel <= 0)
           break;   // found it!
         iLevel--;  // dropped back a level
         }
       else if (((iFlags & PAREN_NEST_BRACES) && (strchr (START_DELIMS, c))) ||
                (!(iFlags & PAREN_NEST_BRACES) && (c == cStartDelim)))
         {
         iLevel++;   // up a level, need another closing delimiter
         if (iLevel >= MAX_NEST)
          {
          return false;
          }    // too many levels
         cNestArray [iLevel] = GetMatchingDelimiter (c);
         }
       else if (c == '\\' && (iFlags & PAREN_BACKSLASH_ESCAPES))
         {
         if (*(++p) == 0) // skip next character, if end of buffer, stop
           break;
         }    // end of backslash escape
       else if (c == '%' && (iFlags & PAREN_PERCENT_ESCAPES))
         {
         if (*(++p) == 0) // skip next character, if end of buffer, stop
           break;
         }    // end of percent escape
       else if ((c == '\"' && (iFlags & PAREN_DOUBLE_QUOTES)) ||
                (c == '\'' && (iFlags & PAREN_SINGLE_QUOTES)))
         {  // in quotes, skip quoted part
         for (p++; *p && *p != c; p++)
           {  // quote skipper
            if (*p == '\\' &&
               (iFlags & PAREN_BACKSLASH_ESCAPES) &&
             (((iFlags & PAREN_ESCAPE_SINGLE_QUOTES) && c == '\'') ||
              ((iFlags & PAREN_ESCAPE_DOUBLE_QUOTES) && c == '\"'))
               )   // can have escaped quotes inside quotes
             {
             if (*(++p) == 0) // skip next character, if end of buffer, stop
               break;
             }  // end of backslash escape
            else if (*p == '%' &&
               (iFlags & PAREN_PERCENT_ESCAPES) &&
             (((iFlags & PAREN_ESCAPE_SINGLE_QUOTES) && c == '\'') ||
              ((iFlags & PAREN_ESCAPE_DOUBLE_QUOTES) && c == '\"'))
               )   // can have escaped quotes inside quotes
             {
             if (*(++p) == 0) // skip next character, if end of buffer, stop
               break;
             }  // end of percent escape
           }    // end of skipping quotes
         }  // end of having a quote
       }  // end of searching for delimiter

     if (*p == 0)
        {
        return false;
        }    // no end delimiter found


     // work out where end character is
     nEndChar += p - pStart;

    }  // end of searching forwards

  else
    {  // go backwards
    bBackwards = true;
    cNestArray [++iLevel] = GetMatchingDelimiter (cStartDelim);
    for (p--; p >= pStartBuf; p--)
       {
       int iEscapeCount = 0;
       c = *p;

       // if an odd number of escape characters, ignore it
       while ((iFlags & PAREN_BACKSLASH_ESCAPES) &&
           p > pStartBuf &&
           *(p - 1) == '\\')
         {
         p--;
         iEscapeCount++;
         }

       if (iEscapeCount == 0)
         while ((iFlags & PAREN_PERCENT_ESCAPES) &&
             p > pStartBuf &&
             *(p - 1) == '%')
           {
           p--;
           iEscapeCount++;
           }

       if (iEscapeCount & 1)
         continue;

       if (c == cNestArray [iLevel])
         {
         if (iLevel <= 0)
           {
           p += iEscapeCount;
           break;   // found it!
           }
         iLevel--;  // dropped back a level
         }
       else
// ----- Begin Talek suggestion
        if ((iFlags & PAREN_NEST_BRACES) && (strchr (START_DELIMS, c)))
          {
          // we must have had an unmatched closing paren - remove it from the stack
         if (iLevel <= 0)  // no more levels - error in string
            {
            return false;
            }    // too many levels
         iLevel--;  // dropped back a level
         p++;   // reconsider this character next time through the loop
         }
        else

// ----- End Talek suggestion


         if (((iFlags & PAREN_NEST_BRACES) && (strchr (END_DELIMS, c))) ||
                (!(iFlags & PAREN_NEST_BRACES) && (c == cStartDelim)))
         {
         iLevel++;   // up a level, need another closing delimiter
         if (iLevel >= MAX_NEST)
          {
          return false;
          }    // too many levels
         cNestArray [iLevel] = GetMatchingDelimiter (c);
         }
       else if ((c == '\"' && (iFlags & PAREN_DOUBLE_QUOTES)) ||
                (c == '\'' && (iFlags & PAREN_SINGLE_QUOTES)))
         {  // in quotes, skip quoted part
         for (p--; p >= pStartBuf; p--)
           {  // quote skipper
           // if escaped character, ignore it
            if ((p > pStartBuf && *(p - 1) == '\\') &&
               (iFlags & PAREN_BACKSLASH_ESCAPES) &&
             (((iFlags & PAREN_ESCAPE_SINGLE_QUOTES) && c == '\'') ||
              ((iFlags & PAREN_ESCAPE_DOUBLE_QUOTES) && c == '\"'))
               )   // can have escaped quotes inside quotes
              p--;
            else if ((p > pStartBuf && *(p - 1) == '%') &&
               (iFlags & PAREN_PERCENT_ESCAPES) &&
             (((iFlags & PAREN_ESCAPE_SINGLE_QUOTES) && c == '\'') ||
              ((iFlags & PAREN_ESCAPE_DOUBLE_QUOTES) && c == '\"'))
               )   // can have escaped quotes inside quotes
              p--;
           else
             if (*p == c)
               break;
           }    // end of skipping quotes
         }  // end of having a quote
       }  // end of searching for delimiter

     if (p < pStartBuf)
        {
        return false;
        }    // no end delimiter found

     // work out where end character is
     nStartChar -= pStart - p;

    }   // end of searching backwards

  if (bSelect)
    nEndChar++;   // select end character as well
  else
    if (bBackwards)
      nEndChar = nStartChar;    // make insertion point only
    else
      nStartChar = nEndChar;    // make insertion point only

  return true;

  //  (aaa[bbb]ccc)[ddd{e']'ee}]
  //  (aaaa "bb \" bb" aaaa)
}  // end of ScanMatchingBrace

extern tVirtualKeyMapping VirtualKeys [];   // accelerators.cpp

// ── KeyCodeToString ─────────────────────────────────────────────────────────
// Render an accelerator (HOTKEYF_*/FVIRTKEY modifier byte + virtual-key code)
// as "Ctrl+Alt+F5".  Pure lookup against the VirtualKeys[] table in
// accelerators.cpp; moved here verbatim from Utilities.cpp so the headless
// engine can link CMUSHclientDoc::AcceleratorList, which is the only caller
// outside the GUI.  Declared in stdafx.h.
//
CString KeyCodeToString (const BYTE fVirt, const WORD key)
  {

  CString strKeyCode,
          strKey;

   if (fVirt & FSHIFT)
     strKey += "Shift+";
   if (fVirt & FCONTROL)
     strKey += "Ctrl+";
   if (fVirt & FALT)
     strKey += "Alt+";

   // default is key code itself, particularly if FVIRTKEY not specified
   strKeyCode = (char) key;

//  if (fVirt & FVIRTKEY)
    for (int i = 0; VirtualKeys [i].iVirtualKey; i++)
      {
      if (key == VirtualKeys [i].iVirtualKey)
        {
        strKeyCode = VirtualKeys [i].sKeyName;
        break;
        }
      } // end of finding key name

  strKey += strKeyCode;

  return strKey;

  } // end of KeyCodeToString

// ── StringToKeyCode ─────────────────────────────────────────────────────────
// The inverse: parse "Ctrl+Alt+F5" into the modifier byte + virtual-key code,
// throwing (ThrowErrorException) on anything it cannot parse.  Pure string
// work; moved here from Utilities.cpp for the same reason as KeyCodeToString —
// AcceleratorTo needs it, and the headless build used to get a stub that
// silently returned 0/0, which would have made any test of the accelerator API
// pass for the wrong reason.  Declared in stdafx.h.
//
void StringToKeyCode (const CString strKey,
                      BYTE & fVirt,
                      WORD & key)
  {
  // break key up into individual parts (eg. Ctrl + Shift + blah )
  CStringList strList;
  StringToList (strKey, "+", strList);

  fVirt = FVIRTKEY;
  key = 0;

  for (POSITION pos = strList.GetHeadPosition (); pos; )
    {
    CString strPart = strList.GetNext (pos);
    strPart.TrimLeft ();
    strPart.TrimRight ();

    // look for special cases: Alt, Ctrl, Shift
    if (strPart.CompareNoCase ("Alt") == 0)
      {
      if (fVirt & FALT)
        ThrowErrorException ("You cannot have Alt+Alt");
      fVirt |= FALT;
      }
    else if (strPart.CompareNoCase ("Ctrl") == 0)
      {
      if (fVirt & FCONTROL)
        ThrowErrorException ("You cannot have Ctrl+Ctrl");
      fVirt |= FCONTROL;
      }
    else if (strPart.CompareNoCase ("Shift") == 0)
      {
      if (fVirt & FSHIFT)
        ThrowErrorException ("You cannot have Shift+Shift");
      fVirt |= FSHIFT;
      }
    else
      {
      int j;
      for (j = 0; VirtualKeys [j].iVirtualKey; j++)
        {
        if (strPart.CompareNoCase (VirtualKeys [j].sKeyName) == 0)
          {
          if (key != 0)
            ThrowErrorException ("You cannot specify two keystrokes");

          key = VirtualKeys [j].iVirtualKey;
          break;
          }
        } // end of finding key name

      if (VirtualKeys [j].iVirtualKey == 0)
        ThrowErrorException ("Keystoke %s unknown", (LPCTSTR) strPart);

      } // must be key itself
    } // end of working through part of key

  } // end of StringToKeyCode

// ── QuoteForumCodes ─────────────────────────────────────────────────────────
// For forum posts, where code is being quoted: put a backslash in front of
// every '[', ']' and '\'.  Declared in stdafx.h.
//
// Moved here verbatim from Utilities.cpp (mushgui_adapters-only), where the
// headless engine could not reach it and platform/doc_flip_gui_stubs.cpp
// supplied `return strText;` instead — an identity stub justified by "headless
// never runs the menu handler".  That justification was false: eNotepadConvert-
// QuoteForum routes straight into this function from notepad_convert_shared.cpp:244,
// a CORE_SOURCES TU, so the headless engine WAS running it.
//
// The stub was nevertheless not an observable behaviour bug, and the distinction
// matters for anyone reading this later.  The body ends in a confirmation box,
// and headless UMessageBox falls through to HeadlessMessageBoxAnswer, which
// answers IDCANCEL for MB_OKCANCEL (platform/wx_mfc_hooks.h:87) — so the real
// body ALSO returns strText unchanged when nothing installs a message-box hook.
// Stub and real body agreed; they simply agreed for unrelated reasons.  What the
// hoist buys is that they can no longer stop agreeing, and that a test which
// installs g_wxhook_message_box can now exercise the conversion for real.
CString QuoteForumCodes(const CString & strText)
{
CString strNewText;
const char * pOld;
char * pNew;
int iChanges = 0;

  // count how many characters we see that need converting
  for (pOld = strText; *pOld; pOld++)
    if (*pOld == '[' ||
        *pOld == ']' ||
        *pOld == '\\')
      iChanges++;

  // allocate memory for new buffer
  pNew = strNewText.GetBuffer (strText.GetLength () + iChanges + 1);

  for (pOld = strText; *pOld; pOld++)
    {
    if (*pOld == '[' ||
        *pOld == ']' ||
        *pOld == '\\')
      *pNew++ = '\\';

    // now copy over whatever it was
    *pNew++ = *pOld;
    }

  *pNew++ = 0;    // terminator at end
  strNewText.ReleaseBuffer ();

  if (::UMessageBox (TFormat ("Clipboard converted for use with the Forum, %i change%s made",
      PLURAL (iChanges)), MB_ICONINFORMATION | MB_OKCANCEL) != IDOK)
      return strText;

  return strNewText;

  }  // end of QuoteForumCodes

// ── GetHostNameAndAddresses ─────────────────────────────────────────────────
// Our own host name and the dotted-quad addresses it resolves to, used by the
// chat CHAT:/ZCHAT: handshake to advertise where a peer should call us back
// (chatsock.cpp MakeCall) and by the About box (mainfrm.cpp).  Declared in
// stdafx.h.
//
// Moved verbatim from Utilities.cpp, which is mushgui_adapters-only, so that the
// adopted chat family links headless.  Pure POSIX — gethostname, gethostbyname,
// inet_ntoa — with no MFC or wx in it, which is why it can move at all.
void GetHostNameAndAddresses (CString & strHostName, CString & strAddresses)
  {
 char szHostName[256];

 if( gethostname(szHostName, sizeof szHostName) == 0 )
   {
   // Get host adresses
   struct hostent * pHost;
   int i;

   pHost = gethostbyname(szHostName);
   strHostName = szHostName;

   for( i = 0; pHost != NULL && pHost->h_addr_list [i] != NULL; i++ )
     {
     /*
     CString str;
     int j;

     for( j = 0; j < pHost->h_length; j++ )
       {
       CString addr;

       if( j > 0 )
         str += ".";

       addr.Format("%u", (unsigned int)((unsigned
         char*)pHost->h_addr_list[i])[j]);
       str += addr;

       }
*/
     if (!strAddresses.IsEmpty ())
       strAddresses += ", ";
     strAddresses += inet_ntoa ( *((in_addr *)pHost->h_addr_list[i]));


     }
   }
  } // end of GetHostNameAndAddresses

// ── MacroCommandIdToSlot ─────────────────────────────────────────────────────
// Which m_macros[] slot a macro COMMAND ID names, or -1 for an id that is not a
// macro.  Hoisted verbatim out of CSendView::OnMacroCommand (sendvw.cpp), which
// now calls it, so the mapping has one definition instead of one per frontend:
// the port needs it too, and a hand-copied 50-case switch is exactly the kind of
// twin that drifts.
//
// The gaps are upstream's and are preserved: there is no ID_MACRO_CTRL_F4 case
// (commented out in the original), and ID_ALT_V is absent because no MAC_ALT_V
// slot exists -- see docs/UPSTREAM-BUGS.md.
int MacroCommandIdToSlot (const unsigned int nIDC)
  {
  switch (nIDC)
    {
    case ID_MACRO_F1                : return MAC_F1;
    case ID_MACRO_F2                : return MAC_F2;
    case ID_MACRO_F3                : return MAC_F3;
    case ID_MACRO_F4                : return MAC_F4;
    case ID_MACRO_F5                : return MAC_F5;
    case ID_MACRO_F6                : return MAC_F6;
    case ID_MACRO_F7                : return MAC_F7;
    case ID_MACRO_F8                : return MAC_F8;
    case ID_MACRO_F9                : return MAC_F9;
    case ID_MACRO_F10               : return MAC_F10;
    case ID_MACRO_F11               : return MAC_F11;
    case ID_MACRO_F12               : return MAC_F12;

    case ID_MACRO_CTRL_F1           : return MAC_CTRL_F1;
    case ID_MACRO_CTRL_F2           : return MAC_CTRL_F2;
    case ID_MACRO_CTRL_F3           : return MAC_CTRL_F3;
//  case ID_MACRO_CTRL_F4           : return MAC_CTRL_F4;   // upstream: absent
    case ID_MACRO_CTRL_F5           : return MAC_CTRL_F5;
    case ID_MACRO_CTRL_F6           : return MAC_CTRL_F6;
    case ID_MACRO_CTRL_F7           : return MAC_CTRL_F7;
    case ID_MACRO_CTRL_F8           : return MAC_CTRL_F8;
    case ID_MACRO_CTRL_F9           : return MAC_CTRL_F9;
    case ID_MACRO_CTRL_F10          : return MAC_CTRL_F10;
    case ID_MACRO_CTRL_F11          : return MAC_CTRL_F11;
    case ID_MACRO_CTRL_F12          : return MAC_CTRL_F12;

    case ID_MACRO_SHIFT_F1          : return MAC_SHIFT_F1;
    case ID_MACRO_SHIFT_F2          : return MAC_SHIFT_F2;
    case ID_MACRO_SHIFT_F3          : return MAC_SHIFT_F3;
    case ID_MACRO_SHIFT_F4          : return MAC_SHIFT_F4;
    case ID_MACRO_SHIFT_F5          : return MAC_SHIFT_F5;
    case ID_MACRO_SHIFT_F6          : return MAC_SHIFT_F6;
    case ID_MACRO_SHIFT_F7          : return MAC_SHIFT_F7;
    case ID_MACRO_SHIFT_F8          : return MAC_SHIFT_F8;
    case ID_MACRO_SHIFT_F9          : return MAC_SHIFT_F9;
    case ID_MACRO_SHIFT_F10         : return MAC_SHIFT_F10;
    case ID_MACRO_SHIFT_F11         : return MAC_SHIFT_F11;
    case ID_MACRO_SHIFT_F12         : return MAC_SHIFT_F12;

    case ID_ALT_A                   : return MAC_ALT_A;
    case ID_ALT_B                   : return MAC_ALT_B;
    case ID_ALT_J                   : return MAC_ALT_J;
    case ID_ALT_K                   : return MAC_ALT_K;
    case ID_ALT_L                   : return MAC_ALT_L;
    case ID_ALT_M                   : return MAC_ALT_M;
    case ID_ALT_N                   : return MAC_ALT_N;
    case ID_ALT_O                   : return MAC_ALT_O;
    case ID_ALT_P                   : return MAC_ALT_P;
    case ID_ALT_Q                   : return MAC_ALT_Q;
    case ID_ALT_R                   : return MAC_ALT_R;
    case ID_ALT_S                   : return MAC_ALT_S;
    case ID_ALT_T                   : return MAC_ALT_T;
    case ID_ALT_U                   : return MAC_ALT_U;
    case ID_ALT_X                   : return MAC_ALT_X;
    case ID_ALT_Y                   : return MAC_ALT_Y;
    case ID_ALT_Z                   : return MAC_ALT_Z;
    }

  return -1;
  }   // end of MacroCommandIdToSlot

// ── OSVersionString ─────────────────────────────────────────────────────────
// The operating-system name world.Debug ("summary") reports.  Lifted out of
// world_debug.cpp (which only ever links into the GUI product) so the
// non-Windows arm is reachable from the test suite.  The Windows switch below
// is the original verbatim.  Declared by its callers, like the other externs
// at the top of world_debug.cpp.

CString OSVersionString (void)
  {

#ifndef _WIN32

  // Non-Windows: os_version is filled by the GetVersionEx shim, which reports a
  // fixed Windows 10 and knows nothing about this machine, so ask the kernel
  // instead.  Gives e.g. "Darwin 24.6.0".

  struct utsname sUts;

  if (uname (&sUts) == 0)
    return CFormat ("%s %s", sUts.sysname, sUts.release);

  // uname failed — fall through to the generic description below.

#endif  // !_WIN32

  // work out operating system

  CString sVersion = TFormat ("Unknown (Platform %u, Major %u, Minor %u)",
                             (unsigned) os_version.dwPlatformId,
                             (unsigned) os_version.dwMajorVersion,
                             (unsigned) os_version.dwMinorVersion);

  if (os_version.dwPlatformId == VER_PLATFORM_WIN32_WINDOWS)  // Windows 95-style versions
    {
    switch (os_version.dwMinorVersion)
      {
      case  0: sVersion = "Windows 95"; break;
      case 10: sVersion = "Windows 98"; break;
      case 90: sVersion = "Windows ME"; break;
      } // end of switch on dwMinorVersion
    }  // end of dwPlatformId == 1
  else if  (os_version.dwPlatformId == VER_PLATFORM_WIN32_NT)  // Windows NT versions
    {
    switch (os_version.dwMajorVersion)
      {
      case 3: sVersion = "Windows NT 3.51"; break;
      case 4: sVersion = "Windows NT";      break;
      case 5:   // Major version 5
        switch (os_version.dwMinorVersion)
          {
          case 0: sVersion = "Windows 2000";        break;
          case 1: sVersion = "Windows XP";          break;
          case 2: sVersion = "Windows Server 2003"; break;
          } // end of switch on dwMinorVersion
        break;  // end case 5 of dwMajorVersion

      case 6:   // Major version 6
        switch (os_version.dwMinorVersion)
          {
          case 0: sVersion = "Windows Vista"; break;
          case 1: sVersion = "Windows 7";     break;
          case 2: sVersion = "Windows 8";     break;
          case 3: sVersion = "Windows 8.1";   break;
          } // end of switch on dwMinorVersion
        break;  // end case 6 of dwMajorVersion

      case 10:  // Major version 10
        switch (os_version.dwMinorVersion)
          {
          case 0: sVersion = "Windows 10"; break;
          } // end of switch on dwMinorVersion
        break;  // end case 10 of dwMajorVersion

      }  // end of switch on dwMajorVersion
    }  // end of dwPlatformId == 2

  return sVersion;

  }   // end of OSVersionString
