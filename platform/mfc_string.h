
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
// platform/mfc_string.h
// CString implementation backed by std::string for non-Windows platforms.
//
// This is intentionally a close functional clone of the MFC CString API
// (MultiByte / MBCS variant) so that existing MUSHclient code compiles
// without modification.  It is NOT a complete implementation — only the
// methods actually used in the MUSHclient source are provided.
//
// ── vararg / printf compatibility ────────────────────────────────────────────
// MFC's CString is designed so that when an instance is passed by value as a
// variadic argument (e.g. TFormat("...%s...", someCString)), the ABI places a
// valid char* at offset 0 of the pushed object.  vsnprintf's %s handler then
// does va_arg(ap, char*) which reads those first bytes as a pointer.
//
// We replicate this by keeping `m_psz` as the very first member of CString
// and calling sync() after every mutation to keep it pointing at s_.c_str().
// This means CString objects can be passed to printf-family functions with %s
// without an explicit (LPCTSTR) cast, matching existing Windows behaviour.

#pragma once

#ifndef _WIN32

#include <string>
#include <vector>
#include <cstdio>
#include <cstdarg>
#include <cctype>
#include <algorithm>
#include <stdexcept>

class CString
{
    // ── IMPORTANT: m_psz MUST remain the first member ─────────────────────────
    // vararg ABI relies on it being at offset 0 (see file header).
    const char* m_psz = "";  // always == s_.c_str(); kept in sync after mutations

    std::string        s_;
    std::vector<char>  buf_; // scratch buffer for GetBuffer / ReleaseBuffer

    // Re-point m_psz at the live string data.  Call after every mutation of s_.
    void sync() noexcept { m_psz = s_.c_str(); }

public:
    // ── Constructors ──────────────────────────────────────────────────────────

    CString() noexcept              : m_psz(""), s_()                             { sync(); }
    CString(const CString& o)       : m_psz(""), s_(o.s_)                        { sync(); }
    CString(CString&& o) noexcept   : m_psz(""), s_(std::move(o.s_))             { sync(); o.sync(); }
    CString(std::nullptr_t)         : m_psz(""), s_("")                           { sync(); }
    CString(const char* p)          : m_psz(""), s_(p ? p : "")                  { sync(); }
    CString(const unsigned char* p) : m_psz(""), s_(p ? reinterpret_cast<const char*>(p) : "") { sync(); }
    // MFC: CString(LPCTSTR lpsz, int nLength) — copy first nLength chars
    CString(const char* p, int n)   : m_psz(""), s_(p ? p : "", (size_t)(n < 0 ? 0 : n)) { sync(); }
    CString(const std::string& s)   : m_psz(""), s_(s)                           { sync(); }
    CString(std::string&& s)        : m_psz(""), s_(std::move(s))                { sync(); }
    // Construct from a single character (MFC: CString(TCHAR ch, int nRepeat=1))
    // Not explicit — MFC allows implicit construction from char.
    CString(char ch, int nRepeat = 1) : m_psz(""), s_(nRepeat, ch)               { sync(); }

    // ── Assignment ────────────────────────────────────────────────────────────

    CString& operator=(const CString& rhs)     { s_ = rhs.s_;       sync(); return *this; }
    CString& operator=(CString&& rhs)          { s_ = std::move(rhs.s_); sync(); rhs.sync(); return *this; }
    CString& operator=(const char* p)          { s_ = p ? p : "";   sync(); return *this; }
    CString& operator=(const unsigned char* p) { s_ = p ? reinterpret_cast<const char*>(p) : ""; sync(); return *this; }
    CString& operator=(char ch)                { s_ = std::string(1, ch); sync(); return *this; }
    CString& operator=(const std::string& s)   { s_ = s;            sync(); return *this; }

    // ── Implicit conversion to const char* ────────────────────────────────────
    // MFC CString implicitly converts to LPCTSTR in MultiByte builds.
    // Returns m_psz (== s_.c_str()) so the same pointer is seen by vararg callers.
    operator const char*() const { return m_psz; }

    // Explicit helpers (matches GetBuffer(0) / std::string API for read-only access)
    const char* GetString() const { return m_psz; }
    const char* c_str()     const { return m_psz; }

    // ── Concatenation ─────────────────────────────────────────────────────────

