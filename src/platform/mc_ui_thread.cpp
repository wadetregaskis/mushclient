// platform/mc_ui_thread.cpp — see platform/mc_ui_thread.h.

#include "stdafx.h"
#include "platform/mc_ui_thread.h"

#ifdef MC_DISPATCH_AVAILABLE

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace {

struct Request
  {
  const std::function<void ()> * waited = nullptr;   // on the waiter's stack...
  std::function<void ()> posted;                     // ...or owned by this request
  bool taken   = false;    // the UI thread has it: it can no longer be withdrawn
  bool done    = false;
  bool threw   = false;
  bool refused = false;    // MC_UIThreadShutdown turned it away
  };

// Never destroyed: a script's thread can still be in here as the process exits.
struct Queue
  {
  std::mutex              lock;
  std::condition_variable changed;
  std::deque<Request *>   requests;
  bool                    closing = false;
  };

Queue & queue ()
  {
  static Queue * q = new Queue;
  return *q;
  }

std::thread::id   g_uiThread;
std::atomic<bool> g_inited { false };

std::atomic<int>      g_queued { 0 };           // requests.size (), to read without the lock
std::atomic<bool>     g_inScript { false };     // is the UI thread in a script's own code?
std::atomic<unsigned> g_progress { 0 };         // moves whenever the UI thread gets anywhere
std::atomic<int>      g_parkedLimitMs { kUIThreadParkedLimitMs };

const int kMaxWakes = 4;
std::atomic<void (*) ()> g_wakes [kMaxWakes];
std::atomic<int>         g_wakeCount { 0 };

#ifdef _WIN32

const UINT kServiceMessage = WM_APP + 1;
const char kWindowClass [] = "MUSHclientUIThreadCalls";

std::atomic<HWND> g_window { NULL };
std::atomic<bool> g_messagePosted { false };

LRESULT CALLBACK WindowProc (HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
  {
  if (message == kServiceMessage)
    {
    MC_UIThreadService ();
    return 0;
    }
  return DefWindowProcA (hwnd, message, wParam, lParam);
  }

void CreateMessageWindow ()
  {
  if (g_window)
    return;
  WNDCLASSA wc = { };
  wc.lpfnWndProc   = WindowProc;
  wc.hInstance     = GetModuleHandleA (NULL);
  wc.lpszClassName = kWindowClass;
  RegisterClassA (&wc);    // already registered, the second time: harmless
  g_window = CreateWindowExA (0, kWindowClass, "", 0, 0, 0, 0, 0, HWND_MESSAGE,
                              NULL, wc.hInstance, NULL);
  }

#endif // _WIN32

void Wake ()
  {
#ifdef _WIN32
  // One message is enough for any number of requests, since it serves them all.
  const HWND window = g_window;
  if (window && !g_messagePosted.exchange (true))
    if (!PostMessageA (window, kServiceMessage, 0, 0))
      g_messagePosted = false;
#endif
  const int count = g_wakeCount;
  for (int i = 0; i < count; i++)
    g_wakes [i].load () ();
  }

// False if the client is closing, when nothing is queued.
bool Enqueue (Request * request)
  {
  Queue & q = queue ();
    {
    std::lock_guard<std::mutex> guard (q.lock);
    if (q.closing)
      return false;
    q.requests.push_back (request);
    g_queued++;
    }
  Wake ();
  return true;
  }

} // anonymous namespace

void MC_UIThreadInit ()
  {
  Queue & q = queue ();
  std::lock_guard<std::mutex> guard (q.lock);
  if (g_inited && !q.closing)
    return;
  q.closing  = false;
  g_uiThread = std::this_thread::get_id ();
#ifdef _WIN32
  CreateMessageWindow ();
#endif
  g_inited = true;
  }

bool MC_UIThreadIsCurrent ()
  {
  return !g_inited || std::this_thread::get_id () == g_uiThread;
  }

MCUIResult MC_RunOnUIThread (const std::function<void ()> & call)
  {
  if (MC_UIThreadIsCurrent ())
    {
    call ();
    return MC_UI_RAN;
    }

  Request request;
  request.waited = &call;
  if (!Enqueue (&request))
    return MC_UI_CLOSING;

  // The parked clock runs only while the UI thread is inside a script AND nothing
  // moves.  Anything else restarts it, so a run of short callbacks, each of which
  // finds the UI thread in a script at the instant it is sampled, never adds up.
  typedef std::chrono::steady_clock clock;
  Queue & q = queue ();
  std::unique_lock<std::mutex> guard (q.lock);
  unsigned seen = g_progress;
  clock::time_point parkedSince = clock::now ();
  while (!request.done && !request.refused)
    {
    q.changed.wait_for (guard, std::chrono::milliseconds (20));
    if (request.done || request.refused || request.taken)
      continue;     // taken: the UI thread is running it, so it is not parked

    const unsigned now = g_progress;
    if (now != seen || !g_inScript)
      {
      seen = now;
      parkedSince = clock::now ();
      continue;
      }
    if (clock::now () - parkedSince < std::chrono::milliseconds (g_parkedLimitMs.load ()))
      continue;

    // Withdrawn under the lock, so nothing will touch it (or `call`) again.
    q.requests.erase (std::find (q.requests.begin (), q.requests.end (), &request));
    g_queued--;
    return MC_UI_BLOCKED;
    }

  if (request.refused)
    return MC_UI_CLOSING;
  return request.threw ? MC_UI_THREW : MC_UI_RAN;
  }

void MC_PostToUIThread (std::function<void ()> call)
  {
  Request * request = new Request;
  request->posted = std::move (call);
  if (!Enqueue (request))
    delete request;
  }

bool MC_UIThreadHasWork ()
  {
  return g_queued.load (std::memory_order_relaxed) != 0;
  }

void MC_UIThreadService ()
  {
  if (!g_inited || !MC_UIThreadIsCurrent ())
    return;

  Queue & q = queue ();
  std::unique_lock<std::mutex> guard (q.lock);
#ifdef _WIN32
  g_messagePosted = false;
#endif
  g_progress++;

  // What is here now, and no more: a thread that calls the world in a tight loop
  // must not keep the UI thread in here, away from its messages.
  for (size_t count = q.requests.size (); count > 0 && !q.requests.empty (); count--)
    {
    Request * request = q.requests.front ();
    q.requests.pop_front ();
    g_queued--;
    request->taken = true;
    guard.unlock ();

    bool threw = false;
      {
      MCUIScriptScope notInScript (false);
      try
        {
        if (request->waited)
          (*request->waited) ();
        else
          request->posted ();
        }
      catch (...)   // past here it would strand the waiter, and unwind the message loop
        {
        threw = true;
        }
      }

    guard.lock ();
    g_progress++;
    if (request->waited)
      {
      request->threw = threw;
      request->done  = true;
      q.changed.notify_all ();
      }
    else
      delete request;
    }
  }

void MC_UIThreadAddWake (void (*wake) ())
  {
  const int index = g_wakeCount;
  if (index < kMaxWakes)
    {
    g_wakes [index] = wake;
    g_wakeCount = index + 1;
    }
  }

void MC_UIThreadShutdown ()
  {
  Queue & q = queue ();
  std::lock_guard<std::mutex> guard (q.lock);
  q.closing = true;
  for (Request * request : q.requests)
    {
    if (request->waited)
      request->refused = true;
    else
      delete request;
    }
  q.requests.clear ();
  g_queued = 0;
  q.changed.notify_all ();
#ifdef _WIN32
  if (g_window)
    {
    DestroyWindow (g_window.load ());
    g_window = NULL;
    }
  g_messagePosted = false;
#endif
  }

int MC_UIThreadSetParkedLimit (const int ms)
  {
  return g_parkedLimitMs.exchange (ms);
  }

MCUIScriptScope::MCUIScriptScope (const bool inScript)
  {
  m_previous = g_inScript.load (std::memory_order_relaxed);
  g_inScript.store (inScript, std::memory_order_relaxed);
  g_progress.fetch_add (1, std::memory_order_relaxed);
  }

MCUIScriptScope::~MCUIScriptScope ()
  {
  g_inScript.store (m_previous, std::memory_order_relaxed);
  g_progress.fetch_add (1, std::memory_order_relaxed);
  }

#endif // MC_DISPATCH_AVAILABLE
