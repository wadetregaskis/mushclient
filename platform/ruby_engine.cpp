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
#include "platform/mc_variant_args.h"

#include <string>
#include <vector>
#include <cstdint>
#include <climits>          // LONG_MAX — the 64-bit range a VARIANT's lVal holds
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
    case VT_DATE: return LONG2NUM(MC_VariantDateToUnix(v));
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
    case T_BIGNUM:
      {
      // A Bignum that still fits lVal must cross as VT_I4, not VT_R8.  CRuby's
      // tagged Fixnum stops at 2^62, so 2^62+1 is already a Bignum -- and a
      // double holds only 53 mantissa bits, so it arrived rounded, and every
      // one of them reached a BSTR parameter as "%.14g" scientific notation
      // (world.SetVariable ('big', 2**62+1) stored "4.6116860184274e+18").
      // rb_integer_pack fills the ABSOLUTE value and returns the sign (±2 when
      // it did not fit), so the range test raises nothing: an rb_raise here
      // would longjmp past the live C++ destructors this file warns about.
      unsigned long long mag = 0;
      const int sign = rb_integer_pack (r, &mag, 1, sizeof mag, 0,
                                        INTEGER_PACK_NATIVE_BYTE_ORDER);
      if (sign == 1 && mag <= (unsigned long long) LONG_MAX)
        { out.vt = VT_I4; out.lVal = (long) mag; return; }
      if (sign == -1 && mag <= (unsigned long long) LONG_MAX + 1ULL)
        { out.vt = VT_I4; out.lVal = (long) (- (long long) (mag - 1) - 1); return; }
      out.vt = VT_R8; out.dblVal = NUM2DBL (r); return;   // genuinely out of range
      }
    default: break;
    }
  // strings and everything else -> string (the dispatch coerces as needed)
  VALUE s = rb_obj_as_string (r);
  out.vt = VT_BSTR;
  // RSTRING_PTR, not StringValueCStr: the latter RAISES ArgumentError on a
  // String holding an embedded NUL, and this runs inside world_method_missing
  // with a live std::vector<VARIANT> and std::string on the stack -- the longjmp
  // to ExecuteRuby's rb_protect would skip their destructors (a leak per call).
  // RSTRING_PTR is always NUL-terminated by MRI, and SysAllocString truncates at
  // the first NUL, so this matches the other four engines (perl/js/tcl/py all
  // feed a raw char* straight through) and the non-NUL case is unchanged.
  out.bstrVal = SysAllocString (RSTRING_PTR (s));
  }