    CString& operator+=(const CString& rhs)  { s_ += rhs.s_; sync(); return *this; }
    CString& operator+=(const char* p)       { if (p) s_ += p; sync(); return *this; }
    CString& operator+=(char ch)             { s_ += ch;      sync(); return *this; }

    friend CString operator+(const CString& a, const CString& b)
        { return CString(a.s_ + b.s_); }
    friend CString operator+(const CString& a, const char* b)
        { return CString(b ? a.s_ + b : a.s_); }
    friend CString operator+(const char* a, const CString& b)
        { return CString(a ? std::string(a) + b.s_ : b.s_); }
    friend CString operator+(const CString& a, char b)
        { return CString(a.s_ + b); }
    friend CString operator+(char a, const CString& b)
        { std::string r(1, a); r += b.s_; return CString(r); }

    // ── Comparison operators ──────────────────────────────────────────────────
    // MFC CString comparisons are case-sensitive by default.

    bool operator==(const CString& o) const  { return s_ == o.s_; }
    bool operator==(const char*    p) const  { return p && s_ == p; }
    bool operator==(char           c) const  { return s_.size()==1 && s_[0]==c; }
    bool operator!=(const CString& o) const  { return s_ != o.s_; }
    bool operator!=(const char*    p) const  { return !p || s_ != p; }
    bool operator!=(char           c) const  { return !(s_.size()==1 && s_[0]==c); }
    bool operator< (const CString& o) const  { return s_ <  o.s_; }
    bool operator<=(const CString& o) const  { return s_ <= o.s_; }
    bool operator> (const CString& o) const  { return s_ >  o.s_; }
    bool operator>=(const CString& o) const  { return s_ >= o.s_; }

    friend bool operator==(const char* p, const CString& s) { return s == p; }
    friend bool operator!=(const char* p, const CString& s) { return s != p; }

    // ── Length / empty ────────────────────────────────────────────────────────

    int  GetLength() const   { return (int)s_.size(); }
    bool IsEmpty()   const   { return s_.empty(); }
    void Empty()             { s_.clear(); sync(); }

    // ── Character access ──────────────────────────────────────────────────────

    char GetAt(int i) const  { return s_[i]; }
    char operator[](int i) const { return s_[i]; }
    void SetAt(int i, char ch)   { s_[i] = ch; /* s_.c_str() unchanged — no sync needed */ }

    // ── Substrings ────────────────────────────────────────────────────────────

    // MFC clamps out-of-range args (returns empty/whole) rather than throwing;
    // std::string::substr throws std::out_of_range when pos > size (and a negative
    // int cast to size_t is huge), so clamp to match MFC and avoid crashes on
    // truncated input (the ~100 Mid call sites in protocol/MXP/chat parsing).
    CString Left(int n) const {
        int len = (int)s_.size();
        if (n <= 0)   return CString();
        if (n >= len) return *this;
        return CString(s_.substr(0, (size_t)n));
    }
    CString Right(int n) const {
        int len = (int)s_.size();
        if (n <= 0)   return CString();
        if (n >= len) return *this;
        return CString(s_.substr((size_t)(len - n)));
    }
    CString Mid(int pos) const {
        int len = (int)s_.size();
        if (pos < 0)    pos = 0;
        if (pos >= len) return CString();
        return CString(s_.substr((size_t)pos));
    }
    CString Mid(int pos, int count) const {
        int len = (int)s_.size();
        if (pos < 0)    pos = 0;
        if (pos >= len) return CString();
        if (count < 0)  count = len - pos;   // MFC: negative count → to end
        return CString(s_.substr((size_t)pos, (size_t)count));
    }

    // ── Search ────────────────────────────────────────────────────────────────

    // Returns -1 if not found (MFC convention)
    int Find(const char* sub, int start = 0) const {
        if (!sub) return -1;
        auto p = s_.find(sub, (size_t)start);
        return (p == std::string::npos) ? -1 : (int)p;
    }
    int Find(char ch, int start = 0) const {
        auto p = s_.find(ch, (size_t)start);
        return (p == std::string::npos) ? -1 : (int)p;
    }
    int ReverseFind(char ch) const {
        auto p = s_.rfind(ch);
        return (p == std::string::npos) ? -1 : (int)p;
    }
    // Check whether string starts with prefix
    bool StartsWith(const char* prefix) const {
        if (!prefix) return false;
        size_t n = strlen(prefix);
        return s_.size() >= n && s_.compare(0, n, prefix) == 0;
    }

