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

#if (defined(_WIN32) || defined(MUSHCLIENT_FULL_BUILD)) && defined(MUSHCLIENT_TCL_SCRIPTING)

#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "scripting/scripting.h"
#include "platform/mc_dispatch.h"
#include "platform/mc_variant_args.h"
#include "scripting/errors.h"         // eOK, eWorldClosed -- SendPkt

// The port defines WIN32 (no underscore) for source compatibility; tcl.h would
// then treat this as a Windows build (dllimport storage classes, Win types).  The
// MFC headers above are already parsed, so undefining it here only affects tcl.h.
// On Windows it IS a Windows build, and the Tcl linked is the static one
// (packaging\windows\fetch-deps.ps1), which tcl.h has to be told.
#ifdef _WIN32
#define STATIC_BUILD
#elif defined(WIN32)
#undef WIN32
#endif

#include <tcl.h>
#include <string>
#include <vector>
#include <cstdlib>          // setenv, realpath
#include <sys/stat.h>       // stat
#ifdef _WIN32
#ifndef PATH_MAX
#define PATH_MAX MAX_PATH
#endif
#elif defined(__APPLE__)
#include <mach-o/dyld.h>    // _NSGetExecutablePath
#else
#include <unistd.h>         // readlink
#endif
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
Tcl_Obj* tcl_from_bytes (CMUSHclientDoc* doc, const std::string& s);

// The world's text in `doc`, in Tcl's terms: tcl_from_bytes, below.
Tcl_Obj* variant_to_tcl (CMUSHclientDoc* doc, Tcl_Interp* interp, const VARIANT& v)
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
          { const std::string s = MC_BstrBytes (*p);
            Tcl_ListObjAppendElement (interp, list, tcl_from_bytes (doc, s)); }
          }
        else
          {
          VARIANT* p = ((VARIANT*) sa->pvData) + i;
          Tcl_ListObjAppendElement (interp, list, variant_to_tcl (doc, interp, *p));
          }
        }
      }
    return list;
    }

  switch (v.vt)
    {
    // VT_DISPATCH (a nested `world` object, e.g. GetWorld) has no Tcl object
    // -- which also means a Tcl script cannot hold a proxy to another world and
    // so cannot outlive one.  The other four engines mint a proxy here and need
    // an MC_DispatchDocIsLive check on every call; Tcl is safe by construction.
    // form; return empty.  Cross-world scripting is the one gap vs the JS engine.
    case VT_EMPTY: case VT_NULL: case VT_ERROR: case VT_DISPATCH:
      return Tcl_NewObj ();
    case VT_I2:   return Tcl_NewLongObj (v.iVal);
    case VT_I4:   return Tcl_NewLongObj (v.lVal);
    case VT_R4:   return Tcl_NewDoubleObj (v.fltVal);
    case VT_R8:   return Tcl_NewDoubleObj (v.dblVal);
    case VT_BOOL: return Tcl_NewBooleanObj (v.boolVal != 0);
    case VT_BSTR: { const std::string s = MC_BstrBytes (v.bstrVal);
                    return tcl_from_bytes (doc, s); }
    case VT_DATE: return Tcl_NewLongObj (MC_VariantDateToUnix(v));
    default:      return Tcl_NewObj ();
    }
  }

// ── Text: the world's bytes <-> Tcl's strings ────────────────────────────────
// A Tcl string is Unicode, kept as "modified UTF-8": NUL as C0 80, and in 8.6 a
// character past U+FFFF as a pair of encoded surrogates.  Handing that over as the
// world's bytes, as this did, was right for plain ASCII and plain UTF-8 and wrong
// for everything else:
//
//   * "\0" reached the world as C0 80, and "\xff" -- U+00FF, how Tcl writes a byte
//     -- as C3 BF, so `world SendPkt "\xff\xfb\x01"` (IAC WILL ECHO) sent five bytes
//     of nonsense, in every world;
//   * a non-UTF-8 world's text came in as raw bytes that Tcl read as Latin-1, so
//     the first string operation that rebuilt it (string range, string map, regsub)
//     turned its E9 into C3 A9 on the way back out.
//
// So, per world:
//   UTF-8 world -- the text is UTF-8: in as it is (NUL as C0 80); out as proper
//     UTF-8 (C0 80 back to NUL, surrogate pairs joined).  Nothing that was plain
//     UTF-8 or ASCII before changes.
//   any other world -- the text is bytes, which come in as U+0000..U+00FF, one
//     character a byte, and go out the same way: exact both ways.  A character a
//     script wrote that no byte means is the Windows-1252 byte for it if there is
//     one (the world's text is taken as 1252 elsewhere), and its UTF-8 otherwise,
//     as before.
//   SendPkt, in any world -- a packet is bytes, and Tcl's own way of writing bytes
//     is characters U+0000..U+00FF (\xNN, `binary format`, `encoding convertto`), so
//     a packet made only of those is sent as those bytes.  One with any character
//     beyond goes as text, as above.