// ── world.<Method|Property>[=] — World#method_missing ────────────────────────
// argv[0] is the method name (a Symbol); argv[1..] are the arguments.
VALUE world_method_missing (int argc, VALUE* argv, VALUE self)
  {
  CMUSHclientDoc* doc = (CMUSHclientDoc*) (intptr_t) NUM2LL (rb_iv_get (self, "@doc"));

  // rb_raise longjmps to the enclosing rb_protect and does NOT unwind C++
  // frames, so nothing with a destructor may be live when one fires.  Every such
  // object lives in the inner scope below; an error is recorded in a plain char
  // buffer (no destructor) and raised only once that scope has fully unwound.
  // Three raises used to fire with `name`, `vargs` and `outargs` alive -- and
  // outargs' VARIANTs had not even been cleared, so a failed [out] getter leaked
  // whatever strings or arrays the method had already allocated.
  char  errmsg[320] = "";
  VALUE errclass    = Qnil;
  VALUE ret         = Qnil;

  // argc: method_missing has arity -1 and a script may call it directly, in
  // which case argv[0] is out of bounds and SYM2ID would read garbage.  Checked
  // before anything is constructed, so raising here is safe.
  if (argc < 1)
    rb_raise (rb_eArgError, "world: no method name given");

  // The proxy holds a bare CMUSHclientDoc*; there is no COM here, so nothing
  // AddRefs it the way pDoc->GetIDispatch(TRUE) does on Windows.  A script that
  // keeps what world.GetWorld returned and uses it after that world is closed
  // would dereference freed memory.
  if (!MC_DispatchDocIsLive (doc))
    rb_raise (rb_eRuntimeError, "world: that world has been closed");

    {
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
        ret = nargs ? a[0] : Qnil;
        }
      else
        {
        snprintf (errmsg, sizeof errmsg, "world.%s= : unknown property", name.c_str ());
        errclass = rb_eNoMethodError;
        }
      }
    else if (MC_DispatchHas (name.c_str ()))      // a method
      {
      MCVariantArgs vargs ((size_t) nargs);
      for (int i = 0; i < nargs; i++) ruby_to_variant (a[i], vargs[i]);
      VARIANT result; VariantInit (&result);
      std::vector<VARIANT> outargs;
      int rc = MC_DispatchInvoke (doc, name.c_str (),
                                  nargs ? vargs.data () : nullptr, nargs, result, &outargs);
      for (auto& x : vargs) MC_VariantClear (x);

      if (rc != MC_DISP_OK)
        {
        MC_VariantClear (result);
        for (auto& o : outargs) MC_VariantClear (o);   // was leaked on this path
        const std::string why = (rc == MC_DISP_UNKNOWN)
                                ? std::string ("unknown method")
                                : MC_DispatchBadArgsMessage (name.c_str (), nargs);
        snprintf (errmsg, sizeof errmsg, "world.%s : %s", name.c_str (), why.c_str ());
        errclass = rb_eArgError;
        }
      else if (outargs.empty ())
        ret = variant_to_ruby (result);
      else                                        // [out] getters -> [result, out1, ...]
        {
        ret = rb_ary_new ();
        rb_ary_push (ret, variant_to_ruby (result));
        for (auto& o : outargs) { rb_ary_push (ret, variant_to_ruby (o)); MC_VariantClear (o); }
        }
      if (rc == MC_DISP_OK) MC_VariantClear (result);
      }
    else if (MC_DispatchHasProperty (name.c_str ()))   // a property read
      {
      VARIANT v;
      if (MC_DispatchGetProperty (doc, name.c_str (), v))
        { ret = variant_to_ruby (v); MC_VariantClear (v); }
      else
        {
        snprintf (errmsg, sizeof errmsg, "world.%s : property is not readable", name.c_str ());
        errclass = rb_eNoMethodError;
        }
      }
    else
      {
      snprintf (errmsg, sizeof errmsg, "world.%s : unknown method or property", name.c_str ());
      errclass = rb_eNoMethodError;
      }
    }   // every destructor has run by here

  if (!NIL_P (errclass))
    rb_raise (errclass, "%s", errmsg);
  return ret;
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

// Convert a callback's result via #to_s under rb_protect: a script-defined #to_s
// can raise, and with no enclosing Ruby tag a raise here would longjmp straight
// through the C++ frames (skipping destructors). Same hazard as ruby_error_message.
static VALUE ruby_to_s (VALUE v) { return rb_obj_as_string (v); }

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
      // Length-explicit, like the ruby_to_s path below: StringValueCStr RAISES
      // on an embedded NUL, and here there is NO enclosing Ruby tag (rb_protect
      // already returned), so that raise hits rb_bug -> abort, crashing the whole
      // client on any `raise "...\0..."` from a Ruby script.  A message with a
      // NUL truncates like everything else instead of taking the process down.
      msg.assign (RSTRING_PTR (s), RSTRING_LEN (s));
    }
  rb_set_errinfo (Qnil);
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
  ReportScriptError (doc, "Ruby error",
                     TFormat ("Function/Sub: %s called by %s", proc ? proc : "", what ? what : ""),
                     CString ((klass + ": " + msg).c_str ()), raisedBy);
  fprintf (stderr, "[RubyError] %s%s%s : %s: %s\n", what ? what : "",
           (proc && *proc) ? " / " : "", proc ? proc : "",
           klass.c_str (), msg.c_str ());
  }


// Is `name` a handler THE SCRIPT defined?
//
// rb_respond_to answers yes for all ~58 public instance methods every Object
// inherits (inspect, to_s, send, class, hash, tap, then, itself, freeze, ...)
// and for the `world` accessor OpenRuby installs -- so "does this plugin
// implement that callback?", which is how plugin dispatch works, always
// answered yes, and a trigger sending to a script named `send` or `display`
// silently invoked Ruby's own method with the wrong arity.
//
// A script's `def` inside instance_eval becomes a SINGLETON method on the
// isolation object, so `singleton_methods(false)` is exactly the set the script
// wrote -- minus `world`, which the engine installed the same way.
struct RespondCtx { VALUE obj; ID mid; };
VALUE do_singleton_defined (VALUE p)
  {
  RespondCtx* c = (RespondCtx*) p;
  VALUE list = rb_funcall (c->obj, rb_intern ("singleton_methods"), 1, Qfalse);
  return rb_ary_includes (list, ID2SYM (c->mid)) ? Qtrue : Qfalse;
  }

