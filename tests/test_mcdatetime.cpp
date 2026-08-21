// tests/test_mcdatetime.cpp
// Unit tests for CmcDateTime / CmcDateTimeSpan.

#include "mc_test.h"
#include <set>
#include <ctime>
#include "stdafx.h"
#include "mcdatetime.h"
#include "pal/iclock.h"        // pal::g_clock — must be null for the real path

// mcdatetime.cpp keeps this private; the test needs it to read m_dt as seconds.
static const double kSecsInDay = 60.0 * 60.0 * 24.0;

// ── CmcDateTime basic construction ───────────────────────────────────────

TEST(CmcDateTimeTest, DefaultConstructorIsValid) {
    CmcDateTime dt;
    EXPECT_EQ(CmcDateTime::valid, dt.GetStatus());
}

TEST(CmcDateTimeTest, ConstructFromComponents) {
    CmcDateTime dt(2024, 6, 15, 10, 30, 0.0);
    EXPECT_EQ(CmcDateTime::valid, dt.GetStatus());
    EXPECT_EQ(2024, dt.GetYear());
    EXPECT_EQ(6,    dt.GetMonth());
    EXPECT_EQ(15,   dt.GetDay());
    EXPECT_EQ(10,   dt.GetHour());
    EXPECT_EQ(30,   dt.GetMinute());
}

// mcDateFromTm's seconds parameter is a double, so a fractional second survives
// construction.  It used to be a WORD: an "at time" timer configured for 30.75
// seconds (world.SetTimerOption "second" allows up to 59.9999, and the Timer
// dialog validates the same range) was built as 30.0 and fired up to a second
// early.
TEST(CmcDateTimeTest, ConstructorKeepsFractionalSeconds) {
    CmcDateTime dt(2024, 1, 1, 0, 0, 30.75);
    ASSERT_EQ(CmcDateTime::valid, dt.GetStatus());

    const CmcDateTime midnight(2024, 1, 1, 0, 0, 0.0);
    ASSERT_EQ(CmcDateTime::valid, midnight.GetStatus());

    // seconds since midnight, read off the DATE double.  The tolerance is set by
    // what a DATE can hold at this magnitude (~1 microsecond), not by the defect,
    // which was three quarters of a second.
    const double secs = (dt.m_dt - midnight.m_dt) * kSecsInDay;
    EXPECT_NEAR(30.75, secs, 1e-4);

    // widening the parameter must not have widened the range it accepts
    CmcDateTime edge(2024, 1, 1, 0, 0, 59.9999);   // the configured maximum
    EXPECT_EQ(CmcDateTime::valid, edge.GetStatus());
    CmcDateTime whole_minute(2024, 1, 1, 0, 0, 60.0);
    EXPECT_EQ(CmcDateTime::invalid, whole_minute.GetStatus());
    CmcDateTime negative(2024, 1, 1, 0, 0, -1.0);
    EXPECT_EQ(CmcDateTime::invalid, negative.GetStatus());
}

TEST(CmcDateTimeTest, GetTimeNowReturnsNonZero) {
    CmcDateTime now = CmcDateTime::GetTimeNow();
    EXPECT_EQ(CmcDateTime::valid, now.GetStatus());
    EXPECT_NE(0.0, now.m_dt);
}

