// platform/mc_ui_thread.h — running a script thread's world call on the UI thread.
//
// A world method touches MFC windows, which belong to the thread that made them,
// and a document nothing else locks.  So a thread a SCRIPT created (Python's
// threading.Thread, a Ruby Thread) must not run one itself: its call is queued
// here and it waits while the UI thread runs it.  The UI thread gets to the queue
//
//   * from its message loop, through a posted message, so modal dialogs and menus
//     do not stop it;
//   * at its own next world call (MC_DispatchInvoke); and
//   * through whatever wake an engine adds -- Python's Py_AddPendingCall, which
//     runs between bytecodes of a callback that is busy but not blocked.
//
// A CALLER MUST HOLD NO LOCK THE UI THREAD CAN WANT while it waits: the world call
// may well run a trigger, which needs the GIL.  Hence Py_BEGIN_ALLOW_THREADS and
// rb_thread_call_without_gvl around MC_RunOnUIThread.
//
// The one way this can still hang is the UI thread waiting for the waiter:
//
//     def on_line(name, line, wc):
//         t = threading.Thread(target=work); t.start(); t.join()    # work() calls world.Note
//
// So the UI thread says when it is inside a script's own code (MCUIScriptScope), and
// a waiter gives up if the UI thread stays there, serving nothing, for the parked
// limit: MC_UI_BLOCKED, which the engine raises as an exception in the thread.  That
// normally ends the thread or releases what it held, and so frees the UI thread.
// The UI thread being busy anywhere else -- a long Lua callback, a world method,
// C++ -- is not a deadlock, because none of those can be waiting for a script's
// thread, and the waiter just keeps waiting.  A callback that calls time.sleep(10)
// looks exactly like a blocked one, and so a thread calling the world meanwhile gets
// the exception; that cannot be told apart from here.
//
// The other way round is a call the UI thread has started, which then runs a script
// callback that waits for the thread -- for a lock the thread holds, say.  The same
// test, on the same clock, lets the waiter give that up too (MC_RunOnUIThreadAbandonable,
// MC_UI_ABANDONED), and the same caveat applies: a callback inside the call that
// computes for longer than the limit, without a world call, looks stuck.

#pragma once
#include "platform/mc_dispatch.h"

#ifdef MC_DISPATCH_AVAILABLE

#include <functional>

enum MCUIResult
  {
  MC_UI_RAN,        // the call ran
  MC_UI_BLOCKED,    // the UI thread is parked in a script: the call did NOT run
  MC_UI_CLOSING,    // the client is closing down: the call did NOT run
  MC_UI_THREW,      // the call ran, and a C++ exception escaped from it
  MC_UI_ABANDONED,  // the call STARTED, then the UI thread stuck in a script inside it
                    // for the limit: it may still finish, but nobody hears how
  };

// How long a waiter tolerates the UI thread parked in a script.
const int kUIThreadParkedLimitMs = 5000;

// The calling thread is the UI thread from now on.  Called by the first document's
// constructor, which runs before any script can exist.
void MC_UIThreadInit ();

// Is this the UI thread?  (True before MC_UIThreadInit: nothing has been told otherwise.)
bool MC_UIThreadIsCurrent ();

// Run `call` on the UI thread and wait for it.  On the UI thread it just runs it.
// Once the UI thread has started it, this waits for it however long it takes.
MCUIResult MC_RunOnUIThread (const std::function<void ()> & call);

// The same, except that it can also give up on a call the UI thread has STARTED:
// when inside it the UI thread gets stuck in a script, unmoving, for the limit.  That
// is a thread holding a lock while it calls the world, and the call running a
// callback (world.Send's OnPluginSend) that waits for the same lock: neither can move,
// and the client froze.  MC_UI_ABANDONED, then, and the thread's exception lets go of
// its lock.  So `call` must own everything it touches -- capture by value -- because
// it may run on, and is destroyed by the UI thread, after this has returned.
MCUIResult MC_RunOnUIThreadAbandonable (std::function<void ()> call);

// Have the UI thread run `call` when it can, without waiting for it.
void MC_PostToUIThread (std::function<void ()> call);

// Does anything want running?  Cheap; for the UI thread's own world calls.
bool MC_UIThreadHasWork ();

// Run what is queued.  UI thread only; anywhere else it does nothing.
void MC_UIThreadService ();

// One more thing to call when work is queued, from whichever thread queued it.  The
// Windows message window needs no adding.
void MC_UIThreadAddWake (void (*wake) ());

// The client is closing: everyone waiting gets MC_UI_CLOSING, and so does anyone
// who asks from now on.
void MC_UIThreadShutdown ();

// For tests.  Returns the previous limit.
int MC_UIThreadSetParkedLimit (int ms);

// The UI thread is (true) or is not (false) inside a script's own code, for as long
// as this exists.  They nest: a callback calling a world method calling a callback.
class MCUIScriptScope
  {
  public:
  explicit MCUIScriptScope (bool inScript);
  ~MCUIScriptScope ();

  private:
  bool m_previous;
  MCUIScriptScope (const MCUIScriptScope &) = delete;
  MCUIScriptScope & operator= (const MCUIScriptScope &) = delete;
  };

#endif // MC_DISPATCH_AVAILABLE
