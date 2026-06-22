// platform/ruby_engine.cpp — embedded Ruby scripting engine for the port.
//
// libruby is embedded directly.  CMake prefers a vendored, relocatable Ruby built
// from source (packaging/fetch-ruby.sh) — bundled under Contents/lib and self-
// locating via --enable-load-relative — and falls back to the (deprecated) macOS
// system Ruby.framework when that is absent.
//
// Windows' Ruby support was an ActiveScripting (COM) engine; the port embeds
// libruby directly instead.  As with the Python/Perl/JS engines the world object
// model is reached through the portable dispatch-map Invoke (mc_dispatch.cpp),
// not a hand-written binding: a `World` object's method_missing forwards every
// world.Method(...) / world.Property / world.Property = v call to MC_DispatchInvoke
// / MC_DispatchGet|SetProperty.
//
// Ruby has ONE interpreter per process (ruby_init runs once and cannot be
// re-initialised), so — like the Python/Perl engines — the single VM is shared
// and each world/plugin (CScriptEngine) is isolated in its own object: the
// script is run with instance_eval, so its top-level `def`s become singleton
// methods of that object and cannot collide between worlds/plugins.  All
// scripting runs on the UI thread, so the shared VM is simply kept current.
//
// Built only when CMake finds the Ruby framework (MUSHCLIENT_RUBY_SCRIPTING).

#if !defined(_WIN32) && defined(MUSHCLIENT_FULL_BUILD) && defined(MUSHCLIENT_RUBY_SCRIPTING)

#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "scripting/scripting.h"
#include "platform/mc_dispatch.h"

#include <string>
#include <vector>
#include <cstdint>
#include <pthread.h>        // pthread_get_stackaddr_np — GC stack base (see ensure_started)

// MUSHclient defines WIN32 (no underscore) for source compatibility; Ruby's
// headers read WIN32 as a native-Windows build.  The MFC/MUSHclient headers above
// are already parsed, so undefining it here only affects the Ruby include block.
#ifdef WIN32
#undef WIN32
#endif

#include <ruby.h>

#ifndef VT_ERROR
#define VT_ERROR 10
#endif
#ifndef VT_DISPATCH
#define VT_DISPATCH 9
#endif

// Per-engine Ruby state (opaque; CScriptEngine holds it as CRubyEngine*).
class CRubyEngine
  {
  public:
  CMUSHclientDoc* doc   = nullptr;
  VALUE           obj   = Qnil;   // per-engine isolation object (holds the script's defs)
  VALUE           world = Qnil;   // the `world` host object bound to doc
  };

