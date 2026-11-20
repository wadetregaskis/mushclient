// platform/mc_world_call.h — one world call from a script's thread, owning everything
// it touches.
//
// A thread's world call runs on the UI thread (platform/mc_ui_thread.h), and the
// thread can give up waiting for it after the UI thread has STARTED it
// (MC_RunOnUIThreadAbandonable).  The call then runs on, after the thread's own
// stack frame -- where its arguments and results used to live -- has gone.  So it is
// made of this instead: the arguments moved in, the results kept here, the whole held
// by a shared_ptr that the call itself and the waiting thread both own.
//
// Include AFTER stdafx.h (VARIANT) and platform/mc_dispatch.h.

#pragma once

#ifdef MC_DISPATCH_AVAILABLE

#include <string>
#include <utility>
#include <vector>

enum MCWorldCallKind
  {
  MC_CALL_INVOKE,       // a method
  MC_CALL_GET,          // a property's value
  MC_CALL_SET,          // a property's new value (args[0])
  };

// UI thread.  Make the call: MC_DispatchInvoke for a method, or
// MC_DispatchGet/SetProperty.  `rc` is the method's MC_DISP_* code, `ok` the
// property call's answer.
void MC_WorldCallRun (MCWorldCallKind kind, CMUSHclientDoc* doc, const char* name,
                      const VARIANT* args, int nargs, unsigned flags,
                      VARIANT& result, std::vector<VARIANT>* outargs, int& rc, bool& ok);

struct MCWorldCall
  {
  MCWorldCallKind      kind;
  CMUSHclientDoc*      doc;
  std::string          name;
  unsigned             flags;
  bool                 wantOutargs;
  std::vector<VARIANT> args;
  VARIANT              result;
  std::vector<VARIANT> outargs;
  int                  rc = MC_DISP_OK;
  bool                 ok = false;

  // Takes `n` VARIANTs from `from`, leaving each VT_EMPTY there.
  MCWorldCall (MCWorldCallKind k, CMUSHclientDoc* d, const char* method,
               VARIANT* from, size_t n, unsigned f, bool out)
    : kind (k), doc (d), name (method), flags (f), wantOutargs (out)
    {
    VariantInit (&result);
    args.resize (n);
    for (size_t i = 0; i < n; i++)
      {
      args [i] = from [i];
      VariantInit (&from [i]);
      }
    }

  ~MCWorldCall ()
    {
    for (VARIANT& a : args) MC_VariantClear (a);
    MC_VariantClear (result);
    for (VARIANT& o : outargs) MC_VariantClear (o);
    }

  void Run ()
    {
    MC_WorldCallRun (kind, doc, name.c_str (), args.empty () ? nullptr : args.data (),
                     (int) args.size (), flags,
                     result, wantOutargs ? &outargs : nullptr, rc, ok);
    }

  // The results, to the caller, who must MC_VariantClear them.
  void TakeResults (VARIANT& r, std::vector<VARIANT>& o)
    {
    r = result;
    VariantInit (&result);
    o = std::move (outargs);
    outargs.clear ();
    }

  MCWorldCall (const MCWorldCall&) = delete;
  MCWorldCall& operator= (const MCWorldCall&) = delete;
  };

#endif // MC_DISPATCH_AVAILABLE
