// platform/tcl_engine.cpp — embedded Tcl engine for the port, via the macOS Tcl
// system framework (no bundled dependency).
//
// Windows' Tcl support was ActiveState's "TclScript" COM ActiveScripting engine;
// the port embeds libtcl directly instead.  As with the Python/Perl/JS engines,
// the world object model is reached through the portable dispatch-map Invoke
// (platform/mc_dispatch.cpp), not a hand-written binding.
//
// Tcl is command-based, so the world object is a single `world` command:
//     world Note "hello"             ;# call a method
//     world Send "look"
//     set v [world GetVariable foo]  ;# method returning a value
//     world Trace                    ;# read a property
//     world Trace 1                  ;# write a property
// Method names take priority over like-named properties (as in the JS engine).
//
// Each world/plugin gets its own Tcl_Interp, so globals/procs are isolated with
// no namespace partitioning (Tcl supports independent interpreters cleanly —
// unlike the single-interpreter Python/Perl engines).
//
// Built only where the Tcl framework is found (MUSHCLIENT_TCL_SCRIPTING).

#if !defined(_WIN32) && defined(MUSHCLIENT_FULL_BUILD) && defined(MUSHCLIENT_TCL_SCRIPTING)

#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "scripting/scripting.h"
#include "platform/mc_dispatch.h"
#include "platform/mc_variant_args.h"

// MUSHclient defines WIN32 (no underscore) for source compatibility; tcl.h would
// then treat this as a Windows build (dllimport storage classes, Win types).  The
// MFC headers above are already parsed, so undefining it here only affects tcl.h.
#ifdef WIN32
#undef WIN32
#endif

#include <tcl.h>
#include <string>
#include <vector>
#include <cstdlib>          // setenv, realpath
#include <sys/stat.h>       // stat
#ifdef __APPLE__
#include <mach-o/dyld.h>    // _NSGetExecutablePath
#else
#include <unistd.h>         // readlink
#endif
#include <limits.h>         // PATH_MAX

#ifndef VT_ERROR
#define VT_ERROR 10
#endif
#ifndef VT_DISPATCH
#define VT_DISPATCH 9
#endif

// Per-engine Tcl state (opaque; CScriptEngine holds it as CTclEngine*).
class CTclEngine
  {
  public:
  CMUSHclientDoc* doc    = nullptr;
  Tcl_Interp*     interp = nullptr;
  };

