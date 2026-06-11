// platform/js_engine.cpp — embedded JavaScript engine for the port, via the
// macOS JavaScriptCore system framework (no bundled dependency).
//
// Windows' "JScript" exists only as a COM ActiveScripting engine with no
// embeddable form, so the port substitutes a modern JS engine.  As with the
// Python and Perl engines, the world object model is reached through the portable
// dispatch-map Invoke (platform/mc_dispatch.cpp): a `world` host object's
// property access returns either a bound method (callable) or a property value,
// routing into MC_DispatchInvoke / MC_DispatchGet|SetProperty.
//
// JavaScriptCore gives each JSGlobalContext fully isolated globals, so unlike the
// single-interpreter Python/Perl engines, each world/plugin simply gets its own
// context — no namespace partitioning needed.
//
// Built only on Apple platforms where the framework is found (MUSHCLIENT_JS_SCRIPTING).

#if !defined(_WIN32) && defined(MUSHCLIENT_FULL_BUILD) && defined(MUSHCLIENT_JS_SCRIPTING)

#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "scripting/scripting.h"
#include "platform/mc_dispatch.h"

// MUSHclient defines WIN32 (no underscore) for source compatibility, which makes
// JavaScriptCore's JS_EXPORT expand to dllimport __declspec.  The MFC headers
// above are already parsed, so undefining WIN32 here only affects the JSC block.
#ifdef WIN32
#undef WIN32
#endif

#include <JavaScriptCore/JavaScript.h>
#include <string>
#include <vector>
#include <cmath>

#ifndef VT_ERROR
#define VT_ERROR 10
#endif
#ifndef VT_DISPATCH
#define VT_DISPATCH 9
#endif

// Per-engine JS state (opaque; CScriptEngine holds it as CJSEngine*).
class CJSEngine
  {
  public:
  CMUSHclientDoc*      doc = nullptr;
  JSGlobalContextRef   ctx = nullptr;
  };