namespace {

VALUE g_worldClass = Qnil;        // the World class (method_missing -> dispatch)
bool  g_started    = false;

VALUE make_world (CMUSHclientDoc* doc);

// ── VARIANT -> Ruby VALUE ─────────────────────────────────────────────────────
VALUE variant_to_ruby (const VARIANT& v)
  {
  if (v.vt & VT_ARRAY)
    {
    SAFEARRAY* sa = v.parray;
    VALUE arr = rb_ary_new ();
    if (sa && sa->cDims >= 1)
      {
      long count = (long) sa->rgsabound[0].cElements;
      unsigned base = v.vt & ~VT_ARRAY;
      for (long i = 0; i < count; i++)
        {
        if (base == VT_BSTR)
          {
          BSTR* p = ((BSTR*) sa->pvData) + i;
          rb_ary_push (arr, rb_str_new_cstr ((*p) ? *p : ""));
          }
        else
          {
          VARIANT* p = ((VARIANT*) sa->pvData) + i;
          rb_ary_push (arr, variant_to_ruby (*p));
          }
        }
      }
    return arr;
    }

  switch (v.vt)
    {
    case VT_EMPTY: case VT_NULL: case VT_ERROR: return Qnil;
    case VT_I2:   return LONG2NUM (v.iVal);
    case VT_I4:   return LONG2NUM (v.lVal);
    case VT_R4:   return DBL2NUM (v.fltVal);
    case VT_R8:   return DBL2NUM (v.dblVal);
    case VT_BOOL: return v.boolVal ? Qtrue : Qfalse;
    case VT_BSTR: return rb_str_new_cstr (v.bstrVal ? v.bstrVal : "");
    case VT_DISPATCH: return v.byref ? make_world ((CMUSHclientDoc*) v.byref) : Qnil;
    default:      return Qnil;
    }
  }

// ── Ruby VALUE -> VARIANT ─────────────────────────────────────────────────────
void ruby_to_variant (VALUE r, VARIANT& out)
  {
  VariantInit (&out);
  switch (TYPE (r))
    {
    case T_NIL:    out.vt = VT_EMPTY; return;
    case T_TRUE:   out.vt = VT_BOOL; out.boolVal = 1; return;
    case T_FALSE:  out.vt = VT_BOOL; out.boolVal = 0; return;
    case T_FIXNUM: out.vt = VT_I4; out.lVal = (long) NUM2LONG (r); return;
    case T_FLOAT:  out.vt = VT_R8; out.dblVal = NUM2DBL (r); return;
    case T_BIGNUM: out.vt = VT_R8; out.dblVal = NUM2DBL (r); return;
    default: break;
    }
  // strings and everything else -> string (the dispatch coerces as needed)
  VALUE s = rb_obj_as_string (r);
  out.vt = VT_BSTR;
  out.bstrVal = SysAllocString (StringValueCStr (s));
  }

// ── world.<Method|Property>[=] — World#method_missing ────────────────────────
// argv[0] is the method name (a Symbol); argv[1..] are the arguments.
VALUE world_method_missing (int argc, VALUE* argv, VALUE self)
  {
  CMUSHclientDoc* doc = (CMUSHclientDoc*) (intptr_t) NUM2LL (rb_iv_get (self, "@doc"));
  std::string name = rb_id2name (SYM2ID (argv[0]));
  int    nargs = argc - 1;
  VALUE* a     = argv + 1;

  bool setter = (!name.empty () && name.back () == '=');
  if (setter) name.pop_back ();

  if (setter)                                   // world.Prop = value
    {
    if (MC_DispatchHasProperty (name.c_str ()) && nargs >= 1)
      {
      VARIANT v; ruby_to_variant (a[0], v);
      MC_DispatchSetProperty (doc, name.c_str (), v);
      MC_VariantClear (v);
      return nargs ? a[0] : Qnil;
      }
    rb_raise (rb_eNoMethodError, "world.%s= : unknown property", name.c_str ());
    }

  if (MC_DispatchHas (name.c_str ()))           // a method
    {
    std::vector<VARIANT> vargs (nargs);
    for (int i = 0; i < nargs; i++) ruby_to_variant (a[i], vargs[i]);
    VARIANT result; VariantInit (&result);
    std::vector<VARIANT> outargs;
    int rc = MC_DispatchInvoke (doc, name.c_str (),
                                nargs ? vargs.data () : nullptr, nargs, result, &outargs);
    for (auto& x : vargs) MC_VariantClear (x);
    if (rc != MC_DISP_OK)
      {
      MC_VariantClear (result);
      rb_raise (rb_eArgError, "world.%s : %s", name.c_str (),
                rc == MC_DISP_UNKNOWN ? "unknown method" : "bad arguments");
      }
    VALUE ret;
    if (outargs.empty ())
      ret = variant_to_ruby (result);
    else                                        // [out] getters -> [result, out1, ...]
      {
      ret = rb_ary_new ();
      rb_ary_push (ret, variant_to_ruby (result));
      for (auto& o : outargs) { rb_ary_push (ret, variant_to_ruby (o)); MC_VariantClear (o); }
      }
    MC_VariantClear (result);
    return ret;
    }

  if (MC_DispatchHasProperty (name.c_str ()))   // a property read
    {
    VARIANT v;
    if (MC_DispatchGetProperty (doc, name.c_str (), v))
      { VALUE r = variant_to_ruby (v); MC_VariantClear (v); return r; }
    }

  rb_raise (rb_eNoMethodError, "world.%s : unknown method or property", name.c_str ());
  }

VALUE make_world (CMUSHclientDoc* doc)
  {
  VALUE w = rb_obj_alloc (g_worldClass);
  rb_iv_set (w, "@doc", LL2NUM ((long long) (intptr_t) doc));
  return w;
  }

void ensure_started ()
  {
  if (g_started) return;
  g_started = true;

  // Embedded Ruby 3.x/4.x needs the FULL bootstrap, not bare ruby_init().  Ruby 3.4+
  // defaults to the Prism parser, and the parser, the symbol/ID tables and the gem
  // prelude are only fully set up by ruby_options() — skip it and method dispatch
  // is left desynced (core C methods such as Kernel#require and GC.disable resolve
  // to the wrong IDs, surfacing as bogus NoMethodErrors).  So run the same sequence
  // the standalone `ruby` binary does, handing it a do-nothing program.
  //
  // The GC must also be told where the C stack begins, or it scans garbage once
  // anything allocates heavily.  Scripting is driven from arbitrary depths of the
  // wx event loop, not one fixed frame, so anchor the base at the very top of the
  // (UI) thread's stack: the GC then conservatively scans the whole stack, valid
  // for every call site.
  static char arg0[] = "mushclient";
  static char arg1[] = "-e0";                // bootstrap with an empty program
  char*  argv_[] = { arg0, arg1, nullptr };
  int    argc_   = 2;
  char** pargv   = argv_;
  ruby_sysinit (&argc_, &pargv);
  ruby_init_stack ((VALUE*) pthread_get_stackaddr_np (pthread_self ()));
  ruby_init ();
  ruby_options (argc_, pargv);                // full runtime: parser, prelude, encodings
  ruby_init_loadpath ();

  g_worldClass = rb_define_class ("MUSHclientWorld", rb_cObject);
  rb_global_variable (&g_worldClass);
  rb_define_method (g_worldClass, "method_missing",
                    (VALUE (*)(ANYARGS)) world_method_missing, -1);
  }

// rb_protect trampolines (rb_protect passes a single VALUE-sized cookie).
struct EvalCtx { VALUE recv; VALUE code; };
VALUE do_instance_eval (VALUE p)
  {
  EvalCtx* c = (EvalCtx*) p;
  VALUE argv[1] = { c->code };
  return rb_obj_instance_eval (1, argv, c->recv);
  }
struct CallCtx { VALUE recv; ID mid; int argc; const VALUE* argv; };
VALUE do_funcall (VALUE p)
  {
  CallCtx* c = (CallCtx*) p;
  return rb_funcall2 (c->recv, c->mid, c->argc, c->argv);
  }

// Fetch err.message() — invoked under rb_protect so a custom exception whose
// #message itself raises can't longjmp out through report_error's C++ frames
// (there's no enclosing Ruby tag here: we're already past the rb_protect that
// caught the original exception).
static VALUE ruby_error_message (VALUE err) { return rb_funcall (err, rb_intern ("message"), 0); }

void report_error (CMUSHclientDoc* doc, const char* what, const char* proc)
  {
  if (!doc) return;
  VALUE err = rb_errinfo ();
  std::string msg = "?", klass;
  if (RTEST (err))
    {
    klass = rb_obj_classname (err);
    int state = 0;
    VALUE s = rb_protect (ruby_error_message, err, &state);
    if (state)
      rb_set_errinfo (Qnil);             // #message itself raised — discard it
    else if (TYPE (s) == T_STRING)
      msg = StringValueCStr (s);
    }
  rb_set_errinfo (Qnil);
  CString hdr;
  hdr.Format ("Ruby error: %s%s%s\n", what ? what : "",
              (proc && *proc) ? " / " : "", proc ? proc : "");
  doc->ColourNote ("white", "red", hdr + CString ((klass + ": " + msg).c_str ()));
  fprintf (stderr, "[RubyError] %s%s%s : %s: %s\n", what ? what : "",
           (proc && *proc) ? " / " : "", proc ? proc : "",
           klass.c_str (), msg.c_str ());
  }

} // anonymous namespace

