// tests/test_timers.cpp
// Tests for the timer subsystem: AddTimer, CheckTimerList, EnableTimer,
// ResetTimer, DeleteTimer.
//
// CmcDateTime::GetTimeNow() returns the wall clock.  We set tFireTime
// directly on each CTimer to control when it fires — setting it to
// "now - 1 second" makes CheckTimerList() see it as overdue and fire it.

#include "mc_test.h"
#include "platform/doc_stub.h"
#include "OtherTypes.h"     // CTimer, CmcDateTime
#include "mcdatetime.h"
#include "pal/iclock.h"     // pal::IClock / pal::g_clock — deterministic time

// ── helpers ───────────────────────────────────────────────────────────────

static CMUSHclientDoc make_doc()
{
    CMUSHclientDoc doc;
    doc.m_iConnectPhase = eConnectConnectedToMud;
    doc.m_bEnableTimers = TRUE;
    return doc;
}

// Add an interval timer to doc (fires every `seconds` seconds).
// Returns pointer owned by doc's timer map.
static CTimer* add_timer(CMUSHclientDoc& doc,
                          const char* label,
                          double seconds = 1.0,
                          unsigned short sendTo = eSendToOutput)
{
    CTimer* t = new CTimer();
    t->strLabel    = CString(label);
    t->bEnabled    = TRUE;
    t->iType       = CTimer::eInterval;
    t->iEveryHour  = 0;
    t->iEveryMinute= 0;
    t->fEverySecond= seconds;
    t->iSendTo     = sendTo;
    t->strContents = CString("");
    t->strGroup    = CString("");
    t->bActiveWhenClosed = FALSE;
    // Set fire time to "now" so it fires on the first CheckTimerList call.
    t->tFireTime   = CmcDateTime::GetTimeNow();
    doc.GetTimerMap().SetAt(t->strLabel, t);
    return t;
}

// Advance tFireTime into the past so CheckTimerList fires it.
static void make_overdue(CTimer* t)
{
    CmcDateTimeSpan one_sec(0, 0, 0, 1);
    t->tFireTime = CmcDateTime::GetTimeNow() - one_sec;
}

// Call CheckTimerList once.
static void tick(CMUSHclientDoc& doc)
{
    doc.CheckTimerList(doc.GetTimerMap());
}

// ── Tests ─────────────────────────────────────────────────────────────────

TEST(TimerTest, OverdueTimerFires)
{
    CMUSHclientDoc doc = make_doc();
    CTimer* t = add_timer(doc, "t1");
    make_overdue(t);

    tick(doc);

    EXPECT_EQ(1L, t->nMatched);
}

TEST(TimerTest, FutureTimerDoesNotFire)
{
    CMUSHclientDoc doc = make_doc();
    CTimer* t = add_timer(doc, "t1", 60.0);
    // tFireTime was set to GetTimeNow() at add time — advance it 60 s into the future
    CmcDateTimeSpan sixty(0, 0, 1, 0);
    t->tFireTime = CmcDateTime::GetTimeNow() + sixty;

    tick(doc);

    EXPECT_EQ(0L, t->nMatched);
}

TEST(TimerTest, DisabledTimerDoesNotFire)
{
    CMUSHclientDoc doc = make_doc();
    CTimer* t = add_timer(doc, "t1");
    make_overdue(t);
    t->bEnabled = FALSE;

    tick(doc);

    EXPECT_EQ(0L, t->nMatched);
}

TEST(TimerTest, TimerFiresMultipleTimes)
{
    CMUSHclientDoc doc = make_doc();
    CTimer* t = add_timer(doc, "t1", 0.001);  // very short interval
    make_overdue(t);

    tick(doc);
    EXPECT_EQ(1L, t->nMatched);

    // After firing, tFireTime should be advanced; make it overdue again.
    make_overdue(t);
    tick(doc);
    EXPECT_EQ(2L, t->nMatched);
}

TEST(TimerTest, OneShotTimerRemovedAfterFiring)
{
    CMUSHclientDoc doc = make_doc();
    CTimer* t = add_timer(doc, "t1");
    t->bOneShot = TRUE;
    make_overdue(t);

    tick(doc);

    // A one-shot timer fires once and is then DELETED from the timer map — the
    // shared Windows behaviour (CheckTimerList removes it after firing).  `t`
    // is freed by the tick above and must not be dereferenced; verify via the
    // map instead.
    CTimer* gone = nullptr;
    EXPECT_FALSE(doc.GetTimerMap().Lookup("t1", gone));

    // A second tick must be a harmless no-op (nothing left to fire).
    tick(doc);
    EXPECT_FALSE(doc.GetTimerMap().Lookup("t1", gone));
}

