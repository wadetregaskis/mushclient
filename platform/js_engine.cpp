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

// ── byte-safe string conversion (surrogate-escape, as py_engine.cpp does) ───
// MUD text is regularly not valid UTF-8: Latin-1 worlds, and raw bytes captured
// by a trigger wildcard.  JavaScriptCore only speaks UTF-16, and its two
// convenience converters both lose such data --
// JSStringCreateWithUTF8CString answers an EMPTY string for invalid input, and
// JSStringGetUTF8CString re-encodes on the way back -- so a Latin-1 line reached
// a JavaScript trigger as "" and could not survive a round trip.
//
// Use the same escape the Python engine uses: an undecodable byte b becomes the
// lone low surrogate U+DC00|b (so 0x80..0xFF map to U+DC80..U+DCFF, exactly
// Python's "surrogateescape"), and the reverse conversion turns those back into
// the original byte.  Everything else is ordinary UTF-8 <-> UTF-16, so valid
// text behaves exactly as before and the escape is invisible to scripts that
// only ever see well-formed input.
static void utf8_to_utf16_escaped (const char* p, size_t n, std::vector<JSChar> & out)
  {
  const unsigned char* b = (const unsigned char*) p;
  for (size_t i = 0; i < n; )
    {
    unsigned char c = b[i];
    size_t extra = 0;
    unsigned int cp = 0;
    if      (c < 0x80)           { out.push_back ((JSChar) c); i++; continue; }
    else if ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07; }
    else                         { out.push_back ((JSChar) (0xDC00 | c)); i++; continue; }

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
      { out.push_back ((JSChar) (0xDC00 | c)); i++; continue; }   // escape lead byte only

    if (cp < 0x10000)
      out.push_back ((JSChar) cp);
    else
      {
      cp -= 0x10000;
      out.push_back ((JSChar) (0xD800 | (cp >> 10)));
      out.push_back ((JSChar) (0xDC00 | (cp & 0x3FF)));
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

std::string jsstr_to_std(JSStringRef s)
  {
  if (!s) return std::string();
  const JSChar* u = JSStringGetCharactersPtr (s);
  size_t n = JSStringGetLength (s);
  if (!u) return std::string();
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

static JSStringRef js_string_from_bytes (const char* p, size_t n)
  {
  if (!p || n == 0) return JSStringCreateWithUTF8CString ("");
  std::vector<JSChar> u16;
  u16.reserve (n);
  utf8_to_utf16_escaped (p, n, u16);
  return JSStringCreateWithCharacters (u16.empty () ? nullptr : u16.data (), u16.size ());
  }

static JSStringRef js_string_from_bytes (const std::string & s)
  { return js_string_from_bytes (s.data (), s.size ()); }

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
          JSStringRef js = js_string_from_bytes((*p) ? *p : "", (*p) ? strlen(*p) : 0);
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
      JSStringRef js = js_string_from_bytes(v.bstrVal ? v.bstrVal : "", v.bstrVal ? strlen(v.bstrVal) : 0);
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

// world.getpluginname (no parens, e.g. in a string concat or passed as an arg) —
// COM languages auto-invoke a no-arg method here.  JS keeps it as a callable, so
// coerce it by invoking with no args and returning the value; world.foo() still
// calls it normally.  Lets COM-style JScript plugins that use world.X work.
JSValueRef Method_convertToType(JSContextRef ctx, JSObjectRef object,
                                JSType /*type*/, JSValueRef* /*exception*/)
  {
  MethodData* m = (MethodData*) JSObjectGetPrivate(object);
  if (!m) return nullptr;
  VARIANT result; VariantInit(&result);
  if (MC_DispatchInvoke(m->doc, m->name.c_str(), nullptr, 0, result, nullptr) != MC_DISP_OK)
    { MC_VariantClear(result); return nullptr; }
  JSValueRef v = variant_to_js(ctx, result);
  MC_VariantClear(result);
  return v;
  }

// world.<name> -> bound method (callable) or property value.
JSValueRef World_getProperty(JSContextRef ctx, JSObjectRef object,
                             JSStringRef propertyName, JSValueRef* /*exception*/)
  {
  CMUSHclientDoc* doc = (CMUSHclientDoc*) JSObjectGetPrivate(object);
  std::string name = jsstr_to_std(propertyName);

  if (MC_DispatchHas(name.c_str()))
    {
    MethodData* md = new MethodData{ doc, name };
    JSObjectRef obj = JSObjectMake(ctx, g_methodClass, md);
    if (!obj) { delete md; return JSValueMakeUndefined(ctx); }  // else md would leak (no finalizer runs)
    return obj;
    }

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
  md.convertToType  = Method_convertToType;
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
  JSStringRef src = js_string_from_bytes ((LPCTSTR) strCode, (size_t) strCode.GetLength ());
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
    { JSStringRef js = js_string_from_bytes (s); args.push_back (JSValueMakeString (ctx, js)); JSStringRelease (js); }

  if (regexp)                            // wildcards as a JS array [whole, $1, …]
    {
    int ncapt = 0;
    pcre_fullinfo (regexp->m_program, regexp->m_extra, PCRE_INFO_CAPTURECOUNT, &ncapt);
    std::vector<JSValueRef> wc;
    for (int i = 0; i <= ncapt; i++)
      { string w (regexp->GetWildcard (i)); JSStringRef js = js_string_from_bytes (w);
        wc.push_back (JSValueMakeString (ctx, js)); JSStringRelease (js); }
    args.push_back (JSObjectMakeArray (ctx, wc.size (), wc.empty () ? nullptr : wc.data (), nullptr));
    }

  if (table)                             // as a JS object
    {
    JSObjectRef o = JSObjectMake (ctx, nullptr, nullptr);
    for (const auto & kv : *table)
      { JSStringRef k = js_string_from_bytes (kv.first);
        JSStringRef vv = js_string_from_bytes (kv.second);
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
    {
    // A function with no explicit return yields `undefined` (a non-NULL value),
    // which is falsy — but on Windows / Lua a no-return handler means "keep going".
    // Only a real returned value decides; undefined/null -> true. (Mirrors the
    // string-returning overload below.)
    *result = true;
    if (ret && !JSValueIsUndefined (ctx, ret) && !JSValueIsNull (ctx, ret))
      *result = JSValueToBoolean (ctx, ret);
    }
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

  JSStringRef js = js_string_from_bytes ((LPCTSTR) strParam, (size_t) strParam.GetLength ());
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
