// mcdatetime.cpp

// MUSHclient's date/time class
// Handles sub-second intervals


#include "stdafx.h"

// The injectable clock is consulted by GetTimeNow() on every platform, so both
// the declaration and its single definition must be unconditional.  Installing
// one is a test-only act; g_clock is nullptr in production everywhere, which is
// the "read the real system clock" path this file has always taken.
#include "pal/iclock.h"

namespace pal {
// The installed clock; nullptr = the real system clock (the production
// default — see pal/iclock.h).  Lives here so every build with the datetime
// foundation links exactly one definition.
IClock* g_clock = nullptr;
}

#ifndef _WIN32
// OLE Automation Date (DATE) is a double: days since Dec 30, 1899.
// On POSIX we define it as double and provide helpers to convert it.
typedef double DATE;

#include <time.h>
#include <ctime>
#include <cmath>

// Offset in days between OLE epoch (Dec 30, 1899) and Unix epoch (Jan 1, 1970)
static const double OLE_UNIX_EPOCH_DIFF = 25569.0;

// Convert an OLE date (double) to a struct tm holding the same date/time
// components that were originally stored.
//
// OLE DATE (as used by Windows COleDateTime and mcDateFromTm) stores local
// time with no timezone offset — the raw hour/minute/day values are encoded
// directly into the fractional-days representation.  To round-trip correctly
// on POSIX we must NOT apply a timezone conversion: we compute a pseudo Unix
// timestamp whose UTC interpretation (gmtime_r) gives back those same
// components.  Using localtime_r instead would shift the hour by the UTC
// offset a second time and produce wrong results on any non-UTC system.
static bool ole_to_localtime(double ole_date, struct tm& tm_out, int* ms_out = nullptr)
{
    // Round to nearest second to avoid floating-point truncation errors.
    time_t t = (time_t)llround((ole_date - OLE_UNIX_EPOCH_DIFF) * 86400.0);
    if (ms_out) {
        double frac = (ole_date - OLE_UNIX_EPOCH_DIFF) * 86400.0 - t;
        *ms_out = (int)(frac * 1000.0);
    }
    // gmtime_r: no timezone shift — preserves the local-time components stored
    // by mcDateFromTm / SetDateTime.
    return gmtime_r(&t, &tm_out) != nullptr;
}
#endif // !_WIN32


#define MIN_DATE                0  // year 1800
#define MAX_DATE                2958465L    // about year 9999

// Half a second, expressed in days
#define HALF_SECOND  (1.0/172800.0)

#define SECS_IN_DAY (60.0 * 60.0 * 24.0)

	// DateTime math
CmcDateTime CmcDateTime::operator+(const CmcDateTimeSpan& dateSpan) const
  {
	  CmcDateTime dateResult;    // Initializes m_status to valid

	  // If either operand NULL, result NULL
	  if (GetStatus() == null || dateSpan.GetStatus() == null)
	    {
		  dateResult.SetStatus(null);
		  return dateResult;
	    }

	  // If either operand invalid, result invalid
	  if (GetStatus() == invalid || dateSpan.GetStatus() == invalid)
	    {
		  dateResult.SetStatus(invalid);
		  return dateResult;
	    }

	  // Compute the actual date difference by adding underlying dates
	  dateResult = m_dt + dateSpan.m_span;

	  // Validate within range
//	    dateResult.CheckRange();

	  return dateResult;
  }  // end of CmcDateTime CmcDateTime::operator+

CmcDateTime CmcDateTime::operator-(const CmcDateTimeSpan& dateSpan) const
  {
	  CmcDateTime dateResult;    // Initializes m_status to valid

	  // If either operand NULL, result NULL
	  if (GetStatus() == null || dateSpan.GetStatus() == null)
	    {
		  dateResult.SetStatus(null);
		  return dateResult;
	    }

	  // If either operand invalid, result invalid
	  if (GetStatus() == invalid || dateSpan.GetStatus() == invalid)
	    {
		  dateResult.SetStatus(invalid);
		  return dateResult;
	    }

	  // Compute the actual date difference by subtracting underlying dates
	  dateResult = m_dt - dateSpan.m_span;

	  // Validate within range
//	    dateResult.CheckRange();

	  return dateResult;
  }  // end of CmcDateTime CmcDateTime::operator-