namespace {

// ── VARIANT -> Tcl_Obj ───────────────────────────────────────────────────────
Tcl_Obj* variant_to_tcl (Tcl_Interp* interp, const VARIANT& v)
  {
  if (v.vt & VT_ARRAY)
    {
    SAFEARRAY* sa = v.parray;
    Tcl_Obj* list = Tcl_NewListObj (0, nullptr);
    if (sa && sa->cDims >= 1)
      {
      long count = (long) sa->rgsabound[0].cElements;
      unsigned base = v.vt & ~VT_ARRAY;
      for (long i = 0; i < count; i++)
        {
        if (base == VT_BSTR)
          {
          BSTR* p = ((BSTR*) sa->pvData) + i;
          Tcl_ListObjAppendElement (interp, list, Tcl_NewStringObj ((*p) ? *p : "", -1));
          }
        else
          {
          VARIANT* p = ((VARIANT*) sa->pvData) + i;
          Tcl_ListObjAppendElement (interp, list, variant_to_tcl (interp, *p));
          }
        }
      }
    return list;
    }

  switch (v.vt)
    {
    // VT_DISPATCH (a nested `world` object, e.g. GetWorld) has no Tcl object
    // -- which also means a Tcl script cannot hold a proxy to another world and
    // so cannot outlive one.  The other four engines mint a proxy here and need
    // an MC_DispatchDocIsLive check on every call; Tcl is safe by construction.
    // form; return empty.  Cross-world scripting is the one gap vs the JS engine.
    case VT_EMPTY: case VT_NULL: case VT_ERROR: case VT_DISPATCH:
      return Tcl_NewObj ();
    case VT_I2:   return Tcl_NewLongObj (v.iVal);
    case VT_I4:   return Tcl_NewLongObj (v.lVal);
    case VT_R4:   return Tcl_NewDoubleObj (v.fltVal);
    case VT_R8:   return Tcl_NewDoubleObj (v.dblVal);
    case VT_BOOL: return Tcl_NewBooleanObj (v.boolVal != 0);
    case VT_BSTR: return Tcl_NewStringObj (v.bstrVal ? v.bstrVal : "", -1);
    case VT_DATE: return Tcl_NewLongObj (MC_VariantDateToUnix(v));
    default:      return Tcl_NewObj ();
    }
  }

// Marshal a Tcl argument as a string (VT_BSTR); the dispatch coerces it to the
// method's real parameter type (mc_dispatch.cpp: atol/atof for numeric params),
// which suits Tcl's everything-is-a-string value model.
void tcl_to_variant (Tcl_Obj* o, VARIANT& out)
  {
  VariantInit (&out);
  out.vt = VT_BSTR;
  out.bstrVal = SysAllocString (o ? Tcl_GetString (o) : "");
  }

// ── the `world` command: world <Method|Property> [args...] ───────────────────
int World_cmd (ClientData cd, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[])
  {
  CMUSHclientDoc* doc = (CMUSHclientDoc*) cd;
  if (objc < 2)
    { Tcl_WrongNumArgs (interp, 1, objv, "method ?arg ...?"); return TCL_ERROR; }

  std::string name = Tcl_GetString (objv[1]);
  int nextra = objc - 2;

  if (MC_DispatchHas (name.c_str ()))           // a method
    {
    MCVariantArgs vargs ((size_t) nextra);
    for (int i = 0; i < nextra; i++) tcl_to_variant (objv[2 + i], vargs[i]);

    VARIANT result; VariantInit (&result);
    std::vector<VARIANT> outargs;
    int rc = MC_DispatchInvoke (doc, name.c_str (),
                                nextra ? vargs.data () : nullptr, nextra, result, &outargs);
    for (auto& a : vargs) MC_VariantClear (a);

    if (rc != MC_DISP_OK)
      {
      MC_VariantClear (result);
      Tcl_SetObjResult (interp, Tcl_NewStringObj (
        ("world " + name + ": " +
         (rc == MC_DISP_UNKNOWN ? std::string ("unknown method")
          : MC_DispatchBadArgsMessage (name.c_str (), nextra))).c_str (), -1));
      return TCL_ERROR;
      }

    if (outargs.empty ())
      Tcl_SetObjResult (interp, variant_to_tcl (interp, result));
    else                                        // [out] getters -> {result out1 out2 ...}
      {
      Tcl_Obj* list = Tcl_NewListObj (0, nullptr);
      Tcl_ListObjAppendElement (interp, list, variant_to_tcl (interp, result));
      for (auto& o : outargs) { Tcl_ListObjAppendElement (interp, list, variant_to_tcl (interp, o)); MC_VariantClear (o); }
      Tcl_SetObjResult (interp, list);
      }
    MC_VariantClear (result);
    return TCL_OK;
    }

  if (MC_DispatchHasProperty (name.c_str ()))   // a property
    {
    if (nextra == 0)                            // read
      {
      VARIANT v;
      if (MC_DispatchGetProperty (doc, name.c_str (), v))
        { Tcl_SetObjResult (interp, variant_to_tcl (interp, v)); MC_VariantClear (v); return TCL_OK; }
      }
    else if (nextra == 1)                       // write
      {
      VARIANT v; tcl_to_variant (objv[2], v);
      bool ok = MC_DispatchSetProperty (doc, name.c_str (), v);
      MC_VariantClear (v);
      if (ok) return TCL_OK;
      }
    Tcl_SetObjResult (interp, Tcl_NewStringObj (("world " + name + ": bad property access").c_str (), -1));
    return TCL_ERROR;
    }

  Tcl_SetObjResult (interp, Tcl_NewStringObj (("world " + name + ": unknown method or property").c_str (), -1));
  return TCL_ERROR;
  }

void report_error (CMUSHclientDoc* doc, Tcl_Interp* interp, const char* what, const char* proc)
  {
  if (!doc || !interp) return;
  const char* msg = Tcl_GetStringResult (interp);
  // Route through the shared policy (scripting/lua_scripting.cpp), so this
  // engine obeys World Properties > Scripting > "Note errors to the output
  // window" and raises the canonical CScriptErrorDlg otherwise -- exactly as
  // Windows does for every COM language via CScriptEngine::ShowError.  It used
  // to ColourNote unconditionally, which made that setting a dead reader and
  // the dialog (and its "show errors in the output window next time" checkbox)
  // unreachable for any world not scripted in Lua.
  CString raisedBy = "No active world";
  if (doc)
    raisedBy = doc->m_CurrentPlugin
             ? TFormat ("Plugin: %s (called from world: %s)",
                        (LPCTSTR) doc->m_CurrentPlugin->m_strName, (LPCTSTR) doc->m_mush_name)
             : TFormat ("World: %s", (LPCTSTR) doc->m_mush_name);
  ReportScriptError (doc, "Tcl error",
                     TFormat ("Function/Sub: %s called by %s", proc ? proc : "", what ? what : ""),
                     msg ? msg : "", raisedBy);
  }

// Does a command/proc of this name exist in the interpreter?
// A handler is a PROC the script defined -- not any command that happens to
// exist.  Tcl_GetCommandInfo finds every built-in too (puts, set, string, ...,
// and the `world` command this engine installs), so it answered "yes, that
// callback is implemented" for hundreds of names no plugin ever wrote.  `info
// procs` is the distinction Tcl itself draws.
bool command_exists (Tcl_Interp* interp, const char* name)
  {
  if (!interp || !name || !*name) return false;
  Tcl_Obj* cmd = Tcl_NewListObj (0, nullptr);
  Tcl_ListObjAppendElement (interp, cmd, Tcl_NewStringObj ("info", -1));
  Tcl_ListObjAppendElement (interp, cmd, Tcl_NewStringObj ("procs", -1));
  Tcl_ListObjAppendElement (interp, cmd, Tcl_NewStringObj (name, -1));
  Tcl_IncrRefCount (cmd);
  const int rc = Tcl_EvalObjEx (interp, cmd, TCL_EVAL_DIRECT);
  Tcl_DecrRefCount (cmd);
  if (rc != TCL_OK) { Tcl_ResetResult (interp); return false; }
  int len = 0;
  Tcl_ListObjLength (interp, Tcl_GetObjResult (interp), &len);
  Tcl_ResetResult (interp);
  return len > 0;
  }

} // anonymous namespace

