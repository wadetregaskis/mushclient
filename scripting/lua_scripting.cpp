// General Lua scripting support

#include "stdafx.h"
  #include "../mainfrm.h"
  #include "../MUSHclient.h"
  #include "../doc.h"
  #include "../dialogs/ScriptErrorDlg.h"
#ifdef _WIN32
  #include <io.h>     // Windows low-level I/O (_open, _read, …)
#endif
  #include <fcntl.h>
  #include "../pcre/config.h"
  #include "../pcre/pcre_internal.h"
  #include "../luacom/luacom.h"

set<string> LuaFunctionsSet;
set<string> LuaTablesSet;

extern "C"
  {
  LUALIB_API int luaopen_rex(lua_State *L);
  LUALIB_API int luaopen_bits(lua_State *L);
  LUALIB_API int luaopen_compress(lua_State *L);
  LUALIB_API int luaopen_bc(lua_State *L);
  LUALIB_API int luaopen_lsqlite3(lua_State *L);
  LUALIB_API int luaopen_lpeg (lua_State *L);
  }

//LUALIB_API int luaopen_trie(lua_State *L);

LUALIB_API int luaopen_progress_dialog(lua_State *L);

static void BuildOneLuaFunction (lua_State * L, const char * sTableName)
  {
  lua_settop (L, 0);    // get rid of stuff lying around

  // get global table
  lua_getglobal (L, sTableName);  

  const int table = 1;

  if (!lua_istable (L, table))
    return;  // aha! caught you!

  // standard Lua table iteration
  for (lua_pushnil (L); lua_next (L, table) != 0; lua_pop (L, 1))
    {
    // extract function names
    if (lua_isfunction (L, -1))
      {
      string sName (luaL_checkstring (L, -2));

      // don't add stuff like __index
      if (sName [0] != '_')
        {
        string sFullName;

        // global table will not have _G prefix
        if (strcmp (sTableName, "_G") != 0)
          {
          sFullName= sTableName;
          sFullName += ".";
          }

        // now the name
        sFullName += sName;

        // add to set
        LuaFunctionsSet.insert (sFullName);
        }  // not prefixed by _
      }   // end if function
    }  // end for loop

  lua_pop (L, 1); // get rid of global table now
  } // end of BuildOneLuaFunction

static void BuildOneLuaTable (lua_State * L, const char * sTableName)
  {
  lua_settop (L, 0);    // get rid of stuff lying around

  // get global table
  lua_getglobal (L, sTableName);  

  const int table = 1;

  if (!lua_istable (L, table))
    return;  // aha! caught you!

  // standard Lua table iteration
  for (lua_pushnil (L); lua_next (L, table) != 0; lua_pop (L, 1))
    {
    if (lua_isstring (L, -2))
      {

      string sName (lua_tostring (L, -2));

      string sFullName = sTableName;
      sFullName += "." + sName;   // table.entry, eg. sendto.world

      // add to set
      LuaTablesSet.insert (sFullName);
      } // if string key
  }  // end for loop

  lua_pop (L, 1); // get rid of global table now
  } // end of BuildOneLuaTable


// for world completion, and help lookup, find all functions in the following tables
static void BuildLuaFunctions (lua_State * L)
  {
  const char * table_names [] = {
      "_G",
      "string",
      "package",
      "os",
      "io",
      "bc",
      "progress",
      "bit",
      "rex",
      "utils",
      "table",
      "math",
      "debug",
      "coroutine",
      "lpeg",
      "sqlite3",

      // add more tables here

      ""   // end of table marker
    };

  // add all functions from each table
  for (int i = 0; table_names [i] [0]; i++)
    BuildOneLuaFunction (L, table_names [i]);

  // experimental function - don't show it
  LuaFunctionsSet.erase ("newproxy");

  } // end of BuildLuaFunctions

// for word completion, find all entries in the following tables
static void BuildLuaTables (lua_State * L)
  {
  const char * table_names [] = {
      "trigger_flag",
      "alias_flag",
      "timer_flag",
      "custom_colour",
      "error_code",
//      "error_desc",   // don't do this, wrong table type
      "sendto",
      "miniwin",

      // add more tables here

      ""   // end of table marker
    };

  // add all functions from each table
  for (int i = 0; table_names [i] [0]; i++)
    BuildOneLuaTable (L, table_names [i]);

  } // end of BuildLuaTables

#ifdef _WIN32
// sigh - luacom_open returns void
int luacom_open_glue (lua_State *L)
  {
  luacom_open (L);
  return 0;
  }
#endif

// I need this extra function to avoid:
// Compiler Error C2712
// cannot use __try in functions that require object unwinding