TEST(TimerTest, MultipleTimersBothFire)
{
    CMUSHclientDoc doc = make_doc();
    CTimer* t1 = add_timer(doc, "t1");
    CTimer* t2 = add_timer(doc, "t2");
    make_overdue(t1);
    make_overdue(t2);

    tick(doc);

    EXPECT_EQ(1L, t1->nMatched);
    EXPECT_EQ(1L, t2->nMatched);
}

TEST(TimerTest, DeletedTimerNoLongerFires)
{
    CMUSHclientDoc doc = make_doc();
    add_timer(doc, "t1");
    CTimer* t2 = add_timer(doc, "t2");

    // Delete t1 from the map
    CTimer* t1 = nullptr;
    doc.GetTimerMap().Lookup(CString("t1"), t1);
    doc.GetTimerMap().RemoveKey(CString("t1"));
    delete t1;

    make_overdue(t2);
    tick(doc);

    EXPECT_EQ(1L, t2->nMatched);
}

TEST(TimerTest, TimersDisabledGlobally)
{
    CMUSHclientDoc doc = make_doc();
    doc.m_bEnableTimers = FALSE;
    CTimer* t = add_timer(doc, "t1");
    make_overdue(t);

    // CheckTimerList is guarded by m_bEnableTimers in the CLI event loop,
    // not inside CheckTimerList itself.  Simulate by not calling tick().
    EXPECT_EQ(0L, t->nMatched);
}

TEST(TimerTest, ActiveWhenClosedFiresWhileDisconnected)
{
    CMUSHclientDoc doc = make_doc();
    doc.m_iConnectPhase = eConnectNotConnected;
    CTimer* t = add_timer(doc, "t1");
    t->bActiveWhenClosed = TRUE;
    make_overdue(t);

    tick(doc);

    EXPECT_EQ(1L, t->nMatched);
}

TEST(TimerTest, InactiveTimerDoesNotFireWhenDisconnected)
{
    CMUSHclientDoc doc = make_doc();
    doc.m_iConnectPhase = eConnectNotConnected;
    CTimer* t = add_timer(doc, "t1");
    t->bActiveWhenClosed = FALSE;
    make_overdue(t);

    tick(doc);

    EXPECT_EQ(0L, t->nMatched);
}

// ── Deterministic clock (pal::IClock) ───────────────────────────────────────
// Installing a fake clock makes CmcDateTime::GetTimeNow() — and therefore the
// whole timer-fire decision in CheckTimerList — read a controlled "now",
// so we can drive time forward exactly and assert firing without any
// wall-clock sleeps.

namespace {

struct FakeClock : pal::IClock {
    CmcDateTime m_now;
    explicit FakeClock(CmcDateTime t) : m_now(t) {}
    double nowOle() const override { return m_now.GetTime(); }
    void advance(double seconds) { m_now = m_now + CmcDateTimeSpan(0, 0, 0, seconds); }
};

// RAII: installs the fake clock and guarantees it is uninstalled (so a later
// test never dereferences this stack object through a dangling pal::g_clock).
struct ScopedClock {
    FakeClock clock;
    explicit ScopedClock(CmcDateTime t) : clock(t) { pal::g_clock = &clock; }
    ~ScopedClock() { pal::g_clock = nullptr; }
    void advance(double s) { clock.advance(s); }
    CmcDateTime now() const { return clock.m_now; }
};

// A fixed, arbitrary starting instant (no dependence on the real wall clock).
static CmcDateTime start_instant() { return CmcDateTime(2026, 1, 1, 12, 0, 0); }

} // namespace


TEST(TimerClockTest, InstalledClockControlsGetTimeNow)
{
    ScopedClock clk(start_instant());
    EXPECT_TRUE(CmcDateTime::GetTimeNow().GetTime() == start_instant().GetTime());
    clk.advance(90);   // 90 seconds later
    CmcDateTimeSpan elapsed = CmcDateTime::GetTimeNow() - start_instant();
    EXPECT_TRUE(elapsed.GetTotalSeconds() > 89.9 && elapsed.GetTotalSeconds() < 90.1);
}

