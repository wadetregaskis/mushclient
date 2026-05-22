// platform/mfc_time.h
// CTime stub for non-Windows platforms.
//
// MUSHclient uses CTime mainly to record when a trigger/alias last fired.
// The full MFC CTime API wraps a time_t; we do the same.

#pragma once

#ifndef _WIN32

#include <ctime>
#include <string>
#include "windows_types.h"
#include "mfc_string.h"

// ── CTimeSpan — lightweight wrapper around a seconds count ────────────────
struct CTimeSpan {
    long m_secs = 0;
    explicit CTimeSpan(long s = 0) : m_secs(s) {}
    // Full constructor: days, hours, minutes, seconds
    CTimeSpan(long days, long hours, long mins, long secs)
        : m_secs(days * 86400L + hours * 3600L + mins * 60L + secs) {}
    long GetTotalSeconds() const { return m_secs; }
    long GetTotalMinutes() const { return m_secs / 60; }
    long GetTotalHours()   const { return m_secs / 3600; }
    long GetDays()         const { return m_secs / 86400; }
    // Component accessors (vs GetTotal* which are cumulative)
    long GetHours()   const { return (m_secs % 86400) / 3600; }
    long GetMinutes() const { return (m_secs % 3600) / 60; }
    long GetSeconds() const { return m_secs % 60; }
    // Arithmetic
    CTimeSpan operator+(const CTimeSpan& o) const { return CTimeSpan(m_secs + o.m_secs); }
    CTimeSpan operator-(const CTimeSpan& o) const { return CTimeSpan(m_secs - o.m_secs); }
    CTimeSpan& operator+=(const CTimeSpan& o) { m_secs += o.m_secs; return *this; }
    CTimeSpan& operator-=(const CTimeSpan& o) { m_secs -= o.m_secs; return *this; }
    bool operator==(const CTimeSpan& o) const { return m_secs == o.m_secs; }
    bool operator!=(const CTimeSpan& o) const { return m_secs != o.m_secs; }
    bool operator< (const CTimeSpan& o) const { return m_secs <  o.m_secs; }
    bool operator<=(const CTimeSpan& o) const { return m_secs <= o.m_secs; }
    bool operator> (const CTimeSpan& o) const { return m_secs >  o.m_secs; }
    bool operator>=(const CTimeSpan& o) const { return m_secs >= o.m_secs; }
    // Format — stub returns empty string (no locale support on macOS stub)
    CString Format(LPCTSTR /*pszFormat*/) const { return {}; }
    CString Format(UINT /*nID*/) const { return {}; }
};

class CTime
{
public:
    // ── Constructors ──────────────────────────────────────────────────────

    CTime()                   : t_(0)   {}
    explicit CTime(time_t t)  : t_(t)   {}
    CTime(const CTime& o)     : t_(o.t_) {}
    // Construct from SYSTEMTIME (Windows calendar struct) — stub, sets to epoch
    CTime(const struct SYSTEMTIME& st) : t_(0) {
        struct tm tm = {};
        tm.tm_year = st.wYear - 1900;
        tm.tm_mon  = st.wMonth - 1;
        tm.tm_mday = st.wDay;
        tm.tm_hour = st.wHour;
        tm.tm_min  = st.wMinute;
        tm.tm_sec  = st.wSecond;
        tm.tm_isdst = -1;
        t_ = mktime(&tm);
        if (t_ == (time_t)-1) t_ = 0;
    }

    // Construct from year/month/day/hour/min/sec (local time)
    CTime(int year, int month, int day, int hour, int min, int sec, int /*dst*/ = -1) {
        struct tm tm = {};
        tm.tm_year  = year - 1900;
        tm.tm_mon   = month - 1;
        tm.tm_mday  = day;
        tm.tm_hour  = hour;
        tm.tm_min   = min;
        tm.tm_sec   = sec;
        tm.tm_isdst = -1;
        t_ = mktime(&tm);
    }

    // ── Static factory ────────────────────────────────────────────────────

    static CTime GetCurrentTime() { return CTime(::time(nullptr)); }

    // ── Assignment / comparison ───────────────────────────────────────────

    CTime& operator=(const CTime& o)           { t_ = o.t_; return *this; }
    CTime& operator=(time_t t)                 { t_ = t;    return *this; }

    bool operator==(const CTime& o) const      { return t_ == o.t_; }
    bool operator!=(const CTime& o) const      { return t_ != o.t_; }
    bool operator< (const CTime& o) const      { return t_ <  o.t_; }
    bool operator<=(const CTime& o) const      { return t_ <= o.t_; }
    bool operator> (const CTime& o) const      { return t_ >  o.t_; }
    bool operator>=(const CTime& o) const      { return t_ >= o.t_; }

    // ── Arithmetic ────────────────────────────────────────────────────────

    CTime     operator+(long seconds)          const { return CTime(t_ + seconds); }
    CTime     operator+(const CTimeSpan& ts)   const { return CTime(t_ + ts.m_secs); }
    CTime     operator-(long seconds)          const { return CTime(t_ - seconds); }
    CTimeSpan operator-(const CTime& o)        const { return CTimeSpan((long)(t_ - o.t_)); }

    // ── Accessors ─────────────────────────────────────────────────────────

    time_t GetTime() const  { return t_; }
    int GetYear()   const   { return localFields().tm_year + 1900; }
    int GetMonth()  const   { return localFields().tm_mon  + 1; }
    int GetDay()    const   { return localFields().tm_mday; }
    int GetHour()   const   { return localFields().tm_hour; }
    int GetMinute() const   { return localFields().tm_min;  }
    int GetSecond() const   { return localFields().tm_sec;  }
    int GetDayOfWeek() const { return localFields().tm_wday + 1; } // 1=Sun

    // ── Formatting ────────────────────────────────────────────────────────

    CString Format(const char* fmt) const {
        // On non-Windows, strftime does not understand the Windows-specific
        // %# flag (which removes leading zeros).  Map "%#X" → "%-X" which is
        // the POSIX/glibc/macOS equivalent (no leading zero).
        std::string cleanFmt;
        cleanFmt.reserve(strlen(fmt) + 1);
        for (const char* p = fmt; *p; ++p) {
            if (p[0] == '%' && p[1] == '#') {
                cleanFmt += "%-";
                ++p;  // skip the '#'
            } else {
                cleanFmt += *p;
            }
        }
        char buf[256];
        struct tm tm = localFields();
        strftime(buf, sizeof(buf), cleanFmt.c_str(), &tm);
        return CString(buf);
    }

    // ── Conversion ────────────────────────────────────────────────────────

    operator time_t() const { return t_; }

private:
    time_t t_;

    struct tm localFields() const {
        struct tm result;
#ifdef _WIN32
        localtime_s(&result, &t_);
#else
        localtime_r(&t_, &result);
#endif
        return result;
    }
};

#endif // !_WIN32
