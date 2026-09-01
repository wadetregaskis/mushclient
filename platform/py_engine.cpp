// platform/py_engine.cpp — embedded CPython scripting engine for the port.
//
// On Windows the alternate languages ride COM ActiveScripting; off Windows we
// embed the real interpreter directly.  The world object model is NOT re-bound
// by hand (the way lua_methods.cpp binds Lua) — instead the `world` proxy routes
// every call through the portable dispatch-map Invoke (platform/mc_dispatch.cpp),
// so all 403 automation methods are reachable for free and stay in sync with the
// Windows dispatch map.
//
// One CPython interpreter is shared process-wide (Py_Initialize once); each world
// or plugin (each CScriptEngine) gets its own module-namespace dict so globals
// don't collide.  All HOST-initiated scripting runs on the UI thread, but the
// GIL is NOT parked there between callbacks: ensure_init releases it
// (PyEval_SaveThread) and every entry point takes it for the call's duration
// (the PyGIL guard below).  "Runs on the UI thread" is a statement about the
// port's threads, not the script's -- holding the GIL across the idle wx event
// loop froze every thread a script created (threading.Timer, a worker doing
// socket.recv), except while some other Python callback happened to be
// executing: plugins "worked while typing and stopped when idle".
//
// Built only when CMake finds Python3 (MUSHCLIENT_PYTHON_SCRIPTING) in the full
// (mushapp) build; scriptengine.cpp provides link stubs otherwise.

#if !defined(_WIN32) && defined(MUSHCLIENT_FULL_BUILD) && defined(MUSHCLIENT_PYTHON_SCRIPTING)

#include <Python.h>          // must precede the MFC/MUSHclient headers

#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "scripting/scripting.h"
#include "platform/mc_dispatch.h"
#include "platform/mc_variant_args.h"

#include <string>
#include <vector>
#include <cstring>
#include <dlfcn.h>           // dladdr — locate the bundled framework at runtime

#ifndef VT_ERROR
#define VT_ERROR 10
#endif
#ifndef VT_DISPATCH
#define VT_DISPATCH 9
#endif

// Per-engine Python state (opaque to the rest of the program; CScriptEngine
// holds it as CPythonEngine*).
class CPythonEngine
  {
  public:
  CMUSHclientDoc* doc   = nullptr;
  PyObject*       ns    = nullptr;   // module namespace (globals for exec/eval)
  PyObject*       world = nullptr;   // the `world` proxy object (a WorldObject)
  };

