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
#include <mach-o/dyld.h>    // _NSGetExecutablePath
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
    // form; return empty.  Cross-world scripting is the one gap vs the JS engine.
    case VT_EMPTY: case VT_NULL: case VT_ERROR: case VT_DISPATCH:
      return Tcl_NewObj ();
    case VT_I2:   return Tcl_NewLongObj (v.iVal);
    case VT_I4:   return Tcl_NewLongObj (v.lVal);
    case VT_R4:   return Tcl_NewDoubleObj (v.fltVal);
    case VT_R8:   return Tcl_NewDoubleObj (v.dblVal);
    case VT_BOOL: return Tcl_NewBooleanObj (v.boolVal != 0);
    case VT_BSTR: return Tcl_NewStringObj (v.bstrVal ? v.bstrVal : "", -1);
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
    std::vector<VARIANT> vargs (nextra);
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
        ("world " + name + (rc == MC_DISP_UNKNOWN ? ": unknown method" : ": bad arguments")).c_str (), -1));
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
  CString hdr;
  hdr.Format ("Tcl error: %s%s%s\n", what ? what : "",
              (proc && *proc) ? " / " : "", proc ? proc : "");
  doc->ColourNote ("white", "red", hdr + CString (msg ? msg : ""));
  }

// Does a command/proc of this name exist in the interpreter?
bool command_exists (Tcl_Interp* interp, const char* name)
  {
  if (!interp || !name || !*name) return false;
  Tcl_CmdInfo info;
  return Tcl_GetCommandInfo (interp, name, &info) != 0;
  }

} // anonymous namespace

// ── CScriptEngine Tcl methods ────────────────────────────────────────────────
void CScriptEngine::OpenTcl ()
  {
  // Point the statically-linked Tcl at the bundled script library
  // (Contents/Resources/tcl8.6) so it finds init.tcl etc. with no system Tcl.
  // Done once, before any interp is created, derived from the executable path so
  // it works in both the dev and packaged .app.  If the bundled library isn't
  // present, leave TCL_LIBRARY unset → Tcl falls back to its built-in path.
  static bool s_tclLibSet = false;
  if (!s_tclLibSet)
    {
    s_tclLibSet = true;
    char exe[PATH_MAX]; uint32_t sz = sizeof (exe); char real[PATH_MAX];
    if (_NSGetExecutablePath (exe, &sz) == 0 && realpath (exe, real))
      {
      std::string p (real);                       // .../Contents/MacOS/MUSHclient
      size_t slash = p.find_last_of ('/');
      if (slash != std::string::npos)
        {
        std::string lib = p.substr (0, slash) + "/../Resources/tcl8.6";
        struct stat st;
        if (stat ((lib + "/init.tcl").c_str (), &st) == 0)
          setenv ("TCL_LIBRARY", lib.c_str (), 1);
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
                                bool * result)
  {
  if (!m_tclEngine) return false;
  if (dispid == DISPID_UNKNOWN) return false;
  Tcl_Interp* interp = m_tclEngine->interp;
  if (!command_exists (interp, szProcedure)) { dispid = DISPID_UNKNOWN; return true; }

  std::vector<Tcl_Obj*> av;
  av.push_back (Tcl_NewStringObj (szProcedure, -1));
  for (double d : nparams)          av.push_back (Tcl_NewDoubleObj (d));
  for (const string & s : sparams)  av.push_back (Tcl_NewStringObj (s.c_str (), -1));
  if (regexp)                                   // wildcards as a Tcl list {whole $1 ...}
    {
    int ncapt = 0;
    pcre_fullinfo (regexp->m_program, regexp->m_extra, PCRE_INFO_CAPTURECOUNT, &ncapt);
    Tcl_Obj* wc = Tcl_NewListObj (0, nullptr);
    for (int i = 0; i <= ncapt; i++)
      { string w (regexp->GetWildcard (i));
        Tcl_ListObjAppendElement (interp, wc, Tcl_NewStringObj (w.c_str (), -1)); }
    av.push_back (wc);
    }
  if (table)                                    // as a Tcl dict (flat key value ...)
    {
    Tcl_Obj* d = Tcl_NewListObj (0, nullptr);
    for (const auto & kv : *table)
      { Tcl_ListObjAppendElement (interp, d, Tcl_NewStringObj (kv.first.c_str (), -1));
        Tcl_ListObjAppendElement (interp, d, Tcl_NewStringObj (kv.second.c_str (), -1)); }
    av.push_back (d);
    }

  for (Tcl_Obj* o : av) Tcl_IncrRefCount (o);
  if (iReason != eDontChangeAction) m_pDoc->m_iCurrentActionSource = iReason;
  int rc = Tcl_EvalObjv (interp, (int) av.size (), av.data (), 0);
  if (iReason != eDontChangeAction) m_pDoc->m_iCurrentActionSource = eUnknownActionSource;

  if (result)                                   // truthy result keeps processing (Lua parity)
    {
    int b = 1;
    if (rc == TCL_OK) Tcl_GetBooleanFromObj (nullptr, Tcl_GetObjResult (interp), &b);
    *result = (rc == TCL_OK) ? (b != 0) : true;
    }
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

  Tcl_Obj* av[2] = { Tcl_NewStringObj (szProcedure, -1),
                     Tcl_NewStringObj ((LPCTSTR) strParam, -1) };
  Tcl_IncrRefCount (av[0]); Tcl_IncrRefCount (av[1]);
  if (iReason != eDontChangeAction) m_pDoc->m_iCurrentActionSource = iReason;
  int rc = Tcl_EvalObjv (interp, 2, av, 0);
  if (iReason != eDontChangeAction) m_pDoc->m_iCurrentActionSource = eUnknownActionSource;
  if (rc == TCL_OK) { const char* r = Tcl_GetStringResult (interp); result = r ? r : ""; }
  Tcl_DecrRefCount (av[0]); Tcl_DecrRefCount (av[1]);

  if (rc != TCL_OK) { dispid = DISPID_UNKNOWN; report_error (m_pDoc, interp, szType, szProcedure); return true; }
  nInvocationCount++;
  return false;
  }

#endif // !_WIN32 && MUSHCLIENT_FULL_BUILD && MUSHCLIENT_TCL_SCRIPTING