// ── CScriptEngine Ruby methods ────────────────────────────────────────────────
void CScriptEngine::OpenRuby ()
  {
  ensure_started ();
  CRubyEngine* e = new CRubyEngine ();
  e->doc   = m_pDoc;
  e->obj   = rb_obj_alloc (rb_cObject);     // the per-engine isolation object
  e->world = make_world (m_pDoc);
  rb_gc_register_address (&e->obj);
  rb_gc_register_address (&e->world);
  // Singleton method `world` on the object returns its world, so the script can
  // simply say world.Note("hi") while its def's stay isolated to this object.
  rb_ivar_set (e->obj, rb_intern ("@__world"), e->world);
  rb_define_singleton_method (e->obj, "world",
      (VALUE (*)(ANYARGS)) +[](VALUE self) -> VALUE {
        return rb_ivar_get (self, rb_intern ("@__world")); }, 0);
  m_rubyEngine = e;
  }

void CScriptEngine::CloseRuby ()
  {
  if (!m_rubyEngine) return;
  rb_gc_unregister_address (&m_rubyEngine->obj);
  rb_gc_unregister_address (&m_rubyEngine->world);
  delete m_rubyEngine;          // the VM itself persists (Ruby cannot re-init)
  m_rubyEngine = NULL;
  }

bool CScriptEngine::ParseRuby (const CString & strCode, const CString & strWhat)
  {
  if (!m_rubyEngine) return true;
  EvalCtx ctx { m_rubyEngine->obj, rb_str_new_cstr ((LPCTSTR) strCode) };
  int state = 0;
  rb_protect (do_instance_eval, (VALUE) &ctx, &state);
  if (state) { report_error (m_rubyEngine->doc, strWhat, ""); return true; }
  return false;
  }