namespace {

// ── world / world-method proxy types ────────────────────────────────────────
struct WorldObject  { PyObject_HEAD; CMUSHclientDoc* doc; };
struct MethodObject { PyObject_HEAD; CMUSHclientDoc* doc; char* name; };

// Head-initialised (ob_refcnt=1, ob_type=NULL, rest zeroed); fields filled in
// ensure_init() so the layout stays robust across CPython versions.
PyTypeObject WorldType  = { PyVarObject_HEAD_INIT(NULL, 0) };
PyTypeObject MethodType = { PyVarObject_HEAD_INIT(NULL, 0) };

PyObject* make_world(CMUSHclientDoc* doc)
  {
  WorldObject* w = PyObject_New(WorldObject, &WorldType);
  if (w) w->doc = doc;
  return (PyObject*) w;
  }

PyObject* make_method(CMUSHclientDoc* doc, const char* name)
  {
  MethodObject* m = PyObject_New(MethodObject, &MethodType);
  if (m) { m->doc = doc; m->name = strdup(name); }
  return (PyObject*) m;
  }

// ── PyObject <-> VARIANT marshalling ────────────────────────────────────────
void py_to_variant(PyObject* o, VARIANT& v)
  {
  VariantInit(&v);
  if (o == nullptr || o == Py_None)        { v.vt = VT_EMPTY; return; }
  if (PyBool_Check(o))                     { v.vt = VT_BOOL; v.boolVal = (o == Py_True); return; }
  if (PyLong_Check(o))
    {
    v.vt = VT_I4;  v.lVal = PyLong_AsLong(o);
    if (v.lVal != -1 || !PyErr_Occurred()) return;
    PyErr_Clear();
    v.vt = VT_R8;  v.dblVal = PyLong_AsDouble(o);
    // An int beyond double range (~1.8e308) makes PyLong_AsDouble itself RAISE
    // OverflowError and answer -1.0 -- unchecked, the method received -1.0 and
    // the pending exception leaked into whatever Python ran next (the same
    // leaked-exception failure mode the Unicode comment below describes
    // fixing).  Marshal the digits instead: fall through to the str(o)
    // fallback at the bottom, which is exact for an integer and is what every
    // other unconvertible value already gets.
    if (v.dblVal != -1.0 || !PyErr_Occurred()) return;
    PyErr_Clear();
    }
  if (PyFloat_Check(o))                    { v.vt = VT_R8;   v.dblVal = PyFloat_AsDouble(o); return; }
  if (PyUnicode_Check(o))
    {
    // surrogateescape, not strict UTF-8.  Everything that entered the script
    // through this engine's own decoders -- trigger lines, wildcards, table
    // values, the simple-callback argument, any string a world method returned
    // -- can hold the lone surrogates U+DC80..U+DCFF that escape produces, and
    // the strict codec refuses them: it returned NULL, we stored an EMPTY BSTR,
    // and the pending UnicodeEncodeError leaked into the next call.  So a
    // wildcard captured from a Latin-1 world could be read but never stored,
    // sent or noted.  This is the mirror image of the result-direction fix
    // below; the two have to agree or the round trip is not closed.
    v.vt = VT_BSTR;
    PyObject* enc = PyUnicode_AsEncodedString(o, "utf-8", "surrogateescape");
    if (enc)
      { v.bstrVal = SysAllocString(PyBytes_AsString(enc) ? PyBytes_AsString(enc) : "");
        Py_DECREF(enc); }
    else
      { PyErr_Clear(); v.bstrVal = SysAllocString(""); }
    return;
    }
  if (PyBytes_Check(o))                    { v.vt = VT_BSTR; v.bstrVal = SysAllocString(PyBytes_AsString(o)); return; }
  // Fallback: str(o)
  PyObject* str = PyObject_Str(o);
  v.vt = VT_BSTR;
  v.bstrVal = nullptr;
  if (str)
    {
    PyObject* enc = PyUnicode_AsEncodedString(str, "utf-8", "surrogateescape");
    if (enc) { v.bstrVal = SysAllocString(PyBytes_AsString(enc)); Py_DECREF(enc); }
    else     PyErr_Clear();
    }
  if (!v.bstrVal) v.bstrVal = SysAllocString("");
  Py_XDECREF(str);
  }

PyObject* variant_to_py(const VARIANT& v)
  {
  if (v.vt & VT_ARRAY)
    {
    SAFEARRAY* sa = v.parray;
    if (!sa || sa->cDims < 1) return PyList_New(0);
    long count = (long) sa->rgsabound[0].cElements;
    unsigned base = v.vt & ~VT_ARRAY;
    PyObject* list = PyList_New(count);
    if (!list) Py_RETURN_NONE;          // allocation failed
    for (long i = 0; i < count; i++)
      {
      PyObject* item;
      if (base == VT_BSTR)
        {
        BSTR* p = ((BSTR*) sa->pvData) + i;
        item = (*p) ? PyUnicode_DecodeUTF8(*p, strlen(*p), "surrogateescape")
                    : PyUnicode_FromString("");
        }
      else // VT_VARIANT (the MUSHclient norm) or anything else: element is a VARIANT
        {
        VARIANT* p = ((VARIANT*) sa->pvData) + i;
        item = variant_to_py(*p);
        }
      PyList_SET_ITEM(list, i, item);   // steals ref
      }
    return list;
    }

  switch (v.vt)
    {
    case VT_EMPTY: case VT_NULL: case VT_ERROR: Py_RETURN_NONE;
    case VT_I2:   return PyLong_FromLong(v.iVal);
    case VT_I4:   return PyLong_FromLong(v.lVal);
    case VT_R4:   return PyFloat_FromDouble(v.fltVal);
    case VT_R8:   return PyFloat_FromDouble(v.dblVal);
    case VT_BOOL: return PyBool_FromLong(v.boolVal);
    case VT_BSTR: return v.bstrVal ? PyUnicode_DecodeUTF8(v.bstrVal, strlen(v.bstrVal), "surrogateescape")
                                   : PyUnicode_FromString("");
    case VT_DISPATCH: if (v.byref) return make_world((CMUSHclientDoc*) v.byref);
                      Py_RETURN_NONE;
    case VT_DATE: return PyLong_FromLong(MC_VariantDateToUnix(v));
    default:      Py_RETURN_NONE;
    }
  }

// ── world.<Method>(...) — marshal args, Invoke, marshal the return ──────────
PyObject* Method_call(PyObject* self, PyObject* args, PyObject* /*kw*/)
  {
  MethodObject* m = (MethodObject*) self;
  // The proxy holds a bare CMUSHclientDoc* -- there is no COM here, so nothing
  // AddRefs it the way pDoc->GetIDispatch(TRUE) does on Windows.  A script that
  // keeps what world.GetWorld() returned and uses it after that world is closed
  // would dereference freed memory; report it instead.
  if (!MC_DispatchDocIsLive(m->doc))
    { PyErr_SetString(PyExc_RuntimeError, "that world has been closed");
      return nullptr; }

  Py_ssize_t n = PyTuple_GET_SIZE(args);

  MCVariantArgs vargs((size_t) n);
  for (Py_ssize_t i = 0; i < n; i++)
    py_to_variant(PyTuple_GET_ITEM(args, i), vargs[(size_t) i]);

  VARIANT result; VariantInit(&result);
  std::vector<VARIANT> outargs;
  int rc = MC_DispatchInvoke(m->doc, m->name,
                             n ? vargs.data() : nullptr, (int) n, result, &outargs);

  for (auto& a : vargs) MC_VariantClear(a);

  if (rc == MC_DISP_UNKNOWN)
    { PyErr_Format(PyExc_AttributeError, "world has no method '%s'", m->name); return nullptr; }
  if (rc != MC_DISP_OK)
    { PyErr_Format(PyExc_RuntimeError, "world.%s: %s", m->name,
                   MC_DispatchBadArgsMessage(m->name, (int) n).c_str());
      MC_VariantClear(result); return nullptr; }

  PyObject* res = variant_to_py(result);
  MC_VariantClear(result);

  if (outargs.empty())
    return res;

  // By-ref getters (GetTrigger/GetAlias/GetTimer): return (result, out0, out1, ...)
  PyObject* tup = PyTuple_New(1 + (Py_ssize_t) outargs.size());
  if (!tup)   // OOM: degrade to the result alone rather than crash on SET_ITEM(NULL)
    { for (auto& o : outargs) MC_VariantClear(o); return res; }
  PyTuple_SET_ITEM(tup, 0, res);
  for (size_t i = 0; i < outargs.size(); i++)
    { PyTuple_SET_ITEM(tup, (Py_ssize_t)(i + 1), variant_to_py(outargs[i])); MC_VariantClear(outargs[i]); }
  return tup;
  }

void Method_dealloc(PyObject* self)
  {
  free(((MethodObject*) self)->name);
  Py_TYPE(self)->tp_free(self);
  }

// str(world.Foo) — COM languages auto-invoke a no-arg method accessed without
// parens (e.g. world.GetPluginID passed as an argument).  Python keeps the
// method as a callable, so coerce it by invoking with no args and stringifying
// the result; this lets COM-style plugins that pass world.X (no parens) work.
PyObject* Method_str(PyObject* self)
  {
  MethodObject* m = (MethodObject*) self;
  if (!MC_DispatchDocIsLive(m->doc))
    { PyErr_SetString(PyExc_RuntimeError, "that world has been closed");
      return nullptr; }
  VARIANT result; VariantInit(&result);
  // MC_DISP_F_INTROSPECT: this 0-arg call is Python's string coercion, not
  // something the plugin author wrote, so strict argument checking must not
  // fire on it (see mc_dispatch.h).
  if (MC_DispatchInvoke(m->doc, m->name, nullptr, 0, result, nullptr,
                        MC_DISP_F_INTROSPECT) == MC_DISP_OK)
    {
    PyObject* v = variant_to_py(result);
    MC_VariantClear(result);
    PyObject* s = PyObject_Str(v);
    Py_DECREF(v);
    return s;
    }
  MC_VariantClear(result);
  return PyUnicode_FromFormat("<world method %s>", m->name);
  }

// world.<name> -> a bound MethodObject for a dispatch method, or the value of a
// dispatch property (world.Trace, world.NoteColour, …).
PyObject* World_getattro(PyObject* self, PyObject* nameobj)
  {
  const char* name = PyUnicode_AsUTF8(nameobj);
  CMUSHclientDoc* doc = ((WorldObject*) self)->doc;
  if (name)
    {
    if (MC_DispatchHas(name))
      return make_method(doc, name);
    if (MC_DispatchHasProperty(name))
      {
      // The same stale-proxy rule Method_call keeps -- and it has to be INSIDE
      // this branch, or a dunder or an unknown name would stop falling through
      // to the generic lookup.  A property is read off the doc's members
      // directly, with no method call anywhere, so Method_call's guard never
      // saw it: world.Trace on a proxy to a closed world read freed memory.
      if (!MC_DispatchDocIsLive(doc))
        { PyErr_SetString(PyExc_RuntimeError, "that world has been closed");
          return nullptr; }
      VARIANT v;
      if (MC_DispatchGetProperty(doc, name, v))
        { PyObject* r = variant_to_py(v); MC_VariantClear(v); return r; }
      }
    }
  return PyObject_GenericGetAttr(self, nameobj);   // dunders, errors
  }

// world.<name> = value -> write a dispatch property (read-only for methods).
int World_setattro(PyObject* self, PyObject* nameobj, PyObject* value)
  {
  const char* name = PyUnicode_AsUTF8(nameobj);
  CMUSHclientDoc* doc = ((WorldObject*) self)->doc;
  if (name && value && MC_DispatchHasProperty(name))
    {
    if (!MC_DispatchDocIsLive(doc))
      { PyErr_SetString(PyExc_RuntimeError, "that world has been closed");
        return -1; }
    VARIANT v;
    py_to_variant(value, v);
    bool ok = MC_DispatchSetProperty(doc, name, v);
    MC_VariantClear(v);
    if (ok) return 0;
    }
  return PyObject_GenericSetAttr(self, nameobj, value);
  }

bool g_inited = false;
PyThreadState* g_mainThreadState = nullptr;   // saved by ensure_init's release

void ensure_init()
  {
  if (g_inited) return;

  PyConfig config;
  PyConfig_InitPythonConfig (&config);
#if defined (__APPLE__)
  // On macOS the embedded interpreter lives in a RELOCATED framework, so CPython's
  // compiled-in prefix is a build-machine path that won't exist on a user's Mac.
  // Point it at the stdlib beside libpython instead: dladdr finds wherever
  // libpython actually loaded from (the @rpath-resolved framework), which works
  // for the dev build and the relocated copy inside the .app alike.  (On Linux the
  // system libpython's own prefix is correct, so leave the default in place.)
  Dl_info info;
  if (dladdr ((void *) &Py_InitializeFromConfig, &info) && info.dli_fname)
    {
    std::string home = info.dli_fname;        // …/Python.framework/Versions/X.Y/Python
    const size_t slash = home.find_last_of ('/');
    if (slash != std::string::npos)
      {
      home.resize (slash);                    // …/Python.framework/Versions/X.Y  (the prefix)
      wchar_t * whome = Py_DecodeLocale (home.c_str (), NULL);
      if (whome)
        {
        PyConfig_SetString (&config, &config.home, whome);
        PyMem_RawFree (whome);
        }
      }
    }
#endif
  PyStatus status = Py_InitializeFromConfig (&config);   // embedded: doesn't trap signals
  PyConfig_Clear (&config);
  if (PyStatus_Exception (status) && !Py_IsInitialized ())
    Py_InitializeEx (0);                      // last-resort fallback (default prefix)
  g_inited = true;

  WorldType.tp_name      = "mushclient.world";
  WorldType.tp_basicsize = sizeof(WorldObject);
  WorldType.tp_flags     = Py_TPFLAGS_DEFAULT;
  WorldType.tp_getattro  = World_getattro;
  WorldType.tp_setattro  = World_setattro;
  WorldType.tp_doc       = "MUSHclient world object";
  PyType_Ready(&WorldType);

  MethodType.tp_name      = "mushclient.world_method";
  MethodType.tp_basicsize = sizeof(MethodObject);
  MethodType.tp_flags     = Py_TPFLAGS_DEFAULT;
  MethodType.tp_call      = Method_call;
  MethodType.tp_dealloc   = Method_dealloc;
  MethodType.tp_str       = Method_str;
  PyType_Ready(&MethodType);

  // Release the GIL that Py_Initialize left held.  From here on, every
  // host->interpreter entry point takes it for the call's duration (PyGIL
  // below), and between callbacks it is free -- which is the only thing that
  // lets a thread a SCRIPT created run while the app sits in the wx event
  // loop.  (There is no Py_Finalize anywhere in this file, so the saved state
  // is never restored for teardown; the process just exits.)
  g_mainThreadState = PyEval_SaveThread ();
  }

// Hold the GIL for the duration of one host->interpreter entry point, on
// every exit path.  PyGILState_Ensure nests correctly (it detects that the
// calling thread already holds a live thread state), so an entry point
// re-entered from inside a callback composes fine.  Everything that only ever
// runs INSIDE one of these -- Method_call, World_getattro/setattro,
// py_to_variant, report_error -- needs no guard of its own.
struct PyGIL
  {
  PyGILState_STATE s;
  PyGIL () : s (PyGILState_Ensure ()) {}
  ~PyGIL () { PyGILState_Release (s); }
  };

// Resolve a (possibly dotted) procedure name in the namespace; new ref or null.
PyObject* resolve(PyObject* ns, const char* dotted)
  {
  if (!ns || !dotted || !*dotted) return nullptr;
  std::string s(dotted);
  PyObject* obj = nullptr;
  size_t start = 0;
  bool first = true;
  for (;;)
    {
    size_t dot = s.find('.', start);
    std::string part = (dot == std::string::npos) ? s.substr(start) : s.substr(start, dot - start);
    if (first)
      {
      PyObject* o = PyDict_GetItemString(ns, part.c_str());   // borrowed
      if (!o) return nullptr;
      Py_INCREF(o); obj = o; first = false;
      }
    else
      {
      PyObject* o = PyObject_GetAttrString(obj, part.c_str()); // new ref
      Py_DECREF(obj);
      if (!o) { PyErr_Clear(); return nullptr; }
      obj = o;
      }
    if (dot == std::string::npos) break;
    start = dot + 1;
    }
  return obj;
  }

// Encode a Python str to bytes for error reporting: surrogateescape, NOT the
// strict codec.  Everything that entered the script through this engine's own
// decoders -- trigger lines, wildcards, table values, the simple-callback
// argument, any string a world method returned -- can hold the lone surrogates
// U+DC80..U+DCFF that escape produces, and PyUnicode_AsUTF8 refuses those.  So
// a handler that raised with the matched line in its message (a Latin-1 MUD is
// enough) came back NULL here and the user was shown an EMPTY description --
// on exactly the lines that most need diagnosing.  Mirrors the
// result-direction fix in the simple ExecutePython overload below, and
// length-explicit for the same reason.  Clearing the encode error is safe: the
// caller holds the exception being formatted in PyErr_Fetch'd locals, so the
// indicator being cleared is only this helper's own failure.
bool py_str_to_cstring (PyObject* o, CString& out)
  {
  PyObject* enc = PyUnicode_AsEncodedString (o, "utf-8", "surrogateescape");
  if (!enc) { PyErr_Clear (); return false; }
  char*      p = nullptr;
  Py_ssize_t n = 0;
  const bool ok = (PyBytes_AsStringAndSize (enc, &p, &n) == 0 && p);
  if (ok) out = CString (p, (int) n);
  else    PyErr_Clear ();
  Py_DECREF (enc);
  return ok;
  }

// Format the pending Python exception and surface it like LuaError does.
void report_error(CMUSHclientDoc* doc, const char* what, const char* proc)
  {
  if (!PyErr_Occurred()) return;
  PyObject *type = nullptr, *value = nullptr, *tb = nullptr;
  PyErr_Fetch(&type, &value, &tb);
  PyErr_NormalizeException(&type, &value, &tb);

  CString msg;
  PyObject* tbmod = PyImport_ImportModule("traceback");
  if (tbmod && type)
    {
    PyObject* fmt = PyObject_CallMethod(tbmod, "format_exception", "OOO",
                                        type, value ? value : Py_None, tb ? tb : Py_None);
    if (fmt)
      {
      PyObject* sep = PyUnicode_FromString("");
      PyObject* joined = PyUnicode_Join(sep, fmt);
      if (joined) { py_str_to_cstring (joined, msg); Py_DECREF (joined); }
      Py_DECREF(sep); Py_DECREF(fmt);
      }
    }
  Py_XDECREF(tbmod);
  if (msg.IsEmpty() && value)
    { PyObject* str = PyObject_Str(value); if (str) { py_str_to_cstring (str, msg); Py_DECREF (str); } }

  Py_XDECREF(type); Py_XDECREF(value); Py_XDECREF(tb);
  PyErr_Clear();

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
  ReportScriptError (doc, "Python error", 
                     TFormat ("Function/Sub: %s called by %s", proc ? proc : "", what ? what : ""),
                     msg, raisedBy);
  }

} // anonymous namespace