// GetTimeNow must carry SUB-SECOND precision.  The POSIX arm used to hand its
// nanoseconds to the CmcDateTime constructor, which truncated them (mcDateFromTm
// took a WORD second), quantising every fractional timer to 1 Hz.  Windows
// sidesteps that by adding its QueryPerformanceCounter delta straight onto m_dt,
// and the POSIX arm now does the same.  (The constructor no longer truncates
// either -- see ConstructorKeepsFractionalSeconds -- but the precision pinned
// here is GetTimeNow's own.)
//
// This cannot be tested through the timer suite: those tests install
// pal::g_clock, whose nowOle() returns a raw double and so bypasses the
// constructor entirely -- which is exactly why a whole green suite never saw
// this.  Here g_clock is null, so the real code path runs.
TEST(CmcDateTimeTest, GetTimeNowHasSubSecondPrecision) {
    ASSERT_TRUE(pal::g_clock == nullptr);   // or the real path is not under test

    // Sample across a fraction of a second.  With the truncating constructor
    // every sample inside one wall-clock second is byte-identical, so the set
    // of distinct values is 1 (or 2 if a second boundary falls in the middle).
    std::set<double> seen;
    const CmcDateTime t0 = CmcDateTime::GetTimeNow();
    for (int i = 0; i < 40; i++) {
        seen.insert(CmcDateTime::GetTimeNow().m_dt);
        struct timespec req = { 0, 5 * 1000 * 1000 };   // 5 ms
        nanosleep(&req, nullptr);
    }
    const CmcDateTime t1 = CmcDateTime::GetTimeNow();

    EXPECT_TRUE(seen.size() > 5);   // truncation gives 1-2

    // ...and the elapsed time is a real fraction, not a whole number of
    // seconds.  ~200 ms of sleeping: more than zero, well under one second.
    const double elapsedSecs = (t1.m_dt - t0.m_dt) * kSecsInDay;
    EXPECT_TRUE(elapsedSecs > 0.05);
    EXPECT_TRUE(elapsedSecs < 1.0);
}

TEST(CmcDateTimeTest, CopyConstruct) {
    CmcDateTime a(2024, 1, 1, 0, 0, 0.0);
    CmcDateTime b(a);
    EXPECT_EQ(a.m_dt, b.m_dt);
    EXPECT_EQ(a.GetStatus(), b.GetStatus());
}

// ── CmcDateTimeSpan ───────────────────────────────────────────────────────

// Parameterised (audit §4): SpanInSeconds/Minutes/Hours/Days collapsed into one
// table over {days, hours, minutes, seconds → expected total seconds}.
TEST(CmcDateTimeSpanTest, SpanTotalSecondsFromComponents) {
    struct Case { int d, h, m; double s; double expectSecs; const char* note; };
    const Case cases[] = {
        { 0, 0, 0, 60.0,     60.0, "seconds" },
        { 0, 0, 5,  0.0,    300.0, "minutes" },
        { 0, 2, 0,  0.0,   7200.0, "hours"   },
        { 1, 0, 0,  0.0,  86400.0, "days"    },
    };
    for (const Case& c : cases) {
        CmcDateTimeSpan span;
        span.SetDateTimeSpan(c.d, c.h, c.m, c.s);
        EXPECT_NEAR(c.expectSecs, span.GetTotalSeconds(), 0.001);   // c.note
    }
}

TEST(CmcDateTimeSpanTest, SpanCompareLess) {
    CmcDateTimeSpan s1, s2;
    s1.SetDateTimeSpan(0, 0, 0, 10.0);
    s2.SetDateTimeSpan(0, 0, 0, 20.0);
    EXPECT_TRUE(s1 < s2);
    EXPECT_FALSE(s2 < s1);
}

TEST(CmcDateTimeSpanTest, SpanCompareGreater) {
    CmcDateTimeSpan s1, s2;
    s1.SetDateTimeSpan(0, 0, 0, 30.0);
    s2.SetDateTimeSpan(0, 0, 0, 20.0);
    EXPECT_TRUE(s1 > s2);
}

TEST(CmcDateTimeSpanTest, SpanEquality) {
    CmcDateTimeSpan s1, s2;
    s1.SetDateTimeSpan(0, 1, 30, 0.0);
    s2.SetDateTimeSpan(0, 1, 30, 0.0);
    EXPECT_TRUE(s1 == s2);
    EXPECT_FALSE(s1 != s2);
}

// ── Arithmetic ────────────────────────────────────────────────────────────