const CmcDateTime& CmcDateTime::operator+=(const CmcDateTimeSpan& dateSpan)
  { 
  *this = *this + dateSpan; 
  return *this; 
  }   // end of CmcDateTime& CmcDateTime::operator+=

const CmcDateTime& CmcDateTime::operator-=(const CmcDateTimeSpan& dateSpan)
  { 
  *this = *this - dateSpan; 
  return *this; 
  }   //  end of CmcDateTime& CmcDateTime::operator-=

void CmcDateTime::CheckRange()
{
	if (m_dt > MAX_DATE || m_dt < MIN_DATE) // about year 100 to about 9999
		SetStatus(invalid);
}


// DateTimeSpan math
CmcDateTimeSpan CmcDateTime::operator-(const CmcDateTime& date) const
  {
#ifdef _WIN32
	COleDateTimeSpan spanResult;

	// If either operand NULL, result NULL
	if (GetStatus() == null || date.GetStatus() == null)
	{
		spanResult.SetStatus(COleDateTimeSpan::null);
		return (double)spanResult;
	}

	// If either operand invalid, result invalid
	if (GetStatus() == invalid || date.GetStatus() == invalid)
	{
		spanResult.SetStatus(COleDateTimeSpan::invalid);
		return (double)spanResult;
	}

	// Return result (span can't be invalid, so don't check range)
	return m_dt - date.m_dt;
#else // !_WIN32
    CmcDateTimeSpan spanResult;
    if (GetStatus() == null || date.GetStatus() == null)
    {
        spanResult.SetStatus(CmcDateTimeSpan::null);
        return spanResult;
    }
    if (GetStatus() == invalid || date.GetStatus() == invalid)
    {
        spanResult.SetStatus(CmcDateTimeSpan::invalid);
        return spanResult;
    }
    return CmcDateTimeSpan(m_dt - date.m_dt);
#endif // _WIN32
  }


#ifdef _WIN32
int CmcDateTime::GetYear() const
  {
  COleDateTime t (m_dt);
  return t.GetYear ();
  }

int CmcDateTime::GetMonth() const       // month of year (1 = Jan)
  {
  COleDateTime t (m_dt);
  return t.GetMonth ();
  }

int CmcDateTime::GetDay() const         // day of month (0-31)
  {
  COleDateTime t (m_dt);
  return t.GetDay ();
  }

int CmcDateTime::GetHour() const        // hour in day (0-23)
  {
  COleDateTime t (m_dt);
  return t.GetHour ();
  }

int CmcDateTime::GetMinute() const      // minute in hour (0-59)
  {
  COleDateTime t (m_dt);
  return t.GetMinute ();
  }

int CmcDateTime::GetSecond() const      // second in minute (0-59)
  {
  COleDateTime t (m_dt);
  return t.GetSecond ();
  }

int CmcDateTime::GetDayOfWeek() const   // 1=Sun, 2=Mon, ..., 7=Sat
  {
  COleDateTime t (m_dt);
  return t.GetDayOfWeek ();
  }

int CmcDateTime::GetDayOfYear() const   // days since start of year, Jan 1 = 1
  {
  COleDateTime t (m_dt);
  return t.GetDayOfYear ();
  }
#else // !_WIN32
// POSIX implementations: convert OLE date to local time via time_t
int CmcDateTime::GetYear() const
  { struct tm t; ole_to_localtime(m_dt, t); return t.tm_year + 1900; }

int CmcDateTime::GetMonth() const
  { struct tm t; ole_to_localtime(m_dt, t); return t.tm_mon + 1; }

int CmcDateTime::GetDay() const
  { struct tm t; ole_to_localtime(m_dt, t); return t.tm_mday; }

int CmcDateTime::GetHour() const
  { struct tm t; ole_to_localtime(m_dt, t); return t.tm_hour; }

int CmcDateTime::GetMinute() const
  { struct tm t; ole_to_localtime(m_dt, t); return t.tm_min; }

int CmcDateTime::GetSecond() const
  { struct tm t; ole_to_localtime(m_dt, t); return t.tm_sec; }

int CmcDateTime::GetDayOfWeek() const   // 1=Sun, 2=Mon, ..., 7=Sat
  { struct tm t; ole_to_localtime(m_dt, t); return t.tm_wday + 1; }

