// platform/mc_dispatch.cpp — portable IDispatch::Invoke over CMUSHclientDoc.
// See platform/mc_dispatch.h for the design rationale.

#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "platform/mc_dispatch.h"

#if !defined(_WIN32) && defined(MUSHCLIENT_FULL_BUILD)

#include <unordered_map>
#include <set>
#include <functional>
#include <string>
#include <vector>
#include <utility>
#include <type_traits>
#include <cstdlib>
#include <cstdio>    // snprintf — MC_DispatchBadArgsMessage()
#include <strings.h>   // strcasecmp — bool_word()

// COM constants not in the port's VT_* set.
#ifndef VT_ERROR
#define VT_ERROR 10                // "parameter not supplied" marker
#endif
#ifndef VT_DISPATCH
#define VT_DISPATCH 9              // an IDispatch* (another world object)
#endif

namespace {

// Automation names are case-insensitive on Windows (IDispatch::GetIDsOfNames),
// so real COM-language plugins call world.note / world.GetPluginName / NOTE
// interchangeably.  Key the tables — and every lookup — by the lower-cased name
// to match.  (ASCII only; all dispatch names are ASCII.)
std::string lc(const char* s)
  {
  std::string r(s ? s : "");
  for (char& c : r) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
  return r;
  }

// ── VARIANT teardown / copy ────────────────────────────────────────────────
// The stock VariantClear (mfc_ui.h) only frees SAFEARRAYs, so dispatch results
// (which may own a BSTR the method allocated) leak through it.  Route all
// VARIANT teardown here.
void var_clear(VARIANT& v)
  {
  if (v.vt == VT_BSTR && v.bstrVal)
    SysFreeString(v.bstrVal);
  else if ((v.vt & VT_ARRAY) && v.parray)
    SafeArrayDestroy(v.parray);
  v.vt = VT_EMPTY;
  v.lVal = 0;
  }

void var_copy(VARIANT& dst, const VARIANT& src)
  {
  var_clear(dst);
  dst = src;                       // shallow copy of the scalar/struct fields
  if (src.vt == VT_BSTR)           // deep-copy the one owned heap resource we marshal
    dst.bstrVal = src.bstrVal ? SysAllocString(src.bstrVal) : nullptr;
  else if (src.vt & 0x2000)        // VT_ARRAY: there is no SafeArrayCopy in the shim, so
    {                              // do NOT share src's SAFEARRAY — that would let both
    dst.parray = nullptr;          // src and dst SafeArrayDestroy() the same pointer
    dst.vt = VT_EMPTY;             // (double free).  Drop it instead (no caller deep-copies
    }                              // arrays today; the array getters marshal directly).
  }

// Boolean words for string→number coercion.  Tcl's everything-is-a-string
// model marshals flags as "true"/"yes"/"on", which atol read as 0 — so the
// idiomatic `world EnableTrigger t true` silently DISABLED the trigger, and
// `world Trace true` turned tracing off.  Full words only, case-insensitive
// (Tcl itself accepts prefixes, but being stricter here can only leave the
// old zero, never invert the other way).  This runs only when a STRING meets
// a numeric/BOOL parameter — string parameters never consult it, so
// world.Note("on") still prints "on".
static bool bool_word (const char* s, long& out)
  {
  if (!s) return false;
  static const struct { const char* w; long v; } k[] = {
    { "true", 1 }, { "yes", 1 }, { "on", 1 },
    { "false", 0 }, { "no", 0 }, { "off", 0 } };
  for (const auto& e : k)
    if (strcasecmp (s, e.w) == 0) { out = e.v; return true; }
  return false;
  }

// ── VARIANT readers (input arg -> C++ scalar/string) ───────────────────────
long var_to_long(const VARIANT* a)
  {
  if (!a) return 0;
  switch (a->vt)
    {
    case VT_I2:   return a->iVal;
    case VT_I4:   return a->lVal;
    case VT_R4:   return (long) a->fltVal;
    case VT_R8:   return (long) a->dblVal;
    case VT_BOOL: return a->boolVal ? 1 : 0;
    case VT_BSTR: { long b;
                    if (bool_word (a->bstrVal, b)) return b;
                    return a->bstrVal ? atol (a->bstrVal) : 0; }
    default:      return 0;
    }
  }

double var_to_double(const VARIANT* a)
  {
  if (!a) return 0.0;
  switch (a->vt)
    {
    case VT_I2:   return a->iVal;
    case VT_I4:   return a->lVal;
    case VT_R4:   return a->fltVal;
    case VT_R8:   return a->dblVal;
    case VT_BOOL: return a->boolVal ? 1 : 0;
    case VT_BSTR: { long b;
                    if (bool_word (a->bstrVal, b)) return b;
                    return a->bstrVal ? atof (a->bstrVal) : 0.0; }
    default:      return 0.0;
    }
  }

CString var_to_cstr(const VARIANT* a)
  {
  CString s;
  if (!a) return s;
  switch (a->vt)
    {
    case VT_BSTR: if (a->bstrVal) s = a->bstrVal;            break;
    case VT_I2:   s.Format ("%d",  (int) a->iVal);          break;
    case VT_I4:   s.Format ("%ld", a->lVal);                break;
    // %.7g / %.14g, NOT %g: %g keeps only 6 significant digits, so a script
    // passing 1234567.89 into a string parameter (SetVariable's value, Note
    // text...) stored "1.23457e+06" — and every JS integer >= 2^31 rides the
    // VT_R8 path.  %.14g is the Lua control's own format (LUA_NUMBER_FMT),
    // so the six languages now agree byte-for-byte; the R4 arm stays at
    // float precision so a widened 2.6f does not print representation noise.
    case VT_R4:   s.Format ("%.7g",  (double) a->fltVal);   break;
    case VT_R8:   s.Format ("%.14g", a->dblVal);            break;
    case VT_BOOL: s = a->boolVal ? "1" : "0";               break;
    default:                                                break;
    }
  return s;
  }

// ── to_variant (C++ return value -> VARIANT) ────────────────────────────────
// `r` arrives VT_EMPTY.  For BSTR/VARIANT returns we take ownership of the
// method's freshly-allocated resource (MFC automation convention) — no copy.
void to_variant(VARIANT& r, long v)   { r.vt = VT_I4;   r.lVal    = v; }
void to_variant(VARIANT& r, int v)    { r.vt = VT_BOOL; r.boolVal = v ? 1 : 0; } // BOOL
void to_variant(VARIANT& r, short v)  { r.vt = VT_I2;   r.iVal    = v; }
void to_variant(VARIANT& r, double v) { r.vt = VT_R8;   r.dblVal  = v; }
void to_variant(VARIANT& r, char* v)  { r.vt = VT_BSTR; r.bstrVal = v ? v : SysAllocString (""); } // BSTR
void to_variant(VARIANT& r, void* v)  { r.vt = VT_DISPATCH; r.byref = v; }       // LPDISPATCH
void to_variant(VARIANT& r, VARIANT v){ r = v; }                                 // take ownership (shallow)
void to_variant(VARIANT& r, unsigned short v) { r.vt = VT_I2; r.iVal = (short) v; }
void to_variant(VARIANT& r, const CString& v) { r.vt = VT_BSTR; r.bstrVal = SysAllocString((LPCTSTR) v); }

// VARIANT -> C++ value for property setters / member assignment.
template<class T> T prop_from(const VARIANT* v);
template<> long           prop_from<long>          (const VARIANT* v) { return var_to_long(v); }
template<> short          prop_from<short>         (const VARIANT* v) { return (short) var_to_long(v); }
template<> int            prop_from<int>           (const VARIANT* v) { return (int) var_to_long(v); }   // BOOL
template<> unsigned short prop_from<unsigned short>(const VARIANT* v) { return (unsigned short) var_to_long(v); }
template<> double         prop_from<double>        (const VARIANT* v) { return var_to_double(v); }
template<> CString        prop_from<CString>       (const VARIANT* v) { return var_to_cstr(v); }

// ── Argument holders (input VARIANT -> typed call argument) ─────────────────
// Each holder owns whatever storage its argument needs and lives in the call's
// argument tuple for the duration of the (doc->*pmf)(...) full-expression, so
// pointers/references it hands out stay valid through the call.  finalize()
// runs after the call to ship [out] VARIANT* parameters back to the caller.
template<class T> struct Holder;

template<> struct Holder<long>   { long v;   Holder(const VARIANT* a):v(var_to_long(a)){}            long   get(){return v;} void finalize(std::vector<VARIANT>*){} };
template<> struct Holder<short>  { short v;  Holder(const VARIANT* a):v((short)var_to_long(a)){}     short  get(){return v;} void finalize(std::vector<VARIANT>*){} };
template<> struct Holder<int>    { int v;    Holder(const VARIANT* a):v((int)var_to_long(a)){}       int    get(){return v;} void finalize(std::vector<VARIANT>*){} };  // BOOL
template<> struct Holder<double> { double v; Holder(const VARIANT* a):v(var_to_double(a)){}          double get(){return v;} void finalize(std::vector<VARIANT>*){} };
template<> struct Holder<float>  { float v;  Holder(const VARIANT* a):v((float)var_to_double(a)){}   float  get(){return v;} void finalize(std::vector<VARIANT>*){} };

template<> struct Holder<const char*>
  {
  CString buf;
  Holder(const VARIANT* a) : buf(var_to_cstr(a)) {}
  const char* get() { return (LPCTSTR) buf; }
  void finalize(std::vector<VARIANT>*) {}
  };

template<> struct Holder<CString>
  {
  CString v;
  Holder(const VARIANT* a) : v(var_to_cstr(a)) {}
  CString& get() { return v; }
  void finalize(std::vector<VARIANT>*) {}
  };

// [out] by-ref parameter (VARIANT FAR*): the method writes its result into the
// owned VARIANT; finalize() hands a copy to the caller's outargs vector.
template<> struct Holder<VARIANT*>
  {
  VARIANT out;
  Holder(const VARIANT* a) { VariantInit(&out); if (a) var_copy(out, *a); }
  VARIANT* get() { return &out; }
  void finalize(std::vector<VARIANT>* o)
    {
    if (o) { o->push_back(out); out.vt = VT_EMPTY; out.lVal = 0; }  // transfer ownership
    else   var_clear(out);
    }
  };

// By-value VARIANT parameter (optional args).  Copies in; frees on teardown.
template<> struct Holder<VARIANT>
  {
  VARIANT v;
  Holder(const VARIANT* a) { VariantInit(&v); if (a) var_copy(v, *a); }
  VARIANT get() { VARIANT c; VariantInit(&c); var_copy(c, v); return c; }  // method gets its own copy
  void finalize(std::vector<VARIANT>*) { var_clear(v); }
  };

// ── The argument the method sees for slot i (missing -> "not supplied") ─────
const VARIANT* arg_at(const VARIANT* args, int n, int i)
  {
  static VARIANT missing = []{ VARIANT m; m.vt = VT_ERROR; m.lVal = 0; return m; }();
  return (i < n) ? &args[i] : &missing;
  }

// ── The generated thunk: marshal args, call, marshal the return ─────────────
template<class Ret, class... Args, std::size_t... Is>
bool call_impl(CMUSHclientDoc* doc, Ret (CMUSHclientDoc::*pmf)(Args...),
               const VARIANT* args, int n, VARIANT& result,
               std::vector<VARIANT>* outargs, std::index_sequence<Is...>)
  {
  std::tuple<Holder<std::decay_t<Args>>...> h
    { Holder<std::decay_t<Args>>(arg_at(args, n, (int) Is))... };
  if constexpr (std::is_void_v<Ret>)
    {
    (doc->*pmf)(std::get<Is>(h).get()...);
    result.vt = VT_EMPTY;
    }
  else
    {
    Ret rv = (doc->*pmf)(std::get<Is>(h).get()...);
    to_variant(result, rv);
    }
  (std::get<Is>(h).finalize(outargs), ...);   // ship [out] params
  return true;
  }

// ── The dispatch table ──────────────────────────────────────────────────────
// Each entry carries beside its thunk the count of parameters the SCRIPT is
// expected to supply, deduced from &CMUSHclientDoc::method rather than from a
// second list that could drift.
//
// That is NOT always sizeof...(Args).  GetTrigger, GetAlias and GetTimer
// declare trailing [out] VARIANT* parameters (VTS_PVARIANT), and on Windows a
// COM script really does pass by-ref variables for them.  Off Windows it does
// not: every engine synthesises those slots itself and hands the values back as
// a tuple/list (py_engine.cpp builds exactly that), so world.GetTrigger("t") is
// a COMPLETE call here with one argument.  Counting the declared arity instead
// would make strict checking reject all three outright.
template<class T> constexpr bool is_out_param   = false;
template<>       constexpr bool is_out_param<VARIANT*> = true;

using Thunk = std::function<bool(CMUSHclientDoc*, const VARIANT*, int,
                                 VARIANT&, std::vector<VARIANT>*)>;
struct Entry { Thunk fn; int inArity; };
std::unordered_map<std::string, Entry>& table()
  {
  static std::unordered_map<std::string, Entry> t;
  return t;
  }

template<class Ret, class... Args>
void register_disp(const char* name, Ret (CMUSHclientDoc::*pmf)(Args...))
  {
  table()[lc(name)] = Entry {
    [pmf](CMUSHclientDoc* doc, const VARIANT* args, int n,
          VARIANT& result, std::vector<VARIANT>* outargs)
      {
      return call_impl(doc, pmf, args, n, result, outargs,
                       std::index_sequence_for<Args...>{});
      },
    (int) (0 + ... + (is_out_param<std::decay_t<Args>> ? 0 : 1)) };
  }

// ── Property table (DISP_PROPERTY_NOTIFY / DISP_PROPERTY_EX) ─────────────────
struct PropEntry
  {
  std::function<void(CMUSHclientDoc*, VARIANT&)>       get;
  std::function<void(CMUSHclientDoc*, const VARIANT&)> set;
  };
std::unordered_map<std::string, PropEntry>& prop_table()
  {
  static std::unordered_map<std::string, PropEntry> t;
  return t;
  }

// Member-backed property; `notify` (may be null) is the on-change handler.
template<class M>
void register_prop_member(const char* name, M CMUSHclientDoc::*pm,
                          void (CMUSHclientDoc::*notify)())
  {
  prop_table()[lc(name)] = PropEntry{
    [pm](CMUSHclientDoc* d, VARIANT& out)        { to_variant(out, d->*pm); },
    [pm, notify](CMUSHclientDoc* d, const VARIANT& v)
      { d->*pm = prop_from<M>(&v); if (notify) (d->*notify)(); }
    };
  }

// Accessor-backed property: R Get(), void Set(A).
template<class R, class A>
void register_prop_ex(const char* name, R (CMUSHclientDoc::*get)(),
                      void (CMUSHclientDoc::*set)(A))
  {
  prop_table()[lc(name)] = PropEntry{
    [get](CMUSHclientDoc* d, VARIANT& out)        { to_variant(out, (d->*get)()); },
    [set](CMUSHclientDoc* d, const VARIANT& v)    { (d->*set)(prop_from<std::decay_t<A>>(&v)); }
    };
  }

// Build the table from the same dispatch map the Windows build compiles.
// DISP_FUNCTION rows become callable methods (table()); DISP_PROPERTY_* rows
// become readable/writable properties (prop_table(), via register_prop_member/
// register_prop_ex below) — both are live and used by all three engines.
void build_table()
  {
  if (!table().empty()) return;

  #undef  DISP_FUNCTION
  #undef  DISP_FUNCTION_ID
  #undef  DISP_PROPERTY
  #undef  DISP_PROPERTY_NOTIFY
  #undef  DISP_PROPERTY_EX
  #undef  DISP_PROPERTY_PARAM
  #undef  DISP_DEFVALUE
  // Note() and ColourNote() each have a port-side convenience overload taking
  // const CString&, so the bare &CMUSHclientDoc::fn the generic sweep forms is
  // ambiguous.  Shadow those two identifiers to an unambiguous stand-in
  // (GetUniqueNumber — long(), supported) so the sweep compiles; the correct
  // LPCTSTR-overload bindings are installed afterwards, overwriting the stand-in
  // entries the sweep left under the "Note"/"ColourNote" keys.
  #define Note        GetUniqueNumber
  #define ColourNote  GetUniqueNumber
  #define DISP_FUNCTION(cls, name, fn, ret, ...)         register_disp(name, &cls::fn);
  #define DISP_FUNCTION_ID(cls, name, id, fn, ret, ...)  register_disp(name, &cls::fn);
  #define DISP_PROPERTY(cls, name, mem, type)            register_prop_member(name, &cls::mem, nullptr);
  #define DISP_PROPERTY_NOTIFY(cls, name, mem, fn, type) register_prop_member(name, &cls::mem, &cls::fn);
  #define DISP_PROPERTY_EX(cls, name, get, set, type)    register_prop_ex(name, &cls::get, &cls::set);
  // Indexed properties have no plain-attribute form; expose their accessors as
  // the documented Get*/Set* methods instead (e.g. world.GetNormalColour(i)).
  #define DISP_PROPERTY_PARAM(cls, name, get, set, type, ...) \
            register_disp(#get, &cls::get); register_disp(#set, &cls::set);
  #define DISP_DEFVALUE(cls, name)

  #include "doc_dispatch_map.inc"

  #undef Note
  #undef ColourNote

  register_disp("Note",
                static_cast<void (CMUSHclientDoc::*)(LPCTSTR)>(&CMUSHclientDoc::Note));
  register_disp("ColourNote",
                static_cast<void (CMUSHclientDoc::*)(LPCTSTR, LPCTSTR, LPCTSTR)>(&CMUSHclientDoc::ColourNote));
  }

} // anonymous namespace

// ── Public API ──────────────────────────────────────────────────────────────
// ── Trailing-argument defaults, mirroring the Lua binding ───────────────────
// A short argument list fills every missing typed parameter with the zero
// value (the VT_ERROR sentinel through var_to_long) — which, for the methods
// whose Lua binding documents a NON-ZERO default, inverted the call:
// world.EnableTrigger("t") from Python/Perl/JS/Tcl/Ruby disabled the trigger,
// WindowShow("w") hid the window, Pause() un-paused, DatabaseOpen(n, f) passed
// sqlite flags 0 so the open failed.  (On Windows these engines rode COM,
// which refuses a short list for typed parameters outright — so no working
// script ever relied on the zero-fill.)  This table is the Lua opt* defaults
// exactly, slot numbers 0-based; methods not listed keep the zero-fill that
// McDispatch.MissingArgumentsAreTheZeroValueNotGarbage pins.  Method names
// LOWERCASE (compared against lc(name)).
//
// Only METHODS belong here, and only slots the method actually has.  A
// PROPERTY (world.Trace = 1) travels MC_DispatchSetProperty, which never
// consults this table, so rows named for the property setters -- setechoinput,
// setloginput, setlognotes, setlogoutput, setmapping, settrace -- could never
// fire and are gone; and MoveWorldWindow / GetWorldWindowPosition do not take
// the Which parameter at all (they hand 1 to the X variant themselves), so the
// window rows name MoveWorldWindowX / GetWorldWindowPositionX, which do.
struct ArgDefault { const char* method; int slot; long value; };
static const ArgDefault kArgDefaults[] = {
  { "addtriggerex",          9, DEFAULT_TRIGGER_SEQUENCE },
  { "blendpixel",            3, 1 },                    // Opacity
  { "bookmark",              1, 1 },
  { "chatacceptcalls",       0, DEFAULT_CHAT_PORT },
  { "chatcall",              1, DEFAULT_CHAT_PORT },
  { "chatcallzchat",         1, DEFAULT_CHAT_PORT },
  { "databaseopen",          2, 2 | 4 },                // SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE
  { "enablealias",           1, 1 },
  { "enablealiasgroup",      1, 1 },
  { "enablegroup",           1, 1 },
  { "enablemapping",         0, 1 },
  { "enableplugin",          1, 1 },
  { "enabletimer",           1, 1 },
  { "enabletimergroup",      1, 1 },
  { "enabletrigger",         1, 1 },
  { "enabletriggergroup",    1, 1 },
  { "getworldwindowpositionx",0, 1 },
  { "metaphone",             1, 4 },
  { "moveworldwindowx",      4, 1 },
  { "notepadreadonly",       1, 1 },
  { "pause",                 0, 1 },
  { "queue",                 1, 1 },
  { "replace",               3, 1 },
  { "setchanged",            0, 1 },
  { "setscroll",             1, 1 },
  { "showinfobar",           0, 1 },
  { "windowblendimage",      7, 1 },
  { "windowdrawimage",       6, 1 },
  { "windowdrawimagealpha",  6, 1 },
  { "windowfont",            8, DEFAULT_CHARSET },
  { "windowmergeimagealpha", 8, 1 },
  { "windowshow",            1, 1 },
};

// Does kArgDefaults supply slot `slot` of method `key` (already lower-cased)?
static bool slot_has_default (const std::string& key, const int slot)
  {
  for (const ArgDefault& d : kArgDefaults)
    if (d.slot == slot && key == d.method)
      return true;
  return false;
  }

// How many leading arguments a caller must actually supply: the declared arity
// less the run of TRAILING slots kArgDefaults fills in.  The walk stops at the
// first undefaulted slot, so a defaulted slot with an undefaulted one after it
// (a gap) does not shorten the count — which makes this exactly equivalent to
// "is there a slot the caller omitted that nothing can fill?", the question
// strict checking actually asks.
static int required_args (const std::string& key, const int arity)
  {
  int required = arity;
  while (required > 0 && slot_has_default (key, required - 1))
    required--;
  return required;
  }

int MC_DispatchInvoke(CMUSHclientDoc* doc, const char* name,
                      const VARIANT* args, int nargs,
                      VARIANT& result, std::vector<VARIANT>* outargs,
                      unsigned flags)
  {
  build_table();
  VariantInit(&result);
  const std::string key = lc(name);
  auto it = table().find(key);
  if (it == table().end())
    return MC_DISP_UNKNOWN;

  // Pad a short list up to the highest defaulted slot.  The padded vector
  // holds shallow VIEWS of the caller's VARIANTs (the thunk only reads them);
  // filler slots without a default carry the same VT_ERROR sentinel arg_at
  // hands out, so unlisted parameters keep their zero-fill semantics.
  std::vector<VARIANT> padded;
  const int nSupplied = nargs;      // before the padding below rewrites it
  int maxSlot = -1;
  for (const ArgDefault& d : kArgDefaults)
    if (d.slot >= nargs && d.slot > maxSlot && key == d.method)
      maxSlot = d.slot;
  if (maxSlot >= 0)
    {
    padded.reserve((size_t) maxSlot + 1);
    for (int i = 0; i <= maxSlot; ++i)
      {
      VARIANT v;
      VariantInit(&v);
      if (i < nargs)
        v = args[i];                       // borrowed, not owned
      else
        {
        v.vt = VT_ERROR; v.lVal = 0;       // "not supplied"
        for (const ArgDefault& d : kArgDefaults)
          if (d.slot == i && key == d.method)
            { v.vt = VT_I4; v.lVal = d.value; break; }
        }
      padded.push_back(v);
      }
    args  = padded.data();
    nargs = maxSlot + 1;
    }

  // ── Strict argument checking (world option "script_strict_arguments") ────
  // Deliberately HERE, after the kArgDefaults padding above: a slot the table
  // supplies counts as supplied, so the ~32 Lua-parity defaults keep working
  // with strict on.  What is left is a parameter the caller never gave and
  // nothing can fill — precisely what the real IDispatch::Invoke the Windows
  // build rides rejects with DISP_E_BADPARAMCOUNT.  Arity only, never types:
  // the Tcl engine marshals every argument as VT_BSTR by design.
  //
  // nSupplied, not nargs: the padding rewrote nargs upward, and asking whether
  // the PADDED list is long enough would compare the fill against itself.
  if (doc && doc->m_bStrictScriptArguments &&
      !(flags & MC_DISP_F_INTROSPECT) &&
      nSupplied < required_args (key, it->second.inArity))
    return MC_DISP_BADARGS;

  if (!it->second.fn(doc, args, nargs, result, outargs))
    return MC_DISP_BADARGS;
  return MC_DISP_OK;
  }

int MC_DispatchRequiredArgs(const char* name)
  {
  build_table();
  const std::string key = lc(name);
  auto it = table().find(key);
  if (it == table().end())
    return -1;
  return required_args(key, it->second.inArity);
  }

std::string MC_DispatchBadArgsMessage(const char* name, int nargs)
  {
  const int need = MC_DispatchRequiredArgs(name);
  char buf[128];
  if (need < 0)
    snprintf(buf, sizeof buf, "bad arguments (%d supplied)", nargs);
  else
    // "at least": a longer list is not an error — extra arguments are ignored,
    // here and on Windows alike.
    snprintf(buf, sizeof buf, "expected at least %d argument%s, got %d",
             need, need == 1 ? "" : "s", nargs);
  return buf;
  }

// Live world documents, maintained by the doc's own constructor and destructor
// (doc_construct.cpp, non-Windows only).  NOT "is it in the doc template": a doc
// is usable before the framework registers it, and asking the template would
// reject the engine's own world.  A freed pointer can also be reused by a later
// allocation, so "known dead" would give false positives -- the set has to be
// membership, maintained at both ends.
namespace {
std::set<CMUSHclientDoc*>& live_docs()
  {
  static std::set<CMUSHclientDoc*> s;
  return s;
  }
}

void MC_DispatchDocRegister   (CMUSHclientDoc* doc) { if (doc) live_docs().insert (doc); }
void MC_DispatchDocUnregister (CMUSHclientDoc* doc) { if (doc) live_docs().erase  (doc); }

bool MC_DispatchDocIsLive(CMUSHclientDoc* doc)
  {
  return doc && live_docs().find (doc) != live_docs().end ();
  }

bool MC_DispatchHas(const char* name)
  {
  build_table();
  return table().find(lc(name)) != table().end();
  }

bool MC_DispatchHasProperty(const char* name)
  {
  build_table();
  return prop_table().find(lc(name)) != prop_table().end();
  }

bool MC_DispatchGetProperty(CMUSHclientDoc* doc, const char* name, VARIANT& result)
  {
  build_table();
  VariantInit(&result);
  auto it = prop_table().find(lc(name));
  if (it == prop_table().end() || !it->second.get) return false;
  it->second.get(doc, result);
  return true;
  }

bool MC_DispatchSetProperty(CMUSHclientDoc* doc, const char* name, const VARIANT& value)
  {
  build_table();
  auto it = prop_table().find(lc(name));
  if (it == prop_table().end() || !it->second.set) return false;
  it->second.set(doc, value);
  return true;
  }

void MC_VariantClear(VARIANT& v) { var_clear(v); }
void MC_VariantCopy(VARIANT& dst, const VARIANT& src) { var_copy(dst, src); }


// See the comment on the declaration.  The COleDateTime -> SYSTEMTIME -> CTime
// route is the one lua_methods.cpp takes; going straight from the OLE double
// would round differently.
long MC_VariantDateToUnix(const VARIANT& v)
{
    COleDateTime dt(v.date);
    SYSTEMTIME   sysTime;
    dt.GetAsSystemTime(sysTime);
    CTime        tm(sysTime);
    return (long) tm.GetTime();
}

#endif // !_WIN32 && MUSHCLIENT_FULL_BUILD