TEST(CmcDateTimeTest, SubtractTwoDatetimes) {
    CmcDateTime t1(2024, 1, 1, 12, 0, 0.0);
    CmcDateTime t2(2024, 1, 1, 13, 0, 0.0);
    CmcDateTimeSpan span = t2 - t1;
    // 1 hour = 3600 seconds; allow small tolerance for DST edge cases
    EXPECT_NEAR(3600.0, span.GetTotalSeconds(), 5.0);
}

TEST(CmcDateTimeTest, AddSpanToDatetime) {
    CmcDateTime t(2024, 1, 1, 12, 0, 0.0);
    CmcDateTimeSpan hour;
    hour.SetDateTimeSpan(0, 1, 0, 0.0);
    CmcDateTime t2 = t + hour;
    EXPECT_EQ(13, t2.GetHour());
}

// ── Invalid status ────────────────────────────────────────────────────────

TEST(CmcDateTimeTest, InvalidStatus) {
    CmcDateTime dt;
    dt.SetStatus(CmcDateTime::invalid);
    EXPECT_EQ(CmcDateTime::invalid, dt.GetStatus());
}

// ═══ Round-4 coverage: status-propagation arms + component getters ═════════

TEST(CmcDateTimeR4, NullAndInvalidStatusPropagation)
{
    CmcDateTime good(2026, 7, 16, 12, 0, 0);
    CmcDateTime nul;  nul.SetStatus(CmcDateTime::null);
    CmcDateTime bad;  bad.SetStatus(CmcDateTime::invalid);
    CmcDateTimeSpan day(1, 0, 0, 0);

    // adding to a NULL date stays null; to an INVALID date stays invalid
    CmcDateTime r1 = nul + day;
    EXPECT_EQ((int)CmcDateTime::null, (int)r1.GetStatus());
    CmcDateTime r2 = bad + day;
    EXPECT_EQ((int)CmcDateTime::invalid, (int)r2.GetStatus());
    CmcDateTime r3 = nul - day;
    EXPECT_EQ((int)CmcDateTime::null, (int)r3.GetStatus());
    CmcDateTime r4 = bad - day;
    EXPECT_EQ((int)CmcDateTime::invalid, (int)r4.GetStatus());

    // date-minus-date span inherits null/invalid too
    CmcDateTimeSpan s1 = nul - good;
    EXPECT_EQ((int)CmcDateTimeSpan::null, (int)s1.GetStatus());
    CmcDateTimeSpan s2 = bad - good;
    EXPECT_EQ((int)CmcDateTimeSpan::invalid, (int)s2.GetStatus());
}

TEST(CmcDateTimeR4, MinusEqualsAndCheckRange)
{
    CmcDateTime d(2026, 7, 16, 12, 0, 0);
    CmcDateTimeSpan day(1, 0, 0, 0);
    d -= day;
    EXPECT_EQ(15, d.GetDay());
    EXPECT_EQ(7, d.GetMonth());

    // CheckRange flags out-of-range dates invalid; it is protected and its
    // operator call-sites are commented out upstream, so reach it through a
    // test subclass
    struct RangeProbe : CmcDateTime {
        void Probe() { m_dt = -50000.0; CheckRange(); }
    } oor;
    oor.Probe();
    EXPECT_EQ((int)CmcDateTime::invalid, (int)oor.GetStatus());
}

TEST(CmcDateTimeR4, ComponentGetters)
{
    // 2026-07-16 is a Thursday (wday 4 → GetDayOfWeek 5, 1-based Sunday)
    CmcDateTime d(2026, 7, 16, 13, 45, 57);
    EXPECT_EQ(57, d.GetSecond());
    EXPECT_EQ(45, d.GetMinute());
    EXPECT_EQ(13, d.GetHour());
    EXPECT_EQ(5, d.GetDayOfWeek());
}

// ── round 5: span arithmetic status arms and inline member sweep ──────────