int CmcDateTime::GetDayOfYear() const   // days since start of year, Jan 1 = 1
  { struct tm t; ole_to_localtime(m_dt, t); return t.tm_yday + 1; }
#endif // _WIN32

// One-based array of days in year at month start
static int MonthDays[13] =
	{0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365};


static BOOL  mcDateFromTm(WORD wYear, WORD wMonth, WORD wDay,
	WORD wHour, WORD wMinute, WORD wSecond, DATE& dtDest)
{
	// Validate year and month (ignore day of week and milliseconds)
	if (wYear > 9999 || wMonth < 1 || wMonth > 12)
		return FALSE;

	//  Check for leap year and set the number of days in the month
	BOOL bLeapYear = ((wYear & 3) == 0) &&
		((wYear % 100) != 0 || (wYear % 400) == 0);

	int nDaysInMonth =
		MonthDays[wMonth] - MonthDays[wMonth-1] +
		((bLeapYear && wDay == 29 && wMonth == 2) ? 1 : 0);

	// Finish validating the date
	if (wDay < 1 || wDay > nDaysInMonth ||
		wHour > 23 || wMinute > 59 ||
		wSecond > 59)
	{
		return FALSE;
	}

	// Cache the date in days and time in fractional days
	long nDate;
	double dblTime;

	//It is a valid date; make Jan 1, 1AD be 1
	nDate = wYear*365L + wYear/4 - wYear/100 + wYear/400 +
		MonthDays[wMonth-1] + wDay;

	//  If leap year and it's before March, subtract 1:
	if (wMonth <= 2 && bLeapYear)
		--nDate;

	//  Offset so that 12/30/1899 is 0
	nDate -= 693959L;

	dblTime = (((long)wHour * 3600L) +  // hrs in seconds
		((long)wMinute * 60L) +  // mins in seconds
		  wSecond) / SECS_IN_DAY;

	dtDest = (double) nDate + ((nDate >= 0) ? dblTime : -dblTime);

	return TRUE;
}  // end of mcDateFromTm

int CmcDateTime::SetDateTime(int nYear, int nMonth, int nDay,
	int nHour, int nMin, double nSec)
{
	return m_status = mcDateFromTm((WORD)nYear, (WORD)nMonth,
		(WORD)nDay, (WORD)nHour, (WORD)nMin, nSec, m_dt) ?
		valid : invalid;
}


/*

  This considerable amount of mucking around is because the date/time routines
  do not seem to return a time to more than 1-second granularity, regardless of
  the fact that GetSystemTime is supposed to return time to the millisecond.

  I guess this is because the real-time clocks are only updated every second.

  So, what I am doing here is taking the 'real' time every RESYNC_EVERY_SECS
  seconds, and then using the high-performance counter to find the number of
  seconds (fractions of seconds) that elapsed since. This is then added to the
  last "real" time to find the actual time, effectively with much higher
  granularity.

  */

#define RESYNC_EVERY_SECS 60