    // ── Case conversion ───────────────────────────────────────────────────────

    void MakeUpper() {
        for (char& c : s_) c = (char)toupper((unsigned char)c);
        sync();
    }
    void MakeLower() {
        for (char& c : s_) c = (char)tolower((unsigned char)c);
        sync();
    }
    CString MakeUpperCopy() const { CString r(*this); r.MakeUpper(); return r; }
    CString MakeLowerCopy() const { CString r(*this); r.MakeLower(); return r; }

    // ── Trim ──────────────────────────────────────────────────────────────────

    void TrimLeft() {
        s_.erase(s_.begin(),
                 std::find_if(s_.begin(), s_.end(),
                              [](unsigned char c){ return !std::isspace(c); }));
        sync();
    }
    void TrimLeft(char ch) {
        size_t p = s_.find_first_not_of(ch);
        s_ = (p == std::string::npos) ? "" : s_.substr(p);
        sync();
    }
    void TrimLeft(const char* chars) {
        if (!chars) return;
        size_t p = s_.find_first_not_of(chars);
        s_ = (p == std::string::npos) ? "" : s_.substr(p);
        sync();
    }
    void TrimRight() {
        s_.erase(std::find_if(s_.rbegin(), s_.rend(),
                              [](unsigned char c){ return !std::isspace(c); }).base(),
                 s_.end());
        sync();
    }
    void TrimRight(char ch) {
        size_t p = s_.find_last_not_of(ch);
        s_ = (p == std::string::npos) ? "" : s_.substr(0, p + 1);
        sync();
    }
    void TrimRight(const char* chars) {
        if (!chars) return;
        size_t p = s_.find_last_not_of(chars);
        s_ = (p == std::string::npos) ? "" : s_.substr(0, p + 1);
        sync();
    }
    void Trim() { TrimLeft(); TrimRight(); }

    // ── Replace ───────────────────────────────────────────────────────────────

    // Returns number of replacements made (MFC convention)
    int Replace(const char* from, const char* to) {
        if (!from || !*from) return 0;
        int count = 0;
        size_t fromLen = strlen(from);
        size_t toLen   = to ? strlen(to) : 0;
        size_t pos = 0;
        while ((pos = s_.find(from, pos)) != std::string::npos) {
            s_.replace(pos, fromLen, to ? to : "");
            pos += toLen;
            ++count;
        }
        sync();
        return count;
    }
    int Replace(char from, char to) {
        int count = 0;
        for (char& c : s_) if (c == from) { c = to; ++count; }
        // in-place char swap: s_.c_str() pointer unchanged — no sync needed
        return count;
    }

    // ── Remove ────────────────────────────────────────────────────────────────
    // Removes all occurrences of the given character; returns count removed.
    int Remove(char ch) {
        int count = 0;
        std::string result;
        result.reserve(s_.size());
        for (char c : s_) {
            if (c == ch) ++count;
            else result += c;
        }
        s_ = std::move(result);
        sync();
        return count;
    }

    // ── Delete ────────────────────────────────────────────────────────────────

    int Delete(int index, int count = 1) {
        s_.erase((size_t)index, (size_t)count);
        sync();
        return (int)s_.size();
    }
    void Insert(int index, const char* str) {
        if (str) s_.insert((size_t)index, str);
        sync();
    }
    void Insert(int index, char ch) {
        s_.insert((size_t)index, 1, ch);
        sync();
    }

    // ── Format (printf-style) ─────────────────────────────────────────────────