void CScriptEngine::OpenLuaDelayed ()
  {
  L = MakeLuaState();   /* opens Lua */
  if (!L)
    return;         // can't open Lua

  luaL_openlibs (L);           // open all standard Lua libraries

  lua_pushlightuserdata(L, (void *)m_pDoc);    /* push value */
  lua_setfield (L, LUA_REGISTRYINDEX, DOCUMENT_STATE);  // document pointer into registry

  CallLuaCFunction (L, RegisterLuaRoutines);    // our stuff
#if !defined(_WIN32) && !defined(MUSHCLIENT_FULL_BUILD)
  RegisterPortableLuaExtras (L);   // mushcore-only world.* extras (ExpandVariables)
#endif
  CallLuaCFunction (L, luaopen_rex);            // regular expression library
  CallLuaCFunction (L, luaopen_bits);           // bit manipulation library
  CallLuaCFunction (L, luaopen_compress);       // compression (utils) library
  CallLuaCFunction (L, luaopen_progress_dialog);// progress dialog (wxProgressDialog on the port)
  CallLuaCFunction (L, luaopen_bc);             // open bc library
  CallLuaCFunction (L, luaopen_lsqlite3);       // open sqlite library
  CallLuaCFunction (L, luaopen_lpeg);           // open lpeg library

  lua_settop(L, 0);   // clear stack

  // for function-name completion, and help
  if (LuaFunctionsSet.empty ())
    {
    BuildLuaFunctions (L);
    BuildLuaTables (L);
    }

  // unless they explicitly enable it, remove ability to load DLLs
  DisableDLLs (L);

#ifdef _WIN32
  // add luacom to package.preload (Windows COM automation, not available on macOS)
  lua_getglobal (L, LUA_LOADLIBNAME);  // package table

  if (lua_istable (L, -1))    // if it exists and is a table
    {
    lua_getfield (L, -1, "preload");  // get preload table inside it

    if (lua_istable (L, -1))   // preload table exists
      {
      lua_pushcfunction(L, luacom_open_glue);   // luacom open
      lua_setfield(L, -2, "luacom");
      } // have package.preload table

    lua_pop (L, 1);   // get rid of preload table from stack
    } // have package table

  lua_pop (L, 1);   // get rid of package table from stack
#endif

#if !defined(_WIN32) && defined(MUSHCLIENT_FULL_BUILD)
  // ── Lua module search path (mushapp / GUI build) ──────────────────────
  // On Windows, plugins locate the bundled Lua modules (movewindow, mapper,
  // gauge, …) via the install directory, which is on package.path implicitly.
  // The non-Windows build must add the module directory explicitly, otherwise
  // every plugin that require()s a shipped module fails to load.  We add the
  // app bundle's Resources/lua (the canonical macOS location) and a lua/
  // folder beside the executable (flat / Linux installs).
  // m_strMUSHclientFileName holds the executable's *directory* (with trailing
  // separator) — see CMUSHclientApp::InitInstance.  Guarded to the full build
  // because mushcore's App stub (CLI/tests) has no m_strMUSHclientFileName, and
  // that path does not load module-using plugins.
  {
    CString strExeDir = App.m_strMUSHclientFileName;
    if (!strExeDir.IsEmpty ())
      {
      CString strCmd;
      strCmd.Format (
        "do local bases = { [[%slua/]], [[%s../Resources/lua/]] }\n"
        "for _, b in ipairs (bases) do\n"
        "  package.path = b .. [[?.lua;]] .. b .. [[?/init.lua;]] .. package.path\n"
        "end end",
        (LPCTSTR) strExeDir, (LPCTSTR) strExeDir);
      ParseLua (strCmd, "Lua module search path");
      }
  }
#endif

  // this is so useful I am adding it in (see check.lua)
  ParseLua ( \
  "function check (result)  \
    if result ~= error_code.eOK then\
      error (error_desc [result] or \
             string.format (\"Unknown error code: %i\", result), 2) end; end", 
  "Check function");

  // preliminary sand-box stuff
  m_pDoc->m_iCurrentActionSource = eLuaSandbox;
  ParseLua (App.m_strLuaScript, "Sandbox");

  m_pDoc->m_iCurrentActionSource = eUnknownActionSource;

  lua_settop(L, 0);   // clear stack

  } // end of OpenLuaDelayed

void CScriptEngine::OpenLua ()
  {

   OpenLuaDelayed ();

  }   // end of CScriptEngine::OpenLua

void CScriptEngine::CloseLua ()
  {
  if (L)
    {
    lua_close (L);
    L = NULL;
    }
  }  // end of CScriptEngine::CloseLua