// ── CScriptEngine Python methods ────────────────────────────────────────────
void CScriptEngine::OpenPython ()
  {
  ensure_init ();
  PyGIL gil;
  CPythonEngine* e = new CPythonEngine ();
  e->doc = m_pDoc;
  e->ns  = PyDict_New ();
  PyObject* builtins = PyImport_ImportModule ("builtins");
  if (builtins) { PyDict_SetItemString (e->ns, "__builtins__", builtins); Py_DECREF (builtins); }
  // Every real module namespace carries __name__, and PyRun_String does not
  // add one to a bare dict.  Without it there is no NameError -- LOAD_NAME
  // falls back to the builtins module, whose own __name__ is "builtins"
  // (measured: exec("x = __name__", {}) yields "builtins") -- which is
  // quietly worse: `if __name__ == "__main__":` guards never fire,
  // logging.getLogger(__name__) logs under "builtins", and script-defined
  // classes get __module__ = "builtins", breaking pickle and
  // typing.get_type_hints.  "__main__" is what running the script directly
  // would set, so the __main__ guard fires the way the author expects for
  // what is, functionally, this script's entry point.
  PyObject* modname = PyUnicode_FromString ("__main__");
  if (modname) { PyDict_SetItemString (e->ns, "__name__", modname); Py_DECREF (modname); }
  e->world = make_world (m_pDoc);
  if (e->world) PyDict_SetItemString (e->ns, "world", e->world);
  m_pyEngine = e;
  }