namespace {

JSClassRef g_worldClass  = nullptr;
JSClassRef g_methodClass = nullptr;

struct MethodData { CMUSHclientDoc* doc; std::string name; };

// ── string helpers ──────────────────────────────────────────────────────────
std::string jsstr_to_std(JSStringRef s)
  {
  if (!s) return std::string();
  size_t maxlen = JSStringGetMaximumUTF8CStringSize(s);
  std::string out(maxlen, '\0');
  size_t n = JSStringGetUTF8CString(s, &out[0], maxlen);
  if (n > 0) out.resize(n - 1);   // n includes the trailing NUL
  else out.clear();
  return out;
  }

JSValueRef make_world(JSContextRef ctx, CMUSHclientDoc* doc);

// ── JSValue <-> VARIANT marshalling ─────────────────────────────────────────
void js_to_variant(JSContextRef ctx, JSValueRef v, VARIANT& out)
  {
  VariantInit(&out);
  if (!v || JSValueIsUndefined(ctx, v) || JSValueIsNull(ctx, v)) { out.vt = VT_EMPTY; return; }
  if (JSValueIsBoolean(ctx, v)) { out.vt = VT_BOOL; out.boolVal = JSValueToBoolean(ctx, v) ? 1 : 0; return; }
  if (JSValueIsNumber(ctx, v))
    {
    double d = JSValueToNumber(ctx, v, nullptr);
    if (std::floor(d) == d && std::fabs(d) < 2147483647.0) { out.vt = VT_I4; out.lVal = (long) d; }
    else                                                   { out.vt = VT_R8; out.dblVal = d; }
    return;
    }
  // strings and everything else -> string
  JSStringRef js = JSValueToStringCopy(ctx, v, nullptr);
  std::string s = jsstr_to_std(js);
  if (js) JSStringRelease(js);
  out.vt = VT_BSTR; out.bstrVal = SysAllocString(s.c_str());
  }

JSValueRef variant_to_js(JSContextRef ctx, const VARIANT& v)
  {
  if (v.vt & VT_ARRAY)
    {
    SAFEARRAY* sa = v.parray;
    std::vector<JSValueRef> items;
    if (sa && sa->cDims >= 1)
      {
      long count = (long) sa->rgsabound[0].cElements;
      unsigned base = v.vt & ~VT_ARRAY;
      for (long i = 0; i < count; i++)
        {
        if (base == VT_BSTR)
          {
          BSTR* p = ((BSTR*) sa->pvData) + i;
          JSStringRef js = JSStringCreateWithUTF8CString((*p) ? *p : "");
          items.push_back(JSValueMakeString(ctx, js));
          JSStringRelease(js);
          }
        else
          {
          VARIANT* p = ((VARIANT*) sa->pvData) + i;
          items.push_back(variant_to_js(ctx, *p));
          }
        }
      }
    return JSObjectMakeArray(ctx, items.size(), items.empty() ? nullptr : items.data(), nullptr);
    }

  switch (v.vt)
    {
    case VT_EMPTY: case VT_NULL: case VT_ERROR: return JSValueMakeUndefined(ctx);
    case VT_I2:   return JSValueMakeNumber(ctx, v.iVal);
    case VT_I4:   return JSValueMakeNumber(ctx, v.lVal);
    case VT_R4:   return JSValueMakeNumber(ctx, v.fltVal);
    case VT_R8:   return JSValueMakeNumber(ctx, v.dblVal);
    case VT_BOOL: return JSValueMakeBoolean(ctx, v.boolVal != 0);
    case VT_BSTR:
      {
      JSStringRef js = JSStringCreateWithUTF8CString(v.bstrVal ? v.bstrVal : "");
      JSValueRef r = JSValueMakeString(ctx, js);
      JSStringRelease(js);
      return r;
      }
    case VT_DISPATCH: return v.byref ? make_world(ctx, (CMUSHclientDoc*) v.byref) : JSValueMakeUndefined(ctx);
    default:      return JSValueMakeUndefined(ctx);
    }
  }

JSValueRef make_js_error(JSContextRef ctx, const std::string& msg)
  {
  JSStringRef s = JSStringCreateWithUTF8CString(msg.c_str());
  JSValueRef a = JSValueMakeString(ctx, s);
  JSStringRelease(s);
  return JSObjectMakeError(ctx, 1, &a, nullptr);
  }

// ── world.<Method>(...) — the callable returned for a dispatch method ────────
JSValueRef Method_call(JSContextRef ctx, JSObjectRef function, JSObjectRef /*thisObj*/,
                       size_t argc, const JSValueRef argv[], JSValueRef* exception)
  {
  MethodData* m = (MethodData*) JSObjectGetPrivate(function);
  if (!m) return JSValueMakeUndefined(ctx);

  std::vector<VARIANT> vargs(argc);
  for (size_t i = 0; i < argc; i++) js_to_variant(ctx, argv[i], vargs[i]);

  VARIANT result; VariantInit(&result);
  std::vector<VARIANT> outargs;
  int rc = MC_DispatchInvoke(m->doc, m->name.c_str(),
                             argc ? vargs.data() : nullptr, (int) argc, result, &outargs);
  for (auto& a : vargs) MC_VariantClear(a);

  if (rc != MC_DISP_OK)
    {
    if (exception) *exception = make_js_error(ctx, "world." + m->name +
                                  (rc == MC_DISP_UNKNOWN ? ": unknown method" : ": bad arguments"));
    MC_VariantClear(result);
    return JSValueMakeUndefined(ctx);
    }

  JSValueRef ret;
  if (outargs.empty())
    ret = variant_to_js(ctx, result);
  else
    {
    std::vector<JSValueRef> tup;
    tup.push_back(variant_to_js(ctx, result));
    for (auto& o : outargs) { tup.push_back(variant_to_js(ctx, o)); MC_VariantClear(o); }
    ret = JSObjectMakeArray(ctx, tup.size(), tup.data(), nullptr);
    }
  MC_VariantClear(result);
  return ret;
  }

void Method_finalize(JSObjectRef function)
  {
  delete (MethodData*) JSObjectGetPrivate(function);
  }

// world.<name> -> bound method (callable) or property value.
JSValueRef World_getProperty(JSContextRef ctx, JSObjectRef object,
                             JSStringRef propertyName, JSValueRef* /*exception*/)
  {
  CMUSHclientDoc* doc = (CMUSHclientDoc*) JSObjectGetPrivate(object);
  std::string name = jsstr_to_std(propertyName);

  if (MC_DispatchHas(name.c_str()))
    return JSObjectMake(ctx, g_methodClass, new MethodData{ doc, name });

  if (MC_DispatchHasProperty(name.c_str()))
    {
    VARIANT v;
    if (MC_DispatchGetProperty(doc, name.c_str(), v))
      { JSValueRef r = variant_to_js(ctx, v); MC_VariantClear(v); return r; }
    }
  return nullptr;   // not ours — fall through to standard JS property lookup
  }

bool World_setProperty(JSContextRef ctx, JSObjectRef object, JSStringRef propertyName,
                       JSValueRef value, JSValueRef* /*exception*/)
  {
  CMUSHclientDoc* doc = (CMUSHclientDoc*) JSObjectGetPrivate(object);
  std::string name = jsstr_to_std(propertyName);
  if (MC_DispatchHasProperty(name.c_str()))
    {
    VARIANT v;
    js_to_variant(ctx, value, v);
    bool ok = MC_DispatchSetProperty(doc, name.c_str(), v);
    MC_VariantClear(v);
    return ok;
    }
  return false;     // not ours — let JS store it normally
  }

JSValueRef make_world(JSContextRef ctx, CMUSHclientDoc* doc)
  {
  return JSObjectMake(ctx, g_worldClass, doc);
  }

void ensure_classes()
  {
  if (g_worldClass) return;

  JSClassDefinition wd = kJSClassDefinitionEmpty;
  wd.className     = "World";
  wd.getProperty   = World_getProperty;
  wd.setProperty   = World_setProperty;
  g_worldClass = JSClassCreate(&wd);

  JSClassDefinition md = kJSClassDefinitionEmpty;
  md.className      = "WorldMethod";
  md.callAsFunction = Method_call;
  md.finalize       = Method_finalize;
  g_methodClass = JSClassCreate(&md);
  }

// Resolve a (possibly dotted) procedure name in the global object to a function.
JSObjectRef resolve(JSContextRef ctx, const char* dotted)
  {
  JSObjectRef obj = JSContextGetGlobalObject(ctx);
  std::string s(dotted ? dotted : "");
  size_t start = 0;
  for (;;)
    {
    size_t dot = s.find('.', start);
    std::string part = (dot == std::string::npos) ? s.substr(start) : s.substr(start, dot - start);
    JSStringRef key = JSStringCreateWithUTF8CString(part.c_str());
    JSValueRef val = JSObjectGetProperty(ctx, obj, key, nullptr);
    JSStringRelease(key);
    if (!JSValueIsObject(ctx, val)) return nullptr;
    obj = JSValueToObject(ctx, val, nullptr);
    if (dot == std::string::npos) break;
    start = dot + 1;
    }
  return (obj && JSObjectIsFunction(ctx, obj)) ? obj : nullptr;
  }

void report_error(CMUSHclientDoc* doc, JSContextRef ctx, JSValueRef exc,
                  const char* what, const char* proc)
  {
  if (!exc) return;
  JSStringRef js = JSValueToStringCopy(ctx, exc, nullptr);
  std::string msg = jsstr_to_std(js);
  if (js) JSStringRelease(js);
  if (doc)
    {
    CString hdr;
    hdr.Format("JavaScript error: %s%s%s\n", what ? what : "",
               (proc && *proc) ? " / " : "", proc ? proc : "");
    doc->ColourNote("white", "red", hdr + CString(msg.c_str()));
    }
  }

} // anonymous namespace

