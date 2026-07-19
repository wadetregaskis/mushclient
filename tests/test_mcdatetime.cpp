// tests/test_mcdatetime.cpp
// Unit tests for CmcDateTime / CmcDateTimeSpan.

#include "mc_test.h"
#include "platform/compat.h"
#include "mcdatetime.h"

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

TEST(CmcDateTimeTest, GetTimeNowReturnsNonZero) {
    CmcDateTime now = CmcDateTime::GetTimeNow();
    EXPECT_EQ(CmcDateTime::valid, now.GetStatus());
    EXPECT_NE(0.0, now.m_dt);
}

TEST(CmcDateTimeTest, CopyConstruct) {
    CmcDateTime a(2024, 1, 1, 0, 0, 0.0);
    CmcDateTime b(a);
    EXPECT_EQ(a.m_dt, b.m_dt);
    EXPECT_EQ(a.GetStatus(), b.GetStatus());
}

// ── CmcDateTimeSpan ───────────────────────────────────────────────────────

TEST(CmcDateTimeSpanTest, SpanInSeconds) {
    CmcDateTimeSpan span;
    span.SetDateTimeSpan(0, 0, 0, 60.0);
    EXPECT_NEAR(60.0, span.GetTotalSeconds(), 0.001);
}

TEST(CmcDateTimeSpanTest, SpanInMinutes) {
    CmcDateTimeSpan span;
    span.SetDateTimeSpan(0, 0, 5, 0.0);
    EXPECT_NEAR(300.0, span.GetTotalSeconds(), 0.001);
}

TEST(CmcDateTimeSpanTest, SpanInHours) {
    CmcDateTimeSpan span;
    span.SetDateTimeSpan(0, 2, 0, 0.0);
    EXPECT_NEAR(7200.0, span.GetTotalSeconds(), 0.001);
}

TEST(CmcDateTimeSpanTest, SpanInDays) {
    CmcDateTimeSpan span;
    span.SetDateTimeSpan(1, 0, 0, 0.0);
    EXPECT_NEAR(86400.0, span.GetTotalSeconds(), 0.001);
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

MC_TEST_MAIN()