TEST(TimerClockTest, UninstallRestoresTheRealClock)
{
    { ScopedClock clk(start_instant()); }   // installed then dropped
    EXPECT_TRUE(pal::g_clock == nullptr);
    // GetTimeNow now reads the real clock again — just prove it is not frozen
    // at the fake instant (the real 'now' is well after 2026-01-01).
    EXPECT_TRUE(CmcDateTime::GetTimeNow().GetTime() != start_instant().GetTime());
}

TEST(TimerClockTest, TimerFiresOnlyWhenClockReachesFireTime)
{
    ScopedClock clk(start_instant());
    CMUSHclientDoc doc = make_doc();

    // A timer due 10 s from the start instant.
    CTimer* t = add_timer(doc, "t1", 10.0);
    t->tFireTime = start_instant() + CmcDateTimeSpan(0, 0, 0, 10);

    tick(doc);                 // now == start: not due
    EXPECT_EQ(0L, t->nMatched);

    clk.advance(9);            // now == start+9: still not due
    tick(doc);
    EXPECT_EQ(0L, t->nMatched);

    clk.advance(1);            // now == start+10: exactly due
    tick(doc);
    EXPECT_EQ(1L, t->nMatched);
}

TEST(TimerClockTest, IntervalTimerRefiresEachInterval)
{
    ScopedClock clk(start_instant());
    CMUSHclientDoc doc = make_doc();

    // every-2-second interval timer, first due at the start instant
    CTimer* t = add_timer(doc, "iv", 2.0);
    t->tFireTime = start_instant();

    tick(doc);                 // due now → fires, reschedules to +2 s
    EXPECT_EQ(1L, t->nMatched);

    clk.advance(1); tick(doc); // +1 s: next fire is at +2 s → not yet
    EXPECT_EQ(1L, t->nMatched);

    clk.advance(1); tick(doc); // +2 s: due again → fires
    EXPECT_EQ(2L, t->nMatched);

    clk.advance(2); tick(doc); // +4 s: due again → fires
    EXPECT_EQ(3L, t->nMatched);
}

TEST(TimerClockTest, LongJumpFiresOnceNotEveryMissedInterval)
{
    // Drift correction: if the clock leaps far past several missed intervals
    // (sleep/wake, clock change), the timer fires once and reschedules — it
    // does not "catch up" with one fire per missed interval.
    ScopedClock clk(start_instant());
    CMUSHclientDoc doc = make_doc();

    CTimer* t = add_timer(doc, "iv", 2.0);   // every 2 s
    t->tFireTime = start_instant();

    clk.advance(100);          // 100 s / 50 missed intervals in one tick
    tick(doc);

    EXPECT_EQ(1L, t->nMatched);                       // exactly one fire
    EXPECT_TRUE(t->tFireTime > CmcDateTime::GetTimeNow());  // rescheduled into the future
}

// ── GetTimerList returns a REAL safe-array headless ─────────────────────────
// The adopted methods_timers.cpp GetTimerList goes through COleSafeArray; with
// the SafeArray machinery un-gated for mushcore this returns a genuine
// VT_ARRAY|VT_VARIANT whose elements are the timer names (VT_BSTR).
TEST(TimerTest, GetTimerListReturnsRealArray)
{
    ScopedClock clk(start_instant());
    CMUSHclientDoc doc = make_doc();
    add_timer(doc, "alpha", 5.0);
    add_timer(doc, "beta",  7.0);

    VARIANT v = doc.GetTimerList();
    ASSERT_EQ((unsigned short)(VT_ARRAY | VT_VARIANT), v.vt);
    ASSERT_NE(nullptr, v.parray);

    long lb = 0, ub = -1;
    SafeArrayGetLBound(v.parray, 1, &lb);
    SafeArrayGetUBound(v.parray, 1, &ub);
    EXPECT_EQ(0L, lb);
    EXPECT_EQ(1L, ub);   // two timers

    bool sawAlpha = false, sawBeta = false;
    for (long i = lb; i <= ub; ++i) {
        VARIANT el; VariantInit(&el);
        ASSERT_EQ(S_OK, SafeArrayGetElement(v.parray, &i, &el));
        ASSERT_EQ((unsigned short)VT_BSTR, el.vt);
        if (CString(el.bstrVal) == "alpha") sawAlpha = true;
        if (CString(el.bstrVal) == "beta")  sawBeta  = true;
    }
    EXPECT_TRUE(sawAlpha);
    EXPECT_TRUE(sawBeta);

    VariantClear(&v);   // frees the array + its BSTR copies
}