TEST(CmcDateTimeSpanR5, ArithmeticPropagatesNullAndInvalid)
{
    CmcDateTimeSpan good(1, 2, 3, 4);
    CmcDateTimeSpan nul;   nul.SetStatus(CmcDateTimeSpan::null);
    CmcDateTimeSpan bad;   bad.SetStatus(CmcDateTimeSpan::invalid);

    EXPECT_EQ(CmcDateTimeSpan::null,    (good + nul).GetStatus());
    EXPECT_EQ(CmcDateTimeSpan::null,    (nul  + good).GetStatus());
    EXPECT_EQ(CmcDateTimeSpan::invalid, (good + bad).GetStatus());
    EXPECT_EQ(CmcDateTimeSpan::null,    (good - nul).GetStatus());
    EXPECT_EQ(CmcDateTimeSpan::invalid, (good - bad).GetStatus());

    CmcDateTimeSpan two(2, 0, 0, 0);
    CmcDateTimeSpan sum = good + two;
    EXPECT_EQ(CmcDateTimeSpan::valid, sum.GetStatus());
    EXPECT_EQ(3L, sum.GetDays());
    CmcDateTimeSpan diff = two - good;
    EXPECT_EQ(CmcDateTimeSpan::valid, diff.GetStatus());
    // two(2d) - good(1d 2h 3m 4s = 1.0854630d) = 0.9145370d.  Pin the exact
    // value (not merely < 1.0) so a sign-flipped subtraction is caught.
    EXPECT_NEAR(0.9145370, diff.GetTotalDays(), 1e-6);
}

TEST(CmcDateTimeSpanR5, TotalsAndComparisons)
{
    CmcDateTimeSpan day(1, 0, 0, 0);
    CmcDateTimeSpan more(1, 6, 0, 0);

    EXPECT_EQ(24.0,   day.GetTotalHours());
    EXPECT_EQ(1440.0, day.GetTotalMinutes());
    EXPECT_EQ(1L,     day.GetDays());

    EXPECT_TRUE(day <= more);
    EXPECT_TRUE(day <= day);
    EXPECT_TRUE(more >= day);
    EXPECT_FALSE(day >= more);
}

TEST(CmcDateTimeR5, InequalityAndOrderingOperators)
{
    CmcDateTime a(2024, 6, 15, 10, 0, 0.0);
    CmcDateTime b(2024, 6, 15, 11, 0, 0.0);
    EXPECT_TRUE(a != b);
    EXPECT_FALSE(a != a);
    EXPECT_TRUE(b >= a);
    EXPECT_TRUE(a >= a);
    EXPECT_FALSE(a >= b);
}

TEST(CmcDateTimeR5, DayOfYearAndFormat)
{
    CmcDateTime feb2(2024, 2, 2, 3, 4, 5.0);
    EXPECT_EQ(33, feb2.GetDayOfYear());   // Jan (31) + 2

    CString s = feb2.Format("%Y-%m-%d %H:%M:%S");
    EXPECT_STREQ("2024-02-02 03:04:05", (LPCTSTR)s);
}

TEST(CmcDateTimeR5, InvalidComponentDatesAreRejected)
{
    // year out of range
    CmcDateTime bad1(10000, 1, 1, 0, 0, 0.0);
    EXPECT_EQ(CmcDateTime::invalid, bad1.GetStatus());
    // day out of range for the month
    CmcDateTime bad2(2023, 2, 29, 0, 0, 0.0);
    EXPECT_EQ(CmcDateTime::invalid, bad2.GetStatus());
    // leap-year Feb 29 is fine
    CmcDateTime ok(2024, 2, 29, 0, 0, 0.0);
    EXPECT_EQ(CmcDateTime::valid, ok.GetStatus());
}