DISPID CScriptEngine::GetRubyDispid (const CString & strName)
  {
  if (!m_rubyEngine || strName.IsEmpty ()) return DISPID_UNKNOWN;
  return rb_respond_to (m_rubyEngine->obj, rb_intern ((LPCTSTR) strName))
           ? 1 : DISPID_UNKNOWN;
  }

// Rich callback: proc(nparams..., sparams..., [wildcards], {table}) — same
// argument order as the JS/Tcl engines, so the engines behave consistently.
bool CScriptEngine::ExecuteRuby (DISPID & dispid, LPCTSTR szProcedure,
                                 const unsigned short iReason,
                                 LPCTSTR szType, LPCTSTR /*szReason*/,
                                 list<double> & nparams, list<string> & sparams,
                                 long & nInvocationCount, const t_regexp * regexp,
                                 map<string, string> * table, CPaneLine * /*paneline*/,
                                 bool * result)
  {
  if (!m_rubyEngine) return false;
  if (dispid == DISPID_UNKNOWN) return false;
  ID mid = rb_intern (szProcedure);
  if (!rb_respond_to (m_rubyEngine->obj, mid)) { dispid = DISPID_UNKNOWN; return true; }

  std::vector<VALUE> av;
  for (double d : nparams)         av.push_back (DBL2NUM (d));
  for (const string & s : sparams) av.push_back (rb_str_new_cstr (s.c_str ()));
  if (regexp)                                   // wildcards as an Array [whole, $1, ...]
    {
    int ncapt = 0;
    pcre_fullinfo (regexp->m_program, regexp->m_extra, PCRE_INFO_CAPTURECOUNT, &ncapt);
    VALUE wc = rb_ary_new ();
    for (int i = 0; i <= ncapt; i++)
      { string w (regexp->GetWildcard (i)); rb_ary_push (wc, rb_str_new_cstr (w.c_str ())); }
    av.push_back (wc);
    }
  if (table)                                    // as a Hash
    {
    VALUE h = rb_hash_new ();
    for (const auto & kv : *table)
      rb_hash_aset (h, rb_str_new_cstr (kv.first.c_str ()), rb_str_new_cstr (kv.second.c_str ()));
    av.push_back (h);
    }

  if (iReason != eDontChangeAction) m_pDoc->m_iCurrentActionSource = iReason;
  CallCtx ctx { m_rubyEngine->obj, mid, (int) av.size (), av.empty () ? nullptr : av.data () };
  int state = 0;
  VALUE r = rb_protect (do_funcall, (VALUE) &ctx, &state);
  if (iReason != eDontChangeAction) m_pDoc->m_iCurrentActionSource = eUnknownActionSource;

  if (state) { dispid = DISPID_UNKNOWN; report_error (m_pDoc, szType, szProcedure); return true; }
  nInvocationCount++;
  if (result) *result = RTEST (r);              // truthy result keeps processing (Lua parity)
  return false;
  }

// Simple callback: proc(strParam) -> result string.
bool CScriptEngine::ExecuteRuby (DISPID & dispid, LPCTSTR szProcedure,
                                 const unsigned short iReason,
                                 LPCTSTR szType, LPCTSTR /*szReason*/,
                                 CString strParam, long & nInvocationCount,
                                 CString & result)
  {
  if (!m_rubyEngine) return false;
  if (dispid == DISPID_UNKNOWN) return false;
  ID mid = rb_intern (szProcedure);
  if (!rb_respond_to (m_rubyEngine->obj, mid)) { dispid = DISPID_UNKNOWN; return true; }

  VALUE arg = rb_str_new_cstr ((LPCTSTR) strParam);
  if (iReason != eDontChangeAction) m_pDoc->m_iCurrentActionSource = iReason;
  CallCtx ctx { m_rubyEngine->obj, mid, 1, &arg };
  int state = 0;
  VALUE r = rb_protect (do_funcall, (VALUE) &ctx, &state);
  if (iReason != eDontChangeAction) m_pDoc->m_iCurrentActionSource = eUnknownActionSource;

  if (state) { dispid = DISPID_UNKNOWN; report_error (m_pDoc, szType, szProcedure); return true; }
  nInvocationCount++;
  if (RTEST (r) && TYPE (r) != T_NIL)
    { VALUE s = rb_obj_as_string (r); result = StringValueCStr (s); }
  return false;
  }

#endif // !_WIN32 && MUSHCLIENT_FULL_BUILD && MUSHCLIENT_RUBY_SCRIPTING
