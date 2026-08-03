// platform/perl_engine.cpp — embedded Perl scripting engine for the port.
//
// The Windows build runs PerlScript through COM ActiveScripting; off Windows we
// embed libperl directly.  As with the Python engine, the world object model is
// reached through the portable dispatch-map Invoke (platform/mc_dispatch.cpp),
// not a hand-written binding: a `MUSHclient::World` package with an AUTOLOAD
// catches every $world->Method(...) / $world->Property() call and forwards it to
// the XS function MUSHclient::_invoke, which marshals into MC_DispatchInvoke.
//
// One interpreter is shared process-wide; each world/plugin (CScriptEngine) gets
// its own Perl package (MC::W<n>) so subs and $world don't collide.  All scripting
// runs on the UI thread, so the single interpreter context is simply kept current.
//
// Built only when CMake finds a working Perl embedding (MUSHCLIENT_PERL_SCRIPTING).

#if !defined(_WIN32) && defined(MUSHCLIENT_FULL_BUILD) && defined(MUSHCLIENT_PERL_SCRIPTING)

// MFC / MUSHclient / std headers FIRST — Perl's headers #define short names
// (New/Move/Copy/…) that would clash with C++ if they came first.
#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "scripting/scripting.h"
#include "platform/mc_dispatch.h"

#include <string>
#include <vector>
#include <cstdint>
#include <unistd.h>           // environ

// MUSHclient defines WIN32 (no underscore) for source compatibility, but Perl's
// headers read WIN32 as "native Windows build" and pull in win32thread.h /
// dllimport __declspec.  The MFC/MUSHclient headers above are already fully
// parsed, so undefining WIN32 here only affects the Perl include block.
#ifdef WIN32
#undef WIN32
#endif

// Perl headers last.
#include <EXTERN.h>
#include <perl.h>
#include <XSUB.h>

// Perl memory-allocation short names we don't use but which would shadow C++.
#undef New
#undef Newx
#undef Move
#undef Copy
#undef Zero
#undef Renew

#ifndef VT_ERROR
#define VT_ERROR 10
#endif
#ifndef VT_DISPATCH
#define VT_DISPATCH 9
#endif

// Per-engine Perl state (opaque; CScriptEngine holds it as CPerlEngine*).
class CPerlEngine
  {
  public:
  CMUSHclientDoc* doc = nullptr;
  std::string     pkg;            // this engine's package, e.g. "MC::W3"
  };

