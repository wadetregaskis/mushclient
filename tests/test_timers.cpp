// tests/test_timers.cpp
// Tests for the timer subsystem: AddTimer, CheckTimerList, EnableTimer,
// ResetTimer, DeleteTimer.
//
// CmcDateTime::GetTimeNow() returns the wall clock.  We set tFireTime
// directly on each CTimer to control when it fires — setting it to
// "now - 1 second" makes CheckTimerList() see it as overdue and fire it.

#include "mc_test.h"
// B-3 atomic flip: real headers (the headless stub is gone; tests are FULL_BUILD).
#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "scripting/errors.h"
#include "OtherTypes.h"     // CTimer, CmcDateTime
#include "mcdatetime.h"
#include "pal/iclock.h"     // pal::IClock / pal::g_clock — deterministic time
#include "platform/wx_mfc_hooks.h"  // g_wxhook_set_status — reads the reconnect's status line

#include <string>

// ── helpers ───────────────────────────────────────────────────────────────

static void make_doc(CMUSHclientDoc& doc)
{
    doc.m_iConnectPhase = eConnectConnectedToMud;
    doc.m_bEnableTimers = TRUE;
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

// OverdueTimerFires deleted (audit §3): the deterministic-clock
// TimerClockTest.TimerFiresOnlyWhenClockReachesFireTime dominates it — it verifies
// the exact fire boundary (not-due at start/+9 s, due at +10 s) rather than a single
// wall-clock make_overdue()+fire.

TEST(TimerTest, FutureTimerDoesNotFire)
{
    CMUSHclientDoc doc; make_doc(doc);
    CTimer* t = add_timer(doc, "t1", 60.0);
    // tFireTime was set to GetTimeNow() at add time — advance it 60 s into the future
    CmcDateTimeSpan sixty(0, 0, 1, 0);
    t->tFireTime = CmcDateTime::GetTimeNow() + sixty;

    tick(doc);

    EXPECT_EQ(0L, t->nMatched);
}

TEST(TimerTest, DisabledTimerDoesNotFire)
{
    CMUSHclientDoc doc; make_doc(doc);
    CTimer* t = add_timer(doc, "t1");
    make_overdue(t);
    t->bEnabled = FALSE;

    tick(doc);

    EXPECT_EQ(0L, t->nMatched);
}

// TimerFiresMultipleTimes deleted (audit §3): the deterministic-clock
// TimerClockTest.IntervalTimerRefiresEachInterval dominates it — it verifies the
// correct reschedule (fires at +0/+2/+4 s, not at the intermediate +1/+3 s) rather
// than a make_overdue()-forced refire.

TEST(TimerTest, OneShotTimerRemovedAfterFiring)
{
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);
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
    // The global m_bEnableTimers guard lives in CheckTimers() (timers.cpp:336),
    // NOT in CheckTimerList — so drive CheckTimers() directly and prove the
    // guard actually gates firing.  make_doc leaves us connected with no
    // plugins and no log file, so CheckTimers reduces to the guard + the
    // world-scope CheckTimerList.
    CMUSHclientDoc doc; make_doc(doc);
    CTimer* t = add_timer(doc, "t1");
    make_overdue(t);

    doc.m_bEnableTimers = FALSE;
    doc.CheckTimers();
    EXPECT_EQ(0L, t->nMatched);   // disabled → overdue timer must NOT fire

    doc.m_bEnableTimers = TRUE;
    doc.CheckTimers();
    EXPECT_EQ(1L, t->nMatched);   // enabled → the same overdue timer fires
}

// ── the automatic reconnect ───────────────────────────────────────────────
// CheckTimers is also where "Reconnect on link failure" lives (timers.cpp:328),
// in COMMON code, and it is the only reconnect either platform has: the port's
// world timer calls CheckTimers every 100 ms by default, so nothing slower could
// ever get there first.  All three of its conditions were untested, and the
// first one -- the preference, which ships OFF -- short-circuited the block on
// its opening line in every test run there has ever been.
//
// Observed through g_wxhook_set_status, the status line the block writes
// immediately before ConnectSocket().  m_server is "0.0.0.0", which
// ConnectSocket refuses on its first line (doc_shared.cpp:1258), so the attempt
// is recorded without resolving a name or opening a socket.
static std::string g_lastStatus;
static void capture_status(const char* msg) { g_lastStatus = msg ? msg : ""; }

