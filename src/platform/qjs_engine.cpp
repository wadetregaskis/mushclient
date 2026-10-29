// platform/qjs_engine.cpp — embedded JavaScript engine for 64-bit Windows, via
// QuickJS-ng (packaging\windows\fetch-deps.ps1 builds it static).
//
// The Windows twin of platform/js_engine.cpp, which does the same job on macOS
// and Linux over JavaScriptCore -- a framework Windows does not have.  The
// contract is that file's, point for point, so that a JavaScript plugin behaves
// the same on all three platforms:
//
//   * `world` is a host object whose property access returns either a callable
//     bound method or a property value, routed into MC_DispatchInvoke /
//     MC_DispatchGet|SetProperty (platform/mc_dispatch.cpp);
//   * a bare method reference coerced to a primitive (world.GetPluginID with no
//     parentheses, in a concatenation) invokes it with no arguments, as a COM
//     language would;
//   * text crosses as the document's BYTES, with undecodable bytes carried as
//     the lone surrogates U+DC80..U+DCFF (Python's "surrogateescape"), so a
//     Latin-1 line survives a round trip through a script;
//   * each world/plugin has its own global object (a QuickJS context; all of
//     them share one runtime, as all the Python worlds share one interpreter);
//   * callbacks return the same "keep processing?" answers.
//
// What JScript, the ActiveScript engine a 32-bit client ran these plugins on,
// adds beyond ECMAScript: only VBArray and Enumerator, the adapters JScript code
// wraps COM arrays and collections in.  Both are defined below as thin wrappers
// over the plain JavaScript arrays this engine returns, so the usual
// `new VBArray(world.GetTriggerList()).toArray()` keeps working.  ActiveXObject
// has no counterpart.

#if defined(_WIN32) && defined(MUSHCLIENT_JS_SCRIPTING)

#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "scripting/scripting.h"
#include "platform/mc_dispatch.h"
#include "platform/mc_variant_args.h"

#include <quickjs.h>
#include <string>
#include <vector>
#include <cmath>

// Per-engine JS state (opaque; CScriptEngine holds it as CJSEngine*).
class CJSEngine
  {
  public:
  CMUSHclientDoc* doc = nullptr;
  JSContext*      ctx = nullptr;
  };

namespace {

JSRuntime* g_rt            = nullptr;
JSClassID  g_worldClassId  = 0;
JSClassID  g_methodClassId = 0;
int        g_depth         = 0;      // nesting of host -> JavaScript entries

struct MethodData { CMUSHclientDoc* doc; std::string name; };

// ── byte-safe string conversion (surrogate escape, as js_engine.cpp) ────────
static void bytes_to_utf16_escaped (const char* p, size_t n, std::vector<uint16_t> & out)
  {
  const unsigned char* b = (const unsigned char*) p;
  for (size_t i = 0; i < n; )
    {
    unsigned char c = b[i];
    size_t extra = 0;
    unsigned int cp = 0;
    if      (c < 0x80)           { out.push_back ((uint16_t) c); i++; continue; }
    else if ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07; }
    else                         { out.push_back ((uint16_t) (0xDC00 | c)); i++; continue; }

    bool ok = (i + extra < n);
    for (size_t k = 1; ok && k <= extra; k++)
      {
      unsigned char cc = b[i + k];
      if ((cc & 0xC0) != 0x80) ok = false;
      else cp = (cp << 6) | (cc & 0x3F);
      }
    if (ok)
      {
      if      (extra == 1 && cp < 0x80)      ok = false;   // overlong
      else if (extra == 2 && cp < 0x800)     ok = false;
      else if (extra == 3 && cp < 0x10000)   ok = false;
      else if (cp > 0x10FFFF)                ok = false;
      else if (cp >= 0xD800 && cp <= 0xDFFF) ok = false;   // encoded surrogate
      }
    if (!ok)
      { out.push_back ((uint16_t) (0xDC00 | c)); i++; continue; }   // escape lead byte only

    if (cp < 0x10000)
      out.push_back ((uint16_t) cp);
    else
      {
      cp -= 0x10000;
      out.push_back ((uint16_t) (0xD800 | (cp >> 10)));
      out.push_back ((uint16_t) (0xDC00 | (cp & 0x3FF)));
      }
    i += extra + 1;
    }
  }