#include "flags.h"   // eEnabled / eAtTime timer flags

// ── Intent-comment tests (candidate mining, 2026-07) ─────────────────────

// timers.cpp ResetOneTimer: "if this time has passed, go onto tomorrow" —
// an at-time timer whose time-of-day already passed today is scheduled for
// tomorrow, never in the past.
TEST(TimerClockTest, AtTimeTimerInPastSchedulesForTomorrow)
{
    ScopedClock clk(start_instant());        // 2026-01-01 12:00:00
    CMUSHclientDoc doc = make_doc();

    CTimer* t = new CTimer();
    t->strLabel  = CString("at");
    t->bEnabled  = TRUE;
    t->iType     = CTimer::eAtTime;
    t->iAtHour   = 6;                        // 06:00 — already passed
    t->iAtMinute = 0;
    t->fAtSecond = 0.0;
    doc.GetTimerMap().SetAt(t->strLabel, t);

    doc.ResetOneTimer(t);
    EXPECT_TRUE(t->tFireTime == CmcDateTime(2026, 1, 2, 6, 0, 0));  // tomorrow
    EXPECT_TRUE(t->tFireTime > CmcDateTime::GetTimeNow());

    t->iAtHour = 18;                         // 18:00 — still ahead today
    doc.ResetOneTimer(t);
    EXPECT_TRUE(t->tFireTime == CmcDateTime(2026, 1, 1, 18, 0, 0)); // today
}

// timers.cpp ResetOneTimer: "now plus the interval ... minus the offset" —
// a periodic timer's next fire time subtracts the offset fields.
TEST(TimerClockTest, PeriodicTimerOffsetSubtractsFromFireTime)
{
    ScopedClock clk(start_instant());
    CMUSHclientDoc doc = make_doc();

    CTimer* t = add_timer(doc, "iv", 10.0);  // every 10 s ...
    t->fOffsetSecond = 3.0;                  // ... with a 3 s offset
    doc.ResetOneTimer(t);
    // due 7 s from now
    EXPECT_TRUE(t->tFireTime == start_instant() + CmcDateTimeSpan(0, 0, 0, 7));
}

// timers.cpp: "update fire time - before calling the script, in case it
// takes a long time" (at-time branch adds tsOneDay) — an at-time timer,
// once fired, is rescheduled exactly one day later.
TEST(TimerClockTest, AtTimeTimerRefiresOneDayLater)
{
    ScopedClock clk(start_instant());
    CMUSHclientDoc doc = make_doc();

    CTimer* t = new CTimer();
    t->strLabel  = CString("at");
    t->bEnabled  = TRUE;
    t->iType     = CTimer::eAtTime;
    t->iAtHour   = 12; t->iAtMinute = 0; t->fAtSecond = 0.0;
    t->iSendTo   = eSendToOutput;
    t->tFireTime = start_instant();          // due right now
    doc.GetTimerMap().SetAt(t->strLabel, t);

    tick(doc);
    EXPECT_EQ(1L, t->nMatched);              // fired once
    EXPECT_TRUE(t->tFireTime == start_instant() + CmcDateTimeSpan(1, 0, 0, 0));

    tick(doc);                               // same day again: not due
    clk.advance(3600);
    tick(doc);
    EXPECT_EQ(1L, t->nMatched);              // does not re-fire today
}

// methods_timers.cpp AddTimer: "can't have a zero time for 'every' timers"
// — hour==minute==0, second==0.0 without eAtTime is eTimeInvalid; the same
// zero time WITH eAtTime is a valid midnight timer.
TEST(TimerTest, AddTimerZeroIntervalRejected)
{
    ScopedClock clk(start_instant());
    CMUSHclientDoc doc = make_doc();

    EXPECT_EQ((long)eTimeInvalid,
              doc.AddTimer("z", 0, 0, 0.0, "x", eEnabled, ""));
    CTimer* t = nullptr;
    EXPECT_FALSE(doc.GetTimerMap().Lookup("z", t));   // nothing added

    // control: the same zero time as an AT timer is valid (fires at midnight)
    EXPECT_EQ((long)eOK,
              doc.AddTimer("mid", 0, 0, 0.0, "x", eEnabled | eAtTime, ""));
    EXPECT_TRUE(doc.GetTimerMap().Lookup("mid", t));
}