// send some code to Lua to be parsed
bool CScriptEngine::ParseLua (const CString & strCode, const CString & strWhat)
  {

  // safety check ;)
  if (!L)
    return true;

  LARGE_INTEGER start, 
                finish;

  if (App.m_iCounterFrequency)
    QueryPerformanceCounter (&start);
  else
    { start.QuadPart = 0; finish.QuadPart = 0; }

  // note - an error here is a *compile* error
  if (luaL_loadbuffer(L, strCode, strCode.GetLength (), strWhat))
    {
    LuaError (L, "Compile error", "", "", "", m_pDoc);
    return true;
    }

  int error = CallLuaWithTraceBack (L, 0, 0);

  // note - an error here is a *runtime* error
  if (error)
    {
    LuaError (L, "Run-time error", "", "", "", m_pDoc);
    return true;
    }

  lua_settop(L, 0);   // clear stack

  if (App.m_iCounterFrequency)
    {
    QueryPerformanceCounter (&finish);
    m_pDoc->m_iScriptTimeTaken += finish.QuadPart - start.QuadPart;
    if (m_pDoc->m_CurrentPlugin)
      m_pDoc->m_CurrentPlugin->m_iScriptTimeTaken += finish.QuadPart - start.QuadPart;
    }

  return false;

  }

// For Lua we will simply use the DISPID as a flag to indicate if we found
// the function or not, to speed up subsequent calls (calls to non-existent 
// functions). Also, if the function later has an error we set the DISPID
// to DISPID_UNKNOWN as a flag to not call it continuously.

// Version 3.75+ supports dotted functions (eg. string.gsub)

DISPID CScriptEngine::GetLuaDispid (const CString & strName)
  {
  return (L && FindLuaFunction (L, strName))
          ? 1 : DISPID_UNKNOWN;  // if known 1 is flag, otherwise DISPID_UNKNOWN
       
  }


// ── ReportScriptError ───────────────────────────────────────────────────────
// The routing half of LuaError, factored out so EVERY engine uses it.
//
// It decides where a script error goes: the canonical CScriptErrorDlg unless
// World Properties > Scripting > "Note errors to the output window" is ticked
// (m_bScriptErrorsToOutputWindow), in which case four ColourNotes; plus the
// "log_script_errors" file.  The dialog is also how a user TICKS that box --
// from the error itself.
//
// On Windows this is what CScriptEngine::ShowError does for the COM languages
// (scripting/scriptengine.cpp), so every language there obeys the setting.  On
// the port the five embedded engines each ColourNoted directly: the setting was
// a dead reader for them, the dialog never appeared, and its "show errors in the
// output window next time" checkbox was unreachable for anyone whose world is
// not scripted in Lua.
//
// Pure extraction -- the body is LuaError's former tail, unchanged.
void ReportScriptError (CMUSHclientDoc * pDoc,
                        LPCTSTR strEvent,
                        LPCTSTR strCalledBy,
                        LPCTSTR strDescription,
                        LPCTSTR strRaisedBy,
                        const bool bImmediate,
                        const int nLine)
  {
#ifndef _WIN32
  // Console diagnostics.  Port-only, deliberately: Windows has no console and
  // has never emitted these, so they must not leak into the Windows build.
  fprintf (stderr, "[LuaError] %s\n", strEvent);
  fprintf (stderr, "  %s\n", (const char *) strRaisedBy);
  if (strlen (strCalledBy) > 0)
    fprintf (stderr, "  %s\n", (const char *) strCalledBy);
  fprintf (stderr, "  %s\n", (const char *) strDescription);
#endif

  CScriptErrorDlg dlg;
  dlg.m_strCalledBy   = strCalledBy;
  dlg.m_strEvent       = strEvent;
  dlg.m_strDescription = strDescription;
  dlg.m_strRaisedBy    = strRaisedBy;

  if (!pDoc || !pDoc->m_bScriptErrorsToOutputWindow)
    {
    if (pDoc)
      dlg.m_bHaveDoc = true;
    dlg.DoModal ();

    if (pDoc && dlg.m_bUseOutputWindow)
      {
      pDoc->m_bScriptErrorsToOutputWindow = true;
      pDoc->SetModifiedFlag (TRUE);
      }
    }
  else
    {
    pDoc->ColourNote (SCRIPTERRORFORECOLOUR, SCRIPTERRORBACKCOLOUR, strEvent);
    pDoc->ColourNote (SCRIPTERRORFORECOLOUR, SCRIPTERRORBACKCOLOUR, strRaisedBy);
    pDoc->ColourNote (SCRIPTERRORFORECOLOUR, SCRIPTERRORBACKCOLOUR, strCalledBy);
    pDoc->ColourNote (SCRIPTERRORFORECOLOUR, SCRIPTERRORBACKCOLOUR, strDescription);
    if (!bImmediate)
      pDoc->ShowErrorLines (nLine);
#ifndef _WIN32
    // Flush the new notes to the output pane immediately.  Port-only: on
    // Windows this would be an extra forced repaint from inside script.
    pDoc->UpdateAllViews (nullptr);
#endif
    }

  // if option "log_script_errors" is active, append to the error log file
  if (pDoc && pDoc->m_bLogScriptErrors)
    {
    // m_strDefaultLogFileDirectory is an MFC CString on Windows (LPCTSTR cast,
    // no c_str()), but on the port it is the shim CString (mushapp) or a real
    // std::string (mushcore) — both of which expose c_str().  So Windows casts,
    // everywhere else uses c_str(); a plain assign would fail on the shim CString.
#ifdef _WIN32
    string fileName = (LPCTSTR) App.m_strDefaultLogFileDirectory;
#else
    string fileName = App.m_strDefaultLogFileDirectory.c_str();
#endif
    fileName += "script_error_log.txt";

    FILE * errorLogFile = fopen (fileName.c_str(), "a+");
    if (!errorLogFile)
      {
      fprintf (stderr, "Cannot open error log file: %s\n", fileName.c_str());
      }
    else
      {
      CTime timeNow = CTime::GetCurrentTime();
      CString strConnected = timeNow.Format (
          TranslateTime ("\n\n--- Scripting error on %A, %B %d, %Y, %#I:%M %p ---\n\n"));
      fputs ((LPCTSTR) strConnected, errorLogFile);
      fputs (strEvent, errorLogFile);
      fputs ("\n", errorLogFile);
      fputs (strRaisedBy, errorLogFile);
      fputs ("\n", errorLogFile);
      fputs (strCalledBy, errorLogFile);
      fputs ("\n", errorLogFile);
      fputs (strDescription, errorLogFile);
      fputs ("\n", errorLogFile);
#ifdef _WIN32
      if (!bImmediate)
        pDoc->WriteErrorLines (nLine, errorLogFile);
#endif
      fclose (errorLogFile);
      }
    }

  }  // end of ReportScriptError