TEST(TimerTest, LinkFailureReconnectsOnlyWhenAllThreeConditionsHold)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_server        = "0.0.0.0";
    doc.m_iConnectPhase = eConnectNotConnected;
    doc.m_bDisconnectOK = false;          // the link DROPPED; nobody asked for it

    const unsigned savedPref = App.m_bReconnectOnLinkFailure;
    void (*savedHook)(const char*) = g_wxhook_set_status;
    g_wxhook_set_status = capture_status;

    // The shipped default: the preference is off, so a dropped link stays down.
    App.m_bReconnectOnLinkFailure = 0;
    g_lastStatus.clear();
    doc.CheckTimers();
    EXPECT_TRUE(g_lastStatus.empty());

    // Turn it on and the very same state reconnects.
    App.m_bReconnectOnLinkFailure = 1;
    g_lastStatus.clear();
    doc.CheckTimers();
    EXPECT_STREQ("Reconnecting ...", g_lastStatus.c_str());

    // A disconnect the user asked for is not a link failure.
    doc.m_bDisconnectOK = true;
    g_lastStatus.clear();
    doc.CheckTimers();
    EXPECT_TRUE(g_lastStatus.empty());

    // Neither is a world that is still connected.
    doc.m_bDisconnectOK = false;
    doc.m_iConnectPhase = eConnectConnectedToMud;
    g_lastStatus.clear();
    doc.CheckTimers();
    EXPECT_TRUE(g_lastStatus.empty());

    g_wxhook_set_status           = savedHook;
    App.m_bReconnectOnLinkFailure = savedPref;
}

TEST(TimerTest, ActiveWhenClosedFiresWhileDisconnected)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_iConnectPhase = eConnectNotConnected;
    CTimer* t = add_timer(doc, "t1");
    t->bActiveWhenClosed = TRUE;
    make_overdue(t);

    tick(doc);

    EXPECT_EQ(1L, t->nMatched);
}

TEST(TimerTest, InactiveTimerDoesNotFireWhenDisconnected)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.m_iConnectPhase = eConnectNotConnected;
    CTimer* t = add_timer(doc, "t1");
    t->bActiveWhenClosed = FALSE;
    make_overdue(t);

    tick(doc);

    EXPECT_EQ(0L, t->nMatched);
}

