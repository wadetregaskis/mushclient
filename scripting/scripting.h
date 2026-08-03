#pragma once

extern "C"
  {
#ifdef LUA_52
    #include "../../lua52/src/lua.h"
    #include "../../lua52/src/lauxlib.h"
#elif defined(_WIN32)
    #include "../lua.h"
    #include "../lauxlib.h"
#else
    #include "lua.h"
    #include "lauxlib.h"
#endif
  }

#include "paneline.h"

// Opaque per-engine state for each embedded alt-language interpreter (one per
// world/plugin).  Defined in platform/{py_engine,perl_engine,js_engine}.cpp;
// null unless the world's scripting language is that one.
class CPythonEngine;
class CPerlEngine;
class CJSEngine;
class CTclEngine;
class CRubyEngine;

#define DOCUMENT_STATE "mushclient.document"
#define WORLD_LIBRARY "world"

// Route a script error to the dialog or the output window, per the world's
// "Note errors to the output window" setting, and to the error log.  Shared by
// LuaError and by all five embedded engines' report_error.
void ReportScriptError (CMUSHclientDoc * pDoc,
                        LPCTSTR strEvent,
                        LPCTSTR strCalledBy,
                        LPCTSTR strDescription,
                        LPCTSTR strRaisedBy,
                        const bool bImmediate = true,
                        const int nLine = 0);

void LuaError (lua_State *L, 
               LPCTSTR strEvent = "Run-time error",
               LPCTSTR strProcedure = "",
               LPCTSTR strType = "",
               LPCTSTR strReason = "",
               CMUSHclientDoc * pDoc = NULL);