// ── CScriptEngine Tcl methods ────────────────────────────────────────────────
void CScriptEngine::OpenTcl ()
  {
  // Tcl's platform initialisation, which sets the system encoding (utf-8 here
  // rather than the built-in iso8859-1 default) and -- given a real argv0 --
  // populates [info nameofexecutable].  Every embedding is supposed to call it
  // once before the first Tcl_CreateInterp.  Passing NULL did only the
  // encoding half: TclpFindExecutable returns immediately on a NULL argv0, so
  // [info nameofexecutable] answered "" in every Tcl world and plugin -- and
  // [file dirname] of "" is ".", silently pointing a script that locates
  // resources beside the app at the process CWD instead.  The executable path
  // is the same one that locates the bundled script library
  // (Contents/Resources/tcl8.6, so init.tcl is found with no system Tcl), so
  // one lookup now serves both.  Done once, before any interp is created,
  // working in the dev build and the packaged .app alike.  If the bundled
  // library isn't present, TCL_LIBRARY stays unset → Tcl falls back to its
  // built-in path.
  static bool s_platformInited = false;
  if (!s_platformInited)
    {
    s_platformInited = true;
    char real[PATH_MAX];
#ifdef __APPLE__
    char exe[PATH_MAX]; uint32_t sz = sizeof (exe);
    const bool haveExe =
        _NSGetExecutablePath (exe, &sz) == 0 && realpath (exe, real) != NULL;
#else
    const ssize_t n = readlink ("/proc/self/exe", real, sizeof (real) - 1);
    if (n > 0) real[n] = '\0';
    const bool haveExe = n > 0;
#endif
    Tcl_FindExecutable (haveExe ? real : NULL);   // copies the name; `real` need not outlive this
    if (haveExe)
      {
      std::string p (real);                       // .../Contents/MacOS/MUSHclient
      size_t slash = p.find_last_of ('/');
      if (slash != std::string::npos)
        {
        const std::string dir = p.substr (0, slash);
        static const char* const kRelCandidates[] = { "/../Resources/tcl8.6", "/tcl8.6", "/../share/mushclient/tcl8.6" };
        for (const char* rel : kRelCandidates)
          {
          std::string lib = dir + rel;
          struct stat st;
          if (stat ((lib + "/init.tcl").c_str (), &st) == 0)
            {
            setenv ("TCL_LIBRARY", lib.c_str (), 1);
            break;
            }
          }
        }
      }
    }

  CTclEngine* e = new CTclEngine ();
  e->doc    = m_pDoc;
  e->interp = Tcl_CreateInterp ();
  Tcl_Init (e->interp);   // best-effort: load the script library (built-ins work regardless)
  Tcl_CreateObjCommand (e->interp, "world", World_cmd, (ClientData) m_pDoc, nullptr);
  m_tclEngine = e;
  }

