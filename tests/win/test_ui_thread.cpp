// tests/win/test_ui_thread.cpp -- platform/mc_ui_thread.cpp: a thread handing a call
// to the UI thread, and giving up when the UI thread is parked in a script.  The
// engines' side, with real Python and Ruby threads, is in --engines-selftest.

#include "mc_test.h"
#include "stdafx.h"
#include "platform/mc_ui_thread.h"

#ifdef MC_DISPATCH_AVAILABLE

#include <atomic>
#include <memory>
#include <chrono>
#include <stdexcept>
#include <thread>

namespace {

struct Fixture
  {
  int limit;
  Fixture () : limit (MC_UIThreadSetParkedLimit (200)) { MC_UIThreadInit (); }
  ~Fixture () { MC_UIThreadSetParkedLimit (limit); }
  };

// A message loop's work, so that what gets through is what the posted message
// delivers: MC_UIThreadService is not called by any test that uses this.
void pump_until (const std::atomic<bool> & done, const int ms = 5000)
  {
  const auto end = std::chrono::steady_clock::now () + std::chrono::milliseconds (ms);
  while (!done && std::chrono::steady_clock::now () < end)
    {
    MSG msg;
    while (PeekMessageA (&msg, NULL, 0, 0, PM_REMOVE))
      DispatchMessageA (&msg);
    Sleep (1);
    }
  }

void pump_for (const int ms)
  {
  std::atomic<bool> never { false };
  pump_until (never, ms);
  }

} // namespace

TEST(UIThread, OnTheUIThreadItJustRuns)
{
    Fixture f;
    EXPECT_TRUE(MC_UIThreadIsCurrent());
    bool ran = false;
    EXPECT_EQ(MC_UI_RAN, MC_RunOnUIThread([&] { ran = true; }));
    EXPECT_TRUE(ran);
}

TEST(UIThread, AThreadsCallRunsOnTheUIThreadByWayOfTheMessageLoop)
{
    Fixture f;
    std::thread::id ranOn;
    std::atomic<bool> done { false };
    std::atomic<int> result { -1 };
    bool threadSawItself = true;
    std::thread t ([&]
      {
      threadSawItself = MC_UIThreadIsCurrent();
      result = MC_RunOnUIThread([&] { ranOn = std::this_thread::get_id(); });
      done = true;
      });
    pump_until(done);
    t.join();
    EXPECT_FALSE(threadSawItself);
    EXPECT_EQ((int) MC_UI_RAN, result.load());
    EXPECT_TRUE(ranOn == std::this_thread::get_id());
}

TEST(UIThread, NothingRunsUntilTheUIThreadGetsToIt)
{
    Fixture f;
    std::atomic<bool> ran { false }, done { false };
    std::thread t ([&] { MC_RunOnUIThread([&] { ran = true; }); done = true; });
    Sleep(100);     // not pumping, and not in a script, so this is not a deadlock
    EXPECT_FALSE(ran.load());
    EXPECT_FALSE(done.load());
    pump_until(done);
    t.join();
    EXPECT_TRUE(ran.load());
}

TEST(UIThread, AWaiterGivesUpOnAUIThreadParkedInAScript)
{
    Fixture f;
    std::atomic<bool> ran { false }, done { false };
    std::atomic<int> result { -1 };
    std::thread t;
    const auto start = std::chrono::steady_clock::now ();
      {
      MCUIScriptScope inScript (true);
      t = std::thread ([&]
        {
        result = MC_RunOnUIThread([&] { ran = true; });
        done = true;
        });
      while (!done && std::chrono::steady_clock::now () - start < std::chrono::seconds (5))
        Sleep(5);
      }
    t.join();
    EXPECT_EQ((int) MC_UI_BLOCKED, result.load());

    // It was withdrawn, and the lambda it held has gone with the waiter's stack, so
    // nothing is left for the message loop to find.
    EXPECT_FALSE(MC_UIThreadHasWork());
    pump_for(50);
    EXPECT_FALSE(ran.load());
}

TEST(UIThread, AWaiterKeepsWaitingForAUIThreadBusyAnywhereElse)
{
    Fixture f;
    std::atomic<bool> ran { false }, done { false };
    std::atomic<int> result { -1 };
    std::thread t ([&]
      {
      result = MC_RunOnUIThread([&] { ran = true; });
      done = true;
      });
    Sleep(600);     // three times the limit, with no script anywhere
    EXPECT_FALSE(done.load());
    pump_until(done);
    t.join();
    EXPECT_EQ((int) MC_UI_RAN, result.load());
    EXPECT_TRUE(ran.load());
}

TEST(UIThread, InAWorldMethodCalledFromAScriptIsNotParked)
{
    Fixture f;
    std::atomic<bool> done { false };
    std::atomic<int> result { -1 };
    std::thread t;
      {
      MCUIScriptScope inScript (true);
        {
        MCUIScriptScope worldCall (false);
        t = std::thread ([&]
          {
          result = MC_RunOnUIThread([] { });
          done = true;
          });
        Sleep(600);
        EXPECT_FALSE(done.load());
        }
      pump_until(done);     // back in the script, but it is getting served
      }
    t.join();
    EXPECT_EQ((int) MC_UI_RAN, result.load());
}