// Regression (upstream bug): SetTimerOption("at_time") toggled a timer between
// interval and at-time modes but copied the HOUR into the MINUTE field
// (`iAtMinute = iEveryHour`, `iEveryMinute = iAtHour`).  Flipping an
// "every 2h 30m 15s" timer to at-time must keep at-minute 30, not 2.  The
// existing round-trip test missed this because it read "minute" BEFORE setting
// at_time; this reads it AFTER the toggle.
TEST(TimerTest, AtTimeToggleKeepsMinuteField)
{
    const long eEnabled = 0x01;   // flags.h AddTimer flag (flags.h has no guard)
    CMUSHclientDoc doc;
    ASSERT_EQ((long)eOK, doc.AddTimer("t1", 2, 30, 15.0, "hi", eEnabled, ""));
    ASSERT_EQ((long)eOK, doc.SetTimerOption("t1", "at_time", "1"));  // interval → at-time

    // GetTimerOption marshals hour/minute/second as VT_R8 (double).
    VARIANT vh = doc.GetTimerOption("t1", "hour");
    VARIANT vm = doc.GetTimerOption("t1", "minute");
    ASSERT_EQ((unsigned short)VT_R8, vh.vt);
    ASSERT_EQ((unsigned short)VT_R8, vm.vt);
    EXPECT_EQ(2.0,  vh.dblVal);
    EXPECT_EQ(30.0, vm.dblVal);   // was 2 with the HOUR→MINUTE copy bug
    VariantClear(&vh);
    VariantClear(&vm);
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
    CMUSHclientDoc doc; make_doc(doc);

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
    CMUSHclientDoc doc; make_doc(doc);

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
    CMUSHclientDoc doc; make_doc(doc);

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
    CMUSHclientDoc doc; make_doc(doc);
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
    CMUSHclientDoc doc; make_doc(doc);

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
    CMUSHclientDoc doc; make_doc(doc);

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
    CMUSHclientDoc doc; make_doc(doc);

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
    CMUSHclientDoc doc; make_doc(doc);

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
    CMUSHclientDoc doc; make_doc(doc);

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
    CMUSHclientDoc doc; make_doc(doc);

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
    CMUSHclientDoc doc; make_doc(doc);

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

// ═══ world.* timer API (methods_timers.cpp) ═══════════════════════════════

// (the eEnabled / eAtTime / eOneShot flag enums come via doc.h)
#include <string>

// vs(const VARIANT&) is shared with test_triggers/test_aliases (audit §2/§4).
#include "variant_test_helpers.h"

// AddTimer validation: out-of-range time parts, the zero-interval rule and
// duplicate labels each answer their own error code.
TEST(TimerApi, AddTimerValidatesTime)
{
    CMUSHclientDoc doc; make_doc(doc);

    EXPECT_EQ((long)eOK, doc.AddTimer("t1", 0, 1, 0, "hello", eEnabled, ""));
    EXPECT_EQ((long)eOK, doc.IsTimer("t1"));

    EXPECT_EQ((long)eTimerAlreadyExists,
              doc.AddTimer("t1", 0, 2, 0, "x", eEnabled, ""));
    EXPECT_EQ((long)eOK,
              doc.AddTimer("t1", 0, 2, 0, "x", eEnabled | eReplace, ""));

    EXPECT_EQ((long)eTimeInvalid, doc.AddTimer("h", 24, 0, 0,  "", eEnabled, ""));
    EXPECT_EQ((long)eTimeInvalid, doc.AddTimer("m", 0, 60, 0,  "", eEnabled, ""));
    EXPECT_EQ((long)eTimeInvalid, doc.AddTimer("s", 0, 0, 60.0, "", eEnabled, ""));
    EXPECT_EQ((long)eTimeInvalid, doc.AddTimer("n", 0, 0, -1.0, "", eEnabled, ""));
    // an "every" timer may not be every-zero...
    EXPECT_EQ((long)eTimeInvalid, doc.AddTimer("z", 0, 0, 0, "", eEnabled, ""));
    // ...but AT midnight is a perfectly good moment
    EXPECT_EQ((long)eOK, doc.AddTimer("at0", 0, 0, 0, "", eEnabled | eAtTime, ""));

    EXPECT_EQ((long)eInvalidObjectLabel,
              doc.AddTimer("8bad", 0, 1, 0, "", eEnabled, ""));
}

// GetTimer hands the schedule back; at-time timers answer their parts too.
TEST(TimerApi, GetTimerRoundTripAndList)
{
    CMUSHclientDoc doc; make_doc(doc);
    ASSERT_EQ((long)eOK, doc.AddTimer("rt", 1, 2, 3.5, "the send",
                                      eEnabled, ""));

    VARIANT h, m, s, r, f, sc;
    VariantInit(&h); VariantInit(&m); VariantInit(&s);
    VariantInit(&r); VariantInit(&f); VariantInit(&sc);
    EXPECT_EQ((long)eOK, doc.GetTimer("rt", &h, &m, &s, &r, &f, &sc));
    EXPECT_EQ((short)1, h.iVal);
    EXPECT_EQ((short)2, m.iVal);
    EXPECT_NEAR(3.5, s.dblVal, 0.0001);
    EXPECT_STREQ("the send", vs(r).c_str());
    EXPECT_TRUE((f.lVal & eEnabled) != 0);
    EXPECT_EQ((long)eTimerNotFound, doc.GetTimer("no", &h, &m, &s, &r, &f, &sc));

    VARIANT v = doc.GetTimerList();
    ASSERT_EQ((int)(VT_ARRAY | VT_VARIANT), (int)v.vt);
    COleSafeArray sa;
    sa.Attach(v);
    EXPECT_EQ(1L, sa.GetOneDimSize());
    long i0 = 0;
    VARIANT e; VariantInit(&e);
    sa.GetElement(&i0, &e);
    EXPECT_STREQ("rt", vs(e).c_str());
}

// Timer options: y/n booleans, numeric range validation, group as alpha.
TEST(TimerApi, TimerOptionRoundTripAndErrors)
{
    CMUSHclientDoc doc; make_doc(doc);
    ASSERT_EQ((long)eOK, doc.AddTimer("to", 0, 5, 0, "", eEnabled, ""));

    EXPECT_EQ((long)eTimerNotFound, doc.SetTimerOption("zz", "enabled", "n"));
    EXPECT_EQ((long)eUnknownOption, doc.SetTimerOption("to", "no_such", "1"));

    // numeric timer options come back as VT_R8 doubles (seconds can be
    // fractional, so the whole numeric family answers doubles)
    EXPECT_EQ((long)eOK, doc.SetTimerOption("to", "enabled", "n"));
    EXPECT_NEAR(0.0, doc.GetTimerOption("to", "enabled").dblVal, 0.0001);
    EXPECT_EQ((long)eOK, doc.SetTimerOption("to", "enabled", "y"));
    EXPECT_NEAR(1.0, doc.GetTimerOption("to", "enabled").dblVal, 0.0001);

    EXPECT_EQ((long)eOK, doc.SetTimerOption("to", "minute", "30"));
    EXPECT_NEAR(30.0, doc.GetTimerOption("to", "minute").dblVal, 0.0001);
    EXPECT_EQ((long)eOptionOutOfRange, doc.SetTimerOption("to", "minute", "99"));
    EXPECT_EQ((long)eOptionOutOfRange, doc.SetTimerOption("to", "minute", "abc"));

    EXPECT_EQ((long)eOK, doc.SetTimerOption("to", "group", "grp"));
    EXPECT_STREQ("grp", vs(doc.GetTimerOption("to", "group")).c_str());
}

// The DoAfter family plants temporary one-shot timers with the right
// destination; sub-0.1-second and >24h requests are refused.
TEST(TimerApi, DoAfterFamilyPlantsTemporaryOneShots)
{
    CMUSHclientDoc doc; make_doc(doc);
    EXPECT_EQ(0, (int)doc.GetTimerMap().GetCount());

    EXPECT_EQ((long)eOK, doc.DoAfter(5, "go north"));
    EXPECT_EQ((long)eOK, doc.DoAfterNote(6, "a note"));
    EXPECT_EQ((long)eOK, doc.DoAfterSpeedWalk(7, "4n"));
    EXPECT_EQ(3, (int)doc.GetTimerMap().GetCount());

    int oneshot = 0, temp = 0, toWorld = 0, toOutput = 0, toSpeedwalk = 0;
    for (POSITION pos = doc.GetTimerMap().GetStartPosition(); pos; ) {
        CString label;
        CTimer* t = nullptr;
        doc.GetTimerMap().GetNextAssoc(pos, label, t);
        if (t->bOneShot) oneshot++;
        if (t->bTemporary) temp++;
        if (t->iSendTo == eSendToWorld) toWorld++;
        if (t->iSendTo == eSendToOutput) toOutput++;
        if (t->iSendTo == eSendToSpeedwalk) toSpeedwalk++;
    }
    EXPECT_EQ(3, oneshot);
    EXPECT_EQ(3, temp);
    EXPECT_EQ(1, toWorld);      // DoAfter
    EXPECT_EQ(1, toOutput);     // DoAfterNote
    EXPECT_EQ(1, toSpeedwalk);  // DoAfterSpeedWalk

    EXPECT_EQ((long)eTimeInvalid,     doc.DoAfter(0.05, "x"));      // < 0.1s
    EXPECT_EQ((long)eTimeInvalid,     doc.DoAfter(86400.0, "x"));   // >= 24h
    EXPECT_EQ((long)eOptionOutOfRange, doc.DoAfterSpecial(1, "x", 99));

    EXPECT_EQ(3L, doc.DeleteTemporaryTimers());
    EXPECT_EQ(0, (int)doc.GetTimerMap().GetCount());
}

// ResetTimer pushes the next fire time forward from now; a missing name
// answers eTimerNotFound.
TEST(TimerApi, ResetTimerRecomputesFireTime)
{
    CMUSHclientDoc doc; make_doc(doc);
    ASSERT_EQ((long)eOK, doc.AddTimer("rt", 0, 0, 30, "", eEnabled, ""));
    CTimer* t = nullptr;
    ASSERT_TRUE(doc.GetTimerMap().Lookup(CString("rt"), t));

    make_overdue(t);
    CmcDateTime overdue = t->tFireTime;
    EXPECT_EQ((long)eOK, doc.ResetTimer("rt"));
    EXPECT_TRUE(t->tFireTime > overdue);                    // moved forward
    EXPECT_TRUE(t->tFireTime > CmcDateTime::GetTimeNow());  // into the future

    EXPECT_EQ((long)eTimerNotFound, doc.ResetTimer("missing"));

    // ResetTimers() sweeps every timer the same way
    make_overdue(t);
    doc.ResetTimers();
    EXPECT_TRUE(t->tFireTime > CmcDateTime::GetTimeNow());
}

// Group enable/delete counts only members; EnableTimer toggles one.
TEST(TimerApi, GroupsAndEnableToggle)
{
    CMUSHclientDoc doc; make_doc(doc);
    doc.AddTimer("g1", 0, 1, 0, "", eEnabled, "");
    doc.AddTimer("g2", 0, 1, 0, "", eEnabled, "");
    doc.AddTimer("solo", 0, 1, 0, "", eEnabled, "");
    doc.SetTimerOption("g1", "group", "grp");
    doc.SetTimerOption("g2", "group", "grp");

    EXPECT_EQ(2L, doc.EnableTimerGroup("grp", FALSE));
    EXPECT_NEAR(0.0, doc.GetTimerOption("g1", "enabled").dblVal, 0.0001);
    EXPECT_NEAR(1.0, doc.GetTimerOption("solo", "enabled").dblVal, 0.0001);
    EXPECT_EQ(0L, doc.EnableTimerGroup("nogroup", TRUE));

    EXPECT_EQ(2L, doc.DeleteTimerGroup("grp"));
    EXPECT_EQ((long)eTimerNotFound, doc.IsTimer("g1"));
    EXPECT_EQ((long)eOK, doc.IsTimer("solo"));

    EXPECT_EQ((long)eOK, doc.EnableTimer("solo", FALSE));
    EXPECT_NEAR(0.0, doc.GetTimerOption("solo", "enabled").dblVal, 0.0001);
    EXPECT_EQ((long)eTimerNotFound, doc.EnableTimer("nope", TRUE));

    EXPECT_EQ((long)eOK, doc.DeleteTimer("solo"));
    EXPECT_EQ((long)eTimerNotFound, doc.DeleteTimer("solo"));
}

// GetTimerInfo conventions: missing timer VT_EMPTY, unknown code VT_NULL.
TEST(TimerApi, GetTimerInfoConventions)
{
    CMUSHclientDoc doc; make_doc(doc);
    ASSERT_EQ((long)eOK, doc.AddTimer("ti", 0, 0, 15, "send me",
                                      eEnabled | eTemporary, ""));

    EXPECT_EQ(VT_EMPTY, (int)doc.GetTimerInfo("missing", 1).vt);
    EXPECT_EQ(VT_NULL,  (int)doc.GetTimerInfo("ti", 999).vt);
    // arm 4 is the response text on every MUSHclient version
    EXPECT_STREQ("send me", vs(doc.GetTimerInfo("ti", 4)).c_str());
}


// DeleteTimer refuses to delete a timer that is mid-callback: the guard
// `if (timer_item->bExecutingScript) return eItemInUse;` (methods_timers.cpp
// ~255).  bExecutingScript is a plain CTimer bool, so this is reachable
// headless without a live engine — the ALIAS twin (AliasScript.
// DeleteAliasFromOwnScriptReturnsItemInUse) already covers the alias path.
TEST(TimerApi, DeleteTimerExecutingScriptReturnsItemInUse)
{
    CMUSHclientDoc doc; make_doc(doc);
    ASSERT_EQ((long)eOK, doc.AddTimer("busy", 0, 1, 0, "hi", eEnabled, ""));
    CTimer* t = nullptr;
    ASSERT_TRUE(doc.GetTimerMap().Lookup(CString("busy"), t));

    t->bExecutingScript = true;                   // pretend we're inside its script
    EXPECT_EQ((long)eItemInUse, doc.DeleteTimer("busy"));
    EXPECT_EQ((long)eOK, doc.IsTimer("busy"));    // still present — NOT deleted

    t->bExecutingScript = false;                  // control: now it deletes cleanly
    EXPECT_EQ((long)eOK, doc.DeleteTimer("busy"));
    EXPECT_EQ((long)eTimerNotFound, doc.IsTimer("busy"));
}

// AddTimer maps the eTimerSpeedWalk / eTimerNote flags onto iSendTo
// (methods_timers.cpp ~231-234): eTimerSpeedWalk -> eSendToSpeedwalk,
// eTimerNote -> eSendToOutput.  With neither flag, iSendTo keeps the CTimer
// ctor default eSendToWorld (OtherTypes.h:646) — so all three are distinct.
TEST(TimerApi, AddTimerFlagsMapToSendTo)
{
    CMUSHclientDoc doc; make_doc(doc);

    ASSERT_EQ((long)eOK,
              doc.AddTimer("sw", 0, 1, 0, "4n", eEnabled | eTimerSpeedWalk, ""));
    ASSERT_EQ((long)eOK,
              doc.AddTimer("nt", 0, 1, 0, "tick", eEnabled | eTimerNote, ""));
    ASSERT_EQ((long)eOK,
              doc.AddTimer("pl", 0, 1, 0, "go", eEnabled, ""));   // no send flag

    CTimer *tsw = nullptr, *tnt = nullptr, *tpl = nullptr;
    ASSERT_TRUE(doc.GetTimerMap().Lookup(CString("sw"), tsw));
    ASSERT_TRUE(doc.GetTimerMap().Lookup(CString("nt"), tnt));
    ASSERT_TRUE(doc.GetTimerMap().Lookup(CString("pl"), tpl));

    EXPECT_EQ((unsigned short)eSendToSpeedwalk, tsw->iSendTo);
    EXPECT_EQ((unsigned short)eSendToOutput,    tnt->iSendTo);
    EXPECT_EQ((unsigned short)eSendToWorld,     tpl->iSendTo);  // ctor default
}

// AddTimer("") auto-generates the internal label "*timer%010ld"
// (methods_timers.cpp ~149-150) instead of rejecting the empty name; that
// made-up name is the map KEY (strLabel is set from the empty TimerName arg).
// Mirrors the DoAfterSpecial auto-name convention (name.Left(6)=="*timer").
TEST(TimerApi, AddTimerEmptyLabelAutoGeneratesName)
{
    CMUSHclientDoc doc; make_doc(doc);
    EXPECT_EQ(0, (int)doc.GetTimerMap().GetCount());

    ASSERT_EQ((long)eOK, doc.AddTimer("", 0, 5, 0.0, "hi", eEnabled, ""));
    ASSERT_EQ(1, (int)doc.GetTimerMap().GetCount());

    POSITION pos = doc.GetTimerMap().GetStartPosition();
    CString name; CTimer* t = nullptr;
    doc.GetTimerMap().GetNextAssoc(pos, name, t);
    ASSERT_NE(nullptr, t);
    EXPECT_TRUE(name.Left(6) == "*timer");   // made-up internal name
    EXPECT_EQ(16, name.GetLength());         // "*timer" (6) + %010ld (10 digits)
}

MC_TEST_MAIN()