// ── CScriptEngine JavaScript methods ────────────────────────────────────────
void CScriptEngine::OpenJS ()
  {
  ensure_classes ();
  CJSEngine* e = new CJSEngine ();
  e->doc = m_pDoc;
  e->ctx = JSGlobalContextCreate (nullptr);
  JSObjectRef global = JSContextGetGlobalObject (e->ctx);
  JSValueRef world = make_world (e->ctx, m_pDoc);
  JSStringRef name = JSStringCreateWithUTF8CString ("world");
  JSObjectSetProperty (e->ctx, global, name, world, kJSPropertyAttributeDontDelete, nullptr);
  JSStringRelease (name);
  m_jsEngine = e;
  }

void CScriptEngine::CloseJS ()
  {
  if (!m_jsEngine) return;
  if (m_jsEngine->ctx) JSGlobalContextRelease (m_jsEngine->ctx);
  delete m_jsEngine;
  m_jsEngine = NULL;
  }

bool CScriptEngine::ParseJS (const CString & strCode, const CString & strWhat)
  {
  if (!m_jsEngine) return true;
  JSStringRef src = JSStringCreateWithUTF8CString ((LPCTSTR) strCode);
  JSValueRef exc = nullptr;
  JSEvaluateScript (m_jsEngine->ctx, src, nullptr, nullptr, 1, &exc);
  JSStringRelease (src);
  if (exc) { report_error (m_jsEngine->doc, m_jsEngine->ctx, exc, strWhat, ""); return true; }
  return false;
  }

DISPID CScriptEngine::GetJSDispid (const CString & strName)
  {
  if (!m_jsEngine || strName.IsEmpty ()) return DISPID_UNKNOWN;
  return resolve (m_jsEngine->ctx, (LPCTSTR) strName) ? 1 : DISPID_UNKNOWN;
  }