// The next character of Tcl's modified UTF-8, as Tcl reads it: a byte that does
// not start a well-formed sequence is the Latin-1 character it would be.
static unsigned int next_char (const unsigned char* p, size_t n, size_t& i)
  {
  const unsigned char c = p [i];
  size_t extra = 0;
  unsigned int cp = 0;
  if      (c < 0x80)           { i++; return c; }
  else if ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1F; }
  else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; }
  else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07; }
  else                         { i++; return c; }
  if (i + extra >= n)            // cut short
    { i++; return c; }
  for (size_t k = 1; k <= extra; k++)
    {
    const unsigned char cc = p [i + k];
    if ((cc & 0xC0) != 0x80)
      { i++; return c; }
    cp = (cp << 6) | (cc & 0x3F);
    }
  // overlong forms are Latin-1 bytes too -- except C0 80, which is NUL
  if ((extra == 1 && cp < 0x80 && !(c == 0xC0 && p [i + 1] == 0x80)) ||
      (extra == 2 && cp < 0x800) || (extra == 3 && cp < 0x10000) || cp > 0x10FFFF)
    { i++; return c; }
  i += extra + 1;
  return cp;
  }

static void put_utf8 (std::string& out, unsigned int cp)
  {
  if (cp < 0x80)          out += (char) cp;
  else if (cp < 0x800)  { out += (char) (0xC0 | (cp >> 6));  out += (char) (0x80 | (cp & 0x3F)); }
  else if (cp < 0x10000){ out += (char) (0xE0 | (cp >> 12)); out += (char) (0x80 | ((cp >> 6) & 0x3F));
                          out += (char) (0x80 | (cp & 0x3F)); }
  else                  { out += (char) (0xF0 | (cp >> 18)); out += (char) (0x80 | ((cp >> 12) & 0x3F));
                          out += (char) (0x80 | ((cp >> 6) & 0x3F)); out += (char) (0x80 | (cp & 0x3F)); }
  }

// The Windows-1252 byte for a character outside Latin-1, or 0.
static unsigned char cp1252_byte (const unsigned int cp)
  {
  static const unsigned short k [32] = {    // 0x80..0x9F; 0 where 1252 has none
    0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0,      0x017D, 0,
    0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178 };
  for (int b = 0; b < 32; b++)
    if (k [b] && k [b] == cp)
      return (unsigned char) (0x80 + b);
  return 0;
  }

static bool utf8_world (CMUSHclientDoc* doc) { return doc && doc->m_bUTF_8; }

// The world's bytes as a Tcl string.
Tcl_Obj* tcl_from_bytes (CMUSHclientDoc* doc, const char* text, size_t n)
  {
  std::string s;
  s.reserve (n + n / 8);
  const bool utf8 = utf8_world (doc);
  for (size_t i = 0; i < n; i++)
    {
    const unsigned char c = (unsigned char) text [i];
    if (c == 0)
      s += "\xC0\x80";
    else if (c < 0x80 || utf8)
      s += (char) c;              // a UTF-8 world's bytes are Tcl's own already
    else
      put_utf8 (s, c);            // a byte, as the character U+00xx
    }
  return Tcl_NewStringObj (s.data (), (int) s.size ());
  }

Tcl_Obj* tcl_from_bytes (CMUSHclientDoc* doc, const std::string& s)
  { return tcl_from_bytes (doc, s.data (), s.size ()); }