void CScriptEngine::CloseTcl ()
  {
  if (!m_tclEngine) return;
  if (m_tclEngine->interp) Tcl_DeleteInterp (m_tclEngine->interp);
  delete m_tclEngine;
  m_tclEngine = NULL;
  }

bool CScriptEngine::ParseTcl (const CString & strCode, const CString & strWhat)
  {
  if (!m_tclEngine) return true;
  if (Tcl_Eval (m_tclEngine->interp, (LPCTSTR) strCode) != TCL_OK)
    { report_error (m_tclEngine->doc, m_tclEngine->interp, strWhat, ""); return true; }
  return false;
  }

DISPID CScriptEngine::GetTclDispid (const CString & strName)
  {
  if (!m_tclEngine || strName.IsEmpty ()) return DISPID_UNKNOWN;
  return command_exists (m_tclEngine->interp, (LPCTSTR) strName) ? 1 : DISPID_UNKNOWN;
  }

// Rich callback: <proc> <nparams...> <sparams...> [wildcards-list] [table-dict]
// (same argument order the JS engine uses, so the engines behave consistently).
bool CScriptEngine::ExecuteTcl (DISPID & dispid, LPCTSTR szProcedure,
                                const unsigned short iReason,
                                LPCTSTR szType, LPCTSTR /*szReason*/,
                                list<double> & nparams, list<string> & sparams,
                                long & nInvocationCount, const t_regexp * regexp,
                                map<string, string> * table, CPaneLine * /*paneline*/,
                                bool * result, const bool bLowercaseWildcards,
                                bool * pbReturnedAValue)
  {
  if (!m_tclEngine) return false;
  if (dispid == DISPID_UNKNOWN) return false;
  Tcl_Interp* interp = m_tclEngine->interp;
  if (!command_exists (interp, szProcedure)) { dispid = DISPID_UNKNOWN; return true; }

  std::vector<Tcl_Obj*> av;
  av.push_back (Tcl_NewStringObj (szProcedure, -1));
  // An integral value becomes an INTEGER object, not a double.  nparams is a
  // list<double> because that is the widest thing the seam carries, but a
  // trigger's line number or a timer's count arriving as "16.0" is useless in
  // Tcl: it cannot be `incr`ed, cannot index a string, and reads wrong in any
  // message the script builds.  Lua and JavaScript both hand the script a number
  // that stringifies without the decimal point; Tcl was the only engine that
  // could not.
  for (double d : nparams)
    {
    if (d == (double) (long long) d &&
        d >= -9007199254740992.0 && d <= 9007199254740992.0)
      av.push_back (Tcl_NewWideIntObj ((Tcl_WideInt) d));
    else
      av.push_back (Tcl_NewDoubleObj (d));
    }
  // Length-explicit (-1 means strlen): sparams carries the raw telnet
  // subnegotiation payload (OnPluginTelnetSubnegotiation), which holds NUL
  // bytes -- ZMP and MSDP separate their fields with them.  Same rule the
  // simple overload below already follows; wildcards and the table ride the
  // same marshalling, so they follow it too.
  for (const string & s : sparams)
    av.push_back (Tcl_NewStringObj (s.data (), (int) s.size ()));
  if (regexp)                                   // wildcards as a Tcl list {whole $1 ...}
    {
    vector<string> wildcards;
    regexp->GetCallbackWildcards (bLowercaseWildcards, wildcards);
    Tcl_Obj* wc = Tcl_NewListObj (0, nullptr);
    for (size_t i = 0; i < wildcards.size (); i++)
      { const string & w = wildcards [i];
        Tcl_ListObjAppendElement (interp, wc, Tcl_NewStringObj (w.data (), (int) w.size ())); }
    av.push_back (wc);
    }
  if (table)                                    // as a Tcl dict (flat key value ...)
    {
    Tcl_Obj* d = Tcl_NewListObj (0, nullptr);
    for (const auto & kv : *table)
      { Tcl_ListObjAppendElement (interp, d, Tcl_NewStringObj (kv.first.data (), (int) kv.first.size ()));
        Tcl_ListObjAppendElement (interp, d, Tcl_NewStringObj (kv.second.data (), (int) kv.second.size ())); }
    av.push_back (d);
    }

  for (Tcl_Obj* o : av) Tcl_IncrRefCount (o);
  TraceCallback (szType, szProcedure);
  if (iReason != eDontChangeAction) m_pDoc->m_iCurrentActionSource = iReason;
  int rc = Tcl_EvalObjv (interp, (int) av.size (), av.data (), 0);
  if (iReason != eDontChangeAction) m_pDoc->m_iCurrentActionSource = eUnknownActionSource;

  // A proc that falls off the end yields the empty string, which is not a
  // boolean -- so the conversion FAILING is Tcl's "said nothing", not just the
  // error arm.  `b` keeps its 1 in that case, exactly as before, so `result` is
  // computed identically; only the new flag is new.  docs/UPSTREAM-BUGS.md 90.
  int b = 1;
  const bool bSaid = rc == TCL_OK &&
        Tcl_GetBooleanFromObj (nullptr, Tcl_GetObjResult (interp), &b) == TCL_OK;
  if (result)                                   // truthy result keeps processing (Lua parity)
    *result = (rc == TCL_OK) ? (b != 0) : true;
  if (pbReturnedAValue)
    *pbReturnedAValue = bSaid;
  for (Tcl_Obj* o : av) Tcl_DecrRefCount (o);

  if (rc != TCL_OK) { dispid = DISPID_UNKNOWN; report_error (m_pDoc, interp, szType, szProcedure); return true; }
  nInvocationCount++;
  return false;
  }