void CScriptEngine::ClosePython ()
  {
  if (!m_pyEngine) return;
  PyGIL gil;
  Py_XDECREF (m_pyEngine->world);
  Py_XDECREF (m_pyEngine->ns);
  delete m_pyEngine;
  m_pyEngine = NULL;
  }

bool CScriptEngine::ParsePython (const CString & strCode, const CString & strWhat)
  {
  if (!m_pyEngine) return true;
  PyGIL gil;
  PyObject* r = PyRun_String ((LPCTSTR) strCode, Py_file_input, m_pyEngine->ns, m_pyEngine->ns);
  if (!r) { report_error (m_pDoc, strWhat, ""); return true; }
  Py_DECREF (r);
  return false;
  }

DISPID CScriptEngine::GetPythonDispid (const CString & strName)
  {
  if (!m_pyEngine || strName.IsEmpty ()) return DISPID_UNKNOWN;
  PyGIL gil;
  PyObject* fn = resolve (m_pyEngine->ns, (LPCTSTR) strName);
  bool ok = fn && PyCallable_Check (fn);
  Py_XDECREF (fn);
  return ok ? 1 : DISPID_UNKNOWN;       // sentinel; the name is what matters
  }

bool CScriptEngine::ExecutePython (DISPID & dispid, LPCTSTR szProcedure,
                                   const unsigned short iReason,
                                   LPCTSTR szType, LPCTSTR szReason,
                                   list<double> & nparams, list<string> & sparams,
                                   long & nInvocationCount, const t_regexp * regexp,
                                   map<string, string> * table, CPaneLine * paneline,
                                   bool * result, const bool bLowercaseWildcards,
                                   bool * pbReturnedAValue)
  {
  if (!m_pyEngine) return false;
  if (dispid == DISPID_UNKNOWN) return false;
  PyGIL gil;

  PyObject* fn = resolve (m_pyEngine->ns, szProcedure);
  if (!fn || !PyCallable_Check (fn))
    { Py_XDECREF (fn); dispid = DISPID_UNKNOWN; return true; }

  // Build the positional argument list: numbers, strings, [wildcards], [table],
  // [styles] — the same order ExecuteLua pushes onto the Lua stack.
  PyObject* argl = PyList_New (0);
  if (!argl)   // OOM
    { Py_XDECREF (fn); dispid = DISPID_UNKNOWN; return true; }

  for (double d : nparams)
    { PyObject* o = PyFloat_FromDouble (d); if (o) { PyList_Append (argl, o); Py_DECREF (o); } }

  for (const string & s : sparams)
    { PyObject* o = PyUnicode_DecodeUTF8 (s.data (), s.size (), "surrogateescape");
      if (o) { PyList_Append (argl, o); Py_DECREF (o); } }   // o may be NULL on OOM; Py_DECREF(NULL) crashes

  if (regexp)
    {
    vector<string> wc;                       // [0]=whole match, [1..n]=groups
    regexp->GetCallbackWildcards (bLowercaseWildcards, wc);
    PyObject* wl = PyList_New (wc.size ());
    if (wl)
      {
      for (size_t i = 0; i < wc.size (); i++)
        {
        const string & w = wc [i];
        PyList_SET_ITEM (wl, i, PyUnicode_DecodeUTF8 (w.data (), w.size (), "surrogateescape"));
        }
      PyList_Append (argl, wl); Py_DECREF (wl);
      }
    }

  if (table)
    {
    PyObject* d = PyDict_New ();
    for (const auto & kv : *table)
      { PyObject* val = PyUnicode_DecodeUTF8 (kv.second.data (), kv.second.size (), "surrogateescape");
        PyDict_SetItemString (d, kv.first.c_str (), val); Py_DECREF (val); }
    PyList_Append (argl, d); Py_DECREF (d);
    }

  // NB: the Lua trigger path also passes a 4th "styles" (paneline) argument, but
  // the Windows COM languages (VBScript/JScript/Python) receive only
  // (name, line, wildcards).  For script parity we deliberately do NOT append
  // the styled-line argument here — `paneline` is accepted and ignored.
  (void) paneline;

  PyObject* argtuple = PyList_AsTuple (argl);
  Py_DECREF (argl);

  TraceCallback (szType, szProcedure);
  if (iReason != eDontChangeAction)
    m_pDoc->m_iCurrentActionSource = iReason;

  PyObject* res = PyObject_CallObject (fn, argtuple);

  if (iReason != eDontChangeAction)
    m_pDoc->m_iCurrentActionSource = eUnknownActionSource;

  Py_DECREF (argtuple);
  Py_DECREF (fn);

  if (!res)
    { dispid = DISPID_UNKNOWN; report_error (m_pDoc, szType, szProcedure); return true; }

  nInvocationCount++;

  if (result)
    {
    *result = true;
    if (PyBool_Check (res))
      *result = (res == Py_True);
    else if (res == Py_None)
      *result = true;                  // no explicit return → keep going
    else
      {
      // Covers PyLong too: ordinary int truthiness never raises, but an int
      // SUBCLASS overriding __bool__ theoretically could, so PyLong is not
      // special-cased out of the -1 check.
      const int t = PyObject_IsTrue (res);
      if (t < 0)
        {
        // The truthiness coercion ITSELF raised (a numpy/pandas-style
        // ambiguous __bool__, or a pathological __len__).  -1 is truthy in C,
        // so this used to read as "keep going, no error" -- with the
        // exception left PENDING on the one interpreter every world and
        // plugin share (see the header comment), to be picked up by whatever
        // unrelated Python callback ran next and misattributed to it.
        // Report it against THIS handler instead, exactly as a raising call
        // is handled a few lines above.
        Py_DECREF (res);
        dispid = DISPID_UNKNOWN;
        report_error (m_pDoc, szType, szProcedure);
        return true;
        }
      *result = (t != 0);
      }
    }

  // Whether the handler answered at all, which `result` cannot express -- its
  // default for a no-return handler is true, and so is an explicit `return True`.
  // The two MXP call sites read the value as a veto and need the difference.
  // docs/UPSTREAM-BUGS.md 90.  None is what a Python handler that falls off the
  // end returns.
  if (pbReturnedAValue)
    *pbReturnedAValue = res != Py_None;

  Py_DECREF (res);
  return false;
  }