static void append_utf8 (std::string & out, unsigned int cp)
  {
  if (cp < 0x80)          out += (char) cp;
  else if (cp < 0x800)  { out += (char) (0xC0 | (cp >> 6));  out += (char) (0x80 | (cp & 0x3F)); }
  else if (cp < 0x10000){ out += (char) (0xE0 | (cp >> 12)); out += (char) (0x80 | ((cp >> 6) & 0x3F));
                          out += (char) (0x80 | (cp & 0x3F)); }
  else                  { out += (char) (0xF0 | (cp >> 18)); out += (char) (0x80 | ((cp >> 12) & 0x3F));
                          out += (char) (0x80 | ((cp >> 6) & 0x3F)); out += (char) (0x80 | (cp & 0x3F)); }
  }

// UTF-16 (with escaped bytes) -> the document's bytes.
static std::string utf16_to_bytes (const uint16_t* u, size_t n)
  {
  std::string out;
  out.reserve (n);
  for (size_t i = 0; i < n; i++)
    {
    unsigned int c = u[i];
    if (c >= 0xDC80 && c <= 0xDCFF)          // escaped byte, put it back
      { out += (char) (unsigned char) (c & 0xFF); continue; }
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n &&
        u[i + 1] >= 0xDC00 && u[i + 1] <= 0xDFFF)
      { append_utf8 (out, 0x10000 + (((c - 0xD800) << 10) | (u[i + 1] - 0xDC00))); i++; continue; }
    append_utf8 (out, c);
    }
  return out;
  }

// Source text for JS_Eval: UTF-8, where an escaped byte is its lone surrogate
// written as three bytes (QuickJS's decoder accepts those, so a Latin-1 byte
// inside a string literal arrives as the same U+DCxx a value would).
static std::string bytes_to_source (const char* p, size_t n)
  {
  std::vector<uint16_t> u16;
  u16.reserve (n);
  bytes_to_utf16_escaped (p, n, u16);
  std::string out;
  out.reserve (n);
  for (size_t i = 0; i < u16.size (); i++)
    {
    unsigned int c = u16[i];
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < u16.size () &&
        u16[i + 1] >= 0xDC00 && u16[i + 1] <= 0xDFFF)
      { append_utf8 (out, 0x10000 + (((c - 0xD800) << 10) | (u16[i + 1] - 0xDC00))); i++; continue; }
    append_utf8 (out, c);                    // a lone surrogate, too
    }
  return out;
  }

JSValue js_string_from_bytes (JSContext* ctx, const char* p, size_t n)
  {
  if (!p || n == 0) return JS_NewStringLen (ctx, "", 0);
  std::vector<uint16_t> u16;
  u16.reserve (n);
  bytes_to_utf16_escaped (p, n, u16);
  return JS_NewStringUTF16 (ctx, u16.data (), u16.size ());
  }

JSValue js_string_from_bytes (JSContext* ctx, const std::string & s)
  { return js_string_from_bytes (ctx, s.data (), s.size ()); }

// Any value -> the document's bytes, through ToString.
std::string js_to_bytes (JSContext* ctx, JSValueConst v)
  {
  size_t n = 0;
  const uint16_t* u = JS_ToCStringLenUTF16 (ctx, &n, v);
  if (!u) { JS_FreeValue (ctx, JS_GetException (ctx)); return std::string (); }
  std::string s = utf16_to_bytes (u, n);
  JS_FreeCStringUTF16 (ctx, u);
  return s;
  }

JSValue make_world (JSContext* ctx, CMUSHclientDoc* doc);

// ── JSValue <-> VARIANT marshalling ─────────────────────────────────────────
void js_to_variant (JSContext* ctx, JSValueConst v, VARIANT& out)
  {
  VariantInit (&out);
  if (JS_IsUndefined (v) || JS_IsNull (v)) { out.vt = VT_EMPTY; return; }
  // -1 is VARIANT_TRUE; see the boolean note in platform/mc_dispatch.cpp.
  if (JS_IsBool (v)) { out.vt = VT_BOOL; out.boolVal = JS_ToBool (ctx, v) ? -1 : 0; return; }
  if (JS_IsNumber (v))
    {
    double d = 0;
    JS_ToFloat64 (ctx, &d, v);
    if (std::floor (d) == d && std::fabs (d) < 2147483647.0) { out.vt = VT_I4; out.lVal = (long) d; }
    else                                                     { out.vt = VT_R8; out.dblVal = d; }
    return;
    }
  // strings and everything else -> string
  const std::string s = js_to_bytes (ctx, v);
  out.vt = VT_BSTR; out.bstrVal = MC_BstrFromBytes (s.c_str ());
  }

