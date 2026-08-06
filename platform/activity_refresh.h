// platform/activity_refresh.h — when the Activity window repopulates.
//
// Global Preferences ▸ Activity carries three settings: the refresh type
// ("On New Activity" / "Periodically, every N seconds" / "Both"), the interval
// N, and — implicitly — the engine's "new activity" flag that the display code
// raises on every new line (doc_shared.cpp "new activity!").  Windows spends
// two separate pumps on them: CMainFrame::OnTimer does the periodic half
// (mainfrm.cpp:768-783, comparing App.m_timeLastActivityUpdate + N against the
// wall clock) and CMUSHclientApp::OnIdle does the activity half
// (MUSHclient.cpp:1085-1091).  The port runs neither pump, so all three
// settings were dead until wx_activity.cpp grew one short wxTimer that answers
// both halves.
//
// WHY IT WAS LIFTED
//
// That answer is live policy — it decides whether the user's "every N seconds"
// means anything — but it lived in a private method of an anonymous-namespace
// wxFrame in a mushgui-only TU, so no test could even name it, let alone reach
// it: every in-app selftest exits from wxApp::OnInit, before the event loop
// that would deliver wxEVT_TIMER (docs/AUDIT-2026-08.md).  It is a pure
// function of (type, interval, flag, elapsed, accumulator), so it is lifted
// here whole — header-only and free of wx/MFC types, like pal/image_placement.h
// — and pinned in tests/test_pal_worldview.cpp.
//
// TWO DELIBERATE DEPARTURES FROM THE WINDOWS PUMPS, both because the port
// counts elapsed milliseconds where Windows compares CTime stamps:
//
//   * the interval is guarded, because a non-positive one would make the
//     accumulator fire on every single tick.  Windows never has to: its
//     comparison is against a stamp, so N=0 merely means "always due", i.e.
//     once per timer tick of the MFC pump.  Here a 0 would be a busy loop
//     rebuilding the list 4× a second.  The prefs dialog's DDV bounds N to
//     1..300 but the prefs DB is not validated, so the guard belongs here.
//   * the remainder is CARRIED past a firing rather than dropped.  Windows
//     restarts its clock at `now` on every update (ActivityView.cpp:158), so
//     it drops the overshoot; the port's tick is 250 ms — 2.5× MFC's default
//     timer — and dropping up to a tick per period would stretch "every 15
//     seconds" a little further with each one.  The carry is bounded to a
//     single interval (modulo, not repeated subtraction) so that waking from a
//     long sleep still costs ONE refresh, exactly as the stamp comparison
//     does, rather than a catch-up storm.

#pragma once

// Mirrors CMUSHclientApp's anonymous refresh-type enum.  Restated rather than
// included so this header stays free of MUSHclient.h (and therefore of MFC);
// wx_activity.cpp static_asserts the two agree, so the copy cannot drift.
enum ActivityRefreshType {
    kActivityRefreshOnActivity   = 0,
    kActivityRefreshPeriodically = 1,
    kActivityRefreshBoth         = 2,
};

// Used when the stored interval is unusable.  Matches the shipped default in
// globalregistryoptions.cpp ("ActivityWindowRefreshInterval", 15).
const int kActivityRefreshDefaultSecs = 15;

struct ActivityRefreshDecision {
    bool      refresh       = false;   // repopulate the list on this tick?
    long long accumulatorMs = 0;       // carry this back into the caller's state
};

// One tick of the Activity window's refresh policy.
//
// refreshType     — App.m_nActivityWindowRefreshType (an unknown value is
//                   treated as "Both": a garbled preference must not leave the
//                   window frozen).
// intervalSecs    — App.m_nActivityWindowRefreshInterval, taken SIGNED on
//                   purpose.  The member is unsigned, so a negative value in
//                   the prefs DB reads back as a huge positive one — and the
//                   OLD code cast it back to int only INSIDE the true arm of
//                   its own guard:
//
//                       const int secs = (App.m_n... > 0) ? (int) App.m_n... : 15;
//
//                   so a stored -5 passed the `> 0` test as 4294967291, became
//                   -5 again, and `elapsed >= -5000` was true on EVERY tick.
//                   The window rebuilt its whole list four times a second, for
//                   ever.  Not "never refresh" — the opposite, which is why the
//                   guard has to take the value already signed.
//
//                   Only the sign half is caught here.  A large POSITIVE
//                   interval (say 100000, comfortably inside int) still means
//                   "effectively never", and there is no upper clamp — the
//                   dialog's DDV bounds it 1..300 and the prefs DB does not.
//                   Same as before, so not a regression, but not covered
//                   either.
// activityPending — App.m_bUpdateActivity, the engine's new-line flag.
// elapsedMs       — real time since the previous tick.  A non-positive value
//                   (a clock stepped backwards) contributes nothing rather
//                   than rewinding the accumulator.
// accumulatorMs   — the caller's running total, from the previous decision.
inline ActivityRefreshDecision ActivityRefreshForTick (unsigned int refreshType,
                                                       int          intervalSecs,
                                                       bool         activityPending,
                                                       long long    elapsedMs,
                                                       long long    accumulatorMs)
  {
  ActivityRefreshDecision decision;
  decision.accumulatorMs = accumulatorMs;

  // The activity half — everything except "Periodically" listens to the flag.
  const bool bActivityDriven =
      refreshType != (unsigned int) kActivityRefreshPeriodically && activityPending;

  // The periodic half — everything except "On New Activity" counts seconds.
  if (refreshType != (unsigned int) kActivityRefreshOnActivity)
    {
    const long long thresholdMs =
        (long long) (intervalSecs > 0 ? intervalSecs : kActivityRefreshDefaultSecs) * 1000;

    if (elapsedMs > 0)
      decision.accumulatorMs += elapsedMs;

    if (decision.accumulatorMs >= thresholdMs)
      {
      decision.refresh        = true;
      decision.accumulatorMs %= thresholdMs;   // carry the remainder, bounded
      }
    }

  // A refresh the flag asked for restarts the periodic clock outright, because
  // the list has just been rebuilt — that is what stamping
  // m_timeLastActivityUpdate = now does on Windows (ActivityView.cpp:158).
  if (bActivityDriven)
    {
    decision.refresh       = true;
    decision.accumulatorMs = 0;
    }

  return decision;
  }   // end of ActivityRefreshForTick