namespace {

PerlInterpreter* g_perl = nullptr;
bool             g_inited = false;
int              g_counter = 0;

const char* WORLD_STASH = "MUSHclient::World";

// ── SV <-> VARIANT marshalling ──────────────────────────────────────────────
void sv_to_variant(SV* sv, VARIANT& v)
  {
  VariantInit(&v);
  if (!sv || !SvOK(sv))            { v.vt = VT_EMPTY; return; }
  if (SvIOK(sv))                   { v.vt = VT_I4;  v.lVal   = (long) SvIV(sv); return; }
  if (SvNOK(sv))                   { v.vt = VT_R8;  v.dblVal = SvNV(sv);        return; }
  STRLEN len = 0;
  const char* s = SvPV(sv, len);
  // SysAllocString copies a NUL-terminated string; the dispatch API is
  // NUL-terminated throughout, so embedded NULs in a scalar are not a concern.
  v.vt = VT_BSTR;
  v.bstrVal = SysAllocString(s ? s : "");
  }

SV* variant_to_sv(const VARIANT& v)
  {
  if (v.vt & VT_ARRAY)
    {
    SAFEARRAY* sa = v.parray;
    AV* av = newAV();
    if (sa && sa->cDims >= 1)
      {
      long count = (long) sa->rgsabound[0].cElements;
      unsigned base = v.vt & ~VT_ARRAY;
      for (long i = 0; i < count; i++)
        {
        if (base == VT_BSTR)
          {
          BSTR* p = ((BSTR*) sa->pvData) + i;
          av_push(av, newSVpv((*p) ? *p : "", 0));
          }
        else
          {
          VARIANT* p = ((VARIANT*) sa->pvData) + i;
          av_push(av, variant_to_sv(*p));
          }
        }
      }
    return newRV_noinc((SV*) av);
    }

  switch (v.vt)
    {
    case VT_EMPTY: case VT_NULL: case VT_ERROR: return newSV(0);
    case VT_I2:   return newSViv(v.iVal);
    case VT_I4:   return newSViv(v.lVal);
    case VT_R4:   return newSVnv(v.fltVal);
    case VT_R8:   return newSVnv(v.dblVal);
    case VT_BOOL: return newSViv(v.boolVal ? 1 : 0);
    case VT_BSTR: return newSVpv(v.bstrVal ? v.bstrVal : "", 0);
    case VT_DISPATCH:
      {
      // A null dispatch is how the engine says "no such thing" -- GetWorld on a
      // world that is not open, for instance.  Blessing it anyway hands the
      // script a reference that looks live and segfaults on first use; the
      // other three engines return undef/None/nil/undefined here.
      if (!v.byref) return newSV(0);
      SV* ref = newRV_noinc(newSViv((IV)(intptr_t) v.byref));
      sv_bless(ref, gv_stashpv(WORLD_STASH, GV_ADD));
      return ref;
      }
    default:      return newSV(0);
    }
  }

// ── XS: MUSHclient::_invoke($doc, $name, @args) — method or property ─────────
XS(mc_perl_invoke);
XS(mc_perl_invoke)
  {
  dXSARGS;
  if (items < 2) { XSRETURN_UNDEF; }

  CMUSHclientDoc* doc = (CMUSHclientDoc*) (intptr_t) SvIV(ST(0));
  STRLEN nl = 0;
  const char* name = SvPV(ST(1), nl);
  // The proxy holds a bare CMUSHclientDoc* -- there is no COM here, so nothing
  // AddRefs it the way pDoc->GetIDispatch(TRUE) does on Windows.  A script that
  // keeps what world.GetWorld() returned and uses it after that world is closed
  // would dereference freed memory; report it instead.
  // Safe to croak here: nothing with a destructor is live yet.
  if (!MC_DispatchDocIsLive(doc))
    croak("world->%s: that world has been closed", name ? name : "?");

  int nargs = items - 2;

  // Every C++ object with a non-trivial destructor lives in this inner scope and
  // is destroyed BEFORE any croak() below.  croak() longjmps to the enclosing
  // Perl eval and would skip C++ destructors (leak + UB), so we capture the
  // error here and croak only once the scope has fully unwound.
  //
  // A plain char buffer, not a std::string: the message itself is live AT the
  // croak, so a std::string would leak its heap buffer -- smaller than the
  // vectors this scope exists to protect, but the same bug.
  char errmsg[320] = "";
  int count = 0;
  {
    std::vector<VARIANT> vargs((size_t) nargs);
    for (int i = 0; i < nargs; i++)
      sv_to_variant(ST(2 + i), vargs[(size_t) i]);

    std::vector<SV*> rets;
    VARIANT result; VariantInit(&result);

    if (MC_DispatchHas(name))
      {
      std::vector<VARIANT> outargs;
      int rc = MC_DispatchInvoke(doc, name, nargs ? vargs.data() : nullptr, nargs,
                                 result, &outargs);
      for (auto& a : vargs) MC_VariantClear(a);
      if (rc != MC_DISP_OK)
        { MC_VariantClear(result);
          for (auto& o : outargs) MC_VariantClear(o);   // was leaked on this path
          snprintf (errmsg, sizeof errmsg, "world->%s: %s", name,
                    rc == MC_DISP_UNKNOWN ? "unknown method" : "bad arguments"); }
      else
        {
        rets.push_back(variant_to_sv(result));
        MC_VariantClear(result);
        for (auto& o : outargs) { rets.push_back(variant_to_sv(o)); MC_VariantClear(o); }
        }
      }
    else if (MC_DispatchHasProperty(name))
      {
      if (nargs == 0)
        { MC_DispatchGetProperty(doc, name, result);
          rets.push_back(variant_to_sv(result)); MC_VariantClear(result); }
      else
        { MC_DispatchSetProperty(doc, name, vargs[0]);
          for (auto& a : vargs) MC_VariantClear(a);
          rets.push_back(newSV(0)); }
      }
    else
      {
      for (auto& a : vargs) MC_VariantClear(a);
      snprintf (errmsg, sizeof errmsg, "world has no method or property '%s'", name);
      }

    if (!errmsg[0])   // push results onto the Perl stack while rets is alive
      {
      count = (int) rets.size();
      SPAGAIN;   // a world method may have re-entered the shared g_perl stack
                 // (plugin callbacks) and reallocated it — re-derive SP before EXTEND
      if (count > items) EXTEND(SP, count - items);
      for (int i = 0; i < count; i++) ST(i) = sv_2mortal(rets[(size_t) i]);
      }
    }   // vargs / outargs / result / rets destroyed here — safe to croak now

  if (errmsg[0]) croak("%s", errmsg);
  XSRETURN(count);
  }

void xs_init(pTHX)
  {
  newXS("MUSHclient::_invoke", mc_perl_invoke, (char*) __FILE__);
  }

void ensure_init()
  {
  if (g_inited) return;
  int argc = 3;
  static char a0[] = "", a1[] = "-e", a2[] = "0";
  static char* argv[] = { a0, a1, a2, nullptr };
  char** av = argv;
  char** env = environ;
  PERL_SYS_INIT3(&argc, &av, &env);
  g_perl = perl_alloc();
  perl_construct(g_perl);
  PL_exit_flags |= PERL_EXIT_DESTRUCT_END;
  perl_parse(g_perl, xs_init, argc, av, env);
  perl_run(g_perl);

  // The world proxy package: every $world->Foo(...) lands in AUTOLOAD and is
  // forwarded to the XS dispatcher with the doc pointer the object carries.
  eval_pv(
    "package MUSHclient::World;"
    "sub new { my ($c,$p)=@_; my $s=\\$p; return bless $s, $c; }"
    "sub AUTOLOAD { my $self=shift; our $AUTOLOAD;"
    "  (my $n=$AUTOLOAD)=~s/.*:://; return if $n eq 'DESTROY';"
    "  return MUSHclient::_invoke($$self, $n, @_); }"
    "1;", TRUE);
  g_inited = true;
  }

// Surface the pending Perl error ($@) like LuaError does, then clear it.
void report_error(CMUSHclientDoc* doc, const char* what, const char* proc)
  {
  if (!SvTRUE(ERRSV)) return;
  STRLEN n = 0;
  const char* msg = SvPV(ERRSV, n);
  CString s(msg, (int) n);
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
  ReportScriptError (doc, "Perl error",
                     TFormat ("Function/Sub: %s called by %s", proc ? proc : "", what ? what : ""),
                     s, raisedBy);
  sv_setpvn(ERRSV, "", 0);
  }

} // anonymous namespace