TEST(CmcDateTimeR5, CenturyLeapYearRule)
{
    // mcDateFromTm leap rule (mcdatetime.cpp:265-266):
    //   bLeapYear = (wYear&3)==0 && ((wYear%100)!=0 || (wYear%400)==0)
    // 1900: divisible by 100 but NOT by 400 -> NOT a leap year -> Feb 29 rejected.
    CmcDateTime y1900(1900, 2, 29, 0, 0, 0.0);
    EXPECT_EQ(CmcDateTime::invalid, y1900.GetStatus());
    // 2000: divisible by 400 -> the century-leap exception fires -> leap year
    //       -> Feb 29 accepted.
    CmcDateTime y2000(2000, 2, 29, 0, 0, 0.0);
    EXPECT_EQ(CmcDateTime::valid, y2000.GetStatus());
}

TEST(CmcDateTimeR5, PreEpochNegativeSerialSign)
{
    // mcDateFromTm (mcdatetime.cpp:299):
    //   dtDest = (double)nDate + ((nDate >= 0) ? dblTime : -dblTime)
    // The OLE epoch (Dec 30 1899) is serial 0, so Dec 29 1899 has nDate = -1.
    // Because nDate < 0 the 06:00 time-of-day (0.25 day) is SUBTRACTED, giving
    // m_dt = -1.25 (NOT -0.75).
    CmcDateTime before(1899, 12, 29, 6, 0, 0.0);
    EXPECT_EQ(CmcDateTime::valid, before.GetStatus());
    EXPECT_NEAR(-1.25, before.m_dt, 1e-9);
    // The mirror date one day AFTER the epoch takes the additive branch -> +1.25,
    // confirming the sign convention flips exactly at the epoch.
    CmcDateTime after(1899, 12, 31, 6, 0, 0.0);
    EXPECT_NEAR(1.25, after.m_dt, 1e-9);
}


// ── round 6: the hand-written span assignment operators (never exercised) ──
// Prior tests only copy-CONSTRUCT spans; operator=(double) (mcdatetime.cpp:423)
// and operator=(const CmcDateTimeSpan&) (mcdatetime.cpp:430) had zero callers.

TEST(CmcDateTimeSpanR6, AssignFromDoubleSetsSpanAndForcesValidStatus)
{
    // operator=(double) (mcdatetime.cpp:423-428): m_span := the double (in
    // days), status forced to valid.  Pre-poison the status to non-valid so the
    // SetStatus(valid) arm is load-bearing (not merely already-valid).
    CmcDateTimeSpan span;
    span.SetStatus(CmcDateTimeSpan::invalid);
    span = 2.5;                                    // 2.5 days -> operator=(double)
    EXPECT_EQ(CmcDateTimeSpan::valid, span.GetStatus());
    EXPECT_NEAR(2.5, span.GetTotalDays(), 1e-9);          // GetTotalDays == m_span
    EXPECT_NEAR(216000.0, span.GetTotalSeconds(), 1e-6);  // 2.5 * 86400
}

TEST(CmcDateTimeSpanR6, AssignFromSpanCopiesBothSpanAndStatus)
{
    // operator=(const CmcDateTimeSpan&) (mcdatetime.cpp:430-435) copies m_span
    // AND m_status field-by-field (hand-written, NOT defaulted).
    // (A) span field: a distinctive valid source must transfer its value.
    CmcDateTimeSpan src(3, 6, 0, 0);               // 3d 6h == 3.25 days
    CmcDateTimeSpan dst;
    dst = src;                                      // operator=(const&)
    EXPECT_EQ(CmcDateTimeSpan::valid, dst.GetStatus());
    EXPECT_NEAR(3.25, dst.GetTotalDays(), 1e-9);
    EXPECT_NEAR(280800.0, dst.GetTotalSeconds(), 1e-6);   // 3.25 * 86400
    // (B) status field: a NON-valid source must overwrite a valid dest's status
    //     (unlike operator=(double), which always forces valid).
    CmcDateTimeSpan nul;  nul.SetStatus(CmcDateTimeSpan::null);
    CmcDateTimeSpan dst2;                           // default-constructed -> valid
    dst2 = nul;                                     // operator=(const&)
    EXPECT_EQ(CmcDateTimeSpan::null, dst2.GetStatus());
}

MC_TEST_MAIN()