bool script_defined (VALUE obj, ID mid)
  {
  if (mid == rb_intern ("world")) return false;   // the engine's own accessor
  RespondCtx c { obj, mid };
  int state = 0;
  VALUE r = rb_protect (do_singleton_defined, (VALUE) &c, &state);
  if (state) { rb_set_errinfo (Qnil); return false; }
  return RTEST (r);
  }

} // anonymous namespace

// ── CScriptEngine Ruby methods ────────────────────────────────────────────────
void CScriptEngine::OpenRuby ()
  {
  ensure_started ();
  CRubyEngine* e = new CRubyEngine ();
  e->doc   = m_pDoc;
  // Register the slots BEFORE filling them.  CRubyEngine lives on the C++ heap,
  // which MRI's conservative collector does not scan, so between the allocation
  // and the rb_gc_register_address that was to follow it the only reference was
  // in unscanned memory -- a collection there would free an object still in use.
  e->obj   = Qnil;
  e->world = Qnil;
  rb_gc_register_address (&e->obj);
  rb_gc_register_address (&e->world);
  e->obj   = rb_obj_alloc (rb_cObject);     // the per-engine isolation object
  e->world = make_world (m_pDoc);
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
  return script_defined (m_rubyEngine->obj, rb_intern ((LPCTSTR) strName))
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
                                 bool * result, const bool bLowercaseWildcards,
                                 bool * pbReturnedAValue)
  {
  if (!m_rubyEngine) return false;
  if (dispid == DISPID_UNKNOWN) return false;
  ID mid = rb_intern (szProcedure);
  if (!script_defined (m_rubyEngine->obj, mid)) { dispid = DISPID_UNKNOWN; return true; }

  // Root the args in a Ruby Array held by a C-stack local — MRI's conservative
  // GC scans the machine stack, so this keeps every freshly-allocated arg (the
  // Strings, the wildcard Array, the Hash) reachable through construction and the
  // call. A C-heap std::vector<VALUE> would be invisible to the GC, so an
  // allocation later in the loop could collect an earlier arg → use-after-collect.
  VALUE av = rb_ary_new ();
  for (double d : nparams)         rb_ary_push (av, DBL2NUM (d));
  for (const string & s : sparams) rb_ary_push (av, rb_str_new_cstr (s.c_str ()));
  if (regexp)                                   // wildcards as an Array [whole, $1, ...]
    {
    vector<string> wildcards;
    regexp->GetCallbackWildcards (bLowercaseWildcards, wildcards);
    VALUE wc = rb_ary_new ();
    for (size_t i = 0; i < wildcards.size (); i++)
      { const string & w = wildcards [i]; rb_ary_push (wc, rb_str_new_cstr (w.c_str ())); }
    rb_ary_push (av, wc);
    }
  if (table)                                    // as a Hash
    {
    VALUE h = rb_hash_new ();
    for (const auto & kv : *table)
      rb_hash_aset (h, rb_str_new_cstr (kv.first.c_str ()), rb_str_new_cstr (kv.second.c_str ()));
    rb_ary_push (av, h);
    }

  TraceCallback (szType, szProcedure);
  if (iReason != eDontChangeAction) m_pDoc->m_iCurrentActionSource = iReason;
  // RB_GC_GUARD after the call: ctx holds a raw INTERIOR pointer into `av`'s
  // payload, which does not point at the object header, so the collector cannot
  // see `av` from it.  Without the guard the compiler is free to treat `av` as
  // dead the moment the pointer is taken, and a collection during the call would
  // free the argument array out from under the callee.
  CallCtx ctx { m_rubyEngine->obj, mid, RARRAY_LENINT (av), RARRAY_CONST_PTR (av) };
  int state = 0;
  VALUE r = rb_protect (do_funcall, (VALUE) &ctx, &state);
  RB_GC_GUARD (av);
  if (iReason != eDontChangeAction) m_pDoc->m_iCurrentActionSource = eUnknownActionSource;

  if (state) { dispid = DISPID_UNKNOWN; report_error (m_pDoc, szType, szProcedure); return true; }
  nInvocationCount++;
  // nil (a callback with no meaningful return — Ruby yields the last expression,
  // usually nil) keeps processing, like Lua/Python/Perl/JS; only an explicit false
  // cancels. (RTEST(nil)==RTEST(false)==false, so nil must be special-cased.)
  if (result)
    {
    // "Keep processing?", and the MUSHclient rule is NOT Ruby truthiness: a
    // handler returning numeric 0 stops, exactly as it does in Lua, Python,
    // Perl, JavaScript and Tcl (and on the Windows COM path, where a returned 0
    // becomes VARIANT_FALSE).  Ruby is the only language here where 0 is truthy,
    // so RTEST alone made a Ruby plugin the only one that could not stop with a
    // 0 -- the form MUSHclient's own documentation tells people to use.
    // Returning nil still means "no explicit return", i.e. keep going.
    if      (r == Qnil)             *result = true;
    // FIXNUM and BIGNUM split, never combined through rb_num2ll: that RAISES
    // RangeError for an Integer outside long long range, and rb_protect has
    // already returned -- a tagless raise here is rb_bug -> abort, killing the
    // whole client on a handler whose last expression is e.g. `1 << 64` (the
    // same hazard ruby_error_message and ruby_to_s above are guarded against).
    // A T_BIGNUM can never be zero -- CRuby normalises anything in Fixnum range
    // to T_FIXNUM, so |Bignum| >= 2^62 -- which makes `true` exact, and both
    // arms allocation-free and raise-free (FIX2LONG is a tag-bit shift).
    else if (RB_TYPE_P (r, T_FIXNUM)) *result = (FIX2LONG (r) != 0);
    else if (RB_TYPE_P (r, T_BIGNUM)) *result = true;
    else if (RB_TYPE_P (r, T_FLOAT))*result = (rb_num2dbl (r) != 0.0);
    else                            *result = RTEST (r);
    }
  // Whether the handler answered at all: nil is what a Ruby method with no
  // meaningful return yields, and `result` cannot tell it from `return true`.
  // docs/UPSTREAM-BUGS.md 90.
  if (pbReturnedAValue)
    *pbReturnedAValue = r != Qnil;
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
  if (!script_defined (m_rubyEngine->obj, mid)) { dispid = DISPID_UNKNOWN; return true; }

  // Length-explicit: rb_str_new_cstr stops at the first NUL, and this overload
  // carries whole MUD packets, which contain them.
  VALUE arg = rb_str_new ((LPCTSTR) strParam, (long) strParam.GetLength ());
  TraceCallback (szType, szProcedure);
  if (iReason != eDontChangeAction) m_pDoc->m_iCurrentActionSource = iReason;
  CallCtx ctx { m_rubyEngine->obj, mid, 1, &arg };
  int state = 0;
  VALUE r = rb_protect (do_funcall, (VALUE) &ctx, &state);
  if (iReason != eDontChangeAction) m_pDoc->m_iCurrentActionSource = eUnknownActionSource;

  if (state) { dispid = DISPID_UNKNOWN; report_error (m_pDoc, szType, szProcedure); return true; }
  nInvocationCount++;
  // A String or a Numeric only: a Hash or an Array #to_s's to "{...}" and must
  // not become the MUD packet.  Lua gates on lua_isstring for the same reason.
  if (RTEST (r) && (RB_TYPE_P (r, T_STRING) || RB_TYPE_P (r, T_FIXNUM) ||
                    RB_TYPE_P (r, T_BIGNUM) || RB_TYPE_P (r, T_FLOAT)))
    {
    int st = 0;
    VALUE s = rb_protect (ruby_to_s, r, &st);
    if (st)
      rb_set_errinfo (Qnil);                          // #to_s raised — leave result empty
    else if (TYPE (s) == T_STRING)
      result = CString (RSTRING_PTR (s), (int) RSTRING_LEN (s));  // length-explicit: tolerates embedded NUL
    }
  return false;
  }

#endif // !_WIN32 && MUSHCLIENT_FULL_BUILD && MUSHCLIENT_RUBY_SCRIPTING