void LuaError (lua_State *L,
                LPCTSTR strEvent,
                LPCTSTR strProcedure,
                LPCTSTR strType,
                LPCTSTR strReason,
                CMUSHclientDoc * pDoc)
  {
  // Collect the description and caller info into plain CStrings so both
  // the Windows dialog path and the portable path can share them.
  CString strCalledBy;
  if (strlen (strProcedure) != 0)
    {
    strCalledBy = "Function/Sub: ";
    strCalledBy += strProcedure;
    strCalledBy += " called by ";
    strCalledBy += strType;
    strCalledBy += ENDLINE;
    strCalledBy += "Reason: ";
    strCalledBy += strReason;
    }
  else
    strCalledBy = "Immediate execution";

  CString strDescription = lua_tostring(L, -1);
  lua_settop(L, 0);   // clear stack

  CString strRaisedBy = "No active world";
  if (pDoc)
    {
    if (pDoc->m_CurrentPlugin)
      {
      strRaisedBy = "Plugin: " + pDoc->m_CurrentPlugin->m_strName;
      strRaisedBy += " (called from world: " + pDoc->m_mush_name + ")";
      }
    else
      strRaisedBy = "World: " + pDoc->m_mush_name;
    }

  // work out the line number where the error is
  bool bImmediate = true;
  int nLine = 0;

  if (strDescription.Left (18) == "[string \"Plugin\"]:")
    {
    bImmediate = false;
    nLine = atoi (strDescription.Mid (18));
    }
  else if (strDescription.Left (23) == "[string \"Script file\"]:")
    {
    bImmediate = false;
    nLine = atoi (strDescription.Mid (23));
    }

  // ONE BODY, EVERY BUILD.  This used to be an #ifdef _WIN32 / #else pair, and
  // the port's arm noted every error to the output window unconditionally.  So
  // World Properties > Scripting > "Note errors to the output window"
  // (m_bScriptErrorsToOutputWindow) did nothing on the port: it behaved
  // permanently as if the box were ticked, and the Scripting Error dialog never
  // appeared -- which also meant its "show errors in output window next time"
  // checkbox, the way you TICK that box from the error itself, was unreachable.
  // The flag's only other readers are in scripting/scriptengine.cpp, which is
  // #ifdef _WIN32 from top to bottom, so nothing off Windows read it at all.
  //
  // The dialog is fully portable and already built into mushgui
  // (dialogs/ScriptErrorDlg.cpp, template IDD_SCRIPT_ERROR in the recovered
  // resources); the headless build gets an inert stub from
  // platform/doc_flip_gui_stubs.cpp, whose DoModal returns IDCANCEL.
  //
  // Note this is the branch taken when there is no document at all -- several
  // callers pass none (LuaError's pDoc defaults to NULL) -- so a doc-less error
  // now raises the dialog on the port too, exactly as it always has on Windows.
  // CScriptErrorDlg::OnInitDialog hides the "raised by" and "use output window"
  // controls when m_bHaveDoc is false, which is what makes that case read
  // sensibly.

  ReportScriptError (pDoc, strEvent, strCalledBy, strDescription,
                     strRaisedBy, bImmediate, nLine);
  }   // end of LuaError