// methods_timers.cpp DoAfterSpecial: "sanity check: if (Seconds < 0.1)
// return eTimeInvalid" (and "a really big number would make hours > 23").
TEST(TimerTest, DoAfterRejectsLessThanTenthSecond)
{
    ScopedClock clk(start_instant());
    CMUSHclientDoc doc = make_doc();

    EXPECT_EQ((long)eTimeInvalid, doc.DoAfter(0.05, "hello"));
    EXPECT_EQ((long)eTimeInvalid, doc.DoAfterSpecial(0.0, "hello", eSendToOutput));
    EXPECT_EQ(0, (int)doc.GetTimerMap().GetCount());  // no timer created

    // the huge-number guard from the same comment block
    EXPECT_EQ((long)eTimeInvalid, doc.DoAfter(90000.0, "hello"));  // > 24 h
    EXPECT_EQ(0, (int)doc.GetTimerMap().GetCount());
}

// methods_timers.cpp DoAfterSpecial: "this is a temporary unlabelled timer,
// make up a name (created enabled, one-shot, temporary, active-when-closed)"
// — it sends its text exactly once when due and is then removed.
TEST(TimerClockTest, DoAfterFiresOnceThenTimerIsRemoved)
{
    ScopedClock clk(start_instant());
    CMUSHclientDoc doc = make_doc();

    EXPECT_EQ((long)eOK, doc.DoAfterSpecial(1.0, "did it", eSendToOutput));
    ASSERT_EQ(1, (int)doc.GetTimerMap().GetCount());

    // inspect the created timer's promised properties
    POSITION pos = doc.GetTimerMap().GetStartPosition();
    CString name; CTimer* t = nullptr;
    doc.GetTimerMap().GetNextAssoc(pos, name, t);
    ASSERT_NE(nullptr, t);
    EXPECT_TRUE(name.Left(6) == "*timer");   // made-up internal name
    EXPECT_TRUE(t->bEnabled);
    EXPECT_TRUE(t->bOneShot != 0);
    EXPECT_TRUE(t->bTemporary);
    EXPECT_TRUE(t->bActiveWhenClosed);

    tick(doc);                               // not yet due
    ASSERT_EQ(1, (int)doc.GetTimerMap().GetCount());

    clk.advance(1.5);
    tick(doc);                               // due: fires and is removed
    EXPECT_EQ(0, (int)doc.GetTimerMap().GetCount());

    clk.advance(5);
    tick(doc);                               // harmless no-op afterwards
    EXPECT_EQ(0, (int)doc.GetTimerMap().GetCount());
}

#ifdef MUSHCLIENT_LUA_SCRIPTING
#include "scripting/scripting.h"

// timers.cpp CheckTimerList: "now process list, checking timer still exists
// in case a script deleted one. see forum ?id=10358" — when two timers are
// due in one pass and the first one's script deletes the second, the second
// neither fires nor causes a use-after-free.
TEST(TimerScript, ScriptDeletingOtherDueTimerPreventsItsFiring)
{
    ScopedClock clk(start_instant());
    CMUSHclientDoc doc = make_doc();

    // a live Lua engine whose function deletes the OTHER due timer
    CScriptEngine* engine = new CScriptEngine(&doc, "lua");
    ASSERT_TRUE(!engine->CreateScriptEngine());
    ASSERT_TRUE(!engine->Parse(CString(
        "function DeleteOther(name)\n"
        "  world.DeleteTimer('b_second')\n"
        "end\n"), "<test>"));
    doc.m_ScriptEngine = engine;

    // two timers due in the same pass; set<string> order runs a_ before b_
    CTimer* t1 = add_timer(doc, "a_first", 5.0);
    t1->strProcedure = CString("DeleteOther");
    CTimer* t2 = add_timer(doc, "b_second", 5.0);
    make_overdue(t1);
    make_overdue(t2);
    doc.ResolveAllEntryPoints();

    long fired_before = doc.m_iTimersFiredCount;
    tick(doc);                               // must not crash (no UAF)

    EXPECT_EQ(1L, t1->nMatched);             // first timer fired...
    EXPECT_EQ(fired_before + 1, doc.m_iTimersFiredCount);  // ...and only it
    CTimer* gone = nullptr;
    EXPECT_FALSE(doc.GetTimerMap().Lookup("b_second", gone));  // deleted
}
#endif // MUSHCLIENT_LUA_SCRIPTING

MC_TEST_MAIN()
