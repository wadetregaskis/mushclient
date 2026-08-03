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
    case VT_BSTR: return a->bstrVal ? atol (a->bstrVal) : 0;
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
    case VT_BSTR: return a->bstrVal ? atof (a->bstrVal) : 0.0;
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
    case VT_R4:   s.Format ("%g",  (double) a->fltVal);     break;
    case VT_R8:   s.Format ("%g",  a->dblVal);              break;
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
using Thunk = std::function<bool(CMUSHclientDoc*, const VARIANT*, int,
                                 VARIANT&, std::vector<VARIANT>*)>;
std::unordered_map<std::string, Thunk>& table()
  {
  static std::unordered_map<std::string, Thunk> t;
  return t;
  }

template<class Ret, class... Args>
void register_disp(const char* name, Ret (CMUSHclientDoc::*pmf)(Args...))
  {
  table()[lc(name)] = [pmf](CMUSHclientDoc* doc, const VARIANT* args, int n,
                        VARIANT& result, std::vector<VARIANT>* outargs)
    {
    return call_impl(doc, pmf, args, n, result, outargs,
                     std::index_sequence_for<Args...>{});
    };
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
int MC_DispatchInvoke(CMUSHclientDoc* doc, const char* name,
                      const VARIANT* args, int nargs,
                      VARIANT& result, std::vector<VARIANT>* outargs)
  {
  build_table();
  VariantInit(&result);
  auto it = table().find(lc(name));
  if (it == table().end())
    return MC_DISP_UNKNOWN;
  if (!it->second(doc, args, nargs, result, outargs))
    return MC_DISP_BADARGS;
  return MC_DISP_OK;
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