class CScriptEngine : public CObject
  {

  public:
    CScriptEngine (CMUSHclientDoc * pDoc,
                   const CString strLanguage) // constructor
      {
      m_pDoc = pDoc;
      m_strLanguage = strLanguage;
#ifdef _WIN32
      m_IActiveScript = NULL;
      m_IActiveScriptParse = NULL;
      m_site = NULL;
      m_pDispatch = NULL;
#endif
      L = NULL;
#ifndef _WIN32
      m_pyEngine = NULL;
      m_perlEngine = NULL;
      m_jsEngine = NULL;
      m_tclEngine = NULL;
      m_rubyEngine = NULL;
#endif
      };  // end of constructor

    ~CScriptEngine () // destructor
      {
      DisableScripting ();
      }; // end of destuctor

  bool CreateScriptEngine (void);
  bool Parse (const CString & strCode, const CString & strWhat);

  DISPID GetDispid (const CString & strName);
  DISPID GetLuaDispid (const CString & strName);

  // returns true if script error
  bool Execute (DISPID & dispid,  // dispatch ID, will be set to DISPID_UNKNOWN on an error
                LPCTSTR szProcedure,  // eg. ON_TRIGGER_XYZ
                const unsigned short iReason,  // value for m_iCurrentActionSource
                LPCTSTR szType,   // eg. trigger, alias
                LPCTSTR szReason, // eg. trigger subroutine XXX
                DISPPARAMS & params,  // parameters
                long & nInvocationCount,  // count of invocations
                COleVariant * result    // result of call
                );
  bool ShowError (const HRESULT result, const CString strMsg);
  void DisableScripting (void);

  void OpenLua ();
  void OpenLuaDelayed ();
  void CloseLua ();

//  void RegisterLuaRoutines ();

  bool ParseLua (const CString & strCode, const CString & strWhat);
  // returns true if script error
  bool ExecuteLua (DISPID & dispid,          // dispatch ID, will be set to DISPID_UNKNOWN on an error
                   LPCTSTR szProcedure,      // eg. ON_TRIGGER_XYZ
                   const unsigned short iReason,  // value for m_iCurrentActionSource
                   LPCTSTR szType,           // eg. trigger, alias
                   LPCTSTR szReason,         // eg. trigger subroutine XXX
                   list<double> & nparams,   // list of number parameters
                   list<string> & sparams,   // list of string parameters
                   long & nInvocationCount,  // count of invocations
                   const t_regexp * regexp = NULL,  // regular expression (for triggers, aliases)
                   map<string, string> * table = NULL, // map of other things
                   CPaneLine * paneline = NULL,        // and the line (for triggers)
                   bool * result = NULL,               // where to put result
                   // The trigger's "convert wildcards to lower case" option.
                   // IGNORED by Lua on purpose -- Windows' native Lua path reads
                   // the raw regexp too (see t_regexp::GetCallbackWildcards).
                   // Honoured by the five embedded engines, which stand in for
                   // the ActiveScript hosts that get FixWildcard'ed wildcards.
                   const bool bLowercaseWildcards = false);

  // returns true if script error
  bool ExecuteLua (DISPID & dispid,          // dispatch ID, will be set to DISPID_UNKNOWN on an error
                   LPCTSTR szProcedure,      // eg. ON_TRIGGER_XYZ
                   const unsigned short iReason,  // value for m_iCurrentActionSource
                   LPCTSTR szType,           // eg. trigger, alias
                   LPCTSTR szReason,         // eg. trigger subroutine XXX
                   CString strParam,         // string parameter
                   long & nInvocationCount,  // count of invocations
                   CString & result);        // where to put result

  // return value is return from call

  const bool IsLua () const { return L != NULL; }
  lua_State           * L;                  // Lua state

  // The non-Lua engines (Python, Perl, JavaScript) are embedded directly on the
  // port and reach the world API through the portable dispatch-map Invoke
  // (platform/mc_dispatch.cpp), not a hand-written per-language binding.  Each
  // Is<Lang>() is defined on every platform — false on Windows, where the COM
  // host runs these languages instead — so the shared trigger/alias/timer/plugin
  // call sites gate on UsesEmbeddedCallback() with no per-site #ifdefs.  See
  // platform/{py_engine,perl_engine,js_engine}.cpp.
#ifdef _WIN32
  const bool IsPython () const { return false; }
  const bool IsPerl   () const { return false; }
  const bool IsJS     () const { return false; }
  const bool IsTcl    () const { return false; }
  const bool IsRuby   () const { return false; }
#else
  const bool IsPython () const { return m_pyEngine   != NULL; }
  const bool IsPerl   () const { return m_perlEngine != NULL; }
  const bool IsJS     () const { return m_jsEngine   != NULL; }
  const bool IsTcl    () const { return m_tclEngine  != NULL; }
  const bool IsRuby   () const { return m_rubyEngine != NULL; }

  // The "Executing <type> script \"<name>\"" line ExecuteLua emits before every
  // callback (lua_scripting.cpp), so the five embedded engines announce
  // themselves the same way.  Suppressed for the two per-frame callbacks, which
  // would otherwise flood the output window.  Defined in lua_scripting.cpp.
  void   TraceCallback (LPCTSTR szType, LPCTSTR szProcedure);

  void   OpenPython ();   void   OpenPerl ();   void   OpenJS ();   void   OpenTcl ();   void   OpenRuby ();
  void   ClosePython ();  void   ClosePerl ();  void   CloseJS ();  void   CloseTcl ();  void   CloseRuby ();
  bool   ParsePython (const CString &, const CString &);
  bool   ParsePerl   (const CString &, const CString &);
  bool   ParseJS     (const CString &, const CString &);
  bool   ParseTcl    (const CString &, const CString &);
  bool   ParseRuby   (const CString &, const CString &);
  DISPID GetPythonDispid (const CString &);
  DISPID GetPerlDispid   (const CString &);
  DISPID GetJSDispid     (const CString &);
  DISPID GetTclDispid    (const CString &);
  DISPID GetRubyDispid   (const CString &);

  // Rich + simple callback overloads — exact mirrors of the ExecuteLua pair, so
  // the same doc/evaluate/plugins/timers/mushview call sites drive every engine.
  #define MC_DECL_EXEC(Name)                                                       \
    bool Name (DISPID & dispid, LPCTSTR szProcedure, const unsigned short iReason,  \
               LPCTSTR szType, LPCTSTR szReason,                                    \
               list<double> & nparams, list<string> & sparams,                     \
               long & nInvocationCount, const t_regexp * regexp = NULL,             \
               map<string, string> * table = NULL, CPaneLine * paneline = NULL,     \
               bool * result = NULL, const bool bLowercaseWildcards = false);       \
    bool Name (DISPID & dispid, LPCTSTR szProcedure, const unsigned short iReason,  \
               LPCTSTR szType, LPCTSTR szReason, CString strParam,                  \
               long & nInvocationCount, CString & result);
  MC_DECL_EXEC (ExecutePython)
  MC_DECL_EXEC (ExecutePerl)
  MC_DECL_EXEC (ExecuteJS)
  MC_DECL_EXEC (ExecuteTcl)
  MC_DECL_EXEC (ExecuteRuby)
  #undef MC_DECL_EXEC

  CPythonEngine * m_pyEngine;               // opaque per-engine engine state
  CPerlEngine   * m_perlEngine;             // (null unless this is that language)
  CJSEngine     * m_jsEngine;
  CTclEngine    * m_tclEngine;
  CRubyEngine   * m_rubyEngine;
#endif

  // True when a trigger/alias/timer/plugin callback should run through the
  // Lua-style call path — Lua itself, or any embedded alt-language engine.
  const bool UsesEmbeddedCallback () const
    { return IsLua () || IsPython () || IsPerl () || IsJS () || IsTcl () || IsRuby (); }

  private:

#ifdef _WIN32
  IActiveScript       * m_IActiveScript;          // VBscript interface
  IActiveScriptParse  * m_IActiveScriptParse;     // parser
  CActiveScriptSite   * m_site;                   // our local site (world object)
  IDispatch           * m_pDispatch;              // script engine dispatch pointer
#endif

  CMUSHclientDoc      * m_pDoc;                   // related MUSHclient document

  CString               m_strLanguage;        // language, (vbscript, jscript, perlscript)

  };