// ── CScriptEngine Perl methods ──────────────────────────────────────────────
void CScriptEngine::OpenPerl ()
  {
  ensure_init ();
  CPerlEngine* e = new CPerlEngine ();
  e->doc = m_pDoc;
  e->pkg = "MC::W" + std::to_string (++g_counter);
  m_perlEngine = e;
  }

void CScriptEngine::ClosePerl ()
  {
  if (!m_perlEngine) return;
  delete m_perlEngine;
  m_perlEngine = NULL;
  }

bool CScriptEngine::ParsePerl (const CString & strCode, const CString & strWhat)
  {
  if (!m_perlEngine) return true;
  PERL_SET_CONTEXT (g_perl);
  CPerlEngine* e = m_perlEngine;

  // Run the script in its own package and seed the package global $world there.
  // Set it WITHOUT `our` (as a plain package variable, like Windows PerlScript):
  // non-strict scripts use $world directly, `use strict` scripts declare their
  // own `our $world` — and either way there's no "redeclared" warning.
  // A package AUTOLOAD forwards bare world-API calls — Note(...), GetPluginInfo(...)
  // — to $world->Note(...), matching how Windows PerlScript injects the world
  // object's methods as global subs.  Script-defined subs (the handlers) are
  // resolved normally and never reach AUTOLOAD.
  SV* sv = newSVpvf ("package %s;\n$%s::world = %s->new(%lld);\n"
                     "sub AUTOLOAD { our $AUTOLOAD; (my $n=$AUTOLOAD)=~s/.*:://;"
                     " return if $n eq 'DESTROY'; return $world->$n(@_); }\n"
                     "#line 1 \"script\"\n",
                     e->pkg.c_str (), e->pkg.c_str (), WORLD_STASH, (long long) (intptr_t) e->doc);
  sv_catpvn (sv, (LPCTSTR) strCode, strCode.GetLength ());
  eval_sv (sv, G_DISCARD);
  SvREFCNT_dec (sv);

  if (SvTRUE (ERRSV)) { report_error (e->doc, strWhat, ""); return true; }
  return false;
  }

DISPID CScriptEngine::GetPerlDispid (const CString & strName)
  {
  if (!m_perlEngine || strName.IsEmpty ()) return DISPID_UNKNOWN;
  PERL_SET_CONTEXT (g_perl);
  std::string full = m_perlEngine->pkg + "::" + (LPCTSTR) strName;
  return get_cv (full.c_str (), 0) ? 1 : DISPID_UNKNOWN;
  }