// returns true if error
bool CScriptEngine::ExecuteLua (DISPID & dispid,  // dispatch ID, will be set to DISPID_UNKNOWN on an error
                                LPCTSTR szProcedure,      // eg. ON_TRIGGER_XYZ
                                const unsigned short iReason,  // value for m_iCurrentActionSource
                                LPCTSTR szType,           // eg. trigger, alias
                                LPCTSTR szReason,         // eg. trigger subroutine XXX
                                list<double> & nparams,   // list of number parameters
                                list<string> & sparams,   // list of string parameters
                                long & nInvocationCount,  // count of invocations
                                const t_regexp * regexp,  // regular expression (for triggers, aliases)
                                map<string, string> * table,   // map of other things
                                CPaneLine * paneline,     // and the line (for triggers)
                                bool * result,            // where to put result
                                const bool bLowercaseWildcards) // lower-case the wildcards?


  {

#ifndef _WIN32
  // Alt-language world (the call sites gate on UsesEmbeddedCallback(), so an
  // embedded Python/Perl/JS/Tcl engine lands here).  The forward has to give
  // the callback the same SURROUNDINGS Lua's own body gets further down, because
  // the dispatch happens before any of it:
  //   * m_iNoteStyle saved and reset to NORMAL, restored UNCONDITIONALLY (:765).
  //     Without it a style set by world.SetNoteStyle inside one handler leaked
  //     into every later note and out into the next callback.
  //   * the elapsed time added to the world and, inside a plugin, to the
  //     plugin -- but only when the call did NOT error, which is where Lua's
  //     `if (error) return true;` sits relative to its timing block (:767-781).
  //     Without it world Trace timings and GetScriptTime read zero for five of
  //     the six languages.
  if (m_pyEngine || m_perlEngine || m_jsEngine || m_tclEngine || m_rubyEngine)
    {
    const unsigned short iOldStyle = m_pDoc->m_iNoteStyle;
    m_pDoc->m_iNoteStyle = NORMAL;

    LARGE_INTEGER embStart, embFinish;
    if (App.m_iCounterFrequency)
      QueryPerformanceCounter (&embStart);
    else
      { embStart.QuadPart = 0; embFinish.QuadPart = 0; }

    bool err = true;
    if      (m_pyEngine)   err = ExecutePython (dispid, szProcedure, iReason, szType, szReason,
                                                nparams, sparams, nInvocationCount, regexp, table, paneline, result, bLowercaseWildcards);
    else if (m_perlEngine) err = ExecutePerl   (dispid, szProcedure, iReason, szType, szReason,
                                                nparams, sparams, nInvocationCount, regexp, table, paneline, result, bLowercaseWildcards);
    else if (m_jsEngine)   err = ExecuteJS     (dispid, szProcedure, iReason, szType, szReason,
                                                nparams, sparams, nInvocationCount, regexp, table, paneline, result, bLowercaseWildcards);
    else if (m_tclEngine)  err = ExecuteTcl    (dispid, szProcedure, iReason, szType, szReason,
                                                nparams, sparams, nInvocationCount, regexp, table, paneline, result, bLowercaseWildcards);
    else                   err = ExecuteRuby   (dispid, szProcedure, iReason, szType, szReason,
                                                nparams, sparams, nInvocationCount, regexp, table, paneline, result, bLowercaseWildcards);

    m_pDoc->m_iNoteStyle = iOldStyle;

    if (!err && App.m_iCounterFrequency)
      {
      QueryPerformanceCounter (&embFinish);
      const LONGLONG elapsed = embFinish.QuadPart - embStart.QuadPart;
      m_pDoc->m_iScriptTimeTaken += elapsed;
      if (m_pDoc->m_CurrentPlugin)
        m_pDoc->m_CurrentPlugin->m_iScriptTimeTaken += elapsed;
      }

    return err;
    }
#endif

  // Deliberately unused past this point: Lua's callback gets the wildcards raw,
  // on Windows too.  See t_regexp::GetCallbackWildcards.
  (void) bLowercaseWildcards;

  // safety check ;)
  if (!L)
    return false;

  // don't do it if previous problems
  if (dispid == DISPID_UNKNOWN)
    return false;

  lua_settop (L, 0);  // start with empty stack

  LARGE_INTEGER start, 
                finish;

  // do not trace OnPluginDrawOutputWindow or OnPluginTick because they can spam the output window
  if (ON_PLUGIN_DRAW_OUTPUT_WINDOW != szProcedure &&
      ON_PLUGIN_TICK != szProcedure)
    // Trace takes a FORMAT; szProcedure is a plugin-supplied callback name, so
    // handing the composed string straight in would let a '%' in it read a
    // vararg that does not exist.
    m_pDoc->Trace ("%s",
      (LPCTSTR) TFormat ("Executing %s script \"%s\"", szType, szProcedure));

  if (App.m_iCounterFrequency)
    QueryPerformanceCounter (&start);
  else
    { start.QuadPart = 0; finish.QuadPart = 0; }

  unsigned short iOldStyle = m_pDoc->m_iNoteStyle;
  m_pDoc->m_iNoteStyle = NORMAL;    // back to default style

  if (!GetNestedFunction (L, szProcedure, true))
    {
    dispid = DISPID_UNKNOWN;   // stop further invocations
    return true;    // error return
    }

  int paramCount = 0;

  // push all supplied number parameters
  for (list<double>::const_iterator niter = nparams.begin ();
       niter != nparams.end ();
       niter++)
     lua_pushnumber (L, *niter);

  paramCount += nparams.size ();

  // push all supplied string parameters
  for (list<string>::const_iterator siter = sparams.begin ();
       siter != sparams.end ();
       siter++)
     lua_pushlstring (L, siter->c_str (), siter->size ());

  paramCount += sparams.size ();

// if we have a regular expression, push the wildcards

  if (regexp)
    {
    int i;
    // Initialised: pcre_fullinfo returns an error WITHOUT writing its output
    // (PCRE_ERROR_NULL / BADMAGIC), and `namecount` then drives a loop over
    // `name_table` -- both garbage.  Zero degrades to "no wildcards", which is
    // the right answer for a regexp we cannot interrogate.
    int ncapt = 0;
    int namecount = 0;
    unsigned char *name_table = NULL;
    int name_entry_size = 0;
    unsigned char *tabptr;
    lua_newtable(L);                                                            
    paramCount++;   // we have one more parameter to the call
    pcre_fullinfo(regexp->m_program, regexp->m_extra, PCRE_INFO_CAPTURECOUNT, &ncapt);

    for (i = 0; i <= ncapt; i++) 
      {
      string wildcard (regexp->GetWildcard (i));
      lua_pushlstring (L, wildcard.c_str (), wildcard.size ());
      lua_rawseti (L, -2, i);
    }

    /* now do named subpatterns  */
    pcre_fullinfo(regexp->m_program, regexp->m_extra, PCRE_INFO_NAMECOUNT, &namecount);
    if (namecount > 0)
      {
      int jchanged = 0;
      pcre_fullinfo(regexp->m_program, regexp->m_extra, PCRE_INFO_NAMETABLE, &name_table);
      pcre_fullinfo(regexp->m_program, regexp->m_extra, PCRE_INFO_NAMEENTRYSIZE, &name_entry_size);
      pcre_fullinfo(regexp->m_program, regexp->m_extra, PCRE_INFO_JCHANGED, &jchanged);
      tabptr = name_table;
      set<string> found_strings;
      for (i = 0; i < namecount; i++, tabptr += name_entry_size) 
        {
        int n = (tabptr[0] << 8) | tabptr[1];
        const unsigned char * name = tabptr + 2;
        // if duplicates were possible then ...
        if (jchanged)
          {
          // this code is to ensure that we don't find a match (eg. mob = Kobold)
          // and then if duplicates were allowed, replace Kobold with false.

          string sName = (LPCTSTR) name;

          // for duplicate names, see if we already added this name
          if (found_strings.find (sName) != found_strings.end ())
            {
            // do not replace if this one is out of range
            if (n < 0 || n > ncapt || regexp->GetWildcard (n) == "")
              continue;
            } // end of duplicate
          else
            found_strings.insert (sName);
          }

        lua_pushstring (L, (LPCTSTR) name);
        if (n >= 0 && n <= ncapt) 
          {
          string wildcard (regexp->GetWildcard (n));
          lua_pushlstring (L, wildcard.c_str (), wildcard.size ());
          }
        else
          lua_pushnil (L);  /* n out of range */
        lua_settable (L, -3);
        
        }   // end of wildcard loop
      } // end of having named wildcards
    } // end of having a regexp

  if (table)
    {
    lua_newtable (L);                                                            
    paramCount++;   // we have one more parameter to the call

    // add each item to the table
    for (map<string, string>::const_iterator iter = table->begin ();
         iter != table->end ();
         iter++)
           {
           lua_pushstring (L, iter->first.c_str ());
           lua_pushstring (L, iter->second.c_str ());
           lua_settable(L, -3);
           }  // end of doing each one

    } // end of having an optional table

  if (paneline)
    {
    lua_newtable(L);                                                            
    paramCount++;   // we have one more parameter to the call
    int i = 1;          // style run number

    for (CPaneStyleVector::iterator style_it = paneline->m_vStyles.begin (); 
         style_it != paneline->m_vStyles.end (); 
         style_it++, i++)
      {
      lua_newtable(L);                                                            
      MakeTableItem     (L, "text",         (*style_it)->m_sText); 
      MakeTableItem     (L, "length",       (*style_it)->m_sText.length ());  
      MakeTableItem     (L, "textcolour",   (*style_it)->m_cText);  
      MakeTableItem     (L, "backcolour",   (*style_it)->m_cBack);  
      MakeTableItem     (L, "style",        (*style_it)->m_iStyle); 

      lua_rawseti (L, -2, i);  // set table item as number of style
      }

    }   // end of having an optional style run thingo

  if (iReason != eDontChangeAction)
    m_pDoc->m_iCurrentActionSource = iReason;

  int error = CallLuaWithTraceBack (L, paramCount, LUA_MULTRET);

  if (iReason != eDontChangeAction)
    m_pDoc->m_iCurrentActionSource = eUnknownActionSource;

// -----------------

  m_pDoc->m_iNoteStyle = iOldStyle;

  if (error)
    {
    dispid = DISPID_UNKNOWN;   // stop further invocations
    LuaError (L, "Run-time error", szProcedure, szType, szReason, m_pDoc);
    return true;    // error return
    }

  nInvocationCount++;   // count number of times used

  if (App.m_iCounterFrequency)
    {
    QueryPerformanceCounter (&finish);
    m_pDoc->m_iScriptTimeTaken += finish.QuadPart - start.QuadPart;
    if (m_pDoc->m_CurrentPlugin)
      m_pDoc->m_CurrentPlugin->m_iScriptTimeTaken += finish.QuadPart - start.QuadPart;
    }

  if (result)
    {
    *result = true;

    // if a boolean result wanted, return it

    if (lua_gettop (L) > 0)
      {
      if (lua_isboolean (L, 1))
        *result = lua_toboolean (L, 1);
      else
        *result = lua_tonumber (L, 1);  // I use number rather than boolean
                                       // because 0 is considered true in Lua
      }
    }   // end of result wanted

  lua_settop (L, 0);  // discard any results now
  
  return false;   // no error
  } // end of CScriptEngine::ExecuteLua 