// Simple string-in / string-out overload (speed-walk, send-to-script, etc.)
bool CScriptEngine::ExecutePython (DISPID & dispid, LPCTSTR szProcedure,
                                   const unsigned short iReason,
                                   LPCTSTR szType, LPCTSTR szReason,
                                   CString strParam, long & nInvocationCount,
                                   CString & result)
  {
  if (!m_pyEngine) return false;
  if (dispid == DISPID_UNKNOWN) return false;
  PyGIL gil;

  PyObject* fn = resolve (m_pyEngine->ns, szProcedure);
  if (!fn || !PyCallable_Check (fn))
    { Py_XDECREF (fn); dispid = DISPID_UNKNOWN; return true; }

  PyObject* arg = PyUnicode_DecodeUTF8 ((LPCTSTR) strParam, strParam.GetLength (), "surrogateescape");

  TraceCallback (szType, szProcedure);
  if (iReason != eDontChangeAction)
    m_pDoc->m_iCurrentActionSource = iReason;
  PyObject* res = PyObject_CallFunctionObjArgs (fn, arg, NULL);
  if (iReason != eDontChangeAction)
    m_pDoc->m_iCurrentActionSource = eUnknownActionSource;

  Py_DECREF (arg);
  Py_DECREF (fn);

  if (!res)
    { dispid = DISPID_UNKNOWN; report_error (m_pDoc, szType, szProcedure); return true; }

  nInvocationCount++;

  // Only a STRING or a NUMBER replaces the caller's string, mirroring Lua's
  // lua_isstring gate (lua_scripting.cpp).  This overload carries whole MUD
  // packets and typed commands, so a handler that happens to return a dict or a
  // list must not turn the packet into "{'a': 1}"; a stray return value is a
  // programming slip, not a rewrite request.
  if (res != Py_None &&
      (PyUnicode_Check (res) || PyBytes_Check (res) ||
       PyLong_Check (res)    || PyFloat_Check (res)))
    {
    PyObject* str = PyObject_Str (res);
    if (str)
      {
      // surrogateescape on the way OUT as well as in (the arguments are decoded
      // with it a few lines up).  PyUnicode_AsUTF8 refuses a string holding the
      // lone surrogates that escape produces, so a callback handed a Latin-1 MUD
      // line and asked to return it got NULL here -- the caller's string was
      // silently left as it was, and the pending UnicodeEncodeError leaked into
      // the next call.
      PyObject* enc = PyUnicode_AsEncodedString (str, "utf-8", "surrogateescape");
      if (enc)
        {
        char*      p = nullptr;
        Py_ssize_t n = 0;
        if (PyBytes_AsStringAndSize (enc, &p, &n) == 0 && p)
          result = CString (p, (int) n);
        Py_DECREF (enc);
        }
      else
        PyErr_Clear ();
      Py_DECREF (str);
      }
    }
  Py_DECREF (res);
  return false;
  }

#endif // !_WIN32 && MUSHCLIENT_FULL_BUILD && MUSHCLIENT_PYTHON_SCRIPTING