int RegisterLuaRoutines (lua_State * L);
int DisableDLLs (lua_State * L);
#ifndef _WIN32
// Port-only world.* extras kept beyond the Windows Lua surface
// (currently just ExpandVariables); defined in lua_utils_portable.cpp,
// registered AFTER RegisterLuaRoutines by mushcore builds only.
void RegisterPortableLuaExtras (lua_State * L);
#endif
#include "lua_helpers.h"


// ----------- here used for Lua in choosing from combo-box

class CKeyValuePair
  {

  public:
    CKeyValuePair () :
        bNumber_ (false), iKey_ (0.0) { };  // default constructor

    CKeyValuePair (const string sValue) : 
        bNumber_ (false), iKey_ (0.0), sValue_ (sValue) { };  // constructor

    // copy constructor
    CKeyValuePair (const CKeyValuePair & k)
                    : bNumber_ (k.bNumber_), 
                      sKey_ (k.sKey_),
                      iKey_ (k.iKey_),
                      sValue_ (k.sValue_)
                       {};

    // operator =
    const CKeyValuePair & operator= (const CKeyValuePair & rhs)
      {
      bNumber_ = rhs.bNumber_;
      sKey_  = rhs.sKey_;
      iKey_ = rhs.iKey_;
      sValue_ = rhs.sValue_;
      return *this;
      };


  bool   bNumber_; // true if key a number, false if a string

  string sKey_;    // key if string
  double iKey_;    // key if number?

  string sValue_;  // value 

  };   // end of class  CStringValuePair

// call a C function with the Lua environment
void CallLuaCFunction (lua_State * L, lua_CFunction fn);