#ifndef _WIN32
// Announce a callback the way ExecuteLua does, for the five embedded engines.
// Trace is a format string and szProcedure is plugin-supplied, so the text goes
// through as an ARGUMENT -- a callback whose name contains a % must not read a
// vararg that was never pushed.
void CScriptEngine::TraceCallback (LPCTSTR szType, LPCTSTR szProcedure)
  {
  if (!m_pDoc) return;
  if (ON_PLUGIN_DRAW_OUTPUT_WINDOW == szProcedure || ON_PLUGIN_TICK == szProcedure)
    return;                       // per-frame: would flood the output window
  m_pDoc->Trace ("%s",
    (LPCTSTR) TFormat ("Executing %s script \"%s\"", szType, szProcedure));
  }
#endif // !_WIN32

// returns true if script error
bool CScriptEngine::ExecuteLua (DISPID & dispid,          // dispatch ID, will be set to DISPID_UNKNOWN on an error
                               LPCTSTR szProcedure,      // eg. ON_TRIGGER_XYZ
                               const unsigned short iReason,  // value for m_iCurrentActionSource
                               LPCTSTR szType,           // eg. trigger, alias
                               LPCTSTR szReason,         // eg. trigger subroutine XXX
                               CString strParam,         // string parameter
                               long & nInvocationCount,  // count of invocations
                               CString & result)         // where to put result
  {

#ifndef _WIN32
  // Same surroundings as the rich overload above -- see the comment there for
  // why the note style and the timing have to live at the dispatch point.
  if (m_pyEngine || m_perlEngine || m_jsEngine || m_tclEngine || m_rubyEngine)
    {
    const unsigned short iOldStyle = m_pDoc->m_iNoteStyle;
    m_pDoc->m_iNoteStyle = NORMAL;

    LARGE_INTEGER embStart, embFinish;
    if (App.m_iCounterFrequency)
      QueryPerformanceCounter (&embStart);
    else
      { embStart.QuadPart = 0; embFinish.QuadPart = 0; }

    bool err = true;
    if      (m_pyEngine)   err = ExecutePython (dispid, szProcedure, iReason, szType, szReason,
                                                strParam, nInvocationCount, result);
    else if (m_perlEngine) err = ExecutePerl   (dispid, szProcedure, iReason, szType, szReason,
                                                strParam, nInvocationCount, result);
    else if (m_jsEngine)   err = ExecuteJS     (dispid, szProcedure, iReason, szType, szReason,
                                                strParam, nInvocationCount, result);
    else if (m_tclEngine)  err = ExecuteTcl    (dispid, szProcedure, iReason, szType, szReason,
                                                strParam, nInvocationCount, result);
    else                   err = ExecuteRuby   (dispid, szProcedure, iReason, szType, szReason,
                                                strParam, nInvocationCount, result);

    m_pDoc->m_iNoteStyle = iOldStyle;

    if (!err && App.m_iCounterFrequency)
      {
      QueryPerformanceCounter (&embFinish);
      const LONGLONG elapsed = embFinish.QuadPart - embStart.QuadPart;
      m_pDoc->m_iScriptTimeTaken += elapsed;
      if (m_pDoc->m_CurrentPlugin)
        m_pDoc->m_CurrentPlugin->m_iScriptTimeTaken += elapsed;
      }

    return err;
    }
#endif

  // safety check ;)
  if (!L)
    return false;

  // don't do it if previous problems
  if (dispid == DISPID_UNKNOWN)
    return false;

  lua_settop (L, 0);  // start with empty stack

  LARGE_INTEGER start, 
                finish;

  // do not trace OnPluginDrawOutputWindow or OnPluginTick because they can spam the output window
  if (ON_PLUGIN_DRAW_OUTPUT_WINDOW != szProcedure &&
      ON_PLUGIN_TICK != szProcedure)
    // Same reason as the sibling call above: Trace takes a format, and
    // szProcedure is plugin-supplied.
    m_pDoc->Trace ("%s",
      (LPCTSTR) TFormat ("Executing %s script \"%s\"", szType, szProcedure));

  if (App.m_iCounterFrequency)
    QueryPerformanceCounter (&start);
  else
    { start.QuadPart = 0; finish.QuadPart = 0; }

  unsigned short iOldStyle = m_pDoc->m_iNoteStyle;
  m_pDoc->m_iNoteStyle = NORMAL;    // back to default style

  if (!GetNestedFunction (L, szProcedure, true))
    {
    dispid = DISPID_UNKNOWN;   // stop further invocations
    return true;    // error return
    }

  lua_pushlstring (L, strParam, strParam.GetLength ());    // the solitary argument

  if (iReason != eDontChangeAction)
    m_pDoc->m_iCurrentActionSource = iReason;

  int error = CallLuaWithTraceBack (L, 1, LUA_MULTRET);

  if (iReason != eDontChangeAction)
    m_pDoc->m_iCurrentActionSource = eUnknownActionSource;

  m_pDoc->m_iNoteStyle = iOldStyle;

  if (error)
    {
    dispid = DISPID_UNKNOWN;   // stop further invocations
    LuaError (L, "Run-time error", szProcedure, szType, szReason, m_pDoc);
    return true;    // error return
    }

  nInvocationCount++;   // count number of times used

  if (App.m_iCounterFrequency)
    {
    QueryPerformanceCounter (&finish);
    m_pDoc->m_iScriptTimeTaken += finish.QuadPart - start.QuadPart;
    if (m_pDoc->m_CurrentPlugin)
      m_pDoc->m_CurrentPlugin->m_iScriptTimeTaken += finish.QuadPart - start.QuadPart;
    }

  if (lua_gettop (L) > 0 && lua_isstring (L, 1))
    {
    // get result

    size_t textLength;
    const char * text = luaL_checklstring (L, 1, &textLength);

    result = CString (text, textLength);
    }

  lua_settop (L, 0);  // discard any results now

  return false;   // no error

  } // end of CScriptEngine::ExecuteLua 