JSValue variant_to_js (JSContext* ctx, const VARIANT& v)
  {
  if (v.vt & VT_ARRAY)
    {
    SAFEARRAY* sa = v.parray;
    JSValue arr = JS_NewArray (ctx);
    if (sa && sa->cDims >= 1)
      {
      long count = (long) sa->rgsabound[0].cElements;
      unsigned base = v.vt & ~VT_ARRAY;
      for (long i = 0; i < count; i++)
        {
        JSValue item;
        if (base == VT_BSTR)
          item = js_string_from_bytes (ctx, MC_BstrBytes (((BSTR*) sa->pvData) [i]));
        else
          item = variant_to_js (ctx, ((VARIANT*) sa->pvData) [i]);
        JS_SetPropertyUint32 (ctx, arr, (uint32_t) i, item);   // takes ownership
        }
      }
    return arr;
    }

  switch (v.vt)
    {
    case VT_EMPTY: case VT_NULL: case VT_ERROR: return JS_UNDEFINED;
    case VT_I2:   return JS_NewFloat64 (ctx, v.iVal);
    case VT_I4:   return JS_NewFloat64 (ctx, v.lVal);
    case VT_R4:   return JS_NewFloat64 (ctx, v.fltVal);
    case VT_R8:   return JS_NewFloat64 (ctx, v.dblVal);
    case VT_BOOL: return JS_NewBool (ctx, v.boolVal != 0);
    case VT_BSTR: return js_string_from_bytes (ctx, MC_BstrBytes (v.bstrVal));
    case VT_DISPATCH: return v.byref ? make_world (ctx, (CMUSHclientDoc*) v.byref) : JS_UNDEFINED;
    case VT_DATE: return JS_NewFloat64 (ctx, (double) MC_VariantDateToUnix (v));
    default:      return JS_UNDEFINED;
    }
  }