// A Tcl string as the world's bytes; `packet` for SendPkt (see above).
std::string tcl_to_bytes (CMUSHclientDoc* doc, Tcl_Obj* o, const bool packet = false)
  {
  int len = 0;
  const unsigned char* p = o ? (const unsigned char*) Tcl_GetStringFromObj (o, &len) : nullptr;
  const size_t n = p ? (size_t) len : 0;

  // Decode once: Tcl's characters, surrogate pairs joined.
  std::vector<unsigned int> chars;
  chars.reserve (n);
  bool allBytes = true;
  for (size_t i = 0; i < n; )
    {
    unsigned int cp = next_char (p, n, i);
    if (cp >= 0xD800 && cp <= 0xDBFF && i < n)
      {
      size_t j = i;
      const unsigned int lo = next_char (p, n, j);
      if (lo >= 0xDC00 && lo <= 0xDFFF)
        {
        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
        i = j;
        }
      }
    if (cp > 0xFF)
      allBytes = false;
    chars.push_back (cp);
    }

  std::string out;
  out.reserve (n);
  if ((packet && allBytes) || !utf8_world (doc))
    {
    for (unsigned int cp : chars)
      {
      if (cp <= 0xFF)
        out += (char) cp;
      else if (const unsigned char b = cp1252_byte (cp))
        out += (char) b;
      else
        put_utf8 (out, cp);
      }
    return out;
    }

  // A UTF-8 world: what was already bytes stays exactly as it was (a raw byte that
  // is not UTF-8 included); only the modified forms change.
  for (size_t i = 0; i < n; )
    {
    const size_t start = i;
    unsigned int cp = next_char (p, n, i);
    if (cp == 0)
      { out += '\0'; continue; }
    if (cp >= 0xD800 && cp <= 0xDBFF && i < n)
      {
      size_t j = i;
      const unsigned int lo = next_char (p, n, j);
      if (lo >= 0xDC00 && lo <= 0xDFFF)
        {
        put_utf8 (out, 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00));
        i = j;
        continue;
        }
      }
    out.append ((const char*) p + start, i - start);
    }
  return out;
  }