bool CScriptEngine::ExecutePerl (DISPID & dispid, LPCTSTR szProcedure,
                                 const unsigned short iReason,
                                 LPCTSTR szType, LPCTSTR szReason,
                                 list<double> & nparams, list<string> & sparams,
                                 long & nInvocationCount, const t_regexp * regexp,
                                 map<string, string> * table, CPaneLine * paneline,
                                 bool * result)
  {
  if (!m_perlEngine) return false;
  if (dispid == DISPID_UNKNOWN) return false;
  (void) paneline;                       // dropped for Windows-COM parity

  PERL_SET_CONTEXT (g_perl);
  std::string full = m_perlEngine->pkg + "::" + szProcedure;
  CV* cv = get_cv (full.c_str (), 0);
  if (!cv) { dispid = DISPID_UNKNOWN; return true; }

  dSP;
  ENTER; SAVETMPS;
  PUSHMARK (SP);

  for (double d : nparams)
    XPUSHs (sv_2mortal (newSVnv (d)));
  for (const string & s : sparams)
    XPUSHs (sv_2mortal (newSVpvn (s.data (), s.size ())));

  if (regexp)                            // wildcards as an array ref [whole, $1, …]
    {
    int ncapt = 0;
    pcre_fullinfo (regexp->m_program, regexp->m_extra, PCRE_INFO_CAPTURECOUNT, &ncapt);
    AV* av = newAV ();
    for (int i = 0; i <= ncapt; i++)
      { string w (regexp->GetWildcard (i)); av_push (av, newSVpvn (w.data (), w.size ())); }
    XPUSHs (sv_2mortal (newRV_noinc ((SV*) av)));
    }

  if (table)                             // as a hash ref
    {
    HV* hv = newHV ();
    for (const auto & kv : *table)
      hv_store (hv, kv.first.c_str (), (I32) kv.first.size (),
                newSVpvn (kv.second.data (), kv.second.size ()), 0);
    XPUSHs (sv_2mortal (newRV_noinc ((SV*) hv)));
    }

  PUTBACK;

  TraceCallback (szType, szProcedure);
  if (iReason != eDontChangeAction)
    m_pDoc->m_iCurrentActionSource = iReason;
  int n = call_sv ((SV*) cv, G_SCALAR | G_EVAL);
  if (iReason != eDontChangeAction)
    m_pDoc->m_iCurrentActionSource = eUnknownActionSource;

  SPAGAIN;
  bool err = SvTRUE (ERRSV);
  bool truthy = true;
  // A handler with no explicit return yields undef under G_SCALAR; treat that as
  // "keep going" (truthy stays true), matching Lua/Python. Only a *defined* value
  // overrides it — a defined 0 is false, as on the Windows COM path.
  if (n > 0) { SV* r = POPs; if (!err && SvOK (r)) truthy = SvTRUE (r); }
  PUTBACK;
  FREETMPS; LEAVE;

  if (err) { dispid = DISPID_UNKNOWN; report_error (m_pDoc, szType, szProcedure); return true; }
  nInvocationCount++;
  if (result) *result = truthy;
  return false;
  }

bool CScriptEngine::ExecutePerl (DISPID & dispid, LPCTSTR szProcedure,
                                 const unsigned short iReason,
                                 LPCTSTR szType, LPCTSTR szReason,
                                 CString strParam, long & nInvocationCount,
                                 CString & result)
  {
  if (!m_perlEngine) return false;
  if (dispid == DISPID_UNKNOWN) return false;

  PERL_SET_CONTEXT (g_perl);
  std::string full = m_perlEngine->pkg + "::" + szProcedure;
  CV* cv = get_cv (full.c_str (), 0);
  if (!cv) { dispid = DISPID_UNKNOWN; return true; }

  dSP;
  ENTER; SAVETMPS;
  PUSHMARK (SP);
  XPUSHs (sv_2mortal (newSVpvn ((LPCTSTR) strParam, strParam.GetLength ())));
  PUTBACK;

  TraceCallback (szType, szProcedure);
  if (iReason != eDontChangeAction)
    m_pDoc->m_iCurrentActionSource = iReason;
  int n = call_sv ((SV*) cv, G_SCALAR | G_EVAL);
  if (iReason != eDontChangeAction)
    m_pDoc->m_iCurrentActionSource = eUnknownActionSource;

  SPAGAIN;
  bool err = SvTRUE (ERRSV);
  if (n > 0)
    {
    SV* r = POPs;
    // A plain scalar only: a REFERENCE (hashref, arrayref, object) stringifies
    // to "HASH(0x7f...)" and must not become the MUD packet.  Lua gates on
    // lua_isstring for the same reason.
    if (!err && SvOK (r) && !SvROK (r))
      { STRLEN l = 0; const char* p = SvPV (r, l); result = CString (p, (int) l); }
    }
  PUTBACK;
  FREETMPS; LEAVE;

  if (err) { dispid = DISPID_UNKNOWN; report_error (m_pDoc, szType, szProcedure); return true; }
  nInvocationCount++;
  return false;
  }

#endif // !_WIN32 && MUSHCLIENT_FULL_BUILD && MUSHCLIENT_PERL_SCRIPTING