JSValue throw_error (JSContext* ctx, const std::string& msg)
  {
  JSValue e = JS_NewError (ctx);
  JS_DefinePropertyValueStr (ctx, e, "message", js_string_from_bytes (ctx, msg),
                             JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
  return JS_Throw (ctx, e);
  }

// Invoke a world method on `m`'s document: the shared body of a call and of a
// coercion (MC_DISP_F_INTROSPECT).
JSValue invoke (JSContext* ctx, const MethodData* m, int argc, JSValueConst* argv, unsigned flags)
  {
  // The proxy holds a bare CMUSHclientDoc* (mc_dispatch.cpp drops the COM
  // reference GetWorld took), so a script that keeps what world.GetWorld()
  // returned and uses it after that world is closed must be told, not crash.
  if (!MC_DispatchDocIsLive (m->doc))
    return throw_error (ctx, "world." + m->name + ": that world has been closed");

  MCVariantArgs vargs ((size_t) argc);
  for (int i = 0; i < argc; i++) js_to_variant (ctx, argv[i], vargs[i]);

  VARIANT result; VariantInit (&result);
  std::vector<VARIANT> outargs;
  int rc = MC_DispatchInvoke (m->doc, m->name.c_str (),
                              argc ? vargs.data () : nullptr, argc, result,
                              (flags & MC_DISP_F_INTROSPECT) ? nullptr : &outargs, flags);
  for (auto& a : vargs) MC_VariantClear (a);

  if (rc != MC_DISP_OK)
    {
    MC_VariantClear (result);
    return throw_error (ctx, "world." + m->name + ": " +
                        (rc == MC_DISP_UNKNOWN ? std::string ("unknown method")
                         : MC_DispatchBadArgsMessage (m->name.c_str (), argc)));
    }

  JSValue ret;
  if (outargs.empty ())
    ret = variant_to_js (ctx, result);
  else                                       // [out] getters -> [result, out1, out2, ...]
    {
    ret = JS_NewArray (ctx);
    JS_SetPropertyUint32 (ctx, ret, 0, variant_to_js (ctx, result));
    for (size_t i = 0; i < outargs.size (); i++)
      { JS_SetPropertyUint32 (ctx, ret, (uint32_t) (i + 1), variant_to_js (ctx, outargs[i]));
        MC_VariantClear (outargs[i]); }
    }
  MC_VariantClear (result);
  return ret;
  }

// ── world.<Method>(...) — a callable object of the method class ─────────────
JSValue Method_call (JSContext* ctx, JSValueConst func_obj, JSValueConst /*this_val*/,
                     int argc, JSValueConst* argv, int /*flags*/)
  {
  const MethodData* m = (const MethodData*) JS_GetOpaque (func_obj, g_methodClassId);
  if (!m) return JS_UNDEFINED;
  return invoke (ctx, m, argc, argv, 0);
  }

void Method_finalize (JSRuntime* /*rt*/, JSValueConst val)
  {
  delete (MethodData*) JS_GetOpaque (val, g_methodClassId);
  }

// valueOf / toString on the method prototype: world.GetPluginName with no
// parentheses, in "x" + world.GetPluginName or a comparison, invokes it with
// no arguments -- the coercion JavaScriptCore's convertToType gives
// js_engine.cpp.  If the method cannot be called bare, the coercion gives what
// a function would.
JSValue Method_toPrimitive (JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
  {
  const MethodData* m = (const MethodData*) JS_GetOpaque (this_val, g_methodClassId);
  if (!m) return JS_NewString (ctx, "function () { [native code] }");
  JSValue r = invoke (ctx, m, 0, nullptr, MC_DISP_F_INTROSPECT);
  if (JS_IsException (r))
    {
    JS_FreeValue (ctx, JS_GetException (ctx));
    return JS_NewString (ctx, ("function " + m->name + "() { [native code] }").c_str ());
    }
  return r;
  }

// The name an atom spells, or false for a symbol (never a world member).
bool atom_name (JSContext* ctx, JSAtom atom, std::string& name)
  {
  JSValue v = JS_AtomToValue (ctx, atom);
  const bool isSymbol = JS_IsSymbol (v);
  JS_FreeValue (ctx, v);
  if (isSymbol) return false;
  const char* s = JS_AtomToCString (ctx, atom);
  if (!s) { JS_FreeValue (ctx, JS_GetException (ctx)); return false; }
  name = s;
  JS_FreeCString (ctx, s);
  return true;
  }

// ── the world object: exotic get / set / has ────────────────────────────────
// QuickJS consults these after the object's OWN properties, and instead of its
// prototype chain, so a name that is not a world member is looked up on the
// prototype here -- which keeps world.toString, world.hasOwnProperty and
// whatever a script itself stores on `world` behaving as on any object.
JSValue World_get (JSContext* ctx, JSValueConst obj, JSAtom atom, JSValueConst /*receiver*/)
  {
  CMUSHclientDoc* doc = (CMUSHclientDoc*) JS_GetOpaque (obj, g_worldClassId);
  std::string name;
  if (atom_name (ctx, atom, name))
    {
    if (MC_DispatchHas (name.c_str ()))
      {
      JSValue fn = JS_NewObjectClass (ctx, (int) g_methodClassId);
      if (JS_IsException (fn)) return fn;
      JS_SetOpaque (fn, new MethodData { doc, name });
      return fn;
      }
    if (MC_DispatchHasProperty (name.c_str ()))
      {
      // A property is read off the doc's members directly, so it needs the
      // stale-proxy check as much as a method call does.
      if (!MC_DispatchDocIsLive (doc))
        return throw_error (ctx, "world." + name + ": that world has been closed");
      VARIANT v;
      if (MC_DispatchGetProperty (doc, name.c_str (), v))
        { JSValue r = variant_to_js (ctx, v); MC_VariantClear (v); return r; }
      }
    }
  JSValue proto = JS_GetPrototype (ctx, obj);
  JSValue r = JS_IsObject (proto) ? JS_GetProperty (ctx, proto, atom) : JS_UNDEFINED;
  JS_FreeValue (ctx, proto);
  return r;
  }

int World_set (JSContext* ctx, JSValueConst obj, JSAtom atom, JSValueConst value,
               JSValueConst /*receiver*/, int /*flags*/)
  {
  CMUSHclientDoc* doc = (CMUSHclientDoc*) JS_GetOpaque (obj, g_worldClassId);
  std::string name;
  if (atom_name (ctx, atom, name) && MC_DispatchHasProperty (name.c_str ()))
    {
    if (!MC_DispatchDocIsLive (doc))
      { throw_error (ctx, "world." + name + ": that world has been closed"); return -1; }
    VARIANT v;
    js_to_variant (ctx, value, v);
    const bool ok = MC_DispatchSetProperty (doc, name.c_str (), v);
    MC_VariantClear (v);
    return ok ? 1 : 0;
    }
  // Not ours: an ordinary own property, which the lookup above then finds first.
  return JS_DefinePropertyValue (ctx, obj, atom, JS_DupValue (ctx, value), JS_PROP_C_W_E);
  }

int World_has (JSContext* ctx, JSValueConst obj, JSAtom atom)
  {
  std::string name;
  if (atom_name (ctx, atom, name) &&
      (MC_DispatchHas (name.c_str ()) || MC_DispatchHasProperty (name.c_str ())))
    return 1;
  JSValue proto = JS_GetPrototype (ctx, obj);
  const int r = JS_IsObject (proto) ? JS_HasProperty (ctx, proto, atom) : 0;
  JS_FreeValue (ctx, proto);
  return r;
  }

JSValue make_world (JSContext* ctx, CMUSHclientDoc* doc)
  {
  JSValue w = JS_NewObjectClass (ctx, (int) g_worldClassId);
  if (!JS_IsException (w)) JS_SetOpaque (w, doc);
  return w;
  }

// The runtime and the two classes, once.  The stack budget: QuickJS measures
// it from the stack top recorded at the outermost entry (StackGuard below),
// and the UI thread has the default 1 MB, part of it already spent by the
// message loop beneath every callback -- so half of it, leaving room for the
// C++ frames of world calls between JavaScript ones.
void ensure_runtime ()
  {
  if (g_rt) return;
  g_rt = JS_NewRuntime ();
  JS_SetMaxStackSize (g_rt, 512 * 1024);

  static JSClassExoticMethods worldExotic = {};
  worldExotic.get_property = World_get;
  worldExotic.set_property = World_set;
  worldExotic.has_property = World_has;
  JSClassDef wd = {};
  wd.class_name = "World";
  wd.exotic     = &worldExotic;
  JS_NewClassID (g_rt, &g_worldClassId);
  JS_NewClass (g_rt, g_worldClassId, &wd);

  JSClassDef md = {};
  md.class_name = "WorldMethod";
  md.finalizer  = Method_finalize;
  md.call       = Method_call;
  JS_NewClassID (g_rt, &g_methodClassId);
  JS_NewClass (g_rt, g_methodClassId, &md);
  }

// Every host -> JavaScript entry: record the stack top at the outermost one
// (so the budget above is measured from where JavaScript was entered, not
// from wherever the runtime happened to be created), and run the promise jobs
// a script queued once the outermost entry is done, as JavaScriptCore does
// after each evaluation.
struct StackGuard
  {
  JSContext* ctx;
  explicit StackGuard (JSContext* c) : ctx (c) { if (g_depth++ == 0) JS_UpdateStackTop (g_rt); }
  ~StackGuard ()
    {
    if (--g_depth == 0)
      {
      JSContext* jobCtx = nullptr;
      while (JS_IsJobPending (g_rt))
        if (JS_ExecutePendingJob (g_rt, &jobCtx) < 0 && jobCtx)
          JS_FreeValue (jobCtx, JS_GetException (jobCtx));
      }
    }
  };

// JScript's COM adapters, over plain arrays (see the head of this file).
const char kCompatPrelude[] =
  "(function (g) {\n"
  "  function VBArray (a) {\n"
  "    if (!(this instanceof VBArray)) return new VBArray (a);\n"
  "    this._lb = 0;\n"
  "    if (a != null && a.__mcWildcards) {\n"
  // A trigger's or alias's wildcards, as Windows' COM arm built them for JScript:
  // a 1-based SAFEARRAY of MAX_WILDCARDS (10), %1..%9 at 1..9 and the whole
  // matching line at 10 -- so toArray()[0] is %1, which is what every 5.x
  // JScript callback (exampscript.js among them) indexes.
  "      var w = [];\n"
  "      for (var i = 1; i <= 9; i++) w.push (a [i] === undefined ? '' : a [i]);\n"
  "      w.push (a [0] === undefined ? '' : a [0]);\n"
  "      this._a = w;\n"
  "      this._lb = 1;\n"
  "    } else\n"
  "      this._a = (a == null) ? [] : (a instanceof VBArray ? a._a.slice () : Array.from (a));\n"
  "    if (a instanceof VBArray) this._lb = a._lb;\n"
  "  }\n"
  "  VBArray.prototype.toArray    = function () { return this._a.slice (); };\n"
  "  VBArray.prototype.getItem    = function (i) { return this._a [i - this._lb]; };\n"
  "  VBArray.prototype.lbound     = function () { return this._lb; };\n"
  "  VBArray.prototype.ubound     = function () { return this._a.length - 1 + this._lb; };\n"
  "  VBArray.prototype.dimensions = function () { return 1; };\n"
  "  function Enumerator (c) {\n"
  "    if (!(this instanceof Enumerator)) return new Enumerator (c);\n"
  "    this._a = (c == null) ? [] : (c instanceof VBArray ? c._a.slice () : Array.from (c));\n"
  "    this._i = 0;\n"
  "  }\n"
  "  Enumerator.prototype.atEnd     = function () { return this._i >= this._a.length; };\n"
  "  Enumerator.prototype.item      = function () { return this._a [this._i]; };\n"
  "  Enumerator.prototype.moveFirst = function () { this._i = 0; };\n"
  "  Enumerator.prototype.moveNext  = function () { this._i++; };\n"
  "  g.VBArray = VBArray;\n"
  "  g.Enumerator = Enumerator;\n"
  "}) (globalThis);\n";

// Resolve a (possibly dotted) procedure name in the global object to a
// function; a new reference, or JS_UNDEFINED.
JSValue resolve (JSContext* ctx, const char* dotted)
  {
  JSValue obj = JS_GetGlobalObject (ctx);
  std::string s (dotted ? dotted : "");
  size_t start = 0;
  for (;;)
    {
    size_t dot = s.find ('.', start);
    std::string part = (dot == std::string::npos) ? s.substr (start) : s.substr (start, dot - start);
    JSValue val = JS_GetPropertyStr (ctx, obj, part.c_str ());
    JS_FreeValue (ctx, obj);
    if (JS_IsException (val)) { JS_FreeValue (ctx, JS_GetException (ctx)); return JS_UNDEFINED; }
    if (!JS_IsObject (val)) { JS_FreeValue (ctx, val); return JS_UNDEFINED; }
    obj = val;
    if (dot == std::string::npos) break;
    start = dot + 1;
    }
  if (JS_IsFunction (ctx, obj)) return obj;
  JS_FreeValue (ctx, obj);
  return JS_UNDEFINED;
  }

void report_error (CMUSHclientDoc* doc, JSContext* ctx, const char* what, const char* proc)
  {
  JSValue exc = JS_GetException (ctx);
  std::string msg = js_to_bytes (ctx, exc);
  // The stack, when there is one: QuickJS's messages carry no line number of
  // their own, and "at f (Plugin:12)" is where a plugin author looks first.
  if (JS_IsObject (exc))
    {
    JSValue stack = JS_GetPropertyStr (ctx, exc, "stack");
    if (JS_IsString (stack))
      {
      const std::string st = js_to_bytes (ctx, stack);
      if (!st.empty ()) msg += "\n" + st;
      }
    JS_FreeValue (ctx, stack);
    }
  JS_FreeValue (ctx, exc);
  // Through the shared policy (scripting/lua_scripting.cpp), exactly as
  // js_engine.cpp does: "Note errors to the output window", or the dialog.
  CString raisedBy = "No active world";
  if (doc)
    raisedBy = doc->m_CurrentPlugin
             ? TFormat ("Plugin: %s (called from world: %s)",
                        (LPCTSTR) doc->m_CurrentPlugin->m_strName, (LPCTSTR) doc->m_mush_name)
             : TFormat ("World: %s", (LPCTSTR) doc->m_mush_name);
  ReportScriptError (doc, "JavaScript error",
                     TFormat ("Function/Sub: %s called by %s", proc ? proc : "", what ? what : ""),
                     CString (msg.data (), (int) msg.size ()), raisedBy);
  }

// Whether a callback's return value is an answer at all (undefined / null is
// what a function with no explicit return yields).
bool returned_a_value (JSValueConst v) { return !JS_IsUndefined (v) && !JS_IsNull (v); }

} // anonymous namespace

// ── CScriptEngine JavaScript methods ────────────────────────────────────────
void CScriptEngine::OpenJS ()
  {
  ensure_runtime ();
  CJSEngine* e = new CJSEngine ();
  e->doc = m_pDoc;
  e->ctx = JS_NewContext (g_rt);

  // Per-context prototypes for the two classes.  The method prototype sits
  // on Function.prototype (so world.Note.call / .apply / .bind work) and
  // carries the bare-reference coercion.
  JSValue fproto = JS_GetFunctionProto (e->ctx);                // a new reference
  JSValue mproto = JS_NewObjectProto (e->ctx, fproto);
  JS_FreeValue (e->ctx, fproto);
  JS_SetPropertyStr (e->ctx, mproto, "valueOf",  JS_NewCFunction (e->ctx, Method_toPrimitive, "valueOf", 0));
  JS_SetPropertyStr (e->ctx, mproto, "toString", JS_NewCFunction (e->ctx, Method_toPrimitive, "toString", 0));
  JS_SetClassProto (e->ctx, g_methodClassId, mproto);          // takes ownership
  JS_SetClassProto (e->ctx, g_worldClassId, JS_NewObject (e->ctx));

  JSValue global = JS_GetGlobalObject (e->ctx);
  JS_DefinePropertyValueStr (e->ctx, global, "world", make_world (e->ctx, m_pDoc),
                             JS_PROP_WRITABLE | JS_PROP_ENUMERABLE);   // not configurable: DontDelete
  JS_FreeValue (e->ctx, global);

  {
  StackGuard guard (e->ctx);
  JSValue r = JS_Eval (e->ctx, kCompatPrelude, sizeof kCompatPrelude - 1, "<mushclient>", JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException (r)) JS_FreeValue (e->ctx, JS_GetException (e->ctx));
  JS_FreeValue (e->ctx, r);
  }
  m_jsEngine = e;
  }

void CScriptEngine::CloseJS ()
  {
  if (!m_jsEngine) return;
  if (m_jsEngine->ctx) JS_FreeContext (m_jsEngine->ctx);
  delete m_jsEngine;
  m_jsEngine = NULL;
  }

bool CScriptEngine::ParseJS (const CString & strCode, const CString & strWhat)
  {
  if (!m_jsEngine) return true;
  JSContext* ctx = m_jsEngine->ctx;
  const std::string src = bytes_to_source ((LPCTSTR) strCode, (size_t) strCode.GetLength ());
  const std::string file ((LPCTSTR) strWhat);
  StackGuard guard (ctx);
  JSValue r = JS_Eval (ctx, src.c_str (), src.size (), file.c_str (), JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException (r))
    { report_error (m_jsEngine->doc, ctx, strWhat, ""); return true; }
  JS_FreeValue (ctx, r);
  return false;
  }

DISPID CScriptEngine::GetJSDispid (const CString & strName)
  {
  if (!m_jsEngine || strName.IsEmpty ()) return DISPID_UNKNOWN;
  JSValue fn = resolve (m_jsEngine->ctx, (LPCTSTR) strName);
  const bool found = !JS_IsUndefined (fn);
  JS_FreeValue (m_jsEngine->ctx, fn);
  return found ? 1 : DISPID_UNKNOWN;
  }

bool CScriptEngine::ExecuteJS (DISPID & dispid, LPCTSTR szProcedure,
                               const unsigned short iReason,
                               LPCTSTR szType, LPCTSTR szReason,
                               list<double> & nparams, list<string> & sparams,
                               long & nInvocationCount, const t_regexp * regexp,
                               map<string, string> * table, CPaneLine * paneline,
                               bool * result, const bool bLowercaseWildcards,
                               bool * pbReturnedAValue)
  {
  if (!m_jsEngine) return false;
  if (dispid == DISPID_UNKNOWN) return false;
  (void) paneline;                       // dropped for Windows-COM parity
  (void) szReason;

  JSContext* ctx = m_jsEngine->ctx;
  StackGuard guard (ctx);
  JSValue fn = resolve (ctx, szProcedure);
  if (JS_IsUndefined (fn)) { dispid = DISPID_UNKNOWN; return true; }

  std::vector<JSValue> args;
  for (double d : nparams)
    args.push_back (JS_NewFloat64 (ctx, d));
  for (const string & s : sparams)
    args.push_back (js_string_from_bytes (ctx, s));

  if (regexp)                            // wildcards as a JS array [whole, $1, …]
    {
    vector<string> wildcards;
    regexp->GetCallbackWildcards (bLowercaseWildcards, wildcards);
    JSValue wc = JS_NewArray (ctx);
    for (size_t i = 0; i < wildcards.size (); i++)
      JS_SetPropertyUint32 (ctx, wc, (uint32_t) i, js_string_from_bytes (ctx, wildcards [i]));
    // marked, not enumerable, so VBArray (wildcards) can give a 5.x script the
    // shape it was written for (the compat prelude above)
    JS_DefinePropertyValueStr (ctx, wc, "__mcWildcards", JS_TRUE, 0);
    args.push_back (wc);
    }

  if (table)                             // as a JS object
    {
    JSValue o = JS_NewObject (ctx);
    for (const auto & kv : *table)
      {
      JSValue k = js_string_from_bytes (ctx, kv.first);
      JSAtom a = JS_ValueToAtom (ctx, k);
      JS_FreeValue (ctx, k);
      JS_DefinePropertyValue (ctx, o, a, js_string_from_bytes (ctx, kv.second), JS_PROP_C_W_E);
      JS_FreeAtom (ctx, a);
      }
    args.push_back (o);
    }

  TraceCallback (szType, szProcedure);
  if (iReason != eDontChangeAction)
    m_pDoc->m_iCurrentActionSource = iReason;
  JSValue ret = JS_Call (ctx, fn, JS_UNDEFINED, (int) args.size (), args.empty () ? nullptr : args.data ());
  if (iReason != eDontChangeAction)
    m_pDoc->m_iCurrentActionSource = eUnknownActionSource;
  for (JSValue & a : args) JS_FreeValue (ctx, a);
  JS_FreeValue (ctx, fn);

  if (JS_IsException (ret))
    { dispid = DISPID_UNKNOWN; report_error (m_pDoc, ctx, szType, szProcedure); return true; }
  nInvocationCount++;
  if (result)
    {
    // A function with no explicit return yields `undefined`, which on Windows /
    // Lua means "keep going": only a real returned value decides.
    *result = true;
    if (returned_a_value (ret))
      *result = JS_ToBool (ctx, ret) > 0;
    }
  if (pbReturnedAValue)
    *pbReturnedAValue = returned_a_value (ret);
  JS_FreeValue (ctx, ret);
  return false;
  }

bool CScriptEngine::ExecuteJS (DISPID & dispid, LPCTSTR szProcedure,
                               const unsigned short iReason,
                               LPCTSTR szType, LPCTSTR szReason,
                               CString strParam, long & nInvocationCount,
                               CString & result)
  {
  if (!m_jsEngine) return false;
  if (dispid == DISPID_UNKNOWN) return false;
  (void) szReason;

  JSContext* ctx = m_jsEngine->ctx;
  StackGuard guard (ctx);
  JSValue fn = resolve (ctx, szProcedure);
  if (JS_IsUndefined (fn)) { dispid = DISPID_UNKNOWN; return true; }

  JSValue arg = js_string_from_bytes (ctx, (LPCTSTR) strParam, (size_t) strParam.GetLength ());

  TraceCallback (szType, szProcedure);
  if (iReason != eDontChangeAction)
    m_pDoc->m_iCurrentActionSource = iReason;
  JSValue ret = JS_Call (ctx, fn, JS_UNDEFINED, 1, &arg);
  if (iReason != eDontChangeAction)
    m_pDoc->m_iCurrentActionSource = eUnknownActionSource;
  JS_FreeValue (ctx, arg);
  JS_FreeValue (ctx, fn);

  if (JS_IsException (ret))
    { dispid = DISPID_UNKNOWN; report_error (m_pDoc, ctx, szType, szProcedure); return true; }
  nInvocationCount++;
  // A string or a number only: an object or array stringifies to
  // "[object Object]" and must not become the MUD packet.  Lua gates on
  // lua_isstring for the same reason.  Length-explicit: this overload carries
  // whole packets, NUL bytes and all.
  if (JS_IsString (ret) || JS_IsNumber (ret))
    {
    const std::string bytes = js_to_bytes (ctx, ret);
    result = CString (bytes.data (), (int) bytes.size ());
    }
  JS_FreeValue (ctx, ret);
  return false;
  }

#endif // _WIN32 && MUSHCLIENT_JS_SCRIPTING