    MC_PRINTF_FORMAT(2, 3) void Format(const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        FormatV(fmt, args);
        va_end(args);
        // sync() called by FormatV
    }
    void FormatV(const char* fmt, va_list args) {
        // Translate the MSVC 64-bit length modifier "I64" (e.g. %I64d, %08I64x)
        // to the portable "ll" — vsnprintf off-Windows does not understand
        // %I64 and would mis-parse it (producing wrong, often non-unique output;
        // GetUniqueString() relied on %I64d).  Only rewrite inside conversion
        // specifiers so literal text is left untouched.
        static const std::string kConv = "diouxXeEfgGaAcCspn";
        std::string xlat;
        xlat.reserve(std::char_traits<char>::length(fmt) + 8);
        for (const char* p = fmt; *p; ) {
            if (*p != '%') { xlat += *p++; continue; }
            xlat += *p++;                                  // the '%'
            if (*p == '%') { xlat += *p++; continue; }     // literal "%%"
            while (*p && kConv.find(*p) == std::string::npos) {
                if (p[0] == 'I' && p[1] == '6' && p[2] == '4') { xlat += "ll"; p += 3; }
                else xlat += *p++;
            }
            if (*p) xlat += *p++;                          // the conversion char
        }
        const char* f = xlat.c_str();

        va_list args2;
        va_copy(args2, args);
        int n = vsnprintf(nullptr, 0, f, args);
        if (n < 0) { va_end(args2); return; }
        s_.resize((size_t)n);
        vsnprintf(&s_[0], (size_t)n + 1, f, args2);
        va_end(args2);
        sync();
    }

    // LoadString — load from resources; stubs return empty string on macOS
    bool LoadString(unsigned int /*nID*/) { s_.clear(); sync(); return false; }

    // ── Buffer access (for legacy C-style code) ───────────────────────────────
    // GetBuffer / ReleaseBuffer follow MFC semantics:
    //   GetBuffer(n)   — ensures capacity >= n, returns writable pointer
    //   ReleaseBuffer  — re-syncs length from null-terminated buffer

    char* GetBuffer(int nMinBufLength = 0) {
        if (nMinBufLength > 0)
            s_.resize((size_t)nMinBufLength, '\0');
        // Reserve one extra for the null terminator that callers may write
        buf_.assign(s_.begin(), s_.end());
        buf_.push_back('\0');
        return buf_.data();
    }
    void ReleaseBuffer(int nNewLength = -1) {
        if (nNewLength < 0)
            s_ = std::string(buf_.data()); // re-measure at null terminator
        else
            s_.assign(buf_.data(), (size_t)nNewLength);
        buf_.clear();
        sync();
    }

    // ── COM / OLE helpers ─────────────────────────────────────────────────────
    // AllocSysString — Windows OLE allocates a BSTR; on non-Windows return a
    // plain char* copy.  The caller owns the memory (delete[]).
    char* AllocSysString() const {
        char* p = new char[s_.size() + 1];
        std::copy(s_.begin(), s_.end(), p);
        p[s_.size()] = '\0';
        return p;
    }

    // ── Comparison functions (like strcmp, stricmp) ───────────────────────────

    int Compare(const char* p) const {
        return s_.compare(p ? p : "");
    }
    int CompareNoCase(const char* p) const {
        if (!p) return 1;
        std::string a = s_, b(p);
        for (char& c : a) c = (char)tolower((unsigned char)c);
        for (char& c : b) c = (char)tolower((unsigned char)c);
        return a.compare(b);
    }

    // ── Span operations ───────────────────────────────────────────────────────

    int SpanIncluding(const char* chars) const {
        if (!chars) return 0;
        return (int)s_.find_first_not_of(chars);
    }
    CString SpanExcluding(const char* chars) const {
        if (!chars) return *this;
        auto p = s_.find_first_of(chars);
        return (p == std::string::npos) ? *this : CString(s_.substr(0, p));
    }
    int FindOneOf(const char* chars) const {
        if (!chars) return -1;
        auto p = s_.find_first_of(chars);
        return (p == std::string::npos) ? -1 : (int)p;
    }

    // ── Underlying std::string access ─────────────────────────────────────────
    // Not part of MFC API but useful in platform-specific code.
    // CAUTION: if you mutate the string via StdString(), call
    //   reinterpret_cast<CString*>(this)->sync_public()
    // or just reassign the CString to re-sync m_psz.

    const std::string& StdString() const { return s_; }
    // Non-const: returns reference for mutation; caller must immediately
    // re-assign or use only the public mutating methods (which call sync()).
    std::string& StdString() { return s_; }

    // Public sync for rare cases where external code mutates StdString() directly.
    void SyncPtr() noexcept { sync(); }
};

// ── Non-member helpers ────────────────────────────────────────────────────────

// Allow: if (someString) { ... }  (MFC style, tests non-empty)
// (CString already implicitly converts to const char*, which is truthy if
//  the string is non-empty AND the pointer is non-null — but we want the
//  MFC semantic where an empty string is falsy, so this is accurate.)

#endif // !_WIN32