// bugfix thanks to Twisol (previous version didn't handle bad debug table properly)
void GetTracebackFunction (lua_State *L)
  {
  // get debug table
  lua_getglobal (L, LUA_DBLIBNAME);  

  // if it is a table then find traceback function
  if (lua_istable (L, -1))
    {
    lua_getfield (L, -1, "traceback");
    
    // remove debug table, leave traceback function
    lua_remove (L, -2);

    // if it is indeed a function, well and good
    if (lua_isfunction (L, -1))
      return; 
    }

  // not a table, or debug.traceback not a function, get rid of it
  lua_pop (L, 1);

  // return nil to caller
  lua_pushnil (L);
  return; 

  }

int CallLuaWithTraceBack (lua_State *L, const int iArguments, const int iReturn)
  {

  int error;
  int base = lua_gettop (L) - iArguments;  /* function index */
  GetTracebackFunction (L);
  if (lua_isnil (L, -1))
    {
    lua_pop (L, 1);   // pop non-existent function
    error = lua_pcall (L, iArguments, iReturn, 0);
    }  
  else
    {
    lua_insert (L, base);  /* put it under chunk and args */
    error = lua_pcall (L, iArguments, iReturn, base);
    lua_remove (L, base);  /* remove traceback function */
    }

  return error;
  }  // end of CallLuaWithTraceBack


// the more correct way of registering a Lua library -
// this does a lua_call so we get the Lua environment in the C function
void CallLuaCFunction (lua_State * L, lua_CFunction fn)
  {
  lua_pushcfunction (L, fn); 
  lua_call (L, 0, 0);
  }  // end of CallLuaCFunction