CmcDateTime CmcDateTime::GetTimeNow ()
  {
  // Injectable time source (pal::IClock): when a clock is installed (tests /
  // a virtual-time frontend) it is the authority; otherwise fall through to
  // the real system clock below, unchanged.
  if (pal::g_clock)
    return CmcDateTime (pal::g_clock->nowOle ());

#ifdef _WIN32
static bool bFirstTime = true;

static CmcDateTime last_date;
static LARGE_INTEGER last_time;
static LONGLONG iCounterFrequency = 0;

  if (bFirstTime)
    {
    bFirstTime = false;
    LARGE_INTEGER large_int_frequency;
    if (QueryPerformanceFrequency (&large_int_frequency))
      {
      iCounterFrequency = large_int_frequency.QuadPart;
      QueryPerformanceCounter (&last_time);
      }
    }

  double secs = 0;
  LARGE_INTEGER time_now;

  if (iCounterFrequency)
    {
    QueryPerformanceCounter (&time_now);

    LONGLONG offset = time_now.QuadPart - last_time.QuadPart;
    secs = (double) offset / (double) iCounterFrequency;
//    TRACE ("Secs = %10.4f\n", secs);
    }
  else
    {
    time_now.QuadPart = 0;
    time_now.QuadPart = 0;
    }


  // if no high-performance counter, just query the time each time
  if (iCounterFrequency == 0 || 
      last_date.m_dt == 0 ||
      secs > RESYNC_EVERY_SECS)
    {
    SYSTEMTIME systime;
    GetLocalTime (&systime);
 	  last_date = CmcDateTime(systime.wYear, systime.wMonth,
		                  systime.wDay, systime.wHour, systime.wMinute,
		                  (double) systime.wSecond + ( (double) systime.wMilliseconds / 1000.0) );
    secs = 0;
    last_time = time_now;
    }

  CmcDateTime this_date (last_date);

  this_date.m_dt += secs / SECS_IN_DAY;

  return this_date;

#else // !_WIN32
  // POSIX: clock_gettime gives nanosecond precision directly — no need for the
  // performance-counter resync trick used on Windows.
  //
  // But the fraction must NOT be handed to the constructor.  SetDateTime takes
  // a `double nSec` and passes it straight into mcDateFromTm, whose parameter
  // is a WORD — so the implicit conversion truncates, and every sub-second
  // timer was quantised to 1 Hz (a 0.5 s timer fired once a second; DoAfter(0.2)
  // landed anywhere up to a second late).  Windows does not hit this because it
  // never puts a fraction through the constructor either: it builds from whole
  // seconds and then adds the elapsed QueryPerformanceCounter delta straight
  // onto m_dt, a few lines up.
  //
  // Same shape here: whole seconds through the constructor, the fraction added
  // to the DATE double afterwards.  m_dt is days, so nanoseconds scale by
  // 1e9 * SECS_IN_DAY.
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  struct tm local_tm;
  localtime_r(&ts.tv_sec, &local_tm);
  CmcDateTime whole (local_tm.tm_year + 1900,
                     local_tm.tm_mon  + 1,
                     local_tm.tm_mday,
                     local_tm.tm_hour,
                     local_tm.tm_min,
                     (double) local_tm.tm_sec);
  whole.m_dt += ((double) ts.tv_nsec / 1.0e9) / SECS_IN_DAY;
  return whole;
#endif // _WIN32
  }

void CmcDateTimeSpan::SetDateTimeSpan(
	long lDays, int nHours, int nMins, double nSecs)
{
	// Set date span by breaking into fractional days (all input ranges valid)
	m_span = lDays + ((double)nHours)/24 + ((double)nMins)/(24*60) +
		(nSecs)/(24*60*60);

	SetStatus(valid);
}


const CmcDateTimeSpan& CmcDateTimeSpan::operator=(double dblSpanSrc)
{
	m_span = dblSpanSrc;
	SetStatus(valid);
	return *this;
}

const CmcDateTimeSpan& CmcDateTimeSpan::operator=(const CmcDateTimeSpan& dateSpanSrc)
{
	m_span = dateSpanSrc.m_span;
	m_status = dateSpanSrc.m_status;
	return *this;
}


CmcDateTimeSpan CmcDateTimeSpan::operator+(const CmcDateTimeSpan& dateSpan) const
  {
	CmcDateTimeSpan dateSpanTemp;

	// If either operand Null, result Null
	if (GetStatus() == null || dateSpan.GetStatus() == null)
	{
		dateSpanTemp.SetStatus(null);
		return dateSpanTemp;
	}

	// If either operand Invalid, result Invalid
	if (GetStatus() == invalid || dateSpan.GetStatus() == invalid)
	{
		dateSpanTemp.SetStatus(invalid);
		return dateSpanTemp;
	}

	// Add spans and validate within legal range
	dateSpanTemp.m_span = m_span + dateSpan.m_span;
//	dateSpanTemp.CheckRange();

	return dateSpanTemp;
  }

CmcDateTimeSpan CmcDateTimeSpan::operator-(const CmcDateTimeSpan& dateSpan) const
  {
	CmcDateTimeSpan dateSpanTemp;

	// If either operand Null, result Null
	if (GetStatus() == null || dateSpan.GetStatus() == null)
	{
		dateSpanTemp.SetStatus(null);
		return dateSpanTemp;
	}

	// If either operand Invalid, result Invalid
	if (GetStatus() == invalid || dateSpan.GetStatus() == invalid)
	{
		dateSpanTemp.SetStatus(invalid);
		return dateSpanTemp;
	}

	// Subtract spans and validate within legal range
	dateSpanTemp.m_span = m_span - dateSpan.m_span;
//	dateSpanTemp.CheckRange();

	return dateSpanTemp;
  }