TEST(UIThread, ARunOfShortScriptsDoesNotAddUpToParked)
{
    Fixture f;
    std::atomic<bool> stop { false }, done { false };
    std::atomic<int> result { -1 };
    std::thread t ([&]
      {
      result = MC_RunOnUIThread([&] { });
      done = true;
      });
    // In a script nearly all of the time, as a busy world's triggers are, for three
    // times the limit -- and never long enough, in one go, to count.
    const auto end = std::chrono::steady_clock::now () + std::chrono::milliseconds (600);
    while (std::chrono::steady_clock::now () < end)
      {
      MCUIScriptScope inScript (true);
      Sleep(10);
      }
    EXPECT_FALSE(done.load());
    pump_until(done);
    t.join();
    EXPECT_EQ((int) MC_UI_RAN, result.load());
}

TEST(UIThread, ACallThatThrowsDoesNotStrandItsWaiter)
{
    Fixture f;
    std::atomic<bool> done { false };
    std::atomic<int> result { -1 };
    std::thread t ([&]
      {
      result = MC_RunOnUIThread([] { throw std::runtime_error ("from a world method"); });
      done = true;
      });
    pump_until(done);
    t.join();
    EXPECT_EQ((int) MC_UI_THREW, result.load());
}

TEST(UIThread, APostedCallRunsWithoutAnyoneWaiting)
{
    Fixture f;
    std::atomic<bool> ran { false };
    std::thread t ([&] { MC_PostToUIThread([&] { ran = true; }); });
    t.join();
    EXPECT_FALSE(ran.load());
    pump_until(ran);
    EXPECT_TRUE(ran.load());
}

TEST(UIThread, ClosingWakesAWaiterAndTurnsAwayTheNext)
{
    Fixture f;
    std::atomic<bool> ran { false }, done { false };
    std::atomic<int> result { -1 };
    std::thread t ([&]
      {
      result = MC_RunOnUIThread([&] { ran = true; });
      done = true;
      });
    while (!MC_UIThreadHasWork())
      Sleep(1);
    MC_UIThreadShutdown();
    t.join();
    EXPECT_EQ((int) MC_UI_CLOSING, result.load());
    EXPECT_FALSE(ran.load());

    std::atomic<int> later { -1 };
    std::thread t2 ([&] { later = MC_RunOnUIThread([&] { ran = true; }); });
    t2.join();
    EXPECT_EQ((int) MC_UI_CLOSING, later.load());
    EXPECT_FALSE(ran.load());

    MC_UIThreadInit();     // for the tests after this one
}

// A call the UI thread has STARTED, which then sits in a script that does not move --
// the callback a world call runs, waiting for a lock the calling thread holds.  The
// waiter gives up (MC_UI_ABANDONED), lets go of what it held, and the call finishes
// into the state it owns, which the UI thread then destroys.
namespace {
struct Shared
  {
  std::atomic<bool> release { false };
  std::atomic<bool> finished { false };
  std::atomic<bool> * destroyed;
  explicit Shared (std::atomic<bool> * d) : destroyed (d) {}
  ~Shared () { *destroyed = true; }
  };
}

TEST(UIThread, AnAbandonableCallStuckInAScriptIsGivenUp)
{
    Fixture f;
    std::atomic<bool> done { false }, destroyed { false };
    std::atomic<int> result { -1 };
    auto s = std::make_shared<Shared> (&destroyed);
    std::thread t ([&, s]
      {
      result = MC_RunOnUIThreadAbandonable([s]
        {
        MCUIScriptScope inScript (true);        // the callback, waiting for the "lock"
        const auto end = std::chrono::steady_clock::now () + std::chrono::seconds (5);
        while (!s->release && std::chrono::steady_clock::now () < end)
          Sleep (5);
        s->finished = true;
        });
      s->release = true;                        // the thread lets go of its lock
      done = true;
      });
    s.reset();                                  // the thread and the call own it now
    pump_until(done);
    t.join();
    pump_for(50);
    EXPECT_EQ((int) MC_UI_ABANDONED, result.load());
    EXPECT_TRUE(destroyed.load());              // finished, and destroyed by the UI thread
}

TEST(UIThread, APlainCallStuckInAScriptIsStillWaitedFor)
{
    Fixture f;
    std::atomic<bool> done { false };
    std::atomic<int> result { -1 };
    std::thread t ([&]
      {
      result = MC_RunOnUIThread([]
        {
        MCUIScriptScope inScript (true);
        Sleep (600);
        });
      done = true;
      });
    pump_until(done);
    t.join();
    EXPECT_EQ((int) MC_UI_RAN, result.load());
}

TEST(UIThread, AnAbandonableCallBusyOutsideAScriptIsWaitedFor)
{
    Fixture f;
    std::atomic<bool> done { false };
    std::atomic<int> result { -1 };
    std::thread t ([&]
      {
      result = MC_RunOnUIThreadAbandonable([] { Sleep (600); });   // a heavy world method
      done = true;
      });
    pump_until(done);
    t.join();
    EXPECT_EQ((int) MC_UI_RAN, result.load());
}

TEST(UIThread, TheLimitIsFiveSeconds)
{
    EXPECT_EQ(5000, kUIThreadParkedLimitMs);
}

#endif // MC_DISPATCH_AVAILABLE
