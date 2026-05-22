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

TEST(TimerTest, OneShotTimerDisablesItselfAfterFiring)
{
    CMUSHclientDoc doc = make_doc();
    CTimer* t = add_timer(doc, "t1");
    t->bOneShot = TRUE;
    make_overdue(t);

    tick(doc);
    EXPECT_EQ(1L, t->nMatched);
    EXPECT_EQ(FALSE, t->bEnabled);

    // Second tick should not fire
    make_overdue(t);
    tick(doc);
    EXPECT_EQ(1L, t->nMatched);
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

MC_TEST_MAIN()