// Simple callback: <proc> <strParam>, returning the result string.
bool CScriptEngine::ExecuteTcl (DISPID & dispid, LPCTSTR szProcedure,
                                const unsigned short iReason,
                                LPCTSTR szType, LPCTSTR /*szReason*/,
                                CString strParam, long & nInvocationCount,
                                CString & result)
  {
  if (!m_tclEngine) return false;
  if (dispid == DISPID_UNKNOWN) return false;
  Tcl_Interp* interp = m_tclEngine->interp;
  if (!command_exists (interp, szProcedure)) { dispid = DISPID_UNKNOWN; return true; }

  // Length-explicit for the argument: -1 means strlen, and this overload carries
  // whole MUD packets, which contain NUL bytes.
  Tcl_Obj* av[2] = { Tcl_NewStringObj (szProcedure, -1),
                     Tcl_NewStringObj ((LPCTSTR) strParam, strParam.GetLength ()) };
  Tcl_IncrRefCount (av[0]); Tcl_IncrRefCount (av[1]);
  TraceCallback (szType, szProcedure);
  if (iReason != eDontChangeAction) m_pDoc->m_iCurrentActionSource = iReason;
  int rc = Tcl_EvalObjv (interp, 2, av, 0);
  if (iReason != eDontChangeAction) m_pDoc->m_iCurrentActionSource = eUnknownActionSource;
  // Only a non-empty result replaces the caller's string.  This callback shape
  // is ExecutePluginScriptRtn's, where the same CString is both the argument and
  // the result -- OnPluginPacketReceived, OnPluginCommandChanged and friends --
  // so an unconditional assignment blanked every packet and every typed command
  // the moment a Tcl world loaded a plugin.  Tcl has no "returned nothing"
  // sentinel: a proc that falls off the end leaves an empty interpreter result,
  // indistinguishable from an explicit `return ""`.  Empty therefore means
  // "leave it alone", which is the same effect the four sibling engines get from
  // their None / undefined / nil / undef checks.
  if (rc == TCL_OK)
    {
    int rlen = 0;
    const char* r = Tcl_GetStringFromObj (Tcl_GetObjResult (interp), &rlen);
    if (r && rlen > 0) result = CString (r, rlen);
    }
  Tcl_DecrRefCount (av[0]); Tcl_DecrRefCount (av[1]);

  if (rc != TCL_OK) { dispid = DISPID_UNKNOWN; report_error (m_pDoc, interp, szType, szProcedure); return true; }
  nInvocationCount++;
  return false;
  }

#endif // !_WIN32 && MUSHCLIENT_FULL_BUILD && MUSHCLIENT_TCL_SCRIPTING