bool CScriptEngine::ExecuteJS (DISPID & dispid, LPCTSTR szProcedure,
                               const unsigned short iReason,
                               LPCTSTR szType, LPCTSTR szReason,
                               list<double> & nparams, list<string> & sparams,
                               long & nInvocationCount, const t_regexp * regexp,
                               map<string, string> * table, CPaneLine * paneline,
                               bool * result)
  {
  if (!m_jsEngine) return false;
  if (dispid == DISPID_UNKNOWN) return false;
  (void) paneline;                       // dropped for Windows-COM parity

  JSContextRef ctx = m_jsEngine->ctx;
  JSObjectRef fn = resolve (ctx, szProcedure);
  if (!fn) { dispid = DISPID_UNKNOWN; return true; }

  std::vector<JSValueRef> args;
  for (double d : nparams)
    args.push_back (JSValueMakeNumber (ctx, d));
  for (const string & s : sparams)
    { JSStringRef js = JSStringCreateWithUTF8CString (s.c_str ()); args.push_back (JSValueMakeString (ctx, js)); JSStringRelease (js); }

  if (regexp)                            // wildcards as a JS array [whole, $1, …]
    {
    int ncapt = 0;
    pcre_fullinfo (regexp->m_program, regexp->m_extra, PCRE_INFO_CAPTURECOUNT, &ncapt);
    std::vector<JSValueRef> wc;
    for (int i = 0; i <= ncapt; i++)
      { string w (regexp->GetWildcard (i)); JSStringRef js = JSStringCreateWithUTF8CString (w.c_str ());
        wc.push_back (JSValueMakeString (ctx, js)); JSStringRelease (js); }
    args.push_back (JSObjectMakeArray (ctx, wc.size (), wc.empty () ? nullptr : wc.data (), nullptr));
    }

  if (table)                             // as a JS object
    {
    JSObjectRef o = JSObjectMake (ctx, nullptr, nullptr);
    for (const auto & kv : *table)
      { JSStringRef k = JSStringCreateWithUTF8CString (kv.first.c_str ());
        JSStringRef vv = JSStringCreateWithUTF8CString (kv.second.c_str ());
        JSObjectSetProperty (ctx, o, k, JSValueMakeString (ctx, vv), 0, nullptr);
        JSStringRelease (k); JSStringRelease (vv); }
    args.push_back (o);
    }

  if (iReason != eDontChangeAction)
    m_pDoc->m_iCurrentActionSource = iReason;
  JSValueRef exc = nullptr;
  JSValueRef ret = JSObjectCallAsFunction (ctx, fn, nullptr, args.size (),
                                           args.empty () ? nullptr : args.data (), &exc);
  if (iReason != eDontChangeAction)
    m_pDoc->m_iCurrentActionSource = eUnknownActionSource;

  if (exc) { dispid = DISPID_UNKNOWN; report_error (m_pDoc, ctx, exc, szType, szProcedure); return true; }
  nInvocationCount++;
  if (result)
    *result = ret ? JSValueToBoolean (ctx, ret) : true;
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

  JSContextRef ctx = m_jsEngine->ctx;
  JSObjectRef fn = resolve (ctx, szProcedure);
  if (!fn) { dispid = DISPID_UNKNOWN; return true; }

  JSStringRef js = JSStringCreateWithUTF8CString ((LPCTSTR) strParam);
  JSValueRef arg = JSValueMakeString (ctx, js);
  JSStringRelease (js);

  if (iReason != eDontChangeAction)
    m_pDoc->m_iCurrentActionSource = iReason;
  JSValueRef exc = nullptr;
  JSValueRef ret = JSObjectCallAsFunction (ctx, fn, nullptr, 1, &arg, &exc);
  if (iReason != eDontChangeAction)
    m_pDoc->m_iCurrentActionSource = eUnknownActionSource;

  if (exc) { dispid = DISPID_UNKNOWN; report_error (m_pDoc, ctx, exc, szType, szProcedure); return true; }
  nInvocationCount++;
  if (ret && !JSValueIsUndefined (ctx, ret) && !JSValueIsNull (ctx, ret))
    {
    JSStringRef rs = JSValueToStringCopy (ctx, ret, nullptr);
    result = jsstr_to_std (rs).c_str ();
    if (rs) JSStringRelease (rs);
    }
  return false;
  }

#endif // !_WIN32 && MUSHCLIENT_FULL_BUILD && MUSHCLIENT_JS_SCRIPTING