// Marshal a Tcl argument as a string (VT_BSTR); the dispatch coerces it to the
// method's real parameter type (mc_dispatch.cpp: atol/atof for numeric params),
// which suits Tcl's everything-is-a-string value model.  (A NUL ends the string
// there, as it does for every engine: a world method takes a C string.)
void tcl_to_variant (CMUSHclientDoc* doc, Tcl_Obj* o, VARIANT& out, const bool packet = false)
  {
  VariantInit (&out);
  out.vt = VT_BSTR;
  out.bstrVal = MC_BstrFromBytes (tcl_to_bytes (doc, o, packet).c_str ());
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
    MCVariantArgs vargs ((size_t) nextra);
    // SendPkt's argument is a packet, not text: see tcl_to_bytes.
    std::string lower (name);
    for (char& ch : lower) ch = (char) tolower ((unsigned char) ch);   // names are case-blind
    const bool packet = lower == "sendpkt";
    if (packet && nextra >= 1)
      {
      // Straight to the socket, as CMUSHclientDoc::SendPkt does it, but with its
      // length: a method's string parameter ends at the first NUL, and a telnet
      // packet (NAWS's IAC SB 31 0 80 0 24 IAC SE) is full of them.
      const std::string bytes = tcl_to_bytes (doc, objv[2], true);
      long code = eWorldClosed;
      if (doc->m_iConnectPhase == eConnectConnectedToMud)
        {
        doc->SendPacket (bytes.data (), (int) bytes.size ());
        code = eOK;
        }
      Tcl_SetObjResult (interp, Tcl_NewLongObj (code));
      return TCL_OK;
      }
    for (int i = 0; i < nextra; i++) tcl_to_variant (doc, objv[2 + i], vargs[i], packet);

    VARIANT result; VariantInit (&result);
    std::vector<VARIANT> outargs;
    int rc = MC_DispatchInvoke (doc, name.c_str (),
                                nextra ? vargs.data () : nullptr, nextra, result, &outargs);
    for (auto& a : vargs) MC_VariantClear (a);

    if (rc != MC_DISP_OK)
      {
      MC_VariantClear (result);
      Tcl_SetObjResult (interp, Tcl_NewStringObj (
        ("world " + name + ": " + MC_DispatchErrorMessage (name.c_str (), nextra, rc)).c_str (), -1));
      return TCL_ERROR;
      }

    if (outargs.empty ())
      Tcl_SetObjResult (interp, variant_to_tcl (doc, interp, result));
    else                                        // [out] getters -> {result out1 out2 ...}
      {
      Tcl_Obj* list = Tcl_NewListObj (0, nullptr);
      Tcl_ListObjAppendElement (interp, list, variant_to_tcl (doc, interp, result));
      for (auto& o : outargs) { Tcl_ListObjAppendElement (interp, list, variant_to_tcl (doc, interp, o)); MC_VariantClear (o); }
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
        { Tcl_SetObjResult (interp, variant_to_tcl (doc, interp, v)); MC_VariantClear (v); return TCL_OK; }
      }
    else if (nextra == 1)                       // write
      {
      VARIANT v; tcl_to_variant (doc, objv[2], v);
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

// `exit`, in a world's or a plugin's interpreter (OpenTcl).  (An interpreter a
// script makes for itself with `interp create` has Tcl's own, which still exits:
// that one is not ours to replace.)
int Exit_cmd (ClientData, Tcl_Interp* interp, int, Tcl_Obj* const[])
  {
  Tcl_SetObjResult (interp, Tcl_NewStringObj (
    "exit is not allowed in a MUSHclient script: it would close the whole client", -1));
  return TCL_ERROR;
  }

void report_error (CMUSHclientDoc* doc, Tcl_Interp* interp, const char* what, const char* proc)
  {
  if (!doc || !interp) return;
  // The world's bytes, so that an error quoting a line from the MUD quotes it as it was.
  const std::string text = tcl_to_bytes (doc, Tcl_GetObjResult (interp));
  const char* msg = text.c_str ();
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
  ReportScriptError (doc, "Tcl error",
                     TFormat ("Function/Sub: %s called by %s", proc ? proc : "", what ? what : ""),
                     msg ? msg : "", raisedBy);
  }

// Does a command/proc of this name exist in the interpreter?
// A handler is a PROC the script defined -- not any command that happens to
// exist.  Tcl_GetCommandInfo finds every built-in too (puts, set, string, ...,
// and the `world` command this engine installs), so it answered "yes, that
// callback is implemented" for hundreds of names no plugin ever wrote.  `info
// procs` is the distinction Tcl itself draws.
bool command_exists (Tcl_Interp* interp, const char* name)
  {
  if (!interp || !name || !*name) return false;
  Tcl_Obj* cmd = Tcl_NewListObj (0, nullptr);
  Tcl_ListObjAppendElement (interp, cmd, Tcl_NewStringObj ("info", -1));
  Tcl_ListObjAppendElement (interp, cmd, Tcl_NewStringObj ("procs", -1));
  Tcl_ListObjAppendElement (interp, cmd, Tcl_NewStringObj (name, -1));
  Tcl_IncrRefCount (cmd);
  const int rc = Tcl_EvalObjEx (interp, cmd, TCL_EVAL_DIRECT);
  Tcl_DecrRefCount (cmd);
  if (rc != TCL_OK) { Tcl_ResetResult (interp); return false; }
  int len = 0;
  Tcl_ListObjLength (interp, Tcl_GetObjResult (interp), &len);
  Tcl_ResetResult (interp);
  return len > 0;
  }

} // anonymous namespace

// ── CScriptEngine Tcl methods ────────────────────────────────────────────────
void CScriptEngine::OpenTcl ()
  {
  // Tcl's platform initialisation, which sets the system encoding (utf-8 here
  // rather than the built-in iso8859-1 default) and -- given a real argv0 --
  // populates [info nameofexecutable].  Every embedding is supposed to call it
  // once before the first Tcl_CreateInterp.  Passing NULL did only the
  // encoding half: TclpFindExecutable returns immediately on a NULL argv0, so
  // [info nameofexecutable] answered "" in every Tcl world and plugin -- and
  // [file dirname] of "" is ".", silently pointing a script that locates
  // resources beside the app at the process CWD instead.  The executable path
  // is the same one that locates the bundled script library
  // (Contents/Resources/tcl8.6, so init.tcl is found with no system Tcl), so
  // one lookup now serves both.  Done once, before any interp is created,
  // working in the dev build and the packaged .app alike.  If the bundled
  // library isn't present, TCL_LIBRARY stays unset → Tcl falls back to its
  // built-in path.
  static bool s_platformInited = false;
  if (!s_platformInited)
    {
    s_platformInited = true;
    char real[PATH_MAX];
#ifdef _WIN32
    // (Tcl's own TclpFindExecutable asks Windows for the module name itself
    // and ignores the argument; the path still locates the script library.)
    const DWORD n = GetModuleFileNameA (NULL, real, sizeof (real));
    const bool haveExe = n > 0 && n < sizeof (real);
#elif defined(__APPLE__)
    char exe[PATH_MAX]; uint32_t sz = sizeof (exe);
    const bool haveExe =
        _NSGetExecutablePath (exe, &sz) == 0 && realpath (exe, real) != NULL;
#else
    const ssize_t n = readlink ("/proc/self/exe", real, sizeof (real) - 1);
    if (n > 0) real[n] = '\0';
    const bool haveExe = n > 0;
#endif
    if (haveExe)
      {
      std::string p (real);                       // .../Contents/MacOS/MUSHclient
      size_t slash = p.find_last_of ("/\\");
      if (slash != std::string::npos)
        {
        const std::string dir = p.substr (0, slash);
        static const char* const kRelCandidates[] = { "/../Resources/tcl8.6", "/tcl8.6", "/../share/mushclient/tcl8.6" };
        for (const char* rel : kRelCandidates)
          {
          std::string lib = dir + rel;
          struct stat st;
          if (stat ((lib + "/init.tcl").c_str (), &st) == 0)
            {
#ifdef _WIN32
            // The process environment block: Tcl reads it with
            // GetEnvironmentVariableW (tclWinInit.c), not the CRT's copy.
            SetEnvironmentVariableA ("TCL_LIBRARY", lib.c_str ());
#else
            setenv ("TCL_LIBRARY", lib.c_str (), 1);
#endif
            break;
            }
          }
        }
      }
    // AFTER TCL_LIBRARY: initialising the subsystems can settle the library
    // path (the encoding search path is derived from it), and a variable set
    // later would then be too late to count.
    Tcl_FindExecutable (haveExe ? real : NULL);   // copies the name; `real` need not outlive this
    }

  CTclEngine* e = new CTclEngine ();
  e->doc    = m_pDoc;
  e->interp = Tcl_CreateInterp ();
  Tcl_Init (e->interp);   // best-effort: load the script library (built-ins work regardless)
  Tcl_CreateObjCommand (e->interp, "world", World_cmd, (ClientData) m_pDoc, nullptr);
  // Tcl's `exit` ends the PROCESS -- the client, every world in it, unsaved -- where
  // Python's sys.exit and Ruby's exit only raise, which the engines catch.  So here
  // it raises too.
  Tcl_CreateObjCommand (e->interp, "exit", Exit_cmd, nullptr, nullptr);
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
                                bool * result, const bool bLowercaseWildcards,
                                bool * pbReturnedAValue)
  {
  if (!m_tclEngine) return false;
  if (dispid == DISPID_UNKNOWN) return false;
  Tcl_Interp* interp = m_tclEngine->interp;
  if (!command_exists (interp, szProcedure)) { dispid = DISPID_UNKNOWN; return true; }

  std::vector<Tcl_Obj*> av;
  av.push_back (Tcl_NewStringObj (szProcedure, -1));
  // An integral value becomes an INTEGER object, not a double.  nparams is a
  // list<double> because that is the widest thing the seam carries, but a
  // trigger's line number or a timer's count arriving as "16.0" is useless in
  // Tcl: it cannot be `incr`ed, cannot index a string, and reads wrong in any
  // message the script builds.  Lua and JavaScript both hand the script a number
  // that stringifies without the decimal point; Tcl was the only engine that
  // could not.
  for (double d : nparams)
    {
    if (d == (double) (long long) d &&
        d >= -9007199254740992.0 && d <= 9007199254740992.0)
      av.push_back (Tcl_NewWideIntObj ((Tcl_WideInt) d));
    else
      av.push_back (Tcl_NewDoubleObj (d));
    }
  // Length-explicit (-1 means strlen): sparams carries the raw telnet
  // subnegotiation payload (OnPluginTelnetSubnegotiation), which holds NUL
  // bytes -- ZMP and MSDP separate their fields with them.  Same rule the
  // simple overload below already follows; wildcards and the table ride the
  // same marshalling, so they follow it too.
  for (const string & s : sparams)
    av.push_back (tcl_from_bytes (m_pDoc, s));
  if (regexp)                                   // wildcards as a Tcl list {whole $1 ...}
    {
    vector<string> wildcards;
    regexp->GetCallbackWildcards (bLowercaseWildcards, wildcards);
    Tcl_Obj* wc = Tcl_NewListObj (0, nullptr);
    for (size_t i = 0; i < wildcards.size (); i++)
      { const string & w = wildcards [i];
        Tcl_ListObjAppendElement (interp, wc, tcl_from_bytes (m_pDoc, w)); }
    av.push_back (wc);
    }
  if (table)                                    // as a Tcl dict (flat key value ...)
    {
    Tcl_Obj* d = Tcl_NewListObj (0, nullptr);
    for (const auto & kv : *table)
      { Tcl_ListObjAppendElement (interp, d, tcl_from_bytes (m_pDoc, kv.first));
        Tcl_ListObjAppendElement (interp, d, tcl_from_bytes (m_pDoc, kv.second)); }
    av.push_back (d);
    }

  for (Tcl_Obj* o : av) Tcl_IncrRefCount (o);
  TraceCallback (szType, szProcedure);
  if (iReason != eDontChangeAction) m_pDoc->m_iCurrentActionSource = iReason;
  int rc = Tcl_EvalObjv (interp, (int) av.size (), av.data (), 0);
  if (iReason != eDontChangeAction) m_pDoc->m_iCurrentActionSource = eUnknownActionSource;

  // A proc that falls off the end yields the empty string, which is not a
  // boolean -- so the conversion FAILING is Tcl's "said nothing", not just the
  // error arm.  `b` keeps its 1 in that case, exactly as before, so `result` is
  // computed identically; only the new flag is new.  docs/UPSTREAM-BUGS.md 90.
  int b = 1;
  const bool bSaid = rc == TCL_OK &&
        Tcl_GetBooleanFromObj (nullptr, Tcl_GetObjResult (interp), &b) == TCL_OK;
  if (result)                                   // truthy result keeps processing (Lua parity)
    *result = (rc == TCL_OK) ? (b != 0) : true;
  if (pbReturnedAValue)
    *pbReturnedAValue = bSaid;
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

  // Length-explicit for the argument: -1 means strlen, and this overload carries
  // whole MUD packets, which contain NUL bytes.
  Tcl_Obj* av[2] = { Tcl_NewStringObj (szProcedure, -1),
                     tcl_from_bytes (m_pDoc, (LPCTSTR) strParam, (size_t) strParam.GetLength ()) };
  Tcl_IncrRefCount (av[0]); Tcl_IncrRefCount (av[1]);
  TraceCallback (szType, szProcedure);
  if (iReason != eDontChangeAction) m_pDoc->m_iCurrentActionSource = iReason;
  int rc = Tcl_EvalObjv (interp, 2, av, 0);
  if (iReason != eDontChangeAction) m_pDoc->m_iCurrentActionSource = eUnknownActionSource;
  // Only a non-empty result replaces the caller's string.  This callback shape
  // is ExecutePluginScriptRtn's, where the same CString is both the argument and
  // the result -- OnPluginPacketReceived, OnPluginCommandChanged and friends --
  // so an unconditional assignment blanked every packet and every typed command
  // the moment a Tcl world loaded a plugin.  Tcl has no "returned nothing"
  // sentinel: a proc that falls off the end leaves an empty interpreter result,
  // indistinguishable from an explicit `return ""`.  Empty therefore means
  // "leave it alone", which is the same effect the four sibling engines get from
  // their None / undefined / nil / undef checks.
  if (rc == TCL_OK)
    {
    // Through tcl_to_bytes, like everything else going back to the world: an empty
    // result is still "leave it alone".
    int rlen = 0;
    Tcl_GetStringFromObj (Tcl_GetObjResult (interp), &rlen);
    if (rlen > 0) { const std::string r = tcl_to_bytes (m_pDoc, Tcl_GetObjResult (interp));
                    result = CString (r.data (), (int) r.size ()); }
    }
  Tcl_DecrRefCount (av[0]); Tcl_DecrRefCount (av[1]);

  if (rc != TCL_OK) { dispid = DISPID_UNKNOWN; report_error (m_pDoc, interp, szType, szProcedure); return true; }
  nInvocationCount++;
  return false;
  }

#endif // (_WIN32 || MUSHCLIENT_FULL_BUILD) && MUSHCLIENT_TCL_SCRIPTING